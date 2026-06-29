#include "sync_engine.h"

#include "client_handshake.h"
#include "delta.h"
#include "file_index.h"
#include "hash_memcache.h"
#include "link_scheduler.h"
#include "net_topology.h"
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
#include <sys/socket.h>
#include <unistd.h>
#elif defined(__linux__)
#include <sys/socket.h>
#include <unistd.h>
#endif
#ifndef _WIN32
#include <fcntl.h>
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
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <set>
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
        return kExitUsage;
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
        return kExitReconnectExhausted;
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

// Binary delta (FC7) server-side byte-range stream. Opened lazily on DeltaRangeOpen, seeked
// to `offset`, then `remaining` bytes are streamed back as DeltaRangeChunk frames followed
// by a single DeltaRangeEnd. Owned exclusively by the send loop (no I/O under any lock).
struct ServerRangeStream {
    std::ifstream input;
    std::string relativePath;
    uint64_t remaining = 0;
    bool errored = false;  // open/read failure -> emit DeltaError instead of DeltaRangeEnd
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

// --- FC6 handshake: version negotiation + the client/server primitives live in
// client_handshake.h/.cpp (fc namespace). HandshakeAndResolveSession below stays here
// because it is coupled to the server-side session registry state. ---
// Forward (non-defining) declaration, NOT external linkage. This and the definition near
// RunServer both live inside fc's single unnamed namespace -- the multiple `namespace {}`
// blocks in this TU are the SAME unnamed namespace -- so both have INTERNAL linkage and name
// the one entity fc::<unnamed>::g_onceTargetClaimed. The `extern` here merely marks this as a
// non-defining declaration so HandshakeAndResolveSession (below) can reference the once-claim
// flag that is defined later; it does not (and cannot) promote it to external linkage.
extern std::atomic<bool> g_onceTargetClaimed;

// Server-side logical session shared by all connections that carry the same sessionId
// (FR-003/004). Per D-02 it only carries merge identity + lifecycle, not transfer state.
struct ServerSession {
    std::string sessionId;
    std::atomic<uint32_t> liveConns{0};
    // Lifecycle field: ALWAYS access under SessionRegistry::mu_ (Create/Join/SweepExpired/
    // OnConnectionClosed). It is not atomic, so any unlocked access is a data race.
    std::chrono::steady_clock::time_point lastActivity{std::chrono::steady_clock::now()};
    // --once: set ONLY on the conn_done clean-return path (FR-07A). Defaults keep
    // non-once behavior unchanged.
    std::atomic<bool> completedOk{false};
    // --once: set when any lane of this session enters conn_error (FR-07).
    std::atomic<bool> hadError{false};
};

// Process-wide registry: sessionId -> session, with idle TTL reclaim (design §5.1/§5.3).
class SessionRegistry {
public:
    // New first connection: mint an unguessable token (NFR-007) and register it with one
    // live connection already counted for the creating connection.
    std::shared_ptr<ServerSession> CreateSession() {
        auto session = std::make_shared<ServerSession>();
        session->sessionId = GenerateSessionToken();
        session->liveConns.store(1, std::memory_order_relaxed);
        session->lastActivity = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(mu_);
        byId_.emplace(session->sessionId, session);
        // --once-multi: count every real session created (never probes / Join lanes), so the
        // main thread can tell "has served >=1 real session" even after the session is swept
        // (FR-08 served-marker / FR-12). Write under mu_, same as byId_.
        ++createdTotal_;
        return session;
    }

    // --once-multi snapshot: number of real sessions still holding a live lane (multi-lane
    // counts as 1, design §6.1 / FR-08) and the cumulative count of created real sessions.
    // Derived from the single source of truth (liveConns) under mu_ so it stays consistent
    // with concurrent Create/Join/Close (NFR-02). O(active sessions), polled ~5x/s.
    struct IdleSnapshot {
        size_t activeRealSessions = 0;
        uint64_t createdTotal = 0;
    };
    IdleSnapshot SnapshotIdle() {
        std::lock_guard<std::mutex> lock(mu_);
        IdleSnapshot snap;
        snap.createdTotal = createdTotal_;
        for (const auto& kv : byId_) {
            if (kv.second->liveConns.load(std::memory_order_relaxed) > 0) {
                ++snap.activeRealSessions;
            }
        }
        return snap;
    }

    // Follow-up connection: look up an existing session and count this connection.
    // Returns nullptr if the id is unknown (reclaimed / forged).
    std::shared_ptr<ServerSession> Join(const std::string& sessionId) {
        std::lock_guard<std::mutex> lock(mu_);
        const auto it = byId_.find(sessionId);
        if (it == byId_.end()) {
            return nullptr;
        }
        it->second->liveConns.fetch_add(1, std::memory_order_relaxed);
        it->second->lastActivity = std::chrono::steady_clock::now();
        return it->second;
    }

    // Returns the session's live-connection count AFTER decrement. The decrement and the
    // read-of-zero happen under the same mu_, so the --once terminal check (RunServer) can
    // observe "this lane closed AND no lane remains" atomically (NFR-02, no TOCTOU). The
    // single existing caller may ignore the return value, leaving non-once behavior intact.
    uint32_t OnConnectionClosed(const std::shared_ptr<ServerSession>& session) {
        if (!session) {
            return 0;
        }
        // lastActivity must only ever be touched under mu_ (it is read/written by Join /
        // SweepExpired under the same lock). Updating it here without the lock raced with
        // those paths from connection-close threads (review B-02), so take mu_ as well.
        std::lock_guard<std::mutex> lock(mu_);
        uint32_t remaining = session->liveConns.load(std::memory_order_relaxed);
        if (remaining > 0) {
            remaining = session->liveConns.fetch_sub(1, std::memory_order_relaxed) - 1;
        }
        session->lastActivity = std::chrono::steady_clock::now();
        return remaining;
    }

    // Event-driven reclaim (D-03): drop sessions with no live connection that have been
    // idle past the TTL. Never reclaims while liveConns > 0 (FR-004).
    void SweepExpired() {
        const auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(mu_);
        for (auto it = byId_.begin(); it != byId_.end();) {
            const bool idle = it->second->liveConns.load(std::memory_order_relaxed) == 0;
            const bool expired = (now - it->second->lastActivity) > kSessionIdleTtl;
            if (idle && expired) {
                it = byId_.erase(it);
            } else {
                ++it;
            }
        }
    }

