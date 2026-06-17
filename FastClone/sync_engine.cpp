#include "sync_engine.h"

#include "file_index.h"
#include "hash_memcache.h"
#include "path_utils.h"
#include "protocol.h"
#include "protocol_codec.h"
#include "sync_util.h"
#include "transfer_tuning.h"
#include "win_socket.h"

#ifdef _WIN32
#include <Windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <unistd.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <condition_variable>
#include <cctype>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <queue>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;

namespace fc {

namespace {

constexpr const char* kProtocolVersion = "FC5";

// Schedule the next reconnect attempt after a transient failure (session drop or
// server-not-ready connect/handshake). Returns the process exit code when the
// caller must terminate; std::nullopt means backoff completed and the outer session
// loop should continue (retry ConnectTo).
std::optional<int> ScheduleClientReconnectOrExit(const std::string& reason,
                                                  uint32_t reconnectRetries,
                                                  uint64_t reconnectWindowMs,
                                                  uint32_t& reconnectAttemptsUsed,
                                                  const std::chrono::steady_clock::time_point& reconnectWindowStart,
                                                  int exitWhenDisabled) {
    if (IsFatalClientDisconnectReason(reason)) {
        std::cerr << "[reconnect] fatal error, not retrying: \"" << reason << "\"" << std::endl;
        return 1;
    }
    if (reconnectRetries == 0) {
        return exitWhenDisabled;
    }
    const auto reconnectNow = std::chrono::steady_clock::now();
    const auto reconnectWindowLimit = std::chrono::milliseconds(reconnectWindowMs);
    if (reconnectAttemptsUsed >= reconnectRetries ||
        (reconnectNow - reconnectWindowStart) > reconnectWindowLimit) {
        std::cerr << "[reconnect] budget exhausted attempts=" << reconnectAttemptsUsed
                  << "/" << reconnectRetries << " reason=\"" << reason << "\"" << std::endl;
        return 4;
    }
    ++reconnectAttemptsUsed;
    const uint32_t backoffShift = std::min<uint32_t>(reconnectAttemptsUsed - 1, 5u);
    const uint32_t backoffSec = std::min<uint32_t>(30u, 1u << backoffShift);
    std::cerr << "[reconnect] attempt=" << reconnectAttemptsUsed << " reason=\"" << reason
              << "\" backoff_s=" << backoffSec << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(backoffSec));
    return std::nullopt;
}

// Diagnostic wrapper around std::thread::join(). The exception
// "resource deadlock would occur" is thrown by join() in exactly one case: a
// thread tries to join ITSELF (t.get_id() == this_thread::get_id()), which on
// MSVC surfaces as std::system_error{errc::resource_deadlock_would_occur}.
// Behaviour:
//   - Normal join: wrapped in try/catch so the throwing call SITE is logged.
//   - Self-join: this signals corrupted thread ownership; we FAIL FAST (log the
//     site + std::abort). We deliberately do NOT skip or detach -- neither is a
//     safe recovery (see the detailed rationale at the self-join branch below),
//     and hiding the error would carry the corruption into later, harder-to-debug
//     failures. A run that hits the self-join branch is NOT a successful sync.
inline void JoinDiag(std::thread& t, const char* site) {
    if (!t.joinable()) {
        return;
    }
    const std::thread::id self = std::this_thread::get_id();
    const std::thread::id target = t.get_id();
    if (target == self) {
        // SELF-JOIN: a thread is trying to join itself -- the exact condition that
        // raises errc::resource_deadlock_would_occur. With the current design this is
        // UNREACHABLE: every join runs on a thread that does not own the target. If it
        // ever fires, thread ownership has already been violated (e.g. memory
        // corruption, or a captured std::thread reused across threads), so the process
        // state is no longer trustworthy.
        //
        // We FAIL FAST rather than pretend to recover:
        //   - join() is physically impossible here (would deadlock).
        //   - detach() is NOT a safe recovery: it only releases the std::thread handle.
        //     The thread body still references [&]-captured stack objects (mutex/cv/
        //     queue/map/socket); once the owning scope unwinds they are destroyed,
        //     producing use-after-free, silent corruption, false "success", or a hang
        //     -- strictly worse than a loud, immediate abort.
        // The site label pinpoints exactly which join detected the violation; any run
        // that reaches here must NOT be treated as a successful sync.
        std::cerr << "[deadlock-diag] FATAL SELF-JOIN at site=" << site
                  << " thread_id=" << target
                  << " -- thread ownership violated; aborting (state not trustworthy)"
                  << std::endl;
        std::cerr.flush();
        std::abort();
    }
    try {
        t.join();
    } catch (const std::system_error& e) {
        std::cerr << "[deadlock-diag] join FAILED at site=" << site
                  << " code=" << e.code().value()
                  << " msg=\"" << e.code().message() << "\""
                  << " caller_thread=" << self
                  << " target_thread=" << target
                  << std::endl;
        throw;
    }
}

struct ServerStream {
    std::ifstream input;
    std::string relativePath;
};

struct ServerBatchStream {
    std::vector<BatchFileRecord> files;
    size_t index = 0;
    bool headerSent = false;
    std::ifstream input;
    uint64_t remainingBytes = 0;
};

struct ClientHashTask {
    std::string relPath;
    fs::path absPath;
};

enum class CompareAction {
    Skip,
    TransferNow,
    FallbackHash
};

CompareAction DecideCompareAction(const std::optional<FileEntry>& localFile, const FileEntry& remoteFile) {
    constexpr int64_t kMtimeToleranceNs = 2LL * 1000LL * 1000LL;  // 2ms tolerance
    constexpr int64_t kLegacyRawTolerance = 2LL * 1000LL * 1000LL;
    if (!localFile.has_value()) {
        return CompareAction::TransferNow;
    }
    if (localFile->fileSize != remoteFile.fileSize) {
        return CompareAction::TransferNow;
    }

    // Cross-platform compare: normalize both sides to Unix ns before tolerance check.
    int64_t localUnixNs = 0;
    int64_t remoteUnixNs = 0;
    const bool localNormalized = TryNormalizeMtimeToUnixNs(localFile->mtimeNs, localUnixNs);
    const bool remoteNormalized = TryNormalizeMtimeToUnixNs(remoteFile.mtimeNs, remoteUnixNs);
    if (localNormalized && remoteNormalized) {
        const int64_t mtimeDeltaNs = std::llabs(static_cast<long long>(localUnixNs - remoteUnixNs));
        if (mtimeDeltaNs <= kMtimeToleranceNs) {
            return CompareAction::Skip;
        }
    } else {
        // Compatibility fallback for legacy/invalid timestamp payloads.
        const int64_t rawDelta = std::llabs(static_cast<long long>(localFile->mtimeNs - remoteFile.mtimeNs));
        if (rawDelta <= kLegacyRawTolerance) {
            return CompareAction::Skip;
        }
    }
    return CompareAction::FallbackHash;
}

void SendSimple(const SocketHandle& socket, MsgType type, const std::string& text = {}) {
    Frame frame;
    frame.type = type;
    frame.streamId = 0;
    frame.payload.assign(text.begin(), text.end());
    SendFrame(socket, frame);
}

void EnsureHandshakeAsServer(const SocketHandle& socket, const std::string& password) {
    const Frame hello = RecvFrame(socket);
    if (hello.type != MsgType::Hello) {
        throw std::runtime_error("Expected HELLO");
    }
    const std::string clientVersion(reinterpret_cast<const char*>(hello.payload.data()), hello.payload.size());
    if (clientVersion != kProtocolVersion) {
        SendSimple(socket, MsgType::Error, "Protocol version mismatch: server=" + std::string(kProtocolVersion) + " client=" + clientVersion);
        throw std::runtime_error("Protocol version mismatch: server=" + std::string(kProtocolVersion) + " client=" + clientVersion);
    }
    SendSimple(socket, MsgType::Hello, kProtocolVersion);

    const Frame auth = RecvFrame(socket);
    if (auth.type != MsgType::Auth) {
        throw std::runtime_error("Expected AUTH");
    }
    const std::string got(reinterpret_cast<const char*>(auth.payload.data()), auth.payload.size());
    if (got != password) {
        SendSimple(socket, MsgType::AuthFail, "bad password");
        throw std::runtime_error("Authentication failed");
    }
    SendSimple(socket, MsgType::AuthOk, "ok");
}

void EnsureHandshakeAsClient(const SocketHandle& socket, const std::string& password) {
    SendSimple(socket, MsgType::Hello, kProtocolVersion);
    Frame helloBack = RecvFrame(socket);
    if (helloBack.type == MsgType::Error) {
        const std::string payload(reinterpret_cast<const char*>(helloBack.payload.data()), helloBack.payload.size());
        throw std::runtime_error("Server error: " + payload);
    }
    if (helloBack.type != MsgType::Hello) {
        throw std::runtime_error("Server HELLO missing");
    }
    const std::string serverVersion(reinterpret_cast<const char*>(helloBack.payload.data()), helloBack.payload.size());
    if (serverVersion != kProtocolVersion) {
        throw std::runtime_error("Protocol version mismatch: client=" + std::string(kProtocolVersion) + " server=" + serverVersion);
    }
    SendSimple(socket, MsgType::Auth, password);
    Frame authResult = RecvFrame(socket);
    if (authResult.type != MsgType::AuthOk) {
        throw std::runtime_error("Server authentication rejected");
    }
}

inline void AtomicMaxU64(std::atomic<uint64_t>& target, uint64_t value) {
    uint64_t prev = target.load(std::memory_order_relaxed);
    while (value > prev &&
           !target.compare_exchange_weak(prev, value, std::memory_order_relaxed)) {
        // prev was reloaded by compare_exchange_weak; loop until our value is no longer
        // larger or the store succeeds.
    }
}

// Worker count for the parallel directory walks (enumeration + deletion). These are
// latency-bound metadata reads, so oversubscribe the cores to keep the device queue
// deep, but cap the fan-out: past ~16 concurrent listings the queue is saturated and more
// threads only add work-queue lock churn.
inline unsigned ResolveDirWalkWorkerCount() {
    const unsigned hw = std::max<unsigned>(1u, std::thread::hardware_concurrency());
    return std::min<unsigned>(hw * 2, 16u);
}

// rel = relDir + "/" + name (or just name at the root). Single definition for every walk.
inline std::string BuildRelPath(const std::string& relDir, const std::string& name) {
    return relDir.empty() ? name : (relDir + "/" + name);
}

#ifdef _WIN32
// Open a FindFirstFileExW enumeration handle for a directory's children. Centralises the
// "append \\*" pattern build and the (basic-info + large-fetch) flags both walks use.
inline HANDLE OpenDirFind(const std::wstring& absDir, WIN32_FIND_DATAW& fd) {
    std::wstring pattern = absDir;
    if (!pattern.empty() && pattern.back() != L'\\' && pattern.back() != L'/') {
        pattern.push_back(L'\\');
    }
    pattern.append(L"*");
    return FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd,
                            FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
}
#endif

// Diagnostics for the parallel enumeration. All cumulative; the debug printer derives
// per-interval rates/averages by differencing snapshots. The point is to tell apart the
// two things that can throttle enumeration on fast storage:
//   - listing_us  : wall time spent INSIDE processDir (FindFirstFile/readdir) == disk
//                   metadata I/O. If this is what climbs past the 3M mark, the wall is
//                   the device, not our code.
//   - flush_block_us : wall time a worker is blocked handing its frame chunk to the
//                   outbound queue (backpressure + lock). If THIS dominates, the sender/
//                   socket downstream is the cap, not the disk.
// frames_per_flush confirms the batching is actually amortising the outbound lock.
struct EnumStats {
    std::atomic<uint64_t> dirsProcessed{0};
    std::atomic<uint64_t> framesFlushed{0};
    std::atomic<uint64_t> flushCount{0};
    std::atomic<uint64_t> listingUsSum{0};
    std::atomic<uint64_t> listingUsMax{0};
    std::atomic<uint64_t> flushBlockUsSum{0};
    std::atomic<uint64_t> flushBlockUsMax{0};
};

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
static void ParallelDirWalk(PendingDir rootDir,
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

void EnumerateManifestEntriesFast(
    const fs::path& root,
    const std::optional<fs::path>& selfPath,
    const std::atomic<bool>& done,
    const std::function<void(std::vector<Frame>&)>& flushManifestFrames,
    EnumStats& stats) {
    // Hand a worker's buffered frames to the outbound queue (one lock per chunk) and
    // record the hand-off latency. Used both at the per-chunk threshold and on drain.
    auto flushAndMeasure = [&](std::vector<Frame>& buf) {
        const uint64_t frames = buf.size();
        const auto t0 = std::chrono::steady_clock::now();
        flushManifestFrames(buf);  // hands off and clears buf
        const uint64_t us = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - t0).count());
        stats.framesFlushed.fetch_add(frames, std::memory_order_relaxed);
        stats.flushCount.fetch_add(1, std::memory_order_relaxed);
        stats.flushBlockUsSum.fetch_add(us, std::memory_order_relaxed);
        AtomicMaxU64(stats.flushBlockUsMax, us);
    };
    auto finishWorker = [&](std::vector<Frame>& buf) {
        if (!buf.empty()) {
            flushAndMeasure(buf);
        }
    };
    // Time the pure listing of one directory and, after it, flush the worker buffer if it
    // crossed the chunk threshold. Wrapped around the platform listing so the timing
    // excludes the (separately measured) hand-off.
    auto runListing = [&](auto&& listOneDir, const auto& current, auto& subdirsRef,
                          std::vector<Frame>& out) {
        const auto t0 = std::chrono::steady_clock::now();
        listOneDir(current, subdirsRef, out);
        const uint64_t us = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - t0).count());
        stats.listingUsSum.fetch_add(us, std::memory_order_relaxed);
        AtomicMaxU64(stats.listingUsMax, us);
        stats.dirsProcessed.fetch_add(1, std::memory_order_relaxed);
        if (out.size() >= kFrameFlushThreshold) {
            flushAndMeasure(out);
        }
    };
    std::atomic<uint64_t> fileCount{0};
#ifdef _WIN32
    auto FileTimeToTicks = [](FILETIME ft) -> int64_t {
        ULARGE_INTEGER v{};
        v.LowPart = ft.dwLowDateTime;
        v.HighPart = ft.dwHighDateTime;
        return static_cast<int64_t>(v.QuadPart);
    };

    struct PendingDir {
        std::wstring absDir;
        std::string relDir;
    };

    // Extended-length ("\\?\") root so deep source trees (root + relpath > 260) enumerate
    // instead of FindFirstFile silently failing and dropping whole subtrees from the
    // manifest. selfW is prefixed too so the self-exclude comparison stays consistent.
    const std::wstring rootW = ToExtendedLengthPath(root);
    const std::wstring selfW = selfPath.has_value() ? ToExtendedLengthPath(*selfPath) : L"";

    auto listOneDir = [&](const PendingDir& current, std::vector<PendingDir>& subdirs,
                          std::vector<Frame>& out) {
        WIN32_FIND_DATAW fd{};
        HANDLE hFind = OpenDirFind(current.absDir, fd);
        if (hFind == INVALID_HANDLE_VALUE) {
            return;
        }

        do {
            if (done.load()) {
                break;
            }

            const wchar_t* name = fd.cFileName;
            if ((name[0] == L'.' && name[1] == L'\0') ||
                (name[0] == L'.' && name[1] == L'.' && name[2] == L'\0')) {
                continue;
            }

            const bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            std::wstring absPath = current.absDir;
            if (!absPath.empty() && absPath.back() != L'\\' && absPath.back() != L'/') {
                absPath.push_back(L'\\');
            }
            absPath.append(name);

            if (!isDir && !selfW.empty() && _wcsicmp(absPath.c_str(), selfW.c_str()) == 0) {
                continue;
            }

            const std::string nameUtf8 = WideToUtf8(name);
            std::string relPath = BuildRelPath(current.relDir, nameUtf8);

            FileEntry entry;
            entry.relativePath = relPath;
            entry.isDirectory = isDir;
            entry.fileSize = isDir ? 0 : (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
            entry.mtimeNs = FileTimeToTicks(fd.ftLastWriteTime);
            out.push_back(Frame{MsgType::ManifestEntry, 0, EncodeManifestEntry(entry)});

            if (isDir) {
                subdirs.push_back(PendingDir{std::move(absPath), std::move(relPath)});
            } else {
                const uint64_t c = fileCount.fetch_add(1, std::memory_order_relaxed) + 1;
                if (c % 2048 == 0) {
                    std::vector<uint8_t> payload;
                    AppendU64(payload, c);
                    out.push_back(Frame{MsgType::ManifestProgress, 0, std::move(payload)});
                }
            }
        } while (FindNextFileW(hFind, &fd) != 0);

        FindClose(hFind);
    };
    auto processDir = [&](const PendingDir& current, std::vector<PendingDir>& subdirs,
                          std::vector<Frame>& out) {
        runListing(listOneDir, current, subdirs, out);
    };

    const unsigned numWorkers = ResolveDirWalkWorkerCount();
    ParallelDirWalk(PendingDir{rootW, std::string()}, numWorkers, kDirPopBatch, done,
                    "server-enum-walk", std::vector<Frame>{}, processDir, finishWorker);
#else
    struct PendingDir {
        fs::path absDir;
        std::string relDir;
    };

    auto listOneDir = [&](const PendingDir& current, std::vector<PendingDir>& subdirs,
                          std::vector<Frame>& out) {
        std::error_code ec;
        fs::directory_iterator it(current.absDir, fs::directory_options::skip_permission_denied, ec);
        const fs::directory_iterator end;
        if (ec) {
            return;
        }
        for (; it != end; it.increment(ec)) {
            if (done.load()) {
                return;
            }
            if (ec) {
                ec.clear();
                continue;
            }

            const fs::path& absPath = it->path();
            if (selfPath.has_value()) {
                std::error_code eqec;
                if (fs::equivalent(absPath, *selfPath, eqec) && !eqec) {
                    continue;
                }
            }

            const bool isDir = it->is_directory(ec);
            if (ec) {
                ec.clear();
                continue;
            }
            const bool isRegular = it->is_regular_file(ec);
            if (ec) {
                ec.clear();
                continue;
            }
            if (!isDir && !isRegular) {
                continue;
            }
            // Match recursive_directory_iterator's default: do not descend into
            // symlinked directories (avoids cycles), but still emit the entry.
            const bool isSymlink = it->is_symlink(ec);
            if (ec) {
                ec.clear();
            }

            const std::string name = absPath.filename().string();
            std::string relPath = BuildRelPath(current.relDir, name);
            if (relPath.empty()) {
                continue;
            }

            FileEntry entry;
            entry.relativePath = relPath;
            entry.isDirectory = isDir;
            if (isDir) {
                entry.fileSize = 0;
            } else {
                entry.fileSize = static_cast<uint64_t>(fs::file_size(absPath, ec));
                if (ec) {
                    ec.clear();
                    continue;
                }
                const uint64_t c = fileCount.fetch_add(1, std::memory_order_relaxed) + 1;
                if (c % 2048 == 0) {
                    std::vector<uint8_t> payload;
                    AppendU64(payload, c);
                    out.push_back(Frame{MsgType::ManifestProgress, 0, std::move(payload)});
                }
            }
            entry.mtimeNs = ToUnixNs(fs::last_write_time(absPath, ec));
            if (ec) {
                entry.mtimeNs = 0;
                ec.clear();
            }
            out.push_back(Frame{MsgType::ManifestEntry, 0, EncodeManifestEntry(entry)});

            if (isDir && !isSymlink) {
                subdirs.push_back(PendingDir{absPath, std::move(relPath)});
            }
        }
    };
    auto processDir = [&](const PendingDir& current, std::vector<PendingDir>& subdirs,
                          std::vector<Frame>& out) {
        runListing(listOneDir, current, subdirs, out);
    };

    const unsigned numWorkers = ResolveDirWalkWorkerCount();
    ParallelDirWalk(PendingDir{root, std::string()}, numWorkers, kDirPopBatch, done,
                    "server-enum-walk", std::vector<Frame>{}, processDir, finishWorker);
#endif

    // Common tail (both platforms): flush the final progress + ManifestEnd, unless the
    // walk was cancelled.
    if (done.load()) {
        return;
    }
    std::vector<Frame> tail;
    std::vector<uint8_t> payload;
    AppendU64(payload, fileCount.load());
    tail.push_back(Frame{MsgType::ManifestProgress, 0, std::move(payload)});
    tail.push_back(Frame{MsgType::ManifestEnd, 0, {}});
    flushManifestFrames(tail);
}

