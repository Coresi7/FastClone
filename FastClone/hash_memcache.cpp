#include "hash_memcache.h"

#include <stdexcept>

namespace fc {

void ServerHashMemCache::Configure(bool enabled) {
    std::lock_guard<std::mutex> lock(mu_);
    if (configured_ && enabled_ != enabled) {
        throw std::runtime_error("Server hash memcache already configured with different mode");
    }
    enabled_ = enabled;
    configured_ = true;
}

bool ServerHashMemCache::Enabled() const {
    std::lock_guard<std::mutex> lock(mu_);
    return enabled_;
}

bool ServerHashMemCache::TryGet(const std::string& relPath, const HashFingerprint& fingerprint, Hash256& outHash) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!enabled_) {
        return false;
    }
    const auto it = cache_.find(relPath);
    if (it == cache_.end()) {
        ++misses_;
        return false;
    }
    if (it->second.fingerprint.fileSize != fingerprint.fileSize ||
        it->second.fingerprint.mtimeNs != fingerprint.mtimeNs) {
        cache_.erase(it);
        ++misses_;
        return false;
    }
    outHash = it->second.hash;
    ++hits_;
    return true;
}

void ServerHashMemCache::Upsert(const std::string& relPath, const HashFingerprint& fingerprint, const Hash256& hash) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!enabled_) {
        return;
    }
    cache_[relPath] = CachedEntry{fingerprint, hash};
}

size_t ServerHashMemCache::EntryCount() const {
    std::lock_guard<std::mutex> lock(mu_);
    return cache_.size();
}

uint64_t ServerHashMemCache::HitCount() const {
    std::lock_guard<std::mutex> lock(mu_);
    return hits_;
}

uint64_t ServerHashMemCache::MissCount() const {
    std::lock_guard<std::mutex> lock(mu_);
    return misses_;
}

}  // namespace fc
