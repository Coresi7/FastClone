// Tests for the unified disk IO driver (unified-disk-io-driver C3, FR-12/13/25/26/27/28/29/30,
// AC-09/19/20/21/23/24/35). A deterministic in-memory Mock backend exercises the scheduler
// (fairness, multi-op in flight, backpressure, cancellation, counters); the REAL platform backend
// (Windows IOCP+NO_BUFFERING / Linux io_uring-or-pool / macOS pool) is round-tripped end to end
// through a temp file to prove data correctness incl. the small-file and EOF-tail buffered paths.

#include "disk_io_backend.h"
#include "disk_io_driver.h"

#if defined(__linux__)
// Linux-only: the io_uring runtime probe falls back to the pread/pwrite pool, which must report
// counters().ioUringFallback==1 (AC-11 / B-01). Pulled in here to exercise the pool seeding path.
#include "disk_io_backend_pool.h"
#endif

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <mutex>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

void Require(bool cond, const std::string& msg) {
    if (!cond) {
        throw std::runtime_error("disk_io_driver: " + msg);
    }
}

using namespace fc::io;

// Deterministic in-memory backend. Each file is a byte vector; reads/writes hit it directly.
// `paused_` withholds completions from reap() so backpressure/cancel can be observed; peak tracks
// the max simultaneously-held (submitted-not-reaped) ops for the multi-op-in-flight assertion.
class MockBackend : public PlatformIoBackend {
public:
    explicit MockBackend(const IoDriverConfig& cfg) : cfg_(cfg) {}

    BackendKind kind() const override { return BackendKind::Mock; }
    AlignInfo queryAlign(const std::string&) override { return MakeAlignInfo(4096, 4096); }

    uint64_t openFile(const std::string&, OpKind mode, bool unbuffered, uint64_t expectedSize) override {
        std::lock_guard<std::mutex> lk(mu_);
        const uint64_t id = nextId_++;
        MFile f;
        f.mode = mode;
        if (mode == OpKind::Write && expectedSize > 0) {
            f.data.resize(static_cast<size_t>(expectedSize), 0);
        }
        if (unbuffered) {
            counters_.directIo.fetch_add(0);  // marker only; mock does not model direct IO
        }
        files_.emplace(id, std::move(f));
        return id;
    }
    void closeFile(uint64_t id) override {
        std::lock_guard<std::mutex> lk(mu_);
        files_.erase(id);
    }

    bool submit(IoRequest&& req) override {
        std::lock_guard<std::mutex> lk(mu_);
        if (failSubmit_) {
            return false;  // F2: hard backend failure; driver synthesizes the error completion
        }
        held_.push_back(std::move(req));
        peak_ = std::max<size_t>(peak_, held_.size());
        return true;
    }

    size_t reap(std::vector<IoCompletion>& out, size_t max, int timeoutMs) override {
        std::unique_lock<std::mutex> lk(mu_);
        if (paused_) {
            return 0;
        }
        if (held_.empty() && timeoutMs != 0) {
            cv_.wait_for(lk, std::chrono::milliseconds(timeoutMs < 0 ? 50 : timeoutMs),
                         [this] { return !held_.empty() || paused_; });
        }
        size_t n = 0;
        while (!held_.empty() && n < max) {
            IoRequest req = std::move(held_.front());
            held_.pop_front();
            out.push_back(Execute(std::move(req)));
            ++n;
        }
        return n;
    }

    void shutdown() override {
        std::lock_guard<std::mutex> lk(mu_);
        paused_ = false;
        held_.clear();  // release without producing completions (driver already stopping)
    }

    BackendCounters counters() const override {
        BackendCounters c;
        c.directIo = counters_.directIo.load();
        return c;
    }