    size_t SessionCount() {
        std::lock_guard<std::mutex> lock(mu_);
        return byId_.size();
    }

private:
    static constexpr std::chrono::seconds kSessionIdleTtl{60};  // gatekeeper default
    std::mutex mu_;
    std::unordered_map<std::string, std::shared_ptr<ServerSession>> byId_;
    // --once-multi: monotonically increasing count of real sessions ever created. Guarded by
    // mu_; never decremented (a TTL-swept session still counts as "served"). Inert otherwise.
    uint64_t createdTotal_ = 0;
};

SessionRegistry& GetSessionRegistry() {
    static SessionRegistry registry;
    return registry;
}

// Full server handshake: Hello negotiation + session claim (Auth=new / SessionJoin=join).
// On success returns the resolved session (with this connection already counted in
// liveConns); the caller must call OnConnectionClosed exactly once. Throws on rejection.
std::shared_ptr<ServerSession> HandshakeAndResolveSession(const SocketHandle& socket,
                                                          const std::string& password,
                                                          const std::vector<AdvertisedEndpoint>& serverAddrs,
                                                          bool isOnce) {
    NegotiateHelloAsServer(socket);
    const Frame claim = RecvFrame(socket);
    if (claim.type == MsgType::Auth) {
        const std::string got(reinterpret_cast<const char*>(claim.payload.data()), claim.payload.size());
        if (got != password) {
            SendSimple(socket, MsgType::AuthFail, "bad password");
            throw std::runtime_error("Authentication failed");
        }
        if (isOnce && g_onceTargetClaimed.load(std::memory_order_acquire)) {
            SendSimple(socket, MsgType::AuthFail, "once: server already serving one session");
            throw std::runtime_error("Authentication failed: once server already serving one session");
        }
        std::shared_ptr<ServerSession> session = GetSessionRegistry().CreateSession();
        AuthOkInfo info;
        info.role = AuthOkRole::NewSession;
        info.sessionId = session->sessionId;
        info.serverAddrs = serverAddrs;
        // FC7 always advertises delta capability: the server can generate block signatures
        // and serve byte ranges regardless of CLI (delta is a client-driven, opt-in flow).
        info.capabilities |= kCapDelta;
        try {
            SendFrame(socket, Frame{MsgType::AuthOk, 0, EncodeAuthOk(info)});
        } catch (...) {
            GetSessionRegistry().OnConnectionClosed(session);
            throw;
        }
        return session;
    }
    if (claim.type == MsgType::SessionJoin) {
        const SessionJoinInfo join = DecodeSessionJoin(claim.payload);
        if (join.password != password) {
            SendSimple(socket, MsgType::AuthFail, "bad password");
            throw std::runtime_error("Authentication failed");
        }
        std::shared_ptr<ServerSession> session = GetSessionRegistry().Join(join.sessionId);
        if (!session) {
            SendSimple(socket, MsgType::AuthFail, "unknown or expired session");
            throw std::runtime_error("SessionJoin for unknown session");
        }
        AuthOkInfo info;
        info.role = AuthOkRole::JoinAck;
        info.sessionId = session->sessionId;
        info.capabilities |= kCapDelta;
        try {
            SendFrame(socket, Frame{MsgType::AuthOk, 0, EncodeAuthOk(info)});
        } catch (...) {
            GetSessionRegistry().OnConnectionClosed(session);
            throw;
        }
        return session;
    }
    throw std::runtime_error("Expected AUTH or SessionJoin");
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

// Block-signature cache for binary delta (binary-delta D-05). Shares the
// --enable-hash-memcache switch with the full-file hash cache but stores signatures in an
// independent map (zero regression on the full-file hash hot path).
BlockSigMemCache& GetBlockSigMemCache() {
    static BlockSigMemCache cache;
    return cache;
}

// Read an entire regular file into memory (binary delta server-side signature generation /
// design §6.3 "sequentially read the whole file"). Returns false on any open/read failure.
bool ReadWholeFile(const fs::path& abs, std::vector<uint8_t>& out) {
    std::error_code ec;
    const uint64_t size = static_cast<uint64_t>(fs::file_size(abs, ec));
    if (ec) {
        return false;
    }
    std::ifstream input(abs, std::ios::binary);
    if (!input) {
        return false;
    }
    out.resize(static_cast<size_t>(size));
    if (size > 0) {
        input.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(size));
        if (static_cast<uint64_t>(input.gcount()) != size) {
            return false;
        }
    }
    return true;
}

// OS-level durability flush (FR-22 / NFR-05). ofstream::flush only empties the C++ stream
// buffer; FlushFileBuffers / fsync ensures reconstructed bytes reach stable storage before
// the mandatory XXH3 verify and atomic rename.
bool SyncFileToDisk(const fs::path& path) {
#ifdef _WIN32
    // FlushFileBuffers requires a handle opened with GENERIC_WRITE (MSDN); read-only open fails.
    HANDLE handle = CreateFileW(path.wstring().c_str(), GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    const BOOL ok = FlushFileBuffers(handle);
    CloseHandle(handle);
    return ok != 0;
#else
    // fsync must be on a writable fd: macOS (and some Linux filesystems) reject fsync on an
    // O_RDONLY descriptor (EBADF/EINVAL), which would falsely fail the delta durability flush
    // and force an unnecessary full-transfer fallback. We own this freshly-written temp file,
    // so O_RDWR always succeeds; mirrors the Windows GENERIC_WRITE requirement above.
    const int fd = open(path.c_str(), O_RDWR);
    if (fd < 0) {
        return false;
    }
    const bool ok = (fsync(fd) == 0);
    close(fd);
    return ok;
#endif
}

// Per-connection session server. The FC6 handshake + session merge has already been
// performed by the caller (HandshakeAndResolveSession); this body is unchanged from the
// single-connection model and runs independently per connection (D-02).
void RunSessionServer(const SocketHandle& client, const CliOptions& options) {
    const std::optional<fs::path> selfPath = CurrentExePath();
    const bool debugEnabled = IsDebugEnabled();
    const bool hashMemcacheEnabled = GetServerHashMemCache().Enabled();
    const bool blockSigMemcacheEnabled = GetBlockSigMemCache().Enabled();
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
    // Binary delta (FC7): byte-range streams handed off from the receiver, same lock-free
    // discipline as pendingNewStreams (adopted into activeRangeStreams, I/O outside mu).
    std::unordered_map<uint32_t, ServerRangeStream> activeRangeStreams;  // main-loop private
    std::vector<std::pair<uint32_t, ServerRangeStream>> pendingNewRangeStreams;
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
                } else if (frame.type == MsgType::BlockSigRequest) {
                    // Binary delta (FC7, design §6.3). Generate (or memcache-hit) the block
                    // signature set + full-file XXH3-128 and reply BlockSigResponse; any
                    // failure replies DeltaError so the client falls back to full download.
                    const std::string rel = DecodeBlockSigRequest(frame.payload);
                    const fs::path abs = JoinRel(options.rootDir, rel);
                    HashFingerprint fingerprint;
                    const bool fingerprintValid = TryReadHashFingerprint(abs, fingerprint);
                    if (blockSigMemcacheEnabled && fingerprintValid) {
                        Hash256 cachedHash{};
                        delta::SignatureSet cachedSig;
                        if (GetBlockSigMemCache().TryGet(rel, fingerprint, cachedHash, cachedSig)) {
                            enqueueHigh(Frame{MsgType::BlockSigResponse, 0,
                                              EncodeBlockSigResponse(rel, cachedHash, cachedSig)});
                            continue;
                        }
                    }
                    {
                        std::lock_guard<std::mutex> lock(sessionHashMu);
                        ++sessionPendingHashJobs;
                    }
                    try {
                        GetServerHashPool().Enqueue([&, rel, abs, fingerprint, fingerprintValid]() {
                            bool ok = true;
                            Hash256 fileHash{};
                            delta::SignatureSet sig;
                            std::vector<uint8_t> buf;
                            if (!ReadWholeFile(abs, buf)) {
                                ok = false;
                            } else {
                                try {
                                    sig = delta::GenerateSignatures(buf.data(), buf.size());
                                    // Full-file verification hash in the SAME raw layout as
                                    // ComputeFileHash (HashResponse), so the client's FR-23
                                    // check compares like-for-like. Hash the in-memory buffer
                                    // we already read instead of re-reading the file from disk
                                    // (server stays light; avoids a second AV scan pass).
                                    fileHash = ComputeBufferHash(buf.data(), buf.size());
                                } catch (...) {
                                    ok = false;
                                }
                            }
                            if (ok && blockSigMemcacheEnabled) {
                                try {
                                    HashFingerprint afterFingerprint;
                                    if (TryReadHashFingerprint(abs, afterFingerprint) &&
                                        (!fingerprintValid ||
                                         (afterFingerprint.fileSize == fingerprint.fileSize &&
                                          afterFingerprint.mtimeNs == fingerprint.mtimeNs))) {
                                        GetBlockSigMemCache().Upsert(rel, afterFingerprint, fileHash, sig);
                                    }
                                } catch (...) {
                                    // Best-effort cache write; correctness unaffected.
                                }
                            }
                            if (!done.load()) {
                                try {
                                    if (ok) {
                                        enqueueHigh(Frame{MsgType::BlockSigResponse, 0,
                                                          EncodeBlockSigResponse(rel, fileHash, sig)});
                                    } else {
                                        enqueueHigh(Frame{MsgType::DeltaError, 0, EncodeDeltaError(rel)});
                                    }
                                } catch (...) {
                                    failed.store(true);
                                    done.store(true);
                                    outboundCv.notify_all();
                                }
                            }
                            {
                                std::lock_guard<std::mutex> lock(sessionHashMu);
                                if (sessionPendingHashJobs > 0) {
                                    --sessionPendingHashJobs;
                                }
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
                } else if (frame.type == MsgType::DeltaRangeOpen) {
                    // Binary delta (FC7, design §6.3): cheap pread equivalent. Open + seek
                    // here; the send loop streams the bytes lock-free. Failure -> DeltaError.
                    const DeltaRangeRequest req = DecodeDeltaRangeOpen(frame.payload);
                    const fs::path abs = JoinRel(options.rootDir, req.relPath);
                    ServerRangeStream rs;
                    rs.relativePath = req.relPath;
                    rs.remaining = req.length;
                    rs.input.open(abs, std::ios::binary);
                    if (!rs.input) {
                        enqueueHigh(Frame{MsgType::DeltaError, frame.streamId, EncodeDeltaError(req.relPath)});
                        continue;
                    }
                    rs.input.seekg(static_cast<std::streamoff>(req.offset), std::ios::beg);
                    if (!rs.input) {
                        enqueueHigh(Frame{MsgType::DeltaError, frame.streamId, EncodeDeltaError(req.relPath)});
                        continue;
                    }
                    {
                        std::lock_guard<std::mutex> lock(mu);
                        pendingNewRangeStreams.emplace_back(frame.streamId, std::move(rs));
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
                for (auto& kv : pendingNewRangeStreams) {
                    activeRangeStreams.emplace(kv.first, std::move(kv.second));
                    didWork = true;
                }
                pendingNewRangeStreams.clear();
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

                // Binary delta (FC7): serve byte ranges. Each range streams remaining bytes
                // as DeltaRangeChunk frames (bounded burst per round) then DeltaRangeEnd; an
                // I/O error mid-range emits DeltaError so the client falls back to full.
                for (auto it = activeRangeStreams.begin(); it != activeRangeStreams.end();) {
                    ServerRangeStream& rs = it->second;
                    size_t burstBytes = 0;
                    bool finished = false;
                    while (rs.remaining > 0 && burstBytes < perStreamBurstBytes) {
                        const uint64_t toRead =
                            std::min<uint64_t>(rs.remaining, static_cast<uint64_t>(effectiveChunkSize));
                        std::vector<uint8_t> chunk(static_cast<size_t>(toRead));
                        rs.input.read(reinterpret_cast<char*>(chunk.data()),
                                      static_cast<std::streamsize>(chunk.size()));
                        const std::streamsize got = rs.input.gcount();
                        if (got <= 0) {
                            rs.errored = true;
                            break;
                        }
                        chunk.resize(static_cast<size_t>(got));
                        burstBytes += static_cast<size_t>(got);
                        rs.remaining -= static_cast<uint64_t>(got);
                        sendFrames.push_back(Frame{MsgType::DeltaRangeChunk, it->first, std::move(chunk)});
                        didWork = true;
                    }
                    if (rs.errored) {
                        sendFrames.push_back(Frame{MsgType::DeltaError, it->first,
                                                   EncodeDeltaError(rs.relativePath)});
                        it = activeRangeStreams.erase(it);
                        didWork = true;
                        continue;
                    }
                    if (rs.remaining == 0) {
                        sendFrames.push_back(Frame{MsgType::DeltaRangeEnd, it->first, {}});
                        it = activeRangeStreams.erase(it);
                        finished = true;
                        didWork = true;
                    }
                    if (!finished) {
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

// One client-side transport connection in the multipath pool (design §4.2). connId 0 is
// the primary link. Non-copyable/non-movable (atomics + thread); held via unique_ptr.
struct ClientConnection {
    uint32_t connId = 0;
    SocketHandle socket;
    std::string localBind;            // source IP / iface bound (diagnostics)
    std::string serverAddr;           // "host:port" of the peer
    bool isPrimary = false;
    uint32_t nextStreamId = 1;        // per-connection streamId space (main-loop only)
    std::atomic<bool> healthy{true};
    std::string downReason;          // recvThread failure cause (set once on failure)
    size_t inFlight = 0;             // active streams on this connection (main-loop only)
    bool drained = false;            // failover requeue already performed
    std::thread recvThread;
};

// A connection successfully established + joined to the session, returned by the
// auxiliary-connection establishment helper for the caller to wrap into the pool.
struct EstablishedLink {
    SocketHandle socket;
    std::string localBind;
    std::string serverAddr;
};

// Heuristic: a textual local endpoint is a source IP if it looks like a numeric address
// (contains '.' for IPv4 or ':' for IPv6); otherwise it is an interface name (design §6.5).
ConnectBinding BindingFromLocal(const std::string& local) {
    ConnectBinding binding;
    if (local.empty()) {
        return binding;
    }
    if (local.find('.') != std::string::npos || local.find(':') != std::string::npos) {
        binding.localAddr = local;
    } else {
        binding.ifaceName = local;
    }
    return binding;
}

// Establish the auxiliary (non-primary) connections of the pool and JOIN them to the
// session (design §6, FR-005/007/008/009). Best-effort: a failed lane is skipped, never
// fatal (FR-016 / NFR-002). The primary lane (already connected) is excluded via
// primaryServerKey. Explicit --link pins bypass automatic selection (FR-008 / AC-005).
std::vector<EstablishedLink> EstablishAuxiliaryConnections(const CliOptions& options,
                                                           const std::string& sessionId,
                                                           const std::string& primaryServerKey,
                                                           const std::string& primaryLocal,
                                                           const std::string& primaryActualLocal,
                                                           const AuthOkInfo& authInfo,
                                                           bool debugEnabled) {
    std::vector<EstablishedLink> links;
    const size_t maxAux = (options.maxConnections > 0) ? (options.maxConnections - 1) : 0;
    if (maxAux == 0) {
        return links;
    }

    struct Plan {
        std::string local;
        std::string host;
        uint16_t port = 0;
    };
    std::vector<Plan> plans;

    // NIC lookup table for debug logging: ip → LocalAddress (friendlyName, ifaceKey).
    // Populated lazily only when debug output is enabled.
    std::unordered_map<std::string, LocalAddress> nicLookup;
    if (debugEnabled) {
        for (const LocalAddress& c : EnumerateLocalCandidates()) {
            nicLookup.emplace(c.ip, c);
        }
    }

    if (!options.linkPins.empty()) {
        // Explicit mode: pins[0] is the primary (already up); the rest are auxiliaries.
        for (size_t i = 1; i < options.linkPins.size(); ++i) {
            plans.push_back(Plan{options.linkPins[i].local, options.linkPins[i].server,
                                 options.linkPins[i].port});
        }
    } else {
        // Automatic mode: server endpoint set = CLI servers + server-advertised addrs.
        // CLI endpoints have no NIC group (unknown); advertised endpoints carry the server's
        // physical-NIC group as "g<n>". When an advertised endpoint matches a CLI entry
        // (e.g. --server 30.29.53.25 == the primary), backfill the real group onto it so the
        // primary lane's server endpoint resolves to its true NIC for dedup (L-r6-01 / §7.1).
        std::vector<ServerEndpoint> serverEndpoints;
        std::unordered_map<std::string, size_t> serverIndex;  // "host:port" -> index
        auto addServer = [&](const std::string& host, uint16_t port, const std::string& nicGroup) {
            const std::string key = host + ":" + std::to_string(port);
            auto it = serverIndex.find(key);
            if (it == serverIndex.end()) {
                serverIndex.emplace(key, serverEndpoints.size());
                serverEndpoints.push_back(ServerEndpoint{host, port, nicGroup});
            } else if (!nicGroup.empty() && serverEndpoints[it->second].nicGroup.empty()) {
                serverEndpoints[it->second].nicGroup = nicGroup;
            }
        };
        for (const auto& ep : options.servers) {
            addServer(ep.first, ep.second, std::string());
        }
        for (const AdvertisedEndpoint& adv : authInfo.serverAddrs) {
            const auto hp = SplitServerKey(adv.endpoint, options.port);
            addServer(hp.first, hp.second, "g" + std::to_string(adv.nicGroup));
        }
        // Use probe-filtered candidates: deprecated and temporary/privacy IPv6 are
        // excluded, and at most one stable address per (NIC, family) is kept, reducing
        // the probe count from ~60 to a small constant on machines with many deprecated
        // or privacy-extension IPv6 addresses.
        std::vector<LocalAddress> localCands = EnumerateProbeCandidates();
        // Only probe when there is genuine topology to exploit; otherwise stay single-link
        // (degenerate == FC5 behavior, FR-020-adjacent).
        if (serverEndpoints.size() > 1 || localCands.size() > 1) {
            const ReachabilityMatrix matrix = ProbeReachability(localCands, serverEndpoints);
            // Seed the heuristic with the primary's REAL source IP (resolved via
            // getsockname when it used the OS default route), mapped to its physical NIC,
            // plus its server endpoint, so the primary participates in same-NIC + same-side
            // dedup and auxiliaries cannot open a second connection on the primary's NIC
            // (FR-009 / review B-01).
            const std::string primaryDedupLocal =
                !primaryActualLocal.empty() ? primaryActualLocal : primaryLocal;
            const std::string primaryIface = InterfaceKeyForLocalAddress(primaryDedupLocal);
            // Fall back to the IP literal as the seed key when the primary's source IP is
            // not on an enumerable NIC (still blocks the server endpoint via primaryServerKey).
            const std::string primaryDedupIface =
                !primaryIface.empty() ? primaryIface : primaryDedupLocal;
            const std::vector<LinkPlan> auto_ =
                SelectAutoLinks(matrix, options.maxConnections, primaryDedupIface, primaryServerKey);
            for (const LinkPlan& lp : auto_) {
                plans.push_back(Plan{lp.localAddr, lp.serverHost, lp.serverPort});
            }
        }
    }

    std::set<std::string> establishedKeys;
    establishedKeys.insert(primaryLocal + "=>" + primaryServerKey);
    if (!primaryActualLocal.empty()) {
        establishedKeys.insert(primaryActualLocal + "=>" + primaryServerKey);
    }
    for (const Plan& plan : plans) {
        if (links.size() >= maxAux) {
            break;
        }
        const std::string serverKey = plan.host + ":" + std::to_string(plan.port);
        const std::string laneKey = plan.local + "=>" + serverKey;
        if (!establishedKeys.insert(laneKey).second) {
            continue;  // duplicate lane (incl. the primary) -> skip
        }
        try {
            SocketHandle aux = ConnectTo(plan.host, plan.port, BindingFromLocal(plan.local));
            HandshakeClientJoin(aux, options.password, sessionId);
            EstablishedLink link;
            link.socket = std::move(aux);
            link.localBind = plan.local;
            link.serverAddr = serverKey;
            links.push_back(std::move(link));
            if (debugEnabled) {
                std::cout << "[mp] conn_join connId=" << links.size()
                          << " sessionId=" << sessionId << " local=" << plan.local
                          << " server=" << serverKey << " primary=0" << std::endl;
            }
            if (debugEnabled) {
                const auto nicIt = nicLookup.find(plan.local);
                const std::string& nicName =
                    (nicIt != nicLookup.end()) ? nicIt->second.friendlyName : std::string();
                const std::string& ifaceKey =
                    (nicIt != nicLookup.end()) ? nicIt->second.ifaceKey : std::string();
                std::cerr << "[mp][debug] conn_join_nic connId=" << links.size()
                          << " nic=\"" << nicName << "\""
                          << " ifaceKey=" << ifaceKey
                          << " local=" << plan.local
                          << " server=" << serverKey << std::endl;
            }
        } catch (const std::exception& ex) {
            if (debugEnabled) {
                std::cerr << "[mp] conn_join_failed local=" << plan.local
                          << " server=" << serverKey << " reason=\"" << ex.what() << "\""
                          << std::endl;
            }
        }
    }
    return links;
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
                         size_t connections,
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
              << "  Connections: " << connections
              << std::endl;
}

}  // namespace

namespace {

// --- OneShot server mode (--once) process-wide state (design §2.3 / §3.5/§3.6) ---
// Single-instance per process: main() calls RunServer at most once (R-04). All only ever
// read/written when options.exitAfterSync is true; default values are inert otherwise.
std::atomic<bool>         g_onceShouldExit{false};
std::atomic<int>          g_onceExitCode{kExitOk};
std::atomic<SocketNative> g_onceListenSock{kInvalidSocket};
std::atomic<bool>         g_onceTerminalFired{false};
std::atomic<bool>         g_onceTargetClaimed{false};  // write-once (false -> true), never reset
std::mutex                g_onceMu;
std::weak_ptr<ServerSession> g_onceTarget;  // guarded by g_onceMu

// g_onceListenSock is written/exchanged from a non-main (terminal) thread and read on the
// accept loop, so it must be a real lock-free atomic. This holds on every target we ship
// (x64, ARM64 incl. Windows-on-ARM / Apple Silicon, and POSIX int sockets); the only way it
// could degrade to a locking fallback is an exotic 32-bit platform with a 64-bit socket handle
// and no 64-bit atomics, which we do not target. Enforce the assumption at compile time.
static_assert(std::atomic<SocketNative>::is_always_lock_free,
              "g_onceListenSock must be lock-free on supported platforms");

// Claim the first real session as the once-target, or report whether this session IS the
// once-target (its own SessionJoin lanes match). Returns false for a second, independent
// real session, which RunServer then refuses to serve (FR-05 / FR-10).
bool ClaimOrMatchOnceTarget(const std::shared_ptr<ServerSession>& s) {
    std::lock_guard<std::mutex> lk(g_onceMu);
    if (!g_onceTargetClaimed.load(std::memory_order_relaxed)) {
        g_onceTarget = s;
        // Publish "claimed" after g_onceTarget assignment so acquire readers never observe
        // true without the matching target pointer initialized.
        g_onceTargetClaimed.store(true, std::memory_order_release);
        return true;
    }
    auto cur = g_onceTarget.lock();
    return cur && cur == s;
}

// Close the listening socket to interrupt a blocking accept() on the main thread (FR-09).
// exchange() guarantees the socket is closed exactly once; the main loop's accept catch
// then calls listener.Release() so SocketHandle's destructor does not double-close.
void WakeAcceptLoop() {
    const SocketNative s = g_onceListenSock.exchange(kInvalidSocket);
    if (s != kInvalidSocket) {
#ifdef _WIN32
        shutdown(s, SD_BOTH);
        closesocket(s);
#else
        shutdown(s, SHUT_RDWR);
        ::close(s);
#endif
    }
}

// Record the terminal verdict for the once-target session and wake the accept loop. Fires
// exactly once (NFR-02): success -> kExitOk, otherwise -> kExitOnceSessionFailed.
void FireOnceTerminal(bool success) {
    if (g_onceTerminalFired.exchange(true)) {
        return;
    }
    // Publish order is load-bearing: exitCode is written first, then shouldExit is released
    // last, so any reader that observes shouldExit==true (acquire) is guaranteed to read the
    // final exitCode. This release/acquire pairing (with the accept loop below) is required on
    // weak-memory architectures (ARM64 / Apple Silicon / Windows-on-ARM); x86/x64 TSO would
    // otherwise mask a relaxed ordering bug.
    g_onceExitCode.store(success ? kExitOk : kExitOnceSessionFailed, std::memory_order_relaxed);
    g_onceShouldExit.store(true, std::memory_order_release);
    WakeAcceptLoop();
}

// --- --once-multi process-wide state (design §4.3 / §6.4-6.6) ---
// Sticky failure aggregate: set true the first time any real session ends not-clean and never
// reset (B5 / FR-13). Read once by the main thread when firing the terminal verdict. The
// once-multi exit channel reuses g_onceShouldExit / g_onceExitCode / g_onceTerminalFired (D-04);
// it deliberately does NOT touch g_onceTarget / g_onceMu (those stay --once-only, §8).
std::atomic<bool> g_omAnyFailure{false};

// --- --wait-connect-timeout in-flight handshake guard (design §3.7 / NFR-04 / FR-07) ---
// Counts connections that have been accepted and dispatched to a handshake thread but whose
// outcome (createdTotal++ on Auth success, or close on probe/failure) is not yet observable.
// fetch_add happens on the main thread before dispatch; fetch_sub at the handshake thread's
// single exit. EvaluateWaitConnect refuses to declare a timeout while this is > 0, so a real
// connection mid-handshake at the deadline boundary is never falsely killed (AC-09). Probe
// connections close quickly, drop the count back to 0, and let the timeout fire (AC-08).
std::atomic<uint64_t> g_inFlightHandshakes{0};

// Fire the once-multi terminal verdict from the main thread once idle-grace has elapsed. Mirrors
// FireOnceTerminal's publish order (exitCode relaxed, then shouldExit release) so the accept-loop
// top bail returns the final code. WakeAcceptLoop is redundant here (the main thread itself fires
// this) but kept for FR-14 channel parity and harmless idempotence.
void FireOnceMultiTerminal() {
    if (g_onceTerminalFired.exchange(true)) {
        return;
    }
    g_onceExitCode.store(g_omAnyFailure.load(std::memory_order_relaxed)
                             ? kExitOnceSessionFailed
                             : kExitOk,
                         std::memory_order_relaxed);
    g_onceShouldExit.store(true, std::memory_order_release);
    WakeAcceptLoop();
}

// Main-thread idle-grace evaluator (design §6.4). idleSince lives on the RunServer stack and is
// touched ONLY here, so arm/cancel can never race (D-01). State is derived purely from the
// registry snapshot: served (createdTotal>0) gates entry (FR-12/B4); activeRealSessions==0 arms
// the timer (FR-09) and any new real session re-arms it from zero (FR-10); probes never change
// either field so they neither reset nor block the grace (FR-11).
void EvaluateIdleGrace(uint64_t graceMs,
                       std::optional<std::chrono::steady_clock::time_point>& idleSince) {
    const SessionRegistry::IdleSnapshot snap = GetSessionRegistry().SnapshotIdle();
    const bool served = snap.createdTotal > 0;
    const auto now = std::chrono::steady_clock::now();
    if (served && snap.activeRealSessions == 0) {
        if (!idleSince) {
            idleSince = now;  // arm: enter S3 grace window (FR-09)
            std::cout << "[om] idle_grace_armed grace_ms=" << graceMs << std::endl;
        } else if (now - *idleSince >= std::chrono::milliseconds(graceMs)) {
            std::cout << "[om] terminal exit="
                      << (g_omAnyFailure.load(std::memory_order_relaxed)
                              ? kExitOnceSessionFailed
                              : kExitOk)
                      << std::endl;
            FireOnceMultiTerminal();  // S4: grace elapsed (FR-13/FR-14)
        }
    } else if (idleSince) {
        // active>0 again (new/returning real session) -> cancel and reset the timer (FR-10/B4).
        idleSince.reset();
        std::cout << "[om] idle_grace_reset active=" << snap.activeRealSessions << std::endl;
    }
}

// Bounded extra window (past the deadline) during which an in-flight handshake may still defer the
// wait-connect timeout (design §3.7 "defer one tick", made finite to fix B-01). A genuine handshake
// latches createdTotal within milliseconds, so a sub-second cap never falsely kills a real
// connection racing the boundary (AC-09 / NFR-04); but it guarantees that a TCP connection which is
// accepted and then never sends any handshake bytes — leaving its handshake thread blocked in recv
// with g_inFlightHandshakes stuck at >0 — can no longer suppress the timeout indefinitely
// (FR-07 / FR-08 / AC-08).
constexpr int kWaitConnectInFlightGraceMs = 1000;

// Main-thread first-connect-wait evaluator (design §3.3). waitConnectDeadline / firstConnSeen
// live on the RunServer stack and are touched ONLY here, so the same "main-thread single-writer"
// discipline as idle-grace applies (no races, D-01). Returns true iff the wait window has elapsed
// with no valid connection, i.e. the caller must exit with kExitWaitConnectTimeout (FR-08).
//   - firstConnSeen latches true the first time createdTotal>0 (Auth handshake succeeded) and is
//     never reset, so once a valid connection arrives the timer is permanently disabled (FR-09).
//   - Probe connections never increment createdTotal, so they do not stop the timer (FR-07); an
//     in-flight handshake (g_inFlightHandshakes>0) defers the timeout, but only within the BOUNDED
//     grace window [deadline, deadline+kWaitConnectInFlightGraceMs). This avoids killing a real
//     connection at the deadline boundary (NFR-04 / §3.7) while ensuring an accepted-but-silent
//     connection whose handshake thread is parked in recv cannot block the timeout forever (B-01).
bool EvaluateWaitConnect(uint64_t timeoutMs, bool& firstConnSeen,
                         const std::chrono::steady_clock::time_point& deadline) {
    if (firstConnSeen) {
        return false;  // permanently disabled after the first valid connection (FR-09)
    }
    const SessionRegistry::IdleSnapshot snap = GetSessionRegistry().SnapshotIdle();
    if (snap.createdTotal > 0) {
        firstConnSeen = true;  // first valid connection observed (FR-06/FR-09)
        std::cout << "[wc] first_valid_connection (wait-connect disabled)" << std::endl;
        return false;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now < deadline) {
        return false;  // wait window not elapsed yet
    }
    // Deadline reached with no valid connection. Defer to an in-flight handshake only while it is
    // still plausibly mid-handshake (within the bounded grace past the deadline). Past that, a
    // never-completing handshake (B-01: accepted TCP connection that sends nothing) must not stop
    // the timeout from firing.
    if (g_inFlightHandshakes.load(std::memory_order_acquire) > 0 &&
        now < deadline + std::chrono::milliseconds(kWaitConnectInFlightGraceMs)) {
        return false;  // genuine handshake may still latch createdTotal (NFR-04 / §3.7)
    }
    std::cout << "[wc] wait_connect_timeout threshold_ms=" << timeoutMs
              << " no_valid_connection=1" << std::endl;  // FR-13 / AC-13
    return true;
}

}  // namespace

int RunServer(const CliOptions& options) {
    WsaContext wsa;
    const TunedTransferOptions tuned = ResolveTransferOptions(options);
    const uint32_t hashWorkerCount = ResolveServerHashWorkerCount(options);
    GetServerHashPool().Configure(hashWorkerCount);
    GetServerHashMemCache().Configure(options.enableHashMemcache);
    GetBlockSigMemCache().Configure(options.enableHashMemcache);
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
    // Publish the listener fd so a connection-close thread can interrupt accept() on
    // terminal (--once, FR-09). Harmless for non-once: it is only ever read via WakeAcceptLoop.
    g_onceListenSock.store(listener.Get());

    // Collect the server's advertised endpoint list once at startup (FR-005 / §6.1, r6 §6.1).
    // Sent to the first connection in AuthOk so the client can extend the connection pool.
    // Each endpoint carries a dense per-physical-NIC group: all addresses of one adapter
    // (its IPv4 + IPv6) share one group, letting the client dedup by server NIC (L-r6-01).
    std::vector<AdvertisedEndpoint> serverAddrs;
    {
        std::unordered_map<std::string, uint16_t> ifaceToGroup;  // adapter key -> dense group id
        uint16_t nextGroup = 0;
        for (const LocalAddress& cand : EnumerateLocalCandidates()) {
            // IPv6 literals contain ':' and must be bracketed so the host/port split on the
            // client side is unambiguous: "[ipv6]:port" vs the bare IPv4 "ip:port" (review D-01).
            const bool isIpv6 = cand.ip.find(':') != std::string::npos;
            const std::string endpoint = isIpv6
                                             ? "[" + cand.ip + "]:" + std::to_string(options.port)
                                             : cand.ip + ":" + std::to_string(options.port);
            // Group by the physical-interface key; addresses with no key (rare) each get a
            // distinct group so they are never falsely merged onto one NIC.
            const std::string groupKey =
                !cand.ifaceKey.empty() ? cand.ifaceKey : ("addr:" + cand.ip);
            auto it = ifaceToGroup.find(groupKey);
            if (it == ifaceToGroup.end()) {
                it = ifaceToGroup.emplace(groupKey, nextGroup++).first;
            }
            serverAddrs.push_back(AdvertisedEndpoint{endpoint, it->second});
        }
    }
    std::cout << "[mp] advertised_endpoints=" << serverAddrs.size() << std::endl;

    const bool debugEnabled = IsDebugEnabled();
    std::atomic<uint64_t> connIdCounter{0};
    std::atomic<uint32_t> activeSessions{0};
    // --once-multi accept-loop evaluation tick (design §6.2). grace is measured by wall clock,
    // so its resolution is +/- one tick (negligible for second-scale grace).
    constexpr int kOnceMultiTickMs = 200;
    // --once-multi idle timer: main-thread-only, so arm/cancel can never race (design §6.4/§7).
    std::optional<std::chrono::steady_clock::time_point> idleSince;
    // --wait-connect-timeout (design §3.3): arm a first-connect deadline for --once / --once-multi.
    // Both timer state slots are main-thread-only (same discipline as idleSince). firstConnSeen
    // latches the moment the first valid connection appears and permanently disables the timer
    // (FR-09); under --once it also reverts the accept loop from per-tick polling back to the
    // original blocking accept so the post-first-connection path is byte-for-byte unchanged (§3.6).
    constexpr int kWaitConnectTickMs = 200;
    const bool waitConnectActive = options.exitAfterSync || options.onceMulti;
    std::optional<std::chrono::steady_clock::time_point> waitConnectDeadline;
    bool firstConnSeen = false;
    if (waitConnectActive) {
        waitConnectDeadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(options.waitConnectTimeoutMs);
        std::cout << "[wc] wait_connect_armed timeout_ms=" << options.waitConnectTimeoutMs
                  << std::endl;
    }
    while (true) {
        // --once: a terminal verdict may have fired while we were dispatching; bail before
        // blocking again so we never serve a second session (FR-10) and return cleanly (FR-09).
        if (options.exitAfterSync && g_onceShouldExit.load(std::memory_order_acquire)) {
            listener.Release();  // fd already closed by WakeAcceptLoop; drop ownership.
            return g_onceExitCode.load(std::memory_order_relaxed);  // published-before shouldExit
        }
        // --once-multi: idle-grace fired (from this same thread) -> return the aggregated code.
        if (options.onceMulti && g_onceShouldExit.load(std::memory_order_acquire)) {
            listener.Release();
            return g_onceExitCode.load(std::memory_order_relaxed);  // published-before shouldExit
        }
        SocketHandle client;
        if (options.onceMulti) {
            // Poll-with-timeout accept so this thread regains control every tick to evaluate
            // idle-grace, while accept stays open for new sessions (FR-07/FR-15). Per-tick
            // "Waiting for client..." is intentionally suppressed to avoid a 5/s log flood.
            std::optional<SocketHandle> maybe;
            try {
                maybe = AcceptClientTimeout(listener, kOnceMultiTickMs);
            } catch (const std::exception&) {
                if (g_onceShouldExit.load(std::memory_order_acquire)) {
                    listener.Release();
                    return g_onceExitCode.load(std::memory_order_relaxed);
                }
                throw;
            }
            if (!maybe) {
                // Timeout tick: evaluate first-connect-wait BEFORE idle-grace (design §3.8). Before
                // the first valid connection idle-grace is a no-op (served==false), so the ordering
                // is side-effect free; after it, wait-connect is permanently disabled.
                // onceMulti implies waitConnectActive (waitConnectActive = exitAfterSync || onceMulti),
                // so waitConnectDeadline is guaranteed to have_value here; the deref is safe.
                if (!firstConnSeen &&
                    EvaluateWaitConnect(options.waitConnectTimeoutMs, firstConnSeen,
                                        *waitConnectDeadline)) {
                    listener.Release();
                    return kExitWaitConnectTimeout;  // FR-08
                }
                // Timeout tick: evaluate idle-grace ONLY when nothing was accepted this tick,
                // so a just-accepted connection is never immediately judged idle (design §6.2/R-02).
                EvaluateIdleGrace(options.onceIdleGraceMs, idleSince);
                continue;
            }
            client = std::move(*maybe);
        } else if (waitConnectActive && !firstConnSeen) {
            // --once first-connect wait (design §3.6): poll with a short tick instead of blocking
            // accept so wait-connect can fire when only probes (or nothing) arrive (AC-07/AC-08).
            // Once firstConnSeen latches, the branch below resumes the original blocking accept.
            std::optional<SocketHandle> maybe;
            try {
                maybe = AcceptClientTimeout(listener, kWaitConnectTickMs);
            } catch (const std::exception&) {
                // --once: the terminal thread closed the listener to wake us. Tolerate this one
                // accept failure and return the recorded exit code (never exit() mid-flight, FR-09).
                if (options.exitAfterSync && g_onceShouldExit.load(std::memory_order_acquire)) {
                    listener.Release();
                    return g_onceExitCode.load(std::memory_order_relaxed);
                }
                throw;
            }
            if (!maybe) {
                if (EvaluateWaitConnect(options.waitConnectTimeoutMs, firstConnSeen,
                                        *waitConnectDeadline)) {
                    listener.Release();
                    return kExitWaitConnectTimeout;  // FR-08
                }
                continue;
            }
            client = std::move(*maybe);
        } else {
            std::cout << "Waiting for client... active_connections=" << activeSessions.load()
                      << " sessions=" << GetSessionRegistry().SessionCount() << std::endl;
            try {
                client = AcceptClient(listener);
            } catch (const std::exception&) {
                // --once: the terminal thread closed the listener to wake us. Tolerate this one
                // accept failure and return the recorded exit code (never exit() mid-flight, FR-09).
                if (options.exitAfterSync && g_onceShouldExit.load(std::memory_order_acquire)) {
                    listener.Release();
                    return g_onceExitCode.load(std::memory_order_relaxed);  // published-before shouldExit
                }
                throw;  // non-once: preserve existing "accept error -> main catch -> exit 1".
            }
        }
        const uint64_t connSeq = connIdCounter.fetch_add(1) + 1;
        activeSessions.fetch_add(1);
        // wait-connect in-flight guard (§3.7): mark this connection's handshake as pending before
        // dispatch; the thread's single exit drops it. Inert when wait-connect is not armed.
        g_inFlightHandshakes.fetch_add(1, std::memory_order_acq_rel);
        std::thread([connSeq, debugEnabled, &activeSessions, options, serverAddrs,
                     client = std::move(client)]() mutable {
            std::shared_ptr<ServerSession> session;
            try {
                session = HandshakeAndResolveSession(client, options.password, serverAddrs,
                                                     options.exitAfterSync);
                std::cout << "[mp] conn_accept conn=" << connSeq
                          << " sessionId=" << session->sessionId
                          << " live_conns=" << session->liveConns.load() << std::endl;
                if (options.exitAfterSync && !ClaimOrMatchOnceTarget(session)) {
                    // Fallback guard for a tiny race window: if a second Auth slipped past the
                    // pre-AuthFail check before claimed=true became visible, refuse to serve it.
                    // This path should be rare; normal second Auth is rejected in handshake.
                    std::cerr << "[mp] once_reject_post_auth_fallback conn=" << connSeq
                              << " sessionId=" << session->sessionId << std::endl;
                } else {
                    RunSessionServer(client, options);
                    std::cout << "[mp] conn_done conn=" << connSeq
                              << " sessionId=" << session->sessionId << std::endl;
                    if (options.exitAfterSync || options.onceMulti) {
                        session->completedOk.store(true, std::memory_order_relaxed);  // FR-06/07A; om: D-03
                    }
                }
            } catch (const std::exception& ex) {
                // Distinguish pre-handshake close (reachability probe: client connects
                // then immediately closes before sending any bytes → recv returns 0,
                // WSA=0 / errno=0) from a real session error. Pre-handshake closes are
                // benign; suppress [mp] conn_error noise and demote to debug.
                const bool preHandshake = (session == nullptr);
                const std::string errMsg = ex.what();
                const bool isCleanClose =
                    errMsg.find("recv failed WSA=0") != std::string::npos ||
                    errMsg.find("recv failed errno=0") != std::string::npos;
                if (preHandshake && isCleanClose) {
                    if (debugEnabled) {
                        std::cerr << "[mp][debug] probe_or_preauth_close conn=" << connSeq
                                  << std::endl;
                    }
                } else {
                    std::cerr << "[mp] conn_error conn=" << connSeq << " error: " << errMsg
                              << std::endl;
                }
                // --once / --once-multi: any real-session lane error marks the session failed
                // (FR-07). A pre-handshake close has session == null and never sets this (FR-08.1).
                if ((options.exitAfterSync || options.onceMulti) && session) {
                    session->hadError.store(true, std::memory_order_relaxed);
                }
            }
            // Release this connection's session count and run the event-driven TTL sweep.
            // OnConnectionClosed returns the live-lane count after decrement, read under the
            // registry lock (NFR-02).
            const uint32_t remaining = GetSessionRegistry().OnConnectionClosed(session);
            GetSessionRegistry().SweepExpired();
            // --once terminal check: when the last lane of the once-target closes, decide the
            // verdict (FR-06/07/07A) and wake the accept loop. completedOk is set only on the
            // clean conn_done path, so success requires it AND the absence of any lane error.
            if (options.exitAfterSync && session) {
                bool isTarget;
                {
                    std::lock_guard<std::mutex> lk(g_onceMu);
                    isTarget = (g_onceTarget.lock() == session);
                }
                if (isTarget && remaining == 0) {
                    FireOnceTerminal(session->completedOk.load(std::memory_order_relaxed) &&
                                     !session->hadError.load(std::memory_order_relaxed));
                }
            } else if (options.onceMulti && session) {
                // --once-multi: when a real session's last lane closes, fold its verdict into the
                // sticky failure aggregate (B5/FR-13). The main thread later reads it at terminal.
                // No g_onceTarget involvement (§8): every real session is served, not just one.
                if (remaining == 0) {
                    const bool ok = session->completedOk.load(std::memory_order_relaxed) &&
                                    !session->hadError.load(std::memory_order_relaxed);
                    if (!ok) {
                        g_omAnyFailure.store(true, std::memory_order_relaxed);
                    }
                }
            }
            activeSessions.fetch_sub(1);
            // wait-connect in-flight guard (§3.7): single exit for every dispatched connection,
            // covering the clean, error, and pre-handshake-close paths (R-03: no leak -> no stall).
            g_inFlightHandshakes.fetch_sub(1, std::memory_order_acq_rel);
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
    //   binary-delta state (deltaStates/deltaAbandoned/pendingDeltaSigRequests/
    //   deltaSigRequested/pendingDeltaRanges/activeDeltaRanges) -- all declared INSIDE the
    //   while below so each session re-evaluates delta from scratch (FR-25 per-session),
    //   manifestDone, recvError/recvClosed -- each session is a fresh FC5 handshake +
    //   ManifestRequest. Already-synced local files persist on DISK only; the next session
    //   re-enumerates and skips them via size+mtime compare (no in-memory carry-over).
    uint32_t reconnectAttemptsUsed = 0;
    const auto reconnectWindowStart = std::chrono::steady_clock::now();

    // ConnectTo + handshake failures (server not ready) reuse the same reconnect budget
    // as mid-session drops; see ScheduleClientReconnectOrExit().
    while (true) {
    // Multipath connection pool (design §4.2). pool[0] is the primary link.
    std::vector<std::unique_ptr<ClientConnection>> pool;
    std::string sessionId;
    // Binary delta (FC7): enabled this session only when the client opted in
    // (--delta-min-size > 0) AND the server advertised the delta capability bit in AuthOk
    // (binary-delta §8.1 / AC-17). Set right after the primary handshake below.
    bool deltaEnabled = false;
    try {
        // Primary lane + new-session handshake. Explicit mode: linkPins[0] is the primary
        // (FR-002). Otherwise servers[0] with the OS-default source route.
        ConnectBinding primaryBinding;
        std::string primaryHost = options.host;
        uint16_t primaryPort = options.port;
        std::string primaryLocal;
        if (!options.linkPins.empty()) {
            primaryLocal = options.linkPins.front().local;
            primaryBinding = BindingFromLocal(primaryLocal);
            primaryHost = options.linkPins.front().server;
            primaryPort = options.linkPins.front().port;
        }
        SocketHandle primarySocket = ConnectTo(primaryHost, primaryPort, primaryBinding);
        const AuthOkInfo authInfo = HandshakeClientNew(primarySocket, options.password);
        sessionId = authInfo.sessionId;
        deltaEnabled = (options.deltaMinSizeBytes > 0) &&
                       ((authInfo.capabilities & kCapDelta) != 0);

        auto primaryConn = std::make_unique<ClientConnection>();
        primaryConn->connId = 0;
        primaryConn->socket = std::move(primarySocket);
        primaryConn->localBind = primaryLocal;
        primaryConn->serverAddr = primaryHost + ":" + std::to_string(primaryPort);
        primaryConn->isPrimary = true;
        pool.push_back(std::move(primaryConn));
        if (debugEnabled) {
            std::cout << "[mp] conn_join connId=0 sessionId=" << sessionId
                      << " local=" << primaryLocal << " server=" << pool[0]->serverAddr
                      << " primary=1" << std::endl;
        }

        // Resolve the primary's REAL bound source IP so it joins same-side dedup even when
        // it was connected via the OS default route with no explicit bind (review B-01).
        const std::string primaryActualLocal = LocalAddressOf(pool[0]->socket);
        if (debugEnabled) {
            const std::string& lookupIp =
                !primaryActualLocal.empty() ? primaryActualLocal : primaryLocal;
            if (!lookupIp.empty()) {
                for (const LocalAddress& c : EnumerateLocalCandidates()) {
                    if (c.ip == lookupIp) {
                        std::cerr << "[mp][debug] conn_join_nic connId=0"
                                  << " nic=\"" << c.friendlyName << "\""
                                  << " ifaceKey=" << c.ifaceKey
                                  << " local=" << lookupIp
                                  << " server=" << pool[0]->serverAddr << std::endl;
                        break;
                    }
                }
            }
        }
        // Auxiliary lanes (best-effort; a failed lane is skipped, FR-016 / NFR-002).
        std::vector<EstablishedLink> auxLinks = EstablishAuxiliaryConnections(
            options, sessionId, pool[0]->serverAddr, primaryLocal, primaryActualLocal,
            authInfo, debugEnabled);
        for (auto& link : auxLinks) {
            auto conn = std::make_unique<ClientConnection>();
            conn->connId = static_cast<uint32_t>(pool.size());
            conn->socket = std::move(link.socket);
            conn->localBind = link.localBind;
            conn->serverAddr = link.serverAddr;
            conn->isPrimary = false;
            pool.push_back(std::move(conn));
        }
    } catch (const std::exception& ex) {
        const std::string connectReason = ex.what();
        if (const std::optional<int> exitCode = ScheduleClientReconnectOrExit(
                connectReason, options.reconnectRetries, options.reconnectWindowMs,
                reconnectAttemptsUsed, reconnectWindowStart, /*exitWhenDisabled=*/kExitUsage)) {
            if (*exitCode == kExitUsage) {
                std::cerr << "FastClone error: " << connectReason << std::endl;
            }
            return *exitCode;
        }
        continue;
    }
    ClientConnection& primary = *pool[0];
    if (debugEnabled) {
        std::cout << "[mp] pool_size=" << pool.size() << " sessionId=" << sessionId << std::endl;
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

    // Manifest is requested only on the primary link (design §7.5): the server enumerates
    // and streams the manifest on the first connection; auxiliary lanes carry file data only.
    // A peer reset between the handshake and this request (mid-session drop, e.g. server
    // killed) must not be fatal: no manifest was received, so honor the reconnect budget and
    // otherwise exit 3 exactly like the post-loop incomplete-manifest gate (FR-016 / AC-012,
    // IT-3). This send precedes the recv threads / failover machinery, so it is handled here.
    try {
        SendFrame(primary.socket, Frame{MsgType::ManifestRequest, 0, {}});
    } catch (const std::exception& ex) {
        for (auto& cptr : pool) {
            ShutdownBoth(cptr->socket);
        }
        std::cout << "Sync aborted (incomplete manifest). changed_files=0 failed_files=0"
                  << " enumerated=0 elapsed=" << formatElapsed() << std::endl;
        if (const std::optional<int> exitCode = ScheduleClientReconnectOrExit(
                ex.what(), options.reconnectRetries, options.reconnectWindowMs,
                reconnectAttemptsUsed, reconnectWindowStart, /*exitWhenDisabled=*/kExitIncompleteNoReconnect)) {
            return *exitCode;
        }
        continue;
    }
    std::unordered_map<std::string, FileEntry> remoteFiles;
    std::unordered_set<std::string> remoteDirs;
    // Transfer state is keyed by a composite (connId, streamId) so each lane has an
    // independent streamId space and frames never collide across connections (design §7.5).
    // Invariant: connId and streamId each occupy a disjoint 32-bit half of the 64-bit key.
    // connId is sourced from a uint32_t connection counter and streamId from a uint32_t
    // stream counter, so neither can overflow its half today. The static_assert pins the
    // parameter widths so a future widening of either id (e.g. to uint64_t) fails to
    // compile here rather than silently aliasing keys across lanes (review S-03).
    auto streamKey = [](uint32_t connId, uint32_t streamId) -> uint64_t {
        static_assert(sizeof(connId) * 8 + sizeof(streamId) * 8 <= 64,
                      "streamKey requires connId+streamId to fit in 64 bits without aliasing");
        return (static_cast<uint64_t>(connId) << 32) | static_cast<uint64_t>(streamId);
    };
    std::unordered_map<uint64_t, DownloadState> activeDownloads;
    std::unordered_map<uint64_t, BatchDownloadState> activeBatchDownloads;
    std::unordered_map<uint64_t, std::string> streamToPath;
    std::deque<std::string> pendingTransfers;
    std::deque<std::string> pendingBatchTransfers;
    std::deque<std::string> pendingRetryTransfers;
    std::deque<std::string> pendingRetryBatchTransfers;
    std::unordered_set<std::string> scheduledTransfers;
    std::unordered_map<std::string, uint8_t> transferRetryCounts;

    // --- Binary delta (FC7) per-session state (reset every session, binary-delta §6.4) ---
    // Each in-progress delta file: verification hash from BlockSigResponse, temp reconstruct
    // file + writer, and the count of outstanding (possibly sliced) miss ranges.
    struct DeltaFileState {
        Hash256 verifyHash{};        // full-file XXH3-128 for the FR-23 reconstruction check
        uint64_t newFileBytes = 0;
        std::filesystem::path tempPath;
        std::ofstream tempOut;       // random-access reconstruction writer
        uint32_t pendingRanges = 0;  // outstanding miss-range tasks (DeltaRangeEnd decrements)
        bool sigReceived = false;    // guards duplicate BlockSigResponse (failover re-request)
    };
    std::unordered_map<std::string, DeltaFileState> deltaStates;
    std::unordered_set<std::string> deltaAbandoned;        // never retry delta this session (FR-25)
    std::deque<std::string> pendingDeltaSigRequests;       // rel awaiting BlockSigRequest send
    std::unordered_set<std::string> deltaSigRequested;     // rel: BlockSigRequest in flight/sent
    struct DeltaRangeTask {
        std::string rel;
        uint64_t offset = 0;
        uint32_t length = 0;
    };
    std::deque<DeltaRangeTask> pendingDeltaRanges;         // miss ranges awaiting a lane
    struct ActiveDeltaRange {
        std::string rel;
        uint64_t destOffset = 0;
        uint32_t length = 0;
        uint32_t received = 0;
    };
    std::unordered_map<uint64_t, ActiveDeltaRange> activeDeltaRanges;  // (connId,streamId) -> range
    uint64_t deltaReconstructed = 0;  // diagnostics: files completed via delta this session

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

    // Incoming frames carry their originating connId so the main loop can demux transfer
    // streams per connection (design §7.5). All lanes share one queue + worker pool.
    struct IncomingFrame {
        uint32_t connId = 0;
        Frame frame;
    };
    std::mutex incomingMu;
    std::condition_variable incomingDataCv;
    std::deque<IncomingFrame> incomingPriorityFrames;
    std::deque<IncomingFrame> incomingManifestFrames;
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
    // Per-connection receiver (design §7.5). One thread per lane, all feeding the SAME
    // shared incoming queues + worker pool. Frames are tagged with the lane's connId so
    // the main loop can demux transfer streams. On failure the lane is marked unhealthy
    // (failover is decided by the main loop); recvClosed is only set when ALL lanes die.
    auto startRecvThread = [&](ClientConnection* conn) {
        conn->recvThread = std::thread([&, conn]() {
            constexpr size_t kRecvManifestBatchFrames = 1024;
            constexpr uint64_t kRecvManifestBatchBytes = 4ULL * 1024ULL * 1024ULL;
            std::vector<IncomingFrame> manifestBatch;
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
                        for (IncomingFrame& mf : manifestBatch) {
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
                    Frame f = RecvFrame(conn->socket);
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
                        case MsgType::BlockSigResponse:
                        case MsgType::DeltaRangeChunk:
                        case MsgType::DeltaRangeEnd:
                        case MsgType::DeltaError:
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
                        manifestBatch.push_back(IncomingFrame{conn->connId, std::move(f)});
                        if (forceFlush || manifestBatch.size() >= kRecvManifestBatchFrames ||
                            manifestBatchBytes >= kRecvManifestBatchBytes) {
                            flushManifestBatch();
                        }
                    } else {
                        flushManifestBatch();
                        uint64_t queuedBytesSnapshot = 0;
                        {
                            std::lock_guard<std::mutex> lock(incomingMu);
                            incomingPriorityFrames.push_back(IncomingFrame{conn->connId, std::move(f)});
                            incomingQueuedBytes += frameWireBytes(incomingPriorityFrames.back().frame);
                            queuedBytesSnapshot = incomingQueuedBytes;
                        }
                        incomingDataCv.notify_one();
                        applyRecvBackpressure(queuedBytesSnapshot);
                    }
                }
            } catch (const std::exception& ex) {
                if (!recvStop.load()) {
                    conn->downReason = ex.what();
                    conn->healthy.store(false);
                    // Wake the main loop so it can run failover (requeue this lane's files
                    // to healthy lanes, or tear down if this was the last lane).
                    incomingDataCv.notify_all();
                }
            }
        });
    };
    for (auto& cptr : pool) {
        startRecvThread(cptr.get());
    }

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

    // Binary delta (FC7): structured observability for every delta fallback (NFR-04).
    auto deltaFallback = [&](const std::string& rel, const char* reason,
                             uint64_t downloadBytes, uint64_t newFileBytes,
                             const delta::DeltaStats* stats = nullptr) {
        if (debugEnabled || diagnostics) {
            std::cerr << "[delta] fallback rel=" << rel << " reason=" << reason
                      << " downloadBytes=" << downloadBytes
                      << " newFileBytes=" << newFileBytes;
            // DeltaStats (client-local) for perf tuning: present on the BuildPlan-derived
            // fallback (reason=benefit), showing whether the rolling scan early-stopped and
            // how far it scanned before abandoning delta.
            if (stats != nullptr) {
                std::cerr << " early_stopped=" << (stats->earlyStopped ? 1 : 0)
                          << " scanned_bytes=" << stats->scannedBytes
                          << " matched_bytes=" << stats->matchedBytes
                          << " strong_computes=" << stats->strongComputations
                          << " weak_hits=" << stats->weakCandidateHits;
            }
            std::cerr << std::endl;
        }
    };

    // Binary delta gate (binary-delta §6.2, FR-05~FR-08). Returns true only when the file
    // is admitted into the delta flow (a BlockSigRequest is queued); the caller falls back to
    // scheduleTransfer() on false, with zero side effects (FR-06). Conditions, all required:
    //   G2 deltaEnabled (--delta-min-size>0 and server advertised the capability)
    //   FR-25 not already abandoned this session; not already an in-flight delta
    //   G3 remote fileSize >= deltaMinSizeBytes
    //   G4 local old file exists, is readable, and size > 0 (else new/unreadable -> full path)
    auto tryEnterDelta = [&](const std::string& rel) -> bool {
        if (!deltaEnabled) {
            return false;  // G2 (also the deltaMinSize==0 zero-regression bypass)
        }
        if (deltaAbandoned.contains(rel) || deltaStates.contains(rel) ||
            deltaSigRequested.contains(rel)) {
            return false;
        }
        const auto it = remoteFiles.find(rel);
        if (it == remoteFiles.end() || it->second.fileSize < options.deltaMinSizeBytes) {
            return false;  // G3
        }
        const fs::path abs = JoinRel(options.rootDir, rel);
        std::error_code ec;
        if (!fs::exists(abs, ec) || ec) {
            return false;  // G4: brand-new file -> existing full path (FR-07), no reason log
        }
        const uint64_t localSize = static_cast<uint64_t>(fs::file_size(abs, ec));
        if (ec || localSize == 0) {
            return false;  // empty/new -> full path (FR-07)
        }
        {
            std::ifstream probe(abs, std::ios::binary);
            if (!probe) {
                deltaFallback(rel, "old_unreadable", 0, it->second.fileSize);  // FR-08 / AC-12
                return false;
            }
        }
        DeltaFileState st;
        st.newFileBytes = it->second.fileSize;
        deltaStates.emplace(rel, std::move(st));
        pendingDeltaSigRequests.push_back(rel);
        return true;
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
            // Observable requeue (AC-018 / design §11): a file/batch entry put back into the
            // pending pool for redistribution to a healthy lane (the dead lane's connId is
            // reported by the adjacent "[mp] conn_down ... requeued_files=" line).
            if (debugEnabled) {
                std::cout << "[mp] requeue sessionId=" << sessionId << " path=" << rel
                          << " attempt=" << static_cast<uint32_t>(retries)
                          << "/" << static_cast<uint32_t>(kMaxTransferRetries) << std::endl;
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
            // Size-different changed file (e.g. append/insert). delta gate G4 admits it only
            // when a readable local old version exists; brand-new files fall through to full.
            if (!tryEnterDelta(r.relPath)) {
                scheduleTransfer(r.relPath);
            }
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
        PrintClientCounters(enumerated, compared, unchanged, failed, transferred, deleted, pool.size(), lastEnum, lastCompared, lastUnchanged, lastFailed, lastTransferred, lastDeleted);
    };

    // A SYNCHRONOUS send failure (peer reset mid-sync, e.g. WSA 10054) is just another way
    // a lane dies; treat it exactly like a recvThread failure so failoverScan() requeues its
    // in-flight work to healthy lanes (FR-015) or, once EVERY lane is down, trips recvClosed
    // -> the incomplete-manifest abort path (FR-016 / AC-012). Previously such a send threw
    // out of the main loop and aborted the whole sync fatally instead of going through the
    // reconnect/exit-3 contract. exchange() guarantees only the first writer records the
    // reason, matching the "set once on failure" contract the recvThread uses.
    auto markConnDown = [&](ClientConnection* c, const std::string& reason) {
        if (c->healthy.exchange(false)) {
            c->downReason = reason;
        }
    };

    // Control-plane lane for hash/manifest traffic: the primary if healthy, otherwise the
    // first healthy lane. Each per-connection server session answers HashRequests
    // independently (D-02), so any healthy lane is correct; the response's connId is
    // irrelevant to HashResponse handling.
    auto controlConn = [&]() -> ClientConnection* {
        if (pool[0]->healthy.load()) {
            return pool[0].get();
        }
        for (auto& cptr : pool) {
            if (cptr->healthy.load()) {
                return cptr.get();
            }
        }
        return pool[0].get();
    };

    auto dispatchHashRequests = [&]() {
        ClientConnection* ctrl = controlConn();
        std::vector<Frame> outboundFrames;
        outboundFrames.reserve(256);
        // A failed control-lane send must not be fatal: mark the lane down (FR-016) and stop
        // for this pass. If a healthy lane remains it becomes the new controlConn() next pass;
        // if every lane is down, failoverScan() trips recvClosed and the loop aborts to exit 3.
        auto flushOutbound = [&]() -> bool {
            try {
                SendFrameBatch(ctrl->socket, outboundFrames);
                outboundFrames.clear();
                return true;
            } catch (const std::exception& ex) {
                outboundFrames.clear();
                markConnDown(ctrl, ex.what());
                return false;
            }
        };
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
                if (!flushOutbound()) {
                    return;
                }
            }
        }
        if (!outboundFrames.empty()) {
            flushOutbound();
        }
    };

    // Adaptive connection selection (design §8): shortest-queue / least-outstanding-requests.
    // Among healthy lanes below streamLimit, pick the one with the fewest in-flight streams
    // (SelectLeastLoadedLane). This is self-correcting on lane speed -- a slow lane holds its
    // streams longer so its inFlight stays high and it stops drawing new work, while a fast
    // lane drains and is refilled -- with no throughput measurement or feedback loop (FR-013 /
    // FR-014). forcePrimary (large files, FR-012) hard-pins to the primary lane.
    auto pickConnection = [&](bool forcePrimary) -> ClientConnection* {
        std::vector<LaneLoad> lanes;
        lanes.reserve(pool.size());
        for (auto& cptr : pool) {
            LaneLoad ld;
            ld.healthy = cptr->healthy.load();
            ld.inFlight = static_cast<uint32_t>(cptr->inFlight);
            // Weighted shortest-queue (aux-weight FR-04): primary stays 1.0, aux lanes share
            // the CLI auxWeight. With the default auxWeight=1.0 every lane is 1.0 -> identical
            // to the legacy least-inFlight policy (FR-06).
            ld.weight = cptr->isPrimary ? 1.0 : options.auxWeight;
            // Manifest bias (aux-weight FR-07/FR-08): while the manifest is still downloading,
            // nudge the primary down by +1 so a little file work can flow to aux first; once
            // manifestDone the bias is gone and ordering returns to pure weighted load.
            ld.bias = (!manifestDone && cptr->isPrimary) ? 1u : 0u;
            lanes.push_back(ld);
        }
        const int idx = SelectLeastLoadedLane(lanes, streamLimit, forcePrimary);
        return (idx >= 0) ? pool[static_cast<size_t>(idx)].get() : nullptr;
    };
    auto healthyConnCount = [&]() -> size_t {
        size_t n = 0;
        for (auto& cptr : pool) {
            if (cptr->healthy.load()) {
                ++n;
            }
        }
        return n;
    };

    auto tryStartTransfers = [&]() {
        auto totalActiveSlots = [&]() -> size_t {
            return activeDownloads.size() + activeBatchDownloads.size();
        };
        auto hasBatchBacklog = [&]() -> bool {
            return !pendingBatchTransfers.empty() || !pendingRetryBatchTransfers.empty();
        };
        // Global in-flight bound = streamLimit per healthy lane (design §8.3); with the
        // <=8 pool cap this is the R-05 safeguard against fan-out exploding server fds.
        const size_t healthy = std::max<size_t>(1, healthyConnCount());
        const size_t globalSlotCap = static_cast<size_t>(streamLimit) * healthy;
        while (totalActiveSlots() < globalSlotCap) {
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
                // Small-file batches are never "large", so they are distributed across all
                // healthy lanes by throughput weight.
                ClientConnection* conn = pickConnection(false);
                if (conn == nullptr) {
                    break;  // all lanes saturated
                }
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
                    const uint32_t sid = conn->nextStreamId++;
                    const uint64_t key = streamKey(conn->connId, sid);
                    BatchDownloadState batchState;
                    activeBatchDownloads.emplace(key, std::move(batchState));
                    streamToPath.emplace(key, std::string());  // marks lane ownership for failover
                    try {
                        SendFrame(conn->socket, Frame{MsgType::FileBatchOpen, sid, EncodeFileBatchRequest(batchPaths)});
                    } catch (const std::exception& ex) {
                        // The batch header never reached the wire, so this stream holds no
                        // server-acknowledged entries yet and failoverScan() cannot recover
                        // batchPaths from it. Undo the speculative bookkeeping and requeue the
                        // files explicitly (FR-015), then mark the lane down so failoverScan /
                        // the session abort take it from here (FR-016).
                        activeBatchDownloads.erase(key);
                        streamToPath.erase(key);
                        markConnDown(conn, ex.what());
                        for (const std::string& bp : batchPaths) {
                            retryOrFail(bp);
                        }
                        break;
                    }
                    ++conn->inFlight;
                    // Observable link-task allocation (AC-018 / design §11): which lane a
                    // small-file batch was assigned to, with size and primary flag.
                    if (debugEnabled) {
                        std::cout << "[mp] alloc kind=batch connId=" << conn->connId
                                  << " sessionId=" << sessionId << " stream=" << sid
                                  << " files=" << batchPaths.size() << " bytes=" << batchBytes
                                  << " primary=" << (conn->isPrimary ? 1 : 0) << std::endl;
                    }
                    started = true;
                }
            } else if (regularQueue != nullptr) {
                // Reserve one slot for batch work when batch backlog exists, mirroring the
                // single-link behavior but against the pool-wide cap.
                const size_t reservedBatchSlots = (globalSlotCap > 1 && hasBatchBacklog()) ? 1 : 0;
                if (activeDownloads.size() >= (globalSlotCap - reservedBatchSlots)) {
                    break;
                }
                const std::string rel = regularQueue->front();
                // Route by size: files >= largeFileThreshold may be pinned to the primary link
                // (legacy FR-012); others go to the best-weighted healthy lane (FR-013).
                const auto itMeta = remoteFiles.find(rel);
                const bool isLarge = (itMeta != remoteFiles.end() &&
                                      itMeta->second.fileSize >= options.largeFileThresholdBytes);
                // Large-file lane policy (aux-weight FR-13~FR-16). largeFilePrefersAux is a
                // PREFERENCE only: when true the large file uses normal weighted selection
                // (forcePrimary=false), letting aux be favored via weight without bypassing
                // candidate filtering or the streamLimit cap (FR-17). It never forces aux, so
                // with no surviving aux it naturally falls back to the primary.
                bool largeFilePrefersAux = false;
                switch (options.largeFileLane) {
                    case LargeFileLane::Primary: largeFilePrefersAux = false; break;
                    case LargeFileLane::Aux:     largeFilePrefersAux = true; break;
                    case LargeFileLane::Auto:    largeFilePrefersAux = (options.auxWeight >= 2.0); break;
                }
                const bool forcePrimary = isLarge && !largeFilePrefersAux;  // FR-16
                ClientConnection* conn = pickConnection(forcePrimary);
                if (conn == nullptr) {
                    break;  // no eligible lane right now; leave file queued and retry later
                }
                regularQueue->pop_front();
                const fs::path abs = JoinRel(options.rootDir, rel);
                EnsureParentDir(abs);
                DownloadState d;
                d.relPath = rel;
                d.output.open(abs, std::ios::binary | std::ios::trunc);
                if (!d.output) {
                    retryOrFail(rel);
                    PrintClientCounters(enumerated, compared, unchanged, failed, transferred, deleted, pool.size(), lastEnum, lastCompared, lastUnchanged, lastFailed, lastTransferred, lastDeleted);
                } else {
                    const uint32_t sid = conn->nextStreamId++;
                    const uint64_t key = streamKey(conn->connId, sid);
                    d.flushThreshold = downloadFlushThreshold;
                    d.writeBuffer.reserve(d.flushThreshold);
                    const uint64_t fileBytes =
                        (itMeta != remoteFiles.end()) ? itMeta->second.fileSize : 0;
                    activeDownloads.emplace(key, std::move(d));
                    streamToPath.emplace(key, rel);
                    try {
                        SendFrame(conn->socket, Frame{MsgType::FileOpen, sid, EncodeFileOpen(rel)});
                    } catch (const std::exception& ex) {
                        // The activeDownloads/streamToPath entry just added carries rel, so
                        // failoverScan() will requeue it once the lane is marked down
                        // (FR-015 / FR-016). Stop dispatching onto this dead lane.
                        markConnDown(conn, ex.what());
                        break;
                    }
                    ++conn->inFlight;
                    // Observable link-task allocation (AC-018 / design §11): which lane a
                    // file was assigned to, incl. large-file primary-pin routing (FR-012).
                    if (debugEnabled) {
                        std::cout << "[mp] alloc kind=file connId=" << conn->connId
                                  << " sessionId=" << sessionId << " stream=" << sid
                                  << " path=" << rel << " bytes=" << fileBytes
                                  << " large=" << (isLarge ? 1 : 0)
                                  << " primary=" << (conn->isPrimary ? 1 : 0) << std::endl;
                    }
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
        PrintClientCounters(enumerated, compared, unchanged, failed, transferred, deleted, pool.size(), lastEnum, lastCompared, lastUnchanged, lastFailed, lastTransferred, lastDeleted);
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
                // Same size, content differs: try block-level delta before full download.
                if (!tryEnterDelta(rel)) {
                    scheduleTransfer(rel);
                }
            } else {
                const FileEntry& meta = remoteFiles.at(rel);
                SetFileModifyTime(JoinRel(options.rootDir, rel), meta.mtimeNs);
                ++compared;
                ++unchanged;
            }
            hashResolved.insert(rel);
            ++fallbackResolved;
            PrintClientCounters(enumerated, compared, unchanged, failed, transferred, deleted, pool.size(), lastEnum, lastCompared, lastUnchanged, lastFailed, lastTransferred, lastDeleted);
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
            PrintClientCounters(enumerated, compared, unchanged, failed, transferred, deleted, pool.size(),
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

    // --- Binary delta (FC7) client orchestration (design §6.1/§6.7) ---
    auto makeDeltaTempPath = [&](const fs::path& target) -> fs::path {
        static std::atomic<uint64_t> ctr{0};
        const uint64_t n = ctr.fetch_add(1, std::memory_order_relaxed);
#ifdef _WIN32
        const unsigned long pid = static_cast<unsigned long>(GetCurrentProcessId());
#else
        const unsigned long pid = static_cast<unsigned long>(getpid());
#endif
        fs::path p = target;
        p += ".fcdelta." + std::to_string(pid) + "." + std::to_string(n) + ".tmp";
        return p;
    };

    // Reconstruction complete: verify the temp file against the manifest XXH3-128 (FR-23) and
    // either atomic-rename it into place (NFR-05) or discard + abandon delta + full fallback.
    auto finalizeDelta = [&](const std::string& rel) {
        auto it = deltaStates.find(rel);
        if (it == deltaStates.end()) {
            return;
        }
        DeltaFileState& st = it->second;
        if (st.tempOut.is_open()) {
            st.tempOut.flush();
            st.tempOut.close();
        }
        if (!st.tempPath.empty() && !SyncFileToDisk(st.tempPath)) {
            std::error_code rec;
            fs::remove(st.tempPath, rec);
            deltaFallback(rel, "reconstruct_io", st.newFileBytes, st.newFileBytes);
            deltaAbandoned.insert(rel);
            deltaStates.erase(it);
            scheduleTransfer(rel);
            return;
        }
        const fs::path target = JoinRel(options.rootDir, rel);
        bool verifyOk = false;
        try {
            const Hash256 got = ComputeFileHash(st.tempPath);
            verifyOk = HashEquals(got, st.verifyHash);
        } catch (...) {
            verifyOk = false;
        }
        std::error_code rec;
        if (verifyOk) {
            bool renamed = false;
#ifdef _WIN32
            renamed = (MoveFileExW(st.tempPath.wstring().c_str(), target.wstring().c_str(),
                                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0);
#else
            fs::rename(st.tempPath, target, rec);
            renamed = !rec;
#endif
            if (!renamed) {
                fs::remove(st.tempPath, rec);
                deltaFallback(rel, "reconstruct_io", st.newFileBytes, st.newFileBytes);
                deltaAbandoned.insert(rel);
                deltaStates.erase(it);
                scheduleTransfer(rel);
                return;
            }
            const auto metaIt = remoteFiles.find(rel);
            if (metaIt != remoteFiles.end()) {
                SetFileModifyTime(target, metaIt->second.mtimeNs);
            }
            ++compared;
            ++transferred;
            ++deltaReconstructed;
            transferRetryCounts.erase(rel);
            // Capture before erase(): `st` is a reference into deltaStates and is dangling
            // after the erase, so the log line must not read it post-erase.
            const uint64_t reconstructedBytes = st.newFileBytes;
            deltaStates.erase(it);
            if (debugEnabled) {
                std::cout << "[delta] reconstructed rel=" << rel << " sessionId=" << sessionId
                          << " bytes=" << reconstructedBytes << std::endl;
            }
        } else {
            fs::remove(st.tempPath, rec);
            deltaFallback(rel, "verify_fail", st.newFileBytes, st.newFileBytes);  // FR-24 / AC-08
            deltaAbandoned.insert(rel);
            deltaStates.erase(it);
            scheduleTransfer(rel);
        }
        PrintClientCounters(enumerated, compared, unchanged, failed, transferred, deleted, pool.size(),
                            lastEnum, lastCompared, lastUnchanged, lastFailed, lastTransferred, lastDeleted);
    };

    // Drive a BlockSigResponse: build the reconstruction plan over the local old file, apply
    // the benefit gate (FR-17), pre-write all copy ops to a temp file, and enqueue miss ranges
    // (sliced for multi-lane parallelism, §6.6). 100%-match files finalize immediately.
    auto beginDeltaReconstruct = [&](const std::string& rel, const Hash256& fileHash,
                                     const delta::SignatureSet& sig) {
        auto itState = deltaStates.find(rel);
        if (itState == deltaStates.end()) {
            return;  // abandoned before the response arrived
        }
        DeltaFileState& st = itState->second;
        if (st.sigReceived) {
            return;  // duplicate response (failover re-request); ignore
        }
        st.sigReceived = true;
        st.verifyHash = fileHash;
        st.newFileBytes = sig.fileSize;

        const fs::path abs = JoinRel(options.rootDir, rel);
        std::vector<uint8_t> oldData;
        if (!ReadWholeFile(abs, oldData)) {
            deltaFallback(rel, "old_unreadable", 0, sig.fileSize);
            deltaAbandoned.insert(rel);
            deltaStates.erase(itState);
            scheduleTransfer(rel);
            return;
        }
        const delta::DeltaPlan plan = delta::BuildPlan(sig, oldData.data(), oldData.size());
        if (delta::BenefitRejected(plan.downloadBytes, plan.newFileBytes)) {
            deltaFallback(rel, "benefit", plan.downloadBytes, plan.newFileBytes, &plan.stats);  // FR-19 / AC-07
            deltaAbandoned.insert(rel);
            deltaStates.erase(itState);
            scheduleTransfer(rel);
            return;
        }
        EnsureParentDir(abs);
        const fs::path tmp = makeDeltaTempPath(abs);
        std::ofstream out(tmp, std::ios::binary | std::ios::out | std::ios::trunc);
        bool ioOk = static_cast<bool>(out);
        if (ioOk && plan.newFileBytes > 0) {
            // Preallocate to the final size so out-of-order range writes land correctly.
            out.seekp(static_cast<std::streamoff>(plan.newFileBytes - 1), std::ios::beg);
            out.put('\0');
            ioOk = static_cast<bool>(out);
        }
        for (const delta::CopyOp& c : plan.copies) {
            if (!ioOk) {
                break;
            }
            out.seekp(static_cast<std::streamoff>(c.destOffsetNew), std::ios::beg);
            out.write(reinterpret_cast<const char*>(oldData.data() + c.srcOffsetOld),
                      static_cast<std::streamsize>(c.len));
            ioOk = static_cast<bool>(out);
        }
        if (!ioOk) {
            if (out.is_open()) {
                out.close();
            }
            std::error_code rec;
            fs::remove(tmp, rec);
            deltaFallback(rel, "reconstruct_io", plan.downloadBytes, plan.newFileBytes);
            deltaAbandoned.insert(rel);
            deltaStates.erase(itState);
            scheduleTransfer(rel);
            return;
        }
        st.tempPath = tmp;
        st.tempOut = std::move(out);

        // Slice large misses so multiple lanes can fetch one file's regions in parallel.
        const uint64_t sliceLen = std::min<uint64_t>(
            static_cast<uint64_t>(effectiveChunkSize) * 8, 0xFFFFFFFFull);
        uint32_t rangeCount = 0;
        for (const delta::MissOp& m : plan.misses) {
            uint64_t off = m.destOffsetNew;
            uint64_t remaining = m.len;
            while (remaining > 0) {
                const uint32_t take = static_cast<uint32_t>(std::min<uint64_t>(remaining, sliceLen));
                pendingDeltaRanges.push_back(DeltaRangeTask{rel, off, take});
                off += take;
                remaining -= take;
                ++rangeCount;
            }
        }
        st.pendingRanges = rangeCount;
        if (debugEnabled) {
            std::cout << "[delta] plan rel=" << rel << " sessionId=" << sessionId
                      << " newBytes=" << plan.newFileBytes << " downloadBytes=" << plan.downloadBytes
                      << " copies=" << plan.copies.size() << " ranges=" << rangeCount
                      << " scanned_bytes=" << plan.stats.scannedBytes
                      << " matched_bytes=" << plan.stats.matchedBytes
                      << " strong_computes=" << plan.stats.strongComputations
                      << " weak_hits=" << plan.stats.weakCandidateHits
                      << " early_stopped=" << (plan.stats.earlyStopped ? 1 : 0) << std::endl;
        }
        if (rangeCount == 0) {
            finalizeDelta(rel);  // 100% match: copies already cover the whole file (AC-03)
        }
    };

    // Send queued BlockSigRequests on the control lane (binary-delta §6.1). On send failure
    // the lane is marked down and the requests are requeued for a healthy control lane.
    auto pumpDeltaSignatures = [&]() {
        if (pendingDeltaSigRequests.empty()) {
            return;
        }
        ClientConnection* ctrl = controlConn();
        std::vector<Frame> frames;
        std::vector<std::string> batch;
        while (!pendingDeltaSigRequests.empty()) {
            std::string rel = pendingDeltaSigRequests.front();
            pendingDeltaSigRequests.pop_front();
            if (!deltaStates.contains(rel)) {
                continue;
            }
            frames.push_back(Frame{MsgType::BlockSigRequest, 0, EncodeBlockSigRequest(rel)});
            batch.push_back(std::move(rel));
        }
        if (frames.empty()) {
            return;
        }
        try {
            SendFrameBatch(ctrl->socket, frames);
            for (const std::string& r : batch) {
                deltaSigRequested.insert(r);
            }
        } catch (const std::exception& ex) {
            markConnDown(ctrl, ex.what());
            for (auto rit = batch.rbegin(); rit != batch.rend(); ++rit) {
                pendingDeltaSigRequests.push_front(*rit);
            }
        }
    };

    // Assign queued miss ranges to healthy lanes (reuses pickConnection weighting; AC-15). The
    // per-lane streamLimit bounds fan-out (pickConnection returns null when all are saturated).
    auto tryStartDeltaRanges = [&]() {
        while (!pendingDeltaRanges.empty()) {
            const DeltaRangeTask& peek = pendingDeltaRanges.front();
            if (!deltaStates.contains(peek.rel)) {
                pendingDeltaRanges.pop_front();  // file abandoned by a sibling range
                continue;
            }
            ClientConnection* conn = pickConnection(false);
            if (conn == nullptr) {
                break;  // all lanes saturated; retry next pass
            }
            DeltaRangeTask task = pendingDeltaRanges.front();
            pendingDeltaRanges.pop_front();
            const uint32_t sid = conn->nextStreamId++;
            const uint64_t k = streamKey(conn->connId, sid);
            DeltaRangeRequest req;
            req.relPath = task.rel;
            req.offset = task.offset;
            req.length = task.length;
            try {
                SendFrame(conn->socket, Frame{MsgType::DeltaRangeOpen, sid, EncodeDeltaRangeOpen(req)});
            } catch (const std::exception& ex) {
                markConnDown(conn, ex.what());
                pendingDeltaRanges.push_front(task);  // re-route to a healthy lane
                break;
            }
            activeDeltaRanges.emplace(k, ActiveDeltaRange{task.rel, task.offset, task.length, 0});
            ++conn->inFlight;
            if (debugEnabled) {
                std::cout << "[mp] alloc kind=delta_range connId=" << conn->connId
                          << " sessionId=" << sessionId << " stream=" << sid
                          << " path=" << task.rel << " offset=" << task.offset
                          << " len=" << task.length
                          << " primary=" << (conn->isPrimary ? 1 : 0) << std::endl;
            }
        }
    };

    auto processIncomingFrame = [&](uint32_t connId, Frame& frame) {
        // Resolve the originating lane (connId == pool index; pool never reorders). Used
        // for per-connection (connId,streamId) demux and in-flight accounting.
        ClientConnection* conn = (connId < pool.size()) ? pool[connId].get() : nullptr;
        const uint64_t key = streamKey(connId, frame.streamId);
        auto releaseSlot = [&]() {
            if (conn != nullptr && conn->inFlight > 0) {
                --conn->inFlight;
            }
        };
        // A file frame for an unknown (connId,streamId) is a hard desync on a HEALTHY lane,
        // but during failover the lane's recvThread can have queued chunks that arrive after
        // failoverScan() already erased + requeued that lane's streams (FR-015). Those stale
        // frames must be dropped, not treated as a protocol error that aborts the session
        // (FR-016 / AC-011 / AC-012).
        auto staleFromDeadLane = [&]() -> bool {
            return conn == nullptr || !conn->healthy.load();
        };
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
            auto itBatch = activeBatchDownloads.find(key);
            if (itBatch == activeBatchDownloads.end()) {
                if (staleFromDeadLane()) { return; }
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
            auto itBatch = activeBatchDownloads.find(key);
            if (itBatch == activeBatchDownloads.end()) {
                if (staleFromDeadLane()) { return; }
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
            auto itBatch = activeBatchDownloads.find(key);
            if (itBatch == activeBatchDownloads.end()) {
                if (staleFromDeadLane()) { return; }
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
            streamToPath.erase(key);
            releaseSlot();
        } else if (frame.type == MsgType::FileChunk) {
            auto it = activeDownloads.find(key);
            if (it == activeDownloads.end()) {
                if (staleFromDeadLane()) { return; }
                throw std::runtime_error("Received chunk for unknown stream");
            }
            DownloadState& d = it->second;
            d.writeBuffer.insert(d.writeBuffer.end(), frame.payload.begin(), frame.payload.end());
            if (d.writeBuffer.size() >= d.flushThreshold) {
                flushBufferedWrites(d);
            }
        } else if (frame.type == MsgType::FileEnd) {
            auto it = activeDownloads.find(key);
            if (it == activeDownloads.end()) {
                if (staleFromDeadLane()) { return; }
                throw std::runtime_error("Received end for unknown stream");
            }
            flushBufferedWrites(it->second);
            it->second.output.flush();
            it->second.output.close();
            const std::string rel = it->second.relPath;
            const FileEntry& meta = remoteFiles.at(rel);
            SetFileModifyTime(JoinRel(options.rootDir, rel), meta.mtimeNs);
            activeDownloads.erase(it);
            streamToPath.erase(key);
            releaseSlot();
            ++compared;
            ++transferred;
            transferRetryCounts.erase(rel);
            PrintClientCounters(enumerated, compared, unchanged, failed, transferred, deleted, pool.size(), lastEnum, lastCompared, lastUnchanged, lastFailed, lastTransferred, lastDeleted);
        } else if (frame.type == MsgType::FileError) {
            auto itBatch = activeBatchDownloads.find(key);
            if (itBatch != activeBatchDownloads.end()) {
                for (auto& entry : itBatch->second.entries) {
                    if (!entry.finalized) {
                        entry.shouldWrite = false;
                        finalizeBatchEntry(entry);
                    }
                }
                activeBatchDownloads.erase(itBatch);
                streamToPath.erase(key);
                releaseSlot();
                return;
            }
            auto itPath = streamToPath.find(key);
            auto itDl = activeDownloads.find(key);
            if (itDl != activeDownloads.end()) {
                itDl->second.writeBuffer.clear();
                itDl->second.output.close();
                activeDownloads.erase(itDl);
            }
            releaseSlot();
            std::string relPath;
            bool hasRelPath = false;
            if (itPath != streamToPath.end()) {
                relPath = itPath->second;
                hasRelPath = true;
                streamToPath.erase(itPath);
            }
            if (hasRelPath && !relPath.empty()) {
                retryOrFail(relPath);
            } else {
                ++compared;
                ++failed;
                if (debugEnabled) {
                    std::cerr << "[debug][client] transfer_failed path=<unknown> stream=" << frame.streamId << std::endl;
                }
            }
            PrintClientCounters(enumerated, compared, unchanged, failed, transferred, deleted, pool.size(), lastEnum, lastCompared, lastUnchanged, lastFailed, lastTransferred, lastDeleted);
        } else if (frame.type == MsgType::BlockSigResponse) {
            BlockSigResponseInfo info = DecodeBlockSigResponse(frame.payload);
            beginDeltaReconstruct(info.relPath, info.fileHash, info.sig);
        } else if (frame.type == MsgType::DeltaRangeChunk) {
            auto itR = activeDeltaRanges.find(key);
            if (itR == activeDeltaRanges.end()) {
                if (staleFromDeadLane()) { return; }
                throw std::runtime_error("Received delta range chunk for unknown stream");
            }
            ActiveDeltaRange& r = itR->second;
            auto itS = deltaStates.find(r.rel);
            if (itS != deltaStates.end() && itS->second.tempOut.is_open() && !frame.payload.empty()) {
                itS->second.tempOut.seekp(static_cast<std::streamoff>(r.destOffset + r.received), std::ios::beg);
                itS->second.tempOut.write(reinterpret_cast<const char*>(frame.payload.data()),
                                          static_cast<std::streamsize>(frame.payload.size()));
            }
            r.received += static_cast<uint32_t>(frame.payload.size());
        } else if (frame.type == MsgType::DeltaRangeEnd) {
            auto itR = activeDeltaRanges.find(key);
            if (itR == activeDeltaRanges.end()) {
                if (staleFromDeadLane()) { return; }
                throw std::runtime_error("Received delta range end for unknown stream");
            }
            const std::string rel = itR->second.rel;
            activeDeltaRanges.erase(itR);
            releaseSlot();
            auto itS = deltaStates.find(rel);
            if (itS != deltaStates.end()) {
                if (itS->second.pendingRanges > 0) {
                    --itS->second.pendingRanges;
                }
                if (itS->second.pendingRanges == 0) {
                    finalizeDelta(rel);
                }
            }
        } else if (frame.type == MsgType::DeltaError) {
            // Signature generation or range read failed server-side -> abandon delta and fall
            // back to a single full download (FR-25). The frame may be range-tagged (streamId)
            // or sig-level (streamId 0); resolve rel from either source.
            std::string rel = DecodeDeltaError(frame.payload);
            auto itR = activeDeltaRanges.find(key);
            if (itR != activeDeltaRanges.end()) {
                rel = itR->second.rel;
                activeDeltaRanges.erase(itR);
                releaseSlot();
            }
            auto itS = deltaStates.find(rel);
            const bool wasManaged = (itS != deltaStates.end()) || deltaSigRequested.contains(rel);
            if (itS != deltaStates.end()) {
                if (itS->second.tempOut.is_open()) {
                    itS->second.tempOut.close();
                }
                std::error_code rec;
                if (!itS->second.tempPath.empty()) {
                    fs::remove(itS->second.tempPath, rec);
                }
                deltaStates.erase(itS);
            }
            if (wasManaged && !deltaAbandoned.contains(rel)) {
                deltaAbandoned.insert(rel);
                deltaFallback(rel, "sig_error", 0, 0);
                scheduleTransfer(rel);
            }
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

    // Per-connection failover (design §10). A lane whose recvThread failed is drained:
    // its in-flight files are requeued to healthy lanes (FR-015), reusing the existing
    // retry budget (FR-019). Only when EVERY lane is down do we set recvClosed to trigger
    // the session-level reconnect path (FR-016 / AC-012).
    auto failoverScan = [&]() {
        bool newlyDown = false;
        for (auto& cptr : pool) {
            ClientConnection* c = cptr.get();
            if (c->healthy.load() || c->drained) {
                continue;
            }
            const uint32_t connId = c->connId;
            newlyDown = true;
            size_t requeued = 0;
            // Regular downloads on this lane.
            std::vector<uint64_t> regularKeys;
            for (auto& kv : activeDownloads) {
                if (static_cast<uint32_t>(kv.first >> 32) == connId) {
                    regularKeys.push_back(kv.first);
                }
            }
            for (uint64_t k : regularKeys) {
                auto it = activeDownloads.find(k);
                if (it != activeDownloads.end()) {
                    const std::string rel = it->second.relPath;
                    if (it->second.output.is_open()) {
                        it->second.output.close();
                    }
                    activeDownloads.erase(it);
                    streamToPath.erase(k);
                    if (!rel.empty()) {
                        retryOrFail(rel);
                        ++requeued;
                    }
                }
            }
            // Batch downloads on this lane: requeue not-yet-finalized entries.
            std::vector<uint64_t> batchKeys;
            for (auto& kv : activeBatchDownloads) {
                if (static_cast<uint32_t>(kv.first >> 32) == connId) {
                    batchKeys.push_back(kv.first);
                }
            }
            for (uint64_t k : batchKeys) {
                auto it = activeBatchDownloads.find(k);
                if (it != activeBatchDownloads.end()) {
                    for (auto& entry : it->second.entries) {
                        if (!entry.finalized) {
                            if (entry.output.is_open()) {
                                entry.output.close();
                            }
                            retryOrFail(entry.relPath);
                            ++requeued;
                        }
                    }
                    activeBatchDownloads.erase(it);
                    streamToPath.erase(k);
                }
            }
            // Binary delta (FC7): requeue this lane's in-flight ranges. Each range carries its
            // own offset/length and writes at a fixed dest offset, so re-downloading on a
            // healthy lane is idempotent (pendingRanges is only decremented on DeltaRangeEnd,
            // so the deltaState's outstanding count stays correct across the re-route).
            std::vector<uint64_t> rangeKeys;
            for (auto& kv : activeDeltaRanges) {
                if (static_cast<uint32_t>(kv.first >> 32) == connId) {
                    rangeKeys.push_back(kv.first);
                }
            }
            for (uint64_t k : rangeKeys) {
                auto it = activeDeltaRanges.find(k);
                if (it != activeDeltaRanges.end()) {
                    if (deltaStates.contains(it->second.rel)) {
                        pendingDeltaRanges.push_back(
                            DeltaRangeTask{it->second.rel, it->second.destOffset, it->second.length});
                        ++requeued;
                    }
                    activeDeltaRanges.erase(it);
                }
            }
            c->inFlight = 0;
            c->drained = true;
            ShutdownBoth(c->socket);
            if (debugEnabled) {
                std::cout << "[mp] conn_down connId=" << connId << " reason=\"" << c->downReason
                          << "\" requeued_files=" << requeued << std::endl;
            }
        }
        // Binary delta (FC7): a BlockSigResponse is lost when its control lane dies. If a lane
        // just went down and a healthy lane remains, re-queue the still-unanswered signature
        // requests; beginDeltaReconstruct ignores duplicate responses (sigReceived guard), so
        // re-requesting is safe and prevents a stuck deltaState from hanging the sync.
        if (newlyDown && healthyConnCount() > 0) {
            for (auto& kv : deltaStates) {
                if (!kv.second.sigReceived && deltaSigRequested.contains(kv.first)) {
                    deltaSigRequested.erase(kv.first);
                    pendingDeltaSigRequests.push_back(kv.first);
                }
            }
        }
        if (healthyConnCount() == 0 && !recvClosed.load()) {
            // All lanes dead: hand off to the session-level reconnect path.
            for (auto& cptr : pool) {
                if (!cptr->downReason.empty()) {
                    recvError = cptr->downReason;
                    break;
                }
            }
            if (recvError.empty()) {
                recvError = "all connections closed";
            }
            recvClosed.store(true);
        }
    };

    try {
        auto lastDebugPrint = std::chrono::steady_clock::now();
        while (true) {
            bool loopHadForwardProgress = false;
            failoverScan();
            resolveFallbackIfReady();
            dispatchHashRequests();
            refreshSmallBatchTuning();
            rebalanceTransfersTowardBatch();
            tryStartTransfers();
            pumpDeltaSignatures();
            tryStartDeltaRanges();
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
                    PrintClientCounters(enumerated, compared, unchanged, failed, transferred, deleted, pool.size(),
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
            const bool deltaIdle = deltaStates.empty() && pendingDeltaSigRequests.empty() &&
                                   pendingDeltaRanges.empty() && activeDeltaRanges.empty();
            if (manifestDone && allCompareDone &&
                pendingTransfers.empty() && pendingBatchTransfers.empty() &&
                pendingRetryTransfers.empty() && pendingRetryBatchTransfers.empty() &&
                activeDownloads.empty() && activeBatchDownloads.empty() && allHashDone &&
                ioOutstanding == 0 && deltaIdle) {
                break;
            }
            const bool needNetworkFrame = !manifestDone || !activeDownloads.empty() || !activeBatchDownloads.empty() ||
                                          (hashResponsesReceived < hashRequestsSent) || !deltaIdle;
            sweepUnresolvedFallbackIfQuiescent();
            // recvClosed means EVERY lane is down (failoverScan). When a send-side failure
            // requeued the last lane's work, there may be no in-flight network state left, so
            // needNetworkFrame is false even though the session is gone. Don't spin here: fall
            // through to the drain path, which observes recvClosed and breaks into the
            // incomplete-manifest gate (FR-016 / AC-012, IT-3 exit 3).
            if (!needNetworkFrame && !recvClosed.load()) {
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

            std::deque<IncomingFrame> readyFrames;
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
                    IncomingFrame frame = std::move(incomingPriorityFrames.front());
                    incomingPriorityFrames.pop_front();
                    const uint64_t sz = frameWireBytes(frame.frame);
                    incomingQueuedBytes = (incomingQueuedBytes >= sz) ? (incomingQueuedBytes - sz) : 0;
                    readyFrames.push_back(std::move(frame));
                }
                // While ingest is paused, only priority frames (file/hash) are pulled;
                // manifest stays buffered so the thread can drain the compare backlog.
                if (!ingestPaused) {
                    const size_t remaining = kDrainBudget - readyFrames.size();
                    const size_t manifestCount = std::min<size_t>(incomingManifestFrames.size(), remaining);
                    for (size_t i = 0; i < manifestCount; ++i) {
                        IncomingFrame frame = std::move(incomingManifestFrames.front());
                        incomingManifestFrames.pop_front();
                        const uint64_t sz = frameWireBytes(frame.frame);
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
            for (auto& rf : readyFrames) {
                processIncomingFrame(rf.connId, rf.frame);
            }
            // Single lock + notify_all for the whole drained batch (see comment at
            // compareDispatchBuffer): replaces the former per-entry lock/notify.
            flushCompareDispatch();
            if (!readyFrames.empty()) {
                loopHadForwardProgress = true;
                // Manifest hot path no longer prints per entry; emit one throttled
                // progress line per drained batch instead.
                PrintClientCounters(enumerated, compared, unchanged, failed, transferred, deleted, pool.size(),
                                    lastEnum, lastCompared, lastUnchanged, lastFailed, lastTransferred, lastDeleted);
            }
            updateStallWatchdog(loopHadForwardProgress);
        }
    } catch (...) {
        recvStop.store(true);
        incomingDataCv.notify_all();
        for (auto& cptr : pool) {
            ShutdownBoth(cptr->socket);
        }
        compareStop.store(true);
        compareTaskCv.notify_all();
        hashStop.store(true);
        hashTaskCv.notify_all();
        dirStop.store(true);
        dirTaskCv.notify_all();
        ioStop.store(true);
        ioTaskCv.notify_all();
        for (auto& cptr : pool) {
            JoinDiag(cptr->recvThread, "client-catch-recv");
        }
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
        for (auto& cptr : pool) {
            ShutdownBoth(cptr->socket);
        }
        for (auto& cptr : pool) {
            JoinDiag(cptr->recvThread, "client-recv-incomplete");
        }
        std::cout << "Sync aborted (incomplete manifest). changed_files=" << transferred
                  << " failed_files=" << failed << " enumerated=" << enumerated
                  << " elapsed=" << formatElapsed() << std::endl;

        const std::string disconnectReason =
            recvError.empty() ? "connection_closed" : recvError;
        if (const std::optional<int> exitCode = ScheduleClientReconnectOrExit(
                disconnectReason, options.reconnectRetries, options.reconnectWindowMs,
                reconnectAttemptsUsed, reconnectWindowStart, /*exitWhenDisabled=*/kExitIncompleteNoReconnect)) {
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
    PrintClientCounters(enumerated, compared, unchanged, failed, transferred, deleted, pool.size(), lastEnum, lastCompared, lastUnchanged, lastFailed, lastTransferred, lastDeleted, true);
    // SyncDone is sent on every live lane so each per-connection server session (D-02)
    // terminates cleanly; auxiliary lanes that already failed are skipped.
    for (auto& cptr : pool) {
        if (cptr->healthy.load()) {
            try {
                SendFrame(cptr->socket, Frame{MsgType::SyncDone, 0, {}});
            } catch (...) {
                // A lane that died between the last check and here is harmless at teardown.
            }
        }
    }
    recvStop.store(true);
    incomingDataCv.notify_all();
    for (auto& cptr : pool) {
        ShutdownBoth(cptr->socket);
    }
    for (auto& cptr : pool) {
        JoinDiag(cptr->recvThread, "client-recv-final");
    }
    const bool success = (failed == 0);
    std::cout << "Sync completed. changed_files=" << transferred
              << " failed_files=" << failed
              << " elapsed=" << formatElapsed() << std::endl;
    return success ? kExitOk : kExitFailedFiles;
    }  // while (reconnect session)
}

}  // namespace fc
