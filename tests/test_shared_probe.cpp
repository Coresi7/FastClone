// Shared local-probe unit tests - FastCloneTests side (task unify-probe-extra-shared,
// design §8.2.1; FR-10, AC-01/02/03/10/11, B-02/B-03/B-08/B-11).
// The FastCheckTests side (FastCheck/tests/test_shared_probe.cpp) runs the SAME fixture
// and the SAME expected dump through the on-demand form, so both binaries prove they
// produce one identical probe-answer stream (AC-03).

#include "local_probe.h"
#include "test_fixture_support.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

void Expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error("test_shared_probe: " + message);
    }
}

// Probe every fixture file through the BATCH form (DirProbeCache, the FastClone shape).
std::vector<fc::FileEntry> ProbeAllBatch(const fc::test::ProbeFixture& fx, fc::DirProbeCache& cache) {
    std::vector<fc::FileEntry> out;
    for (const std::string& rel : fx.relPaths) {
        const std::optional<fc::FileEntry> e = fc::ProbeLocalFile(fx.root, rel, &cache);
        Expect(e.has_value(), "batch probe must hit fixture file: " + rel);
        out.push_back(*e);
    }
    return out;
}

// Probe every fixture file through the ON-DEMAND form (ctx == nullptr, the FastCheck shape).
std::vector<fc::FileEntry> ProbeAllOnDemand(const fc::test::ProbeFixture& fx) {
    std::vector<fc::FileEntry> out;
    for (const std::string& rel : fx.relPaths) {
        const std::optional<fc::FileEntry> e = fc::ProbeLocalFile(fx.root, rel, nullptr);
        Expect(e.has_value(), "on-demand probe must hit fixture file: " + rel);
        out.push_back(*e);
    }
    return out;
}

// AC-01/AC-02/AC-03: per-field values + byte-equal dump against the shared expectation.
void TestProbeDumpByteEqual() {
    const fc::test::ProbeFixture fx = fc::test::MakeProbeFixture();

    fc::DirProbeCache cache;
    const std::string dumpBatch = fc::test::BuildProbeDump(ProbeAllBatch(fx, cache));
    const std::string dumpOnDemand = fc::test::BuildProbeDump(ProbeAllOnDemand(fx));

    fc::test::WriteDumpIfRequested("probe_dump_fastclone", dumpBatch);
    const std::string expected = fc::test::BuildExpectedProbeDump();
    Expect(dumpBatch == expected, "batch-form dump must equal the shared expected dump (AC-03)");
    Expect(dumpOnDemand == expected, "on-demand-form dump must equal the shared expected dump (AC-03)");
}

// FR-04: batch and on-demand forms must agree on all four fields for the same file
// (batch first, no modification in between - B-01).
void TestProbeFormAgreement() {
    const fc::test::ProbeFixture fx = fc::test::MakeProbeFixture();
    fc::DirProbeCache cache;
    for (const std::string& rel : fx.relPaths) {
        const std::optional<fc::FileEntry> a = fc::ProbeLocalFile(fx.root, rel, &cache);
        const std::optional<fc::FileEntry> b = fc::ProbeLocalFile(fx.root, rel, nullptr);
        Expect(a.has_value() && b.has_value(), "both forms must hit: " + rel);
        Expect(a->relativePath == b->relativePath, "relativePath agreement: " + rel);
        Expect(a->isDirectory == b->isDirectory, "isDirectory agreement: " + rel);
        Expect(a->fileSize == b->fileSize, "fileSize agreement (FR-04): " + rel);
        Expect(a->mtimeNs == b->mtimeNs, "mtimeNs agreement (FR-04, divergence-point A fix): " + rel);
    }
}

// AC-10: the slow path (real directory enumeration) runs exactly once per directory.
void TestCacheSlowPathCountIsOne() {
    const fc::test::ProbeFixture fx = fc::test::MakeProbeFixture();
    fc::DirProbeCache cache;
    // Same directory, 3 files (a.txt / b.bin / empty.bin at the root -> 3 distinct
    // directories among the 6 fixture files: ".", "nested", "nested/deep").
    (void)fc::ProbeLocalFile(fx.root, "a.txt", &cache);
    (void)fc::ProbeLocalFile(fx.root, "b.bin", &cache);
    (void)fc::ProbeLocalFile(fx.root, "empty.bin", &cache);
    Expect(cache.SlowPathCount() == 1, "3 files in one directory -> exactly 1 slow path (AC-10)");
    // Repeat probes hit the cache: still 1.
    (void)fc::ProbeLocalFile(fx.root, "a.txt", &cache);
    Expect(cache.SlowPathCount() == 1, "cache hits add no slow path (I-5)");

    fc::DirProbeCache cache2;
    (void)fc::ProbeLocalFile(fx.root, "a.txt", &cache2);              // "."
    (void)fc::ProbeLocalFile(fx.root, "nested/c.dat", &cache2);       // "nested"
    (void)fc::ProbeLocalFile(fx.root, "nested/deep/d.dat", &cache2);  // "nested/deep"
    Expect(cache2.SlowPathCount() == 3, "3 directories -> exactly 3 slow paths (AC-10)");
}

