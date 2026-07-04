#include "delta.h"

#include <xxhash.h>

#include <algorithm>
#include <cstring>
#include <stdexcept>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace fc::delta {

namespace {

// Unsigned 128-bit value (hi:lo) used only for the early-stop projection comparison so the
// 8 GB-scale triple products never overflow uint64 (NFR-07, delta-perf R-02).
struct U128 {
    uint64_t hi = 0;
    uint64_t lo = 0;
};

// Full 64x64 -> 128-bit unsigned multiply. Deterministic across compilers.
U128 Mul64(uint64_t a, uint64_t b) {
#if defined(_MSC_VER) && defined(_M_X64)
    U128 r;
    r.lo = _umul128(a, b, &r.hi);
    return r;
#elif defined(__SIZEOF_INT128__)
    const unsigned __int128 p = static_cast<unsigned __int128>(a) * b;
    return U128{static_cast<uint64_t>(p >> 64), static_cast<uint64_t>(p)};
#else
    const uint64_t aLo = a & 0xFFFFFFFFull, aHi = a >> 32;
    const uint64_t bLo = b & 0xFFFFFFFFull, bHi = b >> 32;
    const uint64_t lolo = aLo * bLo;
    const uint64_t lohi = aLo * bHi;
    const uint64_t hilo = aHi * bLo;
    const uint64_t hihi = aHi * bHi;
    const uint64_t cross = (lolo >> 32) + (lohi & 0xFFFFFFFFull) + (hilo & 0xFFFFFFFFull);
    U128 r;
    r.lo = (lolo & 0xFFFFFFFFull) | (cross << 32);
    r.hi = hihi + (lohi >> 32) + (hilo >> 32) + (cross >> 32);
    return r;
#endif
}

bool LessEqual128(const U128& x, const U128& y) {
    return x.hi < y.hi || (x.hi == y.hi && x.lo <= y.lo);
}

// Inlined-weak hash entry for the CSR (bucket-contiguous) candidate layout (FR-09 / section 5.2).
struct Entry {
    uint32_t weak = 0;        // inlined so candidate filtering reads 8 contiguous bytes
    uint32_t blockIndex = 0;  // index into sig.blocks
};

// Forward-only sliding window over a sequential byte source (unified-disk-io-driver section 4.2). Serves
// contiguous views [p, p+len) and single bytes for a monotonically advancing cursor while holding
// at most (readAheadChunk + blockSize) bytes - no whole-file allocation (FR-15/AC-04). The BuildPlan
// scan advances p only forward (by 1 on a roll, by L on a match), so every access is >= the cursor.
class SlidingWindow {
public:
    SlidingWindow(uint64_t total, ByteSource src, size_t chunk)
        : total_(total), src_(std::move(src)), chunk_(chunk == 0 ? size_t{1} : chunk) {}

    // Drop the consumed prefix below `p` once it grows past one chunk (amortized O(1) per byte).
    // Called at the top of each scan step, before any live window pointer exists.
    void advance(uint64_t p) {
        if (p > physBase_) {
            const size_t drop = static_cast<size_t>(p - physBase_);
            if (drop >= chunk_) {
                buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(drop));
                physBase_ = p;
            }
        }
    }

    // Contiguous view of [p, p+len). Requires physBase_ <= p and p+len <= total_.
    const uint8_t* window(uint64_t p, uint32_t len) {
        loadTo(p + len);
        return buf_.data() + static_cast<size_t>(p - physBase_);
    }

