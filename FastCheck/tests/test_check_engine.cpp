#include "check_engine.h"

#include "file_index.h"
#include "protocol.h"
#include "protocol_codec.h"

#include <atomic>
#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace fc;
using namespace fc::check;

namespace {

void Expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error("test_check_engine: " + message);
    }
}

// Find a per-file entry by relative path (only filter-passing ones enter entries).
const DiffEntry* FindEntry(const CheckResult& r, const std::string& path) {
    for (const DiffEntry& e : r.entries) {
        if (e.path == path) {
            return &e;
        }
    }
    return nullptr;
}

// In-memory scripted double: recv pops inbound in order; when exhausted it throws an "orderly close" exception (replicating disconnect semantics). send records frames.
struct MockChannel {
    std::deque<Frame> inbound;
    std::vector<Frame> sent;
    bool recvThrowsImmediately = false;

    FrameChannel Make() {
        FrameChannel ch;
        ch.send = [this](const Frame& frame) { sent.push_back(frame); };
        ch.recv = [this]() -> Frame {
            if (recvThrowsImmediately || inbound.empty()) {
                throw std::runtime_error("recv failed WSA=0 (mock EOF)");
            }
            Frame frame = inbound.front();
            inbound.pop_front();
            return frame;
        };
        return ch;
    }

    size_t CountSent(MsgType type) const {
        size_t n = 0;
        for (const Frame& f : sent) {
            if (f.type == type) {
                ++n;
            }
        }
        return n;
    }
    bool SentAny(MsgType type) const { return CountSent(type) > 0; }
};

fs::path MakeTempDir() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path dir = fs::temp_directory_path() / ("fastclone_ce_" + std::to_string(stamp));
    fs::create_directories(dir);
    return dir;
}

void WriteFile(const fs::path& path, const std::string& content) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << content;
}

Frame ManifestEntryFrame(const std::string& rel, uint64_t size, bool isDir = false) {
    FileEntry e;
    e.relativePath = rel;
    e.isDirectory = isDir;
    e.fileSize = size;
    e.mtimeNs = 1700000000000000000LL;
    return Frame{MsgType::ManifestEntry, 0, EncodeManifestEntry(e)};
}

Frame ManifestEndFrame() { return Frame{MsgType::ManifestEnd, 0, {}}; }

Frame HashResponseFrame(const std::string& rel, const Hash256& hash) {
    return Frame{MsgType::HashResponse, 0, EncodeHashResponse(rel, hash)};
}

CheckOptions BaseOptions(const fs::path& target, Mode mode) {
    CheckOptions o;
    o.target = target.string();
    o.mode = mode;
    o.checkers = 8;
    return o;
}

void AssertNoTransferFrames(const MockChannel& mock) {
    Expect(!mock.SentAny(MsgType::FileOpen), "engine must never send FileOpen (AC-36)");
    Expect(!mock.SentAny(MsgType::FileBatchOpen), "engine must never send FileBatchOpen (AC-36)");
    Expect(!mock.SentAny(MsgType::BlockSigRequest), "engine must never send BlockSigRequest (AC-36)");
    Expect(!mock.SentAny(MsgType::DeltaRangeOpen), "engine must never send DeltaRangeOpen (AC-36)");
}

