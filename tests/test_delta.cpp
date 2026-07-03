// Pure-function unit tests for the binary delta core (delta.h/.cpp). These cover AC-01..AC-09
// of docs/requirements/binary-delta.md with in-memory old/new buffers and no network/threads.
//
// The strong checksum / signature generation depend on XXH3-128 (delta.cpp -> xxhash). On a
// host without xxhash for MSVC these tests are exercised via the standalone logic driver using
// a local xxhash shim; the CMake / FastCloneTests build links the real libxxhash.

#include "delta.h"
#include "file_index.h"  // ComputeBufferHash / Hash256 (streaming full-file hash equivalence)

#include <algorithm>
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

// delta-perf AC-09 / FR-11: a verbatim copy of the PRE-optimization BuildPlan algorithm
// (head-insert chain + std::vector<bool> + no early stop). The optimized BuildPlan (with
// enableEarlyStop=false) must produce byte-for-byte identical output against this reference.
fc::delta::DeltaPlan BuildPlanReference(const fc::delta::SignatureSet& sig,
                                        const uint8_t* oldData, uint64_t oldLen) {
    using namespace fc::delta;
    DeltaPlan plan;
    plan.newFileBytes = sig.fileSize;
    if (sig.blockCount == 0 || sig.blockSize == 0) {
        return plan;
    }
    const uint32_t L = sig.blockSize;
    const uint32_t blockCount = sig.blockCount;
    const uint8_t strongLen = sig.strongLen;
    auto blockLen = [&](uint32_t i) -> uint32_t {
        const uint64_t off = static_cast<uint64_t>(i) * L;
        return static_cast<uint32_t>(std::min<uint64_t>(L, sig.fileSize - off));
    };
    std::vector<bool> matched(blockCount, false);
    std::vector<uint64_t> matchedSrc(blockCount, 0);
    std::vector<int32_t> bucketHead(1u << 16, -1);
    std::vector<int32_t> chainNext(blockCount, -1);
    for (uint32_t i = 0; i < blockCount; ++i) {
        if (blockLen(i) != L) {
            continue;
        }
        const uint16_t b = BucketOf(sig.blocks[i].weak);
        chainNext[i] = bucketHead[b];
        bucketHead[b] = static_cast<int32_t>(i);
    }
    if (oldLen >= L) {
        uint64_t p = 0;
        RollingWeak w = WeakInit(oldData, L);
        while (true) {
            const uint32_t weak32 = w.value();
            const uint16_t bucket = BucketOf(weak32);
            bool matchedHere = false;
            std::array<uint8_t, 16> strong{};
            bool strongComputed = false;
            for (int32_t node = bucketHead[bucket]; node != -1; node = chainNext[node]) {
                const BlockSig& bs = sig.blocks[node];
                if (bs.weak != weak32 || matched[node]) {
                    continue;
                }
                if (!strongComputed) {
                    strong = StrongHash(oldData + p, L);
                    strongComputed = true;
                }
                if (std::equal(strong.begin(), strong.begin() + strongLen, bs.strong.begin())) {
                    matched[node] = true;
                    matchedSrc[node] = p;
                    matchedHere = true;
                    break;
                }
            }
            if (matchedHere) {
                p += L;
                if (p + L > oldLen) {
                    break;
                }
                w = WeakInit(oldData + p, L);
            } else {
                if (p + L >= oldLen) {
                    break;
                }
                WeakRoll(w, oldData[p], oldData[p + L], L);
                ++p;
            }
        }
    }
    uint64_t missRunStart = 0;
    uint64_t missRunBytes = 0;
    bool inMissRun = false;
    auto flushMiss = [&]() {
        if (inMissRun) {
            plan.misses.push_back(MissOp{missRunStart, static_cast<uint32_t>(missRunBytes)});
            plan.downloadBytes += missRunBytes;
            inMissRun = false;
            missRunBytes = 0;
        }
    };
    for (uint32_t i = 0; i < blockCount; ++i) {
        const uint64_t destOff = static_cast<uint64_t>(i) * L;
        const uint32_t blen = blockLen(i);
        if (matched[i]) {
            flushMiss();
            plan.copies.push_back(CopyOp{matchedSrc[i], destOff, blen});
        } else {
            if (!inMissRun) {
                inMissRun = true;
                missRunStart = destOff;
                missRunBytes = 0;
            }
            missRunBytes += blen;
        }
    }
    flushMiss();
    return plan;
}

