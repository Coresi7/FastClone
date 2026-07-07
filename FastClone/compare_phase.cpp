#include "compare_phase.h"

#include "parallel_dir_walk.h"  // fc::detail::ParallelDirWalk + BuildRelPath/ResolveDirWalkWorkerCount/OpenDirFind
#include "sync_util.h"          // fc::WideToUtf8 / fc::ToExtendedLengthPath (Windows relPath encoding)

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <iterator>
#include <limits>
#include <mutex>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace fc {

namespace {

// Fast-mode mtime tolerance decision, equivalent to the Skip branch of the existing DecideCompareAction:
// both sides are first normalized to Unix ns and compared with a 2ms tolerance; if normalization fails,
// fall back to a 2ms tolerance on the raw values (compatible with legacy/invalid timestamps).
bool MtimeWithinTolerance(int64_t localMtimeNs, int64_t remoteMtimeNs) {
    constexpr int64_t kMtimeToleranceNs = 2LL * 1000LL * 1000LL;   // 2ms tolerance
    constexpr int64_t kLegacyRawTolerance = 2LL * 1000LL * 1000LL;
    int64_t localUnixNs = 0;
    int64_t remoteUnixNs = 0;
    const bool localNormalized = TryNormalizeMtimeToUnixNs(localMtimeNs, localUnixNs);
    const bool remoteNormalized = TryNormalizeMtimeToUnixNs(remoteMtimeNs, remoteUnixNs);
    if (localNormalized && remoteNormalized) {
        const int64_t deltaNs = std::llabs(static_cast<long long>(localUnixNs - remoteUnixNs));
        return deltaNs <= kMtimeToleranceNs;
    }
    const int64_t rawDelta = std::llabs(static_cast<long long>(localMtimeNs - remoteMtimeNs));
    return rawDelta <= kLegacyRawTolerance;
}

}  // namespace

// Single source of truth for mtime normalization (M1 fix). The original sync_util.cpp::TryNormalizeMtimeToUnixNs
// implementation is migrated here verbatim: above the threshold, treat as Unix ns; below the Windows epoch,
// treat as invalid; in between, convert from FILETIME 100ns ticks to Unix ns.
bool TryNormalizeMtimeToUnixNs(int64_t rawMtime, int64_t& outUnixNs) {
    constexpr int64_t kLikelyUnixNsThreshold = 500000000000000000LL;
    constexpr int64_t kWindowsEpochDiff100ns = 116444736000000000LL;
    if (rawMtime > kLikelyUnixNsThreshold) {
        outUnixNs = rawMtime;
        return true;
    }
    if (rawMtime < kWindowsEpochDiff100ns) {
        return false;
    }
    const int64_t ticksSinceUnixEpoch = rawMtime - kWindowsEpochDiff100ns;
    if (ticksSinceUnixEpoch > (std::numeric_limits<int64_t>::max)() / 100LL ||
        ticksSinceUnixEpoch < (std::numeric_limits<int64_t>::min)() / 100LL) {
        return false;
    }
    outUnixNs = ticksSinceUnixEpoch * 100LL;
    return true;
}

CompareOutcome DecideCompare(CompareMode mode,
                             const std::optional<FileEntry>& localFile,
                             const FileEntry& remoteFile) {
    // Local missing: same in all three modes, directly Missing (equivalent to TransferNow on the sync Fast side).
    if (!localFile.has_value()) {
        return CompareOutcome{CompareCategory::Missing, false};
    }
    // Size differs: same in all three modes, directly Diff and no hash (strict does not hash either).
    if (localFile->fileSize != remoteFile.fileSize) {
        return CompareOutcome{CompareCategory::Diff, false};
    }
    // Sizes match: branch by mode.
    switch (mode) {
        case CompareMode::SizeOnly:
            return CompareOutcome{CompareCategory::Same, false};
        case CompareMode::Strict:
            // Ignore mtime, always hash.
            return CompareOutcome{CompareCategory::Same, true};
        case CompareMode::Fast:
        default:
            if (MtimeWithinTolerance(localFile->mtimeNs, remoteFile.mtimeNs)) {
                return CompareOutcome{CompareCategory::Same, false};
            }
            // mtime not within tolerance: a hash is needed to decide Same/Diff (equivalent to FallbackHash on the sync Fast side).
            return CompareOutcome{CompareCategory::Same, true};
    }
}

CompareCategory ClassifyByHash(bool localReadable,
                               const Hash256& localHash,
                               const Hash256& remoteHash) {
    if (localReadable && HashEquals(localHash, remoteHash)) {
        return CompareCategory::Same;
    }
    return CompareCategory::Diff;
}

