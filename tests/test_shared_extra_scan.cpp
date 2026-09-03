// Shared extra-scan unit tests - FastCloneTests side (task unify-probe-extra-shared,
// design §8.2.2; FR-11, AC-04, NFR-11, B-03/B-04/B-05/B-06).
// The FastCheckTests side (FastCheck/tests/test_shared_extra_scan.cpp) runs the same
// fixture with the unordered_set manifest shape FastCheck uses, proving the two entry
// templates give the same extraFiles answer (FR-09).

#include "extra_scan.h"
#include "path_utils.h"  // fc::IsPathUnderRoot
#include "sync_util.h"   // fc::CurrentExePath

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "test_fixture_support.h"  // WriteBinaryFile / FixtureMtimeNs

namespace fs = std::filesystem;

namespace {

void Expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error("test_shared_extra_scan: " + message);
    }
}

// scan_fixture/ layout (design §8.2.2). Remote sets: files {m1.txt, m2.txt},
// dirs {nested, nested/deep}.
struct ScanFixture {
    fs::path root;
    fs::path outsideFile;  // a real file OUTSIDE the root (E-3)
};

ScanFixture MakeScanFixture() {
    using clock = std::chrono::steady_clock;
    const auto stamp = clock::now().time_since_epoch().count();
    ScanFixture fx;
    fx.root = fs::temp_directory_path() / ("fc_shared_scan_" + std::to_string(stamp));
    fx.outsideFile = fs::temp_directory_path() / ("fc_shared_scan_outside_" + std::to_string(stamp) + ".bin");
    fc::test::WriteBinaryFile(fx.root / "m1.txt", 10);
    fc::test::WriteBinaryFile(fx.root / "m2.txt", 20);
    fc::test::WriteBinaryFile(fx.root / "x1.txt", 30);
    fc::test::WriteBinaryFile(fx.root / "zero.bin", 0);
    fc::test::WriteBinaryFile(fx.root / "nested/x2.txt", 40);
    fc::test::WriteBinaryFile(fx.root / "nested/deep/x3.dat", 50);
    fc::test::WriteBinaryFile(fx.root / "\xe4\xb8\xad\xe6\x96\x87 \xce\xa9.txt", 7);  // 中文 Ω.txt
    fs::create_directories(fx.root / "nested/empty");
    fc::test::WriteBinaryFile(fx.outsideFile, 99);
#ifndef _WIN32
    fs::create_symlink(fx.root / "m1.txt", fx.root / "link_to_file");
    fs::create_symlink(fx.root / "nested", fx.root / "link_to_dir");
#endif
    return fx;
}

std::vector<std::string> ExpectedExtras() {
#ifdef _WIN32
    return {"nested/deep/x3.dat", "nested/x2.txt", "x1.txt", "zero.bin",
            "\xe4\xb8\xad\xe6\x96\x87 \xce\xa9.txt"};
#else
    // POSIX: symlink->file follows regular-file semantics (not in manifest -> extra);
    // symlink->dir is not recursed and never counted (B-03).
    return {"link_to_file", "nested/deep/x3.dat", "nested/x2.txt", "x1.txt", "zero.bin",
            "\xe4\xb8\xad\xe6\x96\x87 \xce\xa9.txt"};
#endif
}

// FastClone's remote-file container shape (unordered_map<string, FileEntry>).
std::unordered_map<std::string, fc::FileEntry> MakeRemoteFiles() {
    std::unordered_map<std::string, fc::FileEntry> files;
    fc::FileEntry m1;
    m1.relativePath = "m1.txt";
    m1.fileSize = 10;
    files.emplace("m1.txt", m1);
    fc::FileEntry m2;
    m2.relativePath = "m2.txt";
    m2.fileSize = 20;
    files.emplace("m2.txt", m2);
    return files;
}

std::unordered_set<std::string> MakeRemoteDirs() {
    return {"nested", "nested/deep"};
}

bool VectorsEqual(const std::vector<std::string>& a, const std::vector<std::string>& b) {
    return a == b;
}

// E-1..E-4 (FR-07): exclude nullopt / hit / outside-root / nonexistent.
void TestExcludeMatrix() {
    const ScanFixture fx = MakeScanFixture();
    const auto remote = MakeRemoteFiles();
    const std::vector<std::string> expected = ExpectedExtras();

    // E-1: no exclude.
    std::vector<std::string> got = fc::CollectExtraFiles(fx.root, remote);
    Expect(VectorsEqual(got, expected), "E-1: nullopt exclude -> full extra list, ascending");

    // E-2: exclude a real file under the root (canonical absolute path).
    const fs::path x1 = fs::weakly_canonical(fx.root / "x1.txt");
    fc::ExtraScanOptions hit;
    hit.excludeAbsPath = x1;
    got = fc::CollectExtraFiles(fx.root, remote, hit);
    std::vector<std::string> expectedE2;
    for (const std::string& s : expected) {
        if (s != "x1.txt") {
            expectedE2.push_back(s);
        }
    }
    Expect(VectorsEqual(got, expectedE2), "E-2: under-root exclude removes exactly that file");

    // E-3: exclude points outside the root -> identical to E-1 (B-06).
    fc::ExtraScanOptions outside;
    outside.excludeAbsPath = fs::weakly_canonical(fx.outsideFile);
    got = fc::CollectExtraFiles(fx.root, remote, outside);
    Expect(VectorsEqual(got, expected), "E-3: outside-root exclude changes nothing");

    // E-4: exclude points at a nonexistent path -> identical to E-1 (B-05).
    fc::ExtraScanOptions ghost;
    ghost.excludeAbsPath = fx.root / "no_such_file.bin";
    got = fc::CollectExtraFiles(fx.root, remote, ghost);
    Expect(VectorsEqual(got, expected), "E-4: nonexistent exclude changes nothing");
}