// Byte-for-byte plan comparison for AC-09: copies, misses, downloadBytes, BenefitRejected.
bool PlansByteEqual(const fc::delta::DeltaPlan& a, const fc::delta::DeltaPlan& b) {
    if (a.downloadBytes != b.downloadBytes || a.newFileBytes != b.newFileBytes) {
        return false;
    }
    if (a.copies.size() != b.copies.size() || a.misses.size() != b.misses.size()) {
        return false;
    }
    for (size_t i = 0; i < a.copies.size(); ++i) {
        if (a.copies[i].srcOffsetOld != b.copies[i].srcOffsetOld ||
            a.copies[i].destOffsetNew != b.copies[i].destOffsetNew ||
            a.copies[i].len != b.copies[i].len) {
            return false;
        }
    }
    for (size_t i = 0; i < a.misses.size(); ++i) {
        if (a.misses[i].destOffsetNew != b.misses[i].destOffsetNew ||
            a.misses[i].len != b.misses[i].len) {
            return false;
        }
    }
    return fc::delta::BenefitRejected(a.downloadBytes, a.newFileBytes) ==
           fc::delta::BenefitRejected(b.downloadBytes, b.newFileBytes);
}

// Number of FULL (length == blockSize) blocks in a signature set.
uint32_t FullBlockCount(const fc::delta::SignatureSet& sig) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < sig.blockCount; ++i) {
        const uint64_t off = static_cast<uint64_t>(i) * sig.blockSize;
        if (sig.fileSize - off >= sig.blockSize) {
            ++n;
        }
    }
    return n;
}

