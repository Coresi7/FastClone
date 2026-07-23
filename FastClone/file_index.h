#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

// Forward declaration only: callers that just need the ComputeFileHashViaDriver signature must not
// be forced to pull in disk_io_backend.h. The definition lives in file_index.cpp, which includes
// disk_io_driver.h.
namespace fc::io {
class DiskIoDriver;
}

namespace fc {

struct FileEntry {
    std::string relativePath;
    bool isDirectory = false;
    uint64_t fileSize = 0;
    int64_t mtimeNs = 0;
};

// Protocol name is kept as Hash256 for compatibility, but value is XXH3_128 (16 bytes).
using Hash256 = std::array<uint8_t, 16>;

std::vector<FileEntry> BuildIndex(const std::filesystem::path& root, const std::optional<std::filesystem::path>& excludeAbsPath);
Hash256 ComputeFileHash(const std::filesystem::path& path);
// Same XXH3-128 digest + raw byte layout as ComputeFileHash, but reads the file content through the
// unified disk IO driver: files <= 256 KiB use one direct driver read + ComputeBufferHash, larger
// files keep the SequentialReader streaming path. Single source of truth shared by the FastClone
// server hash-miss path and the FastCheck client local-hash worker (fastcheck-parallel-hash M1/M3/
// FR-01). Throws std::runtime_error on any open/read failure, matching ComputeFileHash's failure
// contract so callers can keep their existing try/catch handling.
Hash256 ComputeFileHashViaDriver(fc::io::DiskIoDriver& driver, const std::filesystem::path& path,
                                 std::optional<uint64_t> knownSize = std::nullopt);
// Diagnostics: cumulative per-phase wall-clock time spent inside ComputeFileHashViaDriver
// (microseconds, relaxed atomics). Only meaningful in a process that actually calls the function
// (FastCheck client / FastClone server). Intended for the FastCheck progress line to locate where
// the per-file hash time goes (file_size query / openFile / read / XXH3 / closeFile). Zero cost on
// callers that never read it; the atomic adds inside the hot path are ~nanoseconds.
struct HashPhaseTimings {
    uint64_t count = 0;       // number of ComputeFileHashViaDriver calls
    uint64_t totalUs = 0;     // whole-function wall time
    uint64_t fileSizeUs = 0;  // fs::file_size query
    uint64_t openUs = 0;      // driver.openFile
    uint64_t readUs = 0;      // submit+wait+drain (fast path) or SequentialReader loop (slow path)
    uint64_t xxhUs = 0;       // ComputeBufferHash / ComputeHashFromSource
    uint64_t closeUs = 0;     // driver.closeFile
};
HashPhaseTimings GetHashPhaseTimings();
// Same XXH3-128 digest + raw byte layout as ComputeFileHash, but over an in-memory buffer.
// Lets callers that already hold the file bytes (e.g. the delta block-signature path) avoid a
// second full-file read while producing a value the client's ComputeFileHash verify matches.
Hash256 ComputeBufferHash(const uint8_t* data, size_t len);
// Streaming XXH3-128 over a sequential byte source (fills dst up to maxLen, returns the count
// written; 0 means EOF). Produces the exact same Hash256 as ComputeFileHash over the same byte
// sequence -- XXH3 streaming is chunking-independent -- so the unified disk IO driver can supply
// the file content instead of an inline file read (unified-disk-io-driver C9/C10). Read errors are
// surfaced by the caller's source (it returns short/0 and sets its own flag); this function never
// throws on a short source, only on XXH3 state failures.
Hash256 ComputeHashFromSource(const std::function<size_t(uint8_t*, size_t)>& source);
bool HashEquals(const Hash256& a, const Hash256& b);
std::string NormalizeRelativePath(const std::filesystem::path& relativePath);
int64_t ToUnixNs(const std::filesystem::file_time_type& value);
std::filesystem::file_time_type FromUnixNs(int64_t valueNs);
void SetFileModifyTime(const std::filesystem::path& path, int64_t modifyNs);

// S-02 (FR-16 / C3 / D-14-A): normalize a manifest mtime to Unix ns. Pure int64 arithmetic, compiled
// on ALL platforms (no <windows.h> dependency) so it is unit-testable everywhere and shared by the
// POSIX SetFileModifyTime / WriteSmallFileFastPath and the POSIX/uring backends' ToTimespecFromNs.
// Direction mirrors the authoritative compare_phase.cpp::TryNormalizeMtimeToUnixNs:
//   - modifyNs > 5e17: genuine Unix ns (POSIX peer) -> pass through unchanged.
//   - [1.16e17, 5e17]: Windows FILETIME ticks (100ns since 1601) -> (raw - 1.16e17) * 100 ns.
//   - < 1.16e17 (incl. the 0 "unknown" sentinel): pass through; negative clamps to 0.
int64_t NormalizeManifestMtimeToUnixNs(int64_t modifyNs);

// optimize-small-file-write-path W-05: max file size (bytes) eligible for the small-file synchronous
// fast path. Files strictly larger take the DiskIoDriver whole-file write path. Note this is smaller
// than kSmallFileBufferedMax (1 MiB) on purpose (design section 3.5.1).
inline constexpr uint64_t kSmallFileFastPathMax = 256ull * 1024;

// True when a fully-buffered whole-file write of `size` bytes should take the small-file synchronous
// fast path (W-05/FR-13): size <= kSmallFileFastPathMax. So 256 KiB -> true, 256 KiB + 1 -> false.
bool ShouldUseSmallFileFastPath(uint64_t size);

// Write `data` (size bytes) to `path` in one synchronous pass (W-05/FR-13), producing a file
// byte-identical to the DiskIoDriver whole-file path: truncating overwrite (CREATE_ALWAYS /
// O_TRUNC), exact final size (SetEndOfFile / ftruncate), and mtime stamped from modifyNs on the same
// handle before close. The parent directory must already exist (the caller does EnsureParentDir, B6).
// Returns true only when every write/truncate/mtime/close step succeeds; on any failure it returns
// false and leaves the target for the caller's existing retry/fallback path (FR-14/AC-16). `data`
// may be null iff size == 0.
bool WriteSmallFileFastPath(const std::filesystem::path& path, const uint8_t* data, size_t size,
                            int64_t modifyNs);

// Canonical mtime read used by both manifest build and client local probe so the
// two sides compare values in the same unit/epoch.
//  - Windows: raw FILETIME ticks (100ns since 1601), matching the manifest writer;
//    on read failure falls back to ToUnixNs(fs::last_write_time).
//  - POSIX:   ToUnixNs(fs::last_write_time) (both ends already share this unit).
// Returns 0 when the timestamp cannot be read (same failure semantics as the
// previous inline probe logic).
int64_t ReadFileMtimeCanonical(const std::filesystem::path& path);

}  // namespace fc