    // Test controls.
    void pause(bool p) {
        std::lock_guard<std::mutex> lk(mu_);
        paused_ = p;
    }
    size_t peakInFlight() {
        std::lock_guard<std::mutex> lk(mu_);
        return peak_;
    }
    std::vector<uint8_t> fileData(uint64_t id) {
        std::lock_guard<std::mutex> lk(mu_);
        return files_.count(id) ? files_[id].data : std::vector<uint8_t>{};
    }
    void setReadData(uint64_t id, std::vector<uint8_t> d) {
        std::lock_guard<std::mutex> lk(mu_);
        files_[id].data = std::move(d);
    }
    // F2: force every submit() to fail so the driver's failure-synthesis path is exercised.
    void failSubmit(bool f) {
        std::lock_guard<std::mutex> lk(mu_);
        failSubmit_ = f;
    }
    // F4: force every read to complete as IoStatus::Error (AC-10).
    void forceReadError(bool f) {
        std::lock_guard<std::mutex> lk(mu_);
        forceReadError_ = f;
    }

private:
    struct MFile {
        std::vector<uint8_t> data;
        OpKind mode = OpKind::Read;
    };
    IoCompletion Execute(IoRequest&& req) {
        IoCompletion c;
        c.kind = req.kind;
        c.fileId = req.fileId;
        c.offset = req.offset;
        c.requested = req.length;
        c.userTag = req.userTag;
        if (req.kind == OpKind::Read && forceReadError_) {
            c.status = IoStatus::Error;  // F4: AC-10 forced read error at the yield offset
            return c;
        }
        auto it = files_.find(req.fileId);
        if (it == files_.end()) {
            c.status = IoStatus::Error;
            return c;
        }
        MFile& f = it->second;
        if (req.kind == OpKind::Read) {
            const size_t off = static_cast<size_t>(req.offset);
            const size_t n = off < f.data.size()
                                 ? std::min<size_t>(req.length, f.data.size() - off)
                                 : 0;
            if (n > 0) {
                c.data.assign(f.data.begin() + off, f.data.begin() + off + n);
            }
            c.transferred = static_cast<uint32_t>(n);
            c.status = (n < req.length) ? IoStatus::Eof : IoStatus::Ok;
        } else {
            const size_t end = static_cast<size_t>(req.offset) + req.data.size();
            if (f.data.size() < end) {
                f.data.resize(end, 0);
            }
            std::copy(req.data.begin(), req.data.end(),
                      f.data.begin() + static_cast<std::ptrdiff_t>(req.offset));
            c.transferred = static_cast<uint32_t>(req.data.size());
            c.status = IoStatus::Ok;
        }
        return c;
    }

    IoDriverConfig cfg_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::unordered_map<uint64_t, MFile> files_;
    std::deque<IoRequest> held_;
    uint64_t nextId_ = 1;
    size_t peak_ = 0;
    bool paused_ = false;
    bool failSubmit_ = false;
    bool forceReadError_ = false;
    struct { std::atomic<uint64_t> directIo{0}; } counters_;
};

std::vector<uint8_t> RandomBytes(size_t n, uint32_t seed) {
    std::vector<uint8_t> v(n);
    std::mt19937 rng(seed);
    for (auto& b : v) b = static_cast<uint8_t>(rng() & 0xFF);
    return v;
}