    // Single byte at absolute offset `off` (off >= physBase_, off < total_). Never compacts, so a
    // look-ahead read of oldData[p+L] cannot discard bytes the cursor still needs.
    uint8_t byteAt(uint64_t off) {
        loadTo(off + 1);
        return buf_[static_cast<size_t>(off - physBase_)];
    }

private:
    void loadTo(uint64_t end) {
        if (end > total_) {
            end = total_;
        }
        while (physBase_ + buf_.size() < end) {
            const size_t before = buf_.size();
            const uint64_t deficit = end - (physBase_ + buf_.size());
            size_t want = chunk_ > deficit ? chunk_ : static_cast<size_t>(deficit);
            buf_.resize(before + want);
            const size_t got = src_(buf_.data() + before, want);
            if (got < want) {
                buf_.resize(before + got);
                if (got == 0) {
                    break;  // source exhausted (should coincide with total_)
                }
            }
        }
    }

    uint64_t total_;
    ByteSource src_;
    size_t chunk_;
    std::vector<uint8_t> buf_;
    uint64_t physBase_ = 0;
};

}  // namespace

RollingWeak WeakInit(const uint8_t* p, uint32_t L) {
    // s1 = sum(X[j]), s2 = sum((j+1)*X[j]). Accumulating in uint32_t may overflow 2^32, but the
    // final &0xFFFF is correct anyway because (X mod 2^32) mod 2^16 == X mod 2^16.
    uint32_t s1 = 0;
    uint32_t s2 = 0;
    for (uint32_t j = 0; j < L; ++j) {
        const uint32_t x = p[j];
        s1 += x;
        s2 += (j + 1u) * x;
    }
    RollingWeak w;
    w.s1 = s1 & 0xFFFFu;
    w.s2 = s2 & 0xFFFFu;
    return w;
}

void WeakRoll(RollingWeak& w, uint8_t outByte, uint8_t inByte, uint32_t L) {
    // Update s2 with the OLD s1 first, then update s1 (design section 5.2). Subtraction can wrap in
    // uint32_t, but masking to 16 bits yields the correct value mod 2^16 regardless.
    const uint32_t s1 = w.s1;
    const uint32_t s2 = w.s2;
    const uint32_t newS2 = (s2 + L * static_cast<uint32_t>(inByte) - s1) & 0xFFFFu;
    const uint32_t newS1 = (s1 + static_cast<uint32_t>(inByte) - static_cast<uint32_t>(outByte)) & 0xFFFFu;
    w.s2 = newS2;
    w.s1 = newS1;
}

uint16_t BucketOf(uint32_t weak32) {
    return static_cast<uint16_t>(((weak32 >> 16) ^ (weak32 & 0xFFFFu)) & 0xFFFFu);
}

uint64_t Isqrt64(uint64_t value) {
    if (value == 0) {
        return 0;
    }
    // Bit-by-bit integer sqrt (deterministic, no floating point).
    uint64_t result = 0;
    // Highest power of four <= value.
    uint64_t bit = 1ULL << 62;
    while (bit > value) {
        bit >>= 2;
    }
    uint64_t v = value;
    while (bit != 0) {
        if (v >= result + bit) {
            v -= result + bit;
            result = (result >> 1) + bit;
        } else {
            result >>= 1;
        }
        bit >>= 2;
    }
    return result;
}

uint32_t ChooseBlockSize(uint64_t fileSize) {
    const uint64_t raw = Isqrt64(fileSize);
    uint64_t aligned = ((raw + kBlockAlign - 1) / kBlockAlign) * kBlockAlign;
    if (aligned < kMinBlock) {
        aligned = kMinBlock;
    }
    if (aligned > kMaxBlock) {
        aligned = kMaxBlock;
    }
    return static_cast<uint32_t>(aligned);
}

uint8_t ChooseStrongLen(uint32_t blockCount) {
    if (blockCount <= (1u << 16)) {
        return 8;
    }
    if (blockCount <= (1u << 22)) {
        return 12;
    }
    return 16;
}

std::array<uint8_t, 16> StrongHash(const uint8_t* data, uint32_t len) {
    const XXH128_hash_t digest = XXH3_128bits(data, static_cast<size_t>(len));
    std::array<uint8_t, 16> out{};
    // Canonical (endian-stable) byte layout so server and client agree regardless of host
    // endianness; XXH3_128bits returns {low64, high64}.
    XXH128_canonical_t canonical;
    XXH128_canonicalFromHash(&canonical, digest);
    std::memcpy(out.data(), canonical.digest, out.size());
    return out;
}

