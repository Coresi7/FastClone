#include "sync_engine_internal.h"

#include "read_gate.h"

namespace fs = std::filesystem;

namespace fc {

using namespace detail;

namespace detail {
// Read an entire regular file into memory (binary delta server-side signature generation /
// design section 6.3 "sequentially read the whole file"). Returns false on any open/read failure.
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
}  // namespace detail

namespace {

struct ServerStream {
    std::ifstream input;
    std::string relativePath;
    std::vector<uint8_t> readBuffer;  // A1: reused read buffer, lazily sized to effectiveChunkSize
};

// Binary delta (FC7) server-side byte-range stream. Opened on DeltaRangeOpen through the unified
// disk IO driver (unified-disk-io-driver C10 / design section 5.6); `remaining` bytes starting at
// `nextReadOffset` are streamed back as DeltaRangeChunk frames followed by a single DeltaRangeEnd.
// Owned exclusively by the send loop (no I/O under any lock); the driver handle is closed on
// completion/error and on session teardown.
struct ServerRangeStream {
    std::string relativePath;
    uint64_t remaining = 0;
    bool errored = false;      // open/read failure -> emit DeltaError instead of DeltaRangeEnd
    uint64_t fileId = 0;       // driver file handle (0 = none)
    uint64_t nextReadOffset = 0;  // next file offset to read from
};

struct ServerBatchStream {
    std::vector<BatchFileRecord> files;
    size_t index = 0;
    bool headerSent = false;
    std::ifstream input;
    uint64_t remainingBytes = 0;
    std::vector<uint8_t> readBuffer;  // A1: reused read buffer, lazily sized to effectiveChunkSize
};

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

// Session type (fastcheck). Sync=the existing mirror sync session (default, zero impact on the existing path); Check=FastCheck
// read-only comparison session, serving only manifest and hash requests, not entering transfer-wait semantics.
enum class SessionType : uint8_t { Sync = 0, Check = 1 };

// Server-side logical session shared by all connections that carry the same sessionId
// (FR-003/004). Per D-02 it only carries merge identity + lifecycle, not transfer state.
struct ServerSession {
    std::string sessionId;
    // fastcheck: session type. Default Sync, only the CheckAuth branch sets Check. The existing Sync path does not read this field.
    SessionType type = SessionType::Sync;
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

// Process-wide registry: sessionId -> session, with idle TTL reclaim (design section 5.1/section 5.3).
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
    // counts as 1, design section 6.1 / FR-08) and the cumulative count of created real sessions.
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
    if (claim.type == MsgType::CheckAuth) {
        // fastcheck: read-only comparison session claim. Authentication is the same as Auth, but the parsed session is
        // marked Check and does not claim the --once target (D-04: Check is read-only, not equivalent to one sync). Does not
        // advertise multipath endpoints and does not set kCapDelta (Check does no delta).
        const CheckAuthInfo info = DecodeCheckAuth(claim.payload);
        if (info.password != password) {
            SendSimple(socket, MsgType::AuthFail, "bad password");
            throw std::runtime_error("Authentication failed");
        }
        std::shared_ptr<ServerSession> session = GetSessionRegistry().CreateSession();
        session->type = SessionType::Check;
        AuthOkInfo ok;
        ok.role = AuthOkRole::NewSession;
        ok.sessionId = session->sessionId;
        try {
            SendFrame(socket, Frame{MsgType::AuthOk, 0, EncodeAuthOk(ok)});
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
    PendingDir rootPending{rootW, std::string()};
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
                // Change 1 (fastcheck-perf-tune, FR-04): reuse the size the directory_iterator already
                // cached for this entry instead of a second independent stat on absPath. Error path is
                // byte-identical to the former fs::file_size(absPath, ec): on failure skip the entry.
                entry.fileSize = static_cast<uint64_t>(it->file_size(ec));
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
            // Change 1 (fastcheck-perf-tune, FR-05/FR-06): reuse the directory_entry's cached mtime;
            // the ToUnixNs conversion, unit, sign and error-to-0 handling are unchanged.
            entry.mtimeNs = ToUnixNs(it->last_write_time(ec));
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
    PendingDir rootPending{root, std::string()};
#endif

    // C-1: processDir is byte-identical on both platforms, so it (and the walk that
    // consumes it) is defined once here after #endif; each branch above only builds its
    // platform-typed listOneDir and the commonly named rootPending. This still runs
    // before the common flush tail, so the manifest frame sequence is unchanged.
    auto processDir = [&](const PendingDir& current, std::vector<PendingDir>& subdirs,
                          std::vector<Frame>& out) {
        runListing(listOneDir, current, subdirs, out);
    };
    const unsigned numWorkers = ResolveDirWalkWorkerCount();
    ParallelDirWalk(std::move(rootPending), numWorkers, kDirPopBatch, done,
                    "server-enum-walk", std::vector<Frame>{}, processDir, finishWorker);

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

// Read-concurrency limit for streaming BlockSigRequest miss tasks (delta-streaming-fix M7/FR-14,
// in [2,4]). Orthogonal to the hash-pool worker count (FR-15/D6): it bounds how many tasks read
// a file from disk at once, independently of how many workers exist.
constexpr uint32_t kServerReadLimit = 3;

// Streaming read chunk for the signature/hash single pass (FR-03, in [1,4] MiB). 1 MiB keeps the
// per-task resident buffer minimal (readLimit * 1 MiB total), the "stop the IO bleed" objective.
constexpr size_t kServerSigChunkBytes = 1u << 20;

// Global read gate shared across all sessions (whole-machine disk-read concurrency, NFR-02),
// mirroring the GetServerHashPool() singleton pattern.
ReadGate& GetServerReadGate() {
    static ReadGate gate(kServerReadLimit);
    return gate;
}

// Read-ahead window (in-flight ops) each server SequentialReader keeps over its file. Total disk
// read concurrency is bounded by the driver's maxInFlight, not per-reader (design section 5.2/FR-20).
constexpr uint32_t kServerReadAhead = 4;

// Process-wide unified disk IO driver for the server (unified-disk-io-driver C9/C10): the single
// locus for signature/hash miss reads and delta range reads, replacing per-path inline file reads
// and the ReadGate concurrency cap (design section 3.1/section 5.2, FR-20). Mirrors the GetServerHashPool()
// singleton lifetime (never torn down; process-scoped).
fc::io::DiskIoDriver& GetServerDiskIoDriver() {
    static fc::io::IoDriverConfig cfg = [] {
        fc::io::IoDriverConfig c;
        return c;
    }();
    static fc::io::DiskIoDriver driver(cfg);
    return driver;
}

// Read up to `want` bytes at `offset` from an already-open driver file, blocking for that single
// op (the same blocking granularity as the former synchronous ifstream read in the send loop, so
// send-loop responsiveness is unchanged). Returns bytes read; 0 with err=true on a hard error.
uint32_t DriverReadRangeChunk(uint64_t fileId, uint64_t offset, uint32_t want,
                              std::vector<uint8_t>& out, bool& err) {
    out.clear();
    fc::io::DiskIoDriver& drv = GetServerDiskIoDriver();
    std::vector<fc::io::IoRequest> batch(1);
    batch[0].kind = fc::io::OpKind::Read;
    batch[0].fileId = fileId;
    batch[0].offset = offset;
    batch[0].length = want;
    batch[0].prio = fc::io::Prio::Large;
    batch[0].userTag = offset;
    for (int tries = 0; !batch.empty(); ++tries) {
        if (drv.submit(batch) > 0) {
            break;
        }
        if (tries > 5000) {  // ~5s of a persistently full/cancelled queue -> give up
            err = true;
            return 0;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::vector<fc::io::IoCompletion> comps;
    for (;;) {
        drv.drainCompletionsForFile(fileId, comps);
        if (!comps.empty()) {
            break;
        }
        drv.waitForFile(fileId, 1000);
        drv.drainCompletionsForFile(fileId, comps);
        if (!comps.empty()) {
            break;
        }
    }
    fc::io::IoCompletion& c = comps.front();
    if (c.status == fc::io::IoStatus::Error) {
        err = true;
        return 0;
    }
    out = std::move(c.data);
    return static_cast<uint32_t>(out.size());
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

// Per-connection session server. The FC6 handshake + session merge has already been
// performed by the caller (HandshakeAndResolveSession); this body is unchanged from the
// single-connection model and runs independently per connection (D-02).
void RunSessionServer(const SocketHandle& client, const CliOptions& options,
                      SessionType sessionType = SessionType::Sync) {
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
                            // C9: read the file content through the unified driver and stream it
                            // into XXH3, instead of ComputeFileHash's inline read. Same bytes ->
                            // identical Hash256 (AC-06). IO/CPU decoupled; concurrency bounded by
                            // the driver, not per-worker (FR-19). The read+hash logic is the single
                            // shared ComputeFileHashViaDriver (fastcheck-parallel-hash FR-02); any
                            // failure throws and is caught here -> hash.fill(0xFF), byte-for-byte
                            // equivalent to the previous inline block.
                            try {
                                hash = ComputeFileHashViaDriver(GetServerDiskIoDriver(), abs);
                            } catch (...) {
                                hashOk = false;
                            }
                            if (!hashOk) {
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
                } else if (sessionType == SessionType::Check &&
                           (frame.type == MsgType::FileOpen ||
                            frame.type == MsgType::FileBatchOpen ||
                            frame.type == MsgType::BlockSigRequest ||
                            frame.type == MsgType::DeltaRangeOpen)) {
                    // fastcheck: a Check session serves no transfer frames; a well-behaved FastCheck client will not send these frames
                    // (FR-28/AC-36). Log a diagnostic and ignore, do not set failed and do not throw, avoiding a false session-failed verdict.
                    if (debugEnabled) {
                        std::cerr << "[check] ignore transfer frame in Check session type="
                                  << static_cast<int>(static_cast<uint8_t>(frame.type)) << std::endl;
                    }
                    continue;
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
                        std::string whyNotOk;
                        if (!(fs::exists(record.absPath, ec) && !ec)) {
                            whyNotOk = "exists_check_failed:" + ec.message();
                        } else if (!(fs::is_regular_file(record.absPath, ec) && !ec)) {
                            whyNotOk = "not_regular_file:" + ec.message();
                        } else {
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
                                } else {
                                    whyNotOk = "open_for_read_failed";
                                }
                            } else {
                                whyNotOk = "file_size_failed:" + ec.message();
                            }
                        }
                        std::cerr << "[DIAG-BATCH][server] open rel=" << record.relativePath
                                  << " ok=" << (record.ok ? 1 : 0)
                                  << " size=" << record.fileSize
                                  << " reason=" << (record.ok ? "-" : whyNotOk) << std::endl;
                        batch.files.push_back(std::move(record));
                    }
                    {
                        std::lock_guard<std::mutex> lock(mu);
                        pendingNewBatchStreams.emplace_back(frame.streamId, std::move(batch));
                    }
                    outboundCv.notify_one();
                } else if (frame.type == MsgType::BlockSigRequest) {
                    // Binary delta (FC7, design section 6.3). Generate (or memcache-hit) the block
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
                            // Release the session pending-jobs slot on EVERY exit path
                            // (success / failure / exception / cancel), matching the legacy
                            // teardown (FR-17/AC-06/AC-10).
                            auto finalize = [&]() {
                                std::lock_guard<std::mutex> lock(sessionHashMu);
                                if (sessionPendingHashJobs > 0) {
                                    --sessionPendingHashJobs;
                                }
                                sessionHashCv.notify_all();
                            };

                            // If the session is already tearing down, skip the heavy IO for a
                            // response that would be dropped anyway (FR-18/AC-09).
                            if (done.load()) {
                                finalize();
                                return;
                            }

                            // C9 (design section 5.2/FR-20): the file content read now flows through the
                            // unified driver; disk-read concurrency is bounded by the driver
                            // (maxInFlight + read queue + fairness), so the ReadGate permit is no
                            // longer taken on this path. GetServerReadGate()/ReadGate stay defined
                            // (D-08 A: no unrelated deletion). Skip heavy IO if already tearing down.
                            bool ok = true;
                            delta::StreamingResult res;
                            {
                                std::error_code ec;
                                const uint64_t fileSize =
                                    static_cast<uint64_t>(fs::file_size(abs, ec));
                                if (ec) {
                                    ok = false;
                                } else {
                                    // Change 3b (fastcheck-redundant-syscall-elim, FR-21): reuse the
                                    // fileSize just read above so the read open skips the redundant
                                    // Windows FileSizeOnDisk query. Signing bytes are unchanged.
                                    const uint64_t fid = GetServerDiskIoDriver().openFile(
                                        fc::PathToUtf8(abs), fc::io::OpKind::Read, /*unbuffered=*/true,
                                        fileSize);
                                    if (fid == 0) {
                                        ok = false;
                                    } else {
                                        try {
                                            // Single sequential pass: 1 MiB chunks from the driver
                                            // feed the StreamingSigner, producing the SignatureSet
                                            // AND the full-file XXH3-128 without buffering the whole
                                            // file and without a second scan (FR-01/FR-02/NFR-01/
                                            // NFR-03). Bytes are identical to the former ifstream
                                            // pass, so the SignatureSet + fileHash are unchanged.
                                            delta::StreamingSigner signer(fileSize);
                                            fc::io::SequentialReader reader(GetServerDiskIoDriver(),
                                                                            fid, fileSize,
                                                                            kServerSigChunkBytes,
                                                                            kServerReadAhead);
                                            std::vector<uint8_t> cbuf;
                                            for (;;) {
                                                bool okr = true;
                                                cbuf.clear();
                                                const uint32_t n = reader.next(cbuf, okr);
                                                if (!okr) {
                                                    ok = false;  // genuine read error -> DeltaError
                                                    break;
                                                }
                                                if (n == 0) {
                                                    break;
                                                }
                                                signer.Update(cbuf.data(), cbuf.size());
                                            }
                                            if (ok) {
                                                // Byte-count / XXH3 errors throw -> caught below.
                                                res = signer.Finish();
                                            }
                                        } catch (...) {
                                            ok = false;
                                        }
                                        GetServerDiskIoDriver().closeFile(fid);
                                    }
                                }
                            }

                            if (ok && blockSigMemcacheEnabled) {
                                try {
                                    HashFingerprint afterFingerprint;
                                    if (TryReadHashFingerprint(abs, afterFingerprint) &&
                                        (!fingerprintValid ||
                                         (afterFingerprint.fileSize == fingerprint.fileSize &&
                                          afterFingerprint.mtimeNs == fingerprint.mtimeNs))) {
                                        GetBlockSigMemCache().Upsert(rel, afterFingerprint,
                                                                     res.fileHash, res.sig);
                                    }
                                } catch (...) {
                                    // Best-effort cache write; correctness unaffected.
                                }
                            }
                            if (!done.load()) {
                                try {
                                    if (ok) {
                                        enqueueHigh(Frame{MsgType::BlockSigResponse, 0,
                                                          EncodeBlockSigResponse(rel, res.fileHash,
                                                                                 res.sig)});
                                    } else {
                                        enqueueHigh(Frame{MsgType::DeltaError, 0, EncodeDeltaError(rel)});
                                    }
                                } catch (...) {
                                    failed.store(true);
                                    done.store(true);
                                    outboundCv.notify_all();
                                }
                            }
                            finalize();
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
                    // Binary delta (FC7, design section 6.3): C10 routes the range read through the unified
                    // driver (design section 5.6). Open here (handle validation); the send loop streams the
                    // bytes via the driver lock-free. Open failure -> DeltaError.
                    const DeltaRangeRequest req = DecodeDeltaRangeOpen(frame.payload);
                    const fs::path abs = JoinRel(options.rootDir, req.relPath);
                    // Change 3c (fastcheck-redundant-syscall-elim, FR-22 / edge case): a zero-length
                    // or offset+length overflow range must NOT open a driver fileId; enter the
                    // existing DeltaError path instead. Otherwise pass the known read bound
                    // (offset+length) as expectedSize so the read open skips the redundant Windows
                    // FileSizeOnDisk query; the streamed DeltaRangeChunk bytes are unchanged.
                    if (req.length == 0 || req.offset > UINT64_MAX - req.length) {
                        enqueueHigh(Frame{MsgType::DeltaError, frame.streamId, EncodeDeltaError(req.relPath)});
                        continue;
                    }
                    const uint64_t readBound = req.offset + req.length;
                    ServerRangeStream rs;
                    rs.relativePath = req.relPath;
                    rs.remaining = req.length;
                    rs.nextReadOffset = req.offset;
                    rs.fileId = GetServerDiskIoDriver().openFile(fc::PathToUtf8(abs), fc::io::OpKind::Read,
                                                                /*unbuffered=*/true, readBound);
                    if (rs.fileId == 0) {
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
            // fastcheck double safeguard (FR-27/AC-35): for a Check session, after receiving all of manifest+hash, a client
            // FIN or connection close is a clean end. If the exception is an "orderly-close class" one (recv failed WSA=0 / errno=0,
            // same criterion as the dispatch site :2087), set done but not failed, avoiding throwing "Server session failed".
            const std::string what = ex.what();
            const bool orderlyClose =
                what.find("recv failed WSA=0") != std::string::npos ||
                what.find("recv failed errno=0") != std::string::npos;
            if (sessionType == SessionType::Check && orderlyClose) {
                done.store(true);
                sessionHashCv.notify_all();
                outboundCv.notify_all();
            } else {
                failed.store(true);
                done.store(true);
                sessionHashCv.notify_all();
                outboundCv.notify_all();
                errorText = what;
            }
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
                        // A1: reuse per-stream readBuffer instead of allocating a new vector each iteration.
                        if (batch.readBuffer.size() < static_cast<size_t>(toRead)) {
                            batch.readBuffer.resize(static_cast<size_t>(toRead));
                        }
                        batch.input.read(reinterpret_cast<char*>(batch.readBuffer.data()), static_cast<std::streamsize>(toRead));
                        const std::streamsize got = batch.input.gcount();
                        if (got <= 0) {
                            // I/O error mid-file: abort just this batch stream, not the session.
                            sendFrames.push_back(Frame{MsgType::FileError, it->first, {}});
                            batchAborted = true;
                            break;
                        }
                        burstBytes += static_cast<size_t>(got);
                        batchBytesSentThisRound += static_cast<size_t>(got);
                        batch.remainingBytes -= static_cast<uint64_t>(got);
                        sendFrames.push_back(Frame{MsgType::FileBatchChunk, it->first,
                            std::vector<uint8_t>(batch.readBuffer.data(), batch.readBuffer.data() + got)});
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
                        // A1: reuse per-stream readBuffer instead of allocating a new vector each iteration.
                        if (it->second.readBuffer.size() < static_cast<size_t>(effectiveChunkSize)) {
                            it->second.readBuffer.resize(static_cast<size_t>(effectiveChunkSize));
                        }
                        it->second.input.read(reinterpret_cast<char*>(it->second.readBuffer.data()),
                                              static_cast<std::streamsize>(effectiveChunkSize));
                        const std::streamsize got = it->second.input.gcount();
                        if (got > 0) {
                            burstBytes += static_cast<size_t>(got);
                            sendFrames.push_back(Frame{MsgType::FileChunk, it->first,
                                std::vector<uint8_t>(it->second.readBuffer.data(),
                                                     it->second.readBuffer.data() + got)});
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
                    std::vector<uint8_t> chunk;
                    while (rs.remaining > 0 && burstBytes < perStreamBurstBytes) {
                        const uint64_t toRead =
                            std::min<uint64_t>(rs.remaining, static_cast<uint64_t>(effectiveChunkSize));
                        bool readErr = false;
                        const uint32_t got = DriverReadRangeChunk(
                            rs.fileId, rs.nextReadOffset, static_cast<uint32_t>(toRead), chunk,
                            readErr);
                        if (readErr || got == 0) {
                            rs.errored = true;
                            break;
                        }
                        rs.nextReadOffset += got;
                        burstBytes += got;
                        rs.remaining -= static_cast<uint64_t>(got);
                        sendFrames.push_back(
                            Frame{MsgType::DeltaRangeChunk, it->first, std::move(chunk)});
                        chunk.clear();
                        didWork = true;
                    }
                    if (rs.errored) {
                        sendFrames.push_back(Frame{MsgType::DeltaError, it->first,
                                                   EncodeDeltaError(rs.relativePath)});
                        if (rs.fileId != 0) {
                            GetServerDiskIoDriver().closeFile(rs.fileId);
                            rs.fileId = 0;
                        }
                        it = activeRangeStreams.erase(it);
                        didWork = true;
                        continue;
                    }
                    if (rs.remaining == 0) {
                        sendFrames.push_back(Frame{MsgType::DeltaRangeEnd, it->first, {}});
                        if (rs.fileId != 0) {
                            GetServerDiskIoDriver().closeFile(rs.fileId);
                            rs.fileId = 0;
                        }
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
                        return PercentileNearestRank(v, p);
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

    // C10: release any driver file handles still held by in-flight range streams. The send loop
    // closes each on completion/error, but a session that tore down mid-range leaves some open;
    // the process-global driver would otherwise leak these handles across sessions.
    for (auto& kv : activeRangeStreams) {
        if (kv.second.fileId != 0) {
            GetServerDiskIoDriver().closeFile(kv.second.fileId);
            kv.second.fileId = 0;
        }
    }

    if (debugEnabled && failed.load()) {
        // Exception-window context snapshot for post-mortem (AC-B3 / design section B.4):
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
            return PercentileNearestRank(v, p);
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

}  // namespace

namespace {

// --- OneShot server mode (--once) process-wide state (design section 2.3 / section 3.5/section 3.6) ---
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

// --- --once-multi process-wide state (design section 4.3 / section 6.4-6.6) ---
// Sticky failure aggregate: set true the first time any real session ends not-clean and never
// reset (B5 / FR-13). Read once by the main thread when firing the terminal verdict. The
// once-multi exit channel reuses g_onceShouldExit / g_onceExitCode / g_onceTerminalFired (D-04);
// it deliberately does NOT touch g_onceTarget / g_onceMu (those stay --once-only, section 8).
std::atomic<bool> g_omAnyFailure{false};

// --- --wait-connect-timeout in-flight handshake guard (design section 3.7 / NFR-04 / FR-07) ---
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

// Main-thread idle-grace evaluator (design section 6.4). idleSince lives on the RunServer stack and is
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
// wait-connect timeout (design section 3.7 "defer one tick", made finite to fix B-01). A genuine handshake
// latches createdTotal within milliseconds, so a sub-second cap never falsely kills a real
// connection racing the boundary (AC-09 / NFR-04); but it guarantees that a TCP connection which is
// accepted and then never sends any handshake bytes - leaving its handshake thread blocked in recv
// with g_inFlightHandshakes stuck at >0 - can no longer suppress the timeout indefinitely
// (FR-07 / FR-08 / AC-08).
constexpr int kWaitConnectInFlightGraceMs = 1000;

// Main-thread first-connect-wait evaluator (design section 3.3). waitConnectDeadline / firstConnSeen
// live on the RunServer stack and are touched ONLY here, so the same "main-thread single-writer"
// discipline as idle-grace applies (no races, D-01). Returns true iff the wait window has elapsed
// with no valid connection, i.e. the caller must exit with kExitWaitConnectTimeout (FR-08).
//   - firstConnSeen latches true the first time createdTotal>0 (Auth handshake succeeded) and is
//     never reset, so once a valid connection arrives the timer is permanently disabled (FR-09).
//   - Probe connections never increment createdTotal, so they do not stop the timer (FR-07); an
//     in-flight handshake (g_inFlightHandshakes>0) defers the timeout, but only within the BOUNDED
//     grace window [deadline, deadline+kWaitConnectInFlightGraceMs). This avoids killing a real
//     connection at the deadline boundary (NFR-04 / section 3.7) while ensuring an accepted-but-silent
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
        return false;  // genuine handshake may still latch createdTotal (NFR-04 / section 3.7)
    }
    std::cout << "[wc] wait_connect_timeout threshold_ms=" << timeoutMs
              << " no_valid_connection=1" << std::endl;  // FR-13 / AC-13
    return true;
}

}  // namespace

int RunServer(const CliOptions& options) {
    WsaContext wsa;

    // Bind/listen BEFORE the startup banner so a port-in-use failure is the first (and only)
    // output, instead of printing "FastClone server ..." and then appearing to hang/crash.
    // CreateServer uses SO_EXCLUSIVEADDRUSE on Windows / SO_REUSEADDR on POSIX, so an in-use
    // port surfaces as a bind failure here rather than a silent overlapping listen.
    SocketHandle listener;
    try {
        listener = CreateServer(options.port);
    } catch (const std::exception& e) {
        std::cerr << "FastClone: cannot listen on port " << options.port
                  << " (" << e.what() << ")."
                  << " The port may already be in use by another process."
                  << std::endl;
        return kExitListenFailed;
    }
    // Publish the listener fd so a connection-close thread can interrupt accept() on
    // terminal (--once, FR-09). Harmless for non-once: it is only ever read via WakeAcceptLoop.
    g_onceListenSock.store(listener.Get());

    const TunedTransferOptions tuned = ResolveTransferOptions(options);
    const uint32_t hashWorkerCount = ResolveServerHashWorkerCount(options);
    GetServerHashPool().Configure(hashWorkerCount);
    GetServerHashMemCache().Configure(options.enableHashMemcache);
    GetBlockSigMemCache().Configure(options.enableHashMemcache);
    std::cout << "FastClone server root=" << options.rootDir.string() << " port=" << options.port << std::endl;
    std::cout << "[disk-io] backend=" << GetServerDiskIoDriver().backendName() << std::endl;
    std::cout << "[hash-pool] workers=" << hashWorkerCount
              << (options.serverHashWorkers == 0 ? " (auto)" : " (manual)")
              << std::endl;
    std::cout << "[hash-memcache] enabled=" << (options.enableHashMemcache ? 1 : 0) << std::endl;
    if (options.streamAutoTune || options.chunkAutoTune) {
        std::cout << "[auto-tune] streams=" << tuned.streamLimit
                  << " chunk-kb=" << (tuned.chunkSize / 1024)
                  << std::endl;
    }

    // Collect the server's advertised endpoint list once at startup (FR-005 / section 6.1, r6 section 6.1).
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
    for (const AdvertisedEndpoint& ep : serverAddrs) {
        std::cout << "[mp]   " << ep.endpoint << std::endl;
    }

    const bool debugEnabled = IsDebugEnabled();
    std::atomic<uint64_t> connIdCounter{0};
    std::atomic<uint32_t> activeSessions{0};
    // --once-multi accept-loop evaluation tick (design section 6.2). grace is measured by wall clock,
    // so its resolution is +/- one tick (negligible for second-scale grace).
    constexpr int kOnceMultiTickMs = 200;
    // --once-multi idle timer: main-thread-only, so arm/cancel can never race (design section 6.4/section 7).
    std::optional<std::chrono::steady_clock::time_point> idleSince;
    // --wait-connect-timeout (design section 3.3): arm a first-connect deadline for --once / --once-multi.
    // Both timer state slots are main-thread-only (same discipline as idleSince). firstConnSeen
    // latches the moment the first valid connection appears and permanently disables the timer
    // (FR-09); under --once it also reverts the accept loop from per-tick polling back to the
    // original blocking accept so the post-first-connection path is byte-for-byte unchanged (section 3.6).
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
                // Timeout tick: evaluate first-connect-wait BEFORE idle-grace (design section 3.8). Before
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
                // so a just-accepted connection is never immediately judged idle (design section 6.2/R-02).
                EvaluateIdleGrace(options.onceIdleGraceMs, idleSince);
                continue;
            }
            client = std::move(*maybe);
        } else if (waitConnectActive && !firstConnSeen) {
            // --once first-connect wait (design section 3.6): poll with a short tick instead of blocking
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
        // wait-connect in-flight guard (section 3.7): mark this connection's handshake as pending before
        // dispatch; the thread's single exit drops it. Inert when wait-connect is not armed.
        g_inFlightHandshakes.fetch_add(1, std::memory_order_acq_rel);
        std::thread([connSeq, debugEnabled, &activeSessions, options, serverAddrs,
                     client = std::move(client)]() mutable {
            // Surface who connected: the peer's numeric IP via getpeername + NI_NUMERICHOST
            // (metadata only -- no DNS, no socket data consumed, non-blocking). The result is a
            // numeric address literal with no C0/C1 control characters, so it is safe to splice
            // into stdout (no log-injection risk from a peer-controlled string). Printed before
            // the handshake so it appears for every accepted connection, including ones that later
            // fail authentication (then followed by [mp] conn_error below).
            const std::string clientIp = PeerAddressOf(client);
            std::cout << "[mp] client_connected conn=" << connSeq
                      << " ip=" << (clientIp.empty() ? "(unknown)" : clientIp) << std::endl;
            std::shared_ptr<ServerSession> session;
            try {
                session = HandshakeAndResolveSession(client, options.password, serverAddrs,
                                                     options.exitAfterSync);
                std::cout << "[mp] conn_accept conn=" << connSeq
                          << " sessionId=" << session->sessionId
                          << " live_conns=" << session->liveConns.load() << std::endl;
                // fastcheck (D-04): a Check session is a read-only comparison; it does not claim the --once target, does not
                // consume a once slot, and does not change the once verdict (NFR-02). Only Sync sessions participate in ClaimOrMatchOnceTarget / completedOk.
                const bool isCheck = (session->type == SessionType::Check);
                if (options.exitAfterSync && !isCheck && !ClaimOrMatchOnceTarget(session)) {
                    // Fallback guard for a tiny race window: if a second Auth slipped past the
                    // pre-AuthFail check before claimed=true became visible, refuse to serve it.
                    // This path should be rare; normal second Auth is rejected in handshake.
                    std::cerr << "[mp] once_reject_post_auth_fallback conn=" << connSeq
                              << " sessionId=" << session->sessionId << std::endl;
                } else {
                    RunSessionServer(client, options, session->type);
                    std::cout << "[mp] conn_done conn=" << connSeq
                              << " sessionId=" << session->sessionId << std::endl;
                    if (!isCheck && (options.exitAfterSync || options.onceMulti)) {
                        session->completedOk.store(true, std::memory_order_relaxed);  // FR-06/07A; om: D-03
                    }
                }
            } catch (const std::exception& ex) {
                // Distinguish pre-handshake close (reachability probe: client connects
                // then immediately closes before sending any bytes -> recv returns 0,
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
                // fastcheck (D-04/NFR-02): a Check session does not participate in the once verdict and is not folded into the failure aggregation.
                if ((options.exitAfterSync || options.onceMulti) && session &&
                    session->type != SessionType::Check) {
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
            if (options.exitAfterSync && session && session->type != SessionType::Check) {
                bool isTarget;
                {
                    std::lock_guard<std::mutex> lk(g_onceMu);
                    isTarget = (g_onceTarget.lock() == session);
                }
                if (isTarget && remaining == 0) {
                    FireOnceTerminal(session->completedOk.load(std::memory_order_relaxed) &&
                                     !session->hadError.load(std::memory_order_relaxed));
                }
            } else if (options.onceMulti && session && session->type != SessionType::Check) {
                // --once-multi: when a real session's last lane closes, fold its verdict into the
                // sticky failure aggregate (B5/FR-13). The main thread later reads it at terminal.
                // No g_onceTarget involvement (section 8): every real session is served, not just one.
                if (remaining == 0) {
                    const bool ok = session->completedOk.load(std::memory_order_relaxed) &&
                                    !session->hadError.load(std::memory_order_relaxed);
                    if (!ok) {
                        g_omAnyFailure.store(true, std::memory_order_relaxed);
                    }
                }
            }
            activeSessions.fetch_sub(1);
            // wait-connect in-flight guard (section 3.7): single exit for every dispatched connection,
            // covering the clean, error, and pre-handshake-close paths (R-03: no leak -> no stall).
            g_inFlightHandshakes.fetch_sub(1, std::memory_order_acq_rel);
        }).detach();
    }
    return 0;
}

}  // namespace fc
