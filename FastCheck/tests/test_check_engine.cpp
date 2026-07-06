#include "check_engine.h"

#include "disk_io_driver.h"
#include "file_index.h"
#include "protocol.h"
#include "protocol_codec.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>  // FSCTL_SET_SPARSE (Gap B sparse-file coverage)
#endif

namespace fs = std::filesystem;
using namespace fc;
using namespace fc::check;

namespace {

constexpr size_t kSmallFileDirectThreshold = 256u * 1024u;

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

    // Optional request-reactive hash responder. When useResponder is set, send() appends the mapped
    // HashResponse to inbound on each HashRequest -- modelling the real server, which emits a
    // HashResponse only in reply to a HashRequest, so on-the-wire response order always equals request
    // order regardless of the client's internal compare ordering (which the parallel compare pipeline
    // no longer keeps in manifest order). Tests using the responder must NOT also pre-push HashResponse
    // frames into inbound.
    bool useResponder = false;
    std::unordered_map<std::string, Hash256> hashResponder;

    FrameChannel Make() {
        FrameChannel ch;
        ch.send = [this](const Frame& frame) {
            sent.push_back(frame);
            if (useResponder && frame.type == MsgType::HashRequest) {
                const std::string rel = DecodeHashRequest(frame.payload);
                const auto it = hashResponder.find(rel);
                if (it != hashResponder.end()) {
                    inbound.push_back(Frame{MsgType::HashResponse, 0, EncodeHashResponse(rel, it->second)});
                }
            }
        };
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

std::string MakeDeterministicContent(size_t n, uint32_t salt) {
    std::string content;
    content.resize(n);
    for (size_t i = 0; i < n; ++i) {
        content[i] = static_cast<char>((i * 197u + salt * 31u + 5u) & 0xFF);
    }
    return content;
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

class ScriptedHashBackend final : public fc::io::PlatformIoBackend {
public:
    enum class ReadMode : uint8_t {
        Normal,
        Error,
        Cancelled,
        EarlyEof,
        ShortRead,
    };

    ScriptedHashBackend(fc::io::AlignInfo align, ReadMode readMode)
        : align_(align), readMode_(readMode) {}

    fc::io::BackendKind kind() const override { return fc::io::BackendKind::Mock; }
    fc::io::AlignInfo queryAlign(const std::string&) override { return align_; }

    uint64_t openFile(const std::string& path, fc::io::OpKind mode, bool, uint64_t) override {
        std::lock_guard<std::mutex> lk(mu_);
        const uint64_t id = nextId_++;
        openedFileId_ = id;
        if (mode == fc::io::OpKind::Read) {
            files_[id] = ReadWholeFile(path);
        } else {
            files_[id].clear();
        }
        return id;
    }

    void closeFile(uint64_t fileId) override {
        std::lock_guard<std::mutex> lk(mu_);
        ++closeCalls_;
        lastClosedFileId_ = fileId;
        files_.erase(fileId);
    }

    bool submit(fc::io::IoRequest&& req) override {
        std::lock_guard<std::mutex> lk(mu_);
        submitted_.push_back(req);
        fc::io::IoCompletion comp;
        comp.kind = req.kind;
        comp.fileId = req.fileId;
        comp.offset = req.offset;
        comp.requested = req.length;
        comp.userTag = req.userTag;
        BuildReadCompletion(req, comp);
        completions_.push_back(std::move(comp));
        cv_.notify_one();
        return true;
    }

    size_t reap(std::vector<fc::io::IoCompletion>& out, size_t max, int timeoutMs) override {
        std::unique_lock<std::mutex> lk(mu_);
        if (completions_.empty() && !stopped_ && timeoutMs != 0) {
            cv_.wait_for(lk, std::chrono::milliseconds(timeoutMs < 0 ? 50 : timeoutMs),
                         [this] { return stopped_ || !completions_.empty(); });
        }
        size_t n = 0;
        while (!completions_.empty() && n < max) {
            out.push_back(std::move(completions_.front()));
            completions_.pop_front();
            ++n;
        }
        return n;
    }

    void shutdown() override {
        std::lock_guard<std::mutex> lk(mu_);
        stopped_ = true;
        completions_.clear();
        cv_.notify_all();
    }

    fc::io::BackendCounters counters() const override { return {}; }

    size_t readSubmitCount() const {
        std::lock_guard<std::mutex> lk(mu_);
        size_t count = 0;
        for (const auto& req : submitted_) {
            if (req.kind == fc::io::OpKind::Read) {
                ++count;
            }
        }
        return count;
    }

    std::vector<fc::io::IoRequest> submittedRequests() const {
        std::lock_guard<std::mutex> lk(mu_);
        return submitted_;
    }

    uint64_t openedFileId() const {
        std::lock_guard<std::mutex> lk(mu_);
        return openedFileId_;
    }

    size_t closeCount() const {
        std::lock_guard<std::mutex> lk(mu_);
        return closeCalls_;
    }

    uint64_t lastClosedFileId() const {
        std::lock_guard<std::mutex> lk(mu_);
        return lastClosedFileId_;
    }

private:
    static std::vector<uint8_t> ReadWholeFile(const std::string& path) {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            throw std::runtime_error("ScriptedHashBackend: failed to open file");
        }
        in.seekg(0, std::ios::end);
        const std::streamoff size = in.tellg();
        in.seekg(0, std::ios::beg);
        std::vector<uint8_t> data;
        if (size > 0) {
            data.resize(static_cast<size_t>(size));
            in.read(reinterpret_cast<char*>(data.data()), size);
        }
        return data;
    }

    void BuildReadCompletion(const fc::io::IoRequest& req, fc::io::IoCompletion& comp) {
        if (req.kind != fc::io::OpKind::Read) {
            comp.status = fc::io::IoStatus::Ok;
            comp.transferred = req.length;
            return;
        }
        auto it = files_.find(req.fileId);
        if (it == files_.end()) {
            comp.status = fc::io::IoStatus::Error;
            comp.transferred = 0;
            return;
        }
        const std::vector<uint8_t>& data = it->second;
        const size_t off = static_cast<size_t>(req.offset);
        const size_t available = off < data.size() ? (data.size() - off) : 0;
        const size_t fullTransfer = std::min<size_t>(req.length, available);
        const size_t shortTransfer =
            (fullTransfer > 0) ? std::max<size_t>(size_t{1}, fullTransfer / 2) : size_t{0};

        if (readMode_ == ReadMode::Error) {
            comp.status = fc::io::IoStatus::Error;
            comp.transferred = 0;
            return;
        }
        if (readMode_ == ReadMode::Cancelled) {
            comp.status = fc::io::IoStatus::Cancelled;
            comp.transferred = 0;
            return;
        }
        if (readMode_ == ReadMode::EarlyEof) {
            comp.status = fc::io::IoStatus::Eof;
            comp.transferred = static_cast<uint32_t>(shortTransfer);
            comp.data.assign(shortTransfer, 0);
            if (shortTransfer > 0 && off < data.size()) {
                std::copy_n(data.begin() + static_cast<std::ptrdiff_t>(off), shortTransfer,
                            comp.data.begin());
            }
            return;
        }
        if (readMode_ == ReadMode::ShortRead) {
            comp.status = fc::io::IoStatus::Ok;
            comp.transferred = static_cast<uint32_t>(shortTransfer);
            comp.data.assign(shortTransfer, 0);
            if (shortTransfer > 0 && off < data.size()) {
                std::copy_n(data.begin() + static_cast<std::ptrdiff_t>(off), shortTransfer,
                            comp.data.begin());
            }
            return;
        }

        comp.status = (fullTransfer < req.length) ? fc::io::IoStatus::Eof : fc::io::IoStatus::Ok;
        comp.transferred = static_cast<uint32_t>(fullTransfer);
        comp.data.assign(req.length, 0);
        if (fullTransfer > 0 && off < data.size()) {
            std::copy_n(data.begin() + static_cast<std::ptrdiff_t>(off), fullTransfer,
                        comp.data.begin());
        }
    }

    fc::io::AlignInfo align_;
    ReadMode readMode_ = ReadMode::Normal;
    mutable std::mutex mu_;
    std::condition_variable cv_;
    bool stopped_ = false;
    uint64_t nextId_ = 1;
    uint64_t openedFileId_ = 0;
    uint64_t lastClosedFileId_ = 0;
    size_t closeCalls_ = 0;
    std::unordered_map<uint64_t, std::vector<uint8_t>> files_;
    std::vector<fc::io::IoRequest> submitted_;
    std::deque<fc::io::IoCompletion> completions_;
};

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
    // checkers=1 keeps at most one HashRequest in flight, but the parallel compare pipeline may emit
    // requests in a non-manifest order, so use the request-reactive responder (models the real server:
    // one HashResponse per HashRequest, in request order) instead of a fixed pre-scripted response order.
    mock.useResponder = true;
    mock.hashResponder = {{"a.txt", ha}, {"b.txt", hb}, {"c.txt", hc}};

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

// V-06 (AC-06): a HashResponse that arrives before the local hash completes must be stashed as
// pending and the file classified correctly once the local hash finishes, without blocking the recv
// loop. All inbound frames (incl. the HashResponse) are delivered instantly from the in-memory mock,
// while the local hash reads a multi-MiB file through the driver, so the response reliably lands
// before the worker completes -> exercises the serverHashReady pending path. Correctness holds on
// either ordering, so the assertion (Same) is not timing-sensitive.
void TestHashResponseBeforeLocalHash() {
    const fs::path dir = MakeTempDir();
    // ~8 MiB so the driver read + hash is meaningfully slower than the instant mock recv.
    std::string big;
    big.resize(8u * 1024u * 1024u + 7u);
    for (size_t i = 0; i < big.size(); ++i) {
        big[i] = static_cast<char>((i * 131u + 7u) & 0xFF);
    }
    WriteFile(dir / "big.bin", big);
    const Hash256 h = ComputeFileHash(dir / "big.bin");

    MockChannel mock;
    mock.inbound.push_back(ManifestEntryFrame("big.bin", big.size()));
    mock.inbound.push_back(ManifestEndFrame());
    mock.inbound.push_back(HashResponseFrame("big.bin", h));  // correct hash -> Same

    CheckOptions o = BaseOptions(dir, Mode::Strict);
    FrameChannel ch = mock.Make();
    std::atomic<bool> interrupted{false};
    const EngineOutcome outcome = RunCheck(o, ch, interrupted);

    Expect(outcome.result.counters.same == 1, "response-before-local-hash still classifies Same (AC-06)");
    Expect(outcome.result.counters.diff == 0, "no diff");
    Expect(outcome.exit == kIdentical, "identical -> exit 0, no deadlock (AC-06)");
    Expect(mock.CountSent(MsgType::HashRequest) == 1, "one HashRequest sent");
    AssertNoTransferFrames(mock);

    std::error_code ec;
    fs::remove_all(dir, ec);
}

// V-07 (AC-07): --hash-workers 1 (single local hash worker) completes a multi-file strict run with
// all workers joined and no deadlock.
void TestHashWorkersOneSerial() {
    const fs::path dir = MakeTempDir();
    WriteFile(dir / "a.txt", "alpha");
    WriteFile(dir / "b.txt", "bravo");
    WriteFile(dir / "c.txt", "charl");
    const Hash256 ha = ComputeFileHash(dir / "a.txt");
    const Hash256 hb = ComputeFileHash(dir / "b.txt");
    const Hash256 hc = ComputeFileHash(dir / "c.txt");

    MockChannel mock;
    mock.inbound.push_back(ManifestEntryFrame("a.txt", 5));
    mock.inbound.push_back(ManifestEntryFrame("b.txt", 5));
    mock.inbound.push_back(ManifestEntryFrame("c.txt", 5));
    mock.inbound.push_back(ManifestEndFrame());
    mock.inbound.push_back(HashResponseFrame("a.txt", ha));
    mock.inbound.push_back(HashResponseFrame("b.txt", hb));
    mock.inbound.push_back(HashResponseFrame("c.txt", hc));

    CheckOptions o = BaseOptions(dir, Mode::Strict);
    o.hashWorkers = 1;  // single local worker
    FrameChannel ch = mock.Make();
    std::atomic<bool> interrupted{false};
    const EngineOutcome outcome = RunCheck(o, ch, interrupted);

    Expect(outcome.result.counters.same == 3, "--hash-workers 1 classifies all 3 Same (AC-07)");
    Expect(outcome.result.counters.diff == 0, "no diff");
    Expect(outcome.exit == kIdentical, "all same -> exit 0, no deadlock (AC-07)");
    Expect(mock.CountSent(MsgType::HashRequest) == 3, "3 HashRequests sent");

    std::error_code ec;
    fs::remove_all(dir, ec);
}

// V-15 (AC-15): --no-diskio-driver keeps the parallel hash pipeline (non-driver local reads) and
// produces the same classification as the driver path.
void TestNoDiskioDriverDegraded() {
    const fs::path dir = MakeTempDir();
    WriteFile(dir / "f1.txt", "hello");
    WriteFile(dir / "f2.txt", "world");
    const Hash256 h1 = ComputeFileHash(dir / "f1.txt");
    Hash256 wrong{};
    wrong[0] = 0xAB;

    MockChannel mock;
    mock.inbound.push_back(ManifestEntryFrame("f1.txt", 5));
    mock.inbound.push_back(ManifestEntryFrame("f2.txt", 5));
    mock.inbound.push_back(ManifestEndFrame());
    mock.inbound.push_back(HashResponseFrame("f1.txt", h1));
    mock.inbound.push_back(HashResponseFrame("f2.txt", wrong));

    CheckOptions o = BaseOptions(dir, Mode::Strict);
    o.noDiskioDriver = true;  // degraded path: parallel hashing, no driver reads
    FrameChannel ch = mock.Make();
    std::atomic<bool> interrupted{false};
    const EngineOutcome outcome = RunCheck(o, ch, interrupted);

    Expect(outcome.result.counters.same == 1, "--no-diskio-driver same=1 (AC-15)");
    Expect(outcome.result.counters.diff == 1, "--no-diskio-driver diff=1 (AC-15)");
    Expect(outcome.exit == kDiffFound, "one diff -> exit 1");

    std::error_code ec;
    fs::remove_all(dir, ec);
}

// V-14 (AC-02..AC-10 / NFR-02): ComputeFileHashViaDriver must produce the exact same Hash256 as
// ComputeFileHash across empty/small/threshold-boundary and large sizes.
void TestComputeFileHashViaDriverConsistency() {
    const fs::path dir = MakeTempDir();
    const std::vector<size_t> sizes = {
        0u,
        1u,
        64u,
        200u * 1024u,
        kSmallFileDirectThreshold - 1u,
        kSmallFileDirectThreshold,
        kSmallFileDirectThreshold + 1u,
        1u << 20,
        2u << 20,
    };
    fc::io::DiskIoDriver driver(fc::io::IoDriverConfig{});
    for (size_t idx = 0; idx < sizes.size(); ++idx) {
        const size_t n = sizes[idx];
        std::string content;
        content.resize(n);
        for (size_t i = 0; i < n; ++i) {
            content[i] = static_cast<char>((i * 197u + idx * 31u + 5u) & 0xFF);
        }
        const fs::path p = dir / ("blob_" + std::to_string(idx) + ".bin");
        WriteFile(p, content);
        const Hash256 inline_hash = ComputeFileHash(p);
        const Hash256 driver_hash = ComputeFileHashViaDriver(driver, p);
        Expect(HashEquals(inline_hash, driver_hash),
               "ComputeFileHashViaDriver == ComputeFileHash for size " + std::to_string(n) +
                   " (AC-14)");
    }
    std::error_code ec;
    fs::remove_all(dir, ec);
}

// fastcheck-test-coverage-supplement Gap B (FR-07/AC-10): files > 1 MiB whose tail is NOT aligned
// to 1 MiB force the ComputeFileHashViaDriver SequentialReader slow path; its Hash256 must equal
// ComputeFileHash byte-for-byte. Uses a REAL DiskIoDriver (default platform backend).
void TestComputeFileHashViaDriverUnalignedTail() {
    const fs::path dir = MakeTempDir();
    const std::vector<size_t> sizes = {
        (1u << 20) + 1u,
        (1u << 20) + 123u,
        (5u << 20) + 3u,  // 5242883
        5000003u,
    };
    fc::io::DiskIoDriver driver(fc::io::IoDriverConfig{});
    for (size_t idx = 0; idx < sizes.size(); ++idx) {
        const size_t n = sizes[idx];
        const fs::path p = dir / ("unaligned_" + std::to_string(idx) + ".bin");
        WriteFile(p, MakeDeterministicContent(n, 200u + static_cast<uint32_t>(idx)));
        const Hash256 inlineHash = ComputeFileHash(p);
        const Hash256 driverHash = ComputeFileHashViaDriver(driver, p);
        Expect(HashEquals(inlineHash, driverHash),
               "ComputeFileHashViaDriver == ComputeFileHash for unaligned-tail size " +
                   std::to_string(n) + " (AC-10)");
    }
    std::error_code ec;
    fs::remove_all(dir, ec);
}

#if defined(_WIN32)
// Try to create a real Windows sparse file: mark FSCTL_SET_SPARSE, extend the logical size to
// create a zero hole, then write a deterministic tail at an unaligned offset. Returns false (leaving
// the file untouched/removed) if any step fails so the caller can fall back to a plain content file.
bool TryCreateWindowsSparseFile(const fs::path& p, uint64_t logicalSize, uint64_t tailOffset,
                                const std::vector<uint8_t>& tail) {
    HANDLE h = CreateFileW(p.wstring().c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD returned = 0;
    if (!DeviceIoControl(h, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returned, nullptr)) {
        CloseHandle(h);
        return false;
    }
    LARGE_INTEGER end;
    end.QuadPart = static_cast<LONGLONG>(logicalSize);
    if (!SetFilePointerEx(h, end, nullptr, FILE_BEGIN) || !SetEndOfFile(h)) {
        CloseHandle(h);
        return false;
    }
    LARGE_INTEGER at;
    at.QuadPart = static_cast<LONGLONG>(tailOffset);
    if (!SetFilePointerEx(h, at, nullptr, FILE_BEGIN)) {
        CloseHandle(h);
        return false;
    }
    DWORD written = 0;
    if (!::WriteFile(h, tail.data(), static_cast<DWORD>(tail.size()), &written, nullptr) ||
        written != tail.size()) {
        CloseHandle(h);
        return false;
    }
    CloseHandle(h);
    return true;
}
#endif

// fastcheck-test-coverage-supplement Gap B (FR-08/AC-11/AC-12): sparse-file coverage.
//   - Windows sparse creation succeeds -> assert ComputeFileHashViaDriver == ComputeFileHash on the
//     real sparse file (AC-11).
//   - Sparse creation fails / non-Windows -> write an equal-length deterministic content file with
//     the SAME byte layout (zero hole + deterministic tail) and assert the same equivalence (AC-12).
void TestComputeFileHashViaDriverSparseFile() {
    const fs::path dir = MakeTempDir();
    const fs::path p = dir / "sparse.bin";
    const uint64_t logicalSize = (3u << 20) + 777u;   // > 1 MiB, unaligned tail
    const size_t tailLen = 12345u;                    // deterministic non-zero tail
    const uint64_t tailOffset = logicalSize - tailLen;  // zero hole in [0, tailOffset)
    const std::string tailStr = MakeDeterministicContent(tailLen, 88u);
    const std::vector<uint8_t> tail(tailStr.begin(), tailStr.end());

    bool sparseReady = false;
#if defined(_WIN32)
    sparseReady = TryCreateWindowsSparseFile(p, logicalSize, tailOffset, tail);
#endif
    if (!sparseReady) {
        // Fallback: equal-length deterministic content file, same byte layout as the sparse case.
        std::vector<uint8_t> content(static_cast<size_t>(logicalSize), 0);
        std::copy(tail.begin(), tail.end(),
                  content.begin() + static_cast<std::ptrdiff_t>(tailOffset));
        std::ofstream os(p, std::ios::binary | std::ios::trunc);
        os.write(reinterpret_cast<const char*>(content.data()),
                 static_cast<std::streamsize>(content.size()));
    }

    std::error_code sizeEc;
    Expect(static_cast<uint64_t>(fs::file_size(p, sizeEc)) == logicalSize,
           "sparse/fallback file has the expected logical size");

    fc::io::DiskIoDriver driver(fc::io::IoDriverConfig{});
    const Hash256 inlineHash = ComputeFileHash(p);
    const Hash256 driverHash = ComputeFileHashViaDriver(driver, p);
    Expect(HashEquals(inlineHash, driverHash),
           sparseReady ? "sparse file ComputeFileHashViaDriver == ComputeFileHash (AC-11)"
                       : "equal-length content fallback ViaDriver == ComputeFileHash (AC-12)");

    std::error_code ec;
    fs::remove_all(dir, ec);
}

void TestComputeFileHashViaDriverSmallFileSingleRead() {
    const fs::path dir = MakeTempDir();
    const size_t fileSize = 200u * 1024u + 73u;
    const fs::path p = dir / "small_single_read.bin";
    WriteFile(p, MakeDeterministicContent(fileSize, 41u));

    fc::io::IoDriverConfig cfg;
    auto backend = std::make_unique<ScriptedHashBackend>(
        fc::io::MakeAlignInfo(4096u, 4096u), ScriptedHashBackend::ReadMode::Normal);
    ScriptedHashBackend* raw = backend.get();
    fc::io::DiskIoDriver driver(cfg, std::move(backend));

    const Hash256 expected = ComputeFileHash(p);
    const Hash256 actual = ComputeFileHashViaDriver(driver, p);
    Expect(HashEquals(expected, actual), "small-file single-read hash matches ComputeFileHash (AC-12)");

    const std::vector<fc::io::IoRequest> reqs = raw->submittedRequests();
    size_t readReqs = 0;
    for (const auto& req : reqs) {
        if (req.kind == fc::io::OpKind::Read) {
            ++readReqs;
        }
    }
    Expect(readReqs == 1, "small-file path submits exactly one read request (AC-12)");
    Expect(reqs.size() == 1 && reqs[0].fileId == raw->openedFileId(),
           "small-file read request fileId matches openFile result (AC-12)");
    Expect(raw->closeCount() == 1 && raw->lastClosedFileId() == raw->openedFileId(),
           "small-file path closes opened file exactly once");

    std::error_code ec;
    fs::remove_all(dir, ec);
}

void TestComputeFileHashViaDriverAlignedHash() {
    const fs::path dir = MakeTempDir();
    const size_t fileSize = 200u * 1024u + 19u;
    const fs::path p = dir / "aligned_hash.bin";
    WriteFile(p, MakeDeterministicContent(fileSize, 53u));

    const fc::io::AlignInfo align = fc::io::MakeAlignInfo(16u * 1024u, 512u);
    fc::io::IoDriverConfig cfg;
    auto backend = std::make_unique<ScriptedHashBackend>(align, ScriptedHashBackend::ReadMode::Normal);
    ScriptedHashBackend* raw = backend.get();
    fc::io::DiskIoDriver driver(cfg, std::move(backend));

    const Hash256 expected = ComputeFileHash(p);
    const Hash256 actual = ComputeFileHashViaDriver(driver, p);
    Expect(HashEquals(expected, actual), "non-standard AlignInfo keeps hash bit-identical (AC-13)");

    const std::vector<fc::io::IoRequest> reqs = raw->submittedRequests();
    Expect(reqs.size() == 1, "aligned-hash path still submits one read (AC-13)");
    const auto& req = reqs.front();
    const uint64_t expectedLength = fc::io::AlignUp(static_cast<uint64_t>(fileSize), align.ioGranularity);
    Expect(fc::io::IsAligned(req.offset, align.ioGranularity),
           "request offset aligns to mocked ioGranularity (AC-13)");
    Expect(fc::io::IsAligned(req.length, align.ioGranularity),
           "request length aligns to mocked ioGranularity (AC-13)");
    Expect(req.length == expectedLength,
           "request length uses AlignUp(fileSize, ioGranularity) under mocked AlignInfo (AC-13)");

    std::error_code ec;
    fs::remove_all(dir, ec);
}

void TestComputeFileHashViaDriverSmallReadFailures() {
    const fs::path dir = MakeTempDir();
    const size_t fileSize = 200u * 1024u + 33u;
    const fs::path p = dir / "small_read_failures.bin";
    WriteFile(p, MakeDeterministicContent(fileSize, 67u));

    struct FailureCase {
        const char* name;
        ScriptedHashBackend::ReadMode mode;
    };
    const FailureCase cases[] = {
        {"Error", ScriptedHashBackend::ReadMode::Error},
        {"Cancelled", ScriptedHashBackend::ReadMode::Cancelled},
        {"EarlyEof", ScriptedHashBackend::ReadMode::EarlyEof},
        {"ShortRead", ScriptedHashBackend::ReadMode::ShortRead},
    };

    for (const auto& failureCase : cases) {
        fc::io::IoDriverConfig cfg;
        auto backend = std::make_unique<ScriptedHashBackend>(
            fc::io::MakeAlignInfo(4096u, 4096u), failureCase.mode);
        ScriptedHashBackend* raw = backend.get();
        fc::io::DiskIoDriver driver(cfg, std::move(backend));

        bool threw = false;
        try {
            (void)ComputeFileHashViaDriver(driver, p);
        } catch (const std::exception&) {
            threw = true;
        }
        Expect(threw, std::string("small-file read failure must throw (AC-14): ") + failureCase.name);
        Expect(raw->readSubmitCount() == 1,
               std::string("failure case still submits exactly one read (AC-14): ") + failureCase.name);
        Expect(raw->closeCount() == 1 && raw->lastClosedFileId() == raw->openedFileId(),
               std::string("failure case closes opened file once (AC-14): ") + failureCase.name);
    }

    std::error_code ec;
    fs::remove_all(dir, ec);
}

// fastcheck-perf-tune Change 0 (AC-03/AC-04): the hash-worker pool cap policy. auto (--hash-workers 0)
// pins the pool to the hardware thread count (no 4x-core overshoot); explicit --hash-workers N keeps
// the historical max(N, 4*hardwareThreads) headroom.
void TestResolveMaxHashWorkers() {
    // auto mode (hashWorkers == 0): pinned to hardwareThreads for hw=1/2/8 (AC-03), incl. hw=1 boundary.
    Expect(ResolveMaxHashWorkers(0, 1) == 1, "auto hash-workers pins to 1 core (AC-03)");
    Expect(ResolveMaxHashWorkers(0, 2) == 2, "auto hash-workers pins to 2 cores (AC-03)");
    Expect(ResolveMaxHashWorkers(0, 8) == 8, "auto hash-workers pins to 8 cores (AC-03)");
    // auto never overshoots to 4x cores.
    Expect(ResolveMaxHashWorkers(0, 20) == 20, "auto hash-workers pins to 20 cores, no 4x overshoot (AC-03)");

    // Explicit --hash-workers N: keeps max(N, 4*hardwareThreads) headroom (AC-04).
    Expect(ResolveMaxHashWorkers(1, 8) == 32, "explicit 1 -> max(1, 4*8)=32 (AC-04)");
    Expect(ResolveMaxHashWorkers(4, 2) == 8, "explicit 4 -> max(4, 4*2)=8 (AC-04)");
    Expect(ResolveMaxHashWorkers(100, 8) == 100, "explicit 100 -> max(100, 32)=100 (AC-04)");
    // Explicit path on a single-core machine still keeps the 4x floor (boundary).
    Expect(ResolveMaxHashWorkers(1, 1) == 4, "explicit 1 on 1 core -> max(1, 4*1)=4 (AC-04)");
    Expect(ResolveMaxHashWorkers(2, 1) == 4, "explicit 2 on 1 core -> max(2, 4)=4 (AC-04)");
}

// V-09 (AC-10): local worker cap AIMD pure rule -- growing backlog + no read failures increases the
// cap; a read-fail signal halves it.
void TestLocalWorkerCapAimd() {
    // Additive increase: backlog present, all active workers busy (inFlight>=cap), headroom, no fails.
    Expect(NextLocalWorkerCap(/*cap=*/4, /*max=*/32, /*queue=*/10, /*inFlight=*/4, /*failDelta=*/0) == 5,
           "AIMD-local increase when backlog grows and workers saturated (AC-10)");
    // No increase when active workers are not yet saturated.
    Expect(NextLocalWorkerCap(4, 32, 10, 2, 0) == 4,
           "AIMD-local no increase when workers not saturated (AC-10)");
    // No increase at the max cap.
    Expect(NextLocalWorkerCap(32, 32, 10, 32, 0) == 32, "AIMD-local capped at maxCap (AC-10)");
    // Multiplicative decrease on read-fail signal.
    Expect(NextLocalWorkerCap(8, 32, 10, 8, 3) == 4, "AIMD-local halves on read failures (AC-10)");
    // Decrease floors at 1.
    Expect(NextLocalWorkerCap(1, 32, 10, 1, 5) == 1, "AIMD-local floor 1 on failures (AC-10)");
}

// V-10 (AC-11): network window AIMD pure rule -- stable RTT gives additive increase; a spike gives
// multiplicative decrease; too few samples leaves it unchanged.
void TestNetWindowAimd() {
    // Not enough samples yet (ewma<=0) -> unchanged.
    Expect(NextNetWindow(8, 100.0, 0.0, 1, 256) == 8, "AIMD-net unchanged without samples (AC-11)");
    // Stable RTT (<= ewma*1.25) -> additive increase.
    Expect(NextNetWindow(8, 100.0, 100.0, 1, 256) == 9, "AIMD-net additive increase on stable RTT (AC-11)");
    // Increase capped at windowMax.
    Expect(NextNetWindow(256, 1.0, 100.0, 1, 256) == 256, "AIMD-net capped at windowMax (AC-11)");
    // RTT spike (> ewma*2) -> multiplicative decrease (100*0.6=60).
    Expect(NextNetWindow(100, 1000.0, 100.0, 1, 256) == 60, "AIMD-net mult decrease on RTT spike (AC-11)");
    // Middle band (between stable and spike) -> unchanged.
    Expect(NextNetWindow(8, 180.0, 100.0, 1, 256) == 8, "AIMD-net unchanged in middle band (AC-11)");
    // Decrease floors at windowMin.
    Expect(NextNetWindow(1, 1000.0, 100.0, 1, 256) == 1, "AIMD-net floor windowMin on spike (AC-11)");
}

// V-02 (AC-03/AC-06): strict Missing decided on the compare-pipeline path. The manifest lists a file
// absent locally; recv performs no probe (M1/FR-01), so the compare worker's StrictProbe->DecideCompare
// concludes Missing without hashing. Under candidate A (decisions.md D-01) no HashRequest is sent for a
// Missing file; any (arbitrary) unsolicited remote HashResponse is ignored (FR-06/NFR-07).
void TestStrictMissingViaWorker() {
    const fs::path dir = MakeTempDir();
    // "gone.txt" is intentionally NOT created locally.
    Hash256 wrong{};
    wrong[0] = 0x11;  // arbitrary remote hash; must be ignored for a Missing file

    MockChannel mock;
    mock.inbound.push_back(ManifestEntryFrame("gone.txt", 10));
    mock.inbound.push_back(ManifestEndFrame());
    mock.inbound.push_back(HashResponseFrame("gone.txt", wrong));

    CheckOptions o = BaseOptions(dir, Mode::Strict);
    FrameChannel ch = mock.Make();
    std::atomic<bool> interrupted{false};
    const EngineOutcome outcome = RunCheck(o, ch, interrupted);

    Expect(outcome.result.counters.missing == 1, "strict worker-path missing=1 (AC-03)");
    Expect(outcome.result.counters.same == 0 && outcome.result.counters.diff == 0,
           "strict missing yields no same/diff (AC-03)");
    Expect(mock.CountSent(MsgType::HashRequest) == 0,
           "strict missing sends NO HashRequest (candidate A, D-01)");
    Expect(outcome.exit == kDiffFound, "missing present -> exit 1");
    const DiffEntry* m = FindEntry(outcome.result, "gone.txt");
    Expect(m != nullptr, "gone.txt MISSING entry present");
    Expect(m->remoteSize.has_value() && *m->remoteSize == 10,
           "MISSING remoteSize = manifest size (10) (AC-03)");
    Expect(!m->localSize.has_value(), "MISSING localSize null (AC-03)");
    Expect(!m->hashCompared, "MISSING hash_compared=false (AC-03)");
    AssertNoTransferFrames(mock);

    std::error_code ec;
    fs::remove_all(dir, ec);
}

// V-03 (AC-04/AC-06): strict size-difference decided on the compare-pipeline path. Local file exists
// but its size differs from the manifest; the compare worker concludes Diff without hashing. Under
// candidate A (D-01) no HashRequest is sent for a SizeDiff file; an unsolicited HashResponse is ignored.
void TestStrictSizeDiffViaWorker() {
    const fs::path dir = MakeTempDir();
    WriteFile(dir / "s.txt", "12345");  // 5 bytes local; manifest says 999 -> size diff
    Hash256 wrong{};
    wrong[0] = 0x22;

    MockChannel mock;
    mock.inbound.push_back(ManifestEntryFrame("s.txt", 999));
    mock.inbound.push_back(ManifestEndFrame());
    mock.inbound.push_back(HashResponseFrame("s.txt", wrong));

    CheckOptions o = BaseOptions(dir, Mode::Strict);
    FrameChannel ch = mock.Make();
    std::atomic<bool> interrupted{false};
    const EngineOutcome outcome = RunCheck(o, ch, interrupted);

    Expect(outcome.result.counters.diff == 1, "strict worker-path size-diff diff=1 (AC-04)");
    Expect(outcome.result.counters.same == 0 && outcome.result.counters.missing == 0,
           "strict size-diff yields no same/missing (AC-04)");
    Expect(mock.CountSent(MsgType::HashRequest) == 0,
           "strict size-diff sends NO HashRequest (candidate A, D-01)");
    Expect(outcome.exit == kDiffFound, "diff present -> exit 1");
    const DiffEntry* d = FindEntry(outcome.result, "s.txt");
    Expect(d != nullptr, "s.txt DIFF entry present");
    Expect(d->localSize.has_value() && *d->localSize == 5, "DIFF localSize = 5 (AC-04)");
    Expect(d->remoteSize.has_value() && *d->remoteSize == 999, "DIFF remoteSize = 999 (AC-04)");
    Expect(!d->hashCompared, "size-diff hash_compared=false (AC-04)");
    AssertNoTransferFrames(mock);

    std::error_code ec;
    fs::remove_all(dir, ec);
}

// V-06 (AC-07/AC-08): arrival-order independence for strict Missing / SizeDiff / size-equal in a
// single batch. Under candidate A (D-01) only the size-equal file sends a HashRequest; the Missing and
// SizeDiff files are classified on the compare-drain path, so their scripted early HashResponses are
// unsolicited and ignored. Correctness holds on either ordering, so the assertions only check the
// final invariants (each file classified exactly once).
void TestStrictOrderingMissingDiff() {
    const fs::path dir = MakeTempDir();
    WriteFile(dir / "s.txt", "12345");   // 5 bytes, manifest 999 -> size diff
    WriteFile(dir / "eq.txt", "hello");  // 5 bytes, manifest 5 -> size-equal -> hash
    const Hash256 heq = ComputeFileHash(dir / "eq.txt");
    Hash256 wrong{};
    wrong[0] = 0x33;
    // "gone.txt" missing locally.

    MockChannel mock;
    mock.inbound.push_back(ManifestEntryFrame("gone.txt", 10));
    mock.inbound.push_back(HashResponseFrame("gone.txt", wrong));  // early response (before end)
    mock.inbound.push_back(ManifestEntryFrame("s.txt", 999));
    mock.inbound.push_back(HashResponseFrame("s.txt", wrong));     // early response (before end)
    mock.inbound.push_back(ManifestEntryFrame("eq.txt", 5));
    mock.inbound.push_back(ManifestEndFrame());
    mock.inbound.push_back(HashResponseFrame("eq.txt", heq));      // late response -> Same

    CheckOptions o = BaseOptions(dir, Mode::Strict);
    FrameChannel ch = mock.Make();
    std::atomic<bool> interrupted{false};
    const EngineOutcome outcome = RunCheck(o, ch, interrupted);

    Expect(outcome.result.counters.missing == 1, "ordering: missing=1 (AC-07/AC-08)");
    Expect(outcome.result.counters.diff == 1, "ordering: diff=1 (AC-07/AC-08)");
    Expect(outcome.result.counters.same == 1, "ordering: same=1 (AC-07/AC-08)");
    Expect(outcome.result.counters.TotalCompared() == 3, "ordering: total_compared=3");
    // Each file classified exactly once: counters prove one-each; the default filter keeps DIFF/MISSING
    // (not SAME), so entries hold exactly the Missing + SizeDiff files with no duplicates.
    Expect(FindEntry(outcome.result, "gone.txt") != nullptr, "gone.txt recorded once");
    Expect(FindEntry(outcome.result, "s.txt") != nullptr, "s.txt recorded once");
    Expect(outcome.result.entries.size() == 2,
           "exactly 2 listed entries (Missing+SizeDiff), no duplicates (AC-07/AC-08)");
    Expect(outcome.exit == kDiffFound, "diff/missing present -> exit 1");
    Expect(mock.CountSent(MsgType::HashRequest) == 1,
           "1 HashRequest (only the size-equal file; candidate A, D-01)");
    AssertNoTransferFrames(mock);

    std::error_code ec;
    fs::remove_all(dir, ec);
}

// V-07 (AC-12): --no-diskio-driver must apply the identical strict deferred-probe classification for
// Missing / SizeDiff / size-equal as the default driver path.
void TestStrictNoDiskioMissingSizeDiff() {
    const fs::path dir = MakeTempDir();
    WriteFile(dir / "s.txt", "12345");   // size diff (manifest 999)
    WriteFile(dir / "eq.txt", "hello");  // size-equal -> hash Same
    const Hash256 heq = ComputeFileHash(dir / "eq.txt");
    Hash256 wrong{};
    wrong[0] = 0x44;
    // "gone.txt" missing locally.

    MockChannel mock;
    mock.inbound.push_back(ManifestEntryFrame("gone.txt", 10));
    mock.inbound.push_back(ManifestEntryFrame("s.txt", 999));
    mock.inbound.push_back(ManifestEntryFrame("eq.txt", 5));
    mock.inbound.push_back(ManifestEndFrame());
    mock.inbound.push_back(HashResponseFrame("gone.txt", wrong));
    mock.inbound.push_back(HashResponseFrame("s.txt", wrong));
    mock.inbound.push_back(HashResponseFrame("eq.txt", heq));

    CheckOptions o = BaseOptions(dir, Mode::Strict);
    o.noDiskioDriver = true;  // degraded read path, same classification rules
    FrameChannel ch = mock.Make();
    std::atomic<bool> interrupted{false};
    const EngineOutcome outcome = RunCheck(o, ch, interrupted);

    Expect(outcome.result.counters.missing == 1, "--no-diskio strict missing=1 (AC-12)");
    Expect(outcome.result.counters.diff == 1, "--no-diskio strict size-diff diff=1 (AC-12)");
    Expect(outcome.result.counters.same == 1, "--no-diskio strict size-equal same=1 (AC-12)");
    Expect(outcome.result.counters.TotalCompared() == 3, "--no-diskio total_compared=3 (AC-12)");
    Expect(outcome.exit == kDiffFound, "diff/missing present -> exit 1");
    const DiffEntry* m = FindEntry(outcome.result, "gone.txt");
    Expect(m != nullptr && !m->localSize.has_value() && m->remoteSize.has_value() &&
               *m->remoteSize == 10,
           "--no-diskio MISSING fields match driver path (AC-12)");
    const DiffEntry* d = FindEntry(outcome.result, "s.txt");
    Expect(d != nullptr && d->localSize.has_value() && *d->localSize == 5 &&
               d->remoteSize.has_value() && *d->remoteSize == 999,
           "--no-diskio DIFF fields match driver path (AC-12)");

    std::error_code ec;
    fs::remove_all(dir, ec);
}

// V-08 (AC-14): strict mixed batch -- Missing + SizeDiff + hash-Same + hash-Diff + Extra -- yields
// counts consistent with existing strict semantics (same/diff/missing/extra_local/total_compared).
void TestStrictMixed() {
    const fs::path dir = MakeTempDir();
    WriteFile(dir / "s.txt", "12345");     // 5 bytes, manifest 999 -> size diff
    WriteFile(dir / "same.txt", "hello");  // 5 bytes, manifest 5 -> hash Same
    WriteFile(dir / "diffh.txt", "world"); // 5 bytes, manifest 5 -> hash Diff (wrong remote hash)
    WriteFile(dir / "extra.txt", "x");     // not in manifest -> Extra
    // "gone.txt" missing locally.
    const Hash256 hSame = ComputeFileHash(dir / "same.txt");
    Hash256 wrong{};
    wrong[0] = 0x55;

    MockChannel mock;
    mock.inbound.push_back(ManifestEntryFrame("gone.txt", 10));
    mock.inbound.push_back(ManifestEntryFrame("s.txt", 999));
    mock.inbound.push_back(ManifestEntryFrame("same.txt", 5));
    mock.inbound.push_back(ManifestEntryFrame("diffh.txt", 5));
    mock.inbound.push_back(ManifestEndFrame());
    mock.inbound.push_back(HashResponseFrame("gone.txt", wrong));   // ignored (Missing)
    mock.inbound.push_back(HashResponseFrame("s.txt", wrong));      // ignored (SizeDiff)
    mock.inbound.push_back(HashResponseFrame("same.txt", hSame));   // -> Same
    mock.inbound.push_back(HashResponseFrame("diffh.txt", wrong));  // -> Diff

    CheckOptions o = BaseOptions(dir, Mode::Strict);
    FrameChannel ch = mock.Make();
    std::atomic<bool> interrupted{false};
    const EngineOutcome outcome = RunCheck(o, ch, interrupted);

    Expect(outcome.result.counters.same == 1, "mixed strict same=1 (AC-14)");
    Expect(outcome.result.counters.diff == 2, "mixed strict diff=2 (size-diff + hash-diff) (AC-14)");
    Expect(outcome.result.counters.missing == 1, "mixed strict missing=1 (AC-14)");
    Expect(outcome.result.counters.extraLocal == 1, "mixed strict extra_local=1 (AC-14)");
    Expect(outcome.result.counters.TotalCompared() == 4, "mixed strict total_compared=4 (AC-14)");
    Expect(mock.CountSent(MsgType::HashRequest) == 2,
           "2 HashRequests (only the 2 size-equal files; candidate A, D-01)");
    Expect(outcome.exit == kDiffFound, "differences present -> exit 1");
    AssertNoTransferFrames(mock);

    std::error_code ec;
    fs::remove_all(dir, ec);
}

// V-03 (AC-04): strict worker classifies a local DIRECTORY that shadows a manifest FILE as Missing.
// fs::file_size on a directory returns an error_code, so the worker must conclude Missing without
// hashing and without marking the run partial (edge case: directory / special file -> Missing).
void TestStrictDirIsMissing() {
    const fs::path dir = MakeTempDir();
    fs::create_directories(dir / "d.txt");  // local directory named like the manifest file
    Hash256 wrong{};
    wrong[0] = 0x66;  // arbitrary remote hash; must be ignored for a Missing file

    MockChannel mock;
    mock.inbound.push_back(ManifestEntryFrame("d.txt", 10));
    mock.inbound.push_back(ManifestEndFrame());
    mock.inbound.push_back(HashResponseFrame("d.txt", wrong));

    CheckOptions o = BaseOptions(dir, Mode::Strict);
    FrameChannel ch = mock.Make();
    std::atomic<bool> interrupted{false};
    const EngineOutcome outcome = RunCheck(o, ch, interrupted);

    Expect(outcome.result.counters.missing == 1, "strict dir-as-file missing=1 (AC-04)");
    Expect(!outcome.result.partial, "strict dir-as-file must NOT be partial (AC-04)");
    Expect(outcome.exit == kDiffFound, "missing present -> exit 1");
    const DiffEntry* m = FindEntry(outcome.result, "d.txt");
    Expect(m != nullptr, "d.txt MISSING entry present");
    Expect(!m->localSize.has_value(), "dir-as-file MISSING localSize null (AC-04)");
    Expect(!m->hashCompared, "dir-as-file MISSING hash_compared=false (AC-04)");

    std::error_code ec;
    fs::remove_all(dir, ec);
}

// V-04 (AC-06): strict size-diff where the LOCAL file is LARGER than the manifest size. The worker
// concludes Diff without hashing; localSize/remoteSize reflect the two sizes, hashCompared=false.
void TestStrictSizeDiffLocalGreater() {
    const fs::path dir = MakeTempDir();
    WriteFile(dir / "big.txt", "1234567890");  // 10 bytes local; manifest says 4 -> local > remote
    Hash256 wrong{};
    wrong[0] = 0x77;

    MockChannel mock;
    mock.inbound.push_back(ManifestEntryFrame("big.txt", 4));
    mock.inbound.push_back(ManifestEndFrame());
    mock.inbound.push_back(HashResponseFrame("big.txt", wrong));

    CheckOptions o = BaseOptions(dir, Mode::Strict);
    FrameChannel ch = mock.Make();
    std::atomic<bool> interrupted{false};
    const EngineOutcome outcome = RunCheck(o, ch, interrupted);

    Expect(outcome.result.counters.diff == 1, "strict local>remote size-diff diff=1 (AC-06)");
    const DiffEntry* d = FindEntry(outcome.result, "big.txt");
    Expect(d != nullptr, "big.txt DIFF entry present");
    Expect(d->localSize.has_value() && *d->localSize == 10, "DIFF localSize=10 (AC-06)");
    Expect(d->remoteSize.has_value() && *d->remoteSize == 4, "DIFF remoteSize=4 (AC-06)");
    Expect(!d->hashCompared, "local>remote size-diff hash_compared=false (AC-06)");
    Expect(outcome.exit == kDiffFound, "diff present -> exit 1");

    std::error_code ec;
    fs::remove_all(dir, ec);
}

#if defined(_WIN32)
// V-01T (AC-09): TOCTOU. The strict worker's fs::file_size succeeds (size == manifest -> hashOne),
// but the driver read open then fails because the file is held with an exclusive (no-share) handle,
// so hashOne throws -> LocalResult::Failed -> localReadFailed -> partial + kLocalPrecondFailed.
void TestStrictToctouFailed() {
    const fs::path dir = MakeTempDir();
    const fs::path f = dir / "locked.txt";
    WriteFile(f, "hello");                     // 5 bytes; manifest 5 -> size-equal -> hashOne
    const Hash256 h = ComputeFileHash(f);      // compute the hash BEFORE taking the exclusive lock

    // No-share handle: fs::file_size (GetFileAttributesExW, a directory-metadata query) still
    // succeeds, but the driver's CreateFileW(..., FILE_SHARE_READ, ...) read open hits a sharing
    // violation and returns 0 -> ComputeFileHashViaDriver throws -> hashOne -> Failed.
    HANDLE lock = CreateFileW(f.wstring().c_str(), GENERIC_READ, /*dwShareMode=*/0, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Expect(lock != INVALID_HANDLE_VALUE, "toctou: exclusive lock handle opened");

    MockChannel mock;
    mock.inbound.push_back(ManifestEntryFrame("locked.txt", 5));
    mock.inbound.push_back(ManifestEndFrame());
    mock.inbound.push_back(HashResponseFrame("locked.txt", h));

    CheckOptions o = BaseOptions(dir, Mode::Strict);
    FrameChannel ch = mock.Make();
    std::atomic<bool> interrupted{false};
    const EngineOutcome outcome = RunCheck(o, ch, interrupted);
    CloseHandle(lock);

    Expect(outcome.result.partial, "toctou: local read failure -> partial (AC-09)");
    Expect(outcome.exit == kLocalPrecondFailed, "toctou: exit=kLocalPrecondFailed (AC-09)");

    std::error_code ec;
    fs::remove_all(dir, ec);
}
#endif

}  // namespace

void RunCheckEngineTests() {
    TestSizeOnlyCountsNoHash();
    TestStrictHashSameAndDiff();
    TestStrictMissingViaWorker();
    TestStrictSizeDiffViaWorker();
    TestStrictSizeDiffLocalGreater();
    TestStrictDirIsMissing();
#if defined(_WIN32)
    TestStrictToctouFailed();
#endif
    TestStrictOrderingMissingDiff();
    TestStrictNoDiskioMissingSizeDiff();
    TestStrictMixed();
    TestFastHashSameAndDiff();
    TestCheckersOneSerializesHash();
    TestEngineFilterShapesEntries();
    TestIdenticalExitZero();
    TestInterruptedPartial();
    TestDisconnectExitTwo();
    TestEmptyDirectories();
    TestHashResponseBeforeLocalHash();
    TestHashWorkersOneSerial();
    TestNoDiskioDriverDegraded();
    TestComputeFileHashViaDriverConsistency();
    TestComputeFileHashViaDriverUnalignedTail();
    TestComputeFileHashViaDriverSparseFile();
    TestComputeFileHashViaDriverSmallFileSingleRead();
    TestComputeFileHashViaDriverAlignedHash();
    TestComputeFileHashViaDriverSmallReadFailures();
    TestResolveMaxHashWorkers();
    TestLocalWorkerCapAimd();
    TestNetWindowAimd();
}