SignatureSet GenerateSignatures(const uint8_t* data, uint64_t len) {
    SignatureSet sig;
    sig.fileSize = len;
    if (len == 0) {
        return sig;
    }
    sig.blockSize = ChooseBlockSize(len);
    sig.blockCount = static_cast<uint32_t>((len + sig.blockSize - 1) / sig.blockSize);
    sig.strongLen = ChooseStrongLen(sig.blockCount);
    sig.blocks.reserve(sig.blockCount);
    for (uint32_t i = 0; i < sig.blockCount; ++i) {
        const uint64_t off = static_cast<uint64_t>(i) * sig.blockSize;
        const uint32_t blen = static_cast<uint32_t>(std::min<uint64_t>(sig.blockSize, len - off));
        BlockSig bs;
        bs.weak = WeakInit(data + off, blen).value();
        bs.strong = StrongHash(data + off, blen);
        sig.blocks.push_back(bs);
    }
    return sig;
}

// --- Single-pass streaming signer (delta-streaming-fix). Two XXH3 states are kept for the
// whole file: `fileState` (full-file hash, raw layout like ComputeBufferHash) and `blockState`
// (reset per block, canonical layout like StrongHash). The weak checksum is accumulated per
// block with the exact WeakInit arithmetic so cross-chunk continuation is bit-identical. ---
struct StreamingSignerState {
    uint64_t fileSize = 0;
    uint32_t blockSize = 0;
    uint32_t blockCount = 0;
    uint8_t  strongLen = 0;
    uint64_t consumed = 0;
    uint32_t curBlockIndex = 0;
    uint32_t curBlockFilled = 0;  // bytes fed into the current block so far
    uint32_t curBlockLen = 0;     // expected length of the current block = min(blockSize, tail)
    uint32_t weakS1 = 0;
    uint32_t weakS2 = 0;
    uint32_t weakJ = 0;           // 0-based position inside the block -> (j+1) weight, == WeakInit
    XXH3_state_t* blockState = nullptr;
    XXH3_state_t* fileState = nullptr;
    SignatureSet sig;
};

StreamingSigner::StreamingSigner(uint64_t fileSize) : st_(std::make_unique<StreamingSignerState>()) {
    st_->fileSize = fileSize;
    st_->sig.fileSize = fileSize;
    st_->fileState = XXH3_createState();
    if (st_->fileState == nullptr || XXH3_128bits_reset(st_->fileState) == XXH_ERROR) {
        throw std::runtime_error("StreamingSigner: XXH3 file state init failed");
    }
    if (fileSize == 0) {
        return;  // empty file: no blocks, blockState created lazily (never needed)
    }
    st_->blockSize = ChooseBlockSize(fileSize);
    st_->blockCount = static_cast<uint32_t>((fileSize + st_->blockSize - 1) / st_->blockSize);
    st_->strongLen = ChooseStrongLen(st_->blockCount);
    st_->sig.blockSize = st_->blockSize;
    st_->sig.blockCount = st_->blockCount;
    st_->sig.strongLen = st_->strongLen;
    st_->sig.blocks.reserve(st_->blockCount);
    st_->curBlockLen = static_cast<uint32_t>(std::min<uint64_t>(st_->blockSize, fileSize));
    st_->blockState = XXH3_createState();
    if (st_->blockState == nullptr || XXH3_128bits_reset(st_->blockState) == XXH_ERROR) {
        throw std::runtime_error("StreamingSigner: XXH3 block state init failed");
    }
}

StreamingSigner::~StreamingSigner() {
    if (st_) {
        if (st_->blockState != nullptr) {
            XXH3_freeState(st_->blockState);
        }
        if (st_->fileState != nullptr) {
            XXH3_freeState(st_->fileState);
        }
    }
}

