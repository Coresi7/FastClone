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
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

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

}  // namespace

void RunComparePipelineTests() {
    TestBatchEnqueueParallelDrain();
    TestStopJoinNoHang();
    TestProbeExceptionFallback();
    TestBoundedInFlight();
    TestCountersConsistency();
}
