#include "sync_util.h"

#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace {

void Expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error("test_sync_util: " + message);
    }
}

fs::path MakeTempDir() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path dir = fs::temp_directory_path() / ("fastclone_su_" + std::to_string(stamp));
    fs::create_directories(dir);
    return dir;
}

// Counting / scriptable EnsureFn double: records how many filesystem create attempts the cache lets
// through, and can be forced to report failure (simulating a create that did not produce a directory).
struct CountingEnsure {
    int calls = 0;
    bool succeed = true;
    bool operator()(const fs::path&) {
        ++calls;
        return succeed;
    }
};

}  // namespace

// fastcheck-perf-tune Change 2 (V-03a..e): PerWorkerDirCache + EnsureParentDirCached. Uses the
// injectable ensure operator to observe exactly how many create attempts reach the filesystem, so the
// cache hit/miss/ancestor/failure/multi-worker contracts (AC-12..16) are verified deterministically.
void RunSyncUtilTests() {
    const fs::path root = MakeTempDir();

    // V-03b miss (AC-13): a brand-new parent invokes the ensure operator exactly once.
    {
        fc::PerWorkerDirCache cache;
        CountingEnsure ensure;
        const fs::path f = fc::JoinRel(root, "d1/file.txt");
        fc::EnsureParentDirCached(f, cache, [&](const fs::path& p) { return ensure(p); });
        Expect(ensure.calls == 1, "miss path invokes ensure exactly once (AC-13)");
    }

    // V-03a hit (AC-12): a second file under the SAME parent on the same cache does not re-invoke it.
    {
        fc::PerWorkerDirCache cache;
        CountingEnsure ensure;
        const fs::path f1 = fc::JoinRel(root, "d2/file1.txt");
        const fs::path f2 = fc::JoinRel(root, "d2/file2.txt");
        fc::EnsureParentDirCached(f1, cache, [&](const fs::path& p) { return ensure(p); });
        fc::EnsureParentDirCached(f2, cache, [&](const fs::path& p) { return ensure(p); });
        Expect(ensure.calls == 1, "second file under same parent hits cache, no extra ensure (AC-12)");
    }

    // V-03c ancestor (AC-14): after a deep parent is recorded, a file under an ANCESTOR of it hits.
    {
        fc::PerWorkerDirCache cache;
        CountingEnsure ensure;
        const fs::path deep = fc::JoinRel(root, "a/b/c/file.txt");  // parent = <root>/a/b/c
        const fs::path midd = fc::JoinRel(root, "a/b/other.txt");   // parent = <root>/a/b (ancestor)
        fc::EnsureParentDirCached(deep, cache, [&](const fs::path& p) { return ensure(p); });
        fc::EnsureParentDirCached(midd, cache, [&](const fs::path& p) { return ensure(p); });
        Expect(ensure.calls == 1, "ancestor of a cached deep dir hits cache (AC-14)");
    }

    // V-03d fail-not-cached (AC-15): a failed create is NOT cached, so the same parent misses again.
    {
        fc::PerWorkerDirCache cache;
        CountingEnsure ensure;
        ensure.succeed = false;
        const fs::path f = fc::JoinRel(root, "fail/file.txt");
        fc::EnsureParentDirCached(f, cache, [&](const fs::path& p) { return ensure(p); });
        fc::EnsureParentDirCached(f, cache, [&](const fs::path& p) { return ensure(p); });
        Expect(ensure.calls == 2, "failed create is not cached; retried on miss path (AC-15)");
    }

    // V-03e multi-worker (AC-16): two independent per-worker caches processing the SAME parent both
    // run the miss path -- no cross-worker sharing, no failure propagation, no global lock.
    {
        fc::PerWorkerDirCache cacheA;
        fc::PerWorkerDirCache cacheB;
        CountingEnsure ensureA;
        CountingEnsure ensureB;
        const fs::path f = fc::JoinRel(root, "shared/file.txt");
        fc::EnsureParentDirCached(f, cacheA, [&](const fs::path& p) { return ensureA(p); });
        fc::EnsureParentDirCached(f, cacheB, [&](const fs::path& p) { return ensureB(p); });
        Expect(ensureA.calls == 1 && ensureB.calls == 1,
               "each per-worker cache resolves the shared parent independently (AC-16)");
        fc::EnsureParentDirCached(f, cacheA, [&](const fs::path& p) { return ensureA(p); });
        fc::EnsureParentDirCached(f, cacheB, [&](const fs::path& p) { return ensureB(p); });
        Expect(ensureA.calls == 1 && ensureB.calls == 1,
               "per-worker caches independently hit on the second pass (AC-16)");
    }

    // Boundary: a path with no meaningful parent must not cache anything and must not invoke ensure.
    {
        fc::PerWorkerDirCache cache;
        CountingEnsure ensure;
        fc::EnsureParentDirCached(fs::path("bare_name_no_parent"), cache,
                                  [&](const fs::path& p) { return ensure(p); });
        Expect(ensure.calls == 0, "path without a real parent invokes no ensure and caches nothing");
        Expect(cache.size() == 0, "no invalid cache entry recorded for a parentless path");
    }

    // Production overload smoke: EnsureParentDir(path, cache) actually creates the parent on a miss
    // and the directory survives a subsequent cache-hit call.
    {
        fc::PerWorkerDirCache cache;
        const fs::path f = fc::JoinRel(root, "real/nested/file.bin");
        fc::EnsureParentDir(f, cache);
        Expect(fs::is_directory(root / "real" / "nested"),
               "production EnsureParentDir(path,cache) creates the parent directory on miss");
        fc::EnsureParentDir(f, cache);
        Expect(fs::is_directory(root / "real" / "nested"),
               "cache-hit EnsureParentDir leaves the directory intact");
    }

    std::error_code ec;
    fs::remove_all(root, ec);
}