struct RemoveLocalExtrasResult {
    size_t deletedFiles = 0;
    size_t failedOps = 0;
};

// Parallel reconciliation + delete. The local tree is walked by the same worker-pool
// engine as enumeration (deletion is pure metadata I/O -- listing dirs + unlinking --
// so it hits the exact MFT-latency wall single-threaded; fan-out raises queue depth).
// Per directory, a worker FIRST enumerates fully, THEN deletes the files that are absent
// from the remote manifest: never delete while the directory handle is open (modifying a
// directory mid-enumeration is undefined on both NTFS and POSIX). remoteFiles/remoteDirs
// are immutable here (post-ManifestEnd), so the membership lookups are lock-free
// concurrent reads. Extra directories are collected per worker and removed deepest-first
// at the very end (serial: directory count is tiny next to files, and deepest-first has
// an inherent ordering dependency).
RemoveLocalExtrasResult RemoveLocalExtras(const fs::path& root,
                                          const std::unordered_set<std::string>& remoteDirs,
                                          const std::unordered_map<std::string, FileEntry>& remoteFiles,
                                          const std::optional<fs::path>& exclude,
                                          std::unordered_set<std::string>& existingLocalDirs) {
    RemoveLocalExtrasResult result;
    std::atomic<uint64_t> deletedFiles{0};
    std::atomic<uint64_t> failedOps{0};
    std::mutex extraDirsMu;
    std::vector<std::string> extraDirs;        // local dirs absent from remoteDirs
    const std::atomic<bool> noCancel{false};   // deletion walk is not cancellable

    struct DelCtx {
        std::vector<std::string> extraDirs;    // per-worker, merged on finish
        std::vector<std::string> localDirs;    // every existing local dir this worker saw
    };
    auto mergeCtx = [&](DelCtx& ctx) {
        std::lock_guard<std::mutex> lock(extraDirsMu);
        extraDirs.insert(extraDirs.end(),
                         std::make_move_iterator(ctx.extraDirs.begin()),
                         std::make_move_iterator(ctx.extraDirs.end()));
        ctx.extraDirs.clear();
        // Hand back the set of directories that already exist locally so the caller can
        // skip create_directories() for them (on a re-sync that is all of them, and those
        // calls are pure wasted, filter-driver-intercepted metadata I/O).
        existingLocalDirs.insert(std::make_move_iterator(ctx.localDirs.begin()),
                                 std::make_move_iterator(ctx.localDirs.end()));
        ctx.localDirs.clear();
    };

    const unsigned numWorkers = ResolveDirWalkWorkerCount();

#ifdef _WIN32
    struct PendingDir {
        std::wstring absDir;
        std::string relDir;
    };
    const std::wstring excludeW = exclude.has_value() ? ToExtendedLengthPath(*exclude) : L"";
    auto processDir = [&](const PendingDir& current, std::vector<PendingDir>& subdirs, DelCtx& ctx) {
        WIN32_FIND_DATAW fd{};
        HANDLE hFind = OpenDirFind(current.absDir, fd);
        if (hFind == INVALID_HANDLE_VALUE) {
            return;
        }
        // Phase 1: enumerate fully; collect deletions but do NOT touch the directory yet.
        std::vector<std::wstring> filesToDelete;
        do {
            const wchar_t* name = fd.cFileName;
            if ((name[0] == L'.' && name[1] == L'\0') ||
                (name[0] == L'.' && name[1] == L'.' && name[2] == L'\0')) {
                continue;
            }
            std::wstring absPath = current.absDir;
            if (!absPath.empty() && absPath.back() != L'\\' && absPath.back() != L'/') {
                absPath.push_back(L'\\');
            }
            absPath.append(name);
            if (!excludeW.empty() && _wcsicmp(absPath.c_str(), excludeW.c_str()) == 0) {
                continue;
            }
            const bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            const std::string nameUtf8 = WideToUtf8(name);
            std::string relPath = BuildRelPath(current.relDir, nameUtf8);
            if (isDir) {
                if (!remoteDirs.contains(relPath)) {
                    ctx.extraDirs.push_back(relPath);
                }
                ctx.localDirs.push_back(relPath);
                subdirs.push_back(PendingDir{std::move(absPath), std::move(relPath)});
            } else if (!remoteFiles.contains(relPath)) {
                filesToDelete.push_back(std::move(absPath));
            }
        } while (FindNextFileW(hFind, &fd) != 0);
        FindClose(hFind);
        // Phase 2: delete now that the find handle is closed.
        for (const std::wstring& abs : filesToDelete) {
            std::error_code ec;
            if (fs::remove(fs::path(abs), ec)) {
                deletedFiles.fetch_add(1, std::memory_order_relaxed);
            } else if (ec) {
                failedOps.fetch_add(1, std::memory_order_relaxed);
            }
        }
    };
    ParallelDirWalk(PendingDir{ToExtendedLengthPath(root), std::string()}, numWorkers, kDeleteDirPopBatch,
                    noCancel, "client-delete-walk", DelCtx{}, processDir, mergeCtx);
#else
    struct PendingDir {
        fs::path absDir;
        std::string relDir;
    };
    auto processDir = [&](const PendingDir& current, std::vector<PendingDir>& subdirs, DelCtx& ctx) {
        std::error_code ec;
        fs::directory_iterator it(current.absDir, fs::directory_options::skip_permission_denied, ec);
        const fs::directory_iterator end;
        if (ec) {
            return;
        }
        std::vector<fs::path> filesToDelete;
        for (; it != end; it.increment(ec)) {
            if (ec) {
                ec.clear();
                continue;
            }
            const fs::path& absPath = it->path();
            if (exclude.has_value()) {
                std::error_code eqec;
                if (fs::equivalent(absPath, *exclude, eqec) && !eqec) {
                    continue;
                }
            }
            const bool isDir = it->is_directory(ec);
            if (ec) {
                ec.clear();
                continue;
            }
            const bool isRegular = it->is_regular_file(ec);
            if (ec) {
                ec.clear();
                continue;
            }
            if (!isDir && !isRegular) {
                continue;
            }
            const bool isSymlink = it->is_symlink(ec);
            if (ec) {
                ec.clear();
            }
            const std::string name = absPath.filename().string();
            std::string relPath = BuildRelPath(current.relDir, name);
            if (relPath.empty()) {
                continue;
            }
            if (isDir) {
                if (!remoteDirs.contains(relPath)) {
                    ctx.extraDirs.push_back(relPath);
                }
                ctx.localDirs.push_back(relPath);
                if (!isSymlink) {
                    subdirs.push_back(PendingDir{absPath, std::move(relPath)});
                }
            } else if (!remoteFiles.contains(relPath)) {
                filesToDelete.push_back(absPath);
            }
        }
        for (const fs::path& abs : filesToDelete) {
            std::error_code rec;
            if (fs::remove(abs, rec)) {
                deletedFiles.fetch_add(1, std::memory_order_relaxed);
            } else if (rec) {
                failedOps.fetch_add(1, std::memory_order_relaxed);
            }
        }
    };
    ParallelDirWalk(PendingDir{root, std::string()}, numWorkers, kDeleteDirPopBatch,
                    noCancel, "client-delete-walk", DelCtx{}, processDir, mergeCtx);
#endif

    std::sort(extraDirs.begin(), extraDirs.end(), [](const std::string& a, const std::string& b) {
        return a.size() > b.size();
    });
    for (const std::string& dir : extraDirs) {
        std::error_code ec;
        fs::remove(JoinRel(root, dir), ec);
        if (ec) {
            failedOps.fetch_add(1, std::memory_order_relaxed);
        }
    }
    result.deletedFiles = static_cast<size_t>(deletedFiles.load());
    result.failedOps = static_cast<size_t>(failedOps.load());
    return result;
}

uint32_t ResolveServerHashWorkerCount(const CliOptions& options) {
    if (options.serverHashWorkers != 0) {
        return options.serverHashWorkers;
    }
    const uint32_t hw = std::max<uint32_t>(1, std::thread::hardware_concurrency());
    uint32_t workers = hw / 2;
    if (workers == 0) {
        workers = 1;
    }
    if (workers < 2 && hw > 1) {
        workers = 2;
    }
    workers = std::min<uint32_t>(workers, 16);
    return workers;
}

class ServerHashThreadPool {
public:
    ~ServerHashThreadPool() {
        Stop();
    }

    void Configure(uint32_t workerCount) {
        if (workerCount == 0) {
            throw std::runtime_error("Server hash worker count must be > 0");
        }
        std::lock_guard<std::mutex> lock(mu_);
        if (!workers_.empty()) {
            if (configuredWorkers_ != workerCount) {
                throw std::runtime_error("Server hash pool already configured with different worker count");
            }
            return;
        }
        configuredWorkers_ = workerCount;
        stop_ = false;
        for (uint32_t i = 0; i < workerCount; ++i) {
            workers_.emplace_back([this]() {
                while (true) {
                    std::function<void()> task;
                    {
                        std::unique_lock<std::mutex> lock(mu_);
                        cv_.wait(lock, [&]() { return stop_ || !tasks_.empty(); });
                        if (stop_ && tasks_.empty()) {
                            return;
                        }
                        task = std::move(tasks_.front());
                        tasks_.pop_front();
                    }
                    activeTasks_.fetch_add(1, std::memory_order_relaxed);
                    // Backstop: a job MUST NOT escape an exception here. This worker loop has
                    // no caller to catch it, so an escaping exception would std::terminate the
                    // whole server. Individual jobs are expected to handle their own cleanup;
                    // this only guarantees one bad task cannot take down the entire pool.
                    try {
                        task();
                    } catch (...) {
                    }
                    activeTasks_.fetch_sub(1, std::memory_order_relaxed);
                }
            });
        }
    }

    void Enqueue(std::function<void()> task) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (workers_.empty()) {
                throw std::runtime_error("Server hash pool is not configured");
            }
            tasks_.push_back(std::move(task));
        }
        cv_.notify_one();
    }

    size_t PendingTasks() const {
        std::lock_guard<std::mutex> lock(mu_);
        return tasks_.size();
    }

    // Tasks currently executing across all sessions (lock-free gauge).
    size_t ActiveTasks() const {
        return activeTasks_.load(std::memory_order_relaxed);
    }

private:
    void Stop() {
        {
            std::lock_guard<std::mutex> lock(mu_);
            stop_ = true;
        }
        cv_.notify_all();
        for (auto& worker : workers_) {
            JoinDiag(worker, "server-hashpool-stop");
        }
        workers_.clear();
    }

    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> tasks_;
    std::vector<std::thread> workers_;
    uint32_t configuredWorkers_ = 0;
    bool stop_ = false;
    std::atomic<size_t> activeTasks_{0};
};

ServerHashThreadPool& GetServerHashPool() {
    static ServerHashThreadPool pool;
    return pool;
}

bool TryReadHashFingerprint(const fs::path& absPath, HashFingerprint& out) {
    std::error_code ec;
    if (!fs::exists(absPath, ec) || ec) {
        return false;
    }
    if (!fs::is_regular_file(absPath, ec) || ec) {
        return false;
    }
    const uint64_t size = static_cast<uint64_t>(fs::file_size(absPath, ec));
    if (ec) {
        return false;
    }
    const int64_t mtimeNs = ToUnixNs(fs::last_write_time(absPath, ec));
    if (ec) {
        return false;
    }
    out.fileSize = size;
    out.mtimeNs = mtimeNs;
    return true;
}

ServerHashMemCache& GetServerHashMemCache() {
    static ServerHashMemCache cache;
    return cache;
}

