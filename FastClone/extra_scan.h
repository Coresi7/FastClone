#pragma once

// Shared local-extra enumeration (task unify-probe-extra-shared, design §4.3).
// The ONE place that walks a local target tree and decides "which local entries are not
// in the remote manifest" for both FastClone (delete extras) and FastCheck (count extras).
// Results are delivered BY VALUE: when the caller receives them the walk has fully ended
// and every find handle / directory iterator is closed, which is what lets FastClone run
// its existing two-phase delete afterwards (I-7).
// Include whitelist (design §6.1): this header, file_index.h, sync_util.h,
// parallel_dir_walk.h, path_utils.h, std headers.

#include "file_index.h"  // FileEntry (documented value type of the remote containers)

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace fc {

// Enumeration options (FR-07).
struct ExtraScanOptions {
    // Absolute path of the entry to exclude (caller canonicalizes). nullopt excludes
    // nothing. Comparison: Windows = _wcsicmp case-insensitive string equality; POSIX =
    // fs::equivalent with no error. Judged BEFORE the isDir test (FR-07 clause 4 / D-09,
    // migrated line-by-line from RemoveLocalExtras).
    std::optional<std::filesystem::path> excludeAbsPath;
    // Whether to collect directory information. FastClone = true (needs extraDirs /
    // localDirs); FastCheck = false (I-6: dir vectors stay empty AND unallocated).
    bool collectDirs = false;
};

struct ExtraScanResult {
    std::vector<std::string> extraFiles;  // ascending (NFR-11): local files absent from the remote set
    std::vector<std::string> extraDirs;   // ascending: local dirs absent from the remote dir set; empty when collectDirs=false
    std::vector<std::string> localDirs;   // ascending: every local dir (lets FastClone skip create_directories)
};

namespace detail {

// Type-erased membership predicate. The header-level templates below only erase their
// `contains(const std::string&)` call into this pair, so the traversal body exists ONCE
// (extra_scan.cpp) while the hot path keeps zero-copy access to the caller's container
// (no keyset duplication, NFR-03 / design D-04).
struct RemotePredicate {
    bool (*fn)(const void*, const std::string&) = nullptr;
    const void* ctx = nullptr;
};

// The single traversal implementation (defined once in extra_scan.cpp): platform walk +
// exclude check + relPath construction + ParallelDirWalk scheduling + ascending sort.
// dirInRemote with fn == nullptr -> no directory classification (extraDirs/localDirs stay
// empty and unallocated, I-6).
ExtraScanResult ScanExtrasImpl(const std::filesystem::path& root,
                               RemotePredicate fileInRemote,
                               RemotePredicate dirInRemote,
                               const ExtraScanOptions& options);

}  // namespace detail

// Entry 1 (FastCheck form / unit-test form): collect extra FILES only.
// RemoteFiles only needs contains(const std::string&) - compatible with FastClone's
// unordered_map<string,FileEntry> and FastCheck's unordered_set<string> without copying
// keysets on the hot path (compare_phase.h IsLocalExtra's same template constraint).
// collectDirs is forced off (I-6).
template <typename RemoteFiles>
std::vector<std::string> CollectExtraFiles(const std::filesystem::path& root,
                                           const RemoteFiles& remoteFiles,
                                           const ExtraScanOptions& options = {});

// Entry 2 (FastClone form): also returns extraDirs / localDirs. collectDirs is forced on.
template <typename RemoteFiles, typename RemoteDirs>
ExtraScanResult CollectExtraFilesAndDirs(const std::filesystem::path& root,
                                         const RemoteFiles& remoteFiles,
                                         const RemoteDirs& remoteDirs,
                                         const ExtraScanOptions& options = {});

// OQ-01=A shared exclude injector (design §4.4, FR-08): returns the canonical path of
// THIS process's executable file when (and only when) it lies under `root`, else nullopt.
// Both sides inject through this one function, which is the physical guarantee of the
// rule-level exclusion parity (small change B).
std::optional<std::filesystem::path> SelfExcludeUnderRoot(const std::filesystem::path& root);

// ---- template implementations (thin adapters over detail::ScanExtrasImpl) --------------

template <typename RemoteFiles>
std::vector<std::string> CollectExtraFiles(const std::filesystem::path& root,
                                           const RemoteFiles& remoteFiles,
                                           const ExtraScanOptions& options) {
    const detail::RemotePredicate fileInRemote{
        [](const void* ctx, const std::string& rel) {
            return static_cast<const RemoteFiles*>(ctx)->contains(rel);
        },
        static_cast<const void*>(&remoteFiles)};
    ExtraScanOptions opts = options;
    opts.collectDirs = false;  // entry 1 never collects dir info (I-6)
    return detail::ScanExtrasImpl(root, fileInRemote, detail::RemotePredicate{}, opts).extraFiles;
}

template <typename RemoteFiles, typename RemoteDirs>
ExtraScanResult CollectExtraFilesAndDirs(const std::filesystem::path& root,
                                         const RemoteFiles& remoteFiles,
                                         const RemoteDirs& remoteDirs,
                                         const ExtraScanOptions& options) {
    const detail::RemotePredicate fileInRemote{
        [](const void* ctx, const std::string& rel) {
            return static_cast<const RemoteFiles*>(ctx)->contains(rel);
        },
        static_cast<const void*>(&remoteFiles)};
    const detail::RemotePredicate dirInRemote{
        [](const void* ctx, const std::string& rel) {
            return static_cast<const RemoteDirs*>(ctx)->contains(rel);
        },
        static_cast<const void*>(&remoteDirs)};
    ExtraScanOptions opts = options;
    opts.collectDirs = true;  // entry 2 always collects dir info
    return detail::ScanExtrasImpl(root, fileInRemote, dirInRemote, opts);
}

}  // namespace fc