// size-only mode: same/diff/missing/extra counts, and sends no HashRequest (AC-27/28/29).
void TestSizeOnlyCountsNoHash() {
    const fs::path dir = MakeTempDir();
    WriteFile(dir / "a.txt", "aaa");      // 3 bytes -> same as manifest
    WriteFile(dir / "b.txt", "bbbbb");    // 5 bytes -> manifest says 999 -> Diff
    WriteFile(dir / "extra.txt", "x");    // not in manifest -> Extra

    MockChannel mock;
    mock.inbound.push_back(ManifestEntryFrame("a.txt", 3));
    mock.inbound.push_back(ManifestEntryFrame("b.txt", 999));
    mock.inbound.push_back(ManifestEntryFrame("missing.txt", 10));
    mock.inbound.push_back(ManifestEndFrame());

    CheckOptions o = BaseOptions(dir, Mode::SizeOnly);
    FrameChannel ch = mock.Make();
    std::atomic<bool> interrupted{false};
    const EngineOutcome outcome = RunCheck(o, ch, interrupted);

    Expect(outcome.result.counters.same == 1, "size-only same=1");
    Expect(outcome.result.counters.diff == 1, "size-only diff=1");
    Expect(outcome.result.counters.missing == 1, "size-only missing=1");
    Expect(outcome.result.counters.extraLocal == 1, "size-only extra_local=1");
    Expect(outcome.result.counters.TotalCompared() == 3, "total_compared=3 (AC-29 shape)");
    Expect(outcome.exit == kDiffFound, "differences present -> exit 1 (AC-20)");
    Expect(mock.CountSent(MsgType::HashRequest) == 0, "size-only sends NO HashRequest (AC-27)");
    Expect(mock.SentAny(MsgType::ManifestRequest), "engine sent ManifestRequest");
    Expect(mock.SentAny(MsgType::SyncDone), "clean finish sends SyncDone (AC-35)");
    AssertNoTransferFrames(mock);

    // B-01/B-02 regression: EXTRA's remoteSize must be null, localSize=the actual local file size (FR-24/AC-31).
    const DiffEntry* extra = FindEntry(outcome.result, "extra.txt");
    Expect(extra != nullptr, "extra.txt entry present");
    Expect(!extra->remoteSize.has_value(), "EXTRA remoteSize must be null (B-01)");
    Expect(extra->localSize.has_value() && *extra->localSize == 1, "EXTRA localSize = local file size (1)");
    // MISSING's remoteSize must be the actual manifest size, localSize null (FR-24/AC-31).
    const DiffEntry* missing = FindEntry(outcome.result, "missing.txt");
    Expect(missing != nullptr, "missing.txt entry present");
    Expect(missing->remoteSize.has_value() && *missing->remoteSize == 10,
           "MISSING remoteSize = manifest size (10) (B-02)");
    Expect(!missing->localSize.has_value(), "MISSING localSize must be null");
    // DIFF (size) both sides have a size: local=5, remote=999.
    const DiffEntry* diff = FindEntry(outcome.result, "b.txt");
    Expect(diff != nullptr, "b.txt DIFF entry present");
    Expect(diff->localSize.has_value() && *diff->localSize == 5, "DIFF localSize = 5");
    Expect(diff->remoteSize.has_value() && *diff->remoteSize == 999, "DIFF remoteSize = 999");

    std::error_code ec;
    fs::remove_all(dir, ec);
}

// strict mode: same size triggers HashRequest; hash equal -> Same, different -> Diff (AC-25).
void TestStrictHashSameAndDiff() {
    const fs::path dir = MakeTempDir();
    WriteFile(dir / "f1.txt", "hello");
    WriteFile(dir / "f2.txt", "world");
    const Hash256 h1 = ComputeFileHash(dir / "f1.txt");
    Hash256 wrong{};
    wrong[0] = 0xAB;  // deliberately wrong remote hash -> Diff

    MockChannel mock;
    mock.inbound.push_back(ManifestEntryFrame("f1.txt", 5));
    mock.inbound.push_back(ManifestEntryFrame("f2.txt", 5));
    mock.inbound.push_back(ManifestEndFrame());
    mock.inbound.push_back(HashResponseFrame("f1.txt", h1));      // matches -> Same
    mock.inbound.push_back(HashResponseFrame("f2.txt", wrong));   // mismatch -> Diff

    CheckOptions o = BaseOptions(dir, Mode::Strict);
    FrameChannel ch = mock.Make();
    std::atomic<bool> interrupted{false};
    const EngineOutcome outcome = RunCheck(o, ch, interrupted);

    Expect(outcome.result.counters.same == 1, "strict hash-equal -> same=1");
    Expect(outcome.result.counters.diff == 1, "strict hash-diff -> diff=1");
    Expect(mock.CountSent(MsgType::HashRequest) == 2, "strict same-size -> 2 HashRequests (AC-25)");
    Expect(outcome.exit == kDiffFound, "one diff -> exit 1");
    AssertNoTransferFrames(mock);

    std::error_code ec;
    fs::remove_all(dir, ec);
}

