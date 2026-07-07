#include "check_engine.h"

#include "compare_phase.h"
#include "compare_pipeline.h"
#include "disk_io_driver.h"
#include "file_index.h"
#include "protocol_codec.h"

#ifdef _WIN32
#include <Windows.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;

namespace fc::check {

namespace {

CompareMode ToCompareMode(Mode mode) {
    switch (mode) {
        case Mode::Strict:
            return CompareMode::Strict;
        case Mode::SizeOnly:
            return CompareMode::SizeOnly;
        case Mode::Fast:
        default:
            return CompareMode::Fast;
    }
}

#ifdef _WIN32
std::wstring Utf8ToWideLocal(const std::string& value) {
    if (value.empty()) {
        return {};
    }
    const int len = MultiByteToWideChar(CP_UTF8, 0, value.c_str(),
                                        static_cast<int>(value.size()), nullptr, 0);
    if (len <= 0) {
        return {};
    }
    std::wstring output(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()),
                        output.data(), len);
    return output;
}
#endif

// Join a relative path (UTF-8, forward slashes) onto the local root directory. Self-contained, does not depend on
// sync_util::JoinRel (which is not in the FastCheck link closure).
fs::path JoinLocal(const fs::path& root, const std::string& rel) {
#ifdef _WIN32
    std::wstring full = root.wstring();
    if (!full.empty() && full.back() != L'\\' && full.back() != L'/') {
        full.push_back(L'\\');
    }
    std::wstring relW = Utf8ToWideLocal(rel);
    for (wchar_t& c : relW) {
        if (c == L'/') {
            c = L'\\';
        }
    }
    full += relW;
    return fs::path(full);
#else
    return root / fs::path(rel);
#endif
}

// Replicates sync_engine_client's probeLocalFile: one syscall to get size+mtime, yielding optional<FileEntry>.
// The mtime unit matches the manifest side (Win=FILETIME ticks, POSIX=Unix ns), for DecideCompare's normalized comparison.
std::optional<FileEntry> ProbeLocal(const fs::path& root, const std::string& rel) {
    const fs::path abs = JoinLocal(root, rel);
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (GetFileAttributesExW(abs.wstring().c_str(), GetFileExInfoStandard, &data) == 0) {
        return std::nullopt;
    }
    if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return std::nullopt;
    }
    FileEntry entry;
    entry.relativePath = rel;
    entry.isDirectory = false;
    entry.fileSize = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) |
                     static_cast<uint64_t>(data.nFileSizeLow);
    ULARGE_INTEGER mt{};
    mt.LowPart = data.ftLastWriteTime.dwLowDateTime;
    mt.HighPart = data.ftLastWriteTime.dwHighDateTime;
    entry.mtimeNs = static_cast<int64_t>(mt.QuadPart);
    return entry;
#else
    std::error_code ec;
    if (!fs::exists(abs, ec) || ec) {
        return std::nullopt;
    }
    if (!fs::is_regular_file(abs, ec) || ec) {
        return std::nullopt;
    }
    FileEntry entry;
    entry.relativePath = rel;
    entry.isDirectory = false;
    entry.fileSize = static_cast<uint64_t>(fs::file_size(abs, ec));
    if (ec) {
        return std::nullopt;
    }
    entry.mtimeNs = ToUnixNs(fs::last_write_time(abs, ec));
    if (ec) {
        return std::nullopt;
    }
    return entry;
#endif
}

// Strict-mode local probe injected into the ComparePipeline (fastcheck-compare-pipeline D-03). Strict
// ignores mtime, so a single std::filesystem::directory_entry cached metadata query yields both the
// type and the size (preserving the redundant-syscall-elim single-query semantics, dev-map RS-01):
// not-found / directory / special / unreadable -> nullopt (Missing); a regular file -> size only.
// DecideCompare(Strict, ...) then decides Missing / Diff (size differs) / needHash (size equal). mtime
// is deliberately left 0 because Strict never consults it.
std::optional<FileEntry> StrictProbe(const fs::path& root, const std::string& rel) {
    const fs::path abs = JoinLocal(root, rel);
    std::error_code ec;
    const fs::directory_entry entry(abs, ec);
    if (ec) {
        return std::nullopt;
    }
    std::error_code tec;
    if (!entry.is_regular_file(tec) || tec) {
        return std::nullopt;  // not found / directory / special -> Missing
    }
    std::error_code sec;
    const uint64_t localSize = static_cast<uint64_t>(entry.file_size(sec));
    if (sec) {
        return std::nullopt;  // size unreadable -> Missing
    }
    FileEntry fe;
    fe.relativePath = rel;
    fe.isDirectory = false;
    fe.fileSize = localSize;
    fe.mtimeNs = 0;  // Strict ignores mtime
    return fe;
}

}  // namespace

std::size_t NextLocalWorkerCap(std::size_t currentCap, std::size_t maxCap, std::size_t taskQueueLen,
                               std::size_t localInFlight, std::uint64_t readFailDelta) {
    if (readFailDelta > 0) {
        // Multiplicative decrease on read/driver failures (halve, floor 1).
        return std::max<std::size_t>(1, currentCap / 2);
    }
    // Additive increase: backlog present, all active workers busy, headroom below max.
    if (taskQueueLen > 0 && localInFlight >= currentCap && currentCap < maxCap) {
        return currentCap + 1;
    }
    return currentCap;
}

