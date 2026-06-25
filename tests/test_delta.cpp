// Pure-function unit tests for the binary delta core (delta.h/.cpp). These cover AC-01..AC-09
// of docs/requirements/binary-delta.md with in-memory old/new buffers and no network/threads.
//
// The strong checksum / signature generation depend on XXH3-128 (delta.cpp -> xxhash). On a
// host without xxhash for MSVC these tests are exercised via the standalone logic driver using
// a local xxhash shim; the CMake / FastCloneTests build links the real libxxhash.

#include "delta.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void Require(bool cond, const char* msg) {
    if (!cond) {
        throw std::runtime_error(std::string("delta: ") + msg);
    }
}

std::vector<uint8_t> RandomBytes(uint64_t n, uint32_t seed) {
    std::vector<uint8_t> v(static_cast<size_t>(n));
    std::mt19937 rng(seed);
    for (auto& b : v) {
        b = static_cast<uint8_t>(rng() & 0xFF);
    }
    return v;
}

// Brute-force weak checksum over X[p..p+L-1] (the AC-01 reference).
fc::delta::RollingWeak WeakBrute(const std::vector<uint8_t>& x, size_t p, uint32_t L) {
    return fc::delta::WeakInit(x.data() + p, L);
}

// Simulated reconstruction: copies come from the local old file, misses are "downloaded"
// (sourced from the authoritative new file). The result must equal newData byte-for-byte.
std::vector<uint8_t> Reconstruct(const fc::delta::DeltaPlan& plan,
                                 const std::vector<uint8_t>& oldData,
                                 const std::vector<uint8_t>& newData) {
    std::vector<uint8_t> out(static_cast<size_t>(plan.newFileBytes), 0);
    for (const auto& c : plan.copies) {
        std::memcpy(out.data() + c.destOffsetNew, oldData.data() + c.srcOffsetOld, c.len);
    }
    for (const auto& m : plan.misses) {
        std::memcpy(out.data() + m.destOffsetNew, newData.data() + m.destOffsetNew, m.len);
    }
    return out;
}

void TestRollingMatchesBruteForce() {
    // V-01 / AC-01: O(1) WeakRoll must equal a full WeakInit over the same window for every
    // position, across random + boundary vectors and several window widths.
    const std::vector<std::vector<uint8_t>> vectors = {
        RandomBytes(4096, 1),
        std::vector<uint8_t>(2048, 0),     // all zero
        std::vector<uint8_t>(2048, 255),   // all max
        RandomBytes(257, 7),
    };
    const std::vector<uint32_t> widths = {1, 2, 17, 256, 2048};
    for (const auto& x : vectors) {
        for (uint32_t L : widths) {
            if (x.size() < L) {
                continue;
            }
            fc::delta::RollingWeak w = fc::delta::WeakInit(x.data(), L);
            Require(w.value() == WeakBrute(x, 0, L).value(), "rolling init mismatch");
            for (size_t p = 0; p + L < x.size(); ++p) {
                fc::delta::WeakRoll(w, x[p], x[p + L], L);
                const fc::delta::RollingWeak ref = WeakBrute(x, p + 1, L);
                Require(w.value() == ref.value(), "rolling step != brute force");
            }
        }
    }
}

void TestBlockSizeHeuristic() {
    // V-02 / AC-02: the §5.1 boundary table.
    Require(fc::delta::ChooseBlockSize(1ULL << 20) == 2048, "1 MiB block size");
    Require(fc::delta::ChooseBlockSize(16ULL << 20) == 4096, "16 MiB block size");
    Require(fc::delta::ChooseBlockSize(128ULL << 20) == 12288, "128 MiB block size");
    Require(fc::delta::ChooseBlockSize(1ULL << 30) == 32768, "1 GiB block size");
    Require(fc::delta::ChooseBlockSize(1ULL << 40) == 131072, "1 TiB block size (clamp)");
    // Strong-length tiers.
    Require(fc::delta::ChooseStrongLen(100) == 8, "small block count strongLen");
    Require(fc::delta::ChooseStrongLen((1u << 16) + 1) == 12, "mid block count strongLen");
    Require(fc::delta::ChooseStrongLen((1u << 22) + 1) == 16, "large block count strongLen");
    // Integer sqrt determinism.
    Require(fc::delta::Isqrt64(0) == 0, "isqrt 0");
    Require(fc::delta::Isqrt64(1048576) == 1024, "isqrt 1M");
    Require(fc::delta::Isqrt64(15) == 3, "isqrt 15");
}

