// Equivalence tests for the streaming BuildPlan (unified-disk-io-driver C6, FR-15/16/17,
// AC-01..04, V-02/V-03). Each case builds a DeltaPlan two ways over the SAME (sig, old bytes):
//   reference: fc::delta::BuildPlan(sig, oldData.data(), oldLen, opt)        (in-memory baseline)
//   streaming: fc::delta::BuildPlanStreaming(sig, oldLen, memSource, {opt})  (sliding window)
// and asserts every DeltaPlan/DeltaStats field is byte-for-byte equal across a matrix of chunk
// sizes (including 1, prime, block-sized and > 1 MiB). The "capture on match" callback is also
// exercised: the copies it emits during the single scan pass plus the plan misses must reconstruct
// the new file exactly. Pure in-memory; no threads, no disk, no xxhash-in-header dependency.

#include "delta.h"

#include <cstdint>
#include <cstring>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void Require(bool cond, const std::string& msg) {
    if (!cond) {
        throw std::runtime_error("streaming-buildplan: " + msg);
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

// A sequential memory byte source. Reads forward only, exactly what SlidingWindow demands.
fc::delta::ByteSource MemSource(const std::vector<uint8_t>& data) {
    auto pos = std::make_shared<size_t>(0);
    return [&data, pos](uint8_t* dst, size_t maxLen) -> size_t {
        const size_t avail = data.size() - *pos;
        const size_t n = avail < maxLen ? avail : maxLen;
        if (n > 0) {
            std::memcpy(dst, data.data() + *pos, n);
            *pos += n;
        }
        return n;
    };
}

void RequirePlanEqual(const fc::delta::DeltaPlan& ref, const fc::delta::DeltaPlan& s,
                      const std::string& ctx) {
    Require(ref.newFileBytes == s.newFileBytes, ctx + ": newFileBytes");
    Require(ref.downloadBytes == s.downloadBytes, ctx + ": downloadBytes");
    Require(ref.copies.size() == s.copies.size(), ctx + ": copies.size");
    for (size_t i = 0; i < ref.copies.size(); ++i) {
        Require(ref.copies[i].srcOffsetOld == s.copies[i].srcOffsetOld, ctx + ": copy.srcOffsetOld");
        Require(ref.copies[i].destOffsetNew == s.copies[i].destOffsetNew,
                ctx + ": copy.destOffsetNew");
        Require(ref.copies[i].len == s.copies[i].len, ctx + ": copy.len");
    }
    Require(ref.misses.size() == s.misses.size(), ctx + ": misses.size");
    for (size_t i = 0; i < ref.misses.size(); ++i) {
        Require(ref.misses[i].destOffsetNew == s.misses[i].destOffsetNew, ctx + ": miss.destOffset");
        Require(ref.misses[i].len == s.misses[i].len, ctx + ": miss.len");
    }
    Require(ref.stats.scannedBytes == s.stats.scannedBytes, ctx + ": stats.scannedBytes");
    Require(ref.stats.matchedBytes == s.stats.matchedBytes, ctx + ": stats.matchedBytes");
    Require(ref.stats.strongComputations == s.stats.strongComputations,
            ctx + ": stats.strongComputations");
    Require(ref.stats.weakCandidateHits == s.stats.weakCandidateHits,
            ctx + ": stats.weakCandidateHits");
    Require(ref.stats.earlyStopped == s.stats.earlyStopped, ctx + ": stats.earlyStopped");
}

// Full equivalence + capture reconstruction for one (oldData -> newData) pair over a chunk matrix.
void CheckPair(const std::vector<uint8_t>& oldData, const std::vector<uint8_t>& newData,
               const fc::delta::BuildOptions& opt, const std::string& name) {
    const fc::delta::SignatureSet sig =
        fc::delta::GenerateSignatures(newData.data(), newData.size());
    const fc::delta::DeltaPlan ref =
        fc::delta::BuildPlan(sig, oldData.data(), oldData.size(), opt);

    const uint32_t chunks[] = {1u, 2u, 3u, 7u, 64u, 2048u, 2049u, 100003u, 1u << 20};
    for (uint32_t chunk : chunks) {
        fc::delta::StreamingPlanOptions sopt;
        sopt.build = opt;
        sopt.readAheadChunkBytes = chunk;

        // Capture-on-match: write copies into `recon` during the single scan pass.
        std::vector<uint8_t> recon(static_cast<size_t>(sig.fileSize), 0);
        auto onCopy = [&](uint64_t srcOff, uint64_t destOff, uint32_t len, const uint8_t* bytes) {
            (void)srcOff;
            std::memcpy(recon.data() + destOff, bytes, len);
        };

        const fc::delta::DeltaPlan s =
            fc::delta::BuildPlanStreaming(sig, oldData.size(), MemSource(oldData), sopt, onCopy);

        const std::string ctx = name + " chunk=" + std::to_string(chunk);
        RequirePlanEqual(ref, s, ctx);

        if (!s.stats.earlyStopped) {
            // captured copies + downloaded misses (from the authoritative new file) == new file.
            for (const auto& m : s.misses) {
                std::memcpy(recon.data() + m.destOffsetNew, newData.data() + m.destOffsetNew, m.len);
            }
            Require(recon == newData, ctx + ": capture reconstruction mismatch");
        }
    }
}

void TestIdenticalAndEdits() {
    fc::delta::BuildOptions opt;  // default early-stop on
    for (uint64_t size : {uint64_t{2048}, uint64_t{2049}, uint64_t{4096}, uint64_t{5000},
                          uint64_t{70000}, uint64_t{200000}}) {
        const std::vector<uint8_t> base = RandomBytes(size, 700 + static_cast<uint32_t>(size));
        CheckPair(base, base, opt, "identical/" + std::to_string(size));  // all full blocks match

        {  // single-byte overwrite: strong hit on most, one changed block misses
            std::vector<uint8_t> nw = base;
            nw[static_cast<size_t>(size / 2)] ^= 0xFF;
            CheckPair(base, nw, opt, "single-edit/" + std::to_string(size));
        }
        {  // local contiguous edit
            std::vector<uint8_t> nw = base;
            for (uint64_t i = size / 3; i < size / 3 + std::min<uint64_t>(3000, size / 4); ++i) {
                nw[static_cast<size_t>(i)] = static_cast<uint8_t>(nw[static_cast<size_t>(i)] + 5);
            }
            CheckPair(base, nw, opt, "local-edit/" + std::to_string(size));
        }
    }
}

void TestShiftAndTail() {
    fc::delta::BuildOptions opt;
    // Prepend one byte then keep the same length -> everything shifts by 1 -> heavy rolling with
    // many weak candidate hits and strong recomputes (V-03 weak-hit / strong-miss / match-jump).
    const std::vector<uint8_t> body = RandomBytes(120000, 4242);
    {
        std::vector<uint8_t> oldData;
        oldData.push_back(0x5A);
        oldData.insert(oldData.end(), body.begin(), body.end());
        CheckPair(oldData, body, opt, "shift+1");
        CheckPair(body, oldData, opt, "shift-1");
    }
    // Short trailing block (fileSize not a multiple of blockSize) is never matched (always miss).
    {
        const std::vector<uint8_t> nw = RandomBytes(70001, 909);  // 70001 % 2048 != 0
        CheckPair(nw, nw, opt, "tail-short-identical");
    }
    // Append-only -> tail region is the only download.
    {
        std::vector<uint8_t> nw = body;
        const std::vector<uint8_t> tail = RandomBytes(9000, 71);
        nw.insert(nw.end(), tail.begin(), tail.end());
        CheckPair(body, nw, opt, "append");
    }
}

void TestUnrelatedAndEarlyStop() {
    // Unrelated content with default early-stop: reference and streaming must AGREE on the
    // early-stop decision, scannedBytes, and the (empty) emitted plan (V-02/V-03 early stop).
    {
        const std::vector<uint8_t> oldData = RandomBytes(1'000'000, 1234);
        const std::vector<uint8_t> newData = RandomBytes(1'000'000, 5678);
        fc::delta::BuildOptions opt;
        opt.prefixBytesOverride = 8192;  // tiny prefix -> first projection fires early
        CheckPair(oldData, newData, opt, "early-stop-worstcase");
    }
    // Same worst case but early stop DISABLED: scans the whole file, still bit-identical.
    {
        const std::vector<uint8_t> oldData = RandomBytes(300000, 222);
        const std::vector<uint8_t> newData = RandomBytes(300000, 333);
        fc::delta::BuildOptions opt;
        opt.enableEarlyStop = false;
        CheckPair(oldData, newData, opt, "unrelated-no-earlystop");
    }
}

void TestDegenerate() {
    fc::delta::BuildOptions opt;
    // Empty new file: blockCount == 0 -> empty plan, oldLen irrelevant.
    {
        const std::vector<uint8_t> empty;
        const std::vector<uint8_t> old = RandomBytes(4096, 1);
        const fc::delta::SignatureSet sig = fc::delta::GenerateSignatures(empty.data(), 0);
        const fc::delta::DeltaPlan ref = fc::delta::BuildPlan(sig, old.data(), old.size(), opt);
        fc::delta::StreamingPlanOptions sopt;
        const fc::delta::DeltaPlan s =
            fc::delta::BuildPlanStreaming(sig, old.size(), MemSource(old), sopt);
        RequirePlanEqual(ref, s, "empty-new");
    }
    // Old file shorter than one block: no rolling window fits -> all misses.
    {
        const std::vector<uint8_t> newData = RandomBytes(70000, 44);
        const std::vector<uint8_t> oldData = RandomBytes(1000, 45);  // < blockSize
        CheckPair(oldData, newData, opt, "old<blockSize");
    }
    // Old empty, new non-empty.
    {
        const std::vector<uint8_t> newData = RandomBytes(5000, 46);
        const std::vector<uint8_t> oldData;
        CheckPair(oldData, newData, opt, "old-empty");
    }
}

}  // namespace

void RunStreamingBuildPlanTests() {
    TestIdenticalAndEdits();
    TestShiftAndTail();
    TestUnrelatedAndEarlyStop();
    TestDegenerate();
}