std::size_t NextNetWindow(std::size_t currentWindow, double rttSampleUs, double rttEwmaUs,
                          std::size_t windowMin, std::size_t windowMax) {
    constexpr double kStableFactor = 1.25;
    constexpr double kSpikeFactor = 2.0;
    constexpr double kDecreaseFactor = 0.6;
    if (rttEwmaUs <= 0.0) {
        return currentWindow;  // not enough samples to judge
    }
    if (rttSampleUs <= rttEwmaUs * kStableFactor) {
        return std::min<std::size_t>(currentWindow + 1, windowMax);
    }
    if (rttSampleUs > rttEwmaUs * kSpikeFactor) {
        const std::size_t decreased = static_cast<std::size_t>(currentWindow * kDecreaseFactor);
        return std::max<std::size_t>(windowMin, decreased);
    }
    return currentWindow;
}

std::size_t ResolveMaxHashWorkers(std::size_t hashWorkers, std::size_t hardwareThreads) {
    // auto (--hash-workers 0): pin the pool to the core count. The AIMD growth to 4x cores that
    // helped IO-bound big-file workloads overshoots on small-file workloads -- 80 threads on 20
    // cores thrash the NTFS/cache-manager kernel locks (CreateFileW serializes on the MFT lock) and
    // starve the single DiskIoDriver scheduler thread, so per-file open+read latency rises faster
    // than concurrency, netting WORSE throughput. Explicit --hash-workers keeps the 4x headroom
    // (the user opted into tuning for a known IO-bound workload).
    if (hashWorkers == 0) {
        return hardwareThreads;
    }
    return std::max<std::size_t>(hashWorkers, static_cast<std::size_t>(4) * hardwareThreads);
}