// Both sides identical -> exit code 0; and sends no HashRequest (size-only).
void TestIdenticalExitZero() {
    const fs::path dir = MakeTempDir();
    WriteFile(dir / "a.txt", "aaa");

    MockChannel mock;
    mock.inbound.push_back(ManifestEntryFrame("a.txt", 3));
    mock.inbound.push_back(ManifestEndFrame());

    CheckOptions o = BaseOptions(dir, Mode::SizeOnly);
    FrameChannel ch = mock.Make();
    std::atomic<bool> interrupted{false};
    const EngineOutcome outcome = RunCheck(o, ch, interrupted);

    Expect(outcome.result.counters.same == 1, "identical -> same=1");
    Expect(outcome.result.counters.diff == 0 && outcome.result.counters.missing == 0 &&
               outcome.result.counters.extraLocal == 0,
           "identical -> no diff/missing/extra");
    Expect(outcome.exit == kIdentical, "identical -> exit 0 (AC-19)");
    Expect(!outcome.result.partial, "identical clean run not partial");

    std::error_code ec;
    fs::remove_all(dir, ec);
}

// Ctrl+C: preset interrupted -> partial + exit code 4 (AC-21).
void TestInterruptedPartial() {
    const fs::path dir = MakeTempDir();
    MockChannel mock;
    mock.inbound.push_back(ManifestEntryFrame("a.txt", 3));
    mock.inbound.push_back(ManifestEndFrame());

    CheckOptions o = BaseOptions(dir, Mode::SizeOnly);
    FrameChannel ch = mock.Make();
    std::atomic<bool> interrupted{true};  // interrupted right at start
    const EngineOutcome outcome = RunCheck(o, ch, interrupted);

    Expect(outcome.exit == kInterrupted, "interrupted -> exit 4");
    Expect(outcome.result.partial, "interrupted -> partial=true");

    std::error_code ec;
    fs::remove_all(dir, ec);
}

// Disconnect: recv throws -> partial + exit code 2 (AC-48).
void TestDisconnectExitTwo() {
    const fs::path dir = MakeTempDir();
    MockChannel mock;
    mock.recvThrowsImmediately = true;

    CheckOptions o = BaseOptions(dir, Mode::SizeOnly);
    FrameChannel ch = mock.Make();
    std::atomic<bool> interrupted{false};
    const EngineOutcome outcome = RunCheck(o, ch, interrupted);

    Expect(outcome.exit == kConnFailed, "disconnect -> exit 2");
    Expect(outcome.result.partial, "disconnect -> partial=true");

    std::error_code ec;
    fs::remove_all(dir, ec);
}

