#pragma once

// Shared local-file metadata probe (task unify-probe-extra-shared, design §4.2).
// The ONE place that turns "a local path under root" into optional<FileEntry{fileSize,
// mtimeNs}> for every metadata read that happens BEFORE the compare core (DecideCompare),
// on both the FastClone client and the FastCheck tool:
//   * FastClone batch form : pass a DirProbeCache* (lazy per-directory enumeration,
//                            shared_mutex double-checked, zero-syscall cache hits).
//   * FastCheck on-demand  : pass nullptr (single stat per file, no cross-call snapshot).
// Include whitelist (design §6.1): this header, file_index.h, sync_util.h, std headers.

#include "file_index.h"  // FileEntry / ToUnixNs

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>

namespace fc {

// Lazy directory cache, migrated verbatim from the sync client's probeLocalFile lambda
// (task unify-probe-extra-shared, design §4.2.2): the first probe touching a directory
// enumerates it once (one sequential MFT/inode read yielding size+mtime for all files)
// and caches the results; subsequent probes for files in the same directory hit memory.
// This avoids both the full-tree pre-scan (too slow on huge slow disks) and the per-file
// random metadata access that collapses IOPS under concurrency.
//   * lazy         : a directory is enumerated only when first probed;
//   * double-check : shared_lock fast path -> unique_lock slow path -> re-lookup inside;
//   * dir-level    : map<dir relPath, map<fileName, FileEntry>>;
//   * thread-safe  : shared_mutex (multi-reader / single-writer), matching the compare
//                    worker pool concurrency model on the sync side.
// Unreadable directories are cached as empty sets so the failure is never retried per
// file (design §4.2.4 / B-02).
class DirProbeCache {
public:
    DirProbeCache() = default;
    DirProbeCache(const DirProbeCache&) = delete;
    DirProbeCache& operator=(const DirProbeCache&) = delete;

    // Probe one file under root. Cache hit -> zero metadata syscalls. Miss -> the file's
    // directory is enumerated once and cached, then looked up. Missing file / directory
    // entry / unreadable metadata -> nullopt (counted as Missing by the caller).
    std::optional<FileEntry> Probe(const std::filesystem::path& root, const std::string& relPath);

    // Number of real directory enumerations performed (once per directory on the slow
    // path). AC-10 asserts this == distinct directory count. Retained in release builds
    // (same policy as check_engine.cpp's [check-phases] diagnostic counters): the tests
    // run against the Release configuration, and the atomic add happens once per
    // DIRECTORY, not per file (task unify-probe-extra-shared-converge, scope item 3).
    uint64_t SlowPathCount() const { return slowPathCount_.load(std::memory_order_relaxed); }

private:
    mutable std::shared_mutex mu_;
    std::unordered_map<std::string, std::unordered_map<std::string, FileEntry>> cache_;
    std::atomic<uint64_t> slowPathCount_{0};
};

// Unified probe entry (FR-01: the ONLY pre-decision local metadata read in both binaries).
//   ctx == nullptr -> on-demand form (FastCheck): one metadata syscall per file, nothing
//                     retained across calls (I-6: no long-lived snapshot).
//   ctx != nullptr -> batch form (FastClone): directory cache hits cost zero syscalls (I-5).
// Missing / directory / unreadable metadata -> nullopt; never throws, never aborts.
std::optional<FileEntry> ProbeLocalFile(const std::filesystem::path& root,
                                        const std::string& relPath,
                                        DirProbeCache* ctx = nullptr);

// Strict-mode variant (shared from check_engine.cpp's StrictProbe, design D-06): size
// only, mtimeNs always 0 (Strict never consults mtime). On-demand only - the batch/cache
// form never existed in production (FastClone hardcodes CompareMode::Fast and never uses
// SizeOnly; FastCheck's Strict path always probes per file), so the ctx parameter was
// removed in the unify-probe-extra-shared-converge cleanup. Keeps the single
// directory_entry query for type+size (dev-map RS-01 syscall count).
std::optional<FileEntry> ProbeLocalFileSizeOnly(const std::filesystem::path& root,
                                                const std::string& relPath);

namespace detail {

// Directory-enumeration primitive used by DirProbeCache's slow path (platform specific;
// unit tests may call it directly). Files only (directories are skipped).
//   Windows: FindFirstFileW(absDir + L"\\*") loop, skipping FILE_ATTRIBUTE_DIRECTORY;
//   POSIX  : fs::directory_iterator(skip_permission_denied), is_regular_file only.
// Enumeration failure (unreadable dir) leaves `out` as-is (empty when freshly passed) -
// the caller caches the empty set so the failure is not retried per file (B-02).
void EnumerateDirFileEntries(const std::filesystem::path& absDir,
                             std::unordered_map<std::string, FileEntry>& out);

}  // namespace detail
}  // namespace fc