// FR-09: both entry forms give the same extraFiles answer for every exclude variant,
// strictly ascending (NFR-11).
void TestBothFormsSameAnswer() {
    const ScanFixture fx = MakeScanFixture();
    const auto remote = MakeRemoteFiles();
    const auto remoteDirs = MakeRemoteDirs();
    const fs::path x1 = fs::weakly_canonical(fx.root / "x1.txt");

    const std::vector<std::optional<fs::path>> excludes = {
        std::nullopt, x1, fs::weakly_canonical(fx.outsideFile), fx.root / "no_such_file.bin"};

    for (const std::optional<fs::path>& ex : excludes) {
        fc::ExtraScanOptions opts;
        opts.excludeAbsPath = ex;
        const std::vector<std::string> filesOnly = fc::CollectExtraFiles(fx.root, remote, opts);
        const fc::ExtraScanResult both = fc::CollectExtraFilesAndDirs(fx.root, remote, remoteDirs, opts);
        Expect(VectorsEqual(filesOnly, both.extraFiles),
               "FR-09: both forms must give the same extraFiles answer");
        for (size_t i = 1; i < filesOnly.size(); ++i) {
            Expect(filesOnly[i - 1] < filesOnly[i], "NFR-11: strictly ascending extras");
        }
    }
}

// collectDirs=false -> dir vectors empty; collectDirs=true -> localDirs = all dirs,
// extraDirs = local-minus-remote dirs; a directory NEVER appears in extraFiles.
void TestDirsOnlyCollectedWhenRequested() {
    const ScanFixture fx = MakeScanFixture();
    const auto remote = MakeRemoteFiles();
    const auto remoteDirs = MakeRemoteDirs();

    // Entry 1 (collectDirs forced off, I-6).
    const std::vector<std::string> filesOnly = fc::CollectExtraFiles(fx.root, remote);

    // Entry 2 with dir info.
    fc::ExtraScanOptions opts;  // collectDirs forced on by entry 2
    const fc::ExtraScanResult both = fc::CollectExtraFilesAndDirs(fx.root, remote, remoteDirs, opts);
    Expect(VectorsEqual(both.extraFiles, filesOnly), "entry-2 extraFiles == entry-1 answer");

    // localDirs: every dir seen. Windows: nested, nested/deep, nested/empty.
    // POSIX adds link_to_dir? No - symlink dirs are not recursed and not recorded as
    // localDirs? They ARE seen as directories by the walker (is_directory follows the
    // link) - the symlink check only gates RECURSION. On POSIX the expected localDirs
    // therefore also contain link_to_dir.
#ifdef _WIN32
    const std::vector<std::string> expectedLocalDirs = {"nested", "nested/deep", "nested/empty"};
    const std::vector<std::string> expectedExtraDirs = {"nested/empty"};
#else
    const std::vector<std::string> expectedLocalDirs = {"link_to_dir", "nested", "nested/deep", "nested/empty"};
    const std::vector<std::string> expectedExtraDirs = {"link_to_dir", "nested/empty"};
#endif
    Expect(VectorsEqual(both.localDirs, expectedLocalDirs), "localDirs = all local dirs, ascending");
    Expect(VectorsEqual(both.extraDirs, expectedExtraDirs), "extraDirs = local dirs absent from remote");

    // A directory never appears in extraFiles (FR-06).
    for (const std::string& f : both.extraFiles) {
        const bool isKnownDir = std::find(expectedLocalDirs.begin(), expectedLocalDirs.end(), f) != expectedLocalDirs.end();
        Expect(!isKnownDir, "no directory may appear in extraFiles");
    }
}

// OQ-01=A rule-level injector: verify the three constituent steps of
// SelfExcludeUnderRoot (design §8.2.2 note: no production injection hook is added for
// testability; the end-to-end effect is pinned by L4 S5/S6).
void TestSelfExcludeUnderRootRules() {
    // Step 1: CurrentExePath is available.
    const std::optional<fs::path> self = fc::CurrentExePath();
    Expect(self.has_value(), "CurrentExePath must resolve");

    // Step 2+3: a root that cannot contain this process's exe -> nullopt (B-06/E-3).
    using clock = std::chrono::steady_clock;
    const auto stamp = clock::now().time_since_epoch().count();
    const fs::path emptyRoot = fs::temp_directory_path() / ("fc_selfexcl_" + std::to_string(stamp));
    fs::create_directories(emptyRoot);
    const std::optional<fs::path> none = fc::SelfExcludeUnderRoot(emptyRoot);
    Expect(!none.has_value(), "self outside root -> nullopt");

    // Composite rule check with a fake self: weakly_canonical + IsPathUnderRoot must
    // accept a file that really is under the root (the injection rule's "when and only
    // when" positive arm).
    const fs::path fakeSelf = emptyRoot / "fake_self.exe";
    fc::test::WriteBinaryFile(fakeSelf, 16);
    std::error_code ec;
    const fs::path rootCanon = fs::weakly_canonical(emptyRoot, ec);
    Expect(!ec, "weakly_canonical(root) succeeds");
    const fs::path selfCanon = fs::weakly_canonical(fakeSelf, ec);
    Expect(!ec, "weakly_canonical(fakeSelf) succeeds");
    Expect(fc::IsPathUnderRoot(rootCanon, selfCanon), "under-root file passes IsPathUnderRoot");
}

}  // namespace

namespace fc::test {

void RunSharedExtraScanTestsFastCloneSide() {
    TestExcludeMatrix();
    TestBothFormsSameAnswer();
    TestDirsOnlyCollectedWhenRequested();
    TestSelfExcludeUnderRootRules();
}

}  // namespace fc::test
