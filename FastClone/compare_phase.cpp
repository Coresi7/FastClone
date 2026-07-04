#include "compare_phase.h"

#include <cmath>
#include <cstdlib>
#include <limits>

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

std::vector<std::string> CollectExtraLocal(const fs::path& targetRoot,
                                           const std::unordered_set<std::string>& manifestPaths) {
    std::vector<std::string> extras;
    // Read-only enumerate all local entries (BuildIndex is self-contained, no exclude); only files not contained in the manifest count as EXTRA.
    const std::vector<FileEntry> localEntries = BuildIndex(targetRoot, std::nullopt);
    for (const FileEntry& entry : localEntries) {
        if (entry.isDirectory) {
            continue;
        }
        if (!manifestPaths.contains(entry.relativePath)) {
            extras.push_back(entry.relativePath);
        }
    }
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
