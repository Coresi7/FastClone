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

// UTF-8 bytes for a non-ASCII path, built from \x escapes so the source file stays pure ASCII.
// This makes the test independent of both the compiler's source charset and the system ANSI
// codepage (CP_ACP). "中文目录" / "文件.txt":
//   中=E4 B8 AD  文=E6 96 87  目=E7 9B AE  录=E5 BD 95  件=E4 BB B6
const std::string kUtf8DirName = "\xe4\xb8\xad\xe6\x96\x87\xe7\x9b\xae\xe5\xbd\x95";  // 中文目录
const std::string kUtf8FileName = "\xe6\x96\x87\xe4\xbb\xb6.txt";                       // 文件.txt

// Build an fs::path from UTF-8 bytes WITHOUT going through the system ANSI codepage, so the
// construction itself is correct on every Windows locale (CP_ACP=936 included): on Windows we
// widen with CP_UTF8 and construct from the wide string; on POSIX the native byte string is UTF-8.
fs::path PathFromUtf8(const std::string& utf8) {
#ifdef _WIN32
    return fs::path(fc::Utf8ToWide(utf8));
#else
    return fs::path(utf8);
#endif
}

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

    // PathToUtf8 contract (Chinese-source-directory fix). The disk IO backend decodes path
    // std::strings as UTF-8 (CP_UTF8), but on Windows path::string() yields CP_ACP bytes (GBK on a
    // Simplified-Chinese locale), which corrupts any non-ASCII path. PathToUtf8 goes through
    // wstring()+WideToUtf8 so the output is always UTF-8. These assertions are portable on every
    // locale; on CP_ACP=936 they also catch a regression to `return path.string();` (the bytes
    // would differ). On CP_ACP=65001 path::string() already yields UTF-8, so the two implementations
    // converge and the guard is inert there -- that is a physical property of CP_ACP=65001, not a
    // test gap; the contract asserted below (round-trip == original UTF-8) is correct everywhere.
    {
        const fs::path fileOnly = PathFromUtf8(kUtf8FileName);
        Expect(fc::PathToUtf8(fileOnly) == kUtf8FileName,
               "PathToUtf8 preserves non-ASCII filename bytes as UTF-8 (CP_ACP-independent)");

        const fs::path nested = PathFromUtf8(kUtf8DirName) / PathFromUtf8(kUtf8FileName);
#ifdef _WIN32
        Expect(fc::Utf8ToWide(fc::PathToUtf8(nested)) == nested.wstring(),
               "PathToUtf8 round-trips through Utf8ToWide to the original wide path");
        Expect(fc::PathToUtf8(nested) == fc::WideToUtf8(nested.wstring()),
               "PathToUtf8 must equal WideToUtf8(path.wstring()) on Windows (bypasses CP_ACP)");
#else
        Expect(fc::PathToUtf8(nested) == nested.string(),
               "PathToUtf8 == path.string() on POSIX (native UTF-8)");
#endif
    }

    std::error_code ec;
    fs::remove_all(root, ec);
}