// AC-07 helper: incrementally search for an L-byte window whose bucket equals targetBucket but
// whose 32-bit weak differs from avoidWeak (so the only candidate in that bucket is excluded by
// the weak filter before any strong checksum is computed). O(1) per probe via running s1/s2.
std::vector<uint8_t> FindSameBucketDiffWeak(uint32_t L, uint16_t targetBucket,
                                            uint32_t avoidWeak, uint32_t seed) {
    std::vector<uint8_t> c = RandomBytes(L, seed);
    uint32_t s1 = 0, s2 = 0;
    for (uint32_t j = 0; j < L; ++j) {
        const uint32_t x = c[j];
        s1 += x;
        s2 += (j + 1u) * x;
    }
    std::mt19937 rng(seed ^ 0x9e3779b9u);
    for (int iter = 0; iter < 5'000'000; ++iter) {
        const uint32_t weak = ((s2 & 0xFFFFu) << 16) | (s1 & 0xFFFFu);
        if (fc::delta::BucketOf(weak) == targetBucket && weak != avoidWeak) {
            return c;
        }
        const uint32_t idx = rng() % L;
        const uint8_t oldByte = c[idx];
        const uint8_t newByte = static_cast<uint8_t>(rng() & 0xFFu);
        c[idx] = newByte;
        const int32_t diff = static_cast<int32_t>(newByte) - static_cast<int32_t>(oldByte);
        s1 = static_cast<uint32_t>(static_cast<int32_t>(s1) + diff);
        s2 = static_cast<uint32_t>(static_cast<int32_t>(s2) + static_cast<int32_t>(idx + 1u) * diff);
    }
    return {};
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

// ---------------------------------------------------------------------------------------------
// delta-perf P0/P1 tests (V-01..V-09). These extend the suite above; all existing cases remain.
// ---------------------------------------------------------------------------------------------

void TestProjectionAndStats() {
    // V-01 / AC-01: ProjectedRejected matches the integer formula
    //   matchedBytes*oldLen*100 <= newFileBytes*35*scannedBytes
    // and scannedBytes/matchedBytes are reported coherently from a real BuildPlan.
    Require(!fc::delta::ProjectedRejected(0, 0, 8000, 8000),
            "no evidence (scanned==0) must not project");
    // Zero matches over any scanned prefix -> projected download ratio == 1.0 >= 0.65 -> reject.
    Require(fc::delta::ProjectedRejected(0, 1000, 8000, 8000), "0% match must early-reject");
    // Full coverage (matched == scanned, old == new) -> ratio 0 -> never reject.
    Require(!fc::delta::ProjectedRejected(1000, 1000, 8000, 8000),
            "full coverage must not reject");
    // Monotone boundary: with old==new==N and scanned==S, reject iff matched*100 <= N*35*S/S
    // i.e. matched/S <= 0.35. Probe just under / over 35% projected coverage.
    Require(fc::delta::ProjectedRejected(34, 100, 100, 100), "34% projected match -> reject");
    Require(!fc::delta::ProjectedRejected(36, 100, 100, 100), "36% projected match -> continue");
    // Large-value path exercises the 128-bit comparison (8 GB-scale, no uint64 overflow).
    const uint64_t big = 8ull << 30;
    Require(fc::delta::ProjectedRejected(0, big / 4, big, big), "8GB zero-match -> reject");
    Require(!fc::delta::ProjectedRejected(big, big, big, big), "8GB full-match -> continue");

    // BuildPlan stats coherence on an identical buffer.
    const std::vector<uint8_t> data = RandomBytes(70000, 909);
    const fc::delta::SignatureSet sig = fc::delta::GenerateSignatures(data.data(), data.size());
    const fc::delta::DeltaPlan plan = fc::delta::BuildPlan(sig, data.data(), data.size());
    const uint64_t expectMatched = static_cast<uint64_t>(FullBlockCount(sig)) * sig.blockSize;
    Require(plan.stats.matchedBytes == expectMatched, "identical matchedBytes == fullBlocks*L");
    Require(plan.stats.scannedBytes == expectMatched, "identical scannedBytes == matchedBytes");
    Require(!plan.stats.earlyStopped, "identical must not early-stop");
}

void TestEarlyStopWorstCase() {
    // V-02 / AC-02 / B3 / B8: unrelated random old vs new with a small prefix override must
    // early-stop, signal a full benefit rejection, and scan far less than the whole old file.
    const std::vector<uint8_t> oldData = RandomBytes(1'000'000, 1234);
    const std::vector<uint8_t> newData = RandomBytes(1'000'000, 5678);
    const fc::delta::SignatureSet sig = fc::delta::GenerateSignatures(newData.data(), newData.size());
    fc::delta::BuildOptions opt;
    opt.prefixBytesOverride = 8192;  // tiny prefix so the first projection check fires early
    const fc::delta::DeltaPlan plan = fc::delta::BuildPlan(sig, oldData.data(), oldData.size(), opt);
    Require(plan.stats.earlyStopped, "random worst-case must early-stop");
    Require(fc::delta::BenefitRejected(plan.downloadBytes, plan.newFileBytes),
            "early stop must signal benefit rejection");
    Require(plan.downloadBytes == plan.newFileBytes, "early stop downloadBytes == newFileBytes");
    Require(plan.copies.empty() && plan.misses.empty(), "early stop emits no partial plan");
    Require(plan.stats.scannedBytes < oldData.size() / 2,
            "early stop must scan far less than the whole old file");
}

void TestEarlyStopPrefixProtection() {
    // V-03 / AC-03 / B4: a short unaligned head (< prefix) followed by fully matching content
    // must NOT early-stop, because the prefix window already contains matches by the time the
    // first projection check is allowed; reconstruction stays byte-exact.
    const std::vector<uint8_t> body = RandomBytes(200000, 4242);
    std::vector<uint8_t> newData = body;
    std::vector<uint8_t> oldData = RandomBytes(777, 13);  // short unaligned noise head
    oldData.insert(oldData.end(), body.begin(), body.end());
    const fc::delta::SignatureSet sig = fc::delta::GenerateSignatures(newData.data(), newData.size());
    fc::delta::BuildOptions opt;
    opt.prefixBytesOverride = 64 * 1024;  // > noise head, so matches accrue before evaluation
    const fc::delta::DeltaPlan plan = fc::delta::BuildPlan(sig, oldData.data(), oldData.size(), opt);
    Require(!plan.stats.earlyStopped, "prefix protection: aligned body must not early-stop");
    Require(Reconstruct(plan, oldData, newData) == newData, "prefix protection: reconstruct");
    Require(plan.downloadBytes < plan.newFileBytes, "prefix protection: expected copies");
}

void TestHighBenefitNoEarlyStop() {
    // V-04 / V-05 / AC-04 / AC-05 / FR-07: identical, single-byte, local-contiguous and pure
    // append are high-benefit and must never early-stop; reconstruction is byte-exact.
    {  // identical (AC-04)
        const std::vector<uint8_t> data = RandomBytes(300000, 71);
        const fc::delta::SignatureSet sig = fc::delta::GenerateSignatures(data.data(), data.size());
        const fc::delta::DeltaPlan plan = fc::delta::BuildPlan(sig, data.data(), data.size());
        Require(!plan.stats.earlyStopped, "identical: no early stop");
        Require(plan.downloadBytes < sig.blockSize, "identical: only short tail downloaded");
        Require(!fc::delta::BenefitRejected(plan.downloadBytes, plan.newFileBytes),
                "identical: not benefit-rejected");
        Require(Reconstruct(plan, data, data) == data, "identical: reconstruct");
    }
    {  // single-byte overwrite (AC-05)
        std::vector<uint8_t> oldData = RandomBytes(300000, 72);
        std::vector<uint8_t> newData = oldData;
        newData[150000] ^= 0xFF;
        const fc::delta::SignatureSet sig = fc::delta::GenerateSignatures(newData.data(), newData.size());
        const fc::delta::DeltaPlan plan = fc::delta::BuildPlan(sig, oldData.data(), oldData.size());
        Require(!plan.stats.earlyStopped, "single-byte: no early stop");
        Require(Reconstruct(plan, oldData, newData) == newData, "single-byte: reconstruct");
    }
    {  // local contiguous edit (AC-05)
        std::vector<uint8_t> oldData = RandomBytes(300000, 73);
        std::vector<uint8_t> newData = oldData;
        for (size_t i = 120000; i < 130000; ++i) {
            newData[i] = static_cast<uint8_t>(newData[i] + 7);
        }
        const fc::delta::SignatureSet sig = fc::delta::GenerateSignatures(newData.data(), newData.size());
        const fc::delta::DeltaPlan plan = fc::delta::BuildPlan(sig, oldData.data(), oldData.size());
        Require(!plan.stats.earlyStopped, "local-edit: no early stop");
        Require(Reconstruct(plan, oldData, newData) == newData, "local-edit: reconstruct");
    }
    {  // pure append (AC-05)
        const std::vector<uint8_t> oldData = RandomBytes(300000, 74);
        std::vector<uint8_t> newData = oldData;
        const std::vector<uint8_t> tail = RandomBytes(20000, 75);
        newData.insert(newData.end(), tail.begin(), tail.end());
        const fc::delta::SignatureSet sig = fc::delta::GenerateSignatures(newData.data(), newData.size());
        const fc::delta::DeltaPlan plan = fc::delta::BuildPlan(sig, oldData.data(), oldData.size());
        Require(!plan.stats.earlyStopped, "append: no early stop");
        Require(Reconstruct(plan, oldData, newData) == newData, "append: reconstruct");
    }
}

void TestLazyStrongCounts() {
    // V-06 / AC-06 / FR-08 / FR-12: strong computations never exceed weak candidate hits, and
    // an identical buffer computes the strong checksum exactly once per full block.
    {
        const std::vector<uint8_t> oldData = RandomBytes(120000, 301);
        const std::vector<uint8_t> newData = RandomBytes(120000, 302);  // unrelated
        const fc::delta::SignatureSet sig = fc::delta::GenerateSignatures(newData.data(), newData.size());
        const fc::delta::DeltaPlan plan = fc::delta::BuildPlan(sig, oldData.data(), oldData.size());
        Require(plan.stats.strongComputations <= plan.stats.weakCandidateHits,
                "strongComputations <= weakCandidateHits");
    }
    {
        const std::vector<uint8_t> data = RandomBytes(120000, 303);
        const fc::delta::SignatureSet sig = fc::delta::GenerateSignatures(data.data(), data.size());
        const fc::delta::DeltaPlan plan = fc::delta::BuildPlan(sig, data.data(), data.size());
        Require(plan.stats.strongComputations <= plan.stats.weakCandidateHits,
                "identical: strongComputations <= weakCandidateHits");
        Require(plan.stats.strongComputations == FullBlockCount(sig),
                "identical: one strong computation per full block");
    }
}

void TestWeakFilterBeforeStrong() {
    // V-07 / AC-07 / FR-09 / B6: a window sharing a populated bucket but with a different weak
    // must be excluded by the weak filter -> no weak hit recorded, no strong checksum computed.
    const std::vector<uint8_t> newData = RandomBytes(2048, 808);  // exactly one full block
    const fc::delta::SignatureSet sig = fc::delta::GenerateSignatures(newData.data(), newData.size());
    Require(sig.blockCount == 1 && FullBlockCount(sig) == 1, "single-block signature expected");
    const uint32_t L = sig.blockSize;
    const uint32_t blockWeak = sig.blocks[0].weak;
    const uint16_t bucket = fc::delta::BucketOf(blockWeak);
    const std::vector<uint8_t> oldData = FindSameBucketDiffWeak(L, bucket, blockWeak, 909);
    Require(oldData.size() == L, "failed to construct same-bucket-different-weak window");
    const fc::delta::DeltaPlan plan = fc::delta::BuildPlan(sig, oldData.data(), oldData.size());
    Require(plan.stats.weakCandidateHits == 0, "weak-unequal candidate must not count as hit");
    Require(plan.stats.strongComputations == 0, "weak filter must run before any strong work");
    Require(plan.copies.empty(), "weak-unequal window yields no copy");
}

void TestMatchedByteLevel() {
    // V-08 / AC-08 / FR-10: the byte-level matched marker drives correct reconstruction across
    // multiple scattered edits; tampering a downloaded byte makes the reconstruction differ.
    std::vector<uint8_t> oldData = RandomBytes(250000, 11);
    std::vector<uint8_t> newData = oldData;
    newData[5000] ^= 0x01;
    newData[90000] ^= 0x80;
    newData[200000] ^= 0x40;
    const fc::delta::SignatureSet sig = fc::delta::GenerateSignatures(newData.data(), newData.size());
    const fc::delta::DeltaPlan plan = fc::delta::BuildPlan(sig, oldData.data(), oldData.size());
    Require(Reconstruct(plan, oldData, newData) == newData, "byte-level matched: reconstruct");
    if (!plan.misses.empty()) {
        std::vector<uint8_t> tampered = newData;
        tampered[plan.misses.front().destOffsetNew] ^= 0xFF;
        Require(Reconstruct(plan, oldData, tampered) != newData, "tampered download must differ");
    }
}

void TestOptimizedEqualsReference() {
    // V-09 / AC-09 / FR-11 / HC-07: with early stop disabled, the optimized BuildPlan must be
    // byte-for-byte identical to the pre-optimization reference across a fixed input set,
    // including duplicate-block inputs that stress candidate ordering.
    struct Case {
        std::vector<uint8_t> oldData;
        std::vector<uint8_t> newData;
    };
    std::vector<Case> cases;
    {  // identical
        std::vector<uint8_t> d = RandomBytes(180000, 1);
        cases.push_back({d, d});
    }
    {  // single-byte overwrite
        std::vector<uint8_t> o = RandomBytes(180000, 2);
        std::vector<uint8_t> n = o;
        n[90000] ^= 0xFF;
        cases.push_back({o, n});
    }
    {  // head insert (unaligned)
        std::vector<uint8_t> o = RandomBytes(180000, 3);
        std::vector<uint8_t> n = RandomBytes(999, 4);
        n.insert(n.end(), o.begin(), o.end());
        cases.push_back({o, n});
    }
    {  // pure append
        std::vector<uint8_t> o = RandomBytes(180000, 5);
        std::vector<uint8_t> n = o;
        std::vector<uint8_t> t = RandomBytes(7000, 6);
        n.insert(n.end(), t.begin(), t.end());
        cases.push_back({o, n});
    }
    {  // completely different
        cases.push_back({RandomBytes(180000, 7), RandomBytes(180000, 8)});
    }
    {  // duplicate blocks: new has two identical blocks, old holds the content once -> the
       // descending candidate-order replication is what makes the outputs match (§5.4).
        std::vector<uint8_t> block = RandomBytes(4096, 9);  // ample for a full block
        std::vector<uint8_t> n;
        n.insert(n.end(), block.begin(), block.end());
        n.insert(n.end(), block.begin(), block.end());
        std::vector<uint8_t> filler = RandomBytes(60000, 10);
        n.insert(n.end(), filler.begin(), filler.end());
        std::vector<uint8_t> o = block;  // contains the repeated content once
        o.insert(o.end(), filler.begin(), filler.end());
        cases.push_back({o, n});
    }
    fc::delta::BuildOptions noStop;
    noStop.enableEarlyStop = false;
    for (size_t i = 0; i < cases.size(); ++i) {
        const fc::delta::SignatureSet sig =
            fc::delta::GenerateSignatures(cases[i].newData.data(), cases[i].newData.size());
        const fc::delta::DeltaPlan opt =
            fc::delta::BuildPlan(sig, cases[i].oldData.data(), cases[i].oldData.size(), noStop);
        const fc::delta::DeltaPlan ref =
            BuildPlanReference(sig, cases[i].oldData.data(), cases[i].oldData.size());
        Require(PlansByteEqual(opt, ref), "optimized BuildPlan != reference (FR-11)");
    }
}

}  // namespace

