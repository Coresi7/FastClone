#pragma once

// Platform IO backend abstraction for the unified disk IO driver (unified-disk-io-driver design
// section 7.1 matrix, FR-03..FR-10). Each platform provides ONE concrete backend, selected at compile
// time (design D-04, strong isolation so macOS never compiles an O_DIRECT / POSIX-aio branch):
//   Windows : IOCP + FILE_FLAG_NO_BUFFERING            (disk_io_backend_win.cpp)
//   Linux   : io_uring (runtime probe) + O_DIRECT       (disk_io_backend_uring.cpp)
//             -> falls back to bounded pread/pwrite pool (disk_io_backend_posix.cpp)
//   macOS   : F_NOCACHE + bounded pread/pwrite pool      (disk_io_backend_posix.cpp)
// The driver (disk_io_driver.*) owns scheduling/fairness/backpressure/cancellation; a backend only
// executes submitted ops with multiple ops in flight and reaps completions in batches.
//
// Buffer ownership is by value (std::vector), so lifetime is RAII and bounded by queue depth x
// chunk size - never by whole-file size (NFR memory / AC-04). Reads move their bytes out in the
// completion; writes move their source bytes into the request.

#include "disk_io_align.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace fc::io {

enum class OpKind : uint8_t { Read, Write };
enum class Prio : uint8_t { Small, Large };  // small ops (hash/sig/verify reads) jump ahead (FR-26)
enum class IoStatus : uint8_t { Ok, Eof, Cancelled, Error };
enum class BackendKind : uint8_t { WinIocp, LinuxUring, PosixThreadPool, Mock };

// One submitted operation. `fileId` is a backend-open file handle id (see openFile).
struct IoRequest {
    OpKind kind = OpKind::Read;
    uint64_t fileId = 0;
    uint64_t offset = 0;
    uint32_t length = 0;           // read: bytes to read; write: must equal data.size()
    Prio prio = Prio::Large;
    uint64_t userTag = 0;          // opaque caller correlation (file/chunk id)
    std::vector<uint8_t> data;     // write: source bytes (moved in); read: ignored/empty
};

// Exactly one completion is produced per submitted op (FR-14: no dropped/duplicate completions).
struct IoCompletion {
    OpKind kind = OpKind::Read;
    uint64_t fileId = 0;
    uint64_t offset = 0;
    uint32_t requested = 0;
    uint32_t transferred = 0;      // short read/write expressed here (FR-14)
    IoStatus status = IoStatus::Ok;
    uint64_t userTag = 0;
    std::vector<uint8_t> data;     // read: transferred bytes; write: empty
};

// Backend-observable counters surfaced through the driver (FR-30). All monotonic.
struct BackendCounters {
    uint64_t directIo = 0;          // ops issued on an unbuffered handle (NO_BUFFERING/O_DIRECT/F_NOCACHE)
    uint64_t bufferedFallback = 0;  // ops issued on the buffered fallback handle
    uint64_t smallFileFallback = 0; // files routed to buffered because size < kSmallFileBufferedMax
    uint64_t tailZeroFallback = 0;  // EOF sub-granularity tails handled by the buffered/truncate path
    uint64_t ioUringFallback = 0;   // set to 1 when io_uring probe failed and the pool was used
};

// Config shared by the driver and backends.
struct IoDriverConfig {
    uint32_t backendConcurrency = 4;  // in-flight ops / worker threads for the pool backends
    uint32_t maxReadQueue = 256;
    uint32_t maxWriteQueue = 256;
    uint32_t readWeight = 1;          // fair-share credits (design section 3.2 / D-03)
    uint32_t writeWeight = 1;
    uint32_t chunkBytes = 1u << 20;   // 1 MiB read-ahead / write chunk granularity
    uint32_t maxInFlight = 16;        // cap on submitted-not-completed ops (bounds memory, FR-12)
    bool forceBuffered = false;       // tests: force the buffered path regardless of size
    bool recordSchedule = false;      // tests: record submitted-op direction order (AC-19/20)
};

class PlatformIoBackend {
public:
    virtual ~PlatformIoBackend() = default;

    virtual BackendKind kind() const = 0;

    // Runtime alignment for the volume hosting `path` (metadata query, not driver IO).
    virtual AlignInfo queryAlign(const std::string& path) = 0;

    // Open a file for read or write. `expectedSize` (write path) lets the backend set the final
    // size exactly when using an unbuffered/truncate tail strategy. Returns 0 on failure else a
    // nonzero file id. `unbuffered` is a hint; the backend may silently fall back to buffered
    // (small files, unsupported FS, O_DIRECT EINVAL) without changing observable results.
    virtual uint64_t openFile(const std::string& path, OpKind mode, bool unbuffered,
                              uint64_t expectedSize) = 0;
    virtual void closeFile(uint64_t fileId) = 0;

    // Submit one op; multiple ops may be in flight at once (FR-12). Returns false only on a hard
    // backend failure (the driver then emits an Error completion for the op).
    virtual bool submit(IoRequest&& req) = 0;

    // Reap up to `max` completions, blocking up to timeoutMs for the first (0 = non-blocking).
    virtual size_t reap(std::vector<IoCompletion>& out, size_t max, int timeoutMs) = 0;

    // Cancel/quiesce: stop accepting new ops, drain in-flight to completions, then join threads.
    virtual void shutdown() = 0;

    virtual BackendCounters counters() const = 0;
};

// Factory: create the correct backend for the current platform (design D-04/D-05). On Linux it
// runtime-probes io_uring and falls back to the pread/pwrite pool.
std::unique_ptr<PlatformIoBackend> CreatePlatformBackend(const IoDriverConfig& cfg);

}  // namespace fc::io
