// Bounded pread/pwrite thread-pool backend (unified-disk-io-driver design section 3.4.3 / section 7.3 / section 7.4).
//
// This translation unit compiles ONLY on macOS and Linux and is empty everywhere else.
//
// macOS (design section 7.4, FR-06, AC-13/N9): unbuffered intent is expressed with fcntl(F_NOCACHE, 1).
//   This file MUST NOT reference O_DIRECT and MUST NOT reference POSIX aio (aio_read/aio_write) on
//   the macOS path - both are compile-time absent there so the AC-13 source scan stays clean.
// Linux (design section 7.3, FR-04/05/10, AC-11/12): the fd is opened O_DIRECT for the unbuffered path;
//   if O_DIRECT open fails or a direct IO returns EINVAL the op silently falls back to a buffered
//   fd (opened from the path, no O_DIRECT) with posix_fadvise(POSIX_FADV_SEQUENTIAL) - no error
//   surfaces and the resulting bytes are unchanged (AC-12).
//
// Multiple ops are in flight via N worker threads executing synchronous pread/pwrite (FR-12); the
// driver above enforces fairness / backpressure / cancellation.

#if defined(__APPLE__) || defined(__linux__)

#include "disk_io_backend_pool.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace fc::io {
namespace {

struct PoolFile {
    std::string path;
    int fd = -1;
    OpKind mode = OpKind::Read;
    bool unbuffered = false;
    uint64_t expectedSize = 0;
    int64_t modifyNs = 0;    // W-01: mtime to stamp on the write fd at close (if hasModify)
    bool hasModify = false;  // W-01: set by setWriteModifyTime; false => close skips futimens
    AlignInfo align;
};

// Convert a manifest modify time (Unix ns or, for legacy round-trips, Windows FILETIME ticks) to a
// POSIX timespec for futimens (W-01/FR-01). The threshold 5e17 sits between the two encodings:
// values >5e17 are Unix ns (POSIX peer) -> pass through; values in [1.16e17, 5e17] are FILETIME ticks
// (Windows peer, 100ns since 1601) -> convert to Unix ns. Mirrors the authoritative
// NormalizeManifestMtimeToUnixNs in file_index.cpp / TryNormalizeMtimeToUnixNs in compare_phase.cpp
// so the stamped mtime round-trips against the manifest (C3 / D-14-A).
timespec ToTimespecFromNs(int64_t modifyNs) {
    constexpr int64_t kLikelyUnixNsThreshold = 500000000000000000LL;
    constexpr int64_t kWindowsEpochDiff100ns = 116444736000000000LL;
    int64_t unixNs = modifyNs;
    if (modifyNs > kLikelyUnixNsThreshold) {
        // Already Unix ns, pass through.
    } else if (modifyNs >= kWindowsEpochDiff100ns) {
        // FILETIME ticks (100ns since 1601) -> Unix ns.
        unixNs = (modifyNs - kWindowsEpochDiff100ns) * 100LL;
    }
    timespec ts{};
    if (unixNs < 0) {
        unixNs = 0;
    }
    ts.tv_sec = static_cast<time_t>(unixNs / 1000000000LL);
    ts.tv_nsec = static_cast<long>(unixNs % 1000000000LL);
    return ts;
}

class PosixPoolBackend : public PlatformIoBackend {
public:
    explicit PosixPoolBackend(const IoDriverConfig& cfg, bool ioUringFallback = false) : cfg_(cfg) {
        if (ioUringFallback) {
            counters_.ioUringFallback.store(1);  // pool used because io_uring unavailable (AC-11)
        }
        const uint32_t n = cfg.backendConcurrency == 0 ? 1u : cfg.backendConcurrency;
        for (uint32_t i = 0; i < n; ++i) {
            workers_.emplace_back([this] { WorkerLoop(); });
        }
    }
    ~PosixPoolBackend() override { shutdown(); }

    BackendKind kind() const override { return BackendKind::PosixThreadPool; }
    AlignInfo queryAlign(const std::string& path) override { return QueryAlign(path); }

