#include "compare_phase.h"

#include <cmath>
#include <cstdlib>
#include <limits>

namespace fs = std::filesystem;

namespace fc {

namespace {

// Fast 模式的 mtime 容差判定，等价于既有 DecideCompareAction 的 Skip 分支：两侧先归一到
// Unix ns 比 2ms 容差；归一失败则退回裸值 2ms 容差兜底（兼容遗留/非法时间戳）。
bool MtimeWithinTolerance(int64_t localMtimeNs, int64_t remoteMtimeNs) {
    constexpr int64_t kMtimeToleranceNs = 2LL * 1000LL * 1000LL;   // 2ms 容差
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

// mtime 归一化单一真相源（M1 修复）。原 sync_util.cpp::TryNormalizeMtimeToUnixNs 的实现逐字迁此：
// 阈值之上视为 Unix ns；低于 Windows epoch 视为非法；其间按 FILETIME 100ns ticks 折算成 Unix ns。
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
    // 本地缺失：三模式一致，直接 Missing（sync Fast 侧等价 TransferNow）。
    if (!localFile.has_value()) {
        return CompareOutcome{CompareCategory::Missing, false};
    }
    // 大小不同：三模式一致，直接 Diff 且不 hash（strict 也不 hash）。
    if (localFile->fileSize != remoteFile.fileSize) {
        return CompareOutcome{CompareCategory::Diff, false};
    }
    // 大小相同：按 mode 分流。
    switch (mode) {
        case CompareMode::SizeOnly:
            return CompareOutcome{CompareCategory::Same, false};
        case CompareMode::Strict:
            // 忽略 mtime，一律 hash。
            return CompareOutcome{CompareCategory::Same, true};
        case CompareMode::Fast:
        default:
            if (MtimeWithinTolerance(localFile->mtimeNs, remoteFile.mtimeNs)) {
                return CompareOutcome{CompareCategory::Same, false};
            }
            // mtime 不在容差内：需 hash 才能定 Same/Diff（sync Fast 侧等价 FallbackHash）。
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
    // 只读枚举本地全量条目（BuildIndex 自包含，不含 exclude）；仅文件、且 manifest 未包含者算 EXTRA。
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
