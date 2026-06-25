#include "delta.h"

#include <xxhash.h>

#include <algorithm>
#include <cstring>

namespace fc::delta {

RollingWeak WeakInit(const uint8_t* p, uint32_t L) {
    // s1 = Σ X[j], s2 = Σ (j+1)*X[j]. Accumulating in uint32_t may overflow 2^32, but the
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
    // Update s2 with the OLD s1 first, then update s1 (design §5.2). Subtraction can wrap in
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

DeltaPlan BuildPlan(const SignatureSet& sig, const uint8_t* oldData, uint64_t oldLen) {
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

    // Hash table over FULL blocks only: the trailing short block (len < blockSize) cannot be
    // located by a fixed blockSize rolling window, so it is excluded and always a miss (§5.1).
    std::vector<int32_t> bucketHead(1u << 16, -1);
    std::vector<int32_t> chainNext(blockCount, -1);
    for (uint32_t i = 0; i < blockCount; ++i) {
        if (blockLen(i) != L) {
            continue;  // short trailing block
        }
        const uint16_t b = BucketOf(sig.blocks[i].weak);
        chainNext[i] = bucketHead[b];
        bucketHead[b] = static_cast<int32_t>(i);  // head-insert
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
                // Non-overlapping advance past the whole matched block.
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
        }
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

}  // namespace fc::delta
