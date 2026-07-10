// Linux io_uring backend with runtime probe + bounded pread/pwrite pool fallback
// (unified-disk-io-driver design section 3.4.2 / section 7.3, FR-04/05/10, AC-10/11/12).
//
// Compiles ONLY on Linux and is empty elsewhere. The io_uring code is additionally guarded by
// `__has_include(<liburing.h>)`: on kernels/toolchains without liburing the io_uring segment is
// not compiled at all and CreatePlatformBackend returns the pread/pwrite pool (design section 7.3 compile
// guard). At runtime, if io_uring_queue_init fails (-ENOSYS / -EPERM / resource exhaustion) the
// backend also falls back to the pool and records ioUringFallback (AC-11). O_DIRECT is used for the
// unbuffered path; an O_DIRECT open/IO EINVAL silently falls back to buffered (AC-12), handled by
// the pool for the fallback file and by an inline buffered fd for sub-granularity tails.

#if defined(__linux__)

#include "disk_io_backend_pool.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
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

#if __has_include(<liburing.h>)
#define FC_HAVE_LIBURING 1
#include <liburing.h>
#else
#define FC_HAVE_LIBURING 0
#endif

namespace fc::io {

#if FC_HAVE_LIBURING
namespace {

struct UringFile {
    std::string path;
    int fd = -1;
    OpKind mode = OpKind::Read;
    bool unbuffered = false;
    uint64_t expectedSize = 0;
    int64_t modifyNs = 0;    // W-01: mtime to stamp on the write fd at close (if hasModify)
    bool hasModify = false;  // W-01: set by setWriteModifyTime; false => close skips futimens
    AlignInfo align;
};

// See disk_io_backend_posix.cpp ToTimespecFromNs: normalize a manifest mtime to a POSIX timespec for
// futimens (W-01/FR-01 / C3). Direction mirrors file_index.cpp::NormalizeManifestMtimeToUnixNs:
// >5e17 is Unix ns (passthrough); [1.16e17, 5e17] is FILETIME ticks (-> Unix ns); 0/negative pass
// through (clamped to 0).
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

// A submitted op kept alive until its cqe is reaped (buffer lifetime spans the ring op).
struct UringOp {
    IoRequest req;
    void* aligned = nullptr;   // aligned bounce buffer (O_DIRECT); null => buffered path handled inline
    uint32_t alignedLen = 0;
};

class UringBackend : public PlatformIoBackend {
public:
    explicit UringBackend(const IoDriverConfig& cfg, io_uring ring) : cfg_(cfg), ring_(ring) {
        pump_ = std::thread([this] { PumpLoop(); });
    }
    ~UringBackend() override { shutdown(); }

    BackendKind kind() const override { return BackendKind::LinuxUring; }
    AlignInfo queryAlign(const std::string& path) override { return QueryAlign(path); }

