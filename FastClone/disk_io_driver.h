#pragma once

// Unified async disk IO driver (unified-disk-io-driver design section 3). A single scheduler thread pulls
// from bounded read/write queues with weighted-credit fairness + small-op priority (section 3.2), submits
// to the platform backend keeping multiple ops in flight (FR-12/13), and reaps completions into
// per-file completion queues for batch hand-off (section 3.7). Backpressure (FR-27), cancellation (FR-28),
// error isolation (FR-29) and observability counters (FR-30) live here; the backend only executes.
//
// This header pulls in only disk_io_backend.h (no <windows.h>/<liburing.h>), so callers such as
// sync_engine_{client,server}.cpp and the tests include it without platform headers.

#include "disk_io_backend.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <list>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fc::io {

// Driver-level observability counters (FR-30 / AC-35). All monotonic except the *Pending gauges,
// which are instantaneous queue depths.
struct IoCounters {
    uint64_t queued = 0;
    uint64_t submitted = 0;
    uint64_t completed = 0;
    uint64_t cancelled = 0;
    uint64_t failed = 0;
    uint64_t readSubmitted = 0;
    uint64_t writeSubmitted = 0;
    uint64_t readPending = 0;
    uint64_t writePending = 0;
    uint64_t directIo = 0;
    uint64_t bufferedFallback = 0;
    uint64_t ioUringFallback = 0;
    uint64_t smallFileFallback = 0;
    uint64_t tailZeroFallback = 0;
    uint64_t bytesRead = 0;      // cumulative bytes transferred by completed Read ops
    uint64_t bytesWritten = 0;   // cumulative bytes transferred by completed Write ops
};

class DiskIoDriver {
public:
    // Default: create the platform backend (design D-04/D-05). The injected-backend ctor is for
    // deterministic unit tests (mock backend).
    explicit DiskIoDriver(const IoDriverConfig& cfg);
    DiskIoDriver(const IoDriverConfig& cfg, std::unique_ptr<PlatformIoBackend> backend);
    ~DiskIoDriver();

    DiskIoDriver(const DiskIoDriver&) = delete;
    DiskIoDriver& operator=(const DiskIoDriver&) = delete;

    AlignInfo queryAlign(const std::string& path) { return backend_->queryAlign(path); }

    uint64_t openFile(const std::string& path, OpKind mode, bool unbuffered, uint64_t expectedSize) {
        return backend_->openFile(path, mode, unbuffered, expectedSize);
    }
    // Record the mtime to stamp on a WRITE handle at close (optimize-small-file-write-path W-01).
    void setWriteModifyTime(uint64_t fileId, int64_t modifyNs) {
        backend_->setWriteModifyTime(fileId, modifyNs);
    }
    // Finalize (exact-size truncate + optional mtime) and close; returns false on any failure so the
    // write success path can gate transfer counting on it (W-01/FR-02/B7). Read callers may ignore.
    bool closeFile(uint64_t fileId) { return backend_->closeFile(fileId); }

    // Batch submit. Returns the number accepted; fewer than batch.size() signals backpressure
    // (the target queue reached its bound, FR-27). Accepted requests are moved out of `batch`.
    size_t submit(std::vector<IoRequest>& batch);

    // Batch drain of ALL completed ops (any file), appended to out. Non-blocking.
    size_t drainCompletions(std::vector<IoCompletion>& out);
    // Batch drain of completions for a single file only (SequentialReader / per-file hand-off).
    size_t drainCompletionsForFile(uint64_t fileId, std::vector<IoCompletion>& out);
    // Block up to timeoutMs for at least one completion of `fileId` to be available.
    void waitForFile(uint64_t fileId, int timeoutMs);

    // Cancel: flush not-yet-submitted ops as Cancelled completions and stop accepting new ones;
    // already-submitted ops are still reaped (design section 3.6 / AC-23). Idempotent.
    void requestCancel();

    // server-memory-retention C1: session-level release of a file's driver-side bookkeeping —
    // undelivered completions (incl. their payload memory), global order slots and wait state.
    // Contract the caller must satisfy (else undefined behavior):
    //   (a) no further ops will be submitted for fileId after this call;
    //   (b) no reader/stream is still using fileId.
    // Idempotent; may be called repeatedly; thread-safe. If ops are still outstanding (accepted,
    // completion not yet delivered), the cleanup is only REGISTERED here and runs automatically
    // after the LAST completion is delivered — a file with ops in flight loses nothing before
    // that, so an in-flight completion is never dropped ahead of its delivery (hard constraint 1).
    void releaseFile(uint64_t fileId);
    void releaseFiles(const std::vector<uint64_t>& fileIds);