    uint64_t openFile(const std::string& path, OpKind mode, bool unbuffered,
                      uint64_t expectedSize) override {
        PoolFile pf;
        pf.path = path;
        pf.mode = mode;
        pf.expectedSize = expectedSize;
        pf.align = QueryAlign(path);

        // unbuffered-writes M4/FR-13/D-02: keep the small-file (< kSmallFileBufferedMax) whole-file
        // downgrade ONLY for reads. Write opens with unbuffered intent stay unbuffered regardless of
        // size; sub-granularity / unaligned fragments already fall back per-op in DoPwrite (D-03).
        // Change 3 (fastcheck-redundant-syscall-elim §3.3.4/FR-25): the read direct-IO gate is now
        // decoupled from expectedSize so reads stay buffered even though callers began passing a
        // positive expectedSize. This preserves the current direct/buffered strategy and the
        // smallFileFallback count (reads were already always buffered when expectedSize==0).
        const bool wantUnbuffered =
            unbuffered && !cfg_.forceBuffered &&
            (mode == OpKind::Read ? false : true);
        if (unbuffered && !wantUnbuffered && mode == OpKind::Read) {
            counters_.smallFileFallback.fetch_add(1);  // read small-file downgrade (FR-11)
        }

        const int base = (mode == OpKind::Write) ? (O_WRONLY | O_CREAT) : O_RDONLY;
        int fd = -1;
#if defined(__linux__)
        if (wantUnbuffered) {
            fd = ::open(path.c_str(), base | O_DIRECT | O_CLOEXEC, 0644);
            if (fd >= 0) {
                pf.unbuffered = true;
            } else {
                counters_.bufferedFallback.fetch_add(1);  // O_DIRECT open EINVAL: silent fallback
            }
        }
        if (fd < 0) {
            fd = ::open(path.c_str(), base | O_CLOEXEC, 0644);
            if (fd >= 0) {
                ::posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
            }
        }
#else  // __APPLE__ : NO O_DIRECT (AC-13). Express unbuffered intent with F_NOCACHE.
        fd = ::open(path.c_str(), base | O_CLOEXEC, 0644);
        if (fd >= 0 && wantUnbuffered) {
            ::fcntl(fd, F_NOCACHE, 1);
            pf.unbuffered = true;
        }
#endif
        if (fd < 0) {
            return 0;
        }
        pf.fd = fd;
        std::lock_guard<std::mutex> lk(mu_);
        const uint64_t id = nextId_++;
        files_.emplace(id, std::move(pf));
        return id;
    }

    void setWriteModifyTime(uint64_t fileId, int64_t modifyNs) override {
        // W-01/FR-01: only record; futimens on the still-open fd happens in closeFile.
        std::lock_guard<std::mutex> lk(mu_);
        auto it = files_.find(fileId);
        if (it == files_.end() || it->second.mode != OpKind::Write) {
            return;
        }
        it->second.modifyNs = modifyNs;
        it->second.hasModify = true;
    }

    bool closeFile(uint64_t fileId) override {
        PoolFile pf;
        {
            std::lock_guard<std::mutex> lk(mu_);
            auto it = files_.find(fileId);
            if (it == files_.end()) {
                return false;  // unknown / already closed (W-01 idempotency)
            }
            pf = std::move(it->second);
            files_.erase(it);
        }
        bool ok = true;
        // W-01 order (R-01): ftruncate -> (optional) futimens -> close, all on the same write fd.
        // Exact final size on every write close (FR-11). Truncating unconditionally also gives the
        // driver write path ofstream-trunc-equivalent overwrite semantics (a pre-existing target is
        // trimmed to expectedSize, incl. 0 for an empty file, so no stale tail bytes survive).
        if (pf.mode == OpKind::Write && pf.fd >= 0) {
            if (::ftruncate(pf.fd, static_cast<off_t>(pf.expectedSize)) != 0) {
                ok = false;  // ok1
            }
            if (pf.hasModify) {
                timespec ts[2];
                ts[0].tv_sec = 0;
                ts[0].tv_nsec = UTIME_OMIT;         // leave atime untouched
                ts[1] = ToTimespecFromNs(pf.modifyNs);  // mtime
                if (::futimens(pf.fd, ts) != 0) {
                    ok = false;  // ok2
                }
            }
        } else if (pf.mode == OpKind::Write) {
            ok = false;  // write fd vanished; cannot finalize
        }
        if (pf.fd >= 0) {
            if (::close(pf.fd) != 0) {
                ok = false;  // ok3
            }
        }
        return ok;
    }

