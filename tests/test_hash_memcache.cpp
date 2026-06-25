#include "hash_memcache.h"

#include <stdexcept>

namespace {

void Require(bool cond, const char* msg) {
    if (!cond) {
        throw std::runtime_error(msg);
    }
}

fc::Hash256 MakeHash(uint8_t seed) {
    fc::Hash256 value{};
    for (size_t i = 0; i < value.size(); ++i) {
        value[i] = static_cast<uint8_t>(seed + i);
    }
    return value;
}

}  // namespace

void RunHashMemCacheTests() {
    fc::ServerHashMemCache cache;
    cache.Configure(true);

    const fc::HashFingerprint fp{1024, 123456789};
    const fc::Hash256 expected = MakeHash(11);
    cache.Upsert("a.txt", fp, expected);

    fc::Hash256 got{};
    Require(cache.TryGet("a.txt", fp, got), "Expected cache hit");
    Require(got == expected, "Expected cached hash bytes");
    Require(cache.EntryCount() == 1, "Expected one cache entry");
    Require(cache.HitCount() == 1, "Expected one cache hit");
    Require(cache.MissCount() == 0, "Expected zero cache misses");

    const fc::HashFingerprint mismatch{1024, 123456790};
    Require(!cache.TryGet("a.txt", mismatch, got), "Expected cache miss on fingerprint mismatch");
    Require(cache.EntryCount() == 0, "Expected stale entry eviction on mismatch");
    Require(cache.MissCount() == 1, "Expected one cache miss after mismatch");

    Require(!cache.TryGet("a.txt", fp, got), "Expected cache miss after eviction");
    Require(cache.MissCount() == 2, "Expected miss count increment after eviction");

    // ---- BlockSigMemCache (binary-delta AC-14 / D-05): same key+fingerprint discipline ----
    fc::BlockSigMemCache sigCache;
    sigCache.Configure(true);

    fc::delta::SignatureSet sig;
    sig.fileSize = 4096;
    sig.blockSize = 2048;
    sig.blockCount = 2;
    sig.strongLen = 8;
    sig.blocks.resize(2);
    sig.blocks[0].weak = 0x11223344;
    sig.blocks[1].weak = 0x55667788;
    const fc::Hash256 sigFileHash = MakeHash(33);

    const fc::HashFingerprint sfp{4096, 555};
    sigCache.Upsert("big.bin", sfp, sigFileHash, sig);

    fc::Hash256 gotHash{};
    fc::delta::SignatureSet gotSig;
    Require(sigCache.TryGet("big.bin", sfp, gotHash, gotSig), "Expected block-sig cache hit");
    Require(gotHash == sigFileHash, "Expected cached file hash");
    Require(gotSig.fileSize == sig.fileSize && gotSig.blockSize == sig.blockSize &&
                gotSig.blockCount == sig.blockCount && gotSig.strongLen == sig.strongLen,
            "Expected cached signature header");
    Require(gotSig.blocks.size() == 2 && gotSig.blocks[0].weak == 0x11223344 &&
                gotSig.blocks[1].weak == 0x55667788,
            "Expected cached signature blocks");
    Require(sigCache.HitCount() == 1, "Expected one block-sig cache hit");

    // mtime/size change -> fingerprint mismatch -> eviction + miss (memcache edge).
    const fc::HashFingerprint sfpStale{4096, 556};
    Require(!sigCache.TryGet("big.bin", sfpStale, gotHash, gotSig),
            "Expected block-sig miss on fingerprint mismatch");
    Require(sigCache.EntryCount() == 0, "Expected block-sig eviction on mismatch");
}
