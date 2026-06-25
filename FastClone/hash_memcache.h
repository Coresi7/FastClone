#pragma once

#include "delta.h"
#include "file_index.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

namespace fc {

struct HashFingerprint {
    uint64_t fileSize = 0;
    int64_t mtimeNs = 0;
};

class ServerHashMemCache {
public:
    struct CachedEntry {
        HashFingerprint fingerprint;
        Hash256 hash;
    };

    void Configure(bool enabled);
    [[nodiscard]] bool Enabled() const;

    bool TryGet(const std::string& relPath, const HashFingerprint& fingerprint, Hash256& outHash);
    void Upsert(const std::string& relPath, const HashFingerprint& fingerprint, const Hash256& hash);

    [[nodiscard]] size_t EntryCount() const;
    [[nodiscard]] uint64_t HitCount() const;
    [[nodiscard]] uint64_t MissCount() const;

private:
    mutable std::mutex mu_;
    std::unordered_map<std::string, CachedEntry> cache_;
    uint64_t hits_ = 0;
    uint64_t misses_ = 0;
    bool enabled_ = false;
    bool configured_ = false;
};

// Block-signature cache for binary delta (binary-delta D-05 / FR-11). Same relPath key +
// HashFingerprint{fileSize, mtimeNs} eviction discipline as ServerHashMemCache, but stores
// the (potentially large) delta::SignatureSet blob in an independent map so the full-file
// hash hot path is untouched (zero regression). Shares the --enable-hash-memcache switch.
class BlockSigMemCache {
public:
    struct CachedSig {
        HashFingerprint fingerprint;
        Hash256 fileHash;
        delta::SignatureSet sig;
    };

    void Configure(bool enabled);
    [[nodiscard]] bool Enabled() const;

    // On a fingerprint hit, copies the cached full-file hash + signature set into the out
    // params and returns true.
    bool TryGet(const std::string& relPath, const HashFingerprint& fingerprint, Hash256& outFileHash, delta::SignatureSet& outSig);
    void Upsert(const std::string& relPath, const HashFingerprint& fingerprint, const Hash256& fileHash, const delta::SignatureSet& sig);

    [[nodiscard]] size_t EntryCount() const;
    [[nodiscard]] uint64_t HitCount() const;
    [[nodiscard]] uint64_t MissCount() const;

private:
    mutable std::mutex mu_;
    std::unordered_map<std::string, CachedSig> cache_;
    uint64_t hits_ = 0;
    uint64_t misses_ = 0;
    bool enabled_ = false;
    bool configured_ = false;
};

}  // namespace fc