    uint64_t openFile(const std::string& path, OpKind mode, bool unbuffered,
                      uint64_t expectedSize) override {
        UringFile uf;
        uf.path = path;
        uf.mode = mode;
        uf.expectedSize = expectedSize;
        uf.align = QueryAlign(path);
        // unbuffered-writes M4/FR-13/D-02: small-file whole-file downgrade kept for reads only.
        // Write opens with unbuffered intent stay unbuffered regardless of size; unaligned fragments
        // fall back per-op in IssueOrInline. Change 3 (fastcheck-redundant-syscall-elim §3.3.4/
        // FR-25): the read direct-IO gate is decoupled from expectedSize so reads stay buffered even
        // though callers began passing a positive expectedSize, preserving the current direct/
        // buffered strategy and smallFileFallback count (reads were already buffered at expectedSize==0).
        const bool wantUnbuffered =
            unbuffered && !cfg_.forceBuffered &&
            (mode == OpKind::Read ? false : true);
        if (unbuffered && !wantUnbuffered && mode == OpKind::Read) {
            counters_.smallFileFallback.fetch_add(1);
        }
        const int base = (mode == OpKind::Write) ? (O_WRONLY | O_CREAT) : O_RDONLY;
        int fd = -1;
        if (wantUnbuffered) {
            fd = ::open(path.c_str(), base | O_DIRECT | O_CLOEXEC, 0644);
            if (fd >= 0) {
                uf.unbuffered = true;
            } else {
                counters_.bufferedFallback.fetch_add(1);
            }
        }
        if (fd < 0) {
            fd = ::open(path.c_str(), base | O_CLOEXEC, 0644);
            if (fd >= 0) {
                ::posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
            }
        }
        if (fd < 0) {
            return 0;
        }
        uf.fd = fd;
        std::lock_guard<std::mutex> lk(mu_);
        const uint64_t id = nextId_++;
        files_.emplace(id, std::move(uf));
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
        UringFile uf;
        {
            std::lock_guard<std::mutex> lk(mu_);
            auto it = files_.find(fileId);
            if (it == files_.end()) {
                return false;  // unknown / already closed (W-01 idempotency)
            }
            uf = std::move(it->second);
            files_.erase(it);
        }
        bool ok = true;
        // W-01 order (R-01): ftruncate -> (optional) futimens -> close, all on the same write fd.
        // Exact final size on every write close (ofstream-trunc-equivalent overwrite; mirrors the
        // posix pool backend so a pre-existing target is trimmed to expectedSize, incl. 0).
        if (uf.mode == OpKind::Write && uf.fd >= 0) {
            if (::ftruncate(uf.fd, static_cast<off_t>(uf.expectedSize)) != 0) {
                ok = false;  // ok1
            }
            if (uf.hasModify) {
                timespec ts[2];
                ts[0].tv_sec = 0;
                ts[0].tv_nsec = UTIME_OMIT;         // leave atime untouched
                ts[1] = ToTimespecFromNs(uf.modifyNs);  // mtime
                if (::futimens(uf.fd, ts) != 0) {
                    ok = false;  // ok2
                }
            }
        } else if (uf.mode == OpKind::Write) {
            ok = false;  // write fd vanished; cannot finalize
        }
        if (uf.fd >= 0) {
            if (::close(uf.fd) != 0) {
                ok = false;  // ok3
            }
        }
        return ok;
    }

