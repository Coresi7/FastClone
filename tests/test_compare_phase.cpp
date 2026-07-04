#include "compare_phase.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace fs = std::filesystem;

namespace {

void Expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error("test_compare_phase: " + message);
    }
}

// Unix-ns timestamp (> 5e17 threshold, recognized by normalization logic as Unix ns).
constexpr int64_t kBaseMtimeNs = 1700000000000000000LL;

fc::FileEntry MakeRemote(const std::string& rel, uint64_t size, int64_t mtimeNs) {
    fc::FileEntry e;
    e.relativePath = rel;
    e.isDirectory = false;
    e.fileSize = size;
    e.mtimeNs = mtimeNs;
    return e;
}

fs::path MakeTempDir() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path dir = fs::temp_directory_path() / ("fastclone_cp_" + std::to_string(stamp));
    fs::create_directories(dir);
    return dir;
}

void WriteFile(const fs::path& path, const std::string& content) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << content;
}

void TestDecideCompareMissing() {
    const fc::FileEntry remote = MakeRemote("a.txt", 100, kBaseMtimeNs);
    for (fc::CompareMode mode : {fc::CompareMode::Fast, fc::CompareMode::Strict, fc::CompareMode::SizeOnly}) {
        const fc::CompareOutcome out = fc::DecideCompare(mode, std::nullopt, remote);
        Expect(!out.needHash, "missing must not need hash");
        Expect(out.category == fc::CompareCategory::Missing, "missing local -> Missing (all modes)");
    }
}

void TestDecideCompareSizeDiff() {
    const fc::FileEntry remote = MakeRemote("a.txt", 100, kBaseMtimeNs);
    const std::optional<fc::FileEntry> local = MakeRemote("a.txt", 200, kBaseMtimeNs);
    for (fc::CompareMode mode : {fc::CompareMode::Fast, fc::CompareMode::Strict, fc::CompareMode::SizeOnly}) {
        const fc::CompareOutcome out = fc::DecideCompare(mode, local, remote);
        Expect(!out.needHash, "size diff must not need hash (strict included, AC-26)");
        Expect(out.category == fc::CompareCategory::Diff, "size diff -> Diff (all modes)");
    }
}

void TestDecideCompareSizeSameMtimeSame() {
    const fc::FileEntry remote = MakeRemote("a.txt", 100, kBaseMtimeNs);
    const std::optional<fc::FileEntry> local = MakeRemote("a.txt", 100, kBaseMtimeNs);
    // Fast: same size + same mtime -> Same, no hash (AC-23).
    fc::CompareOutcome out = fc::DecideCompare(fc::CompareMode::Fast, local, remote);
    Expect(!out.needHash && out.category == fc::CompareCategory::Same, "fast same size+mtime -> Same no hash");
    // SizeOnly: same size -> Same, no hash (AC-27).
    out = fc::DecideCompare(fc::CompareMode::SizeOnly, local, remote);
    Expect(!out.needHash && out.category == fc::CompareCategory::Same, "size-only same size -> Same no hash");
    // Strict: same size always hash, ignore mtime (AC-25).
    out = fc::DecideCompare(fc::CompareMode::Strict, local, remote);
    Expect(out.needHash, "strict same size (mtime same) -> needHash");
}

void TestDecideCompareSizeSameMtimeDiff() {
    const fc::FileEntry remote = MakeRemote("a.txt", 100, kBaseMtimeNs);
    const std::optional<fc::FileEntry> local = MakeRemote("a.txt", 100, kBaseMtimeNs + 10'000'000LL);  // +10ms
    // Fast: same size + mtime differs (>2ms) -> needHash (AC-24).
    fc::CompareOutcome out = fc::DecideCompare(fc::CompareMode::Fast, local, remote);
    Expect(out.needHash, "fast same size, mtime diff -> needHash");
    // SizeOnly: ignores mtime, still Same (AC-27).
    out = fc::DecideCompare(fc::CompareMode::SizeOnly, local, remote);
    Expect(!out.needHash && out.category == fc::CompareCategory::Same, "size-only ignores mtime -> Same");
    // Strict: needHash (AC-25).
    out = fc::DecideCompare(fc::CompareMode::Strict, local, remote);
    Expect(out.needHash, "strict same size (mtime diff) -> needHash");
}