    IoCounters counters() const;

    // Driver-internal retention snapshot (observability only; no behavior change). Exposed so
    // tests and field diagnostics can assert that the driver does NOT keep state for a file
    // after it is closed, and that undelivered completions do not pile up across sessions.
    // The FastClone server shares ONE process-scoped driver across every session, so anything
    // retained here is retained for the lifetime of the process (server memory-retention).
    struct RetentionSnapshot {
        size_t trackedFiles = 0;     // completionsByFile_ entries (= files with undelivered
                                     // completions; was: every fileId ever completed)
        size_t waitStates = 0;       // fileWait_ entries (= files with a waiter blocked in
                                     // waitForFile; was: every fileId ever waited on)
        size_t completionOrder = 0;  // undelivered completions in the global order list, i.e.
                                     // the true instantaneous backlog. SEMANTIC CHANGE: this
                                     // used to count ever-appended slots that only a global
                                     // drain (which the server never calls) would pop, so old
                                     // field values are NOT comparable with new ones.
        size_t retainedBytes = 0;    // payload bytes still held by undelivered completions
        size_t pendingReleases = 0;  // files whose release is deferred until the last
                                     // outstanding completion is delivered (should be 0 at rest)
        size_t outstandingFiles = 0; // files with accepted-but-not-yet-delivered ops
    };
    RetentionSnapshot retentionSnapshot() const;

    // Human-readable name of the active platform backend (e.g. "Linux io_uring"), for a one-line
    // startup diagnostic. Reflects the runtime choice, including the io_uring -> pool fallback.
    std::string backendName() const;

    const IoDriverConfig& config() const { return cfg_; }

    // Test-only (cfg.recordSchedule): the direction of each submitted op, in submission order,
    // so AC-19 (no 3 consecutive same-direction under 1:1) / AC-20 can be asserted.
    std::vector<OpKind> scheduleLog() const;

private:
    // One arrived, not-yet-delivered completion. Embeds the intrusive global-order list links so
    // the order bookkeeping slot and the completion are 1:1 and a delivered completion removes its
    // slot in O(1) with zero extra heap allocation (server-memory-retention B2, design D-03).
    struct PendingCompletion {
        IoCompletion completion;
        PendingCompletion* orderPrev = nullptr;
        PendingCompletion* orderNext = nullptr;
    };

    // Per-file wait state. Referenced via shared_ptr so a waiter blocked in cv.wait_for holds its
    // own reference: erasing the map entry while the waiter is blocked can never dangle the cv
    // (server-memory-retention B3, design D-02). `waiters`/`dead` make the entry vanish with the
    // last waiter instead of outliving the wait.
    struct FileWaitState {
        std::condition_variable cv;
        int waiters = 0;   // threads currently blocked on this state (protected by cmu_)
        bool dead = false; // release requested: wake waiters; last one out erases the entry
    };

    void SchedulerLoop();
    bool PickAndSubmit();  // returns true if an op was submitted this step
    void EnqueueCompletion(IoCompletion completion);
    // Intrusive global-order list (replaces the unbounded deque<uint64_t> whose slots were never
    // reclaimed until a global drain that the server never performs). Slots are 1:1 with live
    // PendingCompletion nodes and vanish with delivery, so orderCount_ always equals the true
    // undelivered backlog (B2). Both run under cmu_; O(1), no allocation.
    void OrderLink(PendingCompletion* pc);
    void OrderUnlink(PendingCompletion* pc);
    // C1: drop ALL driver-side bookkeeping of fileId (undelivered completions + payloads, order
    // slots, wait state). Caller must hold cmu_; only ever reached through the release machinery
    // (releaseFile immediate branch / EnqueueCompletion delayed-release settlement), which the
    // outstanding-op accounting gates so no live reader can lose a completion it awaits.
    void ReleaseFileLocked(uint64_t fileId);

    IoDriverConfig cfg_;
    std::unique_ptr<PlatformIoBackend> backend_;