void RunSessionServer(const SocketHandle& client, const CliOptions& options) {
    EnsureHandshakeAsServer(client, options.password);
    const std::optional<fs::path> selfPath = CurrentExePath();
    const bool debugEnabled = IsDebugEnabled();
    const bool hashMemcacheEnabled = GetServerHashMemCache().Enabled();
    const TunedTransferOptions tuned = ResolveTransferOptions(options);
    const uint32_t streamLimit = tuned.streamLimit;
    const uint32_t effectiveChunkSize = EffectiveChunkSizeForStreams(tuned.chunkSize, streamLimit);

    static std::atomic<uint64_t> g_sessionIdCounter{0};
    const uint64_t sessionId = g_sessionIdCounter.fetch_add(1, std::memory_order_relaxed) + 1;

    // Lock ordering (must never nest in the other direction):
    //   mu  >  sessionHashMu
    // i.e. it is forbidden to acquire sessionHashMu while holding mu, or vice versa.
    // `mu` protects only the outbound queues and the hand-off queues below; it must
    // NOT cover file I/O. The active*Streams containers are owned exclusively by the
    // main send loop thread (adopted from pendingNew*Streams) and are accessed without
    // mu, so all open()/read() happens outside any lock.
    std::unordered_map<uint32_t, ServerStream> activeStreams;        // main-loop private
    std::unordered_map<uint32_t, ServerBatchStream> activeBatchStreams;  // main-loop private
    std::mutex mu;
    std::condition_variable outboundCv;
    std::queue<Frame> outboundHigh;
    std::queue<Frame> outboundManifest;
    // Hand-off queues: receiver pushes newly opened streams here (cheap move under mu);
    // the main loop adopts them into its private containers, then does I/O lock-free.
    std::vector<std::pair<uint32_t, ServerStream>> pendingNewStreams;
    std::vector<std::pair<uint32_t, ServerBatchStream>> pendingNewBatchStreams;
    // Sized so all enumeration workers can each have a full flush chunk in flight without
    // serialising on backpressure (workers * kFrameFlushThreshold, rounded up). RAM is
    // cheap relative to the throughput win; the sender drains this continuously.
    const size_t maxQueuedManifestFrames = 16384;
    std::atomic<bool> done = false;
    std::atomic<bool> failed = false;
    std::string errorText;
    std::thread manifestThread;
    std::atomic<bool> manifestStarted = false;
    EnumStats enumStats;  // parallel-enumeration diagnostics (see EnumStats / debug printer)
    std::mutex sessionHashMu;  // lock order: acquire only when NOT holding mu (mu > sessionHashMu)
    std::condition_variable sessionHashCv;
    size_t sessionPendingHashJobs = 0;

    // Per-session hash-pipeline diagnostics (debug-only output). All relaxed atomics:
    // written from the receiver thread and the global pool's worker threads, read by the
    // main send loop's debug printer. Relaxed is sufficient because these are monotone
    // counters used only for observability, not for synchronization.
    std::atomic<uint64_t> hashRequestsReceived{0};   // HashRequest frames read from socket
    std::atomic<uint64_t> hashCacheHits{0};          // served directly from memcache (no pool job)
    std::atomic<uint64_t> hashJobsEnqueued{0};       // jobs handed to the global pool
    std::atomic<uint64_t> hashJobsCompleted{0};      // pool jobs that finished
    std::atomic<uint64_t> hashResponsesEnqueued{0};  // HashResponse frames pushed to outboundHigh
    std::atomic<uint64_t> hashResponsesSent{0};      // HashResponse frames handed to the socket batch

    auto enqueueHigh = [&](Frame frame) {
        std::lock_guard<std::mutex> lock(mu);
        outboundHigh.push(std::move(frame));
        outboundCv.notify_one();
    };
    // Batched manifest hand-off: enumeration workers buffer frames locally and flush a
    // chunk at a time, so the outbound queue lock is taken once per chunk instead of once
    // per file. Backpressure is checked once for the whole chunk (a chunk may briefly
    // push the queue past the soft limit, which is fine -- the limit only throttles how
    // far producers run ahead of the sender). The vector is cleared on return so a
    // cancelled flush cannot double-enqueue.
    auto flushManifest = [&](std::vector<Frame>& frames) {
        if (frames.empty()) {
            return;
        }
        {
            std::unique_lock<std::mutex> lock(mu);
            outboundCv.wait(lock, [&]() {
                return done.load() || outboundManifest.size() < maxQueuedManifestFrames;
            });
            if (!done.load()) {
                for (Frame& frame : frames) {
                    outboundManifest.push(std::move(frame));
                }
            }
        }
        frames.clear();
        outboundCv.notify_one();
    };

    std::thread receiver([&]() {
        try {
            while (!done.load()) {
                Frame frame = RecvFrame(client);
                if (frame.type == MsgType::ManifestRequest) {
                    if (manifestStarted.exchange(true)) {
                        continue;
                    }
                    manifestThread = std::thread([&]() {
                        try {
                            EnumerateManifestEntriesFast(options.rootDir, selfPath, done, flushManifest, enumStats);
                        } catch (const std::exception& ex) {
                            failed.store(true);
                            done.store(true);
                            sessionHashCv.notify_all();
                            // Wake the sender (and any producer parked on manifest
                            // backpressure) so it observes done and tears down promptly
                            // instead of waiting out its poll timeout.
                            outboundCv.notify_all();
                            errorText = ex.what();
                        }
                    });
                } else if (frame.type == MsgType::HashRequest) {
                    hashRequestsReceived.fetch_add(1, std::memory_order_relaxed);
                    const std::string rel = DecodeHashRequest(frame.payload);
                    const fs::path abs = JoinRel(options.rootDir, rel);
                    HashFingerprint fingerprint;
                    const bool fingerprintValid = TryReadHashFingerprint(abs, fingerprint);
                    if (hashMemcacheEnabled && fingerprintValid) {
                        Hash256 cachedHash{};
                        if (GetServerHashMemCache().TryGet(rel, fingerprint, cachedHash)) {
                            enqueueHigh(Frame{MsgType::HashResponse, 0, EncodeHashResponse(rel, cachedHash)});
                            hashCacheHits.fetch_add(1, std::memory_order_relaxed);
                            hashResponsesEnqueued.fetch_add(1, std::memory_order_relaxed);
                            continue;
                        }
                    }
                    {
                        std::lock_guard<std::mutex> lock(sessionHashMu);
                        ++sessionPendingHashJobs;
                    }
                    hashJobsEnqueued.fetch_add(1, std::memory_order_relaxed);
                    try {
                        GetServerHashPool().Enqueue([&, rel, abs, fingerprint, fingerprintValid]() {
                            Hash256 hash{};
                            bool hashOk = true;
                            try {
                                hash = ComputeFileHash(abs);
                            } catch (...) {
                                hashOk = false;
                                hash.fill(0xFF);
                            }
                            if (hashMemcacheEnabled && hashOk) {
                                // The memcache write is a best-effort optimisation. It MUST
                                // never be fatal: Upsert does cache_[rel]=... on an unbounded
                                // map, whose rehash can throw std::bad_alloc. This lambda runs
                                // on a global-pool worker that calls task() with no try/catch,
                                // so an escaping exception would std::terminate the whole
                                // server AND skip the sessionPendingHashJobs decrement below
                                // (wedging the 10-min teardown drain). Swallow any failure.
                                try {
                                    HashFingerprint afterFingerprint;
                                    if (TryReadHashFingerprint(abs, afterFingerprint) &&
                                        (!fingerprintValid ||
                                         (afterFingerprint.fileSize == fingerprint.fileSize &&
                                          afterFingerprint.mtimeNs == fingerprint.mtimeNs))) {
                                        GetServerHashMemCache().Upsert(rel, afterFingerprint, hash);
                                    }
                                } catch (...) {
                                    // Cache write failed (e.g. OOM); the hash response is still
                                    // sent below, so correctness is unaffected -- just no cache.
                                }
                            }
                            if (!done.load()) {
                                try {
                                    enqueueHigh(Frame{MsgType::HashResponse, 0, EncodeHashResponse(rel, hash)});
                                    hashResponsesEnqueued.fetch_add(1, std::memory_order_relaxed);
                                } catch (...) {
                                    failed.store(true);
                                    done.store(true);
                                    outboundCv.notify_all();
                                }
                            }
                            hashJobsCompleted.fetch_add(1, std::memory_order_relaxed);
                            {
                                std::lock_guard<std::mutex> lock(sessionHashMu);
                                if (sessionPendingHashJobs > 0) {
                                    --sessionPendingHashJobs;
                                }
                                // Notify WHILE holding sessionHashMu. This is the job's
                                // last access to session-local state; if it ran after the
                                // lock was released, the cleanup drain could observe
                                // pending==0, return, and destroy sessionHashCv before this
                                // notify executed (use-after-free under multi-client load).
                                sessionHashCv.notify_all();
                            }
                        });
                    } catch (...) {
                        {
                            std::lock_guard<std::mutex> lock(sessionHashMu);
                            if (sessionPendingHashJobs > 0) {
                                --sessionPendingHashJobs;
                            }
                            sessionHashCv.notify_all();
                        }
                        throw;
                    }
                } else if (frame.type == MsgType::FileOpen) {
                    const std::string rel = DecodeFileOpen(frame.payload);
                    const fs::path abs = JoinRel(options.rootDir, rel);
                    ServerStream st;
                    st.relativePath = rel;
                    st.input.open(abs, std::ios::binary);
                    if (!st.input) {
                        enqueueHigh(Frame{MsgType::FileError, frame.streamId, std::vector<uint8_t>(rel.begin(), rel.end())});
                        continue;
                    }
                    {
                        std::lock_guard<std::mutex> lock(mu);
                        pendingNewStreams.emplace_back(frame.streamId, std::move(st));
                    }
                    outboundCv.notify_one();
                } else if (frame.type == MsgType::FileBatchOpen) {
                    std::vector<std::string> relPaths = DecodeFileBatchRequest(frame.payload);
                    ServerBatchStream batch;
                    batch.files.reserve(relPaths.size());
                    for (const std::string& rel : relPaths) {
                        BatchFileRecord record;
                        record.relativePath = rel;
                        record.absPath = JoinRel(options.rootDir, rel);
                        std::error_code ec;
                        if (fs::exists(record.absPath, ec) && !ec &&
                            fs::is_regular_file(record.absPath, ec) && !ec) {
                            record.fileSize = static_cast<uint64_t>(fs::file_size(record.absPath, ec));
                            if (!ec) {
                                // Validate the file can actually be OPENED for read here,
                                // the same way the send loop will. A file that exists but
                                // is exclusively locked by another process (e.g. Unity's
                                // UnityLockfile) must be reported as not-ok now; otherwise
                                // the send loop's open would fail mid-stream and used to
                                // throw, killing the ENTIRE session (FR: a single
                                // unreadable file must never abort the whole sync).
                                std::ifstream probe(record.absPath, std::ios::binary);
                                if (probe) {
                                    // Same canonical mtime unit as the manifest path.
                                    record.mtimeNs = ReadFileMtimeCanonical(record.absPath);
                                    record.ok = true;
                                }
                            }
                        }
                        batch.files.push_back(std::move(record));
                    }
                    {
                        std::lock_guard<std::mutex> lock(mu);
                        pendingNewBatchStreams.emplace_back(frame.streamId, std::move(batch));
                    }
                    outboundCv.notify_one();
                } else if (frame.type == MsgType::SyncDone) {
                    done.store(true);
                    sessionHashCv.notify_all();
                    outboundCv.notify_all();
                } else {
                    std::ostringstream os;
                    os << "Unknown message in server session: type="
                       << static_cast<int>(static_cast<uint8_t>(frame.type)) << " ("
                       << MsgTypeName(static_cast<uint8_t>(frame.type)) << ") streamId="
                       << frame.streamId << " payloadLen=" << frame.payload.size() << " "
                       << DescribeRecentFrames();
                    throw std::runtime_error(os.str());
                }
            }
        } catch (const std::exception& ex) {
            failed.store(true);
            done.store(true);
            sessionHashCv.notify_all();
            outboundCv.notify_all();
            errorText = ex.what();
        }
    });

    // Diagnostics-only lock metrics (samples since last debug print), gated by debugEnabled.
    // Declared at function scope so the exception-window snapshot below (AC-B3) can summarise
    // the most recent lock-wait / critical-section samples after the main loop unwinds.
    std::vector<int64_t> muWaitUs;
    std::vector<int64_t> muHoldUs;
    try {
        auto lastDebugPrint = std::chrono::steady_clock::now();
        // Snapshots for per-interval enumeration rates/averages (see EnumStats).
        uint64_t lastEnumDirs = 0;
        uint64_t lastEnumFrames = 0;
        uint64_t lastEnumFlushes = 0;
        uint64_t lastEnumListingUsSum = 0;
        uint64_t lastEnumFlushBlockUsSum = 0;
        const size_t perStreamBurstBytes = (streamLimit <= 8)
                                               ? std::max<size_t>(2 * 1024 * 1024, static_cast<size_t>(effectiveChunkSize) * 2)
                                               : static_cast<size_t>(effectiveChunkSize);
        while (!done.load()) {
            bool didWork = false;
            std::vector<Frame> sendFrames;
            sendFrames.reserve(std::max<size_t>(256, static_cast<size_t>(streamLimit) * 8));
            // Critical section: drain outbound queues and adopt newly opened streams only.
            // No file I/O happens under `mu` (see lock-ordering note at declarations).
            {
                const auto muWaitStart = debugEnabled ? std::chrono::steady_clock::now()
                                                      : std::chrono::steady_clock::time_point{};
                std::unique_lock<std::mutex> lock(mu);
                const auto muAcquired = debugEnabled ? std::chrono::steady_clock::now()
                                                     : std::chrono::steady_clock::time_point{};
                size_t highBudget = 256;
                while (!outboundHigh.empty() && highBudget > 0) {
                    if (outboundHigh.front().type == MsgType::HashResponse) {
                        hashResponsesSent.fetch_add(1, std::memory_order_relaxed);
                    }
                    sendFrames.push_back(std::move(outboundHigh.front()));
                    outboundHigh.pop();
                    didWork = true;
                    --highBudget;
                }
                size_t manifestBudget = 0;
                if (!outboundManifest.empty()) {
                    // Keep manifest draining even under hash/file pressure so ManifestEnd is not starved.
                    manifestBudget = outboundHigh.empty() ? 16 : 4;
                    if (outboundManifest.size() > (maxQueuedManifestFrames / 2)) {
                        manifestBudget = outboundHigh.empty() ? 32 : 8;
                    }
                }
                while (!outboundManifest.empty() && manifestBudget > 0) {
                    sendFrames.push_back(std::move(outboundManifest.front()));
                    outboundManifest.pop();
                    outboundCv.notify_one();
                    didWork = true;
                    --manifestBudget;
                }
                // Adopt any newly opened streams into main-loop-private containers.
                for (auto& kv : pendingNewStreams) {
                    activeStreams.emplace(kv.first, std::move(kv.second));
                    didWork = true;
                }
                pendingNewStreams.clear();
                for (auto& kv : pendingNewBatchStreams) {
                    activeBatchStreams.emplace(kv.first, std::move(kv.second));
                    didWork = true;
                }
                pendingNewBatchStreams.clear();
                if (debugEnabled) {
                    muWaitUs.push_back(std::chrono::duration_cast<std::chrono::microseconds>(muAcquired - muWaitStart).count());
                    const auto muReleased = std::chrono::steady_clock::now();
                    muHoldUs.push_back(std::chrono::duration_cast<std::chrono::microseconds>(muReleased - muAcquired).count());
                }
            }
            // ---- Lock-free section: all file open()/read() runs without holding mu ----
            {
                const bool hasRegularStreams = !activeStreams.empty();
                const size_t batchSendQuotaBytes = (streamLimit <= 8) ? (24 * 1024 * 1024) : (12 * 1024 * 1024);
                size_t batchBytesSentThisRound = 0;
                for (auto it = activeBatchStreams.begin(); it != activeBatchStreams.end();) {
                    if (hasRegularStreams && batchBytesSentThisRound >= batchSendQuotaBytes) {
                        break;
                    }
                    ServerBatchStream& batch = it->second;
                    if (!batch.headerSent) {
                        sendFrames.push_back(Frame{MsgType::FileBatchOpen, it->first, EncodeFileBatchOpenResponse(batch.files)});
                        batch.headerSent = true;
                        didWork = true;
                    }

                    size_t burstBytes = 0;
                    bool batchAborted = false;
                    while (burstBytes < perStreamBurstBytes) {
                        while (batch.index < batch.files.size() && !batch.files[batch.index].ok) {
                            ++batch.index;
                        }
                        if (batch.index >= batch.files.size()) {
                            break;
                        }
                        BatchFileRecord& file = batch.files[batch.index];
                        if (!batch.input.is_open()) {
                            batch.input.open(file.absPath, std::ios::binary);
                            if (!batch.input) {
                                // Rare TOCTOU: the file was openable when the batch header
                                // was built but is now unreadable. Abort only THIS batch
                                // stream (the client fails its remaining entries) instead
                                // of throwing and tearing down the whole session.
                                sendFrames.push_back(Frame{MsgType::FileError, it->first, {}});
                                batchAborted = true;
                                break;
                            }
                            batch.remainingBytes = file.fileSize;
                            if (batch.remainingBytes == 0) {
                                batch.input.close();
                                ++batch.index;
                                continue;
                            }
                        }
                        const uint64_t toRead = std::min<uint64_t>(batch.remainingBytes, static_cast<uint64_t>(effectiveChunkSize));
                        std::vector<uint8_t> chunk(static_cast<size_t>(toRead));
                        batch.input.read(reinterpret_cast<char*>(chunk.data()), static_cast<std::streamsize>(chunk.size()));
                        const std::streamsize got = batch.input.gcount();
                        if (got <= 0) {
                            // I/O error mid-file: abort just this batch stream, not the session.
                            sendFrames.push_back(Frame{MsgType::FileError, it->first, {}});
                            batchAborted = true;
                            break;
                        }
                        chunk.resize(static_cast<size_t>(got));
                        burstBytes += static_cast<size_t>(got);
                        batchBytesSentThisRound += static_cast<size_t>(got);
                        batch.remainingBytes -= static_cast<uint64_t>(got);
                        sendFrames.push_back(Frame{MsgType::FileBatchChunk, it->first, std::move(chunk)});
                        didWork = true;
                        if (batch.remainingBytes == 0) {
                            batch.input.close();
                            ++batch.index;
                        }
                    }

                    if (batchAborted) {
                        if (batch.input.is_open()) {
                            batch.input.close();
                        }
                        it = activeBatchStreams.erase(it);
                        didWork = true;
                        continue;
                    }

                    bool batchDone = true;
                    for (size_t idx = batch.index; idx < batch.files.size(); ++idx) {
                        if (batch.files[idx].ok) {
                            batchDone = false;
                            break;
                        }
                    }
                    if (batchDone && !batch.input.is_open()) {
                        sendFrames.push_back(Frame{MsgType::FileBatchEnd, it->first, {}});
                        it = activeBatchStreams.erase(it);
                        didWork = true;
                    } else {
                        ++it;
                    }
                }

                for (auto it = activeStreams.begin(); it != activeStreams.end();) {
                    size_t burstBytes = 0;
                    bool streamClosed = false;
                    while (!streamClosed && burstBytes < perStreamBurstBytes) {
                        std::vector<uint8_t> chunk(effectiveChunkSize);
                        it->second.input.read(reinterpret_cast<char*>(chunk.data()), static_cast<std::streamsize>(chunk.size()));
                        const std::streamsize got = it->second.input.gcount();
                        if (got > 0) {
                            chunk.resize(static_cast<size_t>(got));
                            burstBytes += static_cast<size_t>(got);
                            sendFrames.push_back(Frame{MsgType::FileChunk, it->first, std::move(chunk)});
                            didWork = true;
                        }
                        if (!it->second.input || got == 0) {
                            sendFrames.push_back(Frame{MsgType::FileEnd, it->first, {}});
                            it = activeStreams.erase(it);
                            streamClosed = true;
                            didWork = true;
                        }
                    }
                    if (!streamClosed) {
                        ++it;
                    }
                }
            }
            if (!sendFrames.empty()) {
                SendFrameBatch(client, sendFrames);
            }
            if (debugEnabled) {
                const auto now = std::chrono::steady_clock::now();
                if ((now - lastDebugPrint) >= std::chrono::seconds(1)) {
                    size_t highQueued = 0;
                    size_t manifestQueued = 0;
                    size_t pendingAdopt = 0;
                    {
                        std::lock_guard<std::mutex> lock(mu);
                        highQueued = outboundHigh.size();
                        manifestQueued = outboundManifest.size();
                        pendingAdopt = pendingNewStreams.size() + pendingNewBatchStreams.size();
                    }
                    // active*Streams are main-loop private; safe to read without mu.
                    const size_t activeStreamCount = activeStreams.size();
                    const size_t activeBatchCount = activeBatchStreams.size();
                    size_t pendingHashes = 0;
                    {
                        std::lock_guard<std::mutex> lock(sessionHashMu);
                        pendingHashes = sessionPendingHashJobs;
                    }
                    const size_t globalPendingHashes = GetServerHashPool().PendingTasks();
                    const size_t globalActiveHashes = GetServerHashPool().ActiveTasks();
                    const size_t hashMemcacheEntries = GetServerHashMemCache().EntryCount();
                    const uint64_t hashMemcacheHits = GetServerHashMemCache().HitCount();
                    const uint64_t hashMemcacheMisses = GetServerHashMemCache().MissCount();
                    const uint64_t hashReqRecv = hashRequestsReceived.load(std::memory_order_relaxed);
                    const uint64_t hashHits = hashCacheHits.load(std::memory_order_relaxed);
                    const uint64_t hashJobsEnq = hashJobsEnqueued.load(std::memory_order_relaxed);
                    const uint64_t hashJobsDone = hashJobsCompleted.load(std::memory_order_relaxed);
                    const uint64_t hashRespEnq = hashResponsesEnqueued.load(std::memory_order_relaxed);
                    const uint64_t hashRespSent = hashResponsesSent.load(std::memory_order_relaxed);
                    auto pct = [](std::vector<int64_t>& v, double p) -> int64_t {
                        if (v.empty()) {
                            return 0;
                        }
                        std::sort(v.begin(), v.end());
                        size_t idx = static_cast<size_t>(p * static_cast<double>(v.size() - 1) + 0.5);
                        if (idx >= v.size()) {
                            idx = v.size() - 1;
                        }
                        return v[idx];
                    };
                    const int64_t muWaitP50 = pct(muWaitUs, 0.50);
                    const int64_t muWaitP95 = pct(muWaitUs, 0.95);
                    const int64_t muWaitP99 = pct(muWaitUs, 0.99);
                    const int64_t critHoldP50 = pct(muHoldUs, 0.50);
                    const int64_t critHoldP95 = pct(muHoldUs, 0.95);
                    const int64_t critHoldP99 = pct(muHoldUs, 0.99);
                    const uint64_t enumDirs = enumStats.dirsProcessed.load(std::memory_order_relaxed);
                    const uint64_t enumFrames = enumStats.framesFlushed.load(std::memory_order_relaxed);
                    const uint64_t enumFlushes = enumStats.flushCount.load(std::memory_order_relaxed);
                    const uint64_t enumListingUsSum = enumStats.listingUsSum.load(std::memory_order_relaxed);
                    const uint64_t enumFlushBlockUsSum = enumStats.flushBlockUsSum.load(std::memory_order_relaxed);
                    const uint64_t dDirs = enumDirs - lastEnumDirs;
                    const uint64_t dFrames = enumFrames - lastEnumFrames;
                    const uint64_t dFlushes = enumFlushes - lastEnumFlushes;
                    const uint64_t dListingUs = enumListingUsSum - lastEnumListingUsSum;
                    const uint64_t dFlushBlockUs = enumFlushBlockUsSum - lastEnumFlushBlockUsSum;
                    const uint64_t enumFramesPerFlush = dFlushes ? (dFrames / dFlushes) : 0;
                    const uint64_t enumListingUsAvg = dDirs ? (dListingUs / dDirs) : 0;
                    const uint64_t enumFlushBlockUsAvg = dFlushes ? (dFlushBlockUs / dFlushes) : 0;
                    lastEnumDirs = enumDirs;
                    lastEnumFrames = enumFrames;
                    lastEnumFlushes = enumFlushes;
                    lastEnumListingUsSum = enumListingUsSum;
                    lastEnumFlushBlockUsSum = enumFlushBlockUsSum;
                    std::cerr << "[debug][server][sid=" << sessionId << "] queued_high=" << highQueued
                              << " queued_manifest=" << manifestQueued
                              << " pending_adopt=" << pendingAdopt
                              << " pending_hash_jobs=" << pendingHashes
                              << " hash_req_recv=" << hashReqRecv
                              << " hash_cache_hits=" << hashHits
                              << " hash_jobs_enq=" << hashJobsEnq
                              << " hash_jobs_done=" << hashJobsDone
                              << " hash_jobs_inflight=" << (hashJobsEnq - hashJobsDone)
                              << " hash_resp_enq=" << hashRespEnq
                              << " hash_resp_sent=" << hashRespSent
                              << " hash_resp_backlog=" << (hashRespEnq - hashRespSent)
                              << " global_hash_queue=" << globalPendingHashes
                              << " global_hash_active=" << globalActiveHashes
                              << " hash_memcache=" << (hashMemcacheEnabled ? 1 : 0)
                              << " hash_memcache_entries=" << hashMemcacheEntries
                              << " hash_memcache_hits=" << hashMemcacheHits
                              << " hash_memcache_misses=" << hashMemcacheMisses
                              << " active_streams=" << activeStreamCount
                              << " active_batches=" << activeBatchCount
                              << " mu_wait_us_p50=" << muWaitP50
                              << " mu_wait_us_p95=" << muWaitP95
                              << " mu_wait_us_p99=" << muWaitP99
                              << " crit_hold_us_p50=" << critHoldP50
                              << " crit_hold_us_p95=" << critHoldP95
                              << " crit_hold_us_p99=" << critHoldP99
                              << " enum_dirs=" << enumDirs
                              << " enum_frames=" << enumFrames
                              << " enum_flushes=" << enumFlushes
                              << " enum_frames_per_flush=" << enumFramesPerFlush
                              << " enum_listing_us_avg=" << enumListingUsAvg
                              << " enum_listing_us_max=" << enumStats.listingUsMax.load(std::memory_order_relaxed)
                              << " enum_flush_block_us_avg=" << enumFlushBlockUsAvg
                              << " enum_flush_block_us_max=" << enumStats.flushBlockUsMax.load(std::memory_order_relaxed)
                              << std::endl;
                    muWaitUs.clear();
                    muHoldUs.clear();
                    lastDebugPrint = now;
                }
            }
            if (!didWork) {
                std::unique_lock<std::mutex> lock(mu);
                outboundCv.wait_for(lock, std::chrono::milliseconds(2), [&]() {
                    return done.load() || !outboundHigh.empty() || !outboundManifest.empty() ||
                           !pendingNewStreams.empty() || !pendingNewBatchStreams.empty();
                });
            }
        }
    } catch (const std::exception& ex) {
        failed.store(true);
        done.store(true);
        sessionHashCv.notify_all();
        outboundCv.notify_all();
        errorText = ex.what();
    }

    if (debugEnabled && failed.load()) {
        // Exception-window context snapshot for post-mortem (AC-B3 / design §B.4):
        // session id + lock-wait/critical-section stats + outbound & hash queues + in-flight tasks.
        size_t highQueued = 0, manifestQueued = 0, pendingAdopt = 0, pendingHashes = 0;
        {
            std::lock_guard<std::mutex> lock(mu);
            highQueued = outboundHigh.size();
            manifestQueued = outboundManifest.size();
            pendingAdopt = pendingNewStreams.size() + pendingNewBatchStreams.size();
        }
        {
            std::lock_guard<std::mutex> lock(sessionHashMu);
            pendingHashes = sessionPendingHashJobs;
        }
        const size_t globalPendingHashes = GetServerHashPool().PendingTasks();
        const size_t globalActiveHashes = GetServerHashPool().ActiveTasks();
        const size_t activeStreamCount = activeStreams.size();
        const size_t activeBatchCount = activeBatchStreams.size();
        // In-flight tasks: active transfer streams + batch streams + pending hash jobs.
        const size_t inFlight = activeStreamCount + activeBatchCount + pendingHashes;
        auto pct = [](std::vector<int64_t>& v, double p) -> int64_t {
            if (v.empty()) {
                return 0;
            }
            std::sort(v.begin(), v.end());
            size_t idx = static_cast<size_t>(p * static_cast<double>(v.size() - 1) + 0.5);
            if (idx >= v.size()) {
                idx = v.size() - 1;
            }
            return v[idx];
        };
        const int64_t muWaitP50 = pct(muWaitUs, 0.50);
        const int64_t muWaitP95 = pct(muWaitUs, 0.95);
        const int64_t muWaitP99 = pct(muWaitUs, 0.99);
        const int64_t critHoldP50 = pct(muHoldUs, 0.50);
        const int64_t critHoldP95 = pct(muHoldUs, 0.95);
        const int64_t critHoldP99 = pct(muHoldUs, 0.99);
        std::cerr << "[debug][server][sid=" << sessionId << "] session_failed=1"
                  << " error=\"" << errorText << "\""
                  << " queued_high=" << highQueued
                  << " queued_manifest=" << manifestQueued
                  << " pending_adopt=" << pendingAdopt
                  << " pending_hash_jobs=" << pendingHashes
                  << " hash_req_recv=" << hashRequestsReceived.load(std::memory_order_relaxed)
                  << " hash_cache_hits=" << hashCacheHits.load(std::memory_order_relaxed)
                  << " hash_jobs_enq=" << hashJobsEnqueued.load(std::memory_order_relaxed)
                  << " hash_jobs_done=" << hashJobsCompleted.load(std::memory_order_relaxed)
                  << " hash_resp_enq=" << hashResponsesEnqueued.load(std::memory_order_relaxed)
                  << " hash_resp_sent=" << hashResponsesSent.load(std::memory_order_relaxed)
                  << " global_hash_queue=" << globalPendingHashes
                  << " global_hash_active=" << globalActiveHashes
                  << " active_streams=" << activeStreamCount
                  << " active_batches=" << activeBatchCount
                  << " in_flight=" << inFlight
                  << " mu_wait_us_p50=" << muWaitP50
                  << " mu_wait_us_p95=" << muWaitP95
                  << " mu_wait_us_p99=" << muWaitP99
                  << " crit_hold_us_p50=" << critHoldP50
                  << " crit_hold_us_p95=" << critHoldP95
                  << " crit_hold_us_p99=" << critHoldP99
                  << std::endl;
    }

    // FR-05 / AC-B2: bound receiver teardown. If the main loop failed while the
    // receiver thread is blocked in RecvFrame, joining it could wait unbounded for
    // the peer. Shutting down the socket forces the blocking recv to return so the
    // receiver loop observes `done` and exits promptly (bounded convergence).
    if (failed.load()) {
        ShutdownBoth(client);
    }
    JoinDiag(receiver, "server-session-receiver");
    JoinDiag(manifestThread, "server-session-manifest");
    {
        // Bounded wait so a leaked/stuck hash job can never produce an unbounded hang
        // (FR-05 / AC-B2). On timeout we converge as a controlled failure.
        constexpr auto kHashDrainTimeout = std::chrono::minutes(10);
        std::unique_lock<std::mutex> lock(sessionHashMu);
        const bool drained = sessionHashCv.wait_for(lock, kHashDrainTimeout,
                                                    [&]() { return sessionPendingHashJobs == 0; });
        if (!drained) {
            failed.store(true);
            if (errorText.empty()) {
                errorText = "hash drain wait timed out after 10min (bounded-wait guard)";
            }
            if (debugEnabled) {
                std::cerr << "[debug][server][sid=" << sessionId << "] hash_drain_timeout=1"
                          << " pending_hash_jobs=" << sessionPendingHashJobs << std::endl;
            }
        }
    }
    if (failed.load()) {
        throw std::runtime_error("Server session failed: " + errorText);
    }
}

