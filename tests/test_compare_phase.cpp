#include "compare_phase.h"
#include "extra_scan.h"  // unify-probe-extra-shared D-07: CollectExtraLocal moved here as CollectExtraFiles

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
    std::vector<std::string> extras = fc::CollectExtraFiles(dir, manifest);
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

// TM-02/AC-05 (FR-06): CollectExtraLocal must skip "." and "..". On Windows FindFirstFileW yields
// "." and ".." for every directory, so the enumerator has to drop them or they would leak into
// relPaths (and "." would even be re-entered as a subdir). On POSIX directory_iterator already omits
// them, so the assertion simply holds there too. Either way only real files may come back.
void TestCollectExtraLocalDotDotSkip() {
    const fs::path dir = MakeTempDir();
    WriteFile(dir / "a.txt", "a");
    WriteFile(dir / "sub" / "b.txt", "b");

    const std::vector<std::string> extras = fc::CollectExtraFiles(dir, std::unordered_set<std::string>{});
    bool hasA = false;
    bool hasB = false;
    for (const std::string& rel : extras) {
        Expect(rel != "." && rel != "..", "'.'/'..' must never be emitted as an extra");
        Expect(rel.find("/.") == std::string::npos,
               "no '.'/'..' path component may leak into a relPath: " + rel);
        if (rel == "a.txt") hasA = true;
        if (rel == "sub/b.txt") hasB = true;
    }
    Expect(hasA && hasB, "both real files collected as extras");
    Expect(extras.size() == 2, "exactly the two real files, no '.'/'..' entries");

    std::error_code ec;
    fs::remove_all(dir, ec);
}

// TM-02/AC-05 (S-01, FR-07/B7): a symlink pointing at a regular FILE follows regular-file semantics
// and is reported as an extra. Runs unconditionally on POSIX; on Windows it needs the create-symlink
// privilege (Developer Mode), so we skip cleanly when unavailable rather than fail the suite.
void TestCollectExtraLocalSymlinkFile() {
    const fs::path dir = MakeTempDir();
    WriteFile(dir / "real_target.txt", "payload");  // in manifest -> not itself an extra

    std::error_code linkEc;
    fs::create_symlink(dir / "real_target.txt", dir / "flink.txt", linkEc);
    if (linkEc) {
        std::error_code ec;
        fs::remove_all(dir, ec);
        return;  // no symlink privilege on this platform/session -> skip
    }

    const std::unordered_set<std::string> manifest = {"real_target.txt"};
    const std::vector<std::string> extras = fc::CollectExtraFiles(dir, manifest);
    bool hasFlink = false;
    for (const std::string& rel : extras) {
        if (rel == "flink.txt") hasFlink = true;
    }
    Expect(hasFlink, "symlink->file is a non-dir candidate -> extra (FR-07/B7/S-01)");

    std::error_code ec;
    fs::remove_all(dir, ec);
}

#ifndef _WIN32
// TM-05/AC-13 (FR-13): a POSIX symlink to a DIRECTORY must NOT be recursed, so the target's contents
// never appear as extras, and the symlink itself (a directory) is never an extra file. POSIX-only: on
// Windows a directory reparse point carries FILE_ATTRIBUTE_DIRECTORY and is intentionally handled as a
// plain directory (design R-A3), so this FR-13 invariant is asserted only where it applies.
void TestCollectExtraLocalSymlinkDirNotRecursed() {
    const fs::path dir = MakeTempDir();
    const fs::path externalDir = MakeTempDir();  // outside the walked root
    WriteFile(externalDir / "inside.txt", "x");
    WriteFile(dir / "normal.txt", "y");

    std::error_code linkEc;
    fs::create_directory_symlink(externalDir, dir / "linkdir", linkEc);
    if (linkEc) {
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::remove_all(externalDir, ec);
        return;  // no privilege -> skip
    }

    const std::vector<std::string> extras = fc::CollectExtraFiles(dir, std::unordered_set<std::string>{});
    bool hasNormal = false;
    bool recursed = false;
    bool hasLinkDir = false;
    for (const std::string& rel : extras) {
        if (rel == "normal.txt") hasNormal = true;
        if (rel == "linkdir/inside.txt") recursed = true;
        if (rel == "linkdir") hasLinkDir = true;
    }
    Expect(hasNormal, "regular extra still collected alongside the symlink dir");
    Expect(!recursed, "symlink directory is NOT recursed (FR-13/AC-13)");
    Expect(!hasLinkDir, "symlink directory itself is a dir -> never an extra file (FR-06)");

    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::remove_all(externalDir, ec);
}

// TM-02/AC-05 (FR-08/B6): a permission-denied subdirectory is skipped (skip_permission_denied) without
// aborting the walk, and accessible siblings are still collected. POSIX-only (relies on chmod 000); on
// Windows the equivalent open-failure skip is the OpenDirFind INVALID_HANDLE_VALUE path.
void TestCollectExtraLocalPermissionDenied() {
    const fs::path dir = MakeTempDir();
    WriteFile(dir / "visible.txt", "v");
    const fs::path noperm = dir / "noperm";
    fs::create_directories(noperm);
    WriteFile(noperm / "secret.txt", "s");

    std::error_code permEc;
    fs::permissions(noperm, fs::perms::none, permEc);
    if (permEc) {
        std::error_code ec;
        fs::permissions(noperm, fs::perms::owner_all, ec);
        fs::remove_all(dir, ec);
        return;  // could not drop permissions -> skip
    }
    // If the mode change did not actually deny reads (e.g. running as root), the skip path is not
    // exercised; skip to keep the assertion meaningful rather than flaky.
    std::error_code probeEc;
    fs::directory_iterator probe(noperm, probeEc);
    if (!probeEc) {
        std::error_code ec;
        fs::permissions(noperm, fs::perms::owner_all, ec);
        fs::remove_all(dir, ec);
        return;
    }

    const std::vector<std::string> extras = fc::CollectExtraFiles(dir, std::unordered_set<std::string>{});
    bool hasVisible = false;
    bool hasSecret = false;
    for (const std::string& rel : extras) {
        if (rel == "visible.txt") hasVisible = true;
        if (rel == "noperm/secret.txt") hasSecret = true;
    }
    Expect(hasVisible, "accessible sibling collected despite a permission-denied dir (FR-08)");
    Expect(!hasSecret, "permission-denied dir contents skipped, walk not aborted (FR-08/B6)");

    std::error_code ec;
    fs::permissions(noperm, fs::perms::owner_all, ec);  // restore so remove_all can clean up
    fs::remove_all(dir, ec);
}
#endif  // !_WIN32

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
    TestCollectExtraLocalDotDotSkip();
    TestCollectExtraLocalSymlinkFile();
#ifndef _WIN32
    TestCollectExtraLocalSymlinkDirNotRecursed();
    TestCollectExtraLocalPermissionDenied();
#endif
    TestPrintCompareCounters();
}