// EXTRA-detection finish pass over the local tree (task fastcheck-extra-parallel-walk).
//
// Why no file_size stat (FR-02/NFR-03): deciding whether a local path is EXTRA needs only
// "directory vs non-directory" + a membership test against the manifest path set. It never
// consumes the file's size. The old path went through BuildIndex, which not only enumerated
// serially but also drove a parallel file_size worker over EVERY file -- pure waste here.
// Size is still read, but only at the call site (ProbeLocal in check_engine.cpp) and only for
// the tiny set of real extras, to fill in localSize; that stays on FR-09's untouched path.
//
// Why reuse ParallelDirWalk (NFR-03): the enumeration is dominated by random MFT / inode read
// LATENCY that a single thread cannot hide (disk sits at queue-depth 1). The shared walker
// fans out the directory listings to raise device queue depth, exactly like RemoveLocalExtras,
// bringing the finish pass down to the same seconds-scale order.
std::vector<std::string> CollectExtraLocal(const fs::path& targetRoot,
                                           const std::unordered_set<std::string>& manifestPaths) {
    std::mutex extrasMu;
    std::vector<std::string> extras;                       // global; only merged under extrasMu in finishWorker
    struct ExtraCtx { std::vector<std::string> local; };   // per-worker, hot path is lock-free
    const std::atomic<bool> noCancel{false};               // finish pass is not cancellable, same as RemoveLocalExtras
    const unsigned numWorkers = fc::detail::ResolveDirWalkWorkerCount();

    auto finishWorker = [&](ExtraCtx& ctx) {
        std::lock_guard<std::mutex> lock(extrasMu);
        extras.insert(extras.end(),
                      std::make_move_iterator(ctx.local.begin()),
                      std::make_move_iterator(ctx.local.end()));
        ctx.local.clear();
    };

#ifdef _WIN32
    struct PendingDir {
        std::wstring absDir;
        std::string relDir;
    };
    // Windows: type comes from WIN32_FIND_DATA attributes (FILE_ATTRIBUTE_DIRECTORY); no file_size read (AC-03).
    auto processDir = [&](const PendingDir& current, std::vector<PendingDir>& subdirs, ExtraCtx& ctx) {
        WIN32_FIND_DATAW fd{};
        HANDLE hFind = fc::detail::OpenDirFind(current.absDir, fd);
        if (hFind == INVALID_HANDLE_VALUE) {
            return;   // no permission / not found -> skip, no fatal (FR-08)
        }
        do {
            const wchar_t* name = fd.cFileName;
            if ((name[0] == L'.' && name[1] == L'\0') ||
                (name[0] == L'.' && name[1] == L'.' && name[2] == L'\0')) {
                continue;
            }
            const bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            // relPath byte-identical to RemoveLocalExtras: WideToUtf8(name) fed to BuildRelPath (FR-05).
            std::string relPath = fc::detail::BuildRelPath(current.relDir, fc::WideToUtf8(name));
            if (isDir) {
                std::wstring absPath = current.absDir;
                if (!absPath.empty() && absPath.back() != L'\\' && absPath.back() != L'/') {
                    absPath.push_back(L'\\');
                }
                absPath.append(name);
                subdirs.push_back(PendingDir{std::move(absPath), std::move(relPath)});   // dirs never become extras (FR-06)
            } else if (!manifestPaths.contains(relPath)) {
                ctx.local.push_back(std::move(relPath));                                  // non-dir candidate (FR-07)
            }
        } while (FindNextFileW(hFind, &fd) != 0);
        FindClose(hFind);
    };
    fc::detail::ParallelDirWalk(PendingDir{fc::ToExtendedLengthPath(targetRoot), std::string()},
                                numWorkers, fc::detail::kDeleteDirPopBatch, noCancel, "check-extra-walk",
                                ExtraCtx{}, processDir, finishWorker);
#else
    struct PendingDir {
        fs::path absDir;
        std::string relDir;
    };
    // POSIX: type comes from directory_entry capabilities (is_directory/is_regular_file/is_symlink); no file_size read (AC-04).
    auto processDir = [&](const PendingDir& current, std::vector<PendingDir>& subdirs, ExtraCtx& ctx) {
        std::error_code ec;
        fs::directory_iterator it(current.absDir, fs::directory_options::skip_permission_denied, ec);
        const fs::directory_iterator end;
        if (ec) {
            return;   // FR-08
        }
        for (; it != end; it.increment(ec)) {
            if (ec) {
                ec.clear();
                continue;   // per-entry failure / delete race -> skip (FR-08/B9)
            }
            const bool isDir = it->is_directory(ec);
            if (ec) {
                ec.clear();
                continue;
            }
            const bool isRegular = it->is_regular_file(ec);
            if (ec) {
                ec.clear();
                continue;
            }
            if (!isDir && !isRegular) {
                continue;   // only dirs + regular files, matching BuildIndex
            }
            const bool isSymlink = it->is_symlink(ec);
            if (ec) {
                ec.clear();
            }
            std::string relPath = fc::detail::BuildRelPath(current.relDir, it->path().filename().string());
            if (relPath.empty()) {
                continue;
            }
            if (isDir) {
                if (!isSymlink) {
                    subdirs.push_back(PendingDir{it->path(), std::move(relPath)});   // symlink dirs are not recursed, aligning with RemoveLocalExtras (FR-13)
                }
            } else if (!manifestPaths.contains(relPath)) {
                ctx.local.push_back(std::move(relPath));                             // symlink->file follows regular-file semantics (FR-07/B7)
            }
        }
    };
    fc::detail::ParallelDirWalk(PendingDir{targetRoot, std::string()},
                                numWorkers, fc::detail::kDeleteDirPopBatch, noCancel, "check-extra-walk",
                                ExtraCtx{}, processDir, finishWorker);
#endif

    // Parallel merge order is non-deterministic; sort ascending to replicate the old BuildIndex detail
    // order (files ascending after directories were dropped), keeping AC-05/06 detail rows byte-identical
    // and satisfying NFR-07 determinism.
    std::sort(extras.begin(), extras.end());
    return extras;
}

void PrintCompareCounters(std::ostream& os, const CompareCounters& c, bool partial) {
    if (partial) {
        os << "[PARTIAL] ";
    }
    os << "same=" << c.same << " diff=" << c.diff << " missing=" << c.missing
       << " extra_local=" << c.extraLocal << " total=" << c.TotalCompared();
}

}  // namespace fc
