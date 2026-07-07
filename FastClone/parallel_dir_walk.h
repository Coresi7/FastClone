#pragma once

// Shared parallel directory-walk component (task fastcheck-extra-parallel-walk, D-01 candidate B).
// The ParallelDirWalk template + its fc::detail helper set (JoinDiag / ResolveDirWalkWorkerCount /
// BuildRelPath / OpenDirFind) were migrated verbatim out of sync_engine_internal.h /
// sync_engine_shared.cpp into this lightweight TU so every target that needs parallel metadata
// traversal (FastClone, FastCloneTests, FastCheck, FastCheckTests) can link one small, self-contained
// unit instead of dragging the whole sync-engine header graph into its compile closure. The behaviour
// is byte-for-byte the old one; only the translation unit changed. sync_engine_internal.h now
// re-exports this header, so sync_engine_client.cpp (RemoveLocalExtras) needs zero source changes.

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <exception>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <Windows.h>
#endif

namespace fc {
namespace detail {

// Diagnostic wrapper around std::thread::join(); fails fast on a self-join (thread-ownership
// violation). Definition in parallel_dir_walk.cpp.
void JoinDiag(std::thread& t, const char* site);

// Worker count for the parallel directory walks (latency-bound metadata reads: oversubscribe cores,
// capped at 16). Definition in parallel_dir_walk.cpp.
unsigned ResolveDirWalkWorkerCount();

// rel = relDir + "/" + name (or just name at the root). Single definition for every walk.
// Definition in parallel_dir_walk.cpp.
std::string BuildRelPath(const std::string& relDir, const std::string& name);

#ifdef _WIN32
// Open a FindFirstFileExW enumeration handle for a directory's children (basic-info + large-fetch).
// Definition in parallel_dir_walk.cpp.
HANDLE OpenDirFind(const std::wstring& absDir, WIN32_FIND_DATAW& fd);
#endif

// Parallel directory traversal. A shared work queue of directories is serviced by a
// pool of worker threads; each pops a directory, lists it (emitting entries through
// processDir) and pushes any sub-directories back. The point is to keep MANY metadata
// reads (FindFirstFile / opendir) in flight at once: on huge trees the per-directory
// listing is dominated by random MFT / inode reads whose LATENCY a single thread cannot
// hide (the disk sits half-idle at queue-depth 1). Fan-out raises the device queue
// depth so those waits overlap, which is the actual lever past the metadata-cache wall.
//
// Termination is detected when the queue is empty AND no worker is mid-directory
// (activeDirs == 0): activeDirs is incremented under the lock at pop time and only
// decremented after the directory's children have been pushed, so it can never read
// zero while work is still being produced.
// Per-directory work is batched so the hot path almost never touches the shared queue
// lock: dirPopBatch directories are popped (and their children pushed back) per qmu
// acquisition, amortising the work-queue lock over many directories. Anything the caller
// does per file (emit frames, delete extras) is done lock-free against its own per-worker
// WorkerCtx; the caller decides when/how to hand that off (see processDir / finishWorker).
//
// Each worker keeps a default-constructed WorkerCtx for the whole walk and threads it
// through processDir; finishWorker runs once when the worker drains, to flush/merge that
// context. Use it to batch any cross-thread hand-off out of the per-file path.
constexpr size_t kDirPopBatch = 8;          // manifest enumeration
constexpr size_t kDeleteDirPopBatch = 64;   // deletion walk: bigger, pure metadata reads
constexpr size_t kFrameFlushThreshold = 1024;

template <typename PendingDir, typename WorkerCtx, typename ProcessFn, typename FinishFn>
void ParallelDirWalk(PendingDir rootDir,
                            unsigned numWorkers,
                            size_t dirPopBatch,
                            const std::atomic<bool>& done,
                            const char* joinSite,
                            WorkerCtx ctxPrototype,
                            ProcessFn&& processDir,
                            FinishFn&& finishWorker) {
    std::mutex qmu;
    std::condition_variable qcv;
    std::deque<PendingDir> queue;
    size_t activeDirs = 0;
    queue.push_back(std::move(rootDir));

    std::mutex errMu;
    std::exception_ptr firstError;

    auto worker = [&]() {
        WorkerCtx ctx = ctxPrototype;
        std::vector<PendingDir> batch;
        std::vector<PendingDir> subdirs;
        while (true) {
            batch.clear();
            {
                std::unique_lock<std::mutex> lock(qmu);
                qcv.wait(lock, [&]() {
                    return done.load() || !queue.empty() || activeDirs == 0;
                });
                if (done.load()) {
                    break;
                }
                if (queue.empty()) {
                    // queue empty && activeDirs == 0 -> walk complete.
                    qcv.notify_all();
                    break;
                }
                const size_t take = std::min<size_t>(dirPopBatch, queue.size());
                for (size_t i = 0; i < take; ++i) {
                    batch.push_back(std::move(queue.back()));
                    queue.pop_back();
                }
                activeDirs += take;
            }
            subdirs.clear();
            // A worker exception (e.g. bad_alloc) must neither call std::terminate NOR leak
            // activeDirs: skipping the decrement below would wedge the termination predicate
            // and hang every other worker on join. So we capture the FIRST exception, always
            // run the queue/activeDirs bookkeeping (just without pushing the failed batch's
            // children), and rethrow once after all workers have joined. Siblings finish the
            // rest of the tree normally, so no one is forced into a stop needing a wake.
            bool threw = false;
            try {
                for (const PendingDir& d : batch) {
                    processDir(d, subdirs, ctx);
                }
            } catch (...) {
                threw = true;
                std::lock_guard<std::mutex> elock(errMu);
                if (!firstError) {
                    firstError = std::current_exception();
                }
            }
            {
                std::unique_lock<std::mutex> lock(qmu);
                if (!threw) {
                    for (PendingDir& d : subdirs) {
                        queue.push_back(std::move(d));
                    }
                }
                activeDirs -= batch.size();
                if (!queue.empty() || activeDirs == 0) {
                    qcv.notify_all();
                }
            }
        }
        // Flush/merge whatever this worker still holds before it exits (outside qmu).
        finishWorker(ctx);
    };

    std::vector<std::thread> workers;
    workers.reserve(numWorkers);
    for (unsigned i = 0; i < numWorkers; ++i) {
        workers.emplace_back(worker);
    }
    for (std::thread& t : workers) {
        JoinDiag(t, joinSite);
    }
    if (firstError) {
        std::rethrow_exception(firstError);
    }
}

}  // namespace detail
}  // namespace fc
