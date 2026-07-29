// Windows IOCP backend with FILE_FLAG_NO_BUFFERING (unified-disk-io-driver design section 3.4.1 / section 7.2,
// FR-03, AC-08/09). Compiles ONLY on Windows and is empty elsewhere.
//
// Large files (>= kSmallFileBufferedMax) are opened FILE_FLAG_OVERLAPPED | FILE_FLAG_NO_BUFFERING
// and associated with an IO completion port; reads/writes are posted with OVERLAPPED and reaped in
// batches via GetQueuedCompletionStatusEx, so many ops are in flight at once (AC-09). Small files
// and any sector-unaligned op fall back to a buffered OVERLAPPED handle (FR-11). Sector/page sizes
// are taken at runtime from disk_io_align (no hard-coded 512/4096, AC-14). Unbuffered writes use a
// zero-filled aligned bounce so no stale disk data is exposed (R-05/N2); the exact final size is
// restored with SetEndOfFile at close (EOF sub-sector tail handling, FR-11).

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "disk_io_backend.h"

#include <atomic>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace fc::io {
namespace {

std::wstring Widen(const std::string& s) {
    if (s.empty()) {
        return std::wstring();
    }
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// Convert a manifest modify time to a Windows FILETIME. The raw value may be either Unix
// nanoseconds (legacy) or raw FILETIME ticks (100ns since 1601). This MUST stay byte-for-byte
// identical to file_index.cpp's ToFileTimeFromNs (optimize-small-file-write-path W-01/C3): the two
// copies exist only so this backend TU does not have to pull mtime conversion out of file_index.h
// (which would leak <windows.h>). If you change one, change the other; V-05 asserts equivalence.
FILETIME ToFileTimeFromNsWin(int64_t unixNs) {
    constexpr int64_t kLikelyUnixNsThreshold = 500000000000000000LL;
    constexpr int64_t kWindowsEpochDiff100ns = 116444736000000000LL;
    int64_t ticks = unixNs;
    if (unixNs > kLikelyUnixNsThreshold) {
        ticks = (unixNs / 100LL) + kWindowsEpochDiff100ns;
    }
    ULARGE_INTEGER v{};
    v.QuadPart = static_cast<ULONGLONG>(ticks < 0 ? 0 : ticks);
    FILETIME ft{};
    ft.dwLowDateTime = v.LowPart;
    ft.dwHighDateTime = v.HighPart;
    return ft;
}

struct WinFile {
    HANDLE hUnbuf = INVALID_HANDLE_VALUE;
    HANDLE hBuf = INVALID_HANDLE_VALUE;
    std::wstring wpath;
    OpKind mode = OpKind::Read;
    bool unbuffered = false;
    uint64_t expectedSize = 0;   // write: exact final size to SetEndOfFile at close
    uint64_t fileSize = 0;       // read: size for tail clamping
    int64_t modifyNs = 0;        // W-01: mtime to stamp on the write handle at close (if hasModify)
    bool hasModify = false;      // W-01: set by setWriteModifyTime; false => close skips SetFileTime
    AlignInfo align;
};

// Per-op context; `ov` MUST be the first member so CONTAINING_RECORD recovers it from a packet.
struct OpCtx {
    OVERLAPPED ov{};
    IoRequest req;
    HANDLE hFile = INVALID_HANDLE_VALUE;
    uint8_t* buf = nullptr;   // aligned bounce (AlignedAlloc); freed on reap
    uint32_t ioLen = 0;       // physical length submitted (sector-multiple on the unbuffered path)
    uint32_t logicalLen = 0;  // bytes the caller asked for
    bool onUnbuffered = false;
};

class WinIocpBackend : public PlatformIoBackend {
public:
    explicit WinIocpBackend(const IoDriverConfig& cfg) : cfg_(cfg) {
        port_ = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0);
    }
    ~WinIocpBackend() override { shutdown(); }

    BackendKind kind() const override { return BackendKind::WinIocp; }
    AlignInfo queryAlign(const std::string& path) override { return QueryAlign(path); }

    uint64_t openFile(const std::string& path, OpKind mode, bool unbuffered,
                      uint64_t expectedSize) override {
        WinFile wf;
        wf.wpath = Widen(path);
        wf.mode = mode;
        wf.expectedSize = expectedSize;
        wf.align = QueryAlign(path);

        // Change 3 (fastcheck-redundant-syscall-elim, FR-15/FR-16): on the read path, a positive
        // expectedSize is the caller's already-known read size/bound, so use it directly and skip
        // the redundant FileSizeOnDisk (GetFileAttributesExW) metadata syscall. expectedSize==0
        // keeps the old read behaviour (query FileSizeOnDisk; 0 stays the unknown/failure sentinel
        // and still drives the small-file buffered fallback + tail clamp). Write mode is different:
        // expectedSize is always the exact final SetEndOfFile size, including a valid 0-byte file.
        const bool sizeKnown = (mode == OpKind::Write) || (mode == OpKind::Read) || expectedSize > 0;
        uint64_t sizeForPolicy;
        if (mode == OpKind::Read) {
            sizeForPolicy = (expectedSize > 0) ? expectedSize : FileSizeOnDisk(wf.wpath);
        } else {
            sizeForPolicy = expectedSize;
        }
        wf.fileSize = (mode == OpKind::Read) ? sizeForPolicy : 0;
        // optimize-small-file-write-path W-02/FR-04/FR-05/D-02: the small-file
        // (< kSmallFileBufferedMax) whole-file downgrade now covers BOTH read and write. A write
        // whose exact size is known and below the threshold opens only the buffered handle -- no
        // unbuffered (NO_BUFFERING) handle, no second IOCP association -- since a single-shot small
        // write gets no benefit from direct IO and pays double open + IOCP cost. Large writes
        // (>= threshold) keep the unbuffered intent; sub-sector tails and unaligned middle
        // fragments still fall back per-op in submit() (D-03), so bytes stay exact (FR-06/C2).
        const bool wantUnbuf = unbuffered && !cfg_.forceBuffered && sizeKnown &&
                               (sizeForPolicy >= kSmallFileBufferedMax);
        if (unbuffered && !wantUnbuf && sizeKnown) {
            counters_.smallFileFallback.fetch_add(1);
        }

        const DWORD access = (mode == OpKind::Write) ? GENERIC_WRITE : GENERIC_READ;
        // unbuffered-writes D-03: the write path may hold BOTH an unbuffered (hUnbuf) and a buffered
        // (hBuf) handle to the same file at once (aligned ops go unbuffered, unaligned middle/tail
        // fragments go buffered). The second open therefore needs FILE_SHARE_WRITE, otherwise it
        // fails with a sharing violation and unaligned fragments would hit an invalid handle.
        //
        // read-share-widen: reads now grant FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE
        // (the same permissive mode file_index.cpp already uses for metadata scanning) instead of
        // FILE_SHARE_READ only. Windows sharing is checked against EVERY existing handle's access:
        // if another process holds e.g. a GENERIC_READ|GENERIC_WRITE handle (Office, editors,
        // loggers, a concurrent reader...), a read that grants only FILE_SHARE_READ is rejected with
        // ERROR_SHARING_VIOLATION even though we only want to read. Widening the granted share lets
        // us read files that are merely open (not exclusively locked) by another process. It has no
        // effect on unoccupied files and zero runtime cost. The only new risk is that a concurrent
        // writer/deleter can mutate the file mid-read, yielding a possibly inconsistent copy -- an
        // accepted best-effort trade-off for a copy/backup tool.
        const DWORD share = (mode == OpKind::Write)
                                ? (FILE_SHARE_READ | FILE_SHARE_WRITE)
                                : (FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE);
        const DWORD disp = (mode == OpKind::Write) ? OPEN_ALWAYS : OPEN_EXISTING;

        if (wantUnbuf) {
            wf.hUnbuf = CreateFileW(wf.wpath.c_str(), access, share, nullptr, disp,
                                    FILE_FLAG_OVERLAPPED | FILE_FLAG_NO_BUFFERING, nullptr);
            if (wf.hUnbuf != INVALID_HANDLE_VALUE) {
                wf.unbuffered = true;
                CreateIoCompletionPort(wf.hUnbuf, port_, 0, 0);
            } else {
                counters_.bufferedFallback.fetch_add(1);
            }
        }
        // Always have a buffered overlapped handle for small files / tails / unaligned ops.
        wf.hBuf = CreateFileW(wf.wpath.c_str(), access, share, nullptr, disp,
                              FILE_FLAG_OVERLAPPED, nullptr);
        if (wf.hBuf == INVALID_HANDLE_VALUE && wf.hUnbuf == INVALID_HANDLE_VALUE) {
            return 0;
        }
        if (wf.hBuf != INVALID_HANDLE_VALUE) {
            CreateIoCompletionPort(wf.hBuf, port_, 0, 0);
        }
        std::lock_guard<std::mutex> lk(mu_);
        const uint64_t id = nextId_++;
        files_.emplace(id, std::move(wf));
        return id;
    }

    void setWriteModifyTime(uint64_t fileId, int64_t modifyNs) override {
        // W-01/FR-01: only record; the actual SetFileTime happens on the still-open write handle in
        // closeFile, so a successful whole-file write never re-opens the target to stamp mtime.
        std::lock_guard<std::mutex> lk(mu_);
        auto it = files_.find(fileId);
        if (it == files_.end() || it->second.mode != OpKind::Write) {
            return;
        }
        it->second.modifyNs = modifyNs;
        it->second.hasModify = true;
    }

    bool closeFile(uint64_t fileId) override {
        WinFile wf;
        {
            std::lock_guard<std::mutex> lk(mu_);
            auto it = files_.find(fileId);
            if (it == files_.end()) {
                return false;  // unknown / already closed: no side effect (W-01 idempotency)
            }
            wf = it->second;
            files_.erase(it);
        }
        bool ok = true;
        // Restore the exact final size on every write close (unbuffered-writes: this also gives the
        // driver write path ofstream-trunc-equivalent overwrite semantics, incl. truncating a
        // pre-existing target down to an empty/expectedSize file so no stale tail bytes survive).
        // W-01 order (R-01): SetEndOfFile -> (optional) SetFileTime -> CloseHandle, all on the same
        // write handle; the returned bool is the AND of every step so the caller can refuse to count
        // a transfer whose truncate/mtime/close failed (FR-02/B7).
        if (wf.mode == OpKind::Write) {
            HANDLE h = wf.hBuf != INVALID_HANDLE_VALUE ? wf.hBuf : wf.hUnbuf;
            if (h != INVALID_HANDLE_VALUE) {
                LARGE_INTEGER li;
                li.QuadPart = static_cast<LONGLONG>(wf.expectedSize);
                if (SetFilePointerEx(h, li, nullptr, FILE_BEGIN) && SetEndOfFile(h)) {
                    // ok1
                } else {
                    ok = false;
                }
                if (wf.hasModify) {
                    const FILETIME ft = ToFileTimeFromNsWin(wf.modifyNs);
                    if (!SetFileTime(h, nullptr, nullptr, &ft)) {
                        ok = false;  // ok2
                    }
                }
            } else {
                ok = false;  // write handle vanished; cannot finalize
            }
        }
        if (wf.hUnbuf != INVALID_HANDLE_VALUE) {
            if (!CloseHandle(wf.hUnbuf)) {
                ok = false;  // ok3
            }
        }
        if (wf.hBuf != INVALID_HANDLE_VALUE) {
            if (!CloseHandle(wf.hBuf)) {
                ok = false;  // ok3
            }
        }
        return ok;
    }

    bool submit(IoRequest&& req) override {
        WinFile wf;
        {
            std::lock_guard<std::mutex> lk(mu_);
            auto it = files_.find(req.fileId);
            if (it == files_.end()) {
                EmitDirect(req, IoStatus::Error, 0, {});
                return true;
            }
            wf = it->second;
        }
        const uint32_t g = wf.align.ioGranularity;
        const bool offAligned = IsAligned(req.offset, g);
        // unbuffered-writes D-03 (R-01): a write may use the unbuffered handle only when BOTH offset
        // and length are sector-aligned, OR it is the EOF tail (offset+length == expectedSize) which
        // is safely zero-padded and trimmed by SetEndOfFile at close. An unaligned MIDDLE fragment
        // (delta copy/range) must NOT go unbuffered: AlignUp+zero-pad would clobber the adjacent
        // sector bytes past its logical range. Such fragments fall back to the buffered handle with
        // an EXACT length write (no padding). Reads keep the existing offset-aligned rule.
        bool useUnbuf;
        if (req.kind == OpKind::Write) {
            const bool lenAligned = IsAligned(req.length, g);
            const bool isEofTail = (wf.expectedSize > 0) &&
                                   (req.offset + req.length == wf.expectedSize);
            useUnbuf = wf.unbuffered && offAligned && (lenAligned || isEofTail);
        } else {
            useUnbuf = wf.unbuffered && offAligned;
        }

        auto* ctx = new OpCtx();
        ctx->logicalLen = req.length;
        ctx->onUnbuffered = useUnbuf;
        ctx->hFile = useUnbuf ? wf.hUnbuf : wf.hBuf;

        if (req.kind == OpKind::Read) {
            uint64_t want = req.length;
            if (useUnbuf && wf.fileSize > req.offset) {
                const uint64_t avail = wf.fileSize - req.offset;
                if (want > avail) {
                    want = avail;  // clamp tail; NO_BUFFERING returns the partial final sector
                }
            }
            ctx->ioLen = useUnbuf ? static_cast<uint32_t>(AlignUp(want, g)) : req.length;
            if (ctx->ioLen == 0) {
                ctx->ioLen = useUnbuf ? g : req.length;
            }
            ctx->buf = static_cast<uint8_t*>(AlignedAlloc(g, ctx->ioLen ? ctx->ioLen : g));
        } else {  // Write
            if (useUnbuf) {
                ctx->ioLen = static_cast<uint32_t>(AlignUp(req.length, g));
                ctx->buf = static_cast<uint8_t*>(AlignedAlloc(g, ctx->ioLen));
                if (ctx->buf) {
                    std::memset(ctx->buf, 0, ctx->ioLen);  // zero pad, never expose old data (R-05)
                    std::memcpy(ctx->buf, req.data.data(), req.data.size());
                }
            } else {
                ctx->ioLen = req.length;
                ctx->buf = static_cast<uint8_t*>(AlignedAlloc(g, ctx->ioLen ? ctx->ioLen : g));
                if (ctx->buf) {
                    std::memcpy(ctx->buf, req.data.data(), req.data.size());
                }
            }
        }
        if (!ctx->buf) {
            EmitDirect(req, IoStatus::Error, 0, {});
            delete ctx;
            return true;
        }
        if (ctx->onUnbuffered) {
            counters_.directIo.fetch_add(1);
        } else {
            counters_.bufferedFallback.fetch_add(1);
            // Any op on an unbuffered-capable file that was routed to the buffered handle
            // (unaligned offset, or a sub-sector/unaligned middle write fragment) is a
            // per-op fallback (D-03).
            if (wf.unbuffered && !useUnbuf) {
                counters_.tailZeroFallback.fetch_add(1);
            }
        }
        ctx->req = std::move(req);
        ctx->ov.Offset = static_cast<DWORD>(ctx->req.offset & 0xFFFFFFFFull);
        ctx->ov.OffsetHigh = static_cast<DWORD>(ctx->req.offset >> 32);

        inflight_.fetch_add(1);
        BOOL ok;
        if (ctx->req.kind == OpKind::Read) {
            ok = ReadFile(ctx->hFile, ctx->buf, ctx->ioLen, nullptr, &ctx->ov);
        } else {
            ok = WriteFile(ctx->hFile, ctx->buf, ctx->ioLen, nullptr, &ctx->ov);
        }
        if (!ok) {
            const DWORD e = GetLastError();
            if (e == ERROR_IO_PENDING) {
                return true;  // completion arrives on the port
            }
            if (e == ERROR_HANDLE_EOF) {
                Finish(ctx, IoStatus::Eof, 0);
                return true;
            }
            Finish(ctx, IoStatus::Error, 0);
            return true;
        }
        return true;  // synchronous success STILL posts a packet to the port (default behavior)
    }

    size_t reap(std::vector<IoCompletion>& out, size_t max, int timeoutMs) override {
        // First surface ops that completed synchronously at submit time (EOF / error / bad file id)
        // and never reached the port, so no completion is ever lost (FR-14).
        size_t produced = 0;
        {
            std::lock_guard<std::mutex> lk(directMu_);
            while (!directCompletions_.empty() && produced < max) {
                out.push_back(std::move(directCompletions_.front()));
                directCompletions_.pop_front();
                ++produced;
            }
        }
        if (produced >= max) {
            return produced;
        }
        std::vector<OVERLAPPED_ENTRY> entries(max - produced);
        ULONG removed = 0;
        const DWORD to = timeoutMs < 0 ? 1000u : static_cast<DWORD>(timeoutMs);
        if (!GetQueuedCompletionStatusEx(port_, entries.data(),
                                         static_cast<ULONG>(entries.size()), &removed, to, FALSE)) {
            return produced;  // timeout or no port completions
        }
        for (ULONG i = 0; i < removed; ++i) {
            auto* ctx = CONTAINING_RECORD(entries[i].lpOverlapped, OpCtx, ov);
            DWORD bytes = 0;
            IoStatus st = IoStatus::Ok;
            if (!GetOverlappedResult(ctx->hFile, &ctx->ov, &bytes, FALSE)) {
                const DWORD e = GetLastError();
                st = (e == ERROR_HANDLE_EOF) ? IoStatus::Eof : IoStatus::Error;
                bytes = entries[i].dwNumberOfBytesTransferred;
            }
            BuildCompletion(ctx, st, bytes, out);
            ++produced;
        }
        return produced;
    }

    void shutdown() override {
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (stopped_) {
                return;
            }
            stopped_ = true;
        }
        // Drain any packets still queued for in-flight ops so buffers are freed (no leak, R-04).
        std::vector<IoCompletion> drain;
        while (inflight_.load() > 0) {
            const size_t got = reap(drain, 64, 50);
            drain.clear();
            if (got == 0) {
                break;
            }
        }
        if (port_ != nullptr) {
            CloseHandle(port_);
            port_ = nullptr;
        }
        // Defensive: close any file handles a caller left open so a leaked fileId (e.g. an
        // abandoned download/temp when a session tears down for reconnect) never leaks an OS handle.
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& kv : files_) {
            if (kv.second.hUnbuf != INVALID_HANDLE_VALUE) {
                CloseHandle(kv.second.hUnbuf);
            }
            if (kv.second.hBuf != INVALID_HANDLE_VALUE) {
                CloseHandle(kv.second.hBuf);
            }
        }
        files_.clear();
    }

    BackendCounters counters() const override { return counters_.snapshot(); }

