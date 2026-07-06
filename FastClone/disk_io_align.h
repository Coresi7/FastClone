#pragma once

// Runtime alignment + aligned allocation for the unified disk IO driver
// (unified-disk-io-driver design section 7.5, FR-08/FR-09/FR-11).
//
// HARD cross-platform constraint (design section 7, task red line): every page size, device logical
// block size and direct-IO granularity below is obtained AT RUNTIME. There is deliberately no
// literal 4096 or 512 used as a page size, sector size, device block size, offset-alignment or
// length-padding constant anywhere in this module (AC-14/AC-15). The only compile-time constant
// is the small-file buffered-fallback threshold, which is a policy knob, not an alignment value.
//
// This header is intentionally free of <windows.h> / <unistd.h>: the platform queries live in
// disk_io_align.cpp so translation units that only need the pure align helpers or the small-file
// threshold do not drag platform headers into their include path.

#include <cstddef>
#include <cstdint>
#include <string>

namespace fc::io {

// Small-file / buffered-fallback threshold (FR-11/D-07). Files strictly smaller than this size
// take the buffered IO path; likewise unaligned head/tail fragments and the EOF sub-granularity
// tail. Centralized here so tests can reason about it (AC-18). This is a policy threshold, NOT an
// alignment constant, so it is not covered by the "no hard-coded 4096/512" rule.
inline constexpr uint64_t kSmallFileBufferedMax = 1ull << 20;  // 1 MiB

// Runtime alignment info (FR-08). All three fields come from the OS at runtime.
struct AlignInfo {
    uint32_t pageSize = 0;         // sysconf(_SC_PAGESIZE) / GetSystemInfo().dwPageSize
    uint32_t deviceBlockSize = 0;  // device logical/sector block size (storage query / statvfs)
    uint32_t ioGranularity = 0;    // = max(pageSize, deviceBlockSize): direct-IO alignment unit
};

// Compute ioGranularity = max(pageSize, deviceBlockSize) and normalize zero inputs to the other
// field (never to a literal). Pure; exposed so the platform query and tests share one rule.
AlignInfo MakeAlignInfo(uint32_t pageSize, uint32_t deviceBlockSize);

// Query alignment for the volume that hosts `path`. The path is used only to select the correct
// volume/device; the query itself reads metadata, not file content, so it does not go through the
// driver (design section 6). When the device block query is unavailable the granularity falls back to the
// runtime page size (still never a literal 512/4096).
AlignInfo QueryAlign(const std::string& path);

// Runtime process page size. Exposed for callers/tests that only need the page size.
uint32_t QueryPageSize();

// Test-only observability probe (Change 2 / AC-20): current entry count of the process-wide
// per-volume QueryAlign cache. Lets the bounded-cache test assert the entry count does not grow
// with file count. Not part of the production API and never called on any hot path (N7/N8 safe).
size_t AlignCacheSizeForTest();

// --- Pure alignment helpers (a must be a power of two; page/block/sector sizes always are). ---
uint64_t AlignDown(uint64_t value, uint32_t alignment);
uint64_t AlignUp(uint64_t value, uint32_t alignment);
bool IsAligned(uint64_t value, uint32_t alignment);

// Platform-matched aligned allocation (FR-09/AC-16): POSIX posix_memalign, Windows _aligned_malloc.
// Returns nullptr on failure. AlignedFree MUST be paired with AlignedAlloc (release path matches
// the allocation path, AC-16). `alignment` must be a power of two and a multiple of sizeof(void*).
void* AlignedAlloc(size_t alignment, size_t size);
void AlignedFree(void* p);

}  // namespace fc::io