// --- delta-streaming-fix: StreamingSigner equivalence (V-01..V-06). Feeds the same bytes to
// StreamingSigner under many chunk sizes (including 1/2/3/7-byte splits and a prime > blockSize
// so chunk edges deliberately fall inside signature blocks) and asserts the SignatureSet and the
// full-file hash are bit-identical to the in-memory GenerateSignatures / ComputeBufferHash. ---
void CheckStreamEquivalent(const std::vector<uint8_t>& data, const std::vector<size_t>& chunks) {
    using namespace fc::delta;
    const SignatureSet ref = GenerateSignatures(data.data(), data.size());
    const fc::Hash256 refHash = fc::ComputeBufferHash(data.data(), data.size());
    for (size_t chunkArg : chunks) {
        const size_t chunk = chunkArg == 0 ? 1 : chunkArg;
        StreamingSigner signer(static_cast<uint64_t>(data.size()));
        size_t pos = 0;
        while (pos < data.size()) {
            const size_t n = std::min(chunk, data.size() - pos);
            signer.Update(data.data() + pos, n);
            pos += n;
        }
        const StreamingResult res = signer.Finish();
        Require(res.sig.fileSize == ref.fileSize, "stream fileSize equals memory path (EQ-01)");
        Require(res.sig.blockSize == ref.blockSize, "stream blockSize equals memory path (EQ-01)");
        Require(res.sig.blockCount == ref.blockCount, "stream blockCount equals memory path (EQ-01)");
        Require(res.sig.strongLen == ref.strongLen, "stream strongLen equals memory path (EQ-01)");
        Require(res.sig.blocks.size() == ref.blocks.size(), "stream block count equals (EQ-02)");
        for (size_t i = 0; i < ref.blocks.size(); ++i) {
            Require(res.sig.blocks[i].weak == ref.blocks[i].weak, "stream weak equals (EQ-02)");
            Require(res.sig.blocks[i].strong == ref.blocks[i].strong, "stream strong equals (EQ-02)");
        }
        Require(res.fileHash == refHash, "stream fileHash equals ComputeBufferHash raw (EQ-03)");
    }
}