struct DownloadState {
    std::ofstream output;
    std::string relPath;
    std::vector<uint8_t> writeBuffer;
    size_t flushThreshold = 0;
};

struct BatchDownloadEntry {
    std::string relPath;
    uint64_t fileSize = 0;
    int64_t mtimeNs = 0;
    bool serverOk = false;
    bool shouldWrite = false;
    bool finalized = false;
    uint64_t received = 0;
    std::ofstream output;
    // Received bytes are buffered here and handed to the async I/O worker pool once
    // the whole file is in; the single main thread no longer writes file payloads to
    // disk inline (which previously serialised all transfer I/O and starved manifest
    // ingestion / compare).
    std::vector<uint8_t> buffer;
};

struct BatchDownloadState {
    bool headerReady = false;
    std::vector<BatchDownloadEntry> entries;
    size_t currentIndex = 0;
};

void PrintClientCounters(size_t enumerated,
                         size_t compared,
                         size_t unchanged,
                         size_t failed,
                         size_t transferred,
                         size_t deleted,
                         size_t& lastEnumerated,
                         size_t& lastCompared,
                         size_t& lastUnchanged,
                         size_t& lastFailed,
                         size_t& lastTransferred,
                         size_t& lastDeleted,
                         bool force = false) {
    using clock = std::chrono::steady_clock;
    static clock::time_point lastPrint = clock::now();
    const auto now = clock::now();
    const bool tickReached = (now - lastPrint) >= std::chrono::seconds(1);
    const bool changedEnough = (enumerated != lastEnumerated) ||
                               (compared != lastCompared) ||
                               (unchanged != lastUnchanged) ||
                               (failed != lastFailed) ||
                               (transferred != lastTransferred) ||
                               (deleted != lastDeleted);
    if (!force && (!tickReached || !changedEnough)) {
        return;
    }
    lastPrint = now;
    lastEnumerated = enumerated;
    lastCompared = compared;
    lastUnchanged = unchanged;
    lastFailed = failed;
    lastTransferred = transferred;
    lastDeleted = deleted;
    // CONSTRAINT-C1: each progress update occupies its own line (no '\r' overwrite),
    // so output stays consistent across Windows/Linux/macOS terminals and log files.
    std::cout << "Enumrated: " << enumerated
              << "  Compared: " << compared
              << "  Unchanged: " << unchanged
              << "  Failed: " << failed
              << "  Transfered: " << transferred
              << "  Deleted: " << deleted
              << std::endl;
}

}  // namespace

int RunServer(const CliOptions& options) {
    WsaContext wsa;
    const TunedTransferOptions tuned = ResolveTransferOptions(options);
    const uint32_t hashWorkerCount = ResolveServerHashWorkerCount(options);
    GetServerHashPool().Configure(hashWorkerCount);
    GetServerHashMemCache().Configure(options.enableHashMemcache);
    std::cout << "FastClone server root=" << options.rootDir.string() << " port=" << options.port << std::endl;
    std::cout << "[hash-pool] workers=" << hashWorkerCount
              << (options.serverHashWorkers == 0 ? " (auto)" : " (manual)")
              << std::endl;
    std::cout << "[hash-memcache] enabled=" << (options.enableHashMemcache ? 1 : 0) << std::endl;
    if (options.streamAutoTune || options.chunkAutoTune) {
        std::cout << "[auto-tune] streams=" << tuned.streamLimit
                  << " chunk-kb=" << (tuned.chunkSize / 1024)
                  << std::endl;
    }
    SocketHandle listener = CreateServer(options.port);
    std::atomic<uint64_t> sessionIdCounter{0};
    std::atomic<uint32_t> activeSessions{0};
    while (true) {
        std::cout << "Waiting for client... active_sessions=" << activeSessions.load() << std::endl;
        SocketHandle client = AcceptClient(listener);
        const uint64_t sessionId = sessionIdCounter.fetch_add(1) + 1;
        activeSessions.fetch_add(1);
        std::thread([sessionId, &activeSessions, options, client = std::move(client)]() mutable {
            try {
                std::cout << "Session#" << sessionId << " started" << std::endl;
                RunSessionServer(client, options);
                std::cout << "Session#" << sessionId << " completed" << std::endl;
            } catch (const std::exception& ex) {
                std::cerr << "Session#" << sessionId << " error: " << ex.what() << std::endl;
            }
            activeSessions.fetch_sub(1);
        }).detach();
    }
    return 0;
}

