// Tests for the unified disk IO driver (unified-disk-io-driver C3, FR-12/13/25/26/27/28/29/30,
// AC-09/19/20/21/23/24/35). A deterministic in-memory Mock backend exercises the scheduler
// (fairness, multi-op in flight, backpressure, cancellation, counters); the REAL platform backend
// (Windows IOCP+NO_BUFFERING / Linux io_uring-or-pool / macOS pool) is round-tripped end to end
// through a temp file to prove data correctness incl. the small-file and EOF-tail buffered paths.

#include "disk_io_backend.h"
#include "disk_io_driver.h"
#include "file_index.h"
#include "sync_util.h"

#if defined(__linux__)
// Linux-only: the io_uring runtime probe falls back to the pread/pwrite pool, which must report
// counters().ioUringFallback==1 (AC-11 / B-01). Pulled in here to exercise the pool seeding path.
#include "disk_io_backend_pool.h"
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
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
    // W-01: record the mtime to stamp on a WRITE handle at close (optimize-small-file-write-path).
    void setWriteModifyTime(uint64_t id, int64_t modifyNs) override {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = files_.find(id);
        if (it == files_.end() || it->second.mode != OpKind::Write) {
            return;
        }
        it->second.modifyNs = modifyNs;
        it->second.hasModify = true;
    }
    // W-01: closeFile returns the finalize + close result. Records, per file id, whether an mtime was
    // recorded before close (V-01) and the value (V-01/V-03), so tests can assert ordering + value.
    // `failCloseFinalize_` forces a WRITE close to report failure (V-02b); an unknown id -> false.
    bool closeFile(uint64_t id) override {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = files_.find(id);
        if (it == files_.end()) {
            return false;  // unknown / already closed (W-01 idempotency)
        }
        const bool wasWrite = (it->second.mode == OpKind::Write);
        closedHadMtime_[id] = it->second.hasModify;
        if (it->second.hasModify) {
            closedMtime_[id] = it->second.modifyNs;
        }
        files_.erase(it);
        return !(wasWrite && failCloseFinalize_);
    }
    // Test probes (W-01/V-01/V-02/V-02b/V-03).
    bool closedHadMtime(uint64_t id) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = closedHadMtime_.find(id);
        return it != closedHadMtime_.end() && it->second;
    }
    int64_t closedMtime(uint64_t id) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = closedMtime_.find(id);
        return it != closedMtime_.end() ? it->second : 0;
    }
    void failCloseFinalize(bool f) {
        std::lock_guard<std::mutex> lk(mu_);
        failCloseFinalize_ = f;
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
        int64_t modifyNs = 0;   // W-01: mtime recorded via setWriteModifyTime
        bool hasModify = false;
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
    bool failCloseFinalize_ = false;  // W-01/V-02b: force WRITE close to report finalize failure
    std::unordered_map<uint64_t, bool> closedHadMtime_;   // W-01/V-01/V-02 probe
    std::unordered_map<uint64_t, int64_t> closedMtime_;   // W-01/V-01/V-03 probe
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

void SubmitSingle(DiskIoDriver& drv, IoRequest req) {
    std::vector<IoRequest> batch;
    batch.push_back(std::move(req));
    while (!batch.empty()) {
        if (drv.submit(batch) == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
}

void TestIoDriverConfigDefaults() {
    // fastcheck-smallfile-disk-perf FR-12/AC-18
    const IoDriverConfig cfg{};
    Require(cfg.maxInFlight == 64, "IoDriverConfig default maxInFlight=64");
    Require(cfg.backendConcurrency == 8, "IoDriverConfig default backendConcurrency=8");
}

void TestPerFileWaitIsolation() {
    // fastcheck-smallfile-disk-perf AC-21: completion for file A must not wake file B waiters.
    IoDriverConfig cfg;
    cfg.maxInFlight = 8;
    auto mock = std::make_unique<MockBackend>(cfg);
    MockBackend* raw = mock.get();
    DiskIoDriver drv(cfg, std::move(mock));

    const uint64_t fidA = drv.openFile("a", OpKind::Read, true, 0);
    const uint64_t fidB = drv.openFile("b", OpKind::Read, true, 0);
    raw->setReadData(fidA, RandomBytes(8192, 101u));
    raw->setReadData(fidB, RandomBytes(8192, 202u));

    std::atomic<bool> doneA{false};
    std::atomic<bool> doneB{false};
    std::vector<IoCompletion> aOut;
    std::vector<IoCompletion> bOut;
    std::mutex outMu;

    auto waiter = [&](uint64_t fid, std::atomic<bool>& done, std::vector<IoCompletion>& out) {
        for (int i = 0; i < 40; ++i) {
            drv.waitForFile(fid, 50);
            std::vector<IoCompletion> tmp;
            drv.drainCompletionsForFile(fid, tmp);
            if (!tmp.empty()) {
                std::lock_guard<std::mutex> lk(outMu);
                out = std::move(tmp);
                done.store(true);
                return;
            }
        }
        done.store(true);  // timeout path
    };

    std::thread wa(waiter, fidA, std::ref(doneA), std::ref(aOut));
    std::thread wb(waiter, fidB, std::ref(doneB), std::ref(bOut));

    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    IoRequest reqA;
    reqA.kind = OpKind::Read;
    reqA.fileId = fidA;
    reqA.offset = 0;
    reqA.length = 4096;
    reqA.prio = Prio::Small;
    SubmitSingle(drv, std::move(reqA));

    for (int i = 0; i < 80 && !doneA.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    Require(doneA.load(), "file A waiter completed after file A completion");
    Require(!doneB.load(), "file B waiter not released by file A completion");

    IoRequest reqB;
    reqB.kind = OpKind::Read;
    reqB.fileId = fidB;
    reqB.offset = 0;
    reqB.length = 4096;
    reqB.prio = Prio::Small;
    SubmitSingle(drv, std::move(reqB));

    wa.join();
    wb.join();
    Require(aOut.size() == 1 && aOut[0].fileId == fidA, "file A waiter drained file A completion");
    Require(bOut.size() == 1 && bOut[0].fileId == fidB, "file B waiter drained file B completion");
}

void TestCancelWakesPerFileWaiters() {
    // fastcheck-smallfile-disk-perf AC-22: requestCancel must wake each file's waiter.
    IoDriverConfig cfg;
    cfg.maxInFlight = 0;  // keep requests queued so cancel flushes all as Cancelled completions.
    auto mock = std::make_unique<MockBackend>(cfg);
    DiskIoDriver drv(cfg, std::move(mock));

    const uint64_t fidA = drv.openFile("a", OpKind::Write, false, 0);
    const uint64_t fidB = drv.openFile("b", OpKind::Write, false, 0);

    IoRequest reqA;
    reqA.kind = OpKind::Write;
    reqA.fileId = fidA;
    reqA.offset = 0;
    reqA.length = 4;
    reqA.data = {1, 2, 3, 4};
    SubmitSingle(drv, std::move(reqA));

    IoRequest reqB;
    reqB.kind = OpKind::Write;
    reqB.fileId = fidB;
    reqB.offset = 0;
    reqB.length = 4;
    reqB.data = {5, 6, 7, 8};
    SubmitSingle(drv, std::move(reqB));

    std::vector<IoCompletion> aOut;
    std::vector<IoCompletion> bOut;
    std::atomic<bool> doneA{false};
    std::atomic<bool> doneB{false};

    auto waiter = [&](uint64_t fid, std::vector<IoCompletion>& out, std::atomic<bool>& done) {
        for (int i = 0; i < 40; ++i) {
            drv.waitForFile(fid, 50);
            drv.drainCompletionsForFile(fid, out);
            if (!out.empty()) {
                done.store(true);
                return;
            }
        }
        done.store(true);
    };

    std::thread wa(waiter, fidA, std::ref(aOut), std::ref(doneA));
    std::thread wb(waiter, fidB, std::ref(bOut), std::ref(doneB));
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    drv.requestCancel();
    wa.join();
    wb.join();

    Require(doneA.load() && doneB.load(), "cancel waiters returned");
    Require(aOut.size() == 1 && aOut[0].status == IoStatus::Cancelled && aOut[0].fileId == fidA,
            "file A waiter received Cancelled");
    Require(bOut.size() == 1 && bOut[0].status == IoStatus::Cancelled && bOut[0].fileId == fidB,
            "file B waiter received Cancelled");
}

void TestDrainGlobalVsPerFile() {
    // fastcheck-smallfile-disk-perf AC-23: per-file drain must not be duplicated by global drain.
    IoDriverConfig cfg;
    cfg.maxInFlight = 8;
    auto mock = std::make_unique<MockBackend>(cfg);
    MockBackend* raw = mock.get();
    DiskIoDriver drv(cfg, std::move(mock));

    const uint64_t fidA = drv.openFile("a", OpKind::Read, true, 0);
    const uint64_t fidB = drv.openFile("b", OpKind::Read, true, 0);
    raw->setReadData(fidA, RandomBytes(4096, 123u));
    raw->setReadData(fidB, RandomBytes(4096, 234u));

    IoRequest reqA;
    reqA.kind = OpKind::Read;
    reqA.fileId = fidA;
    reqA.offset = 0;
    reqA.length = 4096;
    reqA.userTag = 11;
    SubmitSingle(drv, std::move(reqA));

    IoRequest reqB;
    reqB.kind = OpKind::Read;
    reqB.fileId = fidB;
    reqB.offset = 0;
    reqB.length = 4096;
    reqB.userTag = 22;
    SubmitSingle(drv, std::move(reqB));

    drv.waitForFile(fidA, 2000);
    drv.waitForFile(fidB, 2000);

    std::vector<IoCompletion> perFileA;
    Require(drv.drainCompletionsForFile(fidA, perFileA) == 1, "per-file drain got file A completion");
    Require(perFileA[0].fileId == fidA && perFileA[0].userTag == 11, "per-file drain consumed A tag");

    std::vector<IoCompletion> global;
    const size_t n = drv.drainCompletions(global);
    Require(n == 1, "global drain gets remaining completion only");
    Require(global[0].fileId == fidB && global[0].userTag == 22, "global drain only returns file B");
    for (const auto& c : global) {
        Require(c.fileId != fidA, "global drain must not duplicate per-file-drained completion");
    }
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
    // before the scheduler (blocked on the same lock) can pick - makes alternation deterministic.
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

// Read the whole file through the driver and return the bytes (helper for the read-open size test).
std::vector<uint8_t> ReadAllViaDriver(DiskIoDriver& drv, const std::string& path, uint64_t size,
                                      uint32_t chunkBytes, uint64_t expectedSize) {
    // Change 3 (fastcheck-redundant-syscall-elim): a read open passes `expectedSize` = the caller's
    // already-known read size (>0) so the Windows backend skips FileSizeOnDisk, or 0 to force the
    // legacy FileSizeOnDisk query path.
    const uint64_t rf = drv.openFile(path, OpKind::Read, /*unbuffered=*/true, expectedSize);
    Require(rf != 0, "known-size read: open read");
    SequentialReader reader(drv, rf, size, chunkBytes, 4);
    std::vector<uint8_t> got;
    for (;;) {
        std::vector<uint8_t> chunk;
        bool ok = true;
        const uint32_t n = reader.next(chunk, ok);
        Require(ok, "known-size read: sequential read error");
        if (n == 0) break;
        got.insert(got.end(), chunk.begin(), chunk.end());
    }
    drv.closeFile(rf);
    return got;
}

// V-13c/V-14 (AC-24/AC-25): a read open with a positive expectedSize (known size, Windows skips
// FileSizeOnDisk) returns byte-identical content to the same file read with expectedSize==0 (unknown
// size, legacy FileSizeOnDisk path), across a <1 MiB buffered file and a >=1 MiB unbuffered file.
void RealBackendReadKnownSize() {
    namespace fs = std::filesystem;
    for (uint64_t size : {uint64_t(300000), uint64_t((2u << 20) + 777)}) {
        const fs::path tmp = fs::temp_directory_path() /
                             ("fc_io_knownsz_" + std::to_string(size) + ".bin");
        const std::string path = tmp.string();
        std::error_code ec;
        fs::remove(tmp, ec);
        const std::vector<uint8_t> payload = RandomBytes(static_cast<size_t>(size), 71u);

        IoDriverConfig cfg;
        cfg.maxInFlight = 8;
        cfg.chunkBytes = 1u << 20;
        {
            DiskIoDriver drv(cfg);
            const uint64_t wf = drv.openFile(path, OpKind::Write, true, size);
            Require(wf != 0, "known-size read: open write");
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
            const size_t count = batch.size();
            while (!batch.empty()) {
                if (drv.submit(batch) == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            std::vector<IoCompletion> comps;
            Require(DrainUntil(drv, count, comps, 15000) == count, "known-size read: write comps");
            drv.closeFile(wf);
        }
        Require(static_cast<uint64_t>(fs::file_size(tmp, ec)) == size, "known-size read: on-disk size");

        DiskIoDriver drvKnown(cfg);
        const std::vector<uint8_t> gotKnown =
            ReadAllViaDriver(drvKnown, path, size, cfg.chunkBytes, /*expectedSize=*/size);
        DiskIoDriver drvUnknown(cfg);
        const std::vector<uint8_t> gotUnknown =
            ReadAllViaDriver(drvUnknown, path, size, cfg.chunkBytes, /*expectedSize=*/0);

        Require(gotKnown == payload, "known-size read (expectedSize>0) bytes match payload (AC-24)");
        Require(gotUnknown == payload, "unknown-size read (expectedSize==0) bytes match payload (AC-25)");
        Require(gotKnown == gotUnknown, "known vs unknown expectedSize read bytes identical (AC-24/25)");
        fs::remove(tmp, ec);
    }
}

// unbuffered-writes real-backend round-trip that exercises small-file write sizes AND unaligned
// MIDDLE fragments (delta copy/range shape). Verifies byte-exactness and the D-02/D-03 counters.
void RealBackendSmallSizes() {
    namespace fs = std::filesystem;
    // V-11 (AC-12): 1B / 4097B / (1MiB-1)B write files round-trip byte-exact at the exact size,
    // and (D-02) a small WRITE open with unbuffered intent is NOT downgraded via smallFileFallback.
    for (uint64_t size : {uint64_t(1), uint64_t(4097), uint64_t((1u << 20) - 1)}) {
        const fs::path tmp = fs::temp_directory_path() /
                             ("fc_ubw_small_" + std::to_string(size) + ".bin");
        const std::string path = tmp.string();
        std::error_code ec;
        fs::remove(tmp, ec);
        const std::vector<uint8_t> payload = RandomBytes(static_cast<size_t>(size), 91u);
        IoDriverConfig cfg;
        cfg.chunkBytes = 1u << 20;
        DiskIoDriver drv(cfg);
        const uint64_t wf = drv.openFile(path, OpKind::Write, /*unbuffered=*/true, size);
        Require(wf != 0, "ubw small: open write");
        // optimize-small-file-write-path W-02 (V-04/V-05, AC-W02): on Windows a small WRITE
        // (known size < kSmallFileBufferedMax) with unbuffered intent now opens the buffered handle
        // ONLY -- so it bumps smallFileFallback exactly once and never takes a direct-IO op. On POSIX
        // the write small-file gate stays read-only (D-02): small writes remain unbuffered, so no
        // smallFileFallback bump. Both are asserted at their exact expected values.
#if defined(_WIN32)
        Require(drv.counters().smallFileFallback == 1,
                "ubw small: Windows small write opens buffered-only (W-02 smallFileFallback==1)");
        Require(drv.counters().directIo == 0,
                "ubw small: Windows small write takes no direct IO (W-02)");
#else
        Require(drv.counters().smallFileFallback == 0,
                "ubw small: POSIX write small-file gate is read-only (D-02)");
#endif
        std::vector<IoRequest> batch(1);
        batch[0].kind = OpKind::Write;
        batch[0].fileId = wf;
        batch[0].offset = 0;
        batch[0].length = static_cast<uint32_t>(size);
        batch[0].data = payload;
        while (!batch.empty()) {
            if (drv.submit(batch) == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        std::vector<IoCompletion> comps;
        Require(DrainUntil(drv, 1, comps, 15000) == 1, "ubw small: write completion");
        Require(comps[0].status != IoStatus::Error, "ubw small: write op error");
        drv.closeFile(wf);
        Require(static_cast<uint64_t>(fs::file_size(tmp, ec)) == size, "ubw small: on-disk size");

        DiskIoDriver rdrv(cfg);
        const uint64_t rf = rdrv.openFile(path, OpKind::Read, /*unbuffered=*/true, size);
        Require(rf != 0, "ubw small: open read");
        SequentialReader reader(rdrv, rf, size, cfg.chunkBytes, 4);
        std::vector<uint8_t> got;
        for (;;) {
            std::vector<uint8_t> chunk;
            bool ok = true;
            const uint32_t n = reader.next(chunk, ok);
            Require(ok, "ubw small: read error");
            if (n == 0) break;
            got.insert(got.end(), chunk.begin(), chunk.end());
        }
        rdrv.closeFile(rf);
        Require(got == payload, "ubw small: round-trip bytes mismatch (AC-12)");
        fs::remove(tmp, ec);
    }
}

// unbuffered-writes D-03 (R-01) / V-08: random-access temp shape with an aligned region, an
// unaligned MIDDLE fragment, and an aligned EOF region. The unaligned middle write MUST NOT clobber
// its neighbouring bytes (the old win bug AlignUp+zero-padded a middle op past its logical range).
// Byte-exactness is asserted unconditionally across all three platform backends.
void TestUnbufferedRandomWriteNoPollution() {
    namespace fs = std::filesystem;
    IoDriverConfig cfg;
    cfg.chunkBytes = 1u << 20;
    DiskIoDriver drv(cfg);

    const fs::path tmp = fs::temp_directory_path() / "fc_ubw_random.bin";
    const std::string path = tmp.string();
    std::error_code ec;
    fs::remove(tmp, ec);

    const uint32_t g = drv.queryAlign(path).ioGranularity;
    Require(g >= 2, "ubw random: granularity");
    const uint64_t size = static_cast<uint64_t>(g) * 3;  // aligned EOF at 3*g
    const std::vector<uint8_t> payload = RandomBytes(static_cast<size_t>(size), 77u);

    const uint64_t wf = drv.openFile(path, OpKind::Write, /*unbuffered=*/true, size);
    Require(wf != 0, "ubw random: open write");

    // Four non-overlapping ops that together cover [0,3g): op0 aligned sector 0; op1 aligned-offset
    // but sub-sector LENGTH middle fragment; op2 unaligned-offset middle fragment completing sector
    // 1; op3 aligned EOF sector 2. No sector is written by both the unbuffered and buffered handle.
    struct Seg { uint64_t off; uint32_t len; };
    const std::vector<Seg> segs = {
        {0, g},
        {g, g / 2},
        {static_cast<uint64_t>(g) + g / 2, g - g / 2},
        {static_cast<uint64_t>(g) * 2, g},
    };
    for (const Seg& s : segs) {
        std::vector<IoRequest> one(1);
        one[0].kind = OpKind::Write;
        one[0].fileId = wf;
        one[0].offset = s.off;
        one[0].length = s.len;
        one[0].data.assign(payload.begin() + static_cast<std::ptrdiff_t>(s.off),
                           payload.begin() + static_cast<std::ptrdiff_t>(s.off + s.len));
        while (!one.empty()) {
            if (drv.submit(one) == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    std::vector<IoCompletion> comps;
    Require(DrainUntil(drv, segs.size(), comps, 15000) == segs.size(), "ubw random: completions");
    for (const auto& c : comps) {
        Require(c.status != IoStatus::Error, "ubw random: write op error");
    }
    drv.closeFile(wf);
    Require(static_cast<uint64_t>(fs::file_size(tmp, ec)) == size, "ubw random: on-disk size");

    DiskIoDriver rdrv(cfg);
    const uint64_t rf = rdrv.openFile(path, OpKind::Read, /*unbuffered=*/false, size);
    Require(rf != 0, "ubw random: open read");
    SequentialReader reader(rdrv, rf, size, cfg.chunkBytes, 4);
    std::vector<uint8_t> got;
    for (;;) {
        std::vector<uint8_t> chunk;
        bool ok = true;
        const uint32_t n = reader.next(chunk, ok);
        Require(ok, "ubw random: read error");
        if (n == 0) break;
        got.insert(got.end(), chunk.begin(), chunk.end());
    }
    rdrv.closeFile(rf);
    // The whole file must equal the payload: if the unaligned middle op zero-padded past its range
    // (the old bug), the neighbouring bytes in sector 1 would differ here.
    Require(got == payload, "ubw random: unaligned middle write clobbered neighbour (D-03)");
    fs::remove(tmp, ec);
}

// unbuffered-writes: with the intent OFF, no op may take the unbuffered/direct path (M5 - off is
// equivalent to buffered), and reads still honour the small-file downgrade gate.
void TestUnbufferedOffAndReadGate() {
    namespace fs = std::filesystem;
    IoDriverConfig cfg;
    cfg.chunkBytes = 1u << 20;
    const fs::path tmp = fs::temp_directory_path() / "fc_ubw_off.bin";
    const std::string path = tmp.string();
    std::error_code ec;
    fs::remove(tmp, ec);
    const uint64_t size = 2u << 20;  // 2 MiB, aligned: would go direct if intent were on
    const std::vector<uint8_t> payload = RandomBytes(static_cast<size_t>(size), 55u);

    {
        DiskIoDriver drv(cfg);
        const uint64_t wf = drv.openFile(path, OpKind::Write, /*unbuffered=*/false, size);
        Require(wf != 0, "ubw off: open write");
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
        const size_t count = batch.size();
        while (!batch.empty()) {
            if (drv.submit(batch) == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        std::vector<IoCompletion> comps;
        Require(DrainUntil(drv, count, comps, 15000) == count, "ubw off: write completions");
        drv.closeFile(wf);
        // M5: intent off => never direct IO; every op used the buffered handle.
        Require(drv.counters().directIo == 0, "ubw off: no direct IO when intent off");
    }
    // Read-path small-file gate is retained: a small READ with unbuffered intent still downgrades.
    {
        const fs::path small = fs::temp_directory_path() / "fc_ubw_read_small.bin";
        std::error_code sec;
        fs::remove(small, sec);
        {
            std::ofstream os(small, std::ios::binary);
            const std::vector<uint8_t> tiny = RandomBytes(4096, 22u);
            os.write(reinterpret_cast<const char*>(tiny.data()),
                     static_cast<std::streamsize>(tiny.size()));
        }
        DiskIoDriver drv(cfg);
        const uint64_t rf = drv.openFile(small.string(), OpKind::Read, /*unbuffered=*/true, 0);
        Require(rf != 0, "ubw read gate: open read");
        Require(drv.counters().smallFileFallback >= 1,
                "ubw read gate: small read still downgrades (read gate retained)");
        drv.closeFile(rf);
        fs::remove(small, sec);
    }
    fs::remove(tmp, ec);
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

// ---------------------------------------------------------------------------------------------
// fastcheck-test-coverage-supplement (TC-01)
// ---------------------------------------------------------------------------------------------

// Write `payload` to `path` through a REAL DiskIoDriver (helper for Gap A/C/D). Chunks by
// cfg.chunkBytes, drains all write completions, then closeFile (SetEndOfFile -> exact size).
void WriteViaDriver(const std::string& path, const std::vector<uint8_t>& payload,
                    const IoDriverConfig& cfg) {
    DiskIoDriver drv(cfg);
    const uint64_t wf = drv.openFile(path, OpKind::Write, /*unbuffered=*/true, payload.size());
    Require(wf != 0, "WriteViaDriver: open write");
    std::vector<IoRequest> batch;
    for (uint64_t off = 0; off < payload.size(); off += cfg.chunkBytes) {
        IoRequest r;
        r.kind = OpKind::Write;
        r.fileId = wf;
        r.offset = off;
        const uint64_t n = std::min<uint64_t>(cfg.chunkBytes, payload.size() - off);
        r.data.assign(payload.begin() + static_cast<std::ptrdiff_t>(off),
                      payload.begin() + static_cast<std::ptrdiff_t>(off + n));
        r.length = static_cast<uint32_t>(n);
        batch.push_back(std::move(r));
    }
    const size_t count = batch.size();
    while (!batch.empty()) {
        if (drv.submit(batch) == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::vector<IoCompletion> comps;
    Require(DrainUntil(drv, count, comps, 15000) == count, "WriteViaDriver: write completions");
    for (auto& c : comps) Require(c.status != IoStatus::Error, "WriteViaDriver: write op error");
    drv.closeFile(wf);
}

// -------- Gap A: server hash-miss three-way equivalence (M1 / FR-01..FR-04) --------

// test-only: replicate the hash-miss inline block that used to live in sync_engine_server.cpp
// (removed in the fastcheck-* disk-IO convergence; the old block is preserved in git parent
// 37d32ad of the current HEAD 82666dd). The two constants match the production
// kServerSigChunkBytes(1<<20) / kServerReadAhead(4); the server TU is intentionally NOT included,
// so this helper only calls already-public symbols (SequentialReader / ComputeHashFromSource /
// openFile / closeFile) and introduces no production hook/export/friend/#if branch (FR-04/AC-07).
constexpr uint32_t kOldServerHashChunkBytes = 1u << 20;
constexpr uint32_t kOldServerHashReadAhead = 4;

fc::Hash256 ReconstructOldServerHashMiss(DiskIoDriver& drv, const std::filesystem::path& p) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const uint64_t fileSize = static_cast<uint64_t>(fs::file_size(p, ec));
    Require(!ec, "gapA: file_size");
    // Old block opened the read with expectedSize=0 (unknown size); replicated verbatim.
    const uint64_t fid = drv.openFile(p.string(), OpKind::Read, /*unbuffered=*/true, 0);
    Require(fid != 0, "gapA: open");
    bool rerr = false;
    SequentialReader reader(drv, fid, fileSize, kOldServerHashChunkBytes, kOldServerHashReadAhead);
    std::vector<uint8_t> cbuf;
    size_t cpos = 0;
    // C-01: byte-for-byte identical to the removed inline `src` pull loop.
    auto src = [&](uint8_t* dst, size_t maxLen) -> size_t {
        size_t written = 0;
        while (written < maxLen) {
            if (cpos < cbuf.size()) {
                const size_t take = std::min<size_t>(cbuf.size() - cpos, maxLen - written);
                std::memcpy(dst + written, cbuf.data() + cpos, take);
                cpos += take;
                written += take;
                continue;
            }
            bool okr = true;
            cbuf.clear();
            cpos = 0;
            const uint32_t n = reader.next(cbuf, okr);
            if (!okr) {
                rerr = true;
                break;
            }
            if (n == 0) {
                break;
            }
        }
        return written;
    };
    const fc::Hash256 h = fc::ComputeHashFromSource(src);
    drv.closeFile(fid);
    Require(!rerr, "gapA: read");
    return h;
}

// FR-01/FR-02/FR-03 (AC-02..AC-05): for every size in the matrix, the old inline hash-miss path,
// ComputeFileHashViaDriver and ComputeFileHash must all produce the identical Hash256. All reads go
// through a REAL DiskIoDriver against a Windows temp file (no mock, M5/D-05).
//
// FR-19 (redundant-syscall elimination): the SAME matrix also asserts the new
// ComputeFileHashViaDriver(drv, path, knownSize) overload (caller threads the already-probed
// size) yields a byte-identical Hash256 to (a) the no-knownSize fallback path and (b) the inline
// ComputeFileHash ground truth. This pins the optimization to be result-preserving and proves the
// known-size branch does not silently diverge from the legacy size-probe branch.
void TestServerHashMissThreeWayEquivalence() {
    namespace fs = std::filesystem;
    const uint64_t sizes[] = {0,       1,       64,      4096,    65535,   65536,  262143,
                              262144,  262145,  1048576, 1048577, 5242880, 5000003};
    IoDriverConfig cfg;
    cfg.maxInFlight = 8;
    cfg.chunkBytes = 1u << 20;
    DiskIoDriver drv(cfg);  // real platform backend, shared by ViaDriver + old-inline reconstruction

    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path dir = fs::temp_directory_path() / ("fc_gapA_" + std::to_string(stamp));
    std::error_code ec;
    fs::create_directories(dir, ec);

    for (uint64_t size : sizes) {
        const fs::path p = dir / ("blob_" + std::to_string(size) + ".bin");
        const std::vector<uint8_t> payload =
            RandomBytes(static_cast<size_t>(size), 4200u + static_cast<uint32_t>(size));
        {
            std::ofstream os(p, std::ios::binary | std::ios::trunc);
            if (size > 0) {
                os.write(reinterpret_cast<const char*>(payload.data()),
                         static_cast<std::streamsize>(payload.size()));
            }
        }
        Require(static_cast<uint64_t>(fs::file_size(p, ec)) == size, "gapA: on-disk size");

        const fc::Hash256 inlineHash = fc::ComputeFileHash(p);
        const fc::Hash256 viaDriver = fc::ComputeFileHashViaDriver(drv, p);
        const fc::Hash256 viaDriverKnown = fc::ComputeFileHashViaDriver(drv, p, size);
        const fc::Hash256 oldServer = ReconstructOldServerHashMiss(drv, p);

        Require(fc::HashEquals(inlineHash, viaDriver),
                "gapA: ComputeFileHashViaDriver == ComputeFileHash size " + std::to_string(size));
        Require(fc::HashEquals(inlineHash, oldServer),
                "gapA: old-server-inline == ComputeFileHash size " + std::to_string(size));
        Require(fc::HashEquals(viaDriver, oldServer),
                "gapA: old-server-inline == ComputeFileHashViaDriver size " + std::to_string(size));
        // FR-19: the known-size overload must be result-identical to both the fallback (no-knownSize)
        // path and the inline ground truth across the full size matrix.
        Require(fc::HashEquals(inlineHash, viaDriverKnown),
                "gapA(FR-19): known-size path == ComputeFileHash size " + std::to_string(size));
        Require(fc::HashEquals(viaDriver, viaDriverKnown),
                "gapA(FR-19): known-size path == fallback path size " + std::to_string(size));
        fs::remove(p, ec);
    }
    fs::remove_all(dir, ec);
}

// -------- Gap C: read openFile expectedSize contract (M3 / FR-09..FR-12) --------

// FR-09/FR-10/FR-12 (AC-13..AC-15/AC-17): across the kSmallFileBufferedMax(1 MiB) boundary sizes,
// reading with expectedSize==size (known-size policy) and expectedSize==0 (legacy unknown-size
// policy) both return byte-identical content. ReadAllViaDriver loops to a clean EOF and closeFile's
// the read fileId (AC-17).
void TestReadOpenExpectedSizeContract() {
    namespace fs = std::filesystem;
    IoDriverConfig cfg;
    cfg.maxInFlight = 8;
    cfg.chunkBytes = 1u << 20;
    const uint64_t sizes[] = {(1u << 20) - 1, (1u << 20), (1u << 20) + 1};
    for (uint64_t size : sizes) {
        const fs::path tmp =
            fs::temp_directory_path() / ("fc_gapC_" + std::to_string(size) + ".bin");
        const std::string path = tmp.string();
        std::error_code ec;
        fs::remove(tmp, ec);
        const std::vector<uint8_t> payload = RandomBytes(static_cast<size_t>(size), 131u);
        WriteViaDriver(path, payload, cfg);
        Require(static_cast<uint64_t>(fs::file_size(tmp, ec)) == size, "gapC: on-disk size");

        DiskIoDriver drvKnown(cfg);
        const std::vector<uint8_t> gotKnown =
            ReadAllViaDriver(drvKnown, path, size, cfg.chunkBytes, /*expectedSize=*/size);
        DiskIoDriver drvUnknown(cfg);
        const std::vector<uint8_t> gotUnknown =
            ReadAllViaDriver(drvUnknown, path, size, cfg.chunkBytes, /*expectedSize=*/0);

        Require(gotKnown.size() == size, "gapC: expectedSize==size byte count matches");
        Require(gotUnknown.size() == size, "gapC: expectedSize==0 byte count matches");
        Require(gotKnown == payload, "gapC: expectedSize==size bytes == file content (AC-13/14/15)");
        Require(gotUnknown == payload, "gapC: expectedSize==0 bytes == file content (AC-13/14/15)");
        Require(gotKnown == gotUnknown, "gapC: known vs unknown expectedSize identical bytes");
        fs::remove(tmp, ec);
    }
}

// FR-11 (AC-16): expectedSize is a POLICY HINT, not the source of truth for content length. A read
// open with an OVER-estimated expectedSize (real size + delta, crossing the small-file gate) still
// returns byte-identical content, because SequentialReader plans by the real size and never reads
// past EOF.
void TestReadOpenExpectedSizeMismatch() {
    namespace fs = std::filesystem;
    IoDriverConfig cfg;
    cfg.maxInFlight = 8;
    cfg.chunkBytes = 1u << 20;
    const uint64_t size = 300000;                       // < 1 MiB real content
    const uint64_t mismatchExpected = size + (2u << 20);  // over-estimate -> crosses small-file gate
    const fs::path tmp = fs::temp_directory_path() / "fc_gapC_mismatch.bin";
    const std::string path = tmp.string();
    std::error_code ec;
    fs::remove(tmp, ec);
    const std::vector<uint8_t> payload = RandomBytes(static_cast<size_t>(size), 137u);
    WriteViaDriver(path, payload, cfg);
    Require(static_cast<uint64_t>(fs::file_size(tmp, ec)) == size, "gapC mismatch: on-disk size");

    DiskIoDriver drv(cfg);
    // SequentialReader plans by the REAL size; the mismatched expectedSize only steers the openFile
    // buffered/unbuffered policy, not the content length.
    const std::vector<uint8_t> got =
        ReadAllViaDriver(drv, path, size, cfg.chunkBytes, /*expectedSize=*/mismatchExpected);
    Require(got == payload,
            "gapC: expectedSize is a policy hint, not a content-length source of truth; the read "
            "returns the real file bytes (AC-16)");
    fs::remove(tmp, ec);
}

// -------- Gap D: QueryAlign cache + concurrent open/read integration (M4 / FR-13..FR-15) --------

// FR-13/FR-14/FR-15 (AC-18/AC-19/AC-20): a shared REAL DiskIoDriver on one volume, 16 threads x 32
// files. Each thread round-robins files and per iteration: queryAlign -> real read open -> full
// SequentialReader read-back -> byte-exact content verify -> closeFile. C-02 fixes threads=16 /
// files=32 as lower bounds; only the iteration count is tunable.
void TestConcurrentQueryAlignAndReadIntegration() {
    namespace fs = std::filesystem;
    const int kFiles = 32;
    const int kThreads = 16;
    const int kIters = 64;

    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path dir = fs::temp_directory_path() / ("fc_gapD_" + std::to_string(stamp));
    std::error_code ec;
    fs::create_directories(dir, ec);

    IoDriverConfig cfg;
    cfg.maxInFlight = 64;
    cfg.chunkBytes = 1u << 20;

    std::vector<std::vector<uint8_t>> contents(kFiles);
    std::vector<std::string> paths(kFiles);
    for (int i = 0; i < kFiles; ++i) {
        // Distinct deterministic content + mixed sizes; every 8th file is >= 1 MiB to cover the
        // unbuffered read path alongside small buffered files.
        const size_t size = (i % 8 == 0) ? ((1u << 20) + static_cast<size_t>(i) * 331u)
                                          : (4096u + static_cast<size_t>(i) * 997u);
        contents[i] = RandomBytes(size, 9000u + static_cast<uint32_t>(i));
        paths[i] = (dir / ("f_" + std::to_string(i) + ".bin")).string();
        WriteViaDriver(paths[i], contents[i], cfg);
    }

    DiskIoDriver drv(cfg);  // shared across all 16 threads
    std::atomic<int> reads{0};
    std::atomic<int> closes{0};
    std::atomic<int> openFailures{0};
    std::atomic<bool> mismatch{false};

    auto worker = [&](int t) {
        for (int k = 0; k < kIters; ++k) {
            const int i = (t + k) % kFiles;
            const uint64_t size = contents[i].size();
            (void)drv.queryAlign(paths[i]);
            const uint64_t rf = drv.openFile(paths[i], OpKind::Read, /*unbuffered=*/true, size);
            if (rf == 0) {
                openFailures.fetch_add(1);
                return;
            }
            SequentialReader reader(drv, rf, size, cfg.chunkBytes, 4);
            std::vector<uint8_t> got;
            bool readOk = true;
            for (;;) {
                std::vector<uint8_t> chunk;
                bool ok = true;
                const uint32_t n = reader.next(chunk, ok);
                if (!ok) {
                    readOk = false;
                    break;
                }
                if (n == 0) break;
                got.insert(got.end(), chunk.begin(), chunk.end());
            }
            drv.closeFile(rf);
            closes.fetch_add(1);
            if (!readOk || got != contents[i]) {
                mismatch.store(true);
                return;
            }
            reads.fetch_add(1);
        }
    };

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) threads.emplace_back(worker, t);
    for (auto& th : threads) th.join();

    Require(openFailures.load() == 0, "gapD: no read-open failure across 16 threads (AC-18)");
    Require(!mismatch.load(), "gapD: concurrent read content verified byte-exact (AC-18/AC-19)");
    Require(reads.load() == kThreads * kIters, "gapD: every read fully verified (AC-18/AC-20)");
    Require(closes.load() == kThreads * kIters, "gapD: every read fileId closed (AC-20)");

    fs::remove_all(dir, ec);
}

// Non-ASCII (Chinese) path round-trip through the REAL disk IO driver + the production
// ComputeFileHashViaDriver code path (Chinese-source-directory fix). The disk IO backend decodes
// path std::strings as UTF-8 (CP_UTF8); production now hands it fc::PathToUtf8(path) instead of
// path.string() (CP_ACP). On CP_ACP=936 a path.string() revert would open a garbled filename and
// fail to read back the bytes / produce a matching hash; on CP_ACP=65001 path.string() already
// yields UTF-8 so the test stays green either way (the bug does not manifest there). The path bytes
// are built from \x escapes (pure-ASCII source) and widened via CP_UTF8, so construction is also
// codepage-independent. This is the end-to-end regression guard that activates on a 936 CI runner.
void TestNonAsciiPathIoRoundTrip() {
    namespace fs = std::filesystem;
    // "中文目录" / "文件.txt" as UTF-8 byte escapes (see test_sync_util.cpp for the codepoints).
    const std::string kDir = "\xe4\xb8\xad\xe6\x96\x87\xe7\x9b\xae\xe5\xbd\x95";
    const std::string kFile = "\xe6\x96\x87\xe4\xbb\xb6.txt";
    auto pathFromUtf8 = [&](const std::string& u8) -> fs::path {
#ifdef _WIN32
        return fs::path(fc::Utf8ToWide(u8));
#else
        return fs::path(u8);
#endif
    };

    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path root = fs::temp_directory_path() / ("fc_nonascii_" + std::to_string(stamp));
    std::error_code ec;
    fs::create_directories(root, ec);
    const fs::path dir = root / pathFromUtf8(kDir);
    fs::create_directories(dir, ec);
    Require(fs::is_directory(dir), "nonascii: created Chinese-named directory");

    const fs::path file = dir / pathFromUtf8(kFile);
    const std::string pathUtf8 = fc::PathToUtf8(file);
    const std::vector<uint8_t> payload = RandomBytes(200000, 314u);  // spans buffered + unbuffered

    IoDriverConfig cfg;
    cfg.maxInFlight = 8;
    cfg.chunkBytes = 1u << 20;

    // Write through the real backend using the same UTF-8 path shape production now uses.
    WriteViaDriver(pathUtf8, payload, cfg);
    Require(static_cast<uint64_t>(fs::file_size(file, ec)) == payload.size(),
            "nonascii: on-disk size via Chinese-named path");

    // Read back through the real backend and verify byte-exact content.
    {
        DiskIoDriver drv(cfg);
        const std::vector<uint8_t> got =
            ReadAllViaDriver(drv, pathUtf8, payload.size(), cfg.chunkBytes, /*expectedSize=*/payload.size());
        Require(got == payload, "nonascii: real-backend read round-trips bytes through Chinese path");
    }

    // Production code-path guard: ComputeFileHashViaDriver (file_index.cpp now calls PathToUtf8)
    // must match ComputeFileHash (ground truth: CreateFileW + path.wstring(), CP_ACP-independent).
    DiskIoDriver drv(cfg);
    const fc::Hash256 viaDriver = fc::ComputeFileHashViaDriver(drv, file);
    const fc::Hash256 ground = fc::ComputeFileHash(file);
    Require(fc::HashEquals(viaDriver, ground),
            "nonascii: ComputeFileHashViaDriver == ComputeFileHash through Chinese-named path");
    // And the driver path must itself be openable via the UTF-8 form (exercises queryAlign + openFile
    // on a non-ASCII path, the exact pair the fix touched in file_index.cpp).
    Require(drv.queryAlign(pathUtf8).ioGranularity > 0,
            "nonascii: queryAlign resolves a Chinese-named UTF-8 path");

    fs::remove_all(root, ec);
}

// -------- optimize-small-file-write-path: W-01 close/mtime, W-04 zero-byte, W-05 fast path --------

// Fixed FILETIME-tick mtime (~2020-11). Below kLikelyUnixNsThreshold (5e17) so the Windows backend
// passes it through unchanged; ReadFileMtimeCanonical returns ticks on Windows -> exact compare
// (V-01/V-03/V-05). On POSIX this is a FILETIME-ticks value from a (synthetic) Windows peer, so the
// write paths normalize it to Unix ns before stamping; RequireMtimeMatches/RequireMtimeYearIsSane
// below verify the on-disk result in Unix ns.
constexpr int64_t kTestMtimeTicks = 132500000000000000LL;

// Convert a canonical mtime (what ReadFileMtimeCanonical returns) to whole seconds since the Unix
// epoch, INDEPENDENT of NormalizeManifestMtimeToUnixNs. Windows canonical = FILETIME ticks (100ns
// since 1601); POSIX canonical = Unix ns. Used only by the year-sanity guard so that a reversed
// normalization direction (writing 1970 or year 7375) is caught without coupling to the function
// under test.
int64_t CanonicalMtimeToUnixSeconds(int64_t canonical) {
    constexpr int64_t kWindowsEpochDiff100ns = 116444736000000000LL;
#if defined(_WIN32)
    if (canonical < kWindowsEpochDiff100ns) {
        return canonical / 10000000LL;  // pre-1970, approximate (not used by the 2020 guard)
    }
    return (canonical - kWindowsEpochDiff100ns) / 10000000LL;
#else
    return canonical / 1000000000LL;
#endif
}

// Assert an on-disk mtime matches what we asked for. `wantNs` is a manifest mtime (Unix ns OR Windows
// FILETIME ticks). Exact on Windows (FILETIME ticks round-trip via ToFileTimeFromNs); on POSIX the
// write path normalizes ticks->Unix ns before stamping, so the expected on-disk value is
// NormalizeManifestMtimeToUnixNs(wantNs) and we allow a couple seconds tolerance for ns quantisation.
void RequireMtimeMatches(const std::filesystem::path& p, int64_t wantNs, const std::string& what) {
    const int64_t got = fc::ReadFileMtimeCanonical(p);
#if defined(_WIN32)
    Require(got == wantNs, what + " (exact mtime)");
#else
    const int64_t expected = fc::NormalizeManifestMtimeToUnixNs(wantNs);
    const int64_t diff = got > expected ? got - expected : expected - got;
    Require(diff < 2000000000LL, what + " (mtime within 2s of normalized manifest value)");
#endif
}

// INDEPENDENT year-sanity guard (does NOT use NormalizeManifestMtimeToUnixNs). A manifest mtime of
// ~2020 must land on disk as a ~2020 date -- never 1970 (ticks passed through as if they were Unix
// ns) and never year 7375 (Unix ns misread as ticks and multiplied). This is the check that catches a
// reversed normalization direction on POSIX; on Windows it is consistent with the exact-ticks path.
void RequireMtimeYearIsSane(const std::filesystem::path& p, const std::string& what) {
    const int64_t got = fc::ReadFileMtimeCanonical(p);
    const int64_t secs = CanonicalMtimeToUnixSeconds(got);
    // 2019-01-01 = 1546300800, 2022-01-01 = 1640995200. kTestMtimeTicks is ~2020-11.
    Require(secs >= 1546300800LL && secs <= 1640995200LL,
            what + " on-disk mtime year is ~2020 (got " + std::to_string(secs) +
            " s = " + std::to_string(secs / 31557600LL + 1970) + "-ish), not 1970 / 7375");
}

// Whole-file write through the REAL driver: openFile(Write) -> submit all bytes -> drain ->
// setWriteModifyTime -> closeFile. Returns the closeFile() bool (W-01 finalize+close result).
bool WriteWholeFileViaDriver(const std::string& path, const std::vector<uint8_t>& payload,
                             int64_t mtimeNs) {
    IoDriverConfig cfg;
    cfg.maxInFlight = 8;
    cfg.chunkBytes = 1u << 20;
    DiskIoDriver drv(cfg);
    const uint64_t wf = drv.openFile(path, OpKind::Write, /*unbuffered=*/true, payload.size());
    Require(wf != 0, "whole-file: open write");
    std::vector<IoRequest> batch;
    for (uint64_t off = 0; off < payload.size(); off += cfg.chunkBytes) {
        IoRequest r;
        r.kind = OpKind::Write;
        r.fileId = wf;
        r.offset = off;
        const uint64_t n = std::min<uint64_t>(cfg.chunkBytes, payload.size() - off);
        r.data.assign(payload.begin() + static_cast<std::ptrdiff_t>(off),
                      payload.begin() + static_cast<std::ptrdiff_t>(off + n));
        r.length = static_cast<uint32_t>(n);
        batch.push_back(std::move(r));
    }
    const size_t count = batch.size();
    while (!batch.empty()) {
        if (drv.submit(batch) == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (count > 0) {
        std::vector<IoCompletion> comps;
        Require(DrainUntil(drv, count, comps, 15000) == count, "whole-file: write completions");
        for (auto& c : comps) Require(c.status != IoStatus::Error, "whole-file: write op error");
    }
    drv.setWriteModifyTime(wf, mtimeNs);
    return drv.closeFile(wf);  // W-01: exact-size truncate + SetFileTime + close, reported as bool
}

std::vector<uint8_t> ReadWholeFile(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
}

// V-01/V-02/V-02b (W-01/FR-01/FR-02/B7): closeFile is a bool; the mtime is recorded BEFORE close and
// only stamped once; a forced finalize failure is reported so the caller can withhold the count.
void TestCloseMtimeContractMock() {
    IoDriverConfig cfg;
    cfg.maxInFlight = 8;

    // V-01: happy path -- setWriteModifyTime is recorded before closeFile, close returns true.
    {
        auto mock = std::make_unique<MockBackend>(cfg);
        MockBackend* raw = mock.get();
        DiskIoDriver drv(cfg, std::move(mock));
        const uint64_t wf = drv.openFile("w", OpKind::Write, false, 8);
        IoRequest r;
        r.kind = OpKind::Write;
        r.fileId = wf;
        r.offset = 0;
        r.data = RandomBytes(8, 5u);
        r.length = 8;
        SubmitSingle(drv, std::move(r));
        std::vector<IoCompletion> comps;
        Require(DrainUntil(drv, 1, comps, 5000) == 1, "V-01: write completion");
        drv.setWriteModifyTime(wf, kTestMtimeTicks);
        Require(drv.closeFile(wf) == true, "V-01: successful close returns true");
        Require(raw->closedHadMtime(wf), "V-01: mtime was recorded before close");
        Require(raw->closedMtime(wf) == kTestMtimeTicks, "V-01: recorded mtime value matches");
    }
    // V-02: a failed content write => the caller never calls setWriteModifyTime, so close records no
    // mtime (mtime must not be stamped on a write that did not fully succeed).
    {
        auto mock = std::make_unique<MockBackend>(cfg);
        MockBackend* raw = mock.get();
        DiskIoDriver drv(cfg, std::move(mock));
        const uint64_t wf = drv.openFile("w", OpKind::Write, false, 8);
        // (simulate write failure: skip setWriteModifyTime entirely)
        Require(drv.closeFile(wf) == true, "V-02: close of an unstamped file still returns true");
        Require(!raw->closedHadMtime(wf), "V-02: no mtime recorded when write did not stamp it");
    }
    // V-02b: a finalize (SetEndOfFile/SetFileTime/CloseHandle-equivalent) failure is surfaced as
    // closeFile()==false, so the write success path can refuse to count the file as transferred.
    {
        auto mock = std::make_unique<MockBackend>(cfg);
        MockBackend* raw = mock.get();
        DiskIoDriver drv(cfg, std::move(mock));
        const uint64_t wf = drv.openFile("w", OpKind::Write, false, 8);
        drv.setWriteModifyTime(wf, kTestMtimeTicks);
        raw->failCloseFinalize(true);
        Require(drv.closeFile(wf) == false, "V-02b: finalize failure => closeFile returns false");
    }
    // W-01 idempotency: closing an unknown/second-time id returns false, never crashes.
    {
        auto mock = std::make_unique<MockBackend>(cfg);
        DiskIoDriver drv(cfg, std::move(mock));
        Require(drv.closeFile(9999) == false, "V-02b: unknown fileId close returns false");
    }
}

// V-01 real backend: a whole-file driver write stamps the exact mtime on the still-open handle and
// closeFile reports true; the on-disk content, size and mtime all match.
void TestCloseMtimeContractReal() {
    namespace fs = std::filesystem;
    const fs::path tmp = fs::temp_directory_path() / "fc_w01_close_mtime.bin";
    std::error_code ec;
    fs::remove(tmp, ec);
    const std::vector<uint8_t> payload = RandomBytes(4096, 71u);
    Require(WriteWholeFileViaDriver(tmp.string(), payload, kTestMtimeTicks),
            "V-01 real: closeFile returns true on success");
    Require(static_cast<uint64_t>(fs::file_size(tmp, ec)) == payload.size(), "V-01 real: exact size");
    Require(ReadWholeFile(tmp) == payload, "V-01 real: bytes match");
    RequireMtimeMatches(tmp, kTestMtimeTicks, "V-01 real: driver close stamps mtime");
    RequireMtimeYearIsSane(tmp, "V-01 real: driver close mtime year ~2020 (WIN->POSIX direction guard)");
    fs::remove(tmp, ec);
}

// V-11 / W-04: a zero-byte file must be creatable purely through the driver write path (openFile ->
// setWriteModifyTime -> closeFile), i.e. what the async worker does -- no main-thread ofstream. The
// result is an empty file with the exact mtime.
void TestZeroByteDriverWrite() {
    namespace fs = std::filesystem;
    const fs::path tmp = fs::temp_directory_path() / "fc_w04_zero.bin";
    std::error_code ec;
    fs::remove(tmp, ec);
    Require(WriteWholeFileViaDriver(tmp.string(), {}, kTestMtimeTicks),
            "V-11: zero-byte driver write closeFile returns true");
    Require(fs::exists(tmp, ec), "V-11: zero-byte file created");
    Require(static_cast<uint64_t>(fs::file_size(tmp, ec)) == 0, "V-11: zero-byte file is empty");
    RequireMtimeMatches(tmp, kTestMtimeTicks, "V-11: zero-byte file mtime stamped");
    fs::remove(tmp, ec);
}

// W-05 (FR-13): ShouldUseSmallFileFastPath boundary + WriteSmallFileFastPath equivalence with the
// driver whole-file path (content/size/mtime), truncating overwrite, and the failure return.
void TestSmallFileFastPath() {
    namespace fs = std::filesystem;
    // Boundary: <= 256 KiB uses the fast path; 256 KiB + 1 does not (design section 3.5.1).
    Require(fc::ShouldUseSmallFileFastPath(0), "W-05: 0 bytes uses fast path");
    Require(fc::ShouldUseSmallFileFastPath(1), "W-05: 1 byte uses fast path");
    Require(fc::ShouldUseSmallFileFastPath(fc::kSmallFileFastPathMax), "W-05: 256 KiB uses fast path");
    Require(!fc::ShouldUseSmallFileFastPath(fc::kSmallFileFastPathMax + 1),
            "W-05: 256 KiB + 1 does NOT use fast path");

    // Equivalence: same payload + mtime via the fast path vs the driver whole-file path must produce
    // byte-identical content, identical size and identical mtime.
    for (uint64_t size : {uint64_t(0), uint64_t(1), uint64_t(4097),
                          fc::kSmallFileFastPathMax}) {
        const std::vector<uint8_t> payload = RandomBytes(static_cast<size_t>(size), 33u);
        const fs::path fastP = fs::temp_directory_path() / ("fc_w05_fast_" + std::to_string(size));
        const fs::path drvP = fs::temp_directory_path() / ("fc_w05_drv_" + std::to_string(size));
        std::error_code ec;
        fs::remove(fastP, ec);
        fs::remove(drvP, ec);

        Require(fc::WriteSmallFileFastPath(fastP, payload.data(), payload.size(), kTestMtimeTicks),
                "W-05: fast path write succeeds");
        Require(WriteWholeFileViaDriver(drvP.string(), payload, kTestMtimeTicks),
                "W-05: driver write succeeds");

        Require(static_cast<uint64_t>(fs::file_size(fastP, ec)) == size, "W-05: fast path exact size");
        Require(static_cast<uint64_t>(fs::file_size(drvP, ec)) == size, "W-05: driver exact size");
        const std::vector<uint8_t> fastBytes = ReadWholeFile(fastP);
        const std::vector<uint8_t> drvBytes = ReadWholeFile(drvP);
        Require(fastBytes == payload, "W-05: fast path bytes == payload");
        Require(fastBytes == drvBytes, "W-05: fast path bytes == driver bytes (equivalence)");
        RequireMtimeMatches(fastP, kTestMtimeTicks, "W-05: fast path mtime");
        RequireMtimeYearIsSane(fastP, "W-05: fast path mtime year ~2020 (WIN->POSIX direction guard)");
        RequireMtimeYearIsSane(drvP, "W-05: driver mtime year ~2020 (WIN->POSIX direction guard)");
        fs::remove(fastP, ec);
        fs::remove(drvP, ec);
    }

    // Truncating overwrite: a fast-path write over a larger existing file leaves the exact new size.
    {
        const fs::path p = fs::temp_directory_path() / "fc_w05_overwrite.bin";
        std::error_code ec;
        const std::vector<uint8_t> big = RandomBytes(200000, 8u);
        Require(fc::WriteSmallFileFastPath(p, big.data(), big.size(), kTestMtimeTicks),
                "W-05: overwrite seed write");
        const std::vector<uint8_t> small = RandomBytes(1234, 9u);
        Require(fc::WriteSmallFileFastPath(p, small.data(), small.size(), kTestMtimeTicks),
                "W-05: overwrite shrink write");
        Require(static_cast<uint64_t>(fs::file_size(p, ec)) == small.size(),
                "W-05: overwrite truncates to exact new size");
        Require(ReadWholeFile(p) == small, "W-05: overwrite content is the new bytes");
        fs::remove(p, ec);
    }

    // Failure return: a target whose parent directory does not exist cannot be created -> false.
    {
        const fs::path bad = fs::temp_directory_path() / "fc_w05_no_such_dir" / "nested" / "f.bin";
        std::error_code ec;
        fs::remove_all(fs::temp_directory_path() / "fc_w05_no_such_dir", ec);
        const std::vector<uint8_t> payload = RandomBytes(16, 1u);
        Require(!fc::WriteSmallFileFastPath(bad, payload.data(), payload.size(), kTestMtimeTicks),
                "W-05: write into a missing parent dir returns false");
    }
}

// S-02 (FR-16 / AC-24/25): SetFileModifyTime must stamp the same mtime as the driver/fast-path write
// helpers. On Windows the manifest carries raw FILETIME ticks; on POSIX the helper normalizes before
// calling into the filesystem (see NormalizeManifestMtimeToUnixNs in file_index.cpp).
void TestSetFileModifyTime() {
    namespace fs = std::filesystem;
    const fs::path tmp = fs::temp_directory_path() / "fc_s02_mtime.bin";
    std::error_code ec;
    fs::remove(tmp, ec);
    {
        std::ofstream out(tmp, std::ios::binary);
        Require(out.good(), "S-02: seed file for mtime-only update");
    }
    fc::SetFileModifyTime(tmp, kTestMtimeTicks);
    RequireMtimeMatches(tmp, kTestMtimeTicks, "S-02: SetFileModifyTime stamps manifest mtime");
    // Independent year-sanity guard (catches a reversed normalization direction on POSIX that the
    // self-consistent RequireMtimeMatches compare above cannot).
    RequireMtimeYearIsSane(tmp, "S-02: SetFileModifyTime (WIN->POSIX) writes ~2020, not 1970/7375");
    fs::remove(tmp, ec);
}

// S-02 integration guard: all THREE POSIX write paths (SetFileModifyTime, WriteSmallFileFastPath,
// driver setWriteModifyTime+closeFile) must stamp a ~2020 on-disk year for a FILETIME-ticks manifest
// input (WIN->POSIX). This is the cross-platform-branch coverage the pure-function direction test
// and the self-consistent RequireMtimeMatches cannot provide: it asserts the actual on-disk year via
// CanonicalMtimeToUnixSeconds, which is independent of NormalizeManifestMtimeToUnixNs. A reversed
// direction would write 1974 (ticks passed through as Unix ns) or year 7375 (Unix ns misread as
// ticks) and fail the 2019-2022 window here.
void TestWritePathsMtimeYearSanity() {
    namespace fs = std::filesystem;
    std::error_code ec;

    // Path 1: WriteSmallFileFastPath.
    {
        const fs::path p = fs::temp_directory_path() / "fc_s02_year_fast.bin";
        fs::remove(p, ec);
        const std::vector<uint8_t> payload = RandomBytes(64, 21u);
        Require(fc::WriteSmallFileFastPath(p, payload.data(), payload.size(), kTestMtimeTicks),
                "S-02 year: fast path write succeeds");
        RequireMtimeYearIsSane(p, "S-02 year: WriteSmallFileFastPath (WIN->POSIX) ~2020");
        fs::remove(p, ec);
    }

    // Path 2: driver whole-file write (setWriteModifyTime + closeFile).
    {
        const fs::path p = fs::temp_directory_path() / "fc_s02_year_drv.bin";
        fs::remove(p, ec);
        const std::vector<uint8_t> payload = RandomBytes(4096, 22u);
        Require(WriteWholeFileViaDriver(p.string(), payload, kTestMtimeTicks),
                "S-02 year: driver write succeeds");
        RequireMtimeYearIsSane(p, "S-02 year: driver write (WIN->POSIX) ~2020");
        fs::remove(p, ec);
    }

    // Path 3: SetFileModifyTime on a pre-existing file.
    {
        const fs::path p = fs::temp_directory_path() / "fc_s02_year_set.bin";
        fs::remove(p, ec);
        {
            std::ofstream out(p, std::ios::binary);
            Require(out.good(), "S-02 year: seed file for SetFileModifyTime");
        }
        fc::SetFileModifyTime(p, kTestMtimeTicks);
        RequireMtimeYearIsSane(p, "S-02 year: SetFileModifyTime (WIN->POSIX) ~2020");
        fs::remove(p, ec);
    }
}

// S-02 regression guard: NormalizeManifestMtimeToUnixNs direction. The threshold 5e17 sits BETWEEN
// modern Unix-ns (~1.7e18, ABOVE 5e17) and modern Windows FILETIME ticks (~1.3e17, BELOW 5e17), so
// the direction is load-bearing. A previous version of this function treated >5e17 as ticks and
// <5e17 as Unix ns (backwards), which wrote year-7375 mtimes on POSIX->POSIX and 1970-01-02 on
// WIN->POSIX. This test pins the correct direction (mirrors compare_phase.cpp::TryNormalizeMtimeToUnixNs)
// on ALL platforms (the helper is pure int64 arithmetic, compiled everywhere for this purpose).
void TestNormalizeManifestMtimeToUnixNsDirection() {
    using fc::NormalizeManifestMtimeToUnixNs;
    // POSIX peer: genuine Unix ns (~2026) must pass through unchanged.
    const int64_t unixNs2026 = 1778000000000000000LL;  // ~2026
    Require(NormalizeManifestMtimeToUnixNs(unixNs2026) == unixNs2026,
            "S-02: Unix ns (>5e17) passes through unchanged (POSIX->POSIX)");

    // Windows peer: FILETIME ticks (~2020, 1.325e17) must convert to Unix ns, NOT pass through.
    const int64_t ticks2020 = 132500000000000000LL;  // ~2020 FILETIME ticks
    const int64_t expect2020Ns = (ticks2020 - 116444736000000000LL) * 100LL;  // ~1.6055e18 ns
    Require(NormalizeManifestMtimeToUnixNs(ticks2020) == expect2020Ns,
            "S-02: FILETIME ticks ([1.16e17,5e17]) convert to Unix ns (WIN->POSIX)");
    Require(expect2020Ns > 1500000000000000000LL && expect2020Ns < 1700000000000000000LL,
            "S-02: WIN->POSIX converted mtime lands near 2018-2024, not year 7375 / 1970-01-02");

    // The 0 "unknown" sentinel and small pre-epoch values pass through (0 stays 0).
    Require(NormalizeManifestMtimeToUnixNs(0) == 0, "S-02: 0 sentinel passes through as 0");
    // Negative clamps to 0.
    Require(NormalizeManifestMtimeToUnixNs(-1) == 0, "S-02: negative clamps to 0");

    // A value just above the threshold is treated as Unix ns (not ticks).
    Require(NormalizeManifestMtimeToUnixNs(500000000000000001LL) == 500000000000000001LL,
            "S-02: just-above-threshold treated as Unix ns");

    // A realistic upper-band ticks value (year ~2100, ~1.57e17, still < 5e17) converts to Unix ns
    // without overflowing int64 (max ~9.2e18). Pins that the ticks branch covers the upper band.
    const int64_t ticks2100 = 157374000000000000LL;
    const int64_t expect2100Ns = (ticks2100 - 116444736000000000LL) * 100LL;
    Require(NormalizeManifestMtimeToUnixNs(ticks2100) == expect2100Ns,
            "S-02: upper-band FILETIME ticks still convert to Unix ns");
    Require(expect2100Ns > 4000000000000000000LL && expect2100Ns < 5000000000000000000LL,
            "S-02: year-2100 ticks map to ~4e18 ns (post-2038, sane)");
}

}  // namespace

void RunDiskIoDriverTests() {
    TestIoDriverConfigDefaults();
    TestPerFileWaitIsolation();
    TestCancelWakesPerFileWaiters();
    TestDrainGlobalVsPerFile();
    TestMockRoundTripAndCounters();
    TestMultiOpInFlight();
    TestFairness();
    TestBackpressure();
    TestCancel();
    TestSubmitFailureAttribution();
    TestSequentialReaderEarlyError();
    TestSequentialReaderEarlyEof();
    TestSequentialReaderCleanEof();
    TestCloseMtimeContractMock();
    TestCloseMtimeContractReal();
    TestZeroByteDriverWrite();
    TestSmallFileFastPath();
    TestSetFileModifyTime();
    TestNormalizeManifestMtimeToUnixNsDirection();
    TestWritePathsMtimeYearSanity();
    TestRealBackend();
    RealBackendReadKnownSize();
    RealBackendSmallSizes();
    TestUnbufferedRandomWriteNoPollution();
    TestUnbufferedOffAndReadGate();
    TestServerHashMissThreeWayEquivalence();
    TestReadOpenExpectedSizeContract();
    TestReadOpenExpectedSizeMismatch();
    TestConcurrentQueryAlignAndReadIntegration();
    TestNonAsciiPathIoRoundTrip();
#if defined(__linux__)
    TestPosixPoolAlignedDirectIo();
    TestUringFallbackCounter();
#endif
}