    bool submit(IoRequest&& req) override {
        {
            std::lock_guard<std::mutex> lk(qmu_);
            queue_.push_back(std::move(req));
        }
        qcv_.notify_one();
        return true;
    }

    size_t reap(std::vector<IoCompletion>& out, size_t max, int timeoutMs) override {
        std::unique_lock<std::mutex> lk(cmu_);
        if (completions_.empty() && timeoutMs != 0) {
            ccv_.wait_for(lk, std::chrono::milliseconds(timeoutMs < 0 ? 1000 : timeoutMs),
                          [this] { return !completions_.empty(); });
        }
        size_t n = 0;
        while (!completions_.empty() && n < max) {
            out.push_back(std::move(completions_.front()));
            completions_.pop_front();
            ++n;
        }
        return n;
    }

    void shutdown() override {
        {
            std::lock_guard<std::mutex> lk(qmu_);
            if (stop_) {
                return;
            }
            stop_ = true;
        }
        qcv_.notify_all();
        for (auto& t : workers_) {
            if (t.joinable()) {
                t.join();
            }
        }
        workers_.clear();
        // Defensive: close any fds a caller left open so a leaked fileId (e.g. an abandoned
        // download/temp when a session tears down for reconnect) never leaks an OS fd.
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& kv : files_) {
            if (kv.second.fd >= 0) {
                ::close(kv.second.fd);
            }
        }
        files_.clear();
    }

    BackendCounters counters() const override { return counters_.snapshot(); }

private:
    void WorkerLoop() {
        for (;;) {
            IoRequest req;
            {
                std::unique_lock<std::mutex> lk(qmu_);
                qcv_.wait(lk, [this] { return stop_ || !queue_.empty(); });
                if (stop_ && queue_.empty()) {
                    return;
                }
                req = std::move(queue_.front());
                queue_.pop_front();
            }
            Execute(std::move(req));
        }
    }

    void Execute(IoRequest&& req) {
        IoCompletion c;
        c.kind = req.kind;
        c.fileId = req.fileId;
        c.offset = req.offset;
        c.requested = req.length;
        c.userTag = req.userTag;

        PoolFile pf;
        {
            std::lock_guard<std::mutex> lk(mu_);
            auto it = files_.find(req.fileId);
            if (it == files_.end()) {
                c.status = IoStatus::Error;
                PushCompletion(std::move(c));
                return;
            }
            pf = it->second;  // shallow copy: path + fd + align (fd is a shared int, read-only use)
        }

        if (pf.unbuffered) {
            counters_.directIo.fetch_add(1);
        } else {
            counters_.bufferedFallback.fetch_add(1);
        }

        if (req.kind == OpKind::Read) {
            std::vector<uint8_t> buf(req.length);
            const ssize_t got = DoPread(pf, buf.data(), req.length, req.offset);
            if (got < 0) {
                c.status = IoStatus::Error;
            } else {
                buf.resize(static_cast<size_t>(got));
                c.transferred = static_cast<uint32_t>(got);
                c.status = (static_cast<uint32_t>(got) < req.length) ? IoStatus::Eof : IoStatus::Ok;
                c.data = std::move(buf);
            }
        } else {
            const ssize_t put = DoPwrite(pf, req.data.data(),
                                         static_cast<uint32_t>(req.data.size()), req.offset);
            c.status = (put < 0) ? IoStatus::Error : IoStatus::Ok;
            if (put >= 0) {
                c.transferred = static_cast<uint32_t>(put);
            }
        }
        PushCompletion(std::move(c));
    }

#if defined(__linux__)
    // Open a transient buffered fd (no O_DIRECT) from the path for a sub-granularity tail or an
    // unaligned segment; O_DIRECT cannot serve these, so this is the buffered fallback (AC-12/FR-11).
    int OpenBufferedFd(const PoolFile& pf) {
        const int base = (pf.mode == OpKind::Write) ? (O_WRONLY | O_CREAT) : O_RDONLY;
        int fd = ::open(pf.path.c_str(), base | O_CLOEXEC, 0644);
        if (fd >= 0) {
            ::posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
        }
        return fd;
    }
#endif