void TestDecideCompareFastMtimeWithinTolerance() {
    const fc::FileEntry remote = MakeRemote("a.txt", 100, kBaseMtimeNs);
    const std::optional<fc::FileEntry> local = MakeRemote("a.txt", 100, kBaseMtimeNs + 1'000'000LL);  // +1ms <2ms
    const fc::CompareOutcome out = fc::DecideCompare(fc::CompareMode::Fast, local, remote);
    Expect(!out.needHash && out.category == fc::CompareCategory::Same,
           "fast mtime within 2ms tolerance -> Same (equivalence with legacy DecideCompareAction)");
}

void TestClassifyByHash() {
    fc::Hash256 a{};
    fc::Hash256 b{};
    a[0] = 1;
    b[0] = 1;
    fc::Hash256 c{};
    c[0] = 2;
    Expect(fc::ClassifyByHash(true, a, b) == fc::CompareCategory::Same, "readable + equal -> Same");
    Expect(fc::ClassifyByHash(true, a, c) == fc::CompareCategory::Diff, "readable + unequal -> Diff");
    Expect(fc::ClassifyByHash(false, a, b) == fc::CompareCategory::Diff, "not readable -> Diff");
}

void TestIsLocalExtra() {
    std::unordered_set<std::string> remoteSet = {"a.txt", "b.txt"};
    Expect(!fc::IsLocalExtra("a.txt", remoteSet), "present in set -> not extra");
    Expect(fc::IsLocalExtra("z.txt", remoteSet), "absent from set -> extra");
    // Template compatible with the sync side's unordered_map<string,FileEntry>.
    std::unordered_map<std::string, fc::FileEntry> remoteMap;
    remoteMap["a.txt"] = MakeRemote("a.txt", 1, kBaseMtimeNs);
    Expect(!fc::IsLocalExtra("a.txt", remoteMap), "present in map -> not extra");
    Expect(fc::IsLocalExtra("z.txt", remoteMap), "absent from map -> extra");
}

void TestCollectExtraLocal() {
    const fs::path dir = MakeTempDir();
    WriteFile(dir / "a.txt", "aaa");
    WriteFile(dir / "b.txt", "bbb");
    WriteFile(dir / "sub" / "c.txt", "ccc");

    std::unordered_set<std::string> manifest = {"a.txt"};
    std::vector<std::string> extras = fc::CollectExtraLocal(dir, manifest);
    bool hasB = false;
    bool hasC = false;
    bool hasA = false;
    for (const std::string& rel : extras) {
        if (rel == "a.txt") hasA = true;
        if (rel == "b.txt") hasB = true;
        if (rel == "sub/c.txt") hasC = true;
    }
    Expect(!hasA, "a.txt is in manifest -> not extra");
    Expect(hasB, "b.txt is local-only -> extra");
    Expect(hasC, "sub/c.txt (forward slash) is local-only -> extra");

    std::error_code ec;
    fs::remove_all(dir, ec);
}

void TestPrintCompareCounters() {
    fc::CompareCounters c;
    c.same = 2;
    c.diff = 1;
    c.missing = 3;
    c.extraLocal = 4;
    std::ostringstream os;
    fc::PrintCompareCounters(os, c, /*partial=*/false);
    const std::string text = os.str();
    Expect(text.find("same=2") != std::string::npos, "counters text has same=2");
    Expect(text.find("diff=1") != std::string::npos, "counters text has diff=1");
    Expect(text.find("missing=3") != std::string::npos, "counters text has missing=3");
    Expect(text.find("extra_local=4") != std::string::npos, "counters text has extra_local=4");
    Expect(text.find("total=6") != std::string::npos, "total = same+diff+missing = 6");
    Expect(text.find("[PARTIAL]") == std::string::npos, "non-partial has no [PARTIAL] prefix");

    std::ostringstream osp;
    fc::PrintCompareCounters(osp, c, /*partial=*/true);
    Expect(osp.str().find("[PARTIAL]") == 0, "partial output starts with [PARTIAL]");
}

}  // namespace

void RunComparePhaseTests() {
    TestDecideCompareMissing();
    TestDecideCompareSizeDiff();
    TestDecideCompareSizeSameMtimeSame();
    TestDecideCompareSizeSameMtimeDiff();
    TestDecideCompareFastMtimeWithinTolerance();
    TestClassifyByHash();
    TestIsLocalExtra();
    TestCollectExtraLocal();
    TestPrintCompareCounters();
}