// V-06: full boundary dataset — empty, tiny, sub/at/over blockSize, multi-chunk + tail, and a
// ~1 MiB random buffer — each under multiple chunk sizes (V-01/V-02/V-03).
void TestStreamingEquivalenceDataset() {
    const std::vector<size_t> tinyChunks = {1, 2, 3, 7, 2048, 100003, (1u << 20)};
    const uint64_t sizes[] = {0, 1, 2, 2047, 2048, 2049, 4096, 5000, 100000};
    for (uint64_t sz : sizes) {
        CheckStreamEquivalent(RandomBytes(sz, static_cast<uint32_t>(sz) + 1), tinyChunks);
    }
    // ~1 MiB random: skip the 1-byte chunk for runtime but keep a prime and a big chunk so both
    // cross-block and aligned feeds are covered.
    CheckStreamEquivalent(RandomBytes((1u << 20) + 12345, 424242),
                          {3, 100003, (1u << 20), (4u << 20)});
}

// V-04: a chunk boundary lands strictly inside a signature block; that block's weak & strong must
// still match the memory path (proves cross-chunk blocks hash the full byte sequence, R-01/FR-08).
void TestStreamingCrossChunkBlock() {
    using namespace fc::delta;
    // fileSize 5000 -> blockSize 2048 -> blocks {2048, 2048, 904}. chunk 3000 splits block#1
    // (bytes 2048..4095) at offset 3000.
    const std::vector<uint8_t> data = RandomBytes(5000, 7);
    const SignatureSet ref = GenerateSignatures(data.data(), data.size());
    Require(ref.blockSize == 2048, "cross-chunk test expects blockSize 2048");
    Require(ref.blockCount == 3, "cross-chunk test expects 3 blocks");
    StreamingSigner signer(data.size());
    signer.Update(data.data(), 3000);
    signer.Update(data.data() + 3000, data.size() - 3000);
    const StreamingResult res = signer.Finish();
    Require(res.sig.blocks[1].weak == ref.blocks[1].weak, "cross-chunk block weak matches (AC-04)");
    Require(res.sig.blocks[1].strong == ref.blocks[1].strong, "cross-chunk block strong matches (AC-04)");
    Require(res.fileHash == fc::ComputeBufferHash(data.data(), data.size()),
            "cross-chunk fileHash matches (EQ-03)");
}