private:
    static uint64_t FileSizeOnDisk(const std::wstring& wpath) {
        WIN32_FILE_ATTRIBUTE_DATA fad;
        if (GetFileAttributesExW(wpath.c_str(), GetFileExInfoStandard, &fad) == 0) {
            return 0;
        }
        return (static_cast<uint64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
    }

    void BuildCompletion(OpCtx* ctx, IoStatus st, DWORD bytes, std::vector<IoCompletion>& out) {
        IoCompletion c;
        c.kind = ctx->req.kind;
        c.fileId = ctx->req.fileId;
        c.offset = ctx->req.offset;
        c.requested = ctx->logicalLen;
        c.userTag = ctx->req.userTag;
        if (st == IoStatus::Error) {
            c.status = IoStatus::Error;
        } else if (ctx->req.kind == OpKind::Read) {
            uint32_t logical = bytes < ctx->logicalLen ? static_cast<uint32_t>(bytes) : ctx->logicalLen;
            c.transferred = logical;
            c.status = (logical < ctx->logicalLen) ? IoStatus::Eof : IoStatus::Ok;
            c.data.assign(ctx->buf, ctx->buf + logical);
        } else {
            uint32_t logical =
                bytes < ctx->logicalLen ? static_cast<uint32_t>(bytes) : ctx->logicalLen;
            c.transferred = logical;
            c.status = IoStatus::Ok;
        }
        out.push_back(std::move(c));
        AlignedFree(ctx->buf);
        delete ctx;
        inflight_.fetch_sub(1);
    }

    // Complete an op that never made it onto the port (submit-time failure / EOF).
    void Finish(OpCtx* ctx, IoStatus st, DWORD bytes) {
        std::vector<IoCompletion> tmp;
        BuildCompletion(ctx, st, bytes, tmp);
        std::lock_guard<std::mutex> lk(directMu_);
        for (auto& c : tmp) {
            directCompletions_.push_back(std::move(c));
        }
    }

    void EmitDirect(const IoRequest& req, IoStatus st, uint32_t transferred,
                    std::vector<uint8_t>&& data) {
        IoCompletion c;
        c.kind = req.kind;
        c.fileId = req.fileId;
        c.offset = req.offset;
        c.requested = req.length;
        c.transferred = transferred;
        c.status = st;
        c.userTag = req.userTag;
        c.data = std::move(data);
        std::lock_guard<std::mutex> lk(directMu_);
        directCompletions_.push_back(std::move(c));
    }

    IoDriverConfig cfg_;
    HANDLE port_ = nullptr;

    std::mutex mu_;
    std::unordered_map<uint64_t, WinFile> files_;
    uint64_t nextId_ = 1;
    bool stopped_ = false;
    std::atomic<uint64_t> inflight_{0};

    std::mutex directMu_;
    std::deque<IoCompletion> directCompletions_;

    struct WinCounters {
        std::atomic<uint64_t> directIo{0}, bufferedFallback{0}, smallFileFallback{0},
            tailZeroFallback{0}, ioUringFallback{0};
        BackendCounters snapshot() const {
            BackendCounters c;
            c.directIo = directIo.load();
            c.bufferedFallback = bufferedFallback.load();
            c.smallFileFallback = smallFileFallback.load();
            c.tailZeroFallback = tailZeroFallback.load();
            c.ioUringFallback = ioUringFallback.load();
            return c;
        }
    } counters_;
};

}  // namespace

std::unique_ptr<PlatformIoBackend> CreatePlatformBackend(const IoDriverConfig& cfg) {
    return std::make_unique<WinIocpBackend>(cfg);
}

}  // namespace fc::io

#endif  // _WIN32
