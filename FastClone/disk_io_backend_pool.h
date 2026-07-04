#pragma once

// Internal factory for the bounded pread/pwrite thread-pool backend (disk_io_backend_posix.cpp).
// Used directly on macOS and as the io_uring runtime-probe fallback on Linux (design D-05). Kept
// out of disk_io_backend.h because it is POSIX-only.

#include "disk_io_backend.h"

#if defined(__APPLE__) || defined(__linux__)
#include <atomic>

namespace fc::io {

// Shared atomic counter block for the POSIX pool and the io_uring backend (which reuses the pool
// as its runtime-probe fallback, design D-05). Kept here so both TUs agree on the layout.
struct PoolCounters {
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
};

// `ioUringFallback` seeds counters().ioUringFallback to 1 when the pool is created as the Linux
// io_uring fallback (io_uring_queue_init failed or liburing absent, AC-11). macOS / normal-pool
// callers leave it false.
std::unique_ptr<PlatformIoBackend> CreatePosixPoolBackend(const IoDriverConfig& cfg,
                                                          bool ioUringFallback = false);

}  // namespace fc::io
#endif