    bool submit(IoRequest&& req) override {
        {
            std::lock_guard<std::mutex> lk(qmu_);
            pending_.push_back(std::move(req));
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
        if (pump_.joinable()) {
            pump_.join();
        }
        io_uring_queue_exit(&ring_);
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
    void PumpLoop() {
        const uint32_t depth = cfg_.backendConcurrency == 0 ? 1u : cfg_.backendConcurrency;
        for (;;) {
            std::deque<IoRequest> batch;
            {
                std::unique_lock<std::mutex> lk(qmu_);
                // With nothing in flight there is no ring work to reap, so sleep until a submission
                // arrives (or we stop) instead of polling. When ops ARE in flight we skip this wait
                // and block on the ring in WaitAndReap below, so completions surface with minimal
                // latency (replaces the old 50ms poll) and shutdown does not busy-spin.
                if (inflight_ == 0) {
                    qcv_.wait(lk, [this] { return stop_ || !pending_.empty(); });
                }
                if (stop_ && pending_.empty() && inflight_ == 0) {
                    return;
                }
                while (!pending_.empty() && inflight_ < depth) {
                    batch.push_back(std::move(pending_.front()));
                    pending_.pop_front();
                    ++inflight_;
                }
            }
            for (auto& r : batch) {
                IssueOrInline(std::move(r));
            }
            if (inflight_ > 0) {
                WaitAndReap();
            }
        }
    }

    // Block on the ring for the next completion (bounded), then drain any others non-blocking. The
    // short timeout bounds how long newly-submitted pending work waits before the pump loops back
    // to issue it; a ready completion wakes the wait immediately.
    void WaitAndReap() {
        io_uring_cqe* cqe = nullptr;
        __kernel_timespec ts{};
        ts.tv_sec = 0;
        ts.tv_nsec = 2 * 1000 * 1000;  // 2 ms
        const int rc = io_uring_wait_cqe_timeout(&ring_, &cqe, &ts);
        if (rc == 0 && cqe != nullptr) {
            auto* op = static_cast<UringOp*>(io_uring_cqe_get_data(cqe));
            const int res = cqe->res;
            io_uring_cqe_seen(&ring_, cqe);
            if (IsRealOp(op)) {
                FinishRing(op, res);
                --inflight_;
            }
        }
        Rechecks();  // drain any remaining ready completions
    }

    // Try to issue via the ring (aligned O_DIRECT / buffered whole-file); if the op cannot use the
    // ring cleanly (unaligned tail on an O_DIRECT fd) do it inline with a buffered fd.
    void IssueOrInline(IoRequest&& req) {
        UringFile uf;
        {
            std::lock_guard<std::mutex> lk(mu_);
            auto it = files_.find(req.fileId);
            if (it == files_.end()) {
                Complete(req, IoStatus::Error, 0, {});
                --inflight_;
                return;
            }
            uf = it->second;
        }
        const uint32_t g = uf.align.ioGranularity;
        const bool needAlign = uf.unbuffered;
        const bool aligned = !needAlign || (IsAligned(req.offset, g) && IsAligned(req.length, g));
        if (!aligned) {
            counters_.tailZeroFallback.fetch_add(1);
            InlineBuffered(uf, req);
            --inflight_;
            return;
        }

        auto op = new UringOp();
        op->req = std::move(req);
        uint8_t* ioBuf = nullptr;
        if (needAlign) {
            counters_.directIo.fetch_add(1);
            op->alignedLen = op->req.length;
            op->aligned = AlignedAlloc(g, op->alignedLen);
            if (!op->aligned) {
                Complete(op->req, IoStatus::Error, 0, {});
                delete op;
                --inflight_;
                return;
            }
            ioBuf = static_cast<uint8_t*>(op->aligned);
            if (op->req.kind == OpKind::Write) {
                std::memcpy(ioBuf, op->req.data.data(), op->req.data.size());
            }
        } else {
            counters_.bufferedFallback.fetch_add(1);
            if (op->req.kind == OpKind::Read) {
                op->req.data.resize(op->req.length);
            }
            ioBuf = op->req.kind == OpKind::Read ? op->req.data.data() : op->req.data.data();
        }

        io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
        if (!sqe) {
            InlineBuffered(uf, op->req);
            if (op->aligned) {
                AlignedFree(op->aligned);
            }
            delete op;
            --inflight_;
            return;
        }
        if (op->req.kind == OpKind::Read) {
            io_uring_prep_read(sqe, uf.fd, ioBuf, op->req.length, op->req.offset);
        } else {
            io_uring_prep_write(sqe, uf.fd, ioBuf, static_cast<unsigned>(op->req.data.size()),
                                op->req.offset);
        }
        io_uring_sqe_set_data(sqe, op);
        io_uring_submit(&ring_);
    }

    // True when the CQE carries one of our submitted UringOp*; false for the internal timeout CQE
    // that some liburing versions surface from io_uring_wait_cqe_timeout (user_data == (__u64)-1).
    // Modern liburing consumes that internally so this never trips there; kept as a cross-version
    // guard so a sentinel is never dereferenced as a UringOp.
    static bool IsRealOp(const UringOp* op) {
        return op != nullptr && reinterpret_cast<uintptr_t>(op) != static_cast<uintptr_t>(-1);
    }

    void Rechecks() {
        io_uring_cqe* cqe = nullptr;
        // Non-blocking peek loop so the pump can keep issuing new work.
        while (io_uring_peek_cqe(&ring_, &cqe) == 0 && cqe != nullptr) {
            auto* op = static_cast<UringOp*>(io_uring_cqe_get_data(cqe));
            const int res = cqe->res;
            io_uring_cqe_seen(&ring_, cqe);
            if (IsRealOp(op)) {
                FinishRing(op, res);
                --inflight_;
            }
        }
    }

    void FinishRing(UringOp* op, int res) {
        IoRequest& req = op->req;
        if (res < 0) {
            // EINVAL on a direct IO: silent buffered fallback preserving bytes (AC-12).
            if (res == -EINVAL) {
                UringFile uf;
                {
                    std::lock_guard<std::mutex> lk(mu_);
                    auto it = files_.find(req.fileId);
                    if (it != files_.end()) {
                        uf = it->second;
                    }
                }
                counters_.tailZeroFallback.fetch_add(1);
                InlineBuffered(uf, req);
            } else {
                Complete(req, IoStatus::Error, 0, {});
            }
        } else if (req.kind == OpKind::Read) {
            std::vector<uint8_t> data(static_cast<size_t>(res));
            if (op->aligned && res > 0) {
                std::memcpy(data.data(), op->aligned, static_cast<size_t>(res));
            } else if (!op->aligned && res >= 0) {
                data.assign(req.data.begin(), req.data.begin() + res);
            }
            const IoStatus st =
                static_cast<uint32_t>(res) < req.length ? IoStatus::Eof : IoStatus::Ok;
            Complete(req, st, static_cast<uint32_t>(res), std::move(data));
        } else {
            Complete(req, IoStatus::Ok, static_cast<uint32_t>(res), {});
        }
        if (op->aligned) {
            AlignedFree(op->aligned);
        }
        delete op;
    }

    void InlineBuffered(const UringFile& uf, IoRequest& req) {
        const int base = (uf.mode == OpKind::Write) ? (O_WRONLY | O_CREAT) : O_RDONLY;
        int bfd = ::open(uf.path.c_str(), base | O_CLOEXEC, 0644);
        if (bfd < 0) {
            Complete(req, IoStatus::Error, 0, {});
            return;
        }
        ::posix_fadvise(bfd, 0, 0, POSIX_FADV_SEQUENTIAL);
        counters_.bufferedFallback.fetch_add(1);
        if (req.kind == OpKind::Read) {
            std::vector<uint8_t> data(req.length);
            const ssize_t r = ::pread(bfd, data.data(), req.length, static_cast<off_t>(req.offset));
            ::close(bfd);
            if (r < 0) {
                Complete(req, IoStatus::Error, 0, {});
            } else {
                data.resize(static_cast<size_t>(r));
                Complete(req, static_cast<uint32_t>(r) < req.length ? IoStatus::Eof : IoStatus::Ok,
                         static_cast<uint32_t>(r), std::move(data));
            }
        } else {
            const ssize_t r = ::pwrite(bfd, req.data.data(), req.data.size(),
                                       static_cast<off_t>(req.offset));
            ::close(bfd);
            Complete(req, r < 0 ? IoStatus::Error : IoStatus::Ok,
                     r < 0 ? 0 : static_cast<uint32_t>(r), {});
        }
    }

    void Complete(const IoRequest& req, IoStatus status, uint32_t transferred,
                  std::vector<uint8_t>&& data) {
        IoCompletion c;
        c.kind = req.kind;
        c.fileId = req.fileId;
        c.offset = req.offset;
        c.requested = req.length;
        c.transferred = transferred;
        c.status = status;
        c.userTag = req.userTag;
        c.data = std::move(data);
        {
            std::lock_guard<std::mutex> lk(cmu_);
            completions_.push_back(std::move(c));
        }
        ccv_.notify_all();
    }

    IoDriverConfig cfg_;
    io_uring ring_;
    std::thread pump_;

    std::mutex mu_;
    std::unordered_map<uint64_t, UringFile> files_;
    uint64_t nextId_ = 1;

    std::mutex qmu_;
    std::condition_variable qcv_;
    std::deque<IoRequest> pending_;
    bool stop_ = false;
    uint32_t inflight_ = 0;

    std::mutex cmu_;
    std::condition_variable ccv_;
    std::deque<IoCompletion> completions_;

    PoolCounters counters_;
};

}  // namespace
#endif  // FC_HAVE_LIBURING

std::unique_ptr<PlatformIoBackend> CreatePlatformBackend(const IoDriverConfig& cfg) {
#if FC_HAVE_LIBURING
    io_uring ring;
    const unsigned depth = cfg.backendConcurrency == 0 ? 1u : cfg.backendConcurrency;
    const int rc = io_uring_queue_init(depth * 2, &ring, 0);
    if (rc == 0) {
        return std::make_unique<UringBackend>(cfg, ring);  // io_uring available (AC-10)
    }
    // Runtime probe failed (-ENOSYS / -EPERM / resource exhaustion): fall back to the pool and
    // record ioUringFallback=1 so counters()/FR-30 observe the degradation (AC-11).
    return CreatePosixPoolBackend(cfg, /*ioUringFallback=*/true);
#else
    // No liburing at build time: pool only (design section 7.3 compile guard). This is still an io_uring
    // fallback from the caller's perspective, so record ioUringFallback=1 (AC-11).
    return CreatePosixPoolBackend(cfg, /*ioUringFallback=*/true);
#endif
}

}  // namespace fc::io

#endif  // __linux__