void TestSignatureDeterminism() {
    // V-02 / AC-02: identical input -> byte-identical signatures (FR-10).
    const std::vector<uint8_t> data = RandomBytes(70000, 42);
    const fc::delta::SignatureSet a = fc::delta::GenerateSignatures(data.data(), data.size());
    const fc::delta::SignatureSet b = fc::delta::GenerateSignatures(data.data(), data.size());
    Require(a.fileSize == b.fileSize && a.blockSize == b.blockSize &&
                a.blockCount == b.blockCount && a.strongLen == b.strongLen,
            "signature header not deterministic");
    Require(a.blocks.size() == b.blocks.size(), "signature block count not deterministic");
    for (size_t i = 0; i < a.blocks.size(); ++i) {
        Require(a.blocks[i].weak == b.blocks[i].weak, "weak not deterministic");
        Require(a.blocks[i].strong == b.blocks[i].strong, "strong not deterministic");
    }
}

void TestIdentical() {
    // V-03 / AC-03: old == new -> only the short trailing block (if any) is downloaded; the
    // reconstruction is byte-exact.
    const std::vector<uint8_t> data = RandomBytes(70000, 11);
    const fc::delta::SignatureSet sig = fc::delta::GenerateSignatures(data.data(), data.size());
    const fc::delta::DeltaPlan plan = fc::delta::BuildPlan(sig, data.data(), data.size());
    Require(plan.newFileBytes == data.size(), "newFileBytes");
    Require(plan.downloadBytes < sig.blockSize, "identical: only short tail downloaded");
    Require(Reconstruct(plan, data, data) == data, "identical: reconstruct mismatch");
}

void TestSingleByteOverwrite() {
    // V-04 / AC-04: one changed byte -> only its block is downloaded.
    std::vector<uint8_t> oldData = RandomBytes(70000, 23);
    std::vector<uint8_t> newData = oldData;
    newData[40000] ^= 0xFF;  // flip one byte inside a full interior block
    const fc::delta::SignatureSet sig = fc::delta::GenerateSignatures(newData.data(), newData.size());
    const fc::delta::DeltaPlan plan = fc::delta::BuildPlan(sig, oldData.data(), oldData.size());
    Require(Reconstruct(plan, oldData, newData) == newData, "overwrite: reconstruct mismatch");
    // At most the changed block + the short tail need downloading.
    Require(plan.downloadBytes <= static_cast<uint64_t>(sig.blockSize) * 2,
            "overwrite: downloaded too much");
}

void TestHeadInsert() {
    // V-05 / AC-05: prepend bytes (non-block-aligned) -> rolling re-aligns and recovers most
    // blocks; reconstruction is exact.
    const std::vector<uint8_t> oldData = RandomBytes(70000, 31);
    std::vector<uint8_t> newData = RandomBytes(777, 99);  // inserted prefix
    newData.insert(newData.end(), oldData.begin(), oldData.end());
    const fc::delta::SignatureSet sig = fc::delta::GenerateSignatures(newData.data(), newData.size());
    const fc::delta::DeltaPlan plan = fc::delta::BuildPlan(sig, oldData.data(), oldData.size());
    Require(Reconstruct(plan, oldData, newData) == newData, "head-insert: reconstruct mismatch");
    Require(plan.downloadBytes < plan.newFileBytes, "head-insert: expected some copies");
}