// Empty-directory three states (boundary condition).
void TestEmptyDirectories() {
    // (1) manifest empty + local empty -> all 0, exit code 0.
    {
        const fs::path dir = MakeTempDir();
        MockChannel mock;
        mock.inbound.push_back(ManifestEndFrame());
        CheckOptions o = BaseOptions(dir, Mode::Fast);
        FrameChannel ch = mock.Make();
        std::atomic<bool> interrupted{false};
        const EngineOutcome outcome = RunCheck(o, ch, interrupted);
        Expect(outcome.result.counters.same == 0 && outcome.result.counters.diff == 0 &&
                   outcome.result.counters.missing == 0 && outcome.result.counters.extraLocal == 0,
               "both empty -> all zero");
        Expect(outcome.exit == kIdentical, "both empty -> exit 0");
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    // (2) manifest empty + local has files -> all EXTRA, exit code 1.
    {
        const fs::path dir = MakeTempDir();
        WriteFile(dir / "only_local.txt", "z");
        MockChannel mock;
        mock.inbound.push_back(ManifestEndFrame());
        CheckOptions o = BaseOptions(dir, Mode::Fast);
        FrameChannel ch = mock.Make();
        std::atomic<bool> interrupted{false};
        const EngineOutcome outcome = RunCheck(o, ch, interrupted);
        Expect(outcome.result.counters.extraLocal == 1, "server empty + local file -> extra=1");
        Expect(outcome.exit == kDiffFound, "extra present -> exit 1");
        const DiffEntry* extra = FindEntry(outcome.result, "only_local.txt");
        Expect(extra != nullptr, "only_local.txt EXTRA entry present");
        Expect(!extra->remoteSize.has_value(), "EXTRA remoteSize null (B-01)");
        Expect(extra->localSize.has_value() && *extra->localSize == 1, "EXTRA localSize = 1");
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    // (3) manifest has files + local empty -> all MISSING, exit code 1.
    {
        const fs::path dir = MakeTempDir();
        MockChannel mock;
        mock.inbound.push_back(ManifestEntryFrame("srv_only.txt", 4));
        mock.inbound.push_back(ManifestEndFrame());
        CheckOptions o = BaseOptions(dir, Mode::Fast);
        FrameChannel ch = mock.Make();
        std::atomic<bool> interrupted{false};
        const EngineOutcome outcome = RunCheck(o, ch, interrupted);
        Expect(outcome.result.counters.missing == 1, "local empty -> missing=1");
        Expect(outcome.exit == kDiffFound, "missing present -> exit 1");
        const DiffEntry* missing = FindEntry(outcome.result, "srv_only.txt");
        Expect(missing != nullptr, "srv_only.txt MISSING entry present");
        Expect(missing->remoteSize.has_value() && *missing->remoteSize == 4,
               "MISSING remoteSize = manifest size (4) (B-02)");
        Expect(!missing->localSize.has_value(), "MISSING localSize null");
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
}

// G1: Fast mode same size + mtime differs -> needHash -> HashRequest -> hash same/different yields Same/Diff respectively.
// Fills the engine-layer Fast-hash path gap (the decision layer is already covered by test_compare_phase; here we cover orchestration + wrap-up).
// manifest uses a fixed mtimeNs=2023 (kFixedManifestMtime), the local file mtime is the current moment (2026),
// so after normalization the two sides differ far more than the 2ms tolerance, definitely triggering needHash.
void TestFastHashSameAndDiff() {
    const fs::path dir = MakeTempDir();
    WriteFile(dir / "f1.txt", "hello");
    WriteFile(dir / "f2.txt", "world");
    const Hash256 h1 = ComputeFileHash(dir / "f1.txt");
    Hash256 wrong{};
    wrong[0] = 0xCD;

    MockChannel mock;
    mock.inbound.push_back(ManifestEntryFrame("f1.txt", 5));  // mtime=2023 vs local now -> needHash
    mock.inbound.push_back(ManifestEntryFrame("f2.txt", 5));
    mock.inbound.push_back(ManifestEndFrame());
    mock.inbound.push_back(HashResponseFrame("f1.txt", h1));      // matches -> Same
    mock.inbound.push_back(HashResponseFrame("f2.txt", wrong));   // mismatch -> Diff

    CheckOptions o = BaseOptions(dir, Mode::Fast);
    FrameChannel ch = mock.Make();
    std::atomic<bool> interrupted{false};
    const EngineOutcome outcome = RunCheck(o, ch, interrupted);

    Expect(outcome.result.counters.same == 1, "fast hash-equal -> same=1");
    Expect(outcome.result.counters.diff == 1, "fast hash-diff -> diff=1");
    Expect(mock.CountSent(MsgType::HashRequest) == 2, "fast same-size mtime-diff -> 2 HashRequests (G1)");
    Expect(outcome.exit == kDiffFound, "one diff -> exit 1");
    AssertNoTransferFrames(mock);

    // hashCompared flag: entries that went through hash in Fast must have hash_compared=true (FR-24/AC-22).
    const DiffEntry* diff = FindEntry(outcome.result, "f2.txt");
    Expect(diff != nullptr, "f2.txt DIFF entry present");
    Expect(diff->hashCompared, "fast-hash DIFF entry hash_compared=true (G1)");

    std::error_code ec;
    fs::remove_all(dir, ec);
}

// G3: --checkers=1 serializes in-flight HashRequests, still completing all comparisons correctly without deadlock.
// A black box cannot directly assert "in-flight<=1", but checkers=1 + all multi-file hashes completing proves the serial path has no deadlock/no lost responses.
void TestCheckersOneSerializesHash() {
    const fs::path dir = MakeTempDir();
    // Write 3 files of the same size so all three trigger needHash (Fast: mtime differs from the manifest's 2023).
    WriteFile(dir / "a.txt", "aaa");
    WriteFile(dir / "b.txt", "bbb");
    WriteFile(dir / "c.txt", "ccc");
    const Hash256 ha = ComputeFileHash(dir / "a.txt");
    const Hash256 hb = ComputeFileHash(dir / "b.txt");
    const Hash256 hc = ComputeFileHash(dir / "c.txt");

    MockChannel mock;
    mock.inbound.push_back(ManifestEntryFrame("a.txt", 3));
    mock.inbound.push_back(ManifestEntryFrame("b.txt", 3));
    mock.inbound.push_back(ManifestEntryFrame("c.txt", 3));
    mock.inbound.push_back(ManifestEndFrame());
    // Response order matches request order (with checkers=1 at most one is in flight, responses return in order).
    mock.inbound.push_back(HashResponseFrame("a.txt", ha));
    mock.inbound.push_back(HashResponseFrame("b.txt", hb));
    mock.inbound.push_back(HashResponseFrame("c.txt", hc));

    CheckOptions o = BaseOptions(dir, Mode::Fast);
    o.checkers = 1;
    FrameChannel ch = mock.Make();
    std::atomic<bool> interrupted{false};
    const EngineOutcome outcome = RunCheck(o, ch, interrupted);

    Expect(outcome.result.counters.same == 3, "checkers=1 all hash-equal -> same=3");
    Expect(outcome.result.counters.diff == 0, "checkers=1 no diff");
    Expect(mock.CountSent(MsgType::HashRequest) == 3, "checkers=1 sends 3 HashRequests (G3)");
    Expect(outcome.exit == kIdentical, "checkers=1 all same -> exit 0 (no deadlock)");
    AssertNoTransferFrames(mock);

    std::error_code ec;
    fs::remove_all(dir, ec);
}

// G4: the engine-layer filter is already baked into the entries vector (the record lambda pushes only after filtering by o.filter).
// With --filter DIFF, MISSING/EXTRA are still counted but do not appear in entries.
void TestEngineFilterShapesEntries() {
    const fs::path dir = MakeTempDir();
    WriteFile(dir / "same.txt", "aaa");
    WriteFile(dir / "diff.txt", "bbbbb");     // manifest 999 -> size diff -> DIFF
    WriteFile(dir / "extra.txt", "x");        // not in manifest -> EXTRA

    MockChannel mock;
    mock.inbound.push_back(ManifestEntryFrame("same.txt", 3));
    mock.inbound.push_back(ManifestEntryFrame("diff.txt", 999));
    mock.inbound.push_back(ManifestEntryFrame("missing.txt", 10));
    mock.inbound.push_back(ManifestEndFrame());

    CheckOptions o = BaseOptions(dir, Mode::SizeOnly);
    o.filter = FilterSet{/*diff=*/true, /*missing=*/false, /*extra=*/false, /*same=*/false};
    FrameChannel ch = mock.Make();
    std::atomic<bool> interrupted{false};
    const EngineOutcome outcome = RunCheck(o, ch, interrupted);

    // Counts are unaffected by the filter (the summary is always full).
    Expect(outcome.result.counters.diff == 1, "filter DIFF still counts diff=1");
    Expect(outcome.result.counters.missing == 1, "filter DIFF still counts missing=1");
    Expect(outcome.result.counters.extraLocal == 1, "filter DIFF still counts extra=1");
    // entries contain only DIFF (G4: engine-layer filtering, not just the render layer).
    Expect(FindEntry(outcome.result, "diff.txt") != nullptr, "DIFF entry present in entries (G4)");
    Expect(FindEntry(outcome.result, "missing.txt") == nullptr, "MISSING filtered out of entries (G4)");
    Expect(FindEntry(outcome.result, "extra.txt") == nullptr, "EXTRA filtered out of entries (G4)");
    Expect(FindEntry(outcome.result, "same.txt") == nullptr, "SAME filtered out of entries (G4)");
    Expect(outcome.result.entries.size() == 1, "entries size == 1 (only DIFF)");

    std::error_code ec;
    fs::remove_all(dir, ec);
}

}  // namespace

void RunCheckEngineTests() {
    TestSizeOnlyCountsNoHash();
    TestStrictHashSameAndDiff();
    TestFastHashSameAndDiff();
    TestCheckersOneSerializesHash();
    TestEngineFilterShapesEntries();
    TestIdenticalExitZero();
    TestInterruptedPartial();
    TestDisconnectExitTwo();
    TestEmptyDirectories();
}