// V-03 guard: fileHash uses the RAW XXH3 layout (ComputeBufferHash), which must differ from the
// canonical layout used by per-block strong checksums — catches an accidental canonical mix-up.
void TestStreamingRawVsCanonical() {
    using namespace fc::delta;
    const std::vector<uint8_t> data = RandomBytes(4096, 99);
    StreamingSigner signer(data.size());
    signer.Update(data.data(), data.size());
    const StreamingResult res = signer.Finish();
    const std::array<uint8_t, 16> canonical = StrongHash(data.data(), static_cast<uint32_t>(data.size()));
    Require(res.fileHash == fc::ComputeBufferHash(data.data(), data.size()),
            "fileHash is raw layout (EQ-03)");
    Require(!(res.fileHash == canonical), "raw layout differs from canonical (R-02 guard)");
}

// V-07 (unit slice): Finish() must throw when fewer bytes than fileSize are fed (short/changed
// file), so the server maps it to DeltaError (FR-12).
void TestStreamingShortFeedThrows() {
    using namespace fc::delta;
    const std::vector<uint8_t> data = RandomBytes(3000, 11);
    StreamingSigner signer(data.size());
    signer.Update(data.data(), 1000);  // feed fewer bytes than fileSize
    bool threw = false;
    try {
        (void)signer.Finish();
    } catch (const std::exception&) {
        threw = true;
    }
    Require(threw, "Finish throws on byte-count mismatch (FR-12)");
}

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
    // delta-perf P0/P1 extensions (V-01..V-09).
    TestProjectionAndStats();
    TestEarlyStopWorstCase();
    TestEarlyStopPrefixProtection();
    TestHighBenefitNoEarlyStop();
    TestLazyStrongCounts();
    TestWeakFilterBeforeStrong();
    TestMatchedByteLevel();
    TestOptimizedEqualsReference();
    // delta-streaming-fix: StreamingSigner equivalence + cross-chunk + layout guards (V-01..V-07).
    TestStreamingEquivalenceDataset();
    TestStreamingCrossChunkBlock();
    TestStreamingRawVsCanonical();
    TestStreamingShortFeedThrows();
}