int RunClient(const CliOptions& options) {
    const auto syncStartTime = std::chrono::steady_clock::now();
    auto formatElapsed = [&]() -> std::string {
        const auto elapsed = std::chrono::steady_clock::now() - syncStartTime;
        long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
        const long long h = ms / 3600000; ms %= 3600000;
        const long long m = ms / 60000; ms %= 60000;
        const double s = static_cast<double>(ms) / 1000.0;
        std::ostringstream os;
        if (h > 0) {
            os << h << "h ";
        }
        if (h > 0 || m > 0) {
            os << m << "m ";
        }
        os << std::fixed << std::setprecision(1) << s << "s";
        return os.str();
    };
    WsaContext wsa;
    const bool debugEnabled = IsDebugEnabled();
    // --diag flag OR FASTCLONE_DIAG env var enables diagnostics (design §A.3).
    const bool diagnostics = options.diagnostics || IsDiagEnabled();
    const TunedTransferOptions tuned = ResolveTransferOptions(options);
    const uint32_t streamLimit = tuned.streamLimit;
    const uint32_t effectiveChunkSize = EffectiveChunkSizeForStreams(tuned.chunkSize, streamLimit);
    const size_t downloadFlushThreshold = DownloadFlushThresholdForStreams(streamLimit, effectiveChunkSize);
    std::error_code ec;
    fs::create_directories(options.rootDir, ec);

    std::optional<fs::path> selfPath = CurrentExePath();
    if (selfPath.has_value()) {
        std::error_code sec;
        const fs::path canonicalRoot = fs::weakly_canonical(options.rootDir, sec);
        if (sec) {
            selfPath = std::nullopt;
        } else {
            const fs::path canonicalSelf = fs::weakly_canonical(*selfPath, sec);
            if (sec || !IsPathUnderRoot(canonicalRoot, canonicalSelf)) {
                selfPath = std::nullopt;
            } else {
                selfPath = canonicalSelf;
            }
        }
    }

    // --- Session reconnect loop: cross-session vs per-session state ---
    // PERSIST across reconnect attempts (declared outside while):
    //   reconnectAttemptsUsed, reconnectWindowStart/Limit -- reconnect budget
    //   syncStartTime/formatElapsed -- total wall time for the CLI run
    //   tuned/streamLimit/effectiveChunkSize -- CLI transfer tuning (fixed at start)
    //   selfPath, options, diagnostics/debug flags -- run configuration
    //   WsaContext -- process-wide socket init
    // DO NOT add per-session maps/queues/counters here without resetting them inside while.
    // RESET every session (declared inside while, after ConnectTo succeeds):
    //   socket, remoteFiles/remoteDirs, all transfer/hash/compare queues & maps,
    //   activeDownloads/BatchDownloads, worker threads, recv thread, progress counters,
    //   manifestDone, recvError/recvClosed -- each session is a fresh FC5 handshake +
    //   ManifestRequest. Already-synced local files persist on DISK only; the next session
    //   re-enumerates and skips them via size+mtime compare (no in-memory carry-over).
    uint32_t reconnectAttemptsUsed = 0;
    const auto reconnectWindowStart = std::chrono::steady_clock::now();

    // ConnectTo + handshake failures (server not ready) reuse the same reconnect budget
    // as mid-session drops; see ScheduleClientReconnectOrExit().
    while (true) {
    SocketHandle socket;
    try {
        socket = ConnectTo(options.host, options.port);
        EnsureHandshakeAsClient(socket, options.password);
    } catch (const std::exception& ex) {
        const std::string connectReason = ex.what();
        if (const std::optional<int> exitCode = ScheduleClientReconnectOrExit(
                connectReason, options.reconnectRetries, options.reconnectWindowMs,
                reconnectAttemptsUsed, reconnectWindowStart, /*exitWhenDisabled=*/1)) {
            if (*exitCode == 1) {
                std::cerr << "FastClone error: " << connectReason << std::endl;
            }
            return *exitCode;
        }
        continue;
    }
    if (options.streamAutoTune || options.chunkAutoTune) {
        std::cout << "[auto-tune] streams=" << streamLimit
                  << " chunk-kb=" << (tuned.chunkSize / 1024)
                  << std::endl;
    }
    if (!options.streamAutoTune && streamLimit > 8) {
        std::cerr << "[warning] streams=" << streamLimit
                  << " may increase file transfer failure probability on unstable disks/controllers."
                  << std::endl;
    }

    SendFrame(socket, Frame{MsgType::ManifestRequest, 0, {}});
    std::unordered_map<std::string, FileEntry> remoteFiles;
    std::unordered_set<std::string> remoteDirs;
    std::unordered_map<uint32_t, DownloadState> activeDownloads;
    std::unordered_map<uint32_t, BatchDownloadState> activeBatchDownloads;
    std::unordered_map<uint32_t, std::string> streamToPath;
    std::deque<std::string> pendingTransfers;
    std::deque<std::string> pendingBatchTransfers;
    std::deque<std::string> pendingRetryTransfers;
    std::deque<std::string> pendingRetryBatchTransfers;
    std::unordered_set<std::string> scheduledTransfers;
    std::unordered_map<std::string, uint8_t> transferRetryCounts;

    std::unordered_map<std::string, Hash256> remoteHashes;
    std::unordered_map<std::string, Hash256> localHashes;
    std::unordered_set<std::string> hashResolved;
    std::unordered_set<std::string> hashRequested;
    std::deque<std::string> pendingHashRequests;
    std::unordered_set<std::string> localHashFailed;
    std::mutex fallbackReadyMu;
    std::deque<std::string> fallbackReadyQueue;
    struct CompareTask {
        FileEntry remote;
    };
    struct CompareResult {
        std::string relPath;
        CompareAction action = CompareAction::Skip;
    };
    std::mutex compareTaskMu;
    std::condition_variable compareTaskCv;
    std::deque<CompareTask> compareTasks;
    std::mutex compareResultMu;
    std::condition_variable compareResultCv;
    std::deque<CompareResult> compareResults;
    // Diagnostics-only: |local.mtimeNs - remote.mtimeNs| samples for size-equal files.
    std::mutex mtimeDeltaMu;
    std::vector<int64_t> mtimeDeltas;
    std::atomic<bool> compareStop = false;
    std::atomic<size_t> compareTasksIssued = 0;
    std::atomic<size_t> compareResultsHandled = 0;

    // Async file-write pool. Batch transfer payloads are buffered per file and handed
    // off here; workers do EnsureParentDir + open/write/close + SetFileModifyTime in
    // parallel and report success/failure back to the main thread (which owns all the
    // counters / retry bookkeeping, so no atomics are needed for those). This removes
    // the disk-I/O serialisation from the single main loop so it can keep draining the
    // manifest backlog and feeding the compare workers.
    struct IoWriteTask {
        std::string relPath;
        std::vector<uint8_t> data;
        int64_t mtimeNs = 0;
    };
    struct IoWriteResult {
        std::string relPath;
        bool ok = false;
    };
    std::mutex ioTaskMu;
    std::condition_variable ioTaskCv;
    std::deque<IoWriteTask> ioTasks;
    std::mutex ioResultMu;
    std::deque<IoWriteResult> ioResults;
    std::atomic<bool> ioStop = false;
    std::atomic<uint64_t> ioInFlightBytes = 0;
    // Main-thread only: number of files dispatched to the I/O pool whose result has not
    // yet been handled. Used as a completion-gate term so the sync does not finish while
    // writes are still pending.
    size_t ioOutstanding = 0;

    // Directory creation is offloaded to a small worker pool. With deep trees the
    // manifest can carry millions of directory entries; calling fs::create_directories
    // for each one inline on the single main loop serialised millions of filesystem
    // syscalls and was the dominant "wall" (the main thread could not drain manifest
    // frames while blocked in create_directories). Workers create them in parallel and
    // off the critical path; the main loop just records remoteDirs and hands off.
    std::mutex dirTaskMu;
    std::condition_variable dirTaskCv;
    std::deque<std::string> dirTasks;
    std::atomic<bool> dirStop = false;
    std::atomic<size_t> dirTasksIssued = 0;
    std::atomic<size_t> dirTasksDone = 0;

    std::mutex hashTaskMu;
    std::mutex hashResultMu;
    std::condition_variable hashTaskCv;
    std::deque<ClientHashTask> hashTaskQueue;
    std::atomic<bool> hashStop = false;
    std::atomic<size_t> localHashInFlight = 0;

    const uint32_t workerCount = std::max<uint32_t>(1, std::thread::hardware_concurrency());
    std::vector<std::thread> hashWorkers;
    hashWorkers.reserve(workerCount);
    for (uint32_t i = 0; i < workerCount; ++i) {
        hashWorkers.emplace_back([&]() {
            while (true) {
                ClientHashTask task;
                {
                    std::unique_lock<std::mutex> lock(hashTaskMu);
                    hashTaskCv.wait(lock, [&]() { return hashStop.load() || !hashTaskQueue.empty(); });
                    if (hashStop.load() && hashTaskQueue.empty()) {
                        return;
                    }
                    task = std::move(hashTaskQueue.front());
                    hashTaskQueue.pop_front();
                }
                localHashInFlight.fetch_add(1, std::memory_order_relaxed);
                try {
                    Hash256 hash = ComputeFileHash(task.absPath);
                    {
                        std::lock_guard<std::mutex> lock(hashResultMu);
                        localHashes[task.relPath] = hash;
                    }
                } catch (...) {
                    {
                        std::lock_guard<std::mutex> lock(hashResultMu);
                        localHashFailed.insert(task.relPath);
                    }
                }
                localHashInFlight.fetch_sub(1, std::memory_order_relaxed);
                {
                    std::lock_guard<std::mutex> lock(fallbackReadyMu);
                    fallbackReadyQueue.push_back(task.relPath);
                }
            }
        });
    }

    bool manifestDone = false;
    uint32_t nextStreamId = 1;
    std::deque<FileEntry> delayedCompareEntries;

    size_t enumerated = 0;
    size_t compared = 0;
    size_t unchanged = 0;
    size_t failed = 0;
    size_t transferred = 0;
    size_t deleted = 0;
    size_t fallbackCount = 0;
    size_t fallbackResolved = 0;
    size_t hashRequestsSent = 0;
    size_t hashResponsesReceived = 0;
    const size_t maxInFlightHashRequests = std::clamp<size_t>(
        std::max<size_t>(1024, static_cast<size_t>(streamLimit) * 256), 1024, 8192);
    constexpr uint8_t kMaxTransferRetries = 3;
    size_t lastEnum = 0;
    size_t lastCompared = 0;
    size_t lastUnchanged = 0;
    size_t lastFailed = 0;
    size_t lastTransferred = 0;
    size_t lastDeleted = 0;
    size_t reservedEntryCapacity = 0;
    using steady_clock = std::chrono::steady_clock;
    const auto stallWarnThreshold = std::chrono::minutes(1);
    auto lastForwardProgressAt = steady_clock::now();
    auto lastStallWarnAt = steady_clock::time_point{};
    size_t lastProgressEnumerated = 0;
    size_t lastProgressCompared = 0;
    size_t lastProgressUnchanged = 0;
    size_t lastProgressFailed = 0;
    size_t lastProgressTransferred = 0;
    size_t lastProgressFallbackResolved = 0;
    size_t lastProgressHashResponses = 0;
    size_t lastProgressCompareHandled = 0;
    size_t lastProgressDirsDone = 0;

    auto ensureEntryReserve = [&](size_t expectedEntries) {
        const size_t need = expectedEntries + (expectedEntries / 2) + 1024;
        if (need <= reservedEntryCapacity) {
            return;
        }
        // CRITICAL: grow capacity GEOMETRICALLY (doubling), not to the exact running
        // count. ensureEntryReserve() is called every ~2048 entries; reserving to a
        // target that creeps just past the current size made unordered_map::reserve()
        // REHASH every container (millions of elements x ~9 maps) every 2048 inserts,
        // i.e. O(N^2) overall -- this was the hard "wall" near 3M entries where
        // enumeration collapsed to a few thousand/sec. Doubling caps total rehashes at
        // O(log N) and makes amortised insertion O(1) again.
        size_t newCapacity = (reservedEntryCapacity == 0) ? size_t{65536} : reservedEntryCapacity;
        while (newCapacity < need) {
            newCapacity *= 2;
        }
        remoteFiles.reserve(newCapacity);
        remoteHashes.reserve(newCapacity);
        hashResolved.reserve(newCapacity);
        hashRequested.reserve(newCapacity);
        scheduledTransfers.reserve(newCapacity);
        transferRetryCounts.reserve(newCapacity);
        remoteDirs.reserve((newCapacity / 4) + 256);
        {
            // localHashes/localHashFailed are also updated by hash worker threads,
            // so reserve must hold the same mutex to avoid concurrent rehash UB.
            std::lock_guard<std::mutex> lock(hashResultMu);
            localHashes.reserve(newCapacity);
            localHashFailed.reserve(newCapacity);
        }
        reservedEntryCapacity = newCapacity;
    };

    std::mutex incomingMu;
    std::condition_variable incomingDataCv;
    std::deque<Frame> incomingPriorityFrames;
    std::deque<Frame> incomingManifestFrames;
    uint64_t incomingQueuedBytes = 0;
    const uint64_t incomingSoftLimitBytes = std::max<uint64_t>(256ULL * 1024ULL * 1024ULL, options.queuedFileSizeBytes);
    // Upper bound on bytes buffered in flight by the async write pool (received but not
    // yet written). This is a jitter-absorbing ceiling, NOT a steady-state working set:
    // when the SSD keeps up, the workers drain faster than data arrives and this is never
    // approached, so making it generous only helps ride out transient write stalls
    // (AV scans, dir-create hiccups) without leaving the transfer pipeline idle. Tied to
    // the incoming soft limit so a single knob scales both, with a 1 GiB floor.
    const uint64_t ioInFlightLimitBytes =
        std::max<uint64_t>(1024ULL * 1024ULL * 1024ULL, incomingSoftLimitBytes / 2);
    auto frameWireBytes = [](const Frame& f) -> uint64_t {
        return 9ULL + static_cast<uint64_t>(f.payload.size());
    };
    auto isManifestFrame = [](MsgType t) -> bool {
        return t == MsgType::ManifestEntry || t == MsgType::ManifestProgress || t == MsgType::ManifestEnd;
    };
    std::atomic<bool> recvStop = false;
    std::atomic<bool> recvClosed = false;
    std::string recvError;
    auto applyRecvBackpressure = [&](uint64_t queuedBytesSnapshot) {
        if (queuedBytesSnapshot <= incomingSoftLimitBytes) {
            return;
        }
        const uint64_t over = queuedBytesSnapshot - incomingSoftLimitBytes;
        uint64_t sleepUs = 300;
        if (over > incomingSoftLimitBytes * 2) {
            sleepUs = 5000;
        } else if (over > incomingSoftLimitBytes) {
            sleepUs = 3000;
        } else if (over > (incomingSoftLimitBytes / 2)) {
            sleepUs = 1500;
        } else if (over > (incomingSoftLimitBytes / 4)) {
            sleepUs = 700;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(sleepUs));
    };
    std::thread recvThread([&]() {
        // Manifest frames are tiny and arrive in a continuous flood; pushing each one
        // under incomingMu (with a notify) made the receiver monopolise the lock and
        // starve the single consumer (main loop), capping enumeration throughput.
        // Accumulate manifest frames locally and hand them off in batches under ONE
        // lock + one notify. Priority frames (file/hash traffic) are still flushed
        // immediately to keep transfer latency low.
        constexpr size_t kRecvManifestBatchFrames = 1024;
        constexpr uint64_t kRecvManifestBatchBytes = 4ULL * 1024ULL * 1024ULL;
        std::vector<Frame> manifestBatch;
        manifestBatch.reserve(kRecvManifestBatchFrames);
        uint64_t manifestBatchBytes = 0;
        try {
            auto flushManifestBatch = [&]() {
                if (manifestBatch.empty()) {
                    return;
                }
                uint64_t queuedBytesSnapshot = 0;
                {
                    std::lock_guard<std::mutex> lock(incomingMu);
                    for (Frame& mf : manifestBatch) {
                        incomingManifestFrames.push_back(std::move(mf));
                    }
                    incomingQueuedBytes += manifestBatchBytes;
                    queuedBytesSnapshot = incomingQueuedBytes;
                }
                incomingDataCv.notify_one();
                manifestBatch.clear();
                manifestBatchBytes = 0;
                applyRecvBackpressure(queuedBytesSnapshot);
            };

            while (!recvStop.load()) {
                Frame f = RecvFrame(socket);
                // Direction guard (desync diagnostics): the server must only ever send the
                // response/stream frame types below. Receiving anything else (e.g. a
                // client->server request type, a handshake type, or Error) means the byte
                // stream is misaligned or the peer is misbehaving. Detect it HERE, in wire
                // order, with the per-thread recent-frame history -- this is strictly more
                // informative than the later main-thread "unexpected frame" throw, because
                // the priority/manifest split reorders frames before the main loop sees them.
                switch (f.type) {
                    case MsgType::ManifestEntry:
                    case MsgType::ManifestProgress:
                    case MsgType::ManifestEnd:
                    case MsgType::HashResponse:
                    case MsgType::FileChunk:
                    case MsgType::FileEnd:
                    case MsgType::FileError:
                    case MsgType::FileBatchOpen:
                    case MsgType::FileBatchChunk:
                    case MsgType::FileBatchEnd:
                        break;  // legitimately server -> client
                    default: {
                        std::ostringstream os;
                        os << "client received wrong-direction/unknown frame: type="
                           << static_cast<int>(static_cast<uint8_t>(f.type)) << " ("
                           << MsgTypeName(static_cast<uint8_t>(f.type)) << ") streamId="
                           << f.streamId << " payloadLen=" << f.payload.size() << " "
                           << DescribeRecentFrames();
                        throw std::runtime_error(os.str());
                    }
                }
                if (isManifestFrame(f.type)) {
                    const bool forceFlush = (f.type == MsgType::ManifestEnd);
                    manifestBatchBytes += frameWireBytes(f);
                    manifestBatch.push_back(std::move(f));
                    if (forceFlush || manifestBatch.size() >= kRecvManifestBatchFrames ||
                        manifestBatchBytes >= kRecvManifestBatchBytes) {
                        flushManifestBatch();
                    }
                } else {
                    // Keep any buffered manifest frames ahead of nothing in particular,
                    // but flush them first so queue accounting stays monotonic, then
                    // push the priority frame immediately.
                    flushManifestBatch();
                    uint64_t queuedBytesSnapshot = 0;
                    {
                        std::lock_guard<std::mutex> lock(incomingMu);
                        incomingPriorityFrames.push_back(std::move(f));
                        incomingQueuedBytes += frameWireBytes(incomingPriorityFrames.back());
                        queuedBytesSnapshot = incomingQueuedBytes;
                    }
                    incomingDataCv.notify_one();
                    applyRecvBackpressure(queuedBytesSnapshot);
                }
            }
        } catch (const std::exception& ex) {
            if (!recvStop.load()) {
                recvError = ex.what();
                recvClosed.store(true);
                incomingDataCv.notify_all();
            }
        }
    });

    uint64_t smallFileBatchThreshold = 1920 * 1024;
    size_t smallBatchMaxFiles = 7680;
    size_t smallBatchMaxBytes = 320ULL * 1024ULL * 1024ULL;
    // Highest batch threshold for which the pending-regular queue has already been fully
    // promoted to batch. rebalanceTransfersTowardBatch only does its O(N) queue rescan
    // when the threshold has risen above this, instead of every single loop iteration.
    uint64_t rebalanceDoneThreshold = smallFileBatchThreshold;
    auto enqueueTransfer = [&](const std::string& rel, bool isRetry) {
        const auto it = remoteFiles.find(rel);
        const bool useBatch = (it != remoteFiles.end() && it->second.fileSize <= smallFileBatchThreshold);
        if (isRetry) {
            if (useBatch) {
                pendingRetryBatchTransfers.push_back(rel);
            } else {
                pendingRetryTransfers.push_back(rel);
            }
        } else {
            if (useBatch) {
                pendingBatchTransfers.push_back(rel);
            } else {
                pendingTransfers.push_back(rel);
            }
        }
    };

    auto scheduleTransfer = [&](const std::string& rel) {
        if (!scheduledTransfers.insert(rel).second) {
            return;
        }
        enqueueTransfer(rel, false);
    };

    auto markTransferFailed = [&](const std::string& rel) {
        ++compared;
        ++failed;
        if (debugEnabled) {
            std::cerr << "[debug][client] transfer_failed path=" << rel << std::endl;
        }
    };

    auto retryOrFail = [&](const std::string& rel) {
        uint8_t& retries = transferRetryCounts[rel];
        if (retries < kMaxTransferRetries) {
            ++retries;
            if (debugEnabled) {
                std::cerr << "[debug][client] transfer_retry path=" << rel
                          << " attempt=" << static_cast<uint32_t>(retries)
                          << "/" << static_cast<uint32_t>(kMaxTransferRetries)
                          << std::endl;
            }
            enqueueTransfer(rel, true);
        } else {
            markTransferFailed(rel);
            transferRetryCounts.erase(rel);
        }
    };

    auto refreshSmallBatchTuning = [&]() {
        const size_t backlogBatch = pendingBatchTransfers.size() + pendingRetryBatchTransfers.size();
        const size_t backlogRegular = pendingTransfers.size() + pendingRetryTransfers.size();
        const size_t backlog = backlogBatch + backlogRegular;
        uint64_t threshold = 1920 * 1024;
        size_t maxFiles = 7680;
        size_t maxBytes = 320ULL * 1024ULL * 1024ULL;

        if (backlog > 120000) {
            threshold = 5120 * 1024;
            maxFiles = 20480;
            maxBytes = 960ULL * 1024ULL * 1024ULL;
        } else if (backlog > 60000) {
            threshold = 3840 * 1024;
            maxFiles = 15360;
            maxBytes = 640ULL * 1024ULL * 1024ULL;
        } else if (backlog > 16000) {
            threshold = 2560 * 1024;
            maxFiles = 10240;
            maxBytes = 480ULL * 1024ULL * 1024ULL;
        } else if (backlog < 1000) {
            threshold = 1280 * 1024;
            maxFiles = 5120;
            maxBytes = 200ULL * 1024ULL * 1024ULL;
        }

        // Low stream mode needs larger per-batch payload to amortize per-file
        // transaction overhead (open/end/mtime) without raising stream count.
        if (streamLimit <= 8) {
            threshold = std::max<uint64_t>(threshold, 3200 * 1024);
            maxBytes = std::max<size_t>(maxBytes, 640ULL * 1024ULL * 1024ULL);
            maxFiles = std::max<size_t>(maxFiles, 12800);
        }

        smallFileBatchThreshold = threshold;
        smallBatchMaxFiles = maxFiles;
        smallBatchMaxBytes = maxBytes;
    };

    auto rebalanceTransfersTowardBatch = [&]() {
        // Reclassify part of regular queues into batch queues based on current
        // dynamic threshold. This fixes early queueing decisions when threshold
        // increases later under heavy backlog.
        auto moveEligible = [&](std::deque<std::string>& from, std::deque<std::string>& to, size_t budget) {
            if (from.empty() || budget == 0) {
                return size_t{0};
            }
            std::deque<std::string> remain;
            size_t moved = 0;
            while (!from.empty()) {
                std::string rel = std::move(from.front());
                from.pop_front();
                bool eligible = false;
                const auto it = remoteFiles.find(rel);
                if (it != remoteFiles.end()) {
                    eligible = (it->second.fileSize <= smallFileBatchThreshold);
                }
                if (eligible && moved < budget) {
                    to.push_back(std::move(rel));
                    ++moved;
                } else {
                    remain.push_back(std::move(rel));
                }
            }
            from.swap(remain);
            return moved;
        };

        // Promotion (regular -> batch) is only needed after the threshold INCREASES:
        // entries are classified with the current threshold at enqueue time, so a stable
        // (or shrinking) threshold leaves nothing to move. Skipping the rescan otherwise
        // avoids rebuilding the entire (here ~hundreds of thousands deep) pending queue on
        // every loop iteration, which was burning the single main thread's time and
        // starving manifest ingestion / compare.
        if (smallFileBatchThreshold <= rebalanceDoneThreshold) {
            return;
        }

        // Keep per-loop reclassification bounded.
        const size_t moveBudget = std::max<size_t>(512, smallBatchMaxFiles);
        const size_t moved = moveEligible(pendingTransfers, pendingBatchTransfers, moveBudget);
        const size_t retryBudgetBase = std::max<size_t>(moveBudget / 2, size_t{256});
        const size_t retryBudget = (moved >= retryBudgetBase) ? size_t{0} : (retryBudgetBase - moved);
        const size_t retryMoved = moveEligible(pendingRetryTransfers, pendingRetryBatchTransfers, retryBudget);
        // A pass that promotes nothing means the queues hold no more entries eligible at
        // the current threshold; mark this threshold done so we stop rescanning until it
        // rises again.
        if (moved == 0 && retryMoved == 0) {
            rebalanceDoneThreshold = smallFileBatchThreshold;
        }
    };

    auto probeLocalFile = [&](const std::string& relPath) -> std::optional<FileEntry> {
        const fs::path abs = JoinRel(options.rootDir, relPath);
#ifdef _WIN32
        // Compare hot path: collapse the previous 4 std::filesystem metadata calls
        // (exists/is_regular_file/file_size/last_write_time) into ONE syscall.
        // On huge trees the per-file metadata cost dominates compare; a single
        // GetFileAttributesExW avoids redundant path parsing and handle churn.
        WIN32_FILE_ATTRIBUTE_DATA data{};
        if (GetFileAttributesExW(abs.wstring().c_str(), GetFileExInfoStandard, &data) == 0) {
            return std::nullopt;  // missing/inaccessible -> treat as new (TransferNow)
        }
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            return std::nullopt;  // not a regular file
        }
        FileEntry entry;
        entry.relativePath = relPath;
        entry.isDirectory = false;
        entry.fileSize = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) |
                         static_cast<uint64_t>(data.nFileSizeLow);
        // Raw FILETIME ticks (100ns since 1601), identical unit to the manifest
        // writer (FileTimeToTicks). DecideCompareAction() normalizes both sides via
        // TryNormalizeMtimeToUnixNs before tolerance, so correctness is preserved.
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
        entry.relativePath = relPath;
        entry.isDirectory = false;
        entry.fileSize = static_cast<uint64_t>(fs::file_size(abs, ec));
        if (ec) {
            return std::nullopt;
        }
        // Compare hot path: use last_write_time directly to avoid per-file handle
        // open/close overhead from ReadFileMtimeCanonical on huge unchanged sets.
        // DecideCompareAction() already normalizes Unix-ns vs FILETIME ticks before
        // applying tolerance, so cross-platform correctness is preserved.
        entry.mtimeNs = ToUnixNs(fs::last_write_time(abs, ec));
        if (ec) {
            return std::nullopt;
        }
        return entry;
#endif
    };

    // Compare workers follow logical CPU concurrency (hardware_concurrency),
    // with a small safety floor for low-core machines.
    const uint32_t compareWorkerCount = std::max<uint32_t>(4, workerCount);
    // Keep a deep in-flight window so the compare workers do not run dry while the
    // single main thread is busy ingesting a burst of manifest frames between refills.
    const size_t maxInFlightCompareTasks = std::max<size_t>(8192, static_cast<size_t>(compareWorkerCount) * 256);
    constexpr size_t kCompareBatchPop = 32;
    std::vector<std::thread> compareWorkers;
    compareWorkers.reserve(compareWorkerCount);
    for (uint32_t i = 0; i < compareWorkerCount; ++i) {
        compareWorkers.emplace_back([&]() {
            std::vector<int64_t> localMtimeDeltas;
            std::vector<CompareTask> taskBatch;
            taskBatch.reserve(kCompareBatchPop);
            std::vector<CompareResult> resultBatch;
            resultBatch.reserve(kCompareBatchPop);
            while (true) {
                taskBatch.clear();
                {
                    std::unique_lock<std::mutex> lock(compareTaskMu);
                    compareTaskCv.wait(lock, [&]() { return compareStop.load() || !compareTasks.empty(); });
                    if (compareStop.load() && compareTasks.empty()) {
                        break;
                    }
                    const size_t take = std::min<size_t>(kCompareBatchPop, compareTasks.size());
                    for (size_t j = 0; j < take; ++j) {
                        taskBatch.push_back(std::move(compareTasks.front()));
                        compareTasks.pop_front();
                    }
                }
                resultBatch.clear();
                for (const CompareTask& task : taskBatch) {
                    CompareResult result;
                    result.relPath = task.remote.relativePath;
                    const std::optional<FileEntry> localProbe = probeLocalFile(task.remote.relativePath);
                    if (diagnostics && localProbe.has_value() && localProbe->fileSize == task.remote.fileSize) {
                        localMtimeDeltas.push_back(std::llabs(static_cast<long long>(localProbe->mtimeNs - task.remote.mtimeNs)));
                    }
                    result.action = DecideCompareAction(localProbe, task.remote);
                    resultBatch.push_back(std::move(result));
                }
                if (!resultBatch.empty()) {
                    {
                        std::lock_guard<std::mutex> lock(compareResultMu);
                        for (CompareResult& result : resultBatch) {
                            compareResults.push_back(std::move(result));
                        }
                    }
                    // Wake the main loop promptly so it can drain results and refill the
                    // worker queue; without this the loop only polls once per iteration
                    // and workers idle after exhausting the in-flight batch.
                    compareResultCv.notify_one();
                }
            }
            if (diagnostics && !localMtimeDeltas.empty()) {
                std::lock_guard<std::mutex> lock(mtimeDeltaMu);
                mtimeDeltas.insert(mtimeDeltas.end(), localMtimeDeltas.begin(), localMtimeDeltas.end());
            }
        });
    }

    // Directory-creation worker pool (see dirTasks declaration). Each worker pops a
    // batch and runs create_directories without holding any lock; concurrent creation
    // of overlapping paths is safe (idempotent, errors swallowed via error_code).
    // create_directories() is latency-bound metadata I/O (often further serialised by an
    // AV/filter driver intercepting each CreateDirectory), so the CPU/disk sit nearly
    // idle while threads block in the kernel. Like the enumeration walk, the lever is
    // CONCURRENCY, not batch size: oversubscribe past the core count to keep many
    // metadata ops in flight at once.
    const uint32_t dirWorkerCount = std::clamp<uint32_t>(workerCount * 2, 4u, 32u);
    constexpr size_t kDirBatchPop = 64;
    std::vector<std::thread> dirWorkers;
    dirWorkers.reserve(dirWorkerCount);
    for (uint32_t i = 0; i < dirWorkerCount; ++i) {
        dirWorkers.emplace_back([&]() {
            std::vector<std::string> batch;
            batch.reserve(kDirBatchPop);
            while (true) {
                batch.clear();
                {
                    std::unique_lock<std::mutex> lock(dirTaskMu);
                    dirTaskCv.wait(lock, [&]() { return dirStop.load() || !dirTasks.empty(); });
                    if (dirStop.load() && dirTasks.empty()) {
                        return;
                    }
                    const size_t take = std::min<size_t>(kDirBatchPop, dirTasks.size());
                    for (size_t j = 0; j < take; ++j) {
                        batch.push_back(std::move(dirTasks.front()));
                        dirTasks.pop_front();
                    }
                }
                for (const std::string& rel : batch) {
                    CreateDirectoriesLong(JoinRel(options.rootDir, rel));
                }
                dirTasksDone.fetch_add(batch.size(), std::memory_order_relaxed);
            }
        });
    }

    // Async file-write worker pool (see IoWriteTask declaration). I/O-bound, so size it
    // a bit above core count to keep the disk queue full of concurrent small-file
    // create/write/close/set-mtime operations.
    const uint32_t ioWorkerCount = std::clamp<uint32_t>(workerCount, 4u, 16u);
    constexpr size_t kIoBatchPop = 16;
    std::vector<std::thread> ioWorkers;
    ioWorkers.reserve(ioWorkerCount);
    for (uint32_t i = 0; i < ioWorkerCount; ++i) {
        ioWorkers.emplace_back([&]() {
            std::vector<IoWriteTask> batch;
            batch.reserve(kIoBatchPop);
            std::vector<IoWriteResult> results;
            results.reserve(kIoBatchPop);
            while (true) {
                batch.clear();
                {
                    std::unique_lock<std::mutex> lock(ioTaskMu);
                    ioTaskCv.wait(lock, [&]() { return ioStop.load() || !ioTasks.empty(); });
                    if (ioStop.load() && ioTasks.empty()) {
                        return;
                    }
                    const size_t take = std::min<size_t>(kIoBatchPop, ioTasks.size());
                    for (size_t j = 0; j < take; ++j) {
                        batch.push_back(std::move(ioTasks.front()));
                        ioTasks.pop_front();
                    }
                }
                results.clear();
                for (IoWriteTask& t : batch) {
                    bool ok = false;
                    const fs::path abs = JoinRel(options.rootDir, t.relPath);
                    EnsureParentDir(abs);
                    {
                        std::ofstream out(abs, std::ios::binary | std::ios::trunc);
                        if (out) {
                            if (!t.data.empty()) {
                                out.write(reinterpret_cast<const char*>(t.data.data()),
                                          static_cast<std::streamsize>(t.data.size()));
                            }
                            out.flush();
                            ok = out.good();
                            out.close();
                            if (ok && !out.good()) {
                                ok = false;
                            }
                        }
                    }
                    if (ok) {
                        SetFileModifyTime(abs, t.mtimeNs);
                    }
                    ioInFlightBytes.fetch_sub(t.data.size(), std::memory_order_relaxed);
                    results.push_back(IoWriteResult{std::move(t.relPath), ok});
                }
                {
                    std::lock_guard<std::mutex> lock(ioResultMu);
                    for (IoWriteResult& r : results) {
                        ioResults.push_back(std::move(r));
                    }
                }
            }
        });
    }

    auto handleCompareResult = [&](const CompareResult& r) {
        const CompareAction action = r.action;
        if (action == CompareAction::TransferNow) {
            scheduleTransfer(r.relPath);
        } else if (action == CompareAction::Skip) {
            ++compared;
            ++unchanged;
        } else {
            if (!hashRequested.contains(r.relPath)) {
                hashRequested.insert(r.relPath);
                ++fallbackCount;
                pendingHashRequests.push_back(r.relPath);
            }
        }
        PrintClientCounters(enumerated, compared, unchanged, failed, transferred, deleted, lastEnum, lastCompared, lastUnchanged, lastFailed, lastTransferred, lastDeleted);
    };

    auto dispatchHashRequests = [&]() {
        std::vector<Frame> outboundFrames;
        outboundFrames.reserve(256);
        while (!pendingHashRequests.empty() && (hashRequestsSent - hashResponsesReceived) < maxInFlightHashRequests) {
            const std::string rel = pendingHashRequests.front();
            pendingHashRequests.pop_front();
            ++hashRequestsSent;
            outboundFrames.push_back(Frame{MsgType::HashRequest, 0, EncodeHashRequest(rel)});
            {
                std::lock_guard<std::mutex> lock(hashTaskMu);
                hashTaskQueue.push_back(ClientHashTask{rel, JoinRel(options.rootDir, rel)});
            }
            hashTaskCv.notify_one();
            if (outboundFrames.size() >= 256) {
                SendFrameBatch(socket, outboundFrames);
                outboundFrames.clear();
            }
        }
        if (!outboundFrames.empty()) {
            SendFrameBatch(socket, outboundFrames);
        }
    };

    auto tryStartTransfers = [&]() {
        auto activeTransferSlots = [&]() -> size_t {
            return activeDownloads.size() + activeBatchDownloads.size();
        };
        auto hasBatchBacklog = [&]() -> bool {
            return !pendingBatchTransfers.empty() || !pendingRetryBatchTransfers.empty();
        };
        while (activeTransferSlots() < streamLimit) {
            // Backpressure: stop opening new transfers while the async write pool still
            // has a large backlog of buffered (received-but-unwritten) bytes, so memory
            // stays bounded. In-flight streams keep draining and the watermark recovers.
            if (ioInFlightBytes.load(std::memory_order_relaxed) > ioInFlightLimitBytes) {
                break;
            }
            bool started = false;
            std::deque<std::string>* batchQueue = nullptr;
            std::deque<std::string>* regularQueue = nullptr;
            if (!pendingBatchTransfers.empty()) {
                batchQueue = &pendingBatchTransfers;
            } else if (!pendingTransfers.empty()) {
                regularQueue = &pendingTransfers;
            } else if (!pendingRetryBatchTransfers.empty()) {
                batchQueue = &pendingRetryBatchTransfers;
            } else if (!pendingRetryTransfers.empty()) {
                regularQueue = &pendingRetryTransfers;
            }

            if (batchQueue != nullptr) {
                std::vector<std::string> batchPaths;
                batchPaths.reserve(smallBatchMaxFiles);
                size_t batchBytes = 0;
                while (!batchQueue->empty() && batchPaths.size() < smallBatchMaxFiles && batchBytes < smallBatchMaxBytes) {
                    const std::string rel = batchQueue->front();
                    batchQueue->pop_front();
                    batchPaths.push_back(rel);
                    const auto it = remoteFiles.find(rel);
                    if (it != remoteFiles.end()) {
                        batchBytes += static_cast<size_t>(it->second.fileSize);
                    }
                    if (batchBytes >= smallBatchMaxBytes) {
                        break;
                    }
                }
                if (!batchPaths.empty()) {
                    const uint32_t sid = nextStreamId++;
                    BatchDownloadState batchState;
                    activeBatchDownloads.emplace(sid, std::move(batchState));
                    SendFrame(socket, Frame{MsgType::FileBatchOpen, sid, EncodeFileBatchRequest(batchPaths)});
                    started = true;
                }
            } else if (regularQueue != nullptr) {
                // Reserve one transfer slot for batch work whenever batch backlog exists.
                // This prevents regular streams from starving batch streams under low stream limits.
                const size_t reservedBatchSlots = (streamLimit > 1 && hasBatchBacklog()) ? 1 : 0;
                const size_t regularLimit = static_cast<size_t>(streamLimit) - reservedBatchSlots;
                if (activeDownloads.size() >= regularLimit) {
                    break;
                }
                const std::string rel = regularQueue->front();
                regularQueue->pop_front();
                const fs::path abs = JoinRel(options.rootDir, rel);
                EnsureParentDir(abs);
                DownloadState d;
                d.relPath = rel;
                d.output.open(abs, std::ios::binary | std::ios::trunc);
                if (!d.output) {
                    retryOrFail(rel);
                    PrintClientCounters(enumerated, compared, unchanged, failed, transferred, deleted, lastEnum, lastCompared, lastUnchanged, lastFailed, lastTransferred, lastDeleted);
                } else {
                    const uint32_t sid = nextStreamId++;
                    d.flushThreshold = downloadFlushThreshold;
                    d.writeBuffer.reserve(d.flushThreshold);
                    activeDownloads.emplace(sid, std::move(d));
                    streamToPath.emplace(sid, rel);
                    SendFrame(socket, Frame{MsgType::FileOpen, sid, EncodeFileOpen(rel)});
                }
                started = true;
            }
            if (!started) {
                break;
            }
        }
    };

    auto flushBufferedWrites = [&](DownloadState& d) {
        if (d.writeBuffer.empty()) {
            return;
        }
        d.output.write(reinterpret_cast<const char*>(d.writeBuffer.data()), static_cast<std::streamsize>(d.writeBuffer.size()));
        d.writeBuffer.clear();
    };

    auto finalizeBatchEntry = [&](BatchDownloadEntry& entry) {
        if (entry.finalized) {
            return;
        }
        if (entry.shouldWrite) {
            if (entry.output.is_open()) {
                entry.output.flush();
                entry.output.close();
            }
            SetFileModifyTime(JoinRel(options.rootDir, entry.relPath), entry.mtimeNs);
            ++compared;
            ++transferred;
            transferRetryCounts.erase(entry.relPath);
        } else {
            if (entry.output.is_open()) {
                entry.output.close();
            }
            if (!entry.serverOk) {
                // Server explicitly reported this entry as unavailable; retrying won't help.
                markTransferFailed(entry.relPath);
                transferRetryCounts.erase(entry.relPath);
            } else {
                retryOrFail(entry.relPath);
            }
        }
        entry.finalized = true;
        PrintClientCounters(enumerated, compared, unchanged, failed, transferred, deleted, lastEnum, lastCompared, lastUnchanged, lastFailed, lastTransferred, lastDeleted);
    };

    // Hand a fully-received file off to the async I/O pool instead of writing it on the
    // main thread. The counters are bumped later, on the main thread, when the worker's
    // result is drained (see the ioResults handling in the main loop).
    auto dispatchBatchWrite = [&](BatchDownloadEntry& entry) {
        IoWriteTask task;
        task.relPath = entry.relPath;
        task.mtimeNs = entry.mtimeNs;
        task.data = std::move(entry.buffer);
        ioInFlightBytes.fetch_add(task.data.size(), std::memory_order_relaxed);
        ++ioOutstanding;
        {
            std::lock_guard<std::mutex> lock(ioTaskMu);
            ioTasks.push_back(std::move(task));
        }
        ioTaskCv.notify_one();
        entry.finalized = true;
    };

    // Complete a batch entry: data-bearing successful writes go to the async pool; empty
    // files and failures keep the original synchronous finalize path (cheap, no bulk I/O).
    auto completeBatchEntry = [&](BatchDownloadEntry& entry) {
        if (entry.finalized) {
            return;
        }
        if (entry.shouldWrite && entry.fileSize > 0) {
            dispatchBatchWrite(entry);
        } else {
            finalizeBatchEntry(entry);
        }
    };

    auto resolveFallbackIfReady = [&]() {
        std::deque<std::string> readyCandidates;
        {
            std::lock_guard<std::mutex> lock(fallbackReadyMu);
            readyCandidates.swap(fallbackReadyQueue);
        }
        for (const std::string& rel : readyCandidates) {
            if (hashResolved.contains(rel)) {
                continue;
            }
            Hash256 localHash{};
            Hash256 remoteHash{};
            bool remoteHashReady = false;
            bool localHashReady = false;
            bool localFailed = false;
            {
                std::lock_guard<std::mutex> lock(hashResultMu);
                remoteHashReady = remoteHashes.contains(rel);
                if (remoteHashReady) {
                    remoteHash = remoteHashes.at(rel);
                }
                localHashReady = localHashes.contains(rel);
                localFailed = localHashFailed.contains(rel);
                if (localHashReady) {
                    localHash = localHashes.at(rel);
                }
            }
            if (!remoteHashReady || (!localHashReady && !localFailed)) {
                continue;
            }
            if (localFailed || !localHashReady || !HashEquals(localHash, remoteHash)) {
                scheduleTransfer(rel);
            } else {
                const FileEntry& meta = remoteFiles.at(rel);
                SetFileModifyTime(JoinRel(options.rootDir, rel), meta.mtimeNs);
                ++compared;
                ++unchanged;
            }
            hashResolved.insert(rel);
            ++fallbackResolved;
            PrintClientCounters(enumerated, compared, unchanged, failed, transferred, deleted, lastEnum, lastCompared, lastUnchanged, lastFailed, lastTransferred, lastDeleted);
        }
    };

    auto sweepUnresolvedFallbackIfQuiescent = [&]() {
        if (fallbackResolved >= fallbackCount) {
            return;
        }
        if (!pendingHashRequests.empty()) {
            return;
        }
        if (hashRequestsSent != hashResponsesReceived) {
            return;
        }
        if (localHashInFlight.load(std::memory_order_relaxed) != 0) {
            return;
        }
        std::vector<std::string> unresolved;
        unresolved.reserve(hashRequested.size());
        for (const std::string& rel : hashRequested) {
            if (!hashResolved.contains(rel)) {
                unresolved.push_back(rel);
            }
        }
        if (unresolved.empty()) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(fallbackReadyMu);
            for (const std::string& rel : unresolved) {
                fallbackReadyQueue.push_back(rel);
            }
        }
        resolveFallbackIfReady();

        std::vector<std::string> stillUnresolved;
        stillUnresolved.reserve(unresolved.size());
        for (const std::string& rel : unresolved) {
            if (!hashResolved.contains(rel)) {
                stillUnresolved.push_back(rel);
            }
        }
        if (stillUnresolved.empty()) {
            return;
        }

        for (const std::string& rel : stillUnresolved) {
            if (hashResolved.contains(rel)) {
                continue;
            }
            // Fail-safe: if fallback cannot make progress in a fully quiescent state,
            // force transfer to guarantee forward progress and avoid infinite stall.
            scheduleTransfer(rel);
            hashResolved.insert(rel);
            ++fallbackResolved;
            std::cerr << "[warn][client] fallback_force_transfer path=" << rel << std::endl;
            PrintClientCounters(enumerated, compared, unchanged, failed, transferred, deleted,
                                lastEnum, lastCompared, lastUnchanged, lastFailed, lastTransferred, lastDeleted);
        }
    };

    // Batch hand-off buffer: compare tasks accumulated while processing one drained
    // batch of manifest frames are flushed to the worker queue under a SINGLE lock +
    // one notify_all, instead of one lock+notify per file. This removes the
    // producer/consumer lock convoy on compareTaskMu that throttled enumeration
    // throughput to a few tens of K/s while the CPU sat idle (all threads parked).
    std::vector<CompareTask> compareDispatchBuffer;
    auto enqueueCompareTask = [&](const FileEntry& e) {
        compareDispatchBuffer.push_back(CompareTask{e});
        // Count as issued immediately so in-flight gating stays accurate even before
        // the buffer is flushed into compareTasks.
        ++compareTasksIssued;
    };
    auto flushCompareDispatch = [&]() {
        if (compareDispatchBuffer.empty()) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(compareTaskMu);
            for (CompareTask& t : compareDispatchBuffer) {
                compareTasks.push_back(std::move(t));
            }
        }
        compareTaskCv.notify_all();
        compareDispatchBuffer.clear();
    };

    auto processIncomingFrame = [&](Frame& frame) {
        if (frame.type == MsgType::ManifestEntry) {
            FileEntry e = DecodeManifestEntry(frame.payload);
            if (e.isDirectory) {
                // Only record the directory during streaming. Physical creation is
                // deferred to the end and limited to file-less ("empty subtree")
                // directories: any directory that contains files is either already
                // present (unchanged files) or created by EnsureParentDir during
                // transfer, so creating every manifest directory here is almost
                // entirely wasted syscalls on a re-sync (and previously throttled
                // enumeration via directory backpressure).
                remoteDirs.insert(e.relativePath);
                return;
            }
            remoteFiles[e.relativePath] = e;
            ++enumerated;
            if ((enumerated % 2048) == 0) {
                ensureEntryReserve(enumerated + 2048);
            }
            if ((compareTasksIssued.load() - compareResultsHandled.load()) >= maxInFlightCompareTasks) {
                delayedCompareEntries.push_back(e);
            } else {
                enqueueCompareTask(e);
            }
            // NOTE: progress is printed once per drained batch in the main loop, not
            // per entry, to keep the manifest hot path free of clock() calls.
        } else if (frame.type == MsgType::ManifestProgress) {
            size_t cursor = 0;
            const uint64_t serverEnumerated = ReadU64(frame.payload, cursor);
            ensureEntryReserve(static_cast<size_t>(serverEnumerated));
            if (serverEnumerated > enumerated) {
                enumerated = static_cast<size_t>(serverEnumerated);
            }
        } else if (frame.type == MsgType::ManifestEnd) {
            manifestDone = true;
        } else if (frame.type == MsgType::HashResponse) {
            auto value = DecodeHashResponse(frame.payload);
            {
                std::lock_guard<std::mutex> lock(hashResultMu);
                remoteHashes[value.first] = value.second;
            }
            {
                std::lock_guard<std::mutex> lock(fallbackReadyMu);
                fallbackReadyQueue.push_back(value.first);
            }
            ++hashResponsesReceived;
        } else if (frame.type == MsgType::FileBatchOpen) {
            auto itBatch = activeBatchDownloads.find(frame.streamId);
            if (itBatch == activeBatchDownloads.end()) {
                throw std::runtime_error("Received batch open for unknown stream");
            }
            BatchDownloadState& batch = itBatch->second;
            batch.entries.clear();
            batch.currentIndex = 0;
            std::vector<BatchFileRecord> response = DecodeFileBatchOpenResponse(frame.payload);
            batch.entries.reserve(response.size());
            for (auto& rec : response) {
                BatchDownloadEntry entry;
                entry.relPath = rec.relativePath;
                entry.fileSize = rec.fileSize;
                entry.mtimeNs = rec.mtimeNs;
                entry.serverOk = rec.ok;
                entry.shouldWrite = entry.serverOk;
                if (entry.serverOk && entry.fileSize == 0) {
                    const fs::path abs = JoinRel(options.rootDir, entry.relPath);
                    EnsureParentDir(abs);
                    std::ofstream out(abs, std::ios::binary | std::ios::trunc);
                    entry.shouldWrite = out.good();
                }
                if (!entry.serverOk || (entry.fileSize == 0 && entry.serverOk)) {
                    finalizeBatchEntry(entry);
                }
                batch.entries.push_back(std::move(entry));
            }
            while (batch.currentIndex < batch.entries.size() &&
                   (batch.entries[batch.currentIndex].finalized || batch.entries[batch.currentIndex].fileSize == 0)) {
                ++batch.currentIndex;
            }
            batch.headerReady = true;
        } else if (frame.type == MsgType::FileBatchChunk) {
            auto itBatch = activeBatchDownloads.find(frame.streamId);
            if (itBatch == activeBatchDownloads.end()) {
                throw std::runtime_error("Received batch chunk for unknown stream");
            }
            BatchDownloadState& batch = itBatch->second;
            if (!batch.headerReady) {
                throw std::runtime_error("Received batch chunk before batch open");
            }
            size_t offset = 0;
            while (offset < frame.payload.size()) {
                while (batch.currentIndex < batch.entries.size() &&
                       (batch.entries[batch.currentIndex].finalized || batch.entries[batch.currentIndex].fileSize == 0 ||
                        batch.entries[batch.currentIndex].received >= batch.entries[batch.currentIndex].fileSize)) {
                    if (batch.currentIndex < batch.entries.size() &&
                        !batch.entries[batch.currentIndex].finalized &&
                        batch.entries[batch.currentIndex].received >= batch.entries[batch.currentIndex].fileSize) {
                        completeBatchEntry(batch.entries[batch.currentIndex]);
                    }
                    ++batch.currentIndex;
                }
                if (batch.currentIndex >= batch.entries.size()) {
                    break;
                }
                BatchDownloadEntry& entry = batch.entries[batch.currentIndex];
                const uint64_t remainingForEntry = entry.fileSize - entry.received;
                const size_t available = frame.payload.size() - offset;
                const size_t take = static_cast<size_t>(std::min<uint64_t>(remainingForEntry, static_cast<uint64_t>(available)));
                // Buffer the bytes in memory; the actual disk write happens off-thread in
                // the I/O pool once the file is complete (see dispatchBatchWrite).
                if (entry.shouldWrite) {
                    if (entry.buffer.capacity() == 0 && entry.fileSize > 0) {
                        entry.buffer.reserve(static_cast<size_t>(entry.fileSize));
                    }
                    entry.buffer.insert(entry.buffer.end(),
                                        frame.payload.data() + offset,
                                        frame.payload.data() + offset + take);
                }
                entry.received += take;
                offset += take;
                if (entry.received >= entry.fileSize) {
                    completeBatchEntry(entry);
                    ++batch.currentIndex;
                }
            }
        } else if (frame.type == MsgType::FileBatchEnd) {
            auto itBatch = activeBatchDownloads.find(frame.streamId);
            if (itBatch == activeBatchDownloads.end()) {
                throw std::runtime_error("Received batch end for unknown stream");
            }
            BatchDownloadState& batch = itBatch->second;
            for (auto& entry : batch.entries) {
                if (!entry.finalized) {
                    if (entry.received < entry.fileSize) {
                        entry.shouldWrite = false;
                    }
                    // completeBatchEntry routes complete writes through the async pool and
                    // incomplete/failed ones through the synchronous fail path.
                    completeBatchEntry(entry);
                }
            }
            activeBatchDownloads.erase(itBatch);
        } else if (frame.type == MsgType::FileChunk) {
            auto it = activeDownloads.find(frame.streamId);
            if (it == activeDownloads.end()) {
                throw std::runtime_error("Received chunk for unknown stream");
            }
            DownloadState& d = it->second;
            d.writeBuffer.insert(d.writeBuffer.end(), frame.payload.begin(), frame.payload.end());
            if (d.writeBuffer.size() >= d.flushThreshold) {
                flushBufferedWrites(d);
            }
        } else if (frame.type == MsgType::FileEnd) {
            auto it = activeDownloads.find(frame.streamId);
            if (it == activeDownloads.end()) {
                throw std::runtime_error("Received end for unknown stream");
            }
            flushBufferedWrites(it->second);
            it->second.output.flush();
            it->second.output.close();
            const std::string rel = it->second.relPath;
            const FileEntry& meta = remoteFiles.at(rel);
            SetFileModifyTime(JoinRel(options.rootDir, rel), meta.mtimeNs);
            activeDownloads.erase(it);
            streamToPath.erase(frame.streamId);
            ++compared;
            ++transferred;
            transferRetryCounts.erase(rel);
            PrintClientCounters(enumerated, compared, unchanged, failed, transferred, deleted, lastEnum, lastCompared, lastUnchanged, lastFailed, lastTransferred, lastDeleted);
        } else if (frame.type == MsgType::FileError) {
            auto itBatch = activeBatchDownloads.find(frame.streamId);
            if (itBatch != activeBatchDownloads.end()) {
                for (auto& entry : itBatch->second.entries) {
                    if (!entry.finalized) {
                        entry.shouldWrite = false;
                        finalizeBatchEntry(entry);
                    }
                }
                activeBatchDownloads.erase(itBatch);
                return;
            }
            auto itPath = streamToPath.find(frame.streamId);
            auto itDl = activeDownloads.find(frame.streamId);
            if (itDl != activeDownloads.end()) {
                itDl->second.writeBuffer.clear();
                itDl->second.output.close();
                activeDownloads.erase(itDl);
            }
            std::string relPath;
            bool hasRelPath = false;
            if (itPath != streamToPath.end()) {
                relPath = itPath->second;
                hasRelPath = true;
                streamToPath.erase(itPath);
            }
            if (hasRelPath) {
                retryOrFail(relPath);
            } else {
                ++compared;
                ++failed;
                if (debugEnabled) {
                    std::cerr << "[debug][client] transfer_failed path=<unknown> stream=" << frame.streamId << std::endl;
                }
            }
            PrintClientCounters(enumerated, compared, unchanged, failed, transferred, deleted, lastEnum, lastCompared, lastUnchanged, lastFailed, lastTransferred, lastDeleted);
        } else {
            {
                std::ostringstream os;
                os << "Unexpected frame in client stream loop: type="
                   << static_cast<int>(static_cast<uint8_t>(frame.type)) << " ("
                   << MsgTypeName(static_cast<uint8_t>(frame.type)) << ") streamId="
                   << frame.streamId << " payloadLen=" << frame.payload.size();
                throw std::runtime_error(os.str());
            }
        }
    };

    auto updateStallWatchdog = [&](bool loopHadForwardProgress) {
        const size_t compareHandledNow = compareResultsHandled.load();
        const size_t dirsDoneNow = dirTasksDone.load(std::memory_order_relaxed);
        const bool countersAdvanced = loopHadForwardProgress ||
                                      (enumerated != lastProgressEnumerated) ||
                                      (compared != lastProgressCompared) ||
                                      (unchanged != lastProgressUnchanged) ||
                                      (failed != lastProgressFailed) ||
                                      (transferred != lastProgressTransferred) ||
                                      (fallbackResolved != lastProgressFallbackResolved) ||
                                      (hashResponsesReceived != lastProgressHashResponses) ||
                                      (compareHandledNow != lastProgressCompareHandled) ||
                                      (dirsDoneNow != lastProgressDirsDone);
        const auto now = steady_clock::now();
        if (countersAdvanced) {
            lastForwardProgressAt = now;
            lastProgressEnumerated = enumerated;
            lastProgressCompared = compared;
            lastProgressUnchanged = unchanged;
            lastProgressFailed = failed;
            lastProgressTransferred = transferred;
            lastProgressFallbackResolved = fallbackResolved;
            lastProgressHashResponses = hashResponsesReceived;
            lastProgressCompareHandled = compareHandledNow;
            lastProgressDirsDone = dirsDoneNow;
            lastStallWarnAt = steady_clock::time_point{};
            return;
        }
        const bool enoughToWarn = (now - lastForwardProgressAt) >= stallWarnThreshold;
        const bool warnCooldownPassed =
            (lastStallWarnAt == steady_clock::time_point{}) || ((now - lastStallWarnAt) >= stallWarnThreshold);
        if (!enoughToWarn || !warnCooldownPassed) {
            return;
        }
        size_t queuedIncomingPriorityFrames = 0;
        size_t queuedIncomingManifestFrames = 0;
        uint64_t queuedIncomingBytes = 0;
        {
            std::lock_guard<std::mutex> lock(incomingMu);
            queuedIncomingPriorityFrames = incomingPriorityFrames.size();
            queuedIncomingManifestFrames = incomingManifestFrames.size();
            queuedIncomingBytes = incomingQueuedBytes;
        }
        size_t queuedCompareTasks = 0;
        {
            std::lock_guard<std::mutex> lock(compareTaskMu);
            queuedCompareTasks = compareTasks.size();
        }
        size_t queuedHashTasks = 0;
        {
            std::lock_guard<std::mutex> lock(hashTaskMu);
            queuedHashTasks = hashTaskQueue.size();
        }
        const size_t hashLocalInflight = localHashInFlight.load(std::memory_order_relaxed);
        const size_t compareInflight = compareTasksIssued.load() - compareResultsHandled.load();
        const size_t hashInflight = hashRequestsSent - hashResponsesReceived;
        const size_t pendingTransfersTotal = pendingTransfers.size() + pendingBatchTransfers.size() +
                                             pendingRetryTransfers.size() + pendingRetryBatchTransfers.size();
        std::string stallClass;
        auto addStallTag = [&](const char* tag) {
            if (!stallClass.empty()) {
                stallClass.push_back('|');
            }
            stallClass += tag;
        };
        const size_t fallbackOpen = fallbackCount - fallbackResolved;
        if (pendingTransfersTotal > 0 || !activeDownloads.empty() || !activeBatchDownloads.empty()) {
            addStallTag("transfer_backlog");
        }
        if (compareInflight > 0 || queuedCompareTasks > 0 || !delayedCompareEntries.empty()) {
            addStallTag("compare_backlog");
        }
        if (fallbackOpen > 0 || hashInflight > 0 || queuedHashTasks > 0 || hashLocalInflight > 0 || !pendingHashRequests.empty()) {
            addStallTag("hash_wait");
        }
        if (hashLocalInflight > 0) {
            addStallTag("hash_local_compute");
        }
        if (!manifestDone || queuedIncomingManifestFrames > 0) {
            addStallTag("manifest_wait");
        }
        const bool waitingForNetwork = (!manifestDone) || !activeDownloads.empty() || !activeBatchDownloads.empty() ||
                                       (hashResponsesReceived < hashRequestsSent);
        const bool incomingEmpty = (queuedIncomingPriorityFrames == 0 && queuedIncomingManifestFrames == 0);
        if (waitingForNetwork && incomingEmpty) {
            addStallTag("network_idle");
        }
        if (stallClass.empty()) {
            addStallTag("unknown");
        }
        std::cerr << "[warn][client] suspected_stall stall_s="
                  << std::chrono::duration_cast<std::chrono::seconds>(now - lastForwardProgressAt).count()
                  << " stall_class=" << stallClass
                  << " enum=" << enumerated
                  << " compared=" << compared
                  << " unchanged=" << unchanged
                  << " failed=" << failed
                  << " transferred=" << transferred
                  << " manifest_done=" << (manifestDone ? 1 : 0)
                  << " compare_inflight=" << compareInflight
                  << " compare_queued=" << queuedCompareTasks
                  << " hash_inflight=" << hashInflight
                  << " hash_local_queued=" << queuedHashTasks
                  << " hash_local_inflight=" << hashLocalInflight
                  << " hash_pending_req=" << pendingHashRequests.size()
                  << " fallback_open=" << fallbackOpen
                  << " pending_transfers=" << pendingTransfersTotal
                  << " active_downloads=" << activeDownloads.size()
                  << " active_batches=" << activeBatchDownloads.size()
                  << " queued_incoming_prio=" << queuedIncomingPriorityFrames
                  << " queued_incoming_manifest=" << queuedIncomingManifestFrames
                  << " queued_incoming_mb=" << (queuedIncomingBytes / (1024ULL * 1024ULL))
                  << std::endl;
        lastStallWarnAt = now;
    };

    // Compare-buffer backpressure (hysteresis). The single main thread time-shares
    // between ingesting manifest (-> delayedCompareEntries) and servicing compare
    // (drain results + refill workers). Letting ingestion run flat out buries the
    // thread, starves the workers, and balloons delayedCompareEntries in RAM. So once
    // the buffer is comfortably deep we pause manifest ingestion and hand the thread
    // to compare until the buffer drains below the low-water mark.
    const size_t kDelayedHighWater = 512 * 1024;
    const size_t kDelayedLowWater = 256 * 1024;
    bool ingestPaused = false;

    try {
        auto lastDebugPrint = std::chrono::steady_clock::now();
        while (true) {
            bool loopHadForwardProgress = false;
            resolveFallbackIfReady();
            dispatchHashRequests();
            refreshSmallBatchTuning();
            rebalanceTransfersTowardBatch();
            tryStartTransfers();
            {
                std::deque<CompareResult> ready;
                {
                    std::lock_guard<std::mutex> lock(compareResultMu);
                    ready.swap(compareResults);
                }
                for (const auto& r : ready) {
                    handleCompareResult(r);
                    ++compareResultsHandled;
                }
                if (!ready.empty()) {
                    loopHadForwardProgress = true;
                }
            }
            {
                // Drain results from the async file-write pool. Counters and retry
                // bookkeeping stay on the main thread, so no atomics are needed for them.
                std::deque<IoWriteResult> ioDone;
                {
                    std::lock_guard<std::mutex> lock(ioResultMu);
                    ioDone.swap(ioResults);
                }
                for (auto& r : ioDone) {
                    --ioOutstanding;
                    if (r.ok) {
                        ++compared;
                        ++transferred;
                        transferRetryCounts.erase(r.relPath);
                    } else {
                        retryOrFail(r.relPath);
                    }
                    PrintClientCounters(enumerated, compared, unchanged, failed, transferred, deleted,
                                        lastEnum, lastCompared, lastUnchanged, lastFailed, lastTransferred, lastDeleted);
                }
                if (!ioDone.empty()) {
                    loopHadForwardProgress = true;
                }
            }
            while (!delayedCompareEntries.empty() &&
                   (compareTasksIssued.load() - compareResultsHandled.load()) < maxInFlightCompareTasks) {
                FileEntry e = std::move(delayedCompareEntries.front());
                delayedCompareEntries.pop_front();
                enqueueCompareTask(e);
            }
            flushCompareDispatch();
            dispatchHashRequests();

            if (debugEnabled) {
                const auto now = std::chrono::steady_clock::now();
                if ((now - lastDebugPrint) >= std::chrono::seconds(1)) {
                    size_t readyCompareResults = 0;
                    {
                        std::lock_guard<std::mutex> lock(compareResultMu);
                        readyCompareResults = compareResults.size();
                    }
                    size_t queuedCompareTasks = 0;
                    {
                        std::lock_guard<std::mutex> lock(compareTaskMu);
                        queuedCompareTasks = compareTasks.size();
                    }
                    size_t queuedIncomingFrames = 0;
                    size_t queuedIncomingPriorityFrames = 0;
                    size_t queuedIncomingManifestFrames = 0;
                    uint64_t queuedIncomingBytes = 0;
                    {
                        std::lock_guard<std::mutex> lock(incomingMu);
                        queuedIncomingPriorityFrames = incomingPriorityFrames.size();
                        queuedIncomingManifestFrames = incomingManifestFrames.size();
                        queuedIncomingFrames = queuedIncomingPriorityFrames + queuedIncomingManifestFrames;
                        queuedIncomingBytes = incomingQueuedBytes;
                    }
                    size_t queuedHashTasks = 0;
                    {
                        std::lock_guard<std::mutex> lock(hashTaskMu);
                        queuedHashTasks = hashTaskQueue.size();
                    }
                    const size_t compareInflight = compareTasksIssued.load() - compareResultsHandled.load();
                    const size_t hashInflight = hashRequestsSent - hashResponsesReceived;
                    const size_t hashLocalInflight = localHashInFlight.load(std::memory_order_relaxed);
                    std::cerr << "[debug][client] enum=" << enumerated
                              << " compared=" << compared
                              << " unchanged=" << unchanged
                              << " failed=" << failed
                              << " transferred=" << transferred
                              << " in_flight_hash=" << hashInflight
                              << " pending_hash_req=" << pendingHashRequests.size()
                              << " pending_hash_local=" << queuedHashTasks
                              << " in_flight_hash_local=" << hashLocalInflight
                              << " in_flight_compare=" << compareInflight
                              << " queued_compare_tasks=" << queuedCompareTasks
                              << " ready_compare_results=" << readyCompareResults
                              << " delayed_compare_entries=" << delayedCompareEntries.size()
                              << " dirs_created=" << dirTasksDone.load(std::memory_order_relaxed)
                              << " dirs_queued=" << (dirTasksIssued.load(std::memory_order_relaxed) -
                                                     dirTasksDone.load(std::memory_order_relaxed))
                              << " queued_incoming_frames=" << queuedIncomingFrames
                              << " queued_incoming_prio=" << queuedIncomingPriorityFrames
                              << " queued_incoming_manifest=" << queuedIncomingManifestFrames
                              << " queued_incoming_mb=" << (queuedIncomingBytes / (1024ULL * 1024ULL))
                              << " queued_limit_mb=" << (incomingSoftLimitBytes / (1024ULL * 1024ULL))
                              << " pending_transfers=" << (pendingTransfers.size() + pendingBatchTransfers.size() +
                                                           pendingRetryTransfers.size() + pendingRetryBatchTransfers.size())
                              << " active_downloads=" << activeDownloads.size()
                              << " active_batches=" << activeBatchDownloads.size()
                              << " batch_thr_kb=" << (smallFileBatchThreshold / 1024)
                              << " batch_max_files=" << smallBatchMaxFiles
                              << " batch_max_kb=" << (smallBatchMaxBytes / 1024)
                              << " fallback_open=" << (fallbackCount - fallbackResolved)
                              << std::endl;
                    lastDebugPrint = now;
                }
            }

            const bool allHashDone = (fallbackResolved == fallbackCount);
            const bool allCompareDone = (compareResultsHandled.load() == compareTasksIssued.load());
            if (manifestDone && allCompareDone &&
                pendingTransfers.empty() && pendingBatchTransfers.empty() &&
                pendingRetryTransfers.empty() && pendingRetryBatchTransfers.empty() &&
                activeDownloads.empty() && activeBatchDownloads.empty() && allHashDone &&
                ioOutstanding == 0) {
                break;
            }
            const bool needNetworkFrame = !manifestDone || !activeDownloads.empty() || !activeBatchDownloads.empty() ||
                                          (hashResponsesReceived < hashRequestsSent);
            sweepUnresolvedFallbackIfQuiescent();
            if (!needNetworkFrame) {
                updateStallWatchdog(loopHadForwardProgress);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            // Update the ingest pause state (hysteresis) from the current compare
            // buffer depth before deciding how much manifest to pull this iteration.
            const size_t delayedBacklog = delayedCompareEntries.size();
            if (ingestPaused) {
                if (delayedBacklog <= kDelayedLowWater) {
                    ingestPaused = false;
                }
            } else if (delayedBacklog >= kDelayedHighWater) {
                ingestPaused = true;
            }

            std::deque<Frame> readyFrames;
            {
                std::lock_guard<std::mutex> lock(incomingMu);
                size_t kDrainBudget = 512;
                if (incomingQueuedBytes > incomingSoftLimitBytes * 3) {
                    kDrainBudget = 4096;
                } else if (incomingQueuedBytes > incomingSoftLimitBytes * 2) {
                    kDrainBudget = 3072;
                } else if (incomingQueuedBytes > incomingSoftLimitBytes) {
                    kDrainBudget = 2048;
                } else if (incomingQueuedBytes > (incomingSoftLimitBytes / 2)) {
                    kDrainBudget = 1024;
                }
                // Manifest frames are tiny, so the byte thresholds above badly
                // under-count a huge frame backlog. Scale the budget by queued FRAME
                // COUNT too, but keep the per-iteration manifest chunk MODERATE so the
                // loop cycles back to service compare (drain results + refill workers)
                // frequently instead of burying the thread in one giant decode burst.
                const size_t queuedFrames = incomingPriorityFrames.size() + incomingManifestFrames.size();
                size_t frameBudget = 512;
                if (queuedFrames > 50000) {
                    frameBudget = 8192;
                } else if (queuedFrames > 8000) {
                    frameBudget = 4096;
                } else if (queuedFrames > 2000) {
                    frameBudget = 2048;
                }
                kDrainBudget = std::max<size_t>(kDrainBudget, frameBudget);
                const size_t kPriorityBudget = (kDrainBudget * 3) / 4;
                const size_t prioCount = std::min<size_t>(incomingPriorityFrames.size(), kPriorityBudget);
                for (size_t i = 0; i < prioCount; ++i) {
                    Frame frame = std::move(incomingPriorityFrames.front());
                    incomingPriorityFrames.pop_front();
                    const uint64_t sz = frameWireBytes(frame);
                    incomingQueuedBytes = (incomingQueuedBytes >= sz) ? (incomingQueuedBytes - sz) : 0;
                    readyFrames.push_back(std::move(frame));
                }
                // While ingest is paused, only priority frames (file/hash) are pulled;
                // manifest stays buffered so the thread can drain the compare backlog.
                if (!ingestPaused) {
                    const size_t remaining = kDrainBudget - readyFrames.size();
                    const size_t manifestCount = std::min<size_t>(incomingManifestFrames.size(), remaining);
                    for (size_t i = 0; i < manifestCount; ++i) {
                        Frame frame = std::move(incomingManifestFrames.front());
                        incomingManifestFrames.pop_front();
                        const uint64_t sz = frameWireBytes(frame);
                        incomingQueuedBytes = (incomingQueuedBytes >= sz) ? (incomingQueuedBytes - sz) : 0;
                        readyFrames.push_back(std::move(frame));
                    }
                }
            }
            if (readyFrames.empty()) {
                // Two distinct idle reasons:
                //  (a) compare work still in flight (or ingest deliberately paused to
                //      drain the buffer): wait briefly on compare results so we refill
                //      the workers promptly instead of sleeping on incoming frames.
                //  (b) genuinely nothing to do: wait on incoming frames.
                const bool compareWorkPending =
                    (compareTasksIssued.load() != compareResultsHandled.load()) || !delayedCompareEntries.empty();
                if (ingestPaused || compareWorkPending) {
                    {
                        std::unique_lock<std::mutex> lock(compareResultMu);
                        compareResultCv.wait_for(lock, std::chrono::milliseconds(1), [&]() {
                            return !compareResults.empty();
                        });
                    }
                    // updateStallWatchdog() takes its own locks (incomingMu/compareTaskMu/
                    // hashTaskMu) on the warn path, so it MUST run with no loop mutex held.
                    updateStallWatchdog(loopHadForwardProgress);
                    continue;
                }
                bool recvDrainedAndClosed = false;
                {
                    std::unique_lock<std::mutex> lock(incomingMu);
                    incomingDataCv.wait_for(lock, std::chrono::milliseconds(2), [&]() {
                        return recvClosed.load() || !incomingPriorityFrames.empty() || !incomingManifestFrames.empty();
                    });
                    recvDrainedAndClosed = recvClosed.load() && incomingPriorityFrames.empty() &&
                                           incomingManifestFrames.empty();
                }
                if (recvDrainedAndClosed) {
                    break;
                }
                // MUST be outside the incomingMu scope above: updateStallWatchdog()'s warn
                // path re-acquires incomingMu, and re-locking a std::mutex already held by
                // this thread throws std::system_error{resource_deadlock_would_occur} on
                // MSVC (errc 36). That was the "resource deadlock would occur" crash hit
                // after ~1 min of a network stall, the only window where this branch runs
                // every iteration and the watchdog finally crosses its warn threshold.
                updateStallWatchdog(loopHadForwardProgress);
                continue;
            }
            for (auto& frame : readyFrames) {
                processIncomingFrame(frame);
            }
            // Single lock + notify_all for the whole drained batch (see comment at
            // compareDispatchBuffer): replaces the former per-entry lock/notify.
            flushCompareDispatch();
            if (!readyFrames.empty()) {
                loopHadForwardProgress = true;
                // Manifest hot path no longer prints per entry; emit one throttled
                // progress line per drained batch instead.
                PrintClientCounters(enumerated, compared, unchanged, failed, transferred, deleted,
                                    lastEnum, lastCompared, lastUnchanged, lastFailed, lastTransferred, lastDeleted);
            }
            updateStallWatchdog(loopHadForwardProgress);
        }
    } catch (...) {
        recvStop.store(true);
        incomingDataCv.notify_all();
        ShutdownBoth(socket);
        compareStop.store(true);
        compareTaskCv.notify_all();
        hashStop.store(true);
        hashTaskCv.notify_all();
        dirStop.store(true);
        dirTaskCv.notify_all();
        ioStop.store(true);
        ioTaskCv.notify_all();
        JoinDiag(recvThread, "client-catch-recv");
        for (auto& w : compareWorkers) {
            JoinDiag(w, "client-catch-compare");
        }
        for (auto& w : hashWorkers) {
            JoinDiag(w, "client-catch-hash");
        }
        for (auto& w : dirWorkers) {
            JoinDiag(w, "client-catch-dir");
        }
        for (auto& w : ioWorkers) {
            JoinDiag(w, "client-catch-io");
        }
        throw;
    }

    compareStop.store(true);
    compareTaskCv.notify_all();
    hashStop.store(true);
    hashTaskCv.notify_all();
    ioStop.store(true);
    ioTaskCv.notify_all();
    for (auto& w : compareWorkers) {
        JoinDiag(w, "client-compare");
    }
    for (auto& w : hashWorkers) {
        JoinDiag(w, "client-hash");
    }
    for (auto& w : ioWorkers) {
        JoinDiag(w, "client-io");
    }

    // Manifest completeness gate (used both for the deferred directory creation below
    // and the deletion guard further down). A partial manifest (dropped connection /
    // no ManifestEnd) must neither create nor delete against an incomplete remote view.
    const bool manifestComplete = manifestDone && recvError.empty() && !recvClosed.load();

    if (diagnostics) {
        int64_t mtimeDeltaP50 = 0, mtimeDeltaP95 = 0, mtimeDeltaP99 = 0, mtimeDeltaMax = 0;
        if (!mtimeDeltas.empty()) {
            std::sort(mtimeDeltas.begin(), mtimeDeltas.end());
            const size_t n = mtimeDeltas.size();
            auto percentile = [&](double p) -> int64_t {
                size_t idx = static_cast<size_t>(p * static_cast<double>(n - 1) + 0.5);
                if (idx >= n) {
                    idx = n - 1;
                }
                return mtimeDeltas[idx];
            };
            mtimeDeltaP50 = percentile(0.50);
            mtimeDeltaP95 = percentile(0.95);
            mtimeDeltaP99 = percentile(0.99);
            mtimeDeltaMax = mtimeDeltas.back();
        }
        // AC-A3 requires these exact field names to be directly readable in the log.
        // snake_case aliases are kept for backward compatibility with existing tooling.
        std::cout << "[diag] fallbackCount=" << fallbackCount
                  << " fallbackResolved=" << fallbackResolved
                  << " hashRequestsSent=" << hashRequestsSent
                  << " hashResponsesReceived=" << hashResponsesReceived
                  << " mtime_delta_samples=" << mtimeDeltas.size()
                  << " mtime_delta_p50=" << mtimeDeltaP50
                  << " mtime_delta_p95=" << mtimeDeltaP95
                  << " mtime_delta_p99=" << mtimeDeltaP99
                  << " mtime_delta_max=" << mtimeDeltaMax
                  << " fallback_count=" << fallbackCount
                  << " fallback_resolved=" << fallbackResolved
                  << " hash_req_sent=" << hashRequestsSent
                  << " hash_resp_recv=" << hashResponsesReceived
                  << std::endl;
    }

    // SAFETY: only delete local "extras" if the manifest was received in full (see
    // manifestComplete above). If the loop exited because the connection dropped
    // (recvClosed/recvError) or ManifestEnd was never seen, remoteFiles/remoteDirs hold
    // only a PARTIAL view of the server, and deleting against it would wipe out every
    // local file the server hadn't sent yet. Abort instead of destroying data.
    if (!manifestComplete) {
        std::cerr << "[error][client] manifest incomplete (manifest_end="
                  << (manifestDone ? 1 : 0) << " recv_closed=" << (recvClosed.load() ? 1 : 0)
                  << " recv_error=\"" << recvError << "\" enumerated=" << enumerated
                  << "); SKIPPING deletion to avoid destroying local files not yet received."
                  << std::endl;
        dirStop.store(true);
        dirTaskCv.notify_all();
        for (auto& w : dirWorkers) {
            JoinDiag(w, "client-dir-abort");
        }
        recvStop.store(true);
        incomingDataCv.notify_all();
        ShutdownBoth(socket);
        JoinDiag(recvThread, "client-recv-incomplete");
        std::cout << "Sync aborted (incomplete manifest). changed_files=" << transferred
                  << " failed_files=" << failed << " enumerated=" << enumerated
                  << " elapsed=" << formatElapsed() << std::endl;

        const std::string disconnectReason =
            recvError.empty() ? "connection_closed" : recvError;
        if (const std::optional<int> exitCode = ScheduleClientReconnectOrExit(
                disconnectReason, options.reconnectRetries, options.reconnectWindowMs,
                reconnectAttemptsUsed, reconnectWindowStart, /*exitWhenDisabled=*/3)) {
            return *exitCode;
        }
        continue;
    }

    // Delete obsolete files first; the walk also hands back the set of directories that
    // already exist locally, which lets the empty-directory creation below skip the (on a
    // re-sync, ALL) directories that are already present instead of issuing a useless,
    // filter-driver-throttled create_directories() per directory.
    std::unordered_set<std::string> existingLocalDirs;
    std::cout << "Deleting obsoleted files (sync with server)..." << std::endl;
    const RemoveLocalExtrasResult deleteResult =
        RemoveLocalExtras(options.rootDir, remoteDirs, remoteFiles, selfPath, existingLocalDirs);
    deleted = deleteResult.deletedFiles;
    failed += deleteResult.failedOps;
    compared += deleteResult.failedOps;
    compared += deleted;
    std::cout << "Delete done, " << deleted << " files" << std::endl;

    // Create ONLY the empty-subtree remote directories that are actually MISSING locally.
    // (Directories that hold files already exist or are made by EnsureParentDir during
    // transfer; directories that already exist locally are skipped via existingLocalDirs.)
    {
        std::unordered_set<std::string> dirsWithFiles;
        dirsWithFiles.reserve(remoteDirs.size());
        for (const auto& kv : remoteFiles) {
            const std::string& filePath = kv.first;
            size_t slash = filePath.rfind('/');
            while (slash != std::string::npos) {
                std::string dir = filePath.substr(0, slash);
                if (dir.empty()) {
                    break;
                }
                if (!dirsWithFiles.insert(std::move(dir)).second) {
                    break;
                }
                slash = filePath.rfind('/', slash - 1);
            }
        }
        std::vector<std::string> emptyDirs;
        for (const std::string& d : remoteDirs) {
            if (!dirsWithFiles.contains(d) && !existingLocalDirs.contains(d)) {
                emptyDirs.push_back(d);
            }
        }
        // create_directories() builds ancestors implicitly, so issue only the deepest of
        // any nested chain: after a lexicographic sort, a dir that is a path-prefix of the
        // next entry is its ancestor and can be skipped.
        std::sort(emptyDirs.begin(), emptyDirs.end());
        {
            std::lock_guard<std::mutex> lock(dirTaskMu);
            for (size_t i = 0; i < emptyDirs.size(); ++i) {
                const std::string& d = emptyDirs[i];
                if (i + 1 < emptyDirs.size()) {
                    const std::string& next = emptyDirs[i + 1];
                    if (next.size() > d.size() && next.compare(0, d.size(), d) == 0 &&
                        next[d.size()] == '/') {
                        continue;
                    }
                }
                dirTasks.push_back(d);
                dirTasksIssued.fetch_add(1, std::memory_order_relaxed);
            }
        }
        const size_t emptyDirCount = dirTasksIssued.load(std::memory_order_relaxed);
        dirTaskCv.notify_all();
        if (emptyDirCount > 0) {
            std::cout << "Creating " << emptyDirCount << " empty directories..." << std::endl;
            while (dirTasksDone.load(std::memory_order_relaxed) < emptyDirCount) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }
    }
    dirStop.store(true);
    dirTaskCv.notify_all();
    for (auto& w : dirWorkers) {
        JoinDiag(w, "client-dir");
    }
    PrintClientCounters(enumerated, compared, unchanged, failed, transferred, deleted, lastEnum, lastCompared, lastUnchanged, lastFailed, lastTransferred, lastDeleted, true);
    SendFrame(socket, Frame{MsgType::SyncDone, 0, {}});
    recvStop.store(true);
    incomingDataCv.notify_all();
    ShutdownBoth(socket);
    JoinDiag(recvThread, "client-recv-final");
    const bool success = (failed == 0);
    std::cout << "Sync completed. changed_files=" << transferred
              << " failed_files=" << failed
              << " elapsed=" << formatElapsed() << std::endl;
    return success ? 0 : 2;
    }  // while (reconnect session)
}

}  // namespace fc