EngineOutcome RunCheck(const CheckOptions& o, FrameChannel& ch,
                       const std::atomic<bool>& interrupted) {
    const auto startTime = std::chrono::steady_clock::now();
    const CompareMode mode = ToCompareMode(o.mode);
    const fs::path targetRoot(std::filesystem::path(o.target));

    CheckResult result;
    result.mode = o.mode;
    fc::CompareCounters& counters = result.counters;

    // Diagnostics-only accumulators (no behavior change): track how many files need hashing and
    // how many bytes that represents, so the progress line can report avg file size + byte progress
    // and we can tell whether the workload is small-file/IOPS-bound or bandwidth-bound.
    uint64_t hashEnqueued = 0;
    uint64_t hashBytesTotal = 0;
    uint64_t hashBytesDone = 0;

    // Worker per-phase wall-clock accumulators (microseconds, relaxed atomics). ProbeLocal = the
    // stat syscall the strict-defer worker does; hashOne = the ComputeFileHash(ViaDriver) call.
    // Together with GetHashPhaseTimings() (open/read/xxh/close inside the driver hash path) they
    // locate where the per-file time goes. Read in the progress line via per-interval deltas.
    std::atomic<uint64_t> probeUsSum{0};
    std::atomic<uint64_t> probeCount{0};
    std::atomic<uint64_t> hashOneUsSum{0};
    std::atomic<uint64_t> hashOneCount{0};

    std::unordered_set<std::string> manifestPaths;

    // Files that need a hash (needHash decided in the metadata phase). Those already sent and awaiting a response go into awaiting (matched by relPath).
    struct HashNeed {
        FileEntry remote;
        std::optional<FileEntry> local;
    };
    std::deque<HashNeed> hashQueue;
    std::unordered_map<std::string, HashNeed> awaiting;
    size_t inFlight = 0;

    bool manifestDone = false;
    bool userInterrupted = false;
    bool disconnected = false;
    bool localReadFailed = false;
    std::string errorText;

    // --- Async local-hash pipeline (fastcheck-parallel-hash M5/M6/FR-05..FR-09). Mirrors the
    // FastClone sync client worker pool (sync_engine_client.cpp:998-1041): a fixed thread pool reads
    // and hashes local files off the recv loop; the main loop keeps receiving frames and pairs each
    // HashResponse with its local hash whichever arrives first. ---

    // Client-side unified disk IO driver (FR-04/FR-16). std::optional so --no-diskio-driver keeps the
    // parallel pipeline but never constructs a real driver. Destroyed at RunCheck scope exit, after
    // all workers are joined (AC-16).
    std::optional<fc::io::DiskIoDriver> clientDriver;
    std::string backendLabel;
    if (!o.noDiskioDriver) {
        clientDriver.emplace(fc::io::IoDriverConfig{});
        backendLabel = clientDriver->backendName();
        const fc::io::IoDriverConfig& cfg = clientDriver->config();
        std::cerr << "[disk-io] backend=" << backendLabel
                  << " maxInFlight=" << cfg.maxInFlight
                  << " backendConcurrency=" << cfg.backendConcurrency
                  << " chunkBytes=" << cfg.chunkBytes << std::endl;
    } else {
        backendLabel = "disabled(--no-diskio-driver)";
        std::cerr << "[disk-io] backend=" << backendLabel << std::endl;
    }

    struct HashTask {
        std::string rel;
        fs::path abs;
    };
    std::mutex hashTaskMu;
    std::condition_variable hashTaskCv;
    std::deque<HashTask> hashTaskQueue;
    std::atomic<bool> hashStop{false};
    std::atomic<size_t> localHashInFlight{0};
    std::atomic<uint64_t> readFailSignal{0};

    // Worker conclusion for a file. With the shared ComparePipeline (fastcheck-compare-pipeline §4.3),
    // the local existence/size probe already ran in the compare workers, so only size-equal files ever
    // reach a hash worker: the worker's only job is to hash, producing Hashed or Failed. The Missing /
    // SizeDiff kinds and the local backfill (candidate B) are gone -- those verdicts are recorded
    // directly on the drain path (candidate A, decisions.md D-01). The final local FileEntry for
    // recording comes from awaiting[rel].local (captured at enqueue time from the compare probe).
    struct LocalResult {
        enum class Kind { Hashed, Failed };
        Kind kind = Kind::Failed;
        Hash256 hash{};  // valid only for Hashed
    };
    std::mutex hashResultMu;
    std::unordered_map<std::string, LocalResult> localResults;

    std::mutex readyMu;
    std::condition_variable readyCv;
    std::deque<std::string> readyQueue;

    // Server hash responses that arrived before the local hash was ready (pending, FR-07).
    std::unordered_map<std::string, Hash256> serverHashReady;

    // Two independent dynamic-concurrency dimensions (FR-12/FR-13/FR-14).
    const uint32_t hardwareThreads = std::max<uint32_t>(1, std::thread::hardware_concurrency());
    const size_t initialWorkers = (o.hashWorkers == 0) ? hardwareThreads : o.hashWorkers;
    // Pool cap policy lives in ResolveMaxHashWorkers (pure, unit-tested): auto pins to hardwareThreads,
    // explicit --hash-workers keeps the 4x-core headroom. Value is identical to the former inline form.
    const size_t maxWorkers = ResolveMaxHashWorkers(o.hashWorkers, hardwareThreads);
    std::atomic<size_t> hashWorkerCap{initialWorkers};   // local hash dimension (--hash-workers)
    size_t hashWindow = o.checkers;                      // network window dimension (--checkers)
    constexpr size_t kNetWindowMin = 1;
    constexpr size_t kNetWindowMax = 256;
    constexpr size_t kLocalBacklogFactor = 4;

    // Fixed pool of maxWorkers threads; hashWorkerCap gates how many are active (park/unpark, no
    // spawn/join churn, D-04). Worker myIndex >= cap parks until the cap grows or stop is signalled.
    std::vector<std::thread> hashWorkers;
    hashWorkers.reserve(maxWorkers);
    for (size_t idx = 0; idx < maxWorkers; ++idx) {
        hashWorkers.emplace_back([&, myIndex = idx]() {
            while (true) {
                HashTask task;
                bool gotTask = false;
                {
                    std::unique_lock<std::mutex> lk(hashTaskMu);
                    // Wake on stop OR any task available. The cap gate is applied AFTER waking, not in the
                    // predicate: a notify_one may land on an over-cap worker (the pool has maxWorkers
                    // threads but only hashWorkerCap are active). If that worker simply re-parked, the
                    // notify would be consumed without processing and the sole active worker could starve
                    // forever (hang). So an over-cap worker that finds queued work passes the baton
                    // (notify_one) to give an active worker a chance to pick it up before re-parking.
                    hashTaskCv.wait(lk, [&]() {
                        return hashStop.load() || !hashTaskQueue.empty();
                    });
                    if (hashStop.load() && hashTaskQueue.empty()) {
                        return;
                    }
                    if (!hashTaskQueue.empty() && myIndex < hashWorkerCap.load()) {
                        task = std::move(hashTaskQueue.front());
                        hashTaskQueue.pop_front();
                        gotTask = true;
                    } else if (!hashTaskQueue.empty()) {
                        // Work is available but this worker is over the cap: pass the baton so an active
                        // worker is woken instead of letting the notify be lost. Re-loop to re-evaluate.
                        hashTaskCv.notify_one();
                    }
                }
                if (!gotTask) {
                    if (hashStop.load()) {
                        return;
                    }
                    continue;  // spurious / baton-passed: re-wait
                }
                localHashInFlight.fetch_add(1, std::memory_order_relaxed);
                // Worker job: hash the (already size-equal) file. The local existence/size verdict was
                // decided upstream in the compare pipeline, so every task that reaches here is a
                // size-equal file that needs a content hash (Fast mtime-miss or Strict size-equal). The
                // try/catch keeps the existing read-fail semantics: readFailSignal drives the local-worker
                // AIMD MD; the main-thread finalize maps Failed -> localReadFailed.
                LocalResult r;
                {
                    Hash256 hash{};
                    bool hashFailed = false;
                    const auto h0 = std::chrono::steady_clock::now();
                    try {
                        hash = o.noDiskioDriver ? ComputeFileHash(task.abs)
                                                : ComputeFileHashViaDriver(*clientDriver, task.abs);
                    } catch (...) {
                        hashFailed = true;
                    }
                    const auto h1 = std::chrono::steady_clock::now();
                    hashOneUsSum.fetch_add(static_cast<uint64_t>(
                        std::chrono::duration_cast<std::chrono::microseconds>(h1 - h0).count()),
                        std::memory_order_relaxed);
                    hashOneCount.fetch_add(1, std::memory_order_relaxed);
                    if (hashFailed) {
                        r.kind = LocalResult::Kind::Failed;
                    } else {
                        r.kind = LocalResult::Kind::Hashed;
                        r.hash = hash;
                    }
                }
                const bool failed = (r.kind == LocalResult::Kind::Failed);
                {
                    std::lock_guard<std::mutex> lk(hashResultMu);
                    localResults[task.rel] = std::move(r);
                }
                if (failed) {
                    readFailSignal.fetch_add(1, std::memory_order_relaxed);
                }
                localHashInFlight.fetch_sub(1, std::memory_order_relaxed);
                {
                    std::lock_guard<std::mutex> lk(readyMu);
                    readyQueue.push_back(task.rel);
                }
                readyCv.notify_one();
            }
        });
    }

    // RTT sampling for the network window AIMD (D-05, mirrors sync_engine_client.cpp:1926-1935).
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> sentAt;
    double rttEwmaUs = 0.0;
    constexpr double kRttEwmaAlpha = 0.2;

    // Teardown: stop workers, join all, then a final drain of anything that became ready. Runs on
    // every exit path (clean, disconnect, interrupt, local-read-failure) so no worker is ever left
    // hanging and the driver is destroyed only after join (FR-09/AC-16).
    auto joinWorkers = [&]() {
        hashStop.store(true);
        hashTaskCv.notify_all();
        for (auto& w : hashWorkers) {
            if (w.joinable()) {
                w.join();
            }
        }
    };

    // Record a file with a decided category into the counters and (per filter/summaryOnly) the per-file listing.
    auto record = [&](fc::CompareCategory category, const FileEntry& remote,
                      const std::optional<FileEntry>& local, bool hashCompared) {
        switch (category) {
            case fc::CompareCategory::Same:
                ++counters.same;
                break;
            case fc::CompareCategory::Diff:
                ++counters.diff;
                break;
            case fc::CompareCategory::Missing:
                ++counters.missing;
                break;
            case fc::CompareCategory::Extra:
                ++counters.extraLocal;
                break;
        }
        // Diagnostics only: count bytes of files whose comparison actually went through a hash.
        if (hashCompared) {
            hashBytesDone += remote.fileSize;
        }
        if (o.summaryOnly) {
            return;
        }
        const bool keep = (category == fc::CompareCategory::Diff && o.filter.diff) ||
                          (category == fc::CompareCategory::Missing && o.filter.missing) ||
                          (category == fc::CompareCategory::Extra && o.filter.extra) ||
                          (category == fc::CompareCategory::Same && o.filter.same);
        if (!keep) {
            return;
        }
        DiffEntry entry;
        entry.type = category;
        entry.path = remote.relativePath;
        entry.hashCompared = hashCompared;
        // localSize: present only when the local file exists (Same/Diff/Extra); Missing means local absent -> local empty -> null (FR-24).
        if (local.has_value()) {
            entry.localSize = local->fileSize;
        }
        // remoteSize: present only when the remote exists (Same/Diff/Missing all come from manifest entries, remote.fileSize valid);
        // Extra means remote absent -> leave nullopt -> JSON null (FR-24/AC-31). The remote passed for EXTRA is a temporary
        // construct with fileSize=0 and must never be filled into remoteSize.
        if (category != fc::CompareCategory::Extra) {
            entry.remoteSize = remote.fileSize;
        }
        result.entries.push_back(std::move(entry));
    };

    // Finalize a paired file on the main thread: the worker's LocalResult (Hashed/Failed) and the
    // remote HashResponse are both available. Only size-equal files ever reach hash (Missing/SizeDiff
    // were recorded directly on the compare drain path, candidate A / decisions.md D-01), so there is
    // no diagnostic rollback here. The local FileEntry comes from awaiting[rel].local (captured at
    // enqueue time from the compare probe). Records once, then erases awaiting + any pending server
    // hash. Failed follows the existing local-read-failure semantics: set localReadFailed and leave
    // the entry for teardown (no erase).
    auto finalizePaired = [&](std::unordered_map<std::string, HashNeed>::iterator a,
                              const LocalResult& lr, const Hash256& serverHash) {
        const std::string rel = a->first;
        switch (lr.kind) {
            case LocalResult::Kind::Hashed:
                record(ClassifyByHash(true, lr.hash, serverHash), a->second.remote, a->second.local, true);
                break;
            case LocalResult::Kind::Failed:
                localReadFailed = true;
                errorText = "local hash read failed: " + rel;
                return;  // no erase; teardown handles it (existing semantics)
        }
        awaiting.erase(a);
        serverHashReady.erase(rel);
    };

    // Eager enqueue (FR-05): sending the HashRequest and queuing the local hash task happen in the
    // same step, so the network RTT + server hash overlaps the local SSD read. Also stamps sentAt
    // for the network-window RTT sample (D-05). The compare pipeline already probed the local file, so
    // the worker only hashes (no deferred stat).
    auto sendHashRequest = [&](const HashNeed& need) {
        const std::string& rel = need.remote.relativePath;
        ch.send(Frame{MsgType::HashRequest, 0, EncodeHashRequest(rel)});
        awaiting.emplace(rel, need);
        {
            std::lock_guard<std::mutex> lk(hashTaskMu);
            hashTaskQueue.push_back(HashTask{rel, JoinLocal(targetRoot, rel)});
        }
        hashTaskCv.notify_one();
        sentAt[rel] = std::chrono::steady_clock::now();
        ++inFlight;
    };
    // pump double-gate (D-06): send while the network window has room AND the local backlog is under
    // its cap. The network window (hashWindow) and the local backpressure cap (derived from
    // hashWorkerCap) are two independent read-only gates; neither AIMD controller writes the other's
    // target value (FR-14).
    auto pump = [&]() {
        while (!hashQueue.empty() && inFlight < hashWindow) {
            size_t queued = 0;
            {
                std::lock_guard<std::mutex> lk(hashTaskMu);
                queued = hashTaskQueue.size();
            }
            const size_t backlogCap = kLocalBacklogFactor * hashWorkerCap.load();
            if (queued + localHashInFlight.load() >= backlogCap) {
                break;  // local backpressure: hold off sending more until workers catch up
            }
            const HashNeed need = hashQueue.front();
            hashQueue.pop_front();
            sendHashRequest(need);
        }
    };

    // Drain worker-completed rels; classify any whose server HashResponse is already pending (FR-08).
    // A rel whose server response has not arrived yet is simply dropped here and finalized later by
    // the HashResponse handler (which finds the local hash ready). One-and-only-once via awaiting.erase.
    auto drainReady = [&]() {
        std::deque<std::string> done;
        {
            std::lock_guard<std::mutex> lk(readyMu);
            done.swap(readyQueue);
        }
        for (const std::string& rel : done) {
            auto a = awaiting.find(rel);
            if (a == awaiting.end()) {
                continue;
            }
            auto s = serverHashReady.find(rel);
            if (s == serverHashReady.end()) {
                continue;  // server response not here yet; HashResponse handler will finalize
            }
            LocalResult lr;
            {
                std::lock_guard<std::mutex> lk(hashResultMu);
                auto it = localResults.find(rel);
                if (it == localResults.end()) {
                    continue;  // not actually ready (defensive); leave for a later pass
                }
                lr = it->second;
            }
            finalizePaired(a, lr, s->second);
            if (localReadFailed) {
                return;
            }
        }
    };

    // Progress line to stderr every 5s or every 50000 enumerated entries (FR-15/AC-12/NFR-07).
    auto lastProgressAt = std::chrono::steady_clock::now();
    uint64_t lastProgressEnum = 0;
    // Last-snapshot for per-interval per-phase timing deltas (steady-state localization).
    fc::HashPhaseTimings lastHashPhases{};
    uint64_t lastProbeUs = 0, lastProbeCount = 0;
    uint64_t lastHashOneUs = 0, lastHashOneCount = 0;
    auto maybeEmitProgress = [&]() {
        const auto now = std::chrono::steady_clock::now();
        const bool timeDue = (now - lastProgressAt) >= std::chrono::seconds(5);
        const bool countDue = (counters.enumerated - lastProgressEnum) >= 50000;
        if (!timeDue && !countDue) {
            return;
        }
        lastProgressAt = now;
        lastProgressEnum = counters.enumerated;
        const uint64_t compared = counters.same + counters.diff + counters.missing;
        // Snapshot the driver counters (n/a when --no-diskio-driver). readPending is the
        // instantaneous read-queue depth: if it sits at maxInFlight (default 64) the disk pipeline
        // is IO-slot-bound; smallFileFallback vs directIo tells us whether tiny files dominate.
        uint64_t ioSub = 0, ioComp = 0, ioReadPending = 0, ioDirect = 0;
        uint64_t ioBufFallback = 0, ioSmallFallback = 0, ioTailFallback = 0, ioFailed = 0;
        const char* ioTag = "n/a";
        if (clientDriver.has_value()) {
            const fc::io::IoCounters c = clientDriver->counters();
            ioSub = c.submitted;
            ioComp = c.completed;
            ioReadPending = c.readPending;
            ioDirect = c.directIo;
            ioBufFallback = c.bufferedFallback;
            ioSmallFallback = c.smallFileFallback;
            ioTailFallback = c.tailZeroFallback;
            ioFailed = c.failed;
            ioTag = "io";
        }
        std::cerr << "[check] enum=" << counters.enumerated << " compared=" << compared
                  << " same=" << counters.same << " diff=" << counters.diff
                  << " missing=" << counters.missing
                  << " hash_local_inflight=" << localHashInFlight.load()
                  << " hash_inflight=" << inFlight << " backend=" << backendLabel
                  << " hash_workers=" << hashWorkerCap.load() << " net_window=" << hashWindow
                  << " pending_resp=" << serverHashReady.size()
                  << " hash_enqueued=" << hashEnqueued
                  << " hash_bytes_total=" << hashBytesTotal
                  << " hash_bytes_done=" << hashBytesDone
                  << " " << ioTag
                  << "_submitted=" << ioSub
                  << " " << ioTag << "_completed=" << ioComp
                  << " " << ioTag << "_read_pending=" << ioReadPending
                  << " " << ioTag << "_direct=" << ioDirect
                  << " " << ioTag << "_buffered_fallback=" << ioBufFallback
                  << " " << ioTag << "_small_fallback=" << ioSmallFallback
                  << " " << ioTag << "_tail_fallback=" << ioTailFallback
                  << " " << ioTag << "_failed=" << ioFailed
                  << std::endl;

        // Per-phase timing (per-interval delta averages, microseconds/file). Locates where the
        // per-file hash time goes: probe (stat) vs hashOne (open/read/xxh/close), with hashOne
        // broken down by GetHashPhaseTimings(). Empty when --no-diskio-driver or no samples yet.
        const fc::HashPhaseTimings hp = fc::GetHashPhaseTimings();
        const uint64_t hpDcount = hp.count - lastHashPhases.count;
        const uint64_t probeDus = probeUsSum.load() - lastProbeUs;
        const uint64_t probeDcount = probeCount.load() - lastProbeCount;
        const uint64_t hashOneDus = hashOneUsSum.load() - lastHashOneUs;
        const uint64_t hashOneDcount = hashOneCount.load() - lastHashOneCount;
        auto avg = [](uint64_t us, uint64_t n) -> uint64_t {
            return (n == 0) ? 0 : (us / n);
        };
        std::cerr << "[check-phases] probe_us_avg=" << avg(probeDus, probeDcount)
                  << " probe_count=" << probeDcount
                  << " hashone_us_avg=" << avg(hashOneDus, hashOneDcount)
                  << " hashone_count=" << hashOneDcount
                  << " hashph_count=" << hpDcount
                  << " fs_us_avg=" << avg(hp.fileSizeUs - lastHashPhases.fileSizeUs, hpDcount)
                  << " open_us_avg=" << avg(hp.openUs - lastHashPhases.openUs, hpDcount)
                  << " read_us_avg=" << avg(hp.readUs - lastHashPhases.readUs, hpDcount)
                  << " xxh_us_avg=" << avg(hp.xxhUs - lastHashPhases.xxhUs, hpDcount)
                  << " close_us_avg=" << avg(hp.closeUs - lastHashPhases.closeUs, hpDcount)
                  << " total_us_avg=" << avg(hp.totalUs - lastHashPhases.totalUs, hpDcount)
                  << std::endl;
        lastHashPhases = hp;
        lastProbeUs = probeUsSum.load();
        lastProbeCount = probeCount.load();
        lastHashOneUs = hashOneUsSum.load();
        lastHashOneCount = hashOneCount.load();
    };

    // Local worker AIMD sampling (D-04): periodically adjust hashWorkerCap from backlog / in-flight /
    // read-fail signals. Writes only hashWorkerCap (never hashWindow), keeping the two dimensions
    // decoupled (FR-14).
    auto lastLocalTuneAt = std::chrono::steady_clock::now();
    uint64_t lastReadFailSnapshot = 0;
    auto maybeTuneLocal = [&]() {
        const auto now = std::chrono::steady_clock::now();
        if ((now - lastLocalTuneAt) < std::chrono::milliseconds(200)) {
            return;
        }
        lastLocalTuneAt = now;
        const uint64_t cur = readFailSignal.load();
        const uint64_t delta = cur - lastReadFailSnapshot;
        lastReadFailSnapshot = cur;
        size_t queued = 0;
        {
            std::lock_guard<std::mutex> lk(hashTaskMu);
            queued = hashTaskQueue.size();
        }
        const size_t oldCap = hashWorkerCap.load();
        const size_t newCap =
            NextLocalWorkerCap(oldCap, maxWorkers, queued, localHashInFlight.load(), delta);
        if (newCap != oldCap) {
            hashWorkerCap.store(newCap);
            hashTaskCv.notify_all();  // wake newly-activated workers
        }
    };

    // Network window AIMD on a HashResponse RTT sample (D-05). Writes only hashWindow (never
    // hashWorkerCap), keeping the two dimensions decoupled (FR-14).
    auto sampleRttAndTuneNet = [&](const std::string& rel) {
        auto sa = sentAt.find(rel);
        if (sa == sentAt.end()) {
            return;
        }
        const double rttUs = static_cast<double>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - sa->second)
                .count());
        sentAt.erase(sa);
        hashWindow = NextNetWindow(hashWindow, rttUs, rttEwmaUs, kNetWindowMin, kNetWindowMax);
        rttEwmaUs = (rttEwmaUs <= 0.0) ? rttUs : (kRttEwmaAlpha * rttUs + (1.0 - kRttEwmaAlpha) * rttEwmaUs);
    };

    // HashResponse handling (FR-07/M6): pair with a ready local hash, else stash as pending.
    auto handleHashResponse = [&](const Frame& frame) {
        const std::pair<std::string, Hash256> resp = DecodeHashResponse(frame.payload);
        auto it = awaiting.find(resp.first);
        if (it == awaiting.end()) {
            return;  // Unknown/duplicate response, ignore.
        }
        --inFlight;
        sampleRttAndTuneNet(resp.first);
        bool haveLocal = false;
        LocalResult lr;
        {
            std::lock_guard<std::mutex> lk(hashResultMu);
            auto lit = localResults.find(resp.first);
            if (lit != localResults.end()) {
                lr = lit->second;
                haveLocal = true;
            }
        }
        if (haveLocal) {
            finalizePaired(it, lr, resp.second);
            if (localReadFailed) {
                return;
            }
        } else {
            serverHashReady[resp.first] = resp.second;  // pending until local result completes (FR-07)
        }
        pump();  // response freed a window slot
    };

    // Shared compare pipeline (fastcheck-compare-pipeline §4.1). The recv main thread only parses a
    // frame, inserts into manifestPaths, and Enqueue()s; ALL local metadata probing runs in the compare
    // worker pool (FR-02/AC-01/NFR-02). probe = ProbeLocal (Fast/SizeOnly, size+mtime, verbatim) or
    // StrictProbe (Strict, size-only directory_entry, D-03), injected so DecideCompare reaches the same
    // verdict as before (AC-24). onResultsReady wakes the main loop via the existing readyCv. Declared
    // after clientDriver (§6.4) and explicitly Stop()/Join()ed before clientDriver is destroyed (AC-16).
    fc::ComparePipelineConfig compareCfg;
    compareCfg.mode = mode;
    compareCfg.workerCount = std::max<std::size_t>(4, hardwareThreads);
    compareCfg.batchPop = 32;
    fc::LocalProbeFn compareProbe =
        (mode == CompareMode::Strict)
            ? fc::LocalProbeFn([&](const std::string& rel) { return StrictProbe(targetRoot, rel); })
            : fc::LocalProbeFn([&](const std::string& rel) { return ProbeLocal(targetRoot, rel); });
    fc::ComparePipeline comparePipeline(compareCfg, compareProbe, [&]() { readyCv.notify_one(); });

    // Bounded in-flight for the compare pipeline (NFR-07/AC-31); overflow parks in delayedCompareEntries
    // exactly like the FastClone client (mirrors maxInFlightCompareTasks).
    const std::size_t kCompareInFlightCap = std::max<std::size_t>(8192, compareCfg.workerCount * 256);
    std::deque<FileEntry> delayedCompareEntries;

    // Drain compare results and route by outcome (fastcheck-compare-pipeline §4.1): needHash==false is
    // final -> record; needHash==true -> hash schedule (Fast mtime-miss / Strict size-equal). The local
    // FileEntry captured by the compare probe is carried into HashNeed for later record/pairing. Missing
    // and SizeDiff are recorded here directly (candidate A: no eager HashRequest for them, no rollback).
    auto drainCompare = [&]() {
        std::vector<fc::ComparedItem> items;
        comparePipeline.Drain(items);
        for (fc::ComparedItem& item : items) {
            if (item.outcome.needHash) {
                hashQueue.push_back(HashNeed{item.remote, item.local});
                hashBytesTotal += item.remote.fileSize;
                ++hashEnqueued;
            } else {
                record(item.outcome.category, item.remote, item.local, false);
            }
        }
        if (!items.empty()) {
            pump();
        }
    };

    try {
        ch.send(Frame{MsgType::ManifestRequest, 0, {}});
        while (!manifestDone || comparePipeline.InFlight() > 0 || !delayedCompareEntries.empty() ||
               !hashQueue.empty() || inFlight > 0 || !awaiting.empty()) {
            if (interrupted.load()) {
                userInterrupted = true;
                break;
            }
            drainCompare();
            drainReady();
            if (localReadFailed) {
                break;
            }
            // Refill the compare pipeline from the overflow buffer while under the in-flight cap.
            while (!delayedCompareEntries.empty() && comparePipeline.InFlight() < kCompareInFlightCap) {
                comparePipeline.Enqueue(delayedCompareEntries.front());
                delayedCompareEntries.pop_front();
            }
            comparePipeline.Flush();
            pump();
            maybeTuneLocal();
            maybeEmitProgress();

            // Receive a frame only when one is guaranteed in transit AND the compare pipeline has room:
            // manifest not done and under the in-flight cap, or a HashResponse is in flight. Otherwise
            // only local compare/hash work remains -> wait on a worker completion instead of blocking
            // recv forever (D-02, prevents deadlock). When the compare pipeline saturates we stop
            // pulling manifest until Drain frees in-flight (NFR-07 backpressure).
            const bool manifestFramesWanted =
                (!manifestDone) && (comparePipeline.InFlight() < kCompareInFlightCap);
            const bool expectFrame = manifestFramesWanted || (inFlight > 0);
            if (expectFrame) {
                // Batch the receive: pull up to kRecvBatch frames per iteration and Enqueue each into the
                // pipeline's lock-free dispatch buffer, so the single Flush()/notify_all at the top of the
                // NEXT iteration covers the whole batch. Mirrors FastClone's batched ingest (one flush per
                // ~512-8192 frames) -- a per-frame Flush() would notify_all all compare workers per frame
                // (2.4M frames x 20 workers = 48M spurious wakeups + taskMu_ contention), which is the
                // ~19x slowdown versus FastClone on all-skip workloads. Re-check stillWant before each
                // recv so a blocking recv never waits for a frame that is not in transit (manifest done +
                // no hash in flight); ManifestEnd / localReadFailed / batch full also end the batch.
                constexpr size_t kRecvBatch = 4096;
                for (size_t batched = 0; batched < kRecvBatch; ++batched) {
                    const bool stillWant =
                        (!manifestDone && comparePipeline.InFlight() < kCompareInFlightCap) || (inFlight > 0);
                    if (!stillWant) {
                        break;
                    }
                    const Frame frame = ch.recv();
                    if (frame.type == MsgType::ManifestEntry) {
                        FileEntry remote = DecodeManifestEntry(frame.payload);
                        if (!remote.isDirectory) {
                            ++counters.enumerated;
                            manifestPaths.insert(remote.relativePath);  // recv-thread bookkeeping, not a stat (FR-13)
                            // No local probe on the recv path for ANY mode (FR-02/AC-01): enqueue into the
                            // compare pipeline; overflow above the in-flight cap parks in delayedCompareEntries.
                            if (comparePipeline.InFlight() >= kCompareInFlightCap) {
                                delayedCompareEntries.push_back(remote);
                            } else {
                                comparePipeline.Enqueue(remote);
                            }
                        }
                    } else if (frame.type == MsgType::ManifestProgress) {
                        // Progress hint, ignore.
                    } else if (frame.type == MsgType::ManifestEnd) {
                        manifestDone = true;
                        break;  // nothing more expected this batch; top-of-loop drains + refills
                    } else if (frame.type == MsgType::HashResponse) {
                        handleHashResponse(frame);
                        if (localReadFailed) {
                            break;
                        }
                    } else {
                        // Check should not receive transfer frames like File*/Delta*/BlockSig*; log a diagnostic and ignore.
                        std::cerr << "[check] unexpected frame in Check session type="
                                  << static_cast<int>(static_cast<uint8_t>(frame.type)) << std::endl;
                    }
                }
            } else {
                std::unique_lock<std::mutex> lk(readyMu);
                readyCv.wait_for(lk, std::chrono::milliseconds(200),
                                 [&]() { return !readyQueue.empty() || comparePipeline.HasResults(); });
            }
        }
    } catch (const std::exception& ex) {
        // Disconnect (recv throws) or send failure: mark partial, exit code 2 (NFR-07/AC-48).
        disconnected = true;
        errorText = ex.what();
    }

    // Stop and join the compare pipeline first (no external blocking, so it drains fast), then the hash
    // workers, then finalize any pending results now ready. Runs on every exit path so no worker hangs
    // and the driver is torn down only after every worker has joined (FR-09/AC-16).
    comparePipeline.Stop();
    comparePipeline.Join();
    joinWorkers();
    if (!localReadFailed) {
        drainReady();
    }

    EngineOutcome outcome;
    if (userInterrupted) {
        result.partial = true;
        outcome.exit = kInterrupted;
    } else if (disconnected) {
        result.partial = true;
        outcome.exit = kConnFailed;
        std::cerr << "error: connection lost during check: "
                  << (errorText.empty() ? "server disconnected" : errorText) << std::endl;
    } else if (localReadFailed) {
        result.partial = true;
        outcome.exit = kLocalPrecondFailed;
        std::cerr << "error: local file read failed during check: " << errorText << std::endl;
    } else {
        // Full comparison: enumerate local extras (FR-19), then decide exit code 0/1 (FR-14).
        const std::vector<std::string> extras = CollectExtraLocal(targetRoot, manifestPaths);
        for (const std::string& rel : extras) {
            FileEntry extraEntry;
            extraEntry.relativePath = rel;
            const std::optional<FileEntry> local = ProbeLocal(targetRoot, rel);
            // record fills remoteSize from remote.fileSize (not set for EXTRA) and localSize from local.
            record(fc::CompareCategory::Extra, extraEntry, local, false);
        }
        outcome.exit = (counters.diff == 0 && counters.missing == 0 && counters.extraLocal == 0)
                           ? kIdentical
                           : kDiffFound;
    }

    // Wrap-up: best-effort send SyncDone to reuse the server's clean early-exit path (section 8.1). On disconnect the socket is already dead, so ignore failures.
    if (!disconnected) {
        try {
            ch.send(Frame{MsgType::SyncDone, 0, {}});
        } catch (const std::exception&) {
            // Best-effort; a wrap-up send failure does not change the already-decided exit code.
        }
    }

    const auto endTime = std::chrono::steady_clock::now();
    result.durationMs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count());
    outcome.result = std::move(result);
    return outcome;
}

}  // namespace fc::check