// Drain every completion until `expected` are seen or a wall-clock deadline passes.
size_t DrainUntil(DiskIoDriver& drv, size_t expected, std::vector<IoCompletion>& out, int ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (out.size() < expected && std::chrono::steady_clock::now() < deadline) {
        std::vector<IoCompletion> tmp;
        if (drv.drainCompletions(tmp) == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        for (auto& c : tmp) out.push_back(std::move(c));
    }
    return out.size();
}

void TestMockRoundTripAndCounters() {
    IoDriverConfig cfg;
    cfg.maxInFlight = 8;
    auto mock = std::make_unique<MockBackend>(cfg);
    MockBackend* raw = mock.get();
    DiskIoDriver drv(cfg, std::move(mock));

    const uint64_t fid = drv.openFile("mem", OpKind::Write, true, 0);
    const std::vector<uint8_t> payload = RandomBytes(50000, 7);
    std::vector<IoRequest> batch;
    const uint32_t chunk = 4096;
    for (size_t off = 0; off < payload.size(); off += chunk) {
        IoRequest r;
        r.kind = OpKind::Write;
        r.fileId = fid;
        r.offset = off;
        const size_t n = std::min<size_t>(chunk, payload.size() - off);
        r.data.assign(payload.begin() + off, payload.begin() + off + n);
        r.length = static_cast<uint32_t>(n);
        batch.push_back(std::move(r));
    }
    const size_t count = batch.size();
    while (!batch.empty()) {
        drv.submit(batch);
    }
    std::vector<IoCompletion> comps;
    Require(DrainUntil(drv, count, comps, 5000) == count, "mock write completions");
    Require(raw->fileData(fid) == payload, "mock write data mismatch");

    const IoCounters c = drv.counters();
    Require(c.submitted == count, "counter submitted");
    Require(c.completed == count, "counter completed");
    Require(c.writeSubmitted == count, "counter writeSubmitted");
    Require(c.failed == 0 && c.cancelled == 0, "no failed/cancelled");
}

void TestMultiOpInFlight() {
    // AC-09: with a burst of work and maxInFlight>1 the backend observes >1 op held at once.
    IoDriverConfig cfg;
    cfg.maxInFlight = 16;
    auto mock = std::make_unique<MockBackend>(cfg);
    MockBackend* raw = mock.get();
    raw->pause(true);  // hold submissions so a burst accumulates
    DiskIoDriver drv(cfg, std::move(mock));

    const uint64_t fid = drv.openFile("mem", OpKind::Read, true, 0);
    raw->setReadData(fid, RandomBytes(200000, 3));
    std::vector<IoRequest> batch;
    for (int i = 0; i < 32; ++i) {
        IoRequest r;
        r.kind = OpKind::Read;
        r.fileId = fid;
        r.offset = static_cast<uint64_t>(i) * 4096;
        r.length = 4096;
        batch.push_back(std::move(r));
    }
    while (!batch.empty()) drv.submit(batch);
    // Give the scheduler time to submit up to maxInFlight into the (paused) backend.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    Require(raw->peakInFlight() > 1, "multi-op in flight peak > 1");
    raw->pause(false);  // release so the driver can drain and shut down cleanly
    std::vector<IoCompletion> comps;
    DrainUntil(drv, 32, comps, 5000);
}

void TestFairness() {
    // AC-19: 1:1 read/write weight -> no 3 consecutive same-direction submissions.
    IoDriverConfig cfg;
    cfg.readWeight = 1;
    cfg.writeWeight = 1;
    cfg.maxInFlight = 8;
    cfg.recordSchedule = true;
    cfg.maxReadQueue = 512;
    cfg.maxWriteQueue = 512;
    auto mock = std::make_unique<MockBackend>(cfg);
    DiskIoDriver drv(cfg, std::move(mock));

    const uint64_t rf = drv.openFile("r", OpKind::Read, false, 0);
    const uint64_t wf = drv.openFile("w", OpKind::Write, false, 0);
    // Enqueue reads and writes interleaved in ONE submit() call so both queues are populated
    // before the scheduler (blocked on the same lock) can pick — makes alternation deterministic.
    std::vector<IoRequest> batch;
    for (int i = 0; i < 150; ++i) {
        IoRequest r;
        r.kind = OpKind::Read;
        r.fileId = rf;
        r.offset = static_cast<uint64_t>(i) * 4096;
        r.length = 64;
        batch.push_back(std::move(r));
        IoRequest w;
        w.kind = OpKind::Write;
        w.fileId = wf;
        w.offset = static_cast<uint64_t>(i) * 64;
        w.data.assign(64, static_cast<uint8_t>(i));
        w.length = 64;
        batch.push_back(std::move(w));
    }
    const size_t total = batch.size();
    while (!batch.empty()) {
        drv.submit(batch);
    }
    std::vector<IoCompletion> comps;
    DrainUntil(drv, total, comps, 8000);

    const std::vector<OpKind> log = drv.scheduleLog();
    Require(log.size() == total, "schedule log size");
    size_t reads = 0, writes = 0;
    for (auto k : log) (k == OpKind::Read ? reads : writes)++;
    Require(reads == 150 && writes == 150, "balanced submissions");
    for (size_t i = 0; i + 2 < log.size(); ++i) {
        Require(!(log[i] == log[i + 1] && log[i + 1] == log[i + 2]),
                "3 consecutive same-direction submissions violate 1:1 fairness");
    }
}

void TestBackpressure() {
    // AC-21/27: submit() accepts at most the queue bound; the surplus stays for the caller to
    // retry (the network side stops enlarging resident payload).
    IoDriverConfig cfg;
    cfg.maxWriteQueue = 8;
    cfg.maxInFlight = 2;
    auto mock = std::make_unique<MockBackend>(cfg);
    MockBackend* raw = mock.get();
    raw->pause(true);  // nothing drains, so the queue truly fills
    DiskIoDriver drv(cfg, std::move(mock));

    const uint64_t fid = drv.openFile("w", OpKind::Write, false, 0);
    std::vector<IoRequest> batch;
    for (int i = 0; i < 100; ++i) {
        IoRequest r;
        r.kind = OpKind::Write;
        r.fileId = fid;
        r.offset = static_cast<uint64_t>(i) * 64;
        r.data.assign(64, 1);
        r.length = 64;
        batch.push_back(std::move(r));
    }
    const size_t accepted = drv.submit(batch);
    Require(accepted <= cfg.maxWriteQueue, "backpressure caps accepted at queue bound");
    Require(accepted < 100, "backpressure: not all accepted");
    Require(batch.size() == 100 - accepted, "surplus left for retry");
    raw->pause(false);  // let the driver drain + shut down without hanging
    std::vector<IoCompletion> comps;
    DrainUntil(drv, accepted, comps, 5000);
}

void TestCancel() {
    // AC-23/28: requestCancel flushes not-yet-submitted ops as Cancelled; the driver still shuts
    // down cleanly (threads join, no hang).
    IoDriverConfig cfg;
    cfg.maxInFlight = 2;
    auto mock = std::make_unique<MockBackend>(cfg);
    MockBackend* raw = mock.get();
    raw->pause(true);
    DiskIoDriver drv(cfg, std::move(mock));

    const uint64_t fid = drv.openFile("w", OpKind::Write, false, 0);
    std::vector<IoRequest> batch;
    for (int i = 0; i < 40; ++i) {
        IoRequest r;
        r.kind = OpKind::Write;
        r.fileId = fid;
        r.offset = static_cast<uint64_t>(i) * 64;
        r.data.assign(64, 2);
        r.length = 64;
        batch.push_back(std::move(r));
    }
    while (!batch.empty()) {
        if (drv.submit(batch) == 0) break;  // queue full (paused backend) -> stop
    }
    drv.requestCancel();
    std::vector<IoCompletion> comps;
    drv.drainCompletions(comps);
    size_t cancelled = 0;
    for (auto& c : comps) {
        if (c.status == IoStatus::Cancelled) ++cancelled;
    }
    Require(cancelled > 0, "cancel flushed queued ops as Cancelled");
    const IoCounters c = drv.counters();
    Require(c.cancelled >= cancelled, "counter cancelled");
    // Further submits are rejected after cancel.
    std::vector<IoRequest> more(1);
    more[0].kind = OpKind::Write;
    more[0].fileId = fid;
    more[0].length = 0;
    Require(drv.submit(more) == 0, "no submits accepted after cancel");
    raw->pause(false);
}

void TestSubmitFailureAttribution() {
    // F2 (AC-05/06/07): a hard submit() failure must synthesize an Error completion attributed to
    // the ORIGINAL fileId, preserving kind/offset/requested/userTag (not routed to fileId 0).
    IoDriverConfig cfg;
    cfg.maxInFlight = 4;
    auto mock = std::make_unique<MockBackend>(cfg);
    MockBackend* raw = mock.get();
    raw->failSubmit(true);
    DiskIoDriver drv(cfg, std::move(mock));

    const uint64_t fid = drv.openFile("mem", OpKind::Read, true, 0);
    Require(fid != 0, "F2: fileId is nonzero");
    std::vector<IoRequest> batch(1);
    batch[0].kind = OpKind::Read;
    batch[0].fileId = fid;
    batch[0].offset = 12288;
    batch[0].length = 4096;
    batch[0].userTag = 0xABCDEF;
    while (!batch.empty()) {
        drv.submit(batch);
    }

    drv.waitForFile(fid, 2000);
    std::vector<IoCompletion> out;
    const size_t n = drv.drainCompletionsForFile(fid, out);
    Require(n == 1, "F2: exactly one completion for the original file (AC-05)");
    Require(out[0].fileId == fid, "F2: completion fileId == original (AC-05)");
    Require(out[0].status == IoStatus::Error, "F2: completion status Error (AC-05)");
    Require(out[0].kind == OpKind::Read, "F2: kind preserved (AC-07)");
    Require(out[0].offset == 12288, "F2: offset preserved (AC-07)");
    Require(out[0].requested == 4096, "F2: requested preserved (AC-07)");
    Require(out[0].userTag == 0xABCDEF, "F2: userTag preserved (AC-07)");

    std::vector<IoCompletion> out0;
    Require(drv.drainCompletionsForFile(0, out0) == 0,
            "F2: failed op not routed to fileId 0 (AC-06)");
}

void TestSequentialReaderEarlyError() {
    // F4 (AC-10/AC-12): an Error at the next expected offset -> next() returns 0 with ok=false, and
    // subsequent next() calls stay ok=false.
    IoDriverConfig cfg;
    cfg.maxInFlight = 4;
    auto mock = std::make_unique<MockBackend>(cfg);
    MockBackend* raw = mock.get();
    raw->forceReadError(true);
    DiskIoDriver drv(cfg, std::move(mock));

    const uint64_t fid = drv.openFile("mem", OpKind::Read, true, 0);
    raw->setReadData(fid, RandomBytes(8192, 5));
    SequentialReader reader(drv, fid, 8192, 4096, 4);

    std::vector<uint8_t> chunk;
    bool ok = true;
    Require(reader.next(chunk, ok) == 0 && !ok, "F4: read error -> 0 and ok=false (AC-10)");
    ok = true;
    Require(reader.next(chunk, ok) == 0 && !ok, "F4: error state persists (AC-12)");
}

void TestSequentialReaderEarlyEof() {
    // F4 (AC-11/AC-12): an early Eof (data shorter than the planned fileSize_) is a failure, NOT a
    // clean EOF.
    IoDriverConfig cfg;
    cfg.maxInFlight = 4;
    auto mock = std::make_unique<MockBackend>(cfg);
    MockBackend* raw = mock.get();
    DiskIoDriver drv(cfg, std::move(mock));

    const uint64_t fid = drv.openFile("mem", OpKind::Read, true, 0);
    raw->setReadData(fid, RandomBytes(4096, 6));  // only 4096 bytes, but reader plans fileSize_=8192
    SequentialReader reader(drv, fid, 8192, 4096, 4);

    std::vector<uint8_t> chunk;
    bool ok = true;
    Require(reader.next(chunk, ok) == 4096 && ok, "F4: first full chunk ok");
    ok = true;
    Require(reader.next(chunk, ok) == 0 && !ok, "F4: early Eof -> 0 and ok=false (AC-11)");
    ok = true;
    Require(reader.next(chunk, ok) == 0 && !ok, "F4: early Eof state persists (AC-12)");
}

void TestSequentialReaderCleanEof() {
    // F4 (AC-13): reading exactly fileSize_ bytes then next() -> 0 with ok=true (clean EOF).
    IoDriverConfig cfg;
    cfg.maxInFlight = 4;
    auto mock = std::make_unique<MockBackend>(cfg);
    MockBackend* raw = mock.get();
    DiskIoDriver drv(cfg, std::move(mock));

    const uint64_t fid = drv.openFile("mem", OpKind::Read, true, 0);
    raw->setReadData(fid, RandomBytes(8192, 7));
    SequentialReader reader(drv, fid, 8192, 4096, 4);

    std::vector<uint8_t> chunk;
    bool ok = true;
    size_t total = 0;
    for (;;) {
        ok = true;
        const uint32_t n = reader.next(chunk, ok);
        Require(ok, "F4: clean read ok");
        if (n == 0) break;
        total += n;
    }
    Require(total == 8192, "F4: read all bytes before EOF");
    ok = true;
    Require(reader.next(chunk, ok) == 0 && ok, "F4: clean EOF -> 0 and ok=true (AC-13)");
}

// End-to-end round trip through the REAL platform backend (verifies IOCP+NO_BUFFERING on Windows,
// io_uring/pool on Linux, F_NOCACHE pool on macOS), including the small-file buffered path and the
// >1 MiB unbuffered path with an unaligned EOF tail.
void RealBackendRoundTrip(uint64_t size, uint32_t seed) {
    namespace fs = std::filesystem;
    const fs::path tmp = fs::temp_directory_path() /
                         ("fc_io_rt_" + std::to_string(size) + "_" + std::to_string(seed) + ".bin");
    const std::string path = tmp.string();
    std::error_code ec;
    fs::remove(tmp, ec);
    const std::vector<uint8_t> payload = RandomBytes(static_cast<size_t>(size), seed);

    IoDriverConfig cfg;
    cfg.maxInFlight = 8;
    cfg.chunkBytes = 1u << 20;
    {
        DiskIoDriver drv(cfg);
        const uint64_t wf = drv.openFile(path, OpKind::Write, true, size);
        Require(wf != 0, "open write");
        std::vector<IoRequest> batch;
        for (uint64_t off = 0; off < size; off += cfg.chunkBytes) {
            IoRequest r;
            r.kind = OpKind::Write;
            r.fileId = wf;
            r.offset = off;
            const uint64_t n = std::min<uint64_t>(cfg.chunkBytes, size - off);
            r.data.assign(payload.begin() + static_cast<std::ptrdiff_t>(off),
                          payload.begin() + static_cast<std::ptrdiff_t>(off + n));
            r.length = static_cast<uint32_t>(n);
            batch.push_back(std::move(r));
        }
        const size_t count = batch.size() ? batch.size() : 0;
        while (!batch.empty()) {
            if (drv.submit(batch) == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        std::vector<IoCompletion> comps;
        Require(DrainUntil(drv, count, comps, 15000) == count, "write completions");
        for (auto& c : comps) {
            Require(c.status != IoStatus::Error, "write op error");
        }
        drv.closeFile(wf);  // SetEndOfFile / ftruncate to exact size
    }

    // Verify the on-disk size and bytes.
    Require(static_cast<uint64_t>(fs::file_size(tmp, ec)) == size, "on-disk size mismatch");
    {
        DiskIoDriver drv(cfg);
        const uint64_t rf = drv.openFile(path, OpKind::Read, true, size);
        Require(rf != 0, "open read");
        SequentialReader reader(drv, rf, size, cfg.chunkBytes, 4);
        std::vector<uint8_t> got;
        for (;;) {
            std::vector<uint8_t> chunk;
            bool ok = true;
            const uint32_t n = reader.next(chunk, ok);
            Require(ok, "sequential read error");
            if (n == 0) break;
            got.insert(got.end(), chunk.begin(), chunk.end());
        }
        drv.closeFile(rf);
        Require(got.size() == payload.size(), "round-trip read size");
        Require(got == payload, "round-trip bytes mismatch");
    }
    fs::remove(tmp, ec);
}

void TestRealBackend() {
    RealBackendRoundTrip(4096, 11);            // small file -> buffered path (FR-11)
    RealBackendRoundTrip(300000, 12);          // < 1 MiB -> buffered path
    RealBackendRoundTrip((2u << 20), 13);      // 2 MiB, aligned -> unbuffered path
    RealBackendRoundTrip((2u << 20) + 12345, 14);  // 2 MiB + unaligned EOF tail (FR-11 tail)
}

#if defined(__linux__)
// F1 (AC-01/02/03): fully aligned unbuffered ops on the POSIX pool must take the aligned direct IO
// path (internal bounce buffer), NOT a per-op buffered fallback. The differentiator is
// tailZeroFallback: 0 when fixed, N when the buggy dst/src pointer-alignment condition forced the
// fallback. Byte equality (AC-03) is asserted unconditionally; the tailZeroFallback invariant is
// gated on directIo actually advancing (O_DIRECT taken, design R-01 / D2).
void TestPosixPoolAlignedDirectIo() {
    namespace fs = std::filesystem;
    IoDriverConfig cfg;
    auto backend = CreatePosixPoolBackend(cfg);

    const fs::path tmp = fs::temp_directory_path() / "fc_f1_aligned.bin";
    const std::string path = tmp.string();
    std::error_code ec;
    fs::remove(tmp, ec);

    const uint32_t g = backend->queryAlign(path).ioGranularity;
    Require(g > 0, "F1: granularity > 0");
    const uint32_t chunk = g;  // one granule per op -> off%g==0 && len%g==0
    const uint32_t nOps = 4;
    const uint64_t size = static_cast<uint64_t>(chunk) * nOps;
    const std::vector<uint8_t> payload = RandomBytes(static_cast<size_t>(size), 21);

    auto reapN = [&](size_t want) {
        std::vector<IoCompletion> comps;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (comps.size() < want && std::chrono::steady_clock::now() < deadline) {
            backend->reap(comps, 128, 50);
        }
        return comps;
    };

    const uint64_t wf = backend->openFile(path, OpKind::Write, true, size);
    Require(wf != 0, "F1: open write");
    const BackendCounters wc0 = backend->counters();
    for (uint32_t i = 0; i < nOps; ++i) {
        IoRequest r;
        r.kind = OpKind::Write;
        r.fileId = wf;
        r.offset = static_cast<uint64_t>(i) * chunk;
        r.length = chunk;
        r.data.assign(payload.begin() + static_cast<std::ptrdiff_t>(i) * chunk,
                      payload.begin() + static_cast<std::ptrdiff_t>(i + 1) * chunk);
        Require(backend->submit(std::move(r)), "F1: submit write");
    }
    auto wcomps = reapN(nOps);
    Require(wcomps.size() == nOps, "F1: write completions");
    for (auto& c : wcomps) Require(c.status != IoStatus::Error, "F1: write not error");
    const BackendCounters wc1 = backend->counters();
    backend->closeFile(wf);

    const uint64_t rf = backend->openFile(path, OpKind::Read, true, size);
    Require(rf != 0, "F1: open read");
    const BackendCounters rc0 = backend->counters();
    for (uint32_t i = 0; i < nOps; ++i) {
        IoRequest r;
        r.kind = OpKind::Read;
        r.fileId = rf;
        r.offset = static_cast<uint64_t>(i) * chunk;
        r.length = chunk;
        Require(backend->submit(std::move(r)), "F1: submit read");
    }
    auto rcomps = reapN(nOps);
    Require(rcomps.size() == nOps, "F1: read completions");
    const BackendCounters rc1 = backend->counters();
    backend->closeFile(rf);

    std::vector<uint8_t> got(static_cast<size_t>(size), 0);
    for (auto& c : rcomps) {
        Require(c.status != IoStatus::Error, "F1: read not error");
        std::copy(c.data.begin(), c.data.end(),
                  got.begin() + static_cast<std::ptrdiff_t>(c.offset));
    }
    Require(got == payload, "F1: round-trip bit-identical (AC-03)");
    Require(static_cast<uint64_t>(fs::file_size(tmp, ec)) == size, "F1: on-disk size (AC-03)");

    if ((wc1.directIo - wc0.directIo) == nOps) {
        Require((wc1.tailZeroFallback - wc0.tailZeroFallback) == 0,
                "F1: aligned writes take no per-op buffered fallback (AC-02)");
    }
    if ((rc1.directIo - rc0.directIo) == nOps) {
        Require((rc1.tailZeroFallback - rc0.tailZeroFallback) == 0,
                "F1: aligned reads take no per-op buffered fallback (AC-02)");
    }

    backend->shutdown();
    fs::remove(tmp, ec);
}

// B-01 (AC-11): the io_uring runtime-probe fallback to the pool must surface ioUringFallback==1.
// Deterministic coverage exercises the pool seeding used by CreatePlatformBackend's two fallback
// branches; a normal pool leaves the counter at 0. Additionally, when the real factory returns the
// pool (io_uring unavailable at runtime / build) the counter MUST be 1.
void TestUringFallbackCounter() {
    IoDriverConfig cfg;
    auto fb = CreatePosixPoolBackend(cfg, /*ioUringFallback=*/true);
    Require(fb->counters().ioUringFallback == 1, "pool seeded as io_uring fallback -> counter 1");
    fb->shutdown();

    auto normal = CreatePosixPoolBackend(cfg, /*ioUringFallback=*/false);
    Require(normal->counters().ioUringFallback == 0, "normal pool -> counter 0");
    normal->shutdown();

    auto plat = CreatePlatformBackend(cfg);
    if (plat->kind() == BackendKind::PosixThreadPool) {
        Require(plat->counters().ioUringFallback == 1,
                "CreatePlatformBackend fell back to pool -> ioUringFallback 1");
    }
    plat->shutdown();
}
#endif

}  // namespace

void RunDiskIoDriverTests() {
    TestMockRoundTripAndCounters();
    TestMultiOpInFlight();
    TestFairness();
    TestBackpressure();
    TestCancel();
    TestSubmitFailureAttribution();
    TestSequentialReaderEarlyError();
    TestSequentialReaderEarlyEof();
    TestSequentialReaderCleanEof();
    TestRealBackend();
#if defined(__linux__)
    TestPosixPoolAlignedDirectIo();
    TestUringFallbackCounter();
#endif
}
