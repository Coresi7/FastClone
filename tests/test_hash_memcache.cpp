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
}
