#pragma once

// Shared metadata compare-orchestration component (fastcheck-compare-pipeline FR-01).
// ComparePipeline is the ONE orchestration boundary that FastClone and FastCheck both use to turn a
// stream of remote ManifestEntry file entries into per-file compare decisions:
//   batch enqueue -> fixed worker pool runs the caller-injected local probe + DecideCompare -> bounded
//   result queue drained by the caller's main thread.
//
// It deliberately owns ZERO business state (NFR-04): no transfer/report/hash fields. The three compare
// modes differ only through the injected `mode`, the injected `probe`, and how the CALLER routes each
// drained outcome. Content hashing, transfer, delta and reporting all stay in the caller.
//
// It does not touch the network / FrameChannel / socket / DiskIoDriver, so it links cleanly into all
// four targets (FastClone / FastCloneTests / FastCheck / FastCheckTests) and is unit-testable with a
// pure in-memory injected probe (no filesystem, no socket).

#include "compare_phase.h"  // CompareMode / CompareOutcome / DecideCompare (single source of truth)
#include "file_index.h"     // FileEntry

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace fc {

// Caller-injected local metadata probe: rel -> optional<FileEntry> (nullopt = missing / directory /
// unreadable). The size/mtime representation is the caller's responsibility and must stay byte-for-byte
// identical to its existing probe so DecideCompare reaches the same verdict as before (design D-04/D-05).
using LocalProbeFn = std::function<std::optional<FileEntry>(const std::string& relPath)>;

struct ComparePipelineConfig {
    CompareMode mode = CompareMode::Fast;
    std::size_t workerCount = 4;  // fixed probe pool size (caller passes max(4, hardware))
    std::size_t batchPop = 32;    // per-worker single-pop batch (mirrors the legacy kCompareBatchPop)
};

// One compare result produced by a worker. When outcome.needHash==false, outcome.category is the final
// category; when needHash==true, the caller runs its own hash phase (delta / HashRequest).
struct ComparedItem {
    FileEntry remote;
    std::optional<FileEntry> local;  // probe result; FastClone uses it for diagnostics, FastCheck for record/pairing
    CompareOutcome outcome;          // DecideCompare(mode, local, remote)
};

class ComparePipeline {
public:
    // onResultsReady: invoked (on a worker thread) after a batch of results is pushed, to wake the
    // caller's main loop (bind it to the caller's existing CV). May be empty (caller polls). The
    // callback must ONLY notify -- it must never write caller business state (NFR-03/AC-32).
    ComparePipeline(const ComparePipelineConfig& cfg,
                    LocalProbeFn probe,
                    std::function<void()> onResultsReady);
    ~ComparePipeline();  // = Stop() + Join() (idempotent)

    ComparePipeline(const ComparePipeline&) = delete;
    ComparePipeline& operator=(const ComparePipeline&) = delete;

    // Producer side (a single caller producer thread: FastClone main loop / FastCheck recv main thread).
    void Enqueue(const FileEntry& remote);  // buffers, lock-free (single producer)
    void Flush();                           // publishes the buffered batch into the task queue + notifies workers

    // Consumer side (caller main thread). Moves ready results into `out`, returns the count moved.
    std::size_t Drain(std::vector<ComparedItem>& out);

    std::size_t InFlight() const noexcept;      // issued - drained (backpressure gate)
    std::size_t QueuedTasks() const;            // pending probe tasks (diagnostics / optional tuning read)
    std::size_t PendingResults() const noexcept;  // ready-but-undrained result count (diagnostics)
    bool HasResults() const noexcept;           // result queue non-empty (wait predicate, atomic)

    void Stop() noexcept;  // set stop flag + wake all workers (idempotent)
    void Join();           // join all workers (must run before the caller destroys its DiskIoDriver)

private:
    ComparePipelineConfig cfg_;
    LocalProbeFn probe_;
    std::function<void()> onResultsReady_;

    // Task queue (workers consume). Enqueue() writes dispatchBuffer_ without a lock (single producer);
    // Flush() publishes it under taskMu_ with one notify_all, mirroring the legacy flushCompareDispatch
    // to avoid a per-entry lock convoy.
    mutable std::mutex taskMu_;
    std::condition_variable taskCv_;
    std::deque<FileEntry> tasks_;
    std::vector<FileEntry> dispatchBuffer_;

    // Result queue (caller drains).
    mutable std::mutex resultMu_;
    std::deque<ComparedItem> results_;

    // Relaxed counters; the queue mutexes protect the data bodies. issued_ ++ on Enqueue, drained_ +=
    // on Drain, pending_ tracks the result queue depth for a lock-free HasResults().
    std::atomic<std::size_t> issued_{0};
    std::atomic<std::size_t> drained_{0};
    std::atomic<std::size_t> pending_{0};

    std::atomic<bool> stop_{false};
    std::vector<std::thread> workers_;

    void WorkerLoop();
};

}  // namespace fc