// AC-11: the on-demand form never returns a stale snapshot (no cache is involved), and
// the on-demand path never constructs a DirProbeCache (structural: no cache object here).
void TestNoCacheNoStaleSnapshot() {
    const fc::test::ProbeFixture fx = fc::test::MakeProbeFixture();
    const std::string rel = "a.txt";
    const std::optional<fc::FileEntry> before = fc::ProbeLocalFile(fx.root, rel, nullptr);
    Expect(before.has_value(), "on-demand probe hits a.txt");

    // Modify content AND length, then re-stamp the mtime (B-01 discipline).
    const std::filesystem::path abs = fx.root / rel;
    fc::test::WriteBinaryFile(abs, 50 + 64);
    fc::SetFileModifyTime(abs, fc::test::FixtureMtimeNs(0) + 70000000000LL);  // +70s

    const std::optional<fc::FileEntry> after = fc::ProbeLocalFile(fx.root, rel, nullptr);
    Expect(after.has_value(), "on-demand probe still hits a.txt after rewrite");
    Expect(after->fileSize != before->fileSize, "on-demand must NOT return stale size (AC-11)");
    Expect(after->mtimeNs != before->mtimeNs, "on-demand must NOT return stale mtime (AC-11)");
}

// B-02/B-03/B-08/B-11: missing path / directory / empty file / long path / non-ASCII.
void TestProbeNegativeCases() {
    const fc::test::ProbeFixture fx = fc::test::MakeProbeFixture();

    Expect(!fc::ProbeLocalFile(fx.root, "does_not_exist.bin", nullptr).has_value(),
           "missing file -> nullopt");
    Expect(!fc::ProbeLocalFile(fx.root, "nested", nullptr).has_value(),
           "directory -> nullopt");
    // Empty file (B-08): 0 bytes is a HIT, not missing.
    const std::optional<fc::FileEntry> empty = fc::ProbeLocalFile(fx.root, "empty.bin", nullptr);
    Expect(empty.has_value() && empty->fileSize == 0, "empty file hits with fileSize == 0 (B-08)");
    // Non-ASCII name hits (fixture's last file).
    const std::string nonAscii = fx.relPaths.back();
    Expect(fc::ProbeLocalFile(fx.root, nonAscii, nullptr).has_value(), "non-ASCII name hits");

#ifdef _WIN32
    // B-11 long path: build > 260 chars; must still HIT through JoinRel's extended-
    // length prefix (not mis-reported as missing). The fixture file is written via
    // WriteSmallFileFastPath (CreateFileW on the extended-length form) because a plain
    // ofstream cannot open >260-char paths at all.
    std::string longRel;
    while (longRel.size() < 240) {
        longRel += "long_dir_segment_32chars_abcdefghij/";
    }
    longRel += "deep_file.txt";
    const std::filesystem::path longAbs = fx.root / longRel;
    // Parent dirs first (extended-length form: CreateDirectoriesLong's per-layer
    // CreateDirectoryW can only exceed MAX_PATH through the "\\?\" prefix);
    // WriteSmallFileFastPath itself never creates parents (its documented B6 contract).
    fc::CreateDirectoriesLong(std::filesystem::path(fc::ToExtendedLengthPath(longAbs.parent_path())));
    const std::string payload(10, 'x');
    const bool wrote = fc::WriteSmallFileFastPath(
        longAbs, reinterpret_cast<const uint8_t*>(payload.data()), payload.size(),
        fc::test::FixtureMtimeNs(0));
    Expect(wrote, "long-path fixture write must succeed (WriteSmallFileFastPath)");
    const std::optional<fc::FileEntry> lp = fc::ProbeLocalFile(fx.root, longRel, nullptr);
    Expect(lp.has_value() && lp->fileSize == 10, "long path (>260 chars) hits, not Missing (B-11)");
    // The same long path through the batch form.
    fc::DirProbeCache cache;
    const std::optional<fc::FileEntry> lpb = fc::ProbeLocalFile(fx.root, longRel, &cache);
    Expect(lpb.has_value() && lpb->fileSize == 10, "long path hits via cache form (B-11)");
#endif
}

// D-06 regression: size-only variant reports size and mtimeNs == 0; directory/missing
// -> nullopt.
void TestStrictProbeSizeOnly() {
    const fc::test::ProbeFixture fx = fc::test::MakeProbeFixture();
    const std::optional<fc::FileEntry> a = fc::ProbeLocalFileSizeOnly(fx.root, "a.txt");
    Expect(a.has_value() && a->fileSize == 50, "size-only returns correct size");
    Expect(a->mtimeNs == 0, "size-only mtimeNs is always 0 (Strict ignores mtime)");
    const std::optional<fc::FileEntry> d = fc::ProbeLocalFileSizeOnly(fx.root, "nested");
    Expect(!d.has_value(), "size-only: directory -> nullopt");
    const std::optional<fc::FileEntry> m = fc::ProbeLocalFileSizeOnly(fx.root, "nope.bin");
    Expect(!m.has_value(), "size-only: missing -> nullopt");
    // The size-only batch form (ctx != nullptr) was removed with the ctx parameter in the
    // unify-probe-extra-shared-converge cleanup: it never existed in production (FastClone
    // hardcodes CompareMode::Fast and never uses SizeOnly; FastCheck Strict probes
    // on-demand). All on-demand size-only assertions above are unchanged.
}

}  // namespace

namespace fc::test {

void RunSharedProbeTestsFastCloneSide() {
    TestProbeDumpByteEqual();
    TestProbeFormAgreement();
    TestCacheSlowPathCountIsOne();
    TestNoCacheNoStaleSnapshot();
    TestProbeNegativeCases();
    TestStrictProbeSizeOnly();
}

}  // namespace fc::test
