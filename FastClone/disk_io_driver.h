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
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
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
    void closeFile(uint64_t fileId) { backend_->closeFile(fileId); }

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

    IoCounters counters() const;

    // Human-readable name of the active platform backend (e.g. "Linux io_uring"), for a one-line
    // startup diagnostic. Reflects the runtime choice, including the io_uring -> pool fallback.
    std::string backendName() const;

    const IoDriverConfig& config() const { return cfg_; }

    // Test-only (cfg.recordSchedule): the direction of each submitted op, in submission order,
    // so AC-19 (no 3 consecutive same-direction under 1:1) / AC-20 can be asserted.
    std::vector<OpKind> scheduleLog() const;

private:
    void SchedulerLoop();
    bool PickAndSubmit();  // returns true if an op was submitted this step

    IoDriverConfig cfg_;
    std::unique_ptr<PlatformIoBackend> backend_;

    mutable std::mutex qmu_;
    std::condition_variable qcv_;
    std::deque<IoRequest> readQ_;
    std::deque<IoRequest> writeQ_;
    uint32_t readCredit_ = 0;
    uint32_t writeCredit_ = 0;
    bool cancelled_ = false;
    bool stop_ = false;
    uint64_t inFlight_ = 0;

    std::mutex cmu_;
    std::condition_variable ccv_;
    std::unordered_map<uint64_t, std::deque<IoCompletion>> completionsByFile_;
    std::deque<uint64_t> completionOrder_;  // fileIds in completion order for the global drain

    mutable std::mutex countMu_;
    IoCounters counters_;
    std::vector<OpKind> scheduleLog_;

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