void StreamingSigner::Update(const uint8_t* data, size_t len) {
    StreamingSignerState& s = *st_;
    if (len > 0) {
        // Whole segment feeds the full-file hash in one shot (order-preserving, R-02).
        if (XXH3_128bits_update(s.fileState, data, len) == XXH_ERROR) {
            throw std::runtime_error("StreamingSigner: XXH3 file update failed");
        }
    }
    s.consumed += len;

    size_t pos = 0;
    while (pos < len && s.curBlockIndex < s.blockCount) {
        const uint32_t remainingInBlock = s.curBlockLen - s.curBlockFilled;
        const size_t avail = len - pos;
        const uint32_t span = static_cast<uint32_t>(
            std::min<uint64_t>(remainingInBlock, static_cast<uint64_t>(avail)));

        // weak: uint32 accumulate, mask to 16 bits only at block end (bit-identical to WeakInit).
        for (uint32_t k = 0; k < span; ++k) {
            const uint32_t x = data[pos + k];
            s.weakS1 += x;
            s.weakS2 += (s.weakJ + 1u) * x;
            ++s.weakJ;
        }
        // strong: batch-feed the arrived bytes into this block's XXH3 state (R-01: never splices
        // separately-computed fragments; the full block byte sequence is hashed in arrival order).
        if (span > 0 && XXH3_128bits_update(s.blockState, data + pos, span) == XXH_ERROR) {
            throw std::runtime_error("StreamingSigner: XXH3 block update failed");
        }
        s.curBlockFilled += span;
        pos += span;

        if (s.curBlockFilled == s.curBlockLen) {
            BlockSig bs;
            bs.weak = ((s.weakS2 & 0xFFFFu) << 16) | (s.weakS1 & 0xFFFFu);
            const XXH128_hash_t d = XXH3_128bits_digest(s.blockState);
            XXH128_canonical_t canonical;
            XXH128_canonicalFromHash(&canonical, d);
            std::memcpy(bs.strong.data(), canonical.digest, bs.strong.size());
            s.sig.blocks.push_back(bs);

            s.weakS1 = 0;
            s.weakS2 = 0;
            s.weakJ = 0;
            s.curBlockFilled = 0;
            ++s.curBlockIndex;
            if (s.curBlockIndex < s.blockCount) {
                const uint64_t off = static_cast<uint64_t>(s.curBlockIndex) * s.blockSize;
                s.curBlockLen =
                    static_cast<uint32_t>(std::min<uint64_t>(s.blockSize, s.fileSize - off));
                XXH3_128bits_reset(s.blockState);
            }
        }
    }
}

StreamingResult StreamingSigner::Finish() {
    StreamingSignerState& s = *st_;
    if (s.consumed != s.fileSize) {
        throw std::runtime_error("StreamingSigner: fed byte count does not match fileSize");
    }
    StreamingResult result;
    const XXH128_hash_t fd = XXH3_128bits_digest(s.fileState);
    // Raw memcpy (NOT canonical) so fileHash matches ComputeBufferHash/ComputeFileHash exactly
    // (EQ-03/R-02). This is deliberately different from the per-block canonical strong layout.
    std::memcpy(result.fileHash.data(), &fd, result.fileHash.size());
    result.sig = std::move(s.sig);
    return result;
}

uint64_t EarlyStopPrefixBytes(uint64_t oldLen, uint32_t blockSize) {
    // floor = max(kPrefixFloorBytes, blockSize * kPrefixFloorBlocks) - small-file evidence
    // floor and at least kPrefixFloorBlocks blocks of evidence (B5/B4).
    uint64_t floorBytes = kPrefixFloorBytes;
    const uint64_t blockFloor = static_cast<uint64_t>(blockSize) * kPrefixFloorBlocks;
    if (blockFloor > floorBytes) {
        floorBytes = blockFloor;
    }
    const uint64_t pct = oldLen / 100 * kPrefixPercent + (oldLen % 100) * kPrefixPercent / 100;
    uint64_t prefix = floorBytes > pct ? floorBytes : pct;
    if (prefix > kPrefixCapBytes) {  // min(cap, ...) performance cap (section 4.3)
        prefix = kPrefixCapBytes;
    }
    return prefix;
}

