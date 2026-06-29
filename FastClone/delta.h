#pragma once

// Binary delta (rsync-style rolling search, zsync-inverted topology) core algorithms.
//
// Legal: FastClone is MIT-licensed. Every algorithm here (weak rolling checksum recurrence,
// 16-bit bucket + chain index, three-stage match funnel, block-size heuristic, adaptive
// strong-checksum truncation) is an INDEPENDENT implementation from public algorithm
// principles. No rsync (GPLv3) source was copied, adapted, or derived. The strong checksum
// uses FastClone's existing XXH3-128 (NOT MD4/MD5). See docs/design/binary-delta.md §0.
//
// This header is deliberately free of <xxhash.h>: the XXH3 dependency is confined to
// delta.cpp so translation units that only orchestrate delta (sync_engine.cpp) or unit-test
// the pure recurrences stay independent of the xxhash include path.

#include <array>
#include <cstdint>
#include <vector>

namespace fc::delta {

// --- Block-size heuristic constants (design §5.1 / OQ-02). delta only runs on files
// >= --delta-min-size (>= 1 MiB), so blockSize always lands in [kMinBlock, kMaxBlock]. ---
constexpr uint32_t kMinBlock = 2048;     // 2 KiB lower bound
constexpr uint32_t kMaxBlock = 131072;   // 128 KiB upper bound (requirements §5.2)
constexpr uint32_t kBlockAlign = 1024;   // sqrt(fileSize) rounded up to 1 KiB

// --- Benefit-rejection threshold T = 0.65 (design D-02 / OQ-01), integer-only to avoid
// floating-point nondeterminism: reject when downloadBytes*100 >= newFileBytes*65. ---
constexpr uint64_t kBenefitPercentNum = 65;
constexpr uint64_t kBenefitPercentDen = 100;

// --- P0 early-stop tuning (delta-perf design §4.2/§4.3). All compile-time; adjust without
// touching the public interface (R-04). The evaluation prefix is
//   min(kPrefixCapBytes, max(max(kPrefixFloorBytes, blockSize*kPrefixFloorBlocks),
//                            oldLen*kPrefixPercent/100)).
// After the prefix is reached the projected download ratio is re-checked every
// kEarlyStopEvalStride bytes (avoids a 128-bit multiply per scanned byte). ---
constexpr uint64_t kPrefixPercent       = 5;             // oldLen * 5% (mid-file evidence)
constexpr uint64_t kPrefixCapBytes      = 256ull << 20;  // 256 MiB hard performance cap
constexpr uint64_t kPrefixFloorBytes    = 1ull << 20;    // 1 MiB small-file evidence floor
constexpr uint32_t kPrefixFloorBlocks   = 256;           // scan >= 256 blocks before stopping
constexpr uint64_t kEarlyStopEvalStride = 4ull << 20;    // re-evaluate every 4 MiB past prefix

// Per-block signature: 32-bit weak checksum (s2<<16 | s1) + XXH3-128 strong checksum.
// Only the first SignatureSet::strongLen bytes of `strong` are significant on the wire and
// in comparisons (adaptive truncation, design §5.3).
struct BlockSig {
    uint32_t weak = 0;
    std::array<uint8_t, 16> strong{};
};

// Full signature for one new file (server-generated, sent to client).
struct SignatureSet {
    uint64_t fileSize = 0;
    uint32_t blockSize = 0;   // see ChooseBlockSize; 0 only for empty files
    uint32_t blockCount = 0;  // = ceil(fileSize / blockSize); last block may be short
    uint8_t  strongLen = 0;   // significant strong-checksum byte count (8/12/16)
    std::vector<BlockSig> blocks;  // size == blockCount
};

// Rolling weak checksum state. s1/s2 are kept reduced mod 2^16 (stored in low 16 bits).
struct RollingWeak {
    uint32_t s1 = 0;
    uint32_t s2 = 0;
    // weak32 = (s2 << 16) | s1 (design §5.2).
    [[nodiscard]] uint32_t value() const {
        return ((s2 & 0xFFFFu) << 16) | (s1 & 0xFFFFu);
    }
};

// Initialize the weak checksum over X[p .. p+L-1] by brute-force summation (the reference
// against which WeakRoll is validated, AC-01). Left byte weight 1, right byte weight L.
RollingWeak WeakInit(const uint8_t* p, uint32_t L);

// Advance the window one byte (p -> p+1): outByte = X[p] leaves, inByte = X[p+L] enters.
// O(1); must produce the same result as WeakInit over the new window (AC-01).
void WeakRoll(RollingWeak& w, uint8_t outByte, uint8_t inByte, uint32_t L);

// 16-bit bucket fold of a 32-bit weak checksum (self-defined, not rsync's).
uint16_t BucketOf(uint32_t weak32);

// Deterministic integer square root (floor) used by the block-size heuristic.
uint64_t Isqrt64(uint64_t value);

// Block-size heuristic: clamp(alignUp(isqrt(fileSize), 1KiB), kMinBlock, kMaxBlock).
uint32_t ChooseBlockSize(uint64_t fileSize);

// Adaptive strong-checksum truncation length in bytes (design §5.3): 8 / 12 / 16.
uint8_t ChooseStrongLen(uint32_t blockCount);

// XXH3-128 over [data, data+len). Defined in delta.cpp (the only xxhash dependency).
std::array<uint8_t, 16> StrongHash(const uint8_t* data, uint32_t len);

// Server side: generate a deterministic signature set for new-file contents [data, len)
// (FR-09/FR-10). Empty input yields an all-zero SignatureSet (blockCount == 0).
SignatureSet GenerateSignatures(const uint8_t* data, uint64_t len);

// Reconstruction plan operations. A CopyOp sources bytes from the local old file; a MissOp
// must be downloaded from the server. Together they tile [0, newFileBytes) exactly.
struct CopyOp {
    uint64_t srcOffsetOld = 0;
    uint64_t destOffsetNew = 0;
    uint32_t len = 0;
};
struct MissOp {
    uint64_t destOffsetNew = 0;
    uint32_t len = 0;  // coalesced run of consecutive missing blocks (<= blockSize * run)
};
// Per-BuildPlan runtime statistics. All fields are client-local (never on the wire, HC-02);
// they back the early-stop heuristic and the P1 observability asserts (delta-perf §3.1).
struct DeltaStats {
    uint64_t scannedBytes = 0;        // final rolling pointer p (FR-01, monotonic)
    uint64_t matchedBytes = 0;        // matched full blocks * blockSize (FR-01, monotonic)
    uint64_t strongComputations = 0;  // StrongHash invocations (FR-12 / AC-06)
    uint64_t weakCandidateHits = 0;   // candidates whose inlined weak == window weak (FR-12)
    bool earlyStopped = false;        // projected download ratio triggered early stop (FR-03)
};

struct DeltaPlan {
    std::vector<CopyOp> copies;
    std::vector<MissOp> misses;
    uint64_t downloadBytes = 0;  // = sum of misses[].len
    uint64_t newFileBytes = 0;   // = sig.fileSize
    DeltaStats stats;            // delta-perf: runtime stats + early-stop signal (local-only)
};

// BuildPlan controls. Defaults preserve the legacy 3-argument contract exactly (early stop on,
// default prefix formula) so existing callers (sync_engine.cpp) need no change (HC-02/NFR-04).
struct BuildOptions {
    bool     enableEarlyStop     = true;  // AC-09 equivalence comparisons set this false
    uint64_t prefixBytesOverride = 0;     // 0 = default formula; >0 = test seed (delta-perf §8)
};

// Projected-rejection predicate (FR-02/FR-03): extrapolate the matched fraction observed over
// scannedBytes to the whole old file and compare the projected download ratio to T = 0.65,
// sharing kBenefitPercentNum/Den. Returns true when delta should be abandoned early. Uses a
// 128-bit comparison internally so 8 GB-scale products never overflow (NFR-07). Pure function.
bool ProjectedRejected(uint64_t matchedBytes, uint64_t scannedBytes,
                       uint64_t oldLen, uint64_t newFileBytes);

// Early-stop evaluation prefix in bytes (FR-04 / OQ-01). The rolling scan must reach this many
// bytes before any projected-rejection check is allowed, protecting unaligned heads (B4).
uint64_t EarlyStopPrefixBytes(uint64_t oldLen, uint32_t blockSize);

// Client side: roll the weak checksum across the local old file, run the three-stage match
// funnel (bucket -> 32-bit weak -> truncated XXH3-128) and emit the reconstruction plan
// (FR-13/14/15/16). The short trailing block is never matched and is always a MissOp.
// The optional opt enables early stop (default) and test overrides; the 3-argument form keeps
// the legacy behavior contract (delta-perf §3.1).
DeltaPlan BuildPlan(const SignatureSet& sig, const uint8_t* oldData, uint64_t oldLen,
                    const BuildOptions& opt = {});

// Benefit rejection (FR-17, design §5.6): reject delta when downloadBytes/newFileBytes >= T.
bool BenefitRejected(uint64_t downloadBytes, uint64_t newFileBytes);

}  // namespace fc::delta
