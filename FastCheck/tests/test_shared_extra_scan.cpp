// Shared extra-scan unit tests - FastCheckTests side (task unify-probe-extra-shared,
// design §8.2.2; FR-11, AC-04, NFR-11, B-03/B-04/B-05/B-06).
// Same fixture and same expected values as the FastCloneTests side
// (tests/test_shared_extra_scan.cpp), but through FastCheck's manifest shape
// (unordered_set<string>) - the two entry templates must give the same extraFiles
// answer (FR-09).

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
#include <unordered_set>
#include <vector>

#include "test_fixture_support.h"  // WriteBinaryFile

namespace fs = std::filesystem;

namespace {

void Expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error("test_shared_extra_scan(check): " + message);
    }
}

struct ScanFixture {
    fs::path root;
    fs::path outsideFile;
};

ScanFixture MakeScanFixture() {
    using clock = std::chrono::steady_clock;
    const auto stamp = clock::now().time_since_epoch().count();
    ScanFixture fx;
    fx.root = fs::temp_directory_path() / ("fc_shared_scan_chk_" + std::to_string(stamp));
    fx.outsideFile =
        fs::temp_directory_path() / ("fc_shared_scan_out_chk_" + std::to_string(stamp) + ".bin");
    fc::test::WriteBinaryFile(fx.root / "m1.txt", 10);
    fc::test::WriteBinaryFile(fx.root / "m2.txt", 20);
    fc::test::WriteBinaryFile(fx.root / "x1.txt", 30);
    fc::test::WriteBinaryFile(fx.root / "zero.bin", 0);
    fc::test::WriteBinaryFile(fx.root / "nested/x2.txt", 40);
    fc::test::WriteBinaryFile(fx.root / "nested/deep/x3.dat", 50);
    fc::test::WriteBinaryFile(fx.root / "\xe4\xb8\xad\xe6\x96\x87 \xce\xa9.txt", 7);
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
    return {"link_to_file", "nested/deep/x3.dat", "nested/x2.txt", "x1.txt", "zero.bin",
            "\xe4\xb8\xad\xe6\x96\x87 \xce\xa9.txt"};
#endif
}

// FastCheck's manifest shape (unordered_set<string>).
std::unordered_set<std::string> MakeManifest() {
    return {"m1.txt", "m2.txt"};
}

// E-1..E-4 (FR-07).
void TestExcludeMatrix() {
    const ScanFixture fx = MakeScanFixture();
    const auto manifest = MakeManifest();
    const std::vector<std::string> expected = ExpectedExtras();

    std::vector<std::string> got = fc::CollectExtraFiles(fx.root, manifest);
    Expect(got == expected, "E-1: nullopt exclude -> full extra list, ascending");

    fc::ExtraScanOptions hit;
    hit.excludeAbsPath = fs::weakly_canonical(fx.root / "x1.txt");
    got = fc::CollectExtraFiles(fx.root, manifest, hit);
    std::vector<std::string> expectedE2;
    for (const std::string& s : expected) {
        if (s != "x1.txt") {
            expectedE2.push_back(s);
        }
    }
    Expect(got == expectedE2, "E-2: under-root exclude removes exactly that file");

    fc::ExtraScanOptions outside;
    outside.excludeAbsPath = fs::weakly_canonical(fx.outsideFile);
    got = fc::CollectExtraFiles(fx.root, manifest, outside);
    Expect(got == expected, "E-3: outside-root exclude changes nothing");

    fc::ExtraScanOptions ghost;
    ghost.excludeAbsPath = fx.root / "no_such_file.bin";
    got = fc::CollectExtraFiles(fx.root, manifest, ghost);
    Expect(got == expected, "E-4: nonexistent exclude changes nothing");
}

// FR-09: both entry forms give the same extraFiles answer (the dir-entry form is
// exercised with the same set shape for the remote dirs).
void TestBothFormsSameAnswer() {
    const ScanFixture fx = MakeScanFixture();
    const auto manifest = MakeManifest();
    const std::unordered_set<std::string> remoteDirs = {"nested", "nested/deep"};

    const std::vector<std::optional<fs::path>> excludes = {
        std::nullopt,
        fs::weakly_canonical(fx.root / "x1.txt"),
        fs::weakly_canonical(fx.outsideFile),
        fx.root / "no_such_file.bin"};

    for (const std::optional<fs::path>& ex : excludes) {
        fc::ExtraScanOptions opts;
        opts.excludeAbsPath = ex;
        const std::vector<std::string> filesOnly = fc::CollectExtraFiles(fx.root, manifest, opts);
        const fc::ExtraScanResult both =
            fc::CollectExtraFilesAndDirs(fx.root, manifest, remoteDirs, opts);
        Expect(filesOnly == both.extraFiles, "FR-09: both forms must give the same answer");
        for (size_t i = 1; i < filesOnly.size(); ++i) {
            Expect(filesOnly[i - 1] < filesOnly[i], "NFR-11: strictly ascending extras");
        }
    }
}

// Directories never appear in extraFiles (FR-06); with collectDirs forced on, extraDirs
// is the local-minus-remote dir set.
void TestDirsNeverExtra() {
    const ScanFixture fx = MakeScanFixture();
    const auto manifest = MakeManifest();
    const std::unordered_set<std::string> remoteDirs = {"nested", "nested/deep"};
    const fc::ExtraScanResult both = fc::CollectExtraFilesAndDirs(fx.root, manifest, remoteDirs);
#ifdef _WIN32
    const std::vector<std::string> expectedExtraDirs = {"nested/empty"};
#else
    const std::vector<std::string> expectedExtraDirs = {"link_to_dir", "nested/empty"};
#endif
    Expect(both.extraDirs == expectedExtraDirs, "extraDirs = local dirs absent from remote");
    for (const std::string& f : both.extraFiles) {
        const bool isKnownDir =
            std::find(expectedExtraDirs.begin(), expectedExtraDirs.end(), f) != expectedExtraDirs.end() ||
            std::find(both.localDirs.begin(), both.localDirs.end(), f) != both.localDirs.end();
        Expect(!isKnownDir, "no directory may appear in extraFiles");
    }
}

// OQ-01=A rule-level injector: constituent steps (design §8.2.2 note).
void TestSelfExcludeUnderRootRules() {
    const std::optional<fs::path> self = fc::CurrentExePath();
    Expect(self.has_value(), "CurrentExePath must resolve");

    using clock = std::chrono::steady_clock;
    const auto stamp = clock::now().time_since_epoch().count();
    const fs::path emptyRoot = fs::temp_directory_path() / ("fc_selfexcl_chk_" + std::to_string(stamp));
    fs::create_directories(emptyRoot);
    const std::optional<fs::path> none = fc::SelfExcludeUnderRoot(emptyRoot);
    Expect(!none.has_value(), "self outside root -> nullopt");

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

void RunSharedExtraScanTestsFastCheckSide() {
    TestExcludeMatrix();
    TestBothFormsSameAnswer();
    TestDirsNeverExtra();
    TestSelfExcludeUnderRootRules();
}

}  // namespace fc::test