    ssize_t DoPread(const PoolFile& pf, uint8_t* dst, uint32_t len, uint64_t off) {
#if defined(__linux__)
        if (pf.unbuffered) {
            const uint32_t g = pf.align.ioGranularity;
            // F1: direct IO uses an internal bounce buffer (AlignedAlloc below), so the caller's
            // dst address alignment is irrelevant; only off/len must meet the IO granularity.
            const bool aligned = IsAligned(off, g) && IsAligned(len, g);
            if (aligned) {
                void* abuf = AlignedAlloc(g, len);
                if (abuf) {
                    const ssize_t r = ::pread(pf.fd, abuf, len, static_cast<off_t>(off));
                    if (!(r < 0 && errno == EINVAL)) {
                        if (r > 0) {
                            std::memcpy(dst, abuf, static_cast<size_t>(r));
                        }
                        AlignedFree(abuf);
                        return r;
                    }
                    AlignedFree(abuf);  // EINVAL -> silent buffered fallback below
                }
            }
            counters_.tailZeroFallback.fetch_add(1);
            const int bfd = OpenBufferedFd(pf);
            if (bfd < 0) {
                return -1;
            }
            const ssize_t r = ::pread(bfd, dst, len, static_cast<off_t>(off));
            ::close(bfd);
            return r;
        }
#endif
        return ::pread(pf.fd, dst, len, static_cast<off_t>(off));
    }

    ssize_t DoPwrite(const PoolFile& pf, const uint8_t* src, uint32_t len, uint64_t off) {
#if defined(__linux__)
        if (pf.unbuffered) {
            const uint32_t g = pf.align.ioGranularity;
            // F1: direct IO uses an internal bounce buffer (AlignedAlloc below), so the caller's
            // src address alignment is irrelevant; only off/len must meet the IO granularity.
            const bool aligned = IsAligned(off, g) && IsAligned(len, g);
            if (aligned) {
                void* abuf = AlignedAlloc(g, len);
                if (abuf) {
                    std::memcpy(abuf, src, len);
                    const ssize_t r = ::pwrite(pf.fd, abuf, len, static_cast<off_t>(off));
                    AlignedFree(abuf);
                    if (!(r < 0 && errno == EINVAL)) {
                        return r;
                    }
                }
            }
            counters_.tailZeroFallback.fetch_add(1);
            const int bfd = OpenBufferedFd(pf);
            if (bfd < 0) {
                return -1;
            }
            const ssize_t r = ::pwrite(bfd, src, len, static_cast<off_t>(off));
            ::close(bfd);
            return r;
        }
#endif
        return ::pwrite(pf.fd, src, len, static_cast<off_t>(off));
    }

    void PushCompletion(IoCompletion&& c) {
        {
            std::lock_guard<std::mutex> lk(cmu_);
            completions_.push_back(std::move(c));
        }
        ccv_.notify_all();
    }

    IoDriverConfig cfg_;
    std::vector<std::thread> workers_;

    std::mutex mu_;
    std::unordered_map<uint64_t, PoolFile> files_;
    uint64_t nextId_ = 1;

    std::mutex qmu_;
    std::condition_variable qcv_;
    std::deque<IoRequest> queue_;
    bool stop_ = false;

    std::mutex cmu_;
    std::condition_variable ccv_;
    std::deque<IoCompletion> completions_;

    PoolCounters counters_;
};

}  // namespace

std::unique_ptr<PlatformIoBackend> CreatePosixPoolBackend(const IoDriverConfig& cfg,
                                                          bool ioUringFallback) {
    return std::make_unique<PosixPoolBackend>(cfg, ioUringFallback);
}

#if defined(__APPLE__)
// macOS has no io_uring; the bounded pread/pwrite pool (with F_NOCACHE unbuffered intent) is the
// primary backend, not a fallback, so ioUringFallback stays 0 (design section 7.4, pool header note). On
// Linux this factory lives in disk_io_backend_uring.cpp instead, so this definition is guarded to
// macOS only to avoid a duplicate symbol.
std::unique_ptr<PlatformIoBackend> CreatePlatformBackend(const IoDriverConfig& cfg) {
    return CreatePosixPoolBackend(cfg, /*ioUringFallback=*/false);
}
#endif  // __APPLE__

}  // namespace fc::io

#endif  // __APPLE__ || __linux__