bool ProjectedRejected(uint64_t matchedBytes, uint64_t scannedBytes,
                       uint64_t oldLen, uint64_t newFileBytes) {
    if (scannedBytes == 0) {
        return false;  // no evidence yet -> never project
    }
    // Early stop <=> matchedBytes*oldLen*100 <= newFileBytes*35*scannedBytes (delta-perf section 4.1),
    // 35 == kBenefitPercentDen - kBenefitPercentNum. Fold the small constants into one factor
    // each (safe for any realistic file < 2^57 bytes) and compare in 128 bits.
    const U128 left = Mul64(matchedBytes, oldLen * kBenefitPercentDen);
    const U128 right = Mul64(newFileBytes,
                             scannedBytes * (kBenefitPercentDen - kBenefitPercentNum));
    return LessEqual128(left, right);
}

DeltaPlan BuildPlan(const SignatureSet& sig, const uint8_t* oldData, uint64_t oldLen,
                    const BuildOptions& opt) {
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

    std::vector<uint8_t> matched(blockCount, 0);  // byte-direct, not a bit-proxy (FR-10/AC-08)
    std::vector<uint64_t> matchedSrc(blockCount, 0);

    // CSR (bucket-contiguous) candidate layout over FULL blocks only: the trailing short block
    // (len < blockSize) cannot be located by a fixed blockSize rolling window, so it is
    // excluded and always a miss (section 5.1). bucketStart is a prefix sum; entries holds the
    // inlined-weak candidates packed per bucket so candidate scans read sequentially (section 5.2).
    constexpr uint32_t kBuckets = 1u << 16;
    std::vector<uint32_t> bucketStart(kBuckets + 1, 0);
    uint32_t fullBlockCount = 0;
    for (uint32_t i = 0; i < blockCount; ++i) {
        if (blockLen(i) != L) {
            continue;  // short trailing block
        }
        ++bucketStart[BucketOf(sig.blocks[i].weak) + 1];
        ++fullBlockCount;
    }
    for (uint32_t b = 0; b < kBuckets; ++b) {
        bucketStart[b + 1] += bucketStart[b];
    }
    std::vector<Entry> entries(fullBlockCount);
    std::vector<uint32_t> fillPos(bucketStart.begin(), bucketStart.begin() + kBuckets);
    // Iterate block index DESCENDING so the first block written to a bucket (highest index)
    // lands at the lowest slot: scanning a bucket [start,end) then visits blocks in descending
    // index order, byte-for-byte replicating the legacy head-insert chain order (FR-11/section 5.4).
    for (uint32_t ii = blockCount; ii-- > 0;) {
        if (blockLen(ii) != L) {
            continue;
        }
        const uint16_t b = BucketOf(sig.blocks[ii].weak);
        const uint32_t pos = fillPos[b]++;
        entries[pos].weak = sig.blocks[ii].weak;
        entries[pos].blockIndex = ii;
    }

    uint64_t scannedBytes = 0;
    uint64_t matchedBytes = 0;
    bool earlyStopped = false;
    const uint64_t prefixBytes = opt.prefixBytesOverride > 0
                                     ? opt.prefixBytesOverride
                                     : EarlyStopPrefixBytes(oldLen, L);

    if (oldLen >= L) {
        uint64_t p = 0;
        uint64_t nextEval = prefixBytes;  // first projection check once p reaches the prefix
        RollingWeak w = WeakInit(oldData, L);
        while (true) {
            const uint32_t weak32 = w.value();
            const uint16_t bucket = BucketOf(weak32);
            bool matchedHere = false;
            std::array<uint8_t, 16> strong{};
            bool strongComputed = false;
            const uint32_t kStart = bucketStart[bucket];
            const uint32_t kEnd = bucketStart[bucket + 1];
            for (uint32_t k = kStart; k < kEnd; ++k) {
                const Entry& e = entries[k];
                if (e.weak != weak32) {
                    continue;  // weak filter completes before any strong work (FR-09)
                }
                ++plan.stats.weakCandidateHits;
                const uint32_t node = e.blockIndex;
                if (matched[node]) {
                    continue;
                }
                if (!strongComputed) {  // lazy strong: only on a live weak candidate (FR-08)
                    strong = StrongHash(oldData + p, L);
                    strongComputed = true;
                    ++plan.stats.strongComputations;
                }
                if (std::equal(strong.begin(), strong.begin() + strongLen,
                               sig.blocks[node].strong.begin())) {
                    matched[node] = 1;
                    matchedSrc[node] = p;
                    matchedHere = true;
                    break;
                }
            }
            if (matchedHere) {
                // Non-overlapping advance past the whole matched block.
                matchedBytes += L;
                p += L;
                if (p + L > oldLen) {
                    break;
                }
                w = WeakInit(oldData + p, L);
            } else {
                if (p + L >= oldLen) {
                    break;  // no inByte available at p+L to roll further
                }
                WeakRoll(w, oldData[p], oldData[p + L], L);
                ++p;
            }
            // P0 early stop: once past the protective prefix, project the matched fraction and
            // abandon delta if the projected download ratio reaches T = 0.65 (FR-02/03/04).
            if (opt.enableEarlyStop && p >= nextEval) {
                if (ProjectedRejected(matchedBytes, p, oldLen, plan.newFileBytes)) {
                    earlyStopped = true;
                    break;  // stop touching oldData immediately (B8)
                }
                nextEval = p + kEarlyStopEvalStride;
            }
        }
        scannedBytes = p;
    }

    plan.stats.scannedBytes = scannedBytes;
    plan.stats.matchedBytes = matchedBytes;
    plan.stats.earlyStopped = earlyStopped;

    if (earlyStopped) {
        // Never emit a partial delta plan: signal a full benefit rejection so the caller's
        // existing fallback runs (downloadBytes == newFileBytes => BenefitRejected, FR-05/06).
        plan.copies.clear();
        plan.misses.clear();
        plan.downloadBytes = plan.newFileBytes;
        return plan;
    }

    // Emit copies for matched blocks (all full-length) and coalesce consecutive missing
    // blocks (contiguous in dest) into the fewest MissOps.
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

bool BenefitRejected(uint64_t downloadBytes, uint64_t newFileBytes) {
    // reject <=> downloadBytes / newFileBytes >= 0.65, integer form (>= is reject, FR-17):
    //   downloadBytes * 100 >= newFileBytes * 65.
    return downloadBytes * kBenefitPercentDen >= newFileBytes * kBenefitPercentNum;
}

DeltaPlan BuildPlanStreaming(const SignatureSet& sig, uint64_t oldLen, const ByteSource& source,
                             const StreamingPlanOptions& opt, const CopyCapturedFn& onCopy) {
    // Mirrors BuildPlan exactly (design D-02 A). The ONLY difference vs. BuildPlan is that every
    // oldData[] access is served by `win` (a forward-only sliding window); all candidate/CSR/
    // early-stop/copy-coalesce arithmetic is identical, so the result is bit-identical (AC-02/03).
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

    std::vector<uint8_t> matched(blockCount, 0);
    std::vector<uint64_t> matchedSrc(blockCount, 0);

    constexpr uint32_t kBuckets = 1u << 16;
    std::vector<uint32_t> bucketStart(kBuckets + 1, 0);
    uint32_t fullBlockCount = 0;
    for (uint32_t i = 0; i < blockCount; ++i) {
        if (blockLen(i) != L) {
            continue;
        }
        ++bucketStart[BucketOf(sig.blocks[i].weak) + 1];
        ++fullBlockCount;
    }
    for (uint32_t b = 0; b < kBuckets; ++b) {
        bucketStart[b + 1] += bucketStart[b];
    }
    std::vector<Entry> entries(fullBlockCount);
    std::vector<uint32_t> fillPos(bucketStart.begin(), bucketStart.begin() + kBuckets);
    for (uint32_t ii = blockCount; ii-- > 0;) {
        if (blockLen(ii) != L) {
            continue;
        }
        const uint16_t b = BucketOf(sig.blocks[ii].weak);
        const uint32_t pos = fillPos[b]++;
        entries[pos].weak = sig.blocks[ii].weak;
        entries[pos].blockIndex = ii;
    }

    uint64_t scannedBytes = 0;
    uint64_t matchedBytes = 0;
    bool earlyStopped = false;
    const uint64_t prefixBytes = opt.build.prefixBytesOverride > 0
                                     ? opt.build.prefixBytesOverride
                                     : EarlyStopPrefixBytes(oldLen, L);

    if (oldLen >= L) {
        SlidingWindow win(oldLen, source, opt.readAheadChunkBytes);
        uint64_t p = 0;
        uint64_t nextEval = prefixBytes;
        RollingWeak w = WeakInit(win.window(0, L), L);
        while (true) {
            win.advance(p);  // amortized compaction on the monotonic cursor (AC-04)
            const uint32_t weak32 = w.value();
            const uint16_t bucket = BucketOf(weak32);
            bool matchedHere = false;
            std::array<uint8_t, 16> strong{};
            bool strongComputed = false;
            const uint32_t kStart = bucketStart[bucket];
            const uint32_t kEnd = bucketStart[bucket + 1];
            for (uint32_t k = kStart; k < kEnd; ++k) {
                const Entry& e = entries[k];
                if (e.weak != weak32) {
                    continue;
                }
                ++plan.stats.weakCandidateHits;
                const uint32_t node = e.blockIndex;
                if (matched[node]) {
                    continue;
                }
                if (!strongComputed) {
                    strong = StrongHash(win.window(p, L), L);
                    strongComputed = true;
                    ++plan.stats.strongComputations;
                }
                if (std::equal(strong.begin(), strong.begin() + strongLen,
                               sig.blocks[node].strong.begin())) {
                    matched[node] = 1;
                    matchedSrc[node] = p;
                    matchedHere = true;
                    if (onCopy) {
                        // Live block bytes are in the window right now: capture for the temp write
                        // during this single pass (D-01 A). destOffset = node * blockSize.
                        onCopy(p, static_cast<uint64_t>(node) * L, L, win.window(p, L));
                    }
                    break;
                }
            }
            if (matchedHere) {
                matchedBytes += L;
                p += L;
                if (p + L > oldLen) {
                    break;
                }
                win.advance(p);
                w = WeakInit(win.window(p, L), L);
            } else {
                if (p + L >= oldLen) {
                    break;
                }
                const uint8_t outByte = win.byteAt(p);
                const uint8_t inByte = win.byteAt(p + L);
                WeakRoll(w, outByte, inByte, L);
                ++p;
            }
            if (opt.build.enableEarlyStop && p >= nextEval) {
                if (ProjectedRejected(matchedBytes, p, oldLen, plan.newFileBytes)) {
                    earlyStopped = true;
                    break;
                }
                nextEval = p + kEarlyStopEvalStride;
            }
        }
        scannedBytes = p;
    }

    plan.stats.scannedBytes = scannedBytes;
    plan.stats.matchedBytes = matchedBytes;
    plan.stats.earlyStopped = earlyStopped;

    if (earlyStopped) {
        plan.copies.clear();
        plan.misses.clear();
        plan.downloadBytes = plan.newFileBytes;
        return plan;
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

}  // namespace fc::delta
