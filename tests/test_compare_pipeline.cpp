// Unit tests for the shared compare-pipeline component (fastcheck-compare-pipeline FR-21 / AC-28 /
// AC-33). Uses a pure in-memory injected probe double -- no filesystem, no socket, no platform frame
// channel -- so it links only against compare_pipeline.cpp + compare_phase.cpp + file_index.cpp.

#include "compare_pipeline.h"

#include "compare_phase.h"
#include "file_index.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

using namespace fc;

namespace {

void Expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error("test_compare_pipeline: " + message);
    }
}

FileEntry MakeRemote(const std::string& rel, uint64_t size, int64_t mtimeNs) {
    FileEntry e;
    e.relativePath = rel;
    e.isDirectory = false;
    e.fileSize = size;
    e.mtimeNs = mtimeNs;
    return e;
}

// A scripted probe backed by an immutable map (built before the workers start, then only read), so it
// is a thread-safe read used concurrently by all workers.
struct ScriptedProbe {
    std::unordered_map<std::string, std::optional<FileEntry>> table;
    std::optional<FileEntry> operator()(const std::string& rel) const {
        auto it = table.find(rel);
        return (it == table.end()) ? std::nullopt : it->second;
    }
};

// Drive the pipeline to completion: flush, then drain until InFlight()==0. Bounded wait so a bug
// surfaces as a test failure instead of a hang.
std::vector<ComparedItem> RunToCompletion(ComparePipeline& pipeline, std::size_t expected) {
    std::vector<ComparedItem> out;
    pipeline.Flush();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (out.size() < expected) {
        pipeline.Drain(out);
        if (out.size() >= expected) {
            break;
        }
        if (std::chrono::steady_clock::now() > deadline) {
            throw std::runtime_error("test_compare_pipeline: RunToCompletion timed out (possible hang)");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    // One final drain to sweep any late stragglers (there should be none once expected is reached).
    pipeline.Drain(out);
    return out;
}

// Case 1: batch enqueue + parallel probe + drain across all three modes. Each drained outcome must
// equal DecideCompare(mode, scriptedLocal, remote); the set is order-independent, no loss/dup.
void TestBatchEnqueueParallelDrain() {
    for (CompareMode mode : {CompareMode::Fast, CompareMode::Strict, CompareMode::SizeOnly}) {
        constexpr int kN = 500;
        ScriptedProbe probe;
        std::vector<FileEntry> remotes;
        remotes.reserve(kN);
        const int64_t baseMtime = 1700000000000000000LL;
        for (int i = 0; i < kN; ++i) {
            const std::string rel = "f" + std::to_string(i) + ".bin";
            const uint64_t remoteSize = static_cast<uint64_t>(i % 50);
            FileEntry remote = MakeRemote(rel, remoteSize, baseMtime);
            remotes.push_back(remote);
            // Script a varied local: missing / size-equal (mtime hit) / size-equal (mtime miss) / size-diff.
            switch (i % 4) {
                case 0:
                    probe.table[rel] = std::nullopt;  // missing
                    break;
                case 1:
                    probe.table[rel] = MakeRemote(rel, remoteSize, baseMtime);  // size==, mtime hit
                    break;
                case 2:
                    probe.table[rel] = MakeRemote(rel, remoteSize, baseMtime + 5'000'000LL);  // size==, mtime miss (>2ms)
                    break;
                default:
                    probe.table[rel] = MakeRemote(rel, remoteSize + 7, baseMtime);  // size diff
                    break;
            }
        }

        std::atomic<int> readyCalls{0};
        ComparePipelineConfig cfg;
        cfg.mode = mode;
        cfg.workerCount = 4;
        cfg.batchPop = 32;
        ComparePipeline pipeline(cfg, probe, [&]() { readyCalls.fetch_add(1, std::memory_order_relaxed); });

        for (const FileEntry& r : remotes) {
            pipeline.Enqueue(r);
        }
        const std::vector<ComparedItem> items = RunToCompletion(pipeline, kN);

        Expect(items.size() == static_cast<std::size_t>(kN), "case1: drained count == enqueued");
        std::unordered_map<std::string, ComparedItem> byRel;
        for (const ComparedItem& it : items) {
            Expect(byRel.find(it.remote.relativePath) == byRel.end(), "case1: no duplicate results");
            byRel.emplace(it.remote.relativePath, it);
        }
        Expect(byRel.size() == static_cast<std::size_t>(kN), "case1: every rel present exactly once");
        for (const FileEntry& r : remotes) {
            auto it = byRel.find(r.relativePath);
            Expect(it != byRel.end(), "case1: rel found");
            const CompareOutcome expected = DecideCompare(mode, probe(r.relativePath), r);
            Expect(it->second.outcome.needHash == expected.needHash, "case1: needHash matches DecideCompare");
            Expect(it->second.outcome.category == expected.category, "case1: category matches DecideCompare");
        }
        pipeline.Stop();
        pipeline.Join();
        Expect(pipeline.InFlight() == 0, "case1: InFlight() == 0 after full drain");
    }
}

// Case 2: Stop() then Join() must not hang, even with tasks buffered/queued at stop time. Counts stay
// self-consistent: drained <= issued, InFlight() never underflows.
void TestStopJoinNoHang() {
    ScriptedProbe probe;
    for (int i = 0; i < 200; ++i) {
        const std::string rel = "s" + std::to_string(i);
        probe.table[rel] = MakeRemote(rel, 10, 1700000000000000000LL);
    }
    ComparePipelineConfig cfg;
    cfg.mode = CompareMode::Fast;
    cfg.workerCount = 4;
    ComparePipeline pipeline(cfg, probe, nullptr);
    for (int i = 0; i < 200; ++i) {
        pipeline.Enqueue(MakeRemote("s" + std::to_string(i), 10, 1700000000000000000LL));
    }
    pipeline.Flush();
    // Immediately stop without draining everything. Join must return promptly.
    const auto t0 = std::chrono::steady_clock::now();
    pipeline.Stop();
    pipeline.Join();
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    Expect(elapsed < std::chrono::seconds(5), "case2: Stop()+Join() returns promptly (no hang)");

    std::vector<ComparedItem> out;
    pipeline.Drain(out);  // safe after join
    Expect(out.size() <= 200, "case2: no more results than issued");
}

// Case 3: probe that throws -> that item's local == nullopt -> DecideCompare -> Missing. No crash / no
// deadlock (NFR-03).
void TestProbeExceptionFallback() {
    auto throwingProbe = [](const std::string& rel) -> std::optional<FileEntry> {
        if (rel == "boom") {
            throw std::runtime_error("probe blew up");
        }
        return MakeRemote(rel, 5, 1700000000000000000LL);
    };
    ComparePipelineConfig cfg;
    cfg.mode = CompareMode::Fast;
    cfg.workerCount = 3;
    ComparePipeline pipeline(cfg, throwingProbe, nullptr);
    pipeline.Enqueue(MakeRemote("boom", 5, 1700000000000000000LL));
    pipeline.Enqueue(MakeRemote("ok", 5, 1700000000000000000LL));
    const std::vector<ComparedItem> items = RunToCompletion(pipeline, 2);
    Expect(items.size() == 2, "case3: both items produced despite probe exception");
    for (const ComparedItem& it : items) {
        if (it.remote.relativePath == "boom") {
            Expect(!it.local.has_value(), "case3: throwing probe -> local nullopt");
            Expect(it.outcome.category == CompareCategory::Missing && !it.outcome.needHash,
                   "case3: throwing probe -> Missing, no hash");
        }
    }
    pipeline.Stop();
    pipeline.Join();
}

// Case 4: with a caller-side InFlight() gate, in-flight never exceeds the cap, and falls back to 0 after
// draining. Mirrors how FastClone/FastCheck gate the producer.
void TestBoundedInFlight() {
    ScriptedProbe probe;
    constexpr int kN = 2000;
    for (int i = 0; i < kN; ++i) {
        const std::string rel = "b" + std::to_string(i);
        probe.table[rel] = MakeRemote(rel, 8, 1700000000000000000LL);
    }
    ComparePipelineConfig cfg;
    cfg.mode = CompareMode::SizeOnly;
    cfg.workerCount = 4;
    ComparePipeline pipeline(cfg, probe, nullptr);

    constexpr std::size_t kCap = 64;
    std::size_t observedMax = 0;
    std::vector<ComparedItem> out;
    int enqueued = 0;
    while (enqueued < kN || pipeline.InFlight() > 0) {
        // Producer: enqueue while under the cap.
        while (enqueued < kN && pipeline.InFlight() < kCap) {
            pipeline.Enqueue(MakeRemote("b" + std::to_string(enqueued), 8, 1700000000000000000LL));
            ++enqueued;
        }
        pipeline.Flush();
        observedMax = std::max(observedMax, pipeline.InFlight());
        pipeline.Drain(out);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    pipeline.Drain(out);
    Expect(out.size() == static_cast<std::size_t>(kN), "case4: all items drained under bounded gate");
    // The producer only enqueues while InFlight()<cap, so peak in-flight stays within one batch of the
    // cap (cap + one producer burst). Assert it never ran unbounded.
    Expect(observedMax <= kCap + kN, "case4: in-flight observed (sanity)");
    Expect(observedMax <= kCap + 200, "case4: in-flight stayed bounded near the cap");
    Expect(pipeline.InFlight() == 0, "case4: in-flight returns to 0 after full drain");
    pipeline.Stop();
    pipeline.Join();
}

// Case 5: issued / drained / InFlight / HasResults stay self-consistent with enqueue/drain counts.
void TestCountersConsistency() {
    ScriptedProbe probe;
    constexpr int kN = 300;
    for (int i = 0; i < kN; ++i) {
        const std::string rel = "c" + std::to_string(i);
        probe.table[rel] = MakeRemote(rel, 4, 1700000000000000000LL);
    }
    ComparePipelineConfig cfg;
    cfg.mode = CompareMode::Fast;
    cfg.workerCount = 4;
    ComparePipeline pipeline(cfg, probe, nullptr);

    Expect(pipeline.InFlight() == 0, "case5: initial InFlight 0");
    Expect(!pipeline.HasResults(), "case5: initial no results");

    for (int i = 0; i < kN; ++i) {
        pipeline.Enqueue(MakeRemote("c" + std::to_string(i), 4, 1700000000000000000LL));
    }
    Expect(pipeline.InFlight() == static_cast<std::size_t>(kN), "case5: InFlight == issued before drain");

    const std::vector<ComparedItem> items = RunToCompletion(pipeline, kN);
    Expect(items.size() == static_cast<std::size_t>(kN), "case5: drained == issued");
    Expect(pipeline.InFlight() == 0, "case5: InFlight 0 after full drain");
    Expect(!pipeline.HasResults(), "case5: no residual results after full drain");
    pipeline.Stop();
    pipeline.Join();
}

// ---------------------------------------------------------------------------
// SetActiveCap tests: the adaptive concurrency gate mirrors writeActiveCap.
// ---------------------------------------------------------------------------

// A probe that tracks how many workers are inside it simultaneously.
// Uses shared_ptr<atomic> so the struct is copy-constructible (required by std::function).
struct ConcurrencyTrackingProbe {
    std::shared_ptr<std::atomic<int>> active{std::make_shared<std::atomic<int>>(0)};
    std::shared_ptr<std::atomic<int>> peak{std::make_shared<std::atomic<int>>(0)};
    std::optional<FileEntry> operator()(const std::string& rel) const {
        const int cur = active->fetch_add(1, std::memory_order_acq_rel) + 1;
        int prev = peak->load(std::memory_order_relaxed);
        while (cur > prev &&
               !peak->compare_exchange_weak(prev, cur, std::memory_order_relaxed)) {
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));  // hold the slot
        active->fetch_sub(1, std::memory_order_acq_rel);
        return MakeRemote(rel, 1, 1700000000000000000LL);
    }
};

// Case 6: SetActiveCap(0) = unlimited (default). Multiple workers run concurrently.
void TestActiveCapUnlimited() {
    ConcurrencyTrackingProbe probe;
    ComparePipelineConfig cfg;
    cfg.mode = CompareMode::Fast;
    cfg.workerCount = 4;
    cfg.batchPop = 1;  // small batch so workers contend on probe
    ComparePipeline pipeline(cfg, probe, nullptr);
    pipeline.SetActiveCap(0);  // explicit unlimited

    for (int i = 0; i < 100; ++i) {
        pipeline.Enqueue(MakeRemote("u" + std::to_string(i), 1, 1700000000000000000LL));
    }
    const auto items = RunToCompletion(pipeline, 100);
    Expect(items.size() == 100, "case6: all items drained with unlimited cap");
    Expect(probe.peak->load() >= 2, "case6: unlimited cap allows concurrent probes");
    pipeline.Stop();
    pipeline.Join();
}

// Case 7: SetActiveCap(1) = serialized probes. Peak concurrency must be 1.
void TestActiveCapOne() {
    ConcurrencyTrackingProbe probe;
    ComparePipelineConfig cfg;
    cfg.mode = CompareMode::Fast;
    cfg.workerCount = 4;
    cfg.batchPop = 1;
    ComparePipeline pipeline(cfg, probe, nullptr);
    pipeline.SetActiveCap(1);

    for (int i = 0; i < 100; ++i) {
        pipeline.Enqueue(MakeRemote("c" + std::to_string(i), 1, 1700000000000000000LL));
    }
    const auto items = RunToCompletion(pipeline, 100);
    Expect(items.size() == 100, "case7: all items drained with cap=1");
    Expect(probe.peak->load() == 1, "case7: cap=1 limits concurrency to exactly 1");
    pipeline.Stop();
    pipeline.Join();
}

// Case 8: SetActiveCap(N) bounds concurrency to N.
void TestActiveCapBounded() {
    ConcurrencyTrackingProbe probe;
    ComparePipelineConfig cfg;
    cfg.mode = CompareMode::Fast;
    cfg.workerCount = 8;
    cfg.batchPop = 1;
    ComparePipeline pipeline(cfg, probe, nullptr);
    pipeline.SetActiveCap(3);

    for (int i = 0; i < 200; ++i) {
        pipeline.Enqueue(MakeRemote("b" + std::to_string(i), 1, 1700000000000000000LL));
    }
    const auto items = RunToCompletion(pipeline, 200);
    Expect(items.size() == 200, "case8: all items drained with cap=3");
    Expect(probe.peak->load() <= 3, "case8: cap=3 limits concurrency to <=3");
    Expect(probe.peak->load() >= 2, "case8: cap=3 allows some concurrency");
    pipeline.Stop();
    pipeline.Join();
}

// Case 9: Dynamic cap adjustment (raise mid-run) does not hang or lose items.
void TestActiveCapDynamicRaise() {
    ConcurrencyTrackingProbe probe;
    ComparePipelineConfig cfg;
    cfg.mode = CompareMode::Fast;
    cfg.workerCount = 4;
    cfg.batchPop = 1;
    ComparePipeline pipeline(cfg, probe, nullptr);

    pipeline.SetActiveCap(1);
    for (int i = 0; i < 50; ++i) {
        pipeline.Enqueue(MakeRemote("d" + std::to_string(i), 1, 1700000000000000000LL));
    }
    pipeline.Flush();
    // Raise cap while workers are processing.
    pipeline.SetActiveCap(4);
    for (int i = 50; i < 100; ++i) {
        pipeline.Enqueue(MakeRemote("d" + std::to_string(i), 1, 1700000000000000000LL));
    }
    const auto items = RunToCompletion(pipeline, 100);
    Expect(items.size() == 100, "case9: all items drained with dynamic cap raise");
    pipeline.Stop();
    pipeline.Join();
}

// Case 10: Dynamic cap adjustment (lower mid-run) does not hang or lose items.
void TestActiveCapDynamicLower() {
    ConcurrencyTrackingProbe probe;
    ComparePipelineConfig cfg;
    cfg.mode = CompareMode::Fast;
    cfg.workerCount = 4;
    cfg.batchPop = 1;
    ComparePipeline pipeline(cfg, probe, nullptr);

    pipeline.SetActiveCap(4);
    for (int i = 0; i < 50; ++i) {
        pipeline.Enqueue(MakeRemote("l" + std::to_string(i), 1, 1700000000000000000LL));
    }
    pipeline.Flush();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    // Lower cap while workers may be processing.
    pipeline.SetActiveCap(1);
    for (int i = 50; i < 100; ++i) {
        pipeline.Enqueue(MakeRemote("l" + std::to_string(i), 1, 1700000000000000000LL));
    }
    const auto items = RunToCompletion(pipeline, 100);
    Expect(items.size() == 100, "case10: all items drained with dynamic cap lower");
    pipeline.Stop();
    pipeline.Join();
}

// ---------------------------------------------------------------------------
// Lazy directory cache consistency test: verifies that FindFirstFile data
// (size + mtime) matches GetFileAttributesExW for the same files. This is
// the correctness guarantee that allows the cache to replace per-file stat.
// ---------------------------------------------------------------------------

// Case 11: FindFirstFile/FindNextFile returns identical size and mtime to
// GetFileAttributesExW. This validates the lazy directory cache's data source.
void TestLazyDirCacheConsistency() {
#ifdef _WIN32
    namespace fs = std::filesystem;
    const fs::path tmpDir = fs::temp_directory_path() / "fc_lazy_cache_test";
    fs::create_directories(tmpDir);

    struct TestFile {
        std::string name;
        std::string content;
    };
    const TestFile testFiles[] = {
        {"a.txt", "hello"},
        {"b.bin", std::string(100, 'x')},
        {"c.dat", ""},
        {"d.log", std::string(4096, 'z')},
    };
    for (const auto& f : testFiles) {
        std::ofstream(tmpDir / f.name, std::ios::binary) << f.content;
    }

    // Enumerate with FindFirstFile (lazy cache data source).
    std::unordered_map<std::string, std::pair<uint64_t, int64_t>> findData;
    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW((tmpDir.wstring() + L"\\*").c_str(), &fd);
    Expect(hFind != INVALID_HANDLE_VALUE, "case11: FindFirstFile succeeded");
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) continue;
        const uint64_t size =
            (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        ULARGE_INTEGER mt{};
        mt.LowPart = fd.ftLastWriteTime.dwLowDateTime;
        mt.HighPart = fd.ftLastWriteTime.dwHighDateTime;
        const std::string name = fs::path(fd.cFileName).string();
        findData[name] = std::make_pair(size, static_cast<int64_t>(mt.QuadPart));
    } while (FindNextFileW(hFind, &fd));
    FindClose(hFind);

    Expect(findData.size() == 4, "case11: FindFirstFile found all test files");

    // Cross-check each file against GetFileAttributesExW (the original probe path).
    for (const auto& f : testFiles) {
        const fs::path abs = tmpDir / f.name;
        WIN32_FILE_ATTRIBUTE_DATA data{};
        Expect(GetFileAttributesExW(abs.wstring().c_str(), GetFileExInfoStandard, &data) != 0,
               "case11: GetFileAttributesExW succeeded for " + f.name);
        const uint64_t size =
            (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
        ULARGE_INTEGER mt{};
        mt.LowPart = data.ftLastWriteTime.dwLowDateTime;
        mt.HighPart = data.ftLastWriteTime.dwHighDateTime;
        const int64_t mtime = static_cast<int64_t>(mt.QuadPart);

        auto it = findData.find(f.name);
        Expect(it != findData.end(), "case11: FindFirstFile found " + f.name);
        Expect(it->second.first == size, "case11: size matches for " + f.name);
        Expect(it->second.second == mtime, "case11: mtime matches for " + f.name);
    }

    // Verify expected file sizes.
    Expect(findData["a.txt"].first == 5, "case11: a.txt size 5");
    Expect(findData["b.bin"].first == 100, "case11: b.bin size 100");
    Expect(findData["c.dat"].first == 0, "case11: c.dat size 0");
    Expect(findData["d.log"].first == 4096, "case11: d.log size 4096");

    fs::remove_all(tmpDir);
#else
    // Non-Windows: FindFirstFile is not available; the cache uses
    // fs::directory_iterator which already provides cached metadata.
    // Skip on non-Windows.
#endif
}

}  // namespace

void RunComparePipelineTests() {
    TestBatchEnqueueParallelDrain();
    TestStopJoinNoHang();
    TestProbeExceptionFallback();
    TestBoundedInFlight();
    TestCountersConsistency();
    TestActiveCapUnlimited();
    TestActiveCapOne();
    TestActiveCapBounded();
    TestActiveCapDynamicRaise();
    TestActiveCapDynamicLower();
    TestLazyDirCacheConsistency();
}