void TestPureAppend() {
    // V-06 / AC-06: append-only -> only the appended region is downloaded.
    const std::vector<uint8_t> oldData = RandomBytes(70000, 53);
    std::vector<uint8_t> newData = oldData;
    const std::vector<uint8_t> tail = RandomBytes(5000, 71);
    newData.insert(newData.end(), tail.begin(), tail.end());
    const fc::delta::SignatureSet sig = fc::delta::GenerateSignatures(newData.data(), newData.size());
    const fc::delta::DeltaPlan plan = fc::delta::BuildPlan(sig, oldData.data(), oldData.size());
    Require(Reconstruct(plan, oldData, newData) == newData, "append: reconstruct mismatch");
    // Download bound: appended bytes + at most the boundary block + tail slack.
    Require(plan.downloadBytes <= tail.size() + static_cast<uint64_t>(sig.blockSize) * 2,
            "append: downloaded too much");
}

void TestCompletelyDifferent() {
    // V-07 / AC-07: unrelated content -> benefit rejected (>= 65%).
    const std::vector<uint8_t> oldData = RandomBytes(70000, 101);
    const std::vector<uint8_t> newData = RandomBytes(70000, 202);
    const fc::delta::SignatureSet sig = fc::delta::GenerateSignatures(newData.data(), newData.size());
    const fc::delta::DeltaPlan plan = fc::delta::BuildPlan(sig, oldData.data(), oldData.size());
    Require(fc::delta::BenefitRejected(plan.downloadBytes, plan.newFileBytes),
            "different content should be benefit-rejected");
    // Even when rejected, the plan must still be byte-correct if reconstructed.
    Require(Reconstruct(plan, oldData, newData) == newData, "different: reconstruct mismatch");
}

void TestReconstructByteExact() {
    // V-08 / AC-08: a correct reconstruction equals new (verify pass); a tampered download
    // byte yields a different buffer (verify fail -> fallback in the engine).
    std::vector<uint8_t> oldData = RandomBytes(70000, 5);
    std::vector<uint8_t> newData = oldData;
    newData[1000] ^= 0x01;
    newData[68000] ^= 0x80;
    const fc::delta::SignatureSet sig = fc::delta::GenerateSignatures(newData.data(), newData.size());
    const fc::delta::DeltaPlan plan = fc::delta::BuildPlan(sig, oldData.data(), oldData.size());
    std::vector<uint8_t> good = Reconstruct(plan, oldData, newData);
    Require(good == newData, "verify-ok reconstruct mismatch");
    if (!plan.misses.empty()) {
        std::vector<uint8_t> tampered = newData;
        tampered[plan.misses.front().destOffsetNew] ^= 0xFF;  // corrupt a downloaded byte
        std::vector<uint8_t> bad = Reconstruct(plan, oldData, tampered);
        Require(bad != newData, "tampered reconstruct should differ (verify fail)");
    }
}

void TestBenefitThresholdBoundary() {
    // V-09 / AC-09: integer threshold T = 0.65, >= rejects.
    Require(!fc::delta::BenefitRejected(64, 100), "64% must continue");
    Require(fc::delta::BenefitRejected(65, 100), "65% must reject");
    Require(!fc::delta::BenefitRejected(6499, 10000), "64.99% must continue");
    Require(fc::delta::BenefitRejected(6500, 10000), "65.00% must reject");
    Require(fc::delta::BenefitRejected(10000, 10000), "100% must reject");
    Require(!fc::delta::BenefitRejected(0, 10000), "0% must continue");
}

}  // namespace

void RunDeltaTests() {
    TestRollingMatchesBruteForce();
    TestBlockSizeHeuristic();
    TestSignatureDeterminism();
    TestIdentical();
    TestSingleByteOverwrite();
    TestHeadInsert();
    TestPureAppend();
    TestCompletelyDifferent();
    TestReconstructByteExact();
    TestBenefitThresholdBoundary();
}
