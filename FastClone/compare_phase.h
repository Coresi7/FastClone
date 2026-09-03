#pragma once

// Shared comparison-decision translation unit (compare_phase). The sync client and FastCheck share one
// set of "classify a file by metadata/hash" pure functions, guaranteeing both sides reach a
// byte-level-identical conclusion for the same (local, remote) pair.
// The three decisions originally inlined in sync_engine_client.cpp on the sync side now call here;
// all three FastCheck modes go through here.

#include "file_index.h"  // FileEntry / Hash256 / HashEquals

#include <cstdint>
#include <filesystem>
#include <optional>
#include <ostream>
#include <string>
#include <unordered_set>
#include <vector>

namespace fc {

// Compare mode. Fast=if sizes match check mtime, else hash; Strict=if sizes match always hash, ignore mtime;
// SizeOnly=size only, never hash. The sync client always uses Fast; FastCheck supports all three.
enum class CompareMode : uint8_t { Fast, Strict, SizeOnly };

// Per-file final category (reporting and counting basis).
enum class CompareCategory : uint8_t { Same, Diff, Missing, Extra };

// DecideCompare's "metadata phase" conclusion: when needHash==false, category is the final category;
// needHash==true means size/time alone cannot decide Same/Diff and a hash is still needed to finish (via ClassifyByHash).
struct CompareOutcome {
    CompareCategory category = CompareCategory::Same;  // valid only when needHash==false
    bool needHash = false;
};

// Decide by size/time only (per mode). Pure function: local missing -> Missing; size differs -> Diff (same in all three modes,
// strict does not hash either); when sizes match, branch by mode (SizeOnly=Same; Fast=Same within mtime tolerance else needHash;
// Strict=needHash). The Fast branch is equivalent to the existing DecideCompareAction truth table.
CompareOutcome DecideCompare(CompareMode mode,
                             const std::optional<FileEntry>& localFile,
                             const FileEntry& remoteFile);

// Hash-phase finish: local readable and equal to the remote hash -> Same, otherwise Diff. Pure function.
CompareCategory ClassifyByHash(bool localReadable,
                               const Hash256& localHash,
                               const Hash256& remoteHash);

// Normalize the raw manifest mtime (may be Unix ns or Windows FILETIME ticks) to Unix nanoseconds.
// Single source of truth (M1 fix): both the sync engine and FastCheck link compare_phase, so mtime
// normalization has only this one implementation, avoiding the prior double-write drift between the
// sync_util.cpp copy and this copy. Returns false when the value cannot be meaningfully converted (the caller falls back to the raw value).
bool TryNormalizeMtimeToUnixNs(int64_t rawMtime, int64_t& outUnixNs);

// Predicate for the sync decision "is a local item missing on the remote side". Pure
// function, zero overhead: equivalent to !remoteFiles.contains(relPath).
// Templated to be compatible with the sync side's unordered_map<string,FileEntry> and the
// check side's unordered_set<string> - both provide contains(key), avoiding, on the sync
// hot path, copying out a keyset just to accommodate one concrete type (that would
// change the pipeline and break the byte-level equivalence of the pure refactor).
// The shared extra enumeration (fc::CollectExtraFiles[AndDirs], extra_scan.h - where the
// former CollectExtraLocal implementation moved to, task unify-probe-extra-shared D-07)
// reuses this same contains() constraint; both sides now scan extras through that one TU.
template <typename RemoteContainer>
inline bool IsLocalExtra(const std::string& relPath, const RemoteContainer& remoteFilePaths) {
    return !remoteFilePaths.contains(relPath);
}

// NOTE (unify-probe-extra-shared D-07): the former CollectExtraLocal(targetRoot,
// manifestPaths) declaration/implementation was MOVED to FastClone/extra_scan.h/.cpp as
// the shared fc::CollectExtraFiles / fc::CollectExtraFilesAndDirs templates (FR-06 single
// extra-enumeration entry for both FastClone and FastCheck). This TU keeps only the
// compare decision core + counters (the four protected functions stay diff-free, AC-12c).

// Unified counter struct + single-line render (check progress and final summary). Writes to any ostream for unit testing.
struct CompareCounters {
    uint64_t enumerated = 0;  // number of manifest files received so far
    uint64_t same = 0;
    uint64_t diff = 0;
    uint64_t missing = 0;
    uint64_t extraLocal = 0;
    // Total compared files at the end of a full comparison: same + diff + missing (extra is not counted, tracked separately).
    uint64_t TotalCompared() const { return same + diff + missing; }
};

void PrintCompareCounters(std::ostream& os, const CompareCounters& c, bool partial);

}  // namespace fc