    // Lock ordering: qmu_ and cmu_ are never held together; countMu_ may be taken briefly under
    // either for counter updates. waitForFile blocks only on cmu_ (never while holding qmu_).
    mutable std::mutex qmu_;
    std::condition_variable qcv_;
    std::deque<IoRequest> readQ_;
    std::deque<IoRequest> writeQ_;
    uint32_t readCredit_ = 0;
    uint32_t writeCredit_ = 0;
    bool cancelled_ = false;
    bool stop_ = false;
    uint64_t inFlight_ = 0;

    mutable std::mutex cmu_;
    // std::list (not deque): node addresses are stable across push/pop of other nodes, which is
    // what keeps the intrusive order links valid (design D-04). Entries are erased the moment a
    // drain empties a file's queue (B1), so the map only tracks files that actually have
    // undelivered completions — bounded by live work, not by files-ever-touched.
    std::unordered_map<uint64_t, std::list<PendingCompletion>> completionsByFile_;
    // Wait states exist only while a thread is actually blocked in waitForFile (the fast path
    // returns without creating one) and are erased by the last waiter / by release, so the map
    // is empty whenever nobody is waiting (B3).
    std::unordered_map<uint64_t, std::shared_ptr<FileWaitState>> fileWait_;
    // Global completion-order list (B2): order of EnqueueCompletion calls; the head is the oldest
    // undelivered completion. Invariant I: the node set is exactly the set of PendingCompletion
    // nodes sitting in completionsByFile_ lists, in enqueue order.
    PendingCompletion* orderHead_ = nullptr;
    PendingCompletion* orderTail_ = nullptr;
    size_t orderCount_ = 0;

    mutable std::mutex countMu_;
    IoCounters counters_;
    std::vector<OpKind> scheduleLog_;
    // server-memory-retention C1 delayed-release accounting (all under countMu_):
    //   fileOutstanding_[fid] = ops accepted (via submit) whose completion has not been
    //                           delivered (i.e. not yet passed through EnqueueCompletion);
    //   pendingRelease_       = files whose releaseFile arrived while ops were outstanding —
    //                           the cleanup runs in EnqueueCompletion's settlement once the
    //                           last outstanding completion is delivered (invariant II:
    //                           an entry in pendingRelease_ implies outstanding ops exist).
    // Exactly one increment (submit) and one decrement (EnqueueCompletion settlement) per op;
    // requestCancel's flush and PickAndSubmit's hard-failure synthesis both deliver their
    // completions through EnqueueCompletion, so they need no separate decrement.
    std::unordered_map<uint64_t, uint32_t> fileOutstanding_;
    std::unordered_set<uint64_t> pendingRelease_;

    std::thread scheduler_;
};

// Sequential read helper (design section 3.1). Issues a bounded read-ahead window of ops over one file and
// yields completed chunks in strict offset order, hiding reordering behind a simple pull interface.
// Suitable as the ByteSource for BuildPlanStreaming and for the server hash/sig streaming reads.
class SequentialReader {
public:
    SequentialReader(DiskIoDriver& driver, uint64_t fileId, uint64_t fileSize, uint32_t chunkBytes,
                     uint32_t readAhead);

    // Pull the next in-order chunk. Returns bytes read (0 at clean EOF). Sets `ok=false` on error.
    // Bytes are appended to `out` (cleared first).
    uint32_t next(std::vector<uint8_t>& out, bool& ok);

    // Adapter usable directly as fc::delta::ByteSource (fills dst up to maxLen, returns count).
    size_t pull(uint8_t* dst, size_t maxLen);

private:
    void refill();

    DiskIoDriver& drv_;
    uint64_t fileId_;
    uint64_t fileSize_;
    uint32_t chunk_;
    uint32_t readAhead_;
    uint64_t nextSubmitOffset_ = 0;   // next offset to submit
    uint64_t nextYieldOffset_ = 0;    // next offset to hand back (in-order)
    uint32_t inFlight_ = 0;
    bool error_ = false;
    std::unordered_map<uint64_t, IoCompletion> reordered_;  // offset -> completion
    std::vector<uint8_t> carry_;      // leftover bytes for the pull() adapter
    size_t carryPos_ = 0;
};

}  // namespace fc::io
