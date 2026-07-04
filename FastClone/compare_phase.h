#pragma once

// 共享比对判定翻译单元（compare_phase）。sync 客户端与 FastCheck 共用同一套「凭元数据/hash
// 判定文件类别」的纯函数，保证两侧对同一对 (local, remote) 得出字节级一致的结论。
// sync 侧原本内联在 sync_engine_client.cpp 的三处判定改调用这里；FastCheck 三种模式全走这里。

#include "file_index.h"  // FileEntry / Hash256 / HashEquals

#include <cstdint>
#include <filesystem>
#include <optional>
#include <ostream>
#include <string>
#include <unordered_set>
#include <vector>

namespace fc {

// 比对模式。Fast=大小同则看 mtime，异则 hash；Strict=大小同一律 hash，忽略 mtime；
// SizeOnly=只看大小，绝不 hash。sync 客户端固定用 Fast，FastCheck 三种全支持。
enum class CompareMode : uint8_t { Fast, Strict, SizeOnly };

// 逐文件最终类别（报告与计数口径）。
enum class CompareCategory : uint8_t { Same, Diff, Missing, Extra };

// DecideCompare 的「元数据阶段」结论：needHash==false 时 category 即为最终类别；
// needHash==true 表示仅凭大小/时间无法定 Same/Diff，尚需 hash 收尾（走 ClassifyByHash）。
struct CompareOutcome {
    CompareCategory category = CompareCategory::Same;  // needHash==false 时才有效
    bool needHash = false;
};

// 仅凭大小/时间（按 mode）判定。纯函数：本地缺失 -> Missing；大小不同 -> Diff（三模式一致，
// strict 也不 hash）；大小相同时按 mode 分流（SizeOnly=Same；Fast=mtime 容差内 Same 否则 needHash；
// Strict=needHash）。Fast 分支与既有 DecideCompareAction 真值表等价。
CompareOutcome DecideCompare(CompareMode mode,
                             const std::optional<FileEntry>& localFile,
                             const FileEntry& remoteFile);

// hash 阶段收尾：本地可读且与远端 hash 相等 -> Same，否则 Diff。纯函数。
CompareCategory ClassifyByHash(bool localReadable,
                               const Hash256& localHash,
                               const Hash256& remoteHash);

// 将原始 manifest mtime（可能是 Unix ns 或 Windows FILETIME ticks）归一成 Unix 纳秒。
// 单一真相源（M1 修复）：sync 引擎与 FastCheck 都链 compare_phase，mtime 归一只有这一份实现，
// 避免既往 sync_util.cpp 副本与本副本双写漂移。返回 false 表示该值无法有意义转换（调用方退回裸值兜底）。
bool TryNormalizeMtimeToUnixNs(int64_t rawMtime, int64_t& outUnixNs);

// sync 判定「本地项是否在远端缺失」的谓词（供 RemoveLocalExtras 原地调用）。纯函数，零开销：
// 等价于 !remoteFiles.contains(relPath)。模板化以兼容 sync 侧的 unordered_map<string,FileEntry>
// 与 check 侧的 unordered_set<string>——两者都提供 contains(key)，避免在 sync 热路径上为迁就
// 单一具体类型而复制出一份 keyset（那会改变流水线，破坏纯重构的字节级等价）。
template <typename RemoteContainer>
inline bool IsLocalExtra(const std::string& relPath, const RemoteContainer& remoteFilePaths) {
    return !remoteFilePaths.contains(relPath);
}

// 只读枚举本地 targetRoot，返回「服务端 manifest 未包含」的多余文件相对路径（正斜杠归一）。
// 复用 file_index::BuildIndex 自包含枚举，不牵动 sync 引擎，只回 EXTRA，绝不删除。
std::vector<std::string> CollectExtraLocal(const std::filesystem::path& targetRoot,
                                           const std::unordered_set<std::string>& manifestPaths);

// 统一计数结构 + 单行渲染（check 进度与终态摘要）。写入任意 ostream 便于单测。
struct CompareCounters {
    uint64_t enumerated = 0;  // 已收到的 manifest 文件数
    uint64_t same = 0;
    uint64_t diff = 0;
    uint64_t missing = 0;
    uint64_t extraLocal = 0;
    // 完整比对结束时的比对文件总数：same + diff + missing（extra 不计入，另行统计）。
    uint64_t TotalCompared() const { return same + diff + missing; }
};

void PrintCompareCounters(std::ostream& os, const CompareCounters& c, bool partial);

}  // namespace fc
