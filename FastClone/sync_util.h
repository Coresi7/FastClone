#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_set>

namespace fc {

std::optional<std::string> ReadEnvVar(const char* name);
bool ParseBoolEnv(const char* name);
bool IsDebugEnabled();
bool IsDiagEnabled();

// True for permanent auth/protocol/desync errors that must not consume reconnect budget.
// Includes: protocol version mismatch, authentication failures, "Server error: ..." frames,
// RecvFrame desync, wrong-direction frames. Transient network errors return false.
//
// NOTE: classification is string-based today. Throw sites are listed in sync_util.cpp.
// A future DisconnectReason enum at each throw site would remove this coupling.
bool IsFatalClientDisconnectReason(const std::string& reason);

#ifdef _WIN32
std::wstring Utf8ToWide(const std::string& value);
std::string WideToUtf8(const std::wstring& value);
// Convert an absolute path to a Windows extended-length ("\\?\") path so file operations
// bypass the legacy MAX_PATH (260) limit. Forward slashes are converted to backslashes
// (the prefix disables path normalization). Relative or already-prefixed paths are
// returned unchanged.
std::wstring ToExtendedLengthPath(const std::filesystem::path& path);
#endif

// create_directories() that is safe on Windows extended-length ("\\?\") paths (and on
// paths exceeding MAX_PATH). On POSIX this is just std::filesystem::create_directories.
void CreateDirectoriesLong(const std::filesystem::path& dir);

// Normalize a raw manifest mtime (which may be Unix ns or Windows FILETIME ticks) to
// Unix nanoseconds. Returns false when the value cannot be meaningfully converted.
// The implementation has moved to compare_phase (M1 fix: single source of truth for mtime normalization, shared by sync and FastCheck).
// This header keeps only the forward declaration for compatibility with existing includes; callers may also directly #include "compare_phase.h".
bool TryNormalizeMtimeToUnixNs(int64_t rawMtime, int64_t& outUnixNs);

std::filesystem::path JoinRel(const std::filesystem::path& root, const std::string& relPath);
void EnsureParentDir(const std::filesystem::path& filePath);

// Per-worker "directory already exists" cache (fastcheck-perf-tune Change 2, FR-09..14). Each write
// worker owns ONE instance; it is NOT thread-safe and MUST NOT be shared across workers (no global
// lock, no cross-worker failure propagation). On a cache hit EnsureParentDir performs zero filesystem
// syscalls; on a miss it goes through CreateDirectoriesLong and, on success, records the parent plus
// all of its ancestors so later files under the same subtree hit the cache.
class PerWorkerDirCache {
public:
    // Parent directory of filePath, or nullopt when there is nothing meaningful to create/cache
    // (empty path, or a volume/UNC/drive root). Mirrors EnsureParentDir's own parent computation so
    // the cache keys line up exactly with the directories CreateDirectoriesLong would build.
    static std::optional<std::filesystem::path> ParentDirOf(const std::filesystem::path& filePath);

    // True if dir is already known-present in THIS worker's cache (no filesystem syscall).
    bool contains(const std::filesystem::path& dir) const;

    // Record dir and all of its ancestor directories (down to, but excluding, the volume/UNC/drive
    // root) as known-present in this worker's cache. Ancestor splitting reuses CreateDirectoriesLong's
    // volume-root logic so the keys match a later contains() query for the same subtree.
    void addWithAncestors(const std::filesystem::path& dir);

    void clear() { known_.clear(); }
    std::size_t size() const { return known_.size(); }

private:
#ifdef _WIN32
    using Key = std::wstring;
#else
    using Key = std::string;
#endif
    static Key KeyOf(const std::filesystem::path& dir);
    std::unordered_set<Key> known_;
};

// Cache-aware core of EnsureParentDir. On a cache hit it performs ZERO filesystem syscalls. On a miss
// it calls ensure(parentDir) -> bool (true == the directory now exists) and, on success, records the
// parent and its ancestors in the cache. A template (not std::function) so the hit path early-returns
// without ever touching the operator (zero overhead). Production passes a CreateDirectoriesLong+exists
// operator; unit tests inject a counting / failure-simulating double (AC-12..16).
template <class EnsureFn>
void EnsureParentDirCached(const std::filesystem::path& filePath, PerWorkerDirCache& cache,
                           EnsureFn&& ensure) {
    const std::optional<std::filesystem::path> parent = PerWorkerDirCache::ParentDirOf(filePath);
    if (!parent.has_value()) {
        return;
    }
    if (cache.contains(*parent)) {
        return;
    }
    if (ensure(*parent)) {
        cache.addWithAncestors(*parent);
    }
}

// Cache-aware overload of EnsureParentDir: a hit skips all filesystem syscalls; a miss runs the same
// CreateDirectoriesLong path as the plain overload and, on success, caches the parent + ancestors.
void EnsureParentDir(const std::filesystem::path& filePath, PerWorkerDirCache& cache);

std::optional<std::filesystem::path> CurrentExePath();

}  // namespace fc
