#include "sync_engine_internal.h"

namespace fs = std::filesystem;

namespace fc {

using namespace detail;

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
                                                           bool debugEnabled,
                                                           long* outProbeMinRttMs = nullptr) {
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
            // Secondary RTT source (design §3.1 / D-01): the minimum reachable connect RTT in
            // the probe matrix is a single-round-trip estimate that refines the primary lane's
            // connect timing. Single-NIC single-server WAN never probes, so this stays unset
            // and the connect-measured RTT is the sole source.
            if (outProbeMinRttMs != nullptr) {
                long probeMin = -1;
                for (const auto& row : matrix.cells) {
                    for (const ReachabilityCell& cell : row) {
                        if (cell.reachable && cell.rttMs >= 0 &&
                            (probeMin < 0 || cell.rttMs < probeMin)) {
                            probeMin = cell.rttMs;
                        }
                    }
                }
                if (probeMin >= 0) {
                    *outProbeMinRttMs = probeMin;
                }
            }
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
    // Measured session RTT (design §3.1, FR-01). Primary source: the primary lane's TCP
    // connect timing (one round trip). Secondary: the probe matrix minimum (when multipath
    // probing runs). 0 == unknown -> treated as LAN so a missing RTT never enables WAN
    // behavior nor aborts the sync (AC-15 / B7).
    long sessionRttMs = 0;
    long probeMinRttMs = -1;
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
        const auto rttConnectStart = std::chrono::steady_clock::now();
        SocketHandle primarySocket = ConnectTo(primaryHost, primaryPort, primaryBinding);
        // TCP connect returns after the SYN/SYN-ACK exchange ~= one round trip, so this is a
        // close application-independent RTT estimate for the primary lane (design D-01).
        sessionRttMs = static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - rttConnectStart).count());
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
            authInfo, debugEnabled, &probeMinRttMs);
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

    // --- WAN small-file second-pass tuning (design §3.1/§3.2/§3.3, FR-01/02/12/13) ---
    // Refine the connect-measured RTT with the probe matrix minimum when available; either
    // source missing degrades to LAN behavior (AC-15 / B7). RTT now drives the stream count,
    // hash in-flight depth and delta-signal depth -- not just route selection (FR-01).
    if (probeMinRttMs >= 0) {
        sessionRttMs = (sessionRttMs > 0) ? std::min<long>(sessionRttMs, probeMinRttMs) : probeMinRttMs;
    }
    const uint32_t wanHwConcurrency = std::max<uint32_t>(1, std::thread::hardware_concurrency());
    const WanTuning wanTune = ResolveWanTuning(tuned, options, sessionRttMs, wanHwConcurrency);
    const bool wanMode = wanTune.wanMode;
    // activeStreamLimit is the live fan-out cap used by lane selection + the global slot cap.
    // It starts at the WAN-resolved count and can be halved by the failure-rate backoff
    // (FR-13). streamLimit (const, base) still drives chunk sizing / batch tuning, so LAN is
    // byte-for-byte unchanged (HC-04).
    uint32_t activeStreamLimit = wanTune.streamLimit;
    const size_t maxInFlightDeltaSig = wanTune.maxInFlightDeltaSig;
    // Soft-reserve pool[0] as the control lane when WAN + >=2 healthy lanes: large/bulk data
    // is biased onto aux lanes so hash/BlockSig traffic is not head-of-line blocked behind a
    // big file on the same TCP (design §3.5, FR-09/10/11). Single-lane WAN relies on the
    // main-loop control-first ordering alone.
    const bool wanControlReserve = wanMode && (pool.size() >= 2);
    // Failure-rate backoff bookkeeping (FR-13 / NFR-04): sampled windows of completions vs
    // failures; on a weak SSD spike the effective stream count is halved (floor 4) so the
    // session self-heals without the user manually降级 (NFR-06 / B8).
    size_t lastBackoffFailed = 0;
    size_t lastBackoffCompleted = 0;
    auto lastBackoffCheck = std::chrono::steady_clock::now();

    if (options.streamAutoTune || options.chunkAutoTune) {
        std::cout << "[auto-tune] streams=" << activeStreamLimit
                  << " chunk-kb=" << (tuned.chunkSize / 1024)
                  << " session_rtt_ms=" << sessionRttMs
                  << " wan_mode=" << (wanMode ? 1 : 0)
                  << " hash_inflight_cap=" << wanTune.maxInFlightHashRequests
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

    // --- BuildPlan offload (design §3.4, FR-07/FR-10) ---
    // The heavy delta reconstruct (old-file read + delta::BuildPlan + temp pre-write) used to
    // run synchronously on the main loop inside BlockSigResponse handling, periodically
    // freezing the control plane on massive small-file sets. It is offloaded to a worker pool
    // (same task-queue + result-deque pattern as the hash workers); the main loop only applies
    // results (state mutation + range enqueue) so the wire protocol, plan inputs/outputs and
    // 落盘 XXH3-128 verification are unchanged (FR-08 / NFR-05).
    struct DeltaPlanTask {
        std::string rel;
        Hash256 fileHash{};
        delta::SignatureSet sig;
    };
    struct DeltaPlanResult {
        std::string rel;
        bool ok = false;             // false => fall back to a full download
        std::string fallbackReason;  // meaningful when !ok
        bool benefitRejected = false;
        uint64_t downloadBytes = 0;
        uint64_t newFileBytes = 0;
        Hash256 verifyHash{};
        std::filesystem::path tempPath;
        std::ofstream tempOut;
        std::vector<DeltaRangeTask> ranges;
        delta::DeltaStats stats{};
    };
    std::deque<DeltaPlanTask> deltaPlanTaskQueue;
    std::mutex deltaPlanTaskMu;
    std::condition_variable deltaPlanTaskCv;
    std::atomic<bool> deltaPlanStop{false};
    std::deque<DeltaPlanResult> deltaPlanResults;
    std::mutex deltaPlanResultMu;
    std::atomic<size_t> deltaPlanInFlight{0};  // dispatched but result not yet applied

    std::unordered_map<std::string, Hash256> remoteHashes;
    std::unordered_map<std::string, Hash256> localHashes;
    std::unordered_set<std::string> hashResolved;
    std::unordered_set<std::string> hashRequested;
    std::deque<std::string> pendingHashRequests;
    // Parallel enqueue timestamps for the control-plane latency P95 (AC-07 / §3.5): pushed
    // alongside pendingHashRequests (single push site) and popped at send (single pop site),
    // so it stays index-aligned with pendingHashRequests.
    std::deque<std::chrono::steady_clock::time_point> pendingHashRequestsAt;
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

    // Unified async disk IO driver (unified-disk-io-driver C7/C8/C10): the single client-side
    // locus for old-file reads (streaming delta plan), download/temp writes, and the finalize
    // verify read. Declared before the worker pools so it outlives every thread that submits to
    // it (all pools are joined before RunClient returns / rethrows). Read/write share one driver
    // so fairness + backpressure apply across the whole client (design §3.2/§5.4).
    fc::io::IoDriverConfig clientIoCfg;
    fc::io::DiskIoDriver clientDriver(clientIoCfg);

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
    // hash in-flight cap: RTT-adaptive on WAN (breaks the legacy 8192 ceiling, FR-02/AC-02),
    // byte-for-byte the legacy clamp on LAN/同城 (HC-04). See ComputeHashInflightDepth.
    const size_t maxInFlightHashRequests = wanTune.maxInFlightHashRequests;
    size_t hashInflightPeak = 0;        // observability (NFR-07 / AC-12 / AC-02)
    size_t deltaSigInFlight = 0;        // BlockSigRequests sent but not yet answered (WAN gate)
    size_t deltaSigInflightPeak = 0;    // observability (NFR-07 / AC-12)
    // --- AC-12 / NFR-07 acceptance observability (reviewer B-02 / B-03) ---
    uint64_t manifestTotalBytes = 0;             // sum of remote file sizes (AC-12 总字节数)
    std::array<uint64_t, 6> smallFileSizeHist{}; // <4K /4-16K /16-64K /64-256K /256K-1M />=1M
    uint64_t blockSigWaitTotalUs = 0;            // sum of per-file BlockSig wait (AC-04)
    size_t blockSigWaitCount = 0;                // files that completed a BlockSig round trip
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> deltaSigSentAt;
    std::array<uint64_t, 32> ctrlLatencyHistUs{};  // ctrl-msg enqueue->send latency hist (AC-07)
    double maxCtrlGapSec = 0.0;                  // longest stall with no ctrl event (AC-08)
    auto lastCtrlEventAt = std::chrono::steady_clock::now();
    // Log2 histogram bucketing (bucket i covers [2^i, 2^(i+1)) microseconds, clamped to 31).
    auto histAddUs = [](std::array<uint64_t, 32>& hist, uint64_t us) {
        unsigned b = 0;
        while (b < 31 && (us >> (b + 1)) != 0) {
            ++b;
        }
        ++hist[b];
    };
    // Conservative P95 estimate from a log2 histogram: upper edge of the bucket where the
    // cumulative count first reaches 95%.
    auto histP95Us = [](const std::array<uint64_t, 32>& hist) -> uint64_t {
        uint64_t total = 0;
        for (uint64_t c : hist) {
            total += c;
        }
        if (total == 0) {
            return 0;
        }
        const uint64_t target = (total * 95 + 99) / 100;  // ceil(0.95 * total)
        uint64_t cum = 0;
        for (unsigned b = 0; b < hist.size(); ++b) {
            cum += hist[b];
            if (cum >= target) {
                return (b >= 31) ? (uint64_t{1} << 31) : (uint64_t{1} << (b + 1));
            }
        }
        return uint64_t{1} << 31;
    };
    // Mark a control-plane completion event (hash/BlockSig response, manifest entry): refresh
    // the watchdog (AC-08). The 5s judgement itself is left to the acceptance driver.
    auto noteCtrlEvent = [&]() {
        lastCtrlEventAt = std::chrono::steady_clock::now();
    };
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

    // C8: write a whole file through the unified driver (design §5.4). Sequential 1 MiB write ops
    // with a bounded in-flight window overlap the writes; the driver's win backend pads the final
    // sub-sector tail and restores the exact size with SetEndOfFile, and OPEN_ALWAYS + a full
    // [0,size) overwrite + SetEndOfFile is byte-identical to the former ofstream trunc+write. Blocks
    // until the file is fully written so the surrounding pool's completion accounting is unchanged.
    auto driverWriteWholeFile = [&](const std::string& path,
                                    const std::vector<uint8_t>& data) -> bool {
        const uint64_t sz = data.size();
        const uint64_t fid =
            clientDriver.openFile(path, fc::io::OpKind::Write, /*unbuffered=*/true, sz);
        if (fid == 0) {
            return false;
        }
        const uint32_t chunk =
            clientDriver.config().chunkBytes == 0 ? (1u << 20) : clientDriver.config().chunkBytes;
        constexpr uint32_t kWindow = 8;  // in-flight write ops per file (bounds resident buffers)
        bool ok = true;
        uint64_t off = 0;
        uint32_t inflight = 0;
        std::vector<fc::io::IoCompletion> comps;
        while (ok && (off < sz || inflight > 0)) {
            while (off < sz && inflight < kWindow) {
                const uint32_t len = static_cast<uint32_t>(std::min<uint64_t>(chunk, sz - off));
                std::vector<fc::io::IoRequest> one(1);
                one[0].kind = fc::io::OpKind::Write;
                one[0].fileId = fid;
                one[0].offset = off;
                one[0].length = len;
                one[0].userTag = off;
                one[0].data.assign(data.begin() + static_cast<std::ptrdiff_t>(off),
                                   data.begin() + static_cast<std::ptrdiff_t>(off + len));
                if (clientDriver.submit(one) == 0) {
                    break;  // write queue full; drain in-flight below then retry
                }
                off += len;
                ++inflight;
            }
            comps.clear();
            clientDriver.drainCompletionsForFile(fid, comps);
            if (comps.empty()) {
                clientDriver.waitForFile(fid, 1000);
                clientDriver.drainCompletionsForFile(fid, comps);
            }
            for (const fc::io::IoCompletion& c : comps) {
                if (inflight > 0) {
                    --inflight;
                }
                if (c.status == fc::io::IoStatus::Error || c.transferred != c.requested) {
                    ok = false;
                }
            }
        }
        clientDriver.closeFile(fid);
        return ok;
    };

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
                    // C8: the file payload write is now issued through the unified disk IO driver
                    // (design §5.4) instead of an inline ofstream, so all client file-content IO
                    // shares one locus with read/write fairness + backpressure. Bytes and final
                    // size are identical to the former trunc+write path.
                    const fs::path abs = JoinRel(options.rootDir, t.relPath);
                    EnsureParentDir(abs);
                    const bool ok = driverWriteWholeFile(abs.string(), t.data);
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
                pendingHashRequestsAt.push_back(std::chrono::steady_clock::now());
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
            // Control-plane enqueue->send latency sample (AC-07 / §3.5).
            if (!pendingHashRequestsAt.empty()) {
                const auto waited = std::chrono::steady_clock::now() - pendingHashRequestsAt.front();
                pendingHashRequestsAt.pop_front();
                histAddUs(ctrlLatencyHistUs, static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::microseconds>(waited).count()));
            }
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
            uint32_t laneBias = (!manifestDone && cptr->isPrimary) ? 1u : 0u;
            // WAN control-lane soft reservation (design §3.5, FR-09/10/11): bias the primary up
            // by a full stream count so bulk data prefers aux lanes, leaving pool[0]'s send
            // window for hash/BlockSig control traffic. Bias only reorders -- it never relaxes
            // the streamLimit cap -- so the primary is still used once aux lanes saturate and no
            // bandwidth is wasted. Gated on WAN + >=2 lanes, so LAN ordering is unchanged.
            if (wanControlReserve && cptr->isPrimary) {
                laneBias = std::max<uint32_t>(laneBias, activeStreamLimit);
            }
            ld.bias = laneBias;
            lanes.push_back(ld);
        }
        const int idx = SelectLeastLoadedLane(lanes, activeStreamLimit, forcePrimary);
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
        const size_t globalSlotCap = static_cast<size_t>(activeStreamLimit) * healthy;
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
        // C10: stream the reconstructed temp through the unified driver into XXH3 for the verify
        // read, instead of ComputeFileHash's inline read. Same bytes -> identical Hash256, so the
        // verify decision is bit-identical (AC-25).
        try {
            std::error_code szEc;
            const uint64_t verifySize =
                static_cast<uint64_t>(fs::file_size(st.tempPath, szEc));
            if (szEc) {
                verifyOk = false;
            } else {
                const uint64_t fid = clientDriver.openFile(
                    st.tempPath.string(), fc::io::OpKind::Read, /*unbuffered=*/true, 0);
                if (fid == 0) {
                    verifyOk = false;
                } else {
                    bool rerr = false;
                    fc::io::SequentialReader reader(clientDriver, fid, verifySize,
                                                    clientDriver.config().chunkBytes, 4);
                    std::vector<uint8_t> cbuf;
                    size_t cpos = 0;
                    auto src = [&](uint8_t* dst, size_t maxLen) -> size_t {
                        size_t written = 0;
                        while (written < maxLen) {
                            if (cpos < cbuf.size()) {
                                const size_t take =
                                    std::min<size_t>(cbuf.size() - cpos, maxLen - written);
                                std::memcpy(dst + written, cbuf.data() + cpos, take);
                                cpos += take;
                                written += take;
                                continue;
                            }
                            bool okr = true;
                            cbuf.clear();
                            cpos = 0;
                            const uint32_t n = reader.next(cbuf, okr);
                            if (!okr) {
                                rerr = true;
                                break;
                            }
                            if (n == 0) {
                                break;
                            }
                        }
                        return written;
                    };
                    const Hash256 got = ComputeHashFromSource(src);
                    clientDriver.closeFile(fid);
                    verifyOk = !rerr && HashEquals(got, st.verifyHash);
                }
            }
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

    // Worker-side heavy reconstruct for one BlockSigResponse (runs OFF the main loop, design
    // §3.4): read the local old file, build the plan, apply the benefit gate (FR-17), pre-write
    // all copy ops to a temp file, and slice the miss ranges. Touches NO shared state -- it
    // only fills a DeltaPlanResult the main loop later applies. Pure plan inputs/outputs are
    // identical to the former synchronous path (FR-08 / NFR-05).
    auto buildDeltaPlanOffloaded = [&](DeltaPlanTask&& task) -> DeltaPlanResult {
        DeltaPlanResult res;
        res.rel = task.rel;
        res.verifyHash = task.fileHash;
        res.newFileBytes = task.sig.fileSize;
        const fs::path abs = JoinRel(options.rootDir, task.rel);

        // Old-file size is metadata (not driver IO, design §6) and bounds the streaming window so
        // it never allocates a whole-file buffer (AC-04).
        std::error_code sizeEc;
        const uint64_t oldLen = static_cast<uint64_t>(fs::file_size(abs, sizeEc));
        if (sizeEc) {
            res.ok = false;
            res.fallbackReason = "old_unreadable";
            res.downloadBytes = 0;
            return res;
        }
        // C7: read the old file sequentially through the unified driver instead of ReadWholeFile
        // (AC-01). Driver read-ahead overlaps disk IO with the rolling scan; no whole-file buffer.
        const uint64_t oldFileId =
            clientDriver.openFile(abs.string(), fc::io::OpKind::Read, /*unbuffered=*/true, 0);
        if (oldFileId == 0) {
            res.ok = false;
            res.fallbackReason = "old_unreadable";
            res.downloadBytes = 0;
            return res;
        }

        // Preallocate the temp up front so copy captures ("命中即捕获", design D-01 A) can seek and
        // write into it during the SAME scan pass -- no second read, no whole-file buffer.
        EnsureParentDir(abs);
        const fs::path tmp = makeDeltaTempPath(abs);
        std::ofstream out(tmp, std::ios::binary | std::ios::out | std::ios::trunc);
        bool ioOk = static_cast<bool>(out);
        if (ioOk && task.sig.fileSize > 0) {
            // Preallocate to the final size so out-of-order range writes land correctly.
            out.seekp(static_cast<std::streamoff>(task.sig.fileSize - 1), std::ios::beg);
            out.put('\0');
            ioOk = static_cast<bool>(out);
        }

        bool readErr = false;
        fc::io::SequentialReader reader(clientDriver, oldFileId, oldLen,
                                        clientDriver.config().chunkBytes, 4);
        std::vector<uint8_t> chunkBuf;
        size_t chunkPos = 0;
        delta::ByteSource src = [&](uint8_t* dst, size_t maxLen) -> size_t {
            size_t written = 0;
            while (written < maxLen) {
                if (chunkPos < chunkBuf.size()) {
                    const size_t take =
                        std::min<size_t>(chunkBuf.size() - chunkPos, maxLen - written);
                    std::memcpy(dst + written, chunkBuf.data() + chunkPos, take);
                    chunkPos += take;
                    written += take;
                    continue;
                }
                bool ok = true;
                chunkBuf.clear();
                chunkPos = 0;
                const uint32_t n = reader.next(chunkBuf, ok);
                if (!ok) {
                    readErr = true;  // genuine read error -> treat as unreadable (bit-identical to
                    break;           // the old ReadWholeFile==false path)
                }
                if (n == 0) {
                    break;  // clean EOF
                }
            }
            return written;
        };
        // Capture each matched full block's live window bytes and write the copy straight to temp
        // (design D-01 A). The bytes are identical to oldData[srcOffsetOld .. +len), so the temp
        // content stays bit-identical to the former ReadWholeFile+BuildPlan path.
        delta::CopyCapturedFn onCopy = [&](uint64_t /*srcOffsetOld*/, uint64_t destOffsetNew,
                                           uint32_t len, const uint8_t* windowBytes) {
            if (!ioOk) {
                return;
            }
            out.seekp(static_cast<std::streamoff>(destOffsetNew), std::ios::beg);
            out.write(reinterpret_cast<const char*>(windowBytes),
                      static_cast<std::streamsize>(len));
            ioOk = static_cast<bool>(out);
        };

        const delta::DeltaPlan plan =
            delta::BuildPlanStreaming(task.sig, oldLen, src, {}, onCopy);
        res.stats = plan.stats;
        res.downloadBytes = plan.downloadBytes;
        res.newFileBytes = plan.newFileBytes;
        clientDriver.closeFile(oldFileId);

        auto discardTemp = [&]() {
            if (out.is_open()) {
                out.close();
            }
            std::error_code rec;
            fs::remove(tmp, rec);
        };
        if (readErr) {
            discardTemp();
            res.ok = false;
            res.fallbackReason = "old_unreadable";
            res.downloadBytes = 0;
            return res;
        }
        if (delta::BenefitRejected(plan.downloadBytes, plan.newFileBytes)) {
            discardTemp();
            res.ok = false;
            res.fallbackReason = "benefit";  // FR-19 / AC-07
            res.benefitRejected = true;
            return res;
        }
        if (!ioOk) {
            discardTemp();
            res.ok = false;
            res.fallbackReason = "reconstruct_io";
            return res;
        }
        // Slice large misses so multiple lanes can fetch one file's regions in parallel.
        const uint64_t sliceLen = std::min<uint64_t>(
            static_cast<uint64_t>(effectiveChunkSize) * 8, 0xFFFFFFFFull);
        for (const delta::MissOp& m : plan.misses) {
            uint64_t off = m.destOffsetNew;
            uint64_t remaining = m.len;
            while (remaining > 0) {
                const uint32_t take = static_cast<uint32_t>(std::min<uint64_t>(remaining, sliceLen));
                res.ranges.push_back(DeltaRangeTask{task.rel, off, take});
                off += take;
                remaining -= take;
            }
        }
        res.tempPath = tmp;
        res.tempOut = std::move(out);
        res.ok = true;
        return res;
    };

    // Main-thread application of one finished plan (design §3.4): mutate deltaStates, hand the
    // pre-written temp + ranges to the transfer machinery, or fall back. Mirrors the former
    // synchronous tail of beginDeltaReconstruct exactly.
    auto applyDeltaPlanResult = [&](DeltaPlanResult& res) {
        auto itState = deltaStates.find(res.rel);
        if (itState == deltaStates.end()) {
            // File abandoned (e.g. DeltaError) while the plan was building: discard the temp.
            if (res.tempOut.is_open()) {
                res.tempOut.close();
            }
            if (!res.tempPath.empty()) {
                std::error_code rec;
                fs::remove(res.tempPath, rec);
            }
            return;
        }
        if (!res.ok) {
            deltaFallback(res.rel, res.fallbackReason.c_str(), res.downloadBytes, res.newFileBytes,
                          res.benefitRejected ? &res.stats : nullptr);
            deltaAbandoned.insert(res.rel);
            deltaStates.erase(itState);
            scheduleTransfer(res.rel);
            return;
        }
        DeltaFileState& st = itState->second;
        st.verifyHash = res.verifyHash;
        st.newFileBytes = res.newFileBytes;
        st.tempPath = res.tempPath;
        st.tempOut = std::move(res.tempOut);
        const uint32_t rangeCount = static_cast<uint32_t>(res.ranges.size());
        for (DeltaRangeTask& r : res.ranges) {
            pendingDeltaRanges.push_back(std::move(r));
        }
        st.pendingRanges = rangeCount;
        if (debugEnabled) {
            std::cout << "[delta] plan rel=" << res.rel << " sessionId=" << sessionId
                      << " newBytes=" << res.newFileBytes << " downloadBytes=" << res.downloadBytes
                      << " ranges=" << rangeCount
                      << " scanned_bytes=" << res.stats.scannedBytes
                      << " matched_bytes=" << res.stats.matchedBytes
                      << " strong_computes=" << res.stats.strongComputations
                      << " weak_hits=" << res.stats.weakCandidateHits
                      << " early_stopped=" << (res.stats.earlyStopped ? 1 : 0) << std::endl;
        }
        if (rangeCount == 0) {
            finalizeDelta(res.rel);  // 100% match: copies already cover the whole file (AC-03)
        }
    };

    // Send queued BlockSigRequests on the control lane (binary-delta §6.1). On send failure
    // the lane is marked down and the requests are requeued for a healthy control lane.
    // WAN (design §3.4, FR-05/06): cap the in-flight BlockSig pipeline at maxInFlightDeltaSig
    // so a single file's RTT is amortized across a deep batch of concurrent files rather than
    // being a per-file fixed wait. maxInFlightDeltaSig == 0 (LAN) keeps the legacy "send the
    // whole queue every pass" behavior, so 同城/LAN is unchanged (HC-04). Requests that do not
    // fit this pass stay queued (no correctness loss, FR-08).
    auto pumpDeltaSignatures = [&]() {
        if (pendingDeltaSigRequests.empty()) {
            return;
        }
        ClientConnection* ctrl = controlConn();
        std::vector<Frame> frames;
        std::vector<std::string> batch;
        while (!pendingDeltaSigRequests.empty()) {
            if (maxInFlightDeltaSig != 0 &&
                (deltaSigInFlight + batch.size()) >= maxInFlightDeltaSig) {
                break;  // pipeline full this pass
            }
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
            const auto sigSentAt = std::chrono::steady_clock::now();
            for (const std::string& r : batch) {
                deltaSigRequested.insert(r);
                deltaSigSentAt[r] = sigSentAt;  // per-file BlockSig wait start (AC-04)
            }
            deltaSigInFlight += batch.size();
            deltaSigInflightPeak = std::max<size_t>(deltaSigInflightPeak, deltaSigInFlight);
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
            // AC-12 acceptance metrics: total bytes + small-file size distribution.
            manifestTotalBytes += e.fileSize;
            {
                const uint64_t sz = e.fileSize;
                size_t bucket;
                if (sz < 4ull * 1024) bucket = 0;
                else if (sz < 16ull * 1024) bucket = 1;
                else if (sz < 64ull * 1024) bucket = 2;
                else if (sz < 256ull * 1024) bucket = 3;
                else if (sz < 1024ull * 1024) bucket = 4;
                else bucket = 5;
                ++smallFileSizeHist[bucket];
            }
            // Manifest streaming is a control-plane completion event (AC-08 watchdog).
            noteCtrlEvent();
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
            noteCtrlEvent();  // HashResponse is a control-plane completion event (AC-08)
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
            // Account the control-plane round trip (WAN pipeline depth + observability).
            if (deltaSigInFlight > 0) {
                --deltaSigInFlight;
            }
            noteCtrlEvent();  // BlockSigResponse is a control-plane completion event (AC-08)
            // Per-file BlockSig wait accumulation (AC-04 blocksig_wait_us).
            {
                auto itSent = deltaSigSentAt.find(info.relPath);
                if (itSent != deltaSigSentAt.end()) {
                    const auto waited = std::chrono::steady_clock::now() - itSent->second;
                    blockSigWaitTotalUs += static_cast<uint64_t>(
                        std::chrono::duration_cast<std::chrono::microseconds>(waited).count());
                    ++blockSigWaitCount;
                    deltaSigSentAt.erase(itSent);
                }
            }
            // Offload the heavy reconstruct to the worker pool (design §3.4). Set sigReceived
            // on the main thread so a duplicate response (failover re-request) is ignored and
            // the failover re-request scan never re-queues a file already being planned.
            auto itSt = deltaStates.find(info.relPath);
            if (itSt != deltaStates.end() && !itSt->second.sigReceived) {
                itSt->second.sigReceived = true;
                {
                    std::lock_guard<std::mutex> lock(deltaPlanTaskMu);
                    deltaPlanTaskQueue.push_back(
                        DeltaPlanTask{info.relPath, info.fileHash, std::move(info.sig)});
                }
                deltaPlanInFlight.fetch_add(1, std::memory_order_relaxed);
                deltaPlanTaskCv.notify_one();
            }
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
            const bool rangeTagged = (itR != activeDeltaRanges.end());
            if (rangeTagged) {
                rel = itR->second.rel;
                activeDeltaRanges.erase(itR);
                releaseSlot();
            }
            noteCtrlEvent();  // any DeltaError is a control-plane completion event (AC-08, S-11)
            auto itS = deltaStates.find(rel);
            const bool wasManaged = (itS != deltaStates.end()) || deltaSigRequested.contains(rel);
            // B-01: a sig-level DeltaError (no matching active range) is the server's terminal
            // answer to a BlockSigRequest that will NEVER yield a BlockSigResponse, so release
            // its WAN pipeline slot + per-file wait marker exactly like BlockSigResponse would.
            // Without this, deltaSigInFlight leaks once per sig failure; on WAN it climbs to
            // maxInFlightDeltaSig, pumpDeltaSignatures then breaks every pass and the sync wedges
            // (deltaStates never drains -> main loop never exits). Range-tagged errors arrive
            // AFTER the response already settled the count, so only the sig-level path adjusts
            // it; the !sigReceived + erase() guards make a failover-requeued or already-answered
            // signature safe from a double release.
            const bool sigReceived = (itS != deltaStates.end()) && itS->second.sigReceived;
            if (DeltaErrorReleasesSigSlot(rangeTagged, itS != deltaStates.end(), sigReceived) &&
                deltaSigRequested.erase(rel) > 0) {
                if (deltaSigInFlight > 0) {
                    --deltaSigInFlight;
                }
                deltaSigSentAt.erase(rel);
            }
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
        // requests; the BlockSigResponse handler ignores duplicate responses (sigReceived
        // guard), so re-requesting is safe and prevents a stuck deltaState from hanging the sync.
        if (newlyDown && healthyConnCount() > 0) {
            for (auto& kv : deltaStates) {
                if (!kv.second.sigReceived && deltaSigRequested.contains(kv.first)) {
                    deltaSigRequested.erase(kv.first);
                    deltaSigSentAt.erase(kv.first);  // re-measure wait on the re-send (AC-04)
                    pendingDeltaSigRequests.push_back(kv.first);
                    // These were counted in flight; they will be re-sent (re-counted) by
                    // pumpDeltaSignatures, so release the count here to keep the WAN pipeline
                    // gate balanced.
                    if (deltaSigInFlight > 0) {
                        --deltaSigInFlight;
                    }
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

    // BuildPlan offload pool (design §3.4). Spawned here, after every reconstruct helper is in
    // scope; same task-queue + result-deque pattern as the hash workers above.
    std::vector<std::thread> deltaPlanWorkers;
    deltaPlanWorkers.reserve(workerCount);
    for (uint32_t i = 0; i < workerCount; ++i) {
        deltaPlanWorkers.emplace_back([&]() {
            while (true) {
                DeltaPlanTask task;
                {
                    std::unique_lock<std::mutex> lock(deltaPlanTaskMu);
                    deltaPlanTaskCv.wait(lock, [&]() {
                        return deltaPlanStop.load() || !deltaPlanTaskQueue.empty();
                    });
                    if (deltaPlanStop.load() && deltaPlanTaskQueue.empty()) {
                        return;
                    }
                    task = std::move(deltaPlanTaskQueue.front());
                    deltaPlanTaskQueue.pop_front();
                }
                DeltaPlanResult res = buildDeltaPlanOffloaded(std::move(task));
                {
                    std::lock_guard<std::mutex> lock(deltaPlanResultMu);
                    deltaPlanResults.push_back(std::move(res));
                }
            }
        });
    }

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
            {
                // Apply finished BuildPlan offload results (design §3.4): enqueue their miss
                // ranges before tryStartDeltaRanges so data starts the same iteration.
                std::deque<DeltaPlanResult> plansReady;
                {
                    std::lock_guard<std::mutex> lock(deltaPlanResultMu);
                    plansReady.swap(deltaPlanResults);
                }
                for (DeltaPlanResult& pr : plansReady) {
                    applyDeltaPlanResult(pr);
                    deltaPlanInFlight.fetch_sub(1, std::memory_order_relaxed);
                }
                if (!plansReady.empty()) {
                    loopHadForwardProgress = true;
                }
            }
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

            // Track the hash in-flight peak (NFR-07 / AC-02 / AC-12): the WAN cap lets this
            // climb past the legacy 8192 on massive small-file sets.
            {
                const size_t hin = hashRequestsSent - hashResponsesReceived;
                if (hin > hashInflightPeak) {
                    hashInflightPeak = hin;
                }
            }

            // AC-08 watchdog: the longest stall with control-plane work outstanding but no
            // control-plane completion event (hash/BlockSig response, manifest entry). While
            // no control work is pending the gap is not meaningful, so the clock is refreshed.
            {
                const bool deltaBusy =
                    !deltaStates.empty() || !pendingDeltaSigRequests.empty() ||
                    !pendingDeltaRanges.empty() || !activeDeltaRanges.empty() ||
                    deltaPlanInFlight.load(std::memory_order_relaxed) != 0;
                const bool ctrlWorkOutstanding =
                    !manifestDone || (hashRequestsSent != hashResponsesReceived) ||
                    !pendingHashRequests.empty() || deltaBusy;
                if (ctrlWorkOutstanding) {
                    const double gap = std::chrono::duration<double>(
                        std::chrono::steady_clock::now() - lastCtrlEventAt).count();
                    if (gap > maxCtrlGapSec) {
                        maxCtrlGapSec = gap;
                    }
                } else {
                    lastCtrlEventAt = std::chrono::steady_clock::now();
                }
            }

            // WAN parallelism failure-rate backoff (design §3.3, FR-13 / NFR-04 / B8): on a
            // weak SSD/controller failure spike, halve the effective stream count (floor 4) so
            // the session self-heals without the user manually降级 (NFR-06). No-op on LAN and
            // unless the WAN auto-tune actually raised the count above 4.
            if (wanMode && options.streamAutoTune && activeStreamLimit > 4) {
                const auto nowBk = std::chrono::steady_clock::now();
                if ((nowBk - lastBackoffCheck) >= std::chrono::seconds(2)) {
                    const size_t windowDone = (failed + transferred) - lastBackoffCompleted;
                    const size_t windowFailed = failed - lastBackoffFailed;
                    if (windowDone >= 200) {  // enough samples to judge the rate
                        const double rate =
                            static_cast<double>(windowFailed) / static_cast<double>(windowDone);
                        if (rate > kStreamBackoffErrRate) {
                            activeStreamLimit = std::max<uint32_t>(4, activeStreamLimit / 2);
                            if (debugEnabled || diagnostics) {
                                std::cerr << "[wan] stream_backoff rate=" << rate
                                          << " streams=" << activeStreamLimit << std::endl;
                            }
                        }
                        lastBackoffFailed = failed;
                        lastBackoffCompleted = failed + transferred;
                        lastBackoffCheck = nowBk;
                    } else {
                        lastBackoffCheck = nowBk;
                    }
                }
            }

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
                              << " session_rtt_ms=" << sessionRttMs
                              << " wan_mode=" << (wanMode ? 1 : 0)
                              << " active_streams=" << activeStreamLimit
                              << " hash_inflight_cap=" << maxInFlightHashRequests
                              << " hash_inflight_peak=" << hashInflightPeak
                              << " delta_sig_inflight=" << deltaSigInFlight
                              << " delta_sig_inflight_peak=" << deltaSigInflightPeak
                              << " blocksig_wait_us=" << blockSigWaitTotalUs
                              << " blocksig_files=" << blockSigWaitCount
                              << " ctrl_p95_us=" << histP95Us(ctrlLatencyHistUs)
                              << " seconds_since_ctrl_event="
                              << std::chrono::duration<double>(now - lastCtrlEventAt).count()
                              << " max_ctrl_gap_sec=" << maxCtrlGapSec
                              << std::endl;
                    // Per-lane kernel TCP state (design wan-single-tcp §3.2): cwnd sawtooth +
                    // rising retrans is the smoking gun for loss-driven AIMD on a single TCP.
                    // retrans unit differs by platform (Linux: segments, Windows: bytes), so it
                    // is labelled explicitly to avoid cross-platform misreads (reviewer S-02).
#ifdef _WIN32
                    const char* kRetransUnit = "bytes";
#else
                    const char* kRetransUnit = "seg";
#endif
                    for (auto& cptr : pool) {
                        const TcpDiag td = QueryTcpDiag(cptr->socket.Get());
                        if (!td.valid) {
                            continue;
                        }
                        std::cerr << "[debug][tcp] lane=" << cptr->connId
                                  << " primary=" << (cptr->isPrimary ? 1 : 0)
                                  << " healthy=" << (cptr->healthy.load() ? 1 : 0)
                                  << " cwnd_kb=" << (td.cwndBytes / 1024)
                                  << " inflight_kb=" << (td.bytesInFlight / 1024)
                                  << " rtt_ms=" << (td.rttUs / 1000.0)
                                  << " retrans=" << td.retrans
                                  << " retrans_unit=" << kRetransUnit
                                  << " mss=" << td.mss
                                  << std::endl;
                    }
                    lastDebugPrint = now;
                }
            }

            const bool allHashDone = (fallbackResolved == fallbackCount);
            const bool allCompareDone = (compareResultsHandled.load() == compareTasksIssued.load());
            const bool deltaIdle = deltaStates.empty() && pendingDeltaSigRequests.empty() &&
                                   pendingDeltaRanges.empty() && activeDeltaRanges.empty() &&
                                   deltaPlanInFlight.load(std::memory_order_relaxed) == 0;
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
        deltaPlanStop.store(true);
        deltaPlanTaskCv.notify_all();
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
        for (auto& w : deltaPlanWorkers) {
            JoinDiag(w, "client-catch-delta-plan");
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
    deltaPlanStop.store(true);
    deltaPlanTaskCv.notify_all();
    ioStop.store(true);
    ioTaskCv.notify_all();
    for (auto& w : compareWorkers) {
        JoinDiag(w, "client-compare");
    }
    for (auto& w : hashWorkers) {
        JoinDiag(w, "client-hash");
    }
    for (auto& w : deltaPlanWorkers) {
        JoinDiag(w, "client-delta-plan");
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
            mtimeDeltaP50 = PercentileNearestRank(mtimeDeltas, 0.50);
            mtimeDeltaP95 = PercentileNearestRank(mtimeDeltas, 0.95);
            mtimeDeltaP99 = PercentileNearestRank(mtimeDeltas, 0.99);
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

        // --- AC-12 / NFR-07 acceptance report fields (reviewer B-02 / B-03) ---
        const double elapsedSec = std::max<double>(1e-9,
            std::chrono::duration<double>(std::chrono::steady_clock::now() - syncStartTime).count());
        const double throughputMbps =
            (static_cast<double>(manifestTotalBytes) * 8.0) / elapsedSec / 1.0e6;
        const uint64_t blocksigWaitMeanUs =
            (blockSigWaitCount > 0) ? (blockSigWaitTotalUs / blockSigWaitCount) : 0;
        std::cout << "[diag][wan] session_rtt_ms=" << sessionRttMs
                  << " wan_mode=" << (wanMode ? 1 : 0)
                  << " active_streams=" << activeStreamLimit
                  << " hash_inflight_cap=" << maxInFlightHashRequests
                  << " hash_inflight_peak=" << hashInflightPeak
                  << " delta_sig_inflight_peak=" << deltaSigInflightPeak
                  << " blocksig_wait_us=" << blockSigWaitTotalUs
                  << " blocksig_files=" << blockSigWaitCount
                  << " blocksig_wait_mean_us=" << blocksigWaitMeanUs
                  << " ctrl_p95_us=" << histP95Us(ctrlLatencyHistUs)
                  << " max_ctrl_gap_sec=" << maxCtrlGapSec
                  << " file_count=" << enumerated
                  << " total_bytes=" << manifestTotalBytes
                  << " elapsed_sec=" << elapsedSec
                  << " throughput_mbps=" << throughputMbps
                  << " size_dist_lt4k=" << smallFileSizeHist[0]
                  << " size_dist_4k_16k=" << smallFileSizeHist[1]
                  << " size_dist_16k_64k=" << smallFileSizeHist[2]
                  << " size_dist_64k_256k=" << smallFileSizeHist[3]
                  << " size_dist_256k_1m=" << smallFileSizeHist[4]
                  << " size_dist_ge1m=" << smallFileSizeHist[5]
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
