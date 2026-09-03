// Shared local-probe unit tests - FastCheckTests side (task unify-probe-extra-shared,
// design §8.2.1; FR-10, AC-01/02/03/11, B-02/B-03/B-08/B-11).
// Runs the SAME fixture and the SAME expected dump as the FastCloneTests side
// (tests/test_shared_probe.cpp), but through the ON-DEMAND form the FastCheck engine
// actually uses (ctx == nullptr) - both binaries prove one identical probe-answer
// stream (AC-03). NFR-04/I-6: no DirProbeCache is constructed anywhere on this side.

#include "local_probe.h"
#include "test_fixture_support.h"

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
        throw std::runtime_error("test_shared_probe(check): " + message);
    }
}

// Probe every fixture file through the ON-DEMAND form (the FastCheck shape).
std::vector<fc::FileEntry> ProbeAllOnDemand(const fc::test::ProbeFixture& fx) {
    std::vector<fc::FileEntry> out;
    for (const std::string& rel : fx.relPaths) {
        const std::optional<fc::FileEntry> e = fc::ProbeLocalFile(fx.root, rel, nullptr);
        Expect(e.has_value(), "on-demand probe must hit fixture file: " + rel);
        out.push_back(*e);
    }
    return out;
}

// AC-03: byte-equal dump against the SAME shared expectation the FastCloneTests side
// compares with.
void TestProbeDumpByteEqual() {
    const fc::test::ProbeFixture fx = fc::test::MakeProbeFixture();
    const std::string dump = fc::test::BuildProbeDump(ProbeAllOnDemand(fx));
    fc::test::WriteDumpIfRequested("probe_dump_fastcheck", dump);
    const std::string expected = fc::test::BuildExpectedProbeDump();
    Expect(dump == expected, "on-demand dump must equal the shared expected dump (AC-03)");
}

// FR-04: within one process the on-demand form agrees with the batch form (the batch
// form is legal here because FastCheckTests links the same shared TU; production
// FastCheck never constructs the cache - I-6).
void TestProbeFormAgreement() {
    const fc::test::ProbeFixture fx = fc::test::MakeProbeFixture();
    fc::DirProbeCache cache;
    for (const std::string& rel : fx.relPaths) {
        const std::optional<fc::FileEntry> a = fc::ProbeLocalFile(fx.root, rel, &cache);
        const std::optional<fc::FileEntry> b = fc::ProbeLocalFile(fx.root, rel, nullptr);
        Expect(a.has_value() && b.has_value(), "both forms must hit: " + rel);
        Expect(a->relativePath == b->relativePath && a->isDirectory == b->isDirectory &&
                   a->fileSize == b->fileSize && a->mtimeNs == b->mtimeNs,
               "form agreement (FR-04): " + rel);
    }
}

// AC-11: the on-demand form never returns a stale snapshot.
void TestNoCacheNoStaleSnapshot() {
    const fc::test::ProbeFixture fx = fc::test::MakeProbeFixture();
    const std::string rel = "a.txt";
    const std::optional<fc::FileEntry> before = fc::ProbeLocalFile(fx.root, rel, nullptr);
    Expect(before.has_value(), "on-demand probe hits a.txt");
    const std::filesystem::path abs = fx.root / rel;
    fc::test::WriteBinaryFile(abs, 50 + 64);
    fc::SetFileModifyTime(abs, fc::test::FixtureMtimeNs(0) + 70000000000LL);
    const std::optional<fc::FileEntry> after = fc::ProbeLocalFile(fx.root, rel, nullptr);
    Expect(after.has_value(), "on-demand probe still hits a.txt after rewrite");
    Expect(after->fileSize != before->fileSize, "on-demand must NOT return stale size (AC-11)");
    Expect(after->mtimeNs != before->mtimeNs, "on-demand must NOT return stale mtime (AC-11)");
}

// B-02/B-03/B-08: missing / directory / empty file / non-ASCII.
void TestProbeNegativeCases() {
    const fc::test::ProbeFixture fx = fc::test::MakeProbeFixture();
    Expect(!fc::ProbeLocalFile(fx.root, "does_not_exist.bin", nullptr).has_value(),
           "missing file -> nullopt");
    Expect(!fc::ProbeLocalFile(fx.root, "nested", nullptr).has_value(), "directory -> nullopt");
    const std::optional<fc::FileEntry> empty = fc::ProbeLocalFile(fx.root, "empty.bin", nullptr);
    Expect(empty.has_value() && empty->fileSize == 0, "empty file hits with fileSize == 0 (B-08)");
    Expect(fc::ProbeLocalFile(fx.root, fx.relPaths.back(), nullptr).has_value(),
           "non-ASCII name hits");
}

// D-06 regression: the Strict-mode size-only probe (the shape check_engine.cpp injects).
void TestStrictProbeSizeOnly() {
    const fc::test::ProbeFixture fx = fc::test::MakeProbeFixture();
    const std::optional<fc::FileEntry> a = fc::ProbeLocalFileSizeOnly(fx.root, "a.txt");
    Expect(a.has_value() && a->fileSize == 50, "size-only returns correct size");
    Expect(a->mtimeNs == 0, "size-only mtimeNs is always 0");
    Expect(!fc::ProbeLocalFileSizeOnly(fx.root, "nested").has_value(),
           "size-only: directory -> nullopt");
    Expect(!fc::ProbeLocalFileSizeOnly(fx.root, "nope.bin").has_value(),
           "size-only: missing -> nullopt");
}

}  // namespace

namespace fc::test {

void RunSharedProbeTestsFastCheckSide() {
    TestProbeDumpByteEqual();
    TestProbeFormAgreement();
    TestNoCacheNoStaleSnapshot();
    TestProbeNegativeCases();
    TestStrictProbeSizeOnly();
}

}  // namespace fc::test
