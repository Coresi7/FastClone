#pragma once

// Shared test fixture for the shared probe / extra-scan unit tests
// (task unify-probe-extra-shared, design §8.2). Header-only, included by BOTH
// FastCloneTests (tests/test_shared_probe.cpp / tests/test_shared_extra_scan.cpp,
// same-directory quote-include hit) and FastCheckTests (FastCheck/tests/test_shared_probe.cpp
// / FastCheck/tests/test_shared_extra_scan.cpp, which reach it through the tests/ include
// directory added to the FastCheckTests target only - the FastCheck production target's
// include path carries no test directories). Relocated from FastClone/ to tests/ in the
// unify-probe-extra-shared-converge cleanup: it is test-only code and was never part of
// any production source set (design §6.2).

#include "file_index.h"  // FileEntry / SetFileModifyTime / ToUnixNs
#include "sync_util.h"   // fc::CreateDirectoriesLong (long-path-safe directory creation)

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace fc::test {

// Fixture layout (design §8.2.1). Every file gets a FIXED mtime (Unix ns, a multiple of
// 100 ns so the Windows ns -> FILETIME-ticks round-trip is lossless), making the
// expected dump deterministic and identical in FastCloneTests and FastCheckTests (AC-03).
struct ProbeFixture {
    std::filesystem::path root;
    std::vector<std::string> relPaths;  // ascending byte order
};

// Fixed mtimes: > 5e17 (genuine Unix ns for the POSIX-side normalization), 1 s apart.
inline constexpr int64_t kFixtureMtimeBaseNs = 1700000000000000000LL;

inline int64_t FixtureMtimeNs(int index) {
    return kFixtureMtimeBaseNs + static_cast<int64_t>(index) * 1000000000LL;
}

// The mtime value the PLATFORM probe is expected to report for a file whose mtime was
// set to unixNs via SetFileModifyTime (AC-01/AC-02):
//   Windows: raw FILETIME ticks (100 ns since 1601) - SetFileModifyTime converts Unix ns
//            to FILETIME internally and the shared probe reports raw ticks (§4.5/D-08);
//   POSIX  : Unix ns unchanged (ToUnixNs round-trip; assumes a ns-granularity fs - the
//            FASTCLONE_TEST_DUMP_DIR fallback covers coarse-granularity filesystems, R-06).
inline int64_t ExpectedProbeMtimeNs(int64_t unixNs) {
#ifdef _WIN32
    constexpr int64_t kWindowsEpochDiff100ns = 116444736000000000LL;
    return unixNs / 100 + kWindowsEpochDiff100ns;
#else
    return unixNs;
#endif
}

inline void WriteBinaryFile(const std::filesystem::path& path, size_t size) {
#ifdef _WIN32
    // ToExtendedLengthPath normalizes separators AND adds the "\\?\" prefix - both are
    // required: CreateDirectoriesLong walks by '\' only, and its per-layer
    // CreateDirectoryW calls can only exceed MAX_PATH through the prefix.
    if (!path.parent_path().empty()) {
        fc::CreateDirectoriesLong(std::filesystem::path(fc::ToExtendedLengthPath(path.parent_path())));
    }
    // Open through the extended-length form so B-11 fixtures beyond MAX_PATH can be
    // written at all (a plain ofstream open silently fails on >260-char paths).
    const std::filesystem::path openPath(fc::ToExtendedLengthPath(path));
#else
    if (!path.parent_path().empty()) {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
    }
    const std::filesystem::path openPath = path;
#endif
    std::ofstream out(openPath, std::ios::binary | std::ios::trunc);
    for (size_t i = 0; i < size; ++i) {
        out.put(static_cast<char>(i & 0xFF));
    }
}

inline ProbeFixture MakeProbeFixture() {
    using clock = std::chrono::steady_clock;
    const auto stamp = clock::now().time_since_epoch().count();
    ProbeFixture fx;
    fx.root = std::filesystem::temp_directory_path() /
              ("fc_shared_probe_" + std::to_string(stamp));
    std::filesystem::create_directories(fx.root);

    struct Spec {
        const char* rel;
        size_t size;
    };
    // relPath ascending byte order (matches the dump order below).
    const Spec specs[] = {
        {"a.txt", 50},
        {"b.bin", 100 * 1024},
        {"empty.bin", 0},
        {"nested/c.dat", 1024},
        {"nested/deep/d.dat", 2 * 1024},
        {"\xe4\xb8\xad\xe6\x96\x87\xe5\x90\x8d-\xce\xa9.bin", 7},  // 中文名-Ω.bin (UTF-8)
    };
    int mtimeIndex = 0;
    for (const Spec& s : specs) {
        const std::filesystem::path abs = fx.root / s.rel;
        WriteBinaryFile(abs, s.size);
        SetFileModifyTime(abs, FixtureMtimeNs(mtimeIndex++));
        fx.relPaths.push_back(s.rel);
    }
    return fx;
}

// Serialize probe results (or expected values) as "relPath|fileSize|mtimeNs|isDirectory"
// lines in ascending relPath order - the AC-03 byte-equality dump format.
inline std::string BuildProbeDump(const std::vector<FileEntry>& entries) {
    std::string dump;
    for (const FileEntry& e : entries) {
        dump += e.relativePath;
        dump += '|';
        dump += std::to_string(e.fileSize);
        dump += '|';
        dump += std::to_string(e.mtimeNs);
        dump += '|';
        dump += e.isDirectory ? "1" : "0";
        dump += '\n';
    }
    return dump;
}

// The expected dump for MakeProbeFixture() (same constant on both sides, AC-03).
inline std::string BuildExpectedProbeDump() {
    std::vector<FileEntry> expected;
    struct Spec {
        const char* rel;
        size_t size;
    };
    const Spec specs[] = {
        {"a.txt", 50},
        {"b.bin", 100 * 1024},
        {"empty.bin", 0},
        {"nested/c.dat", 1024},
        {"nested/deep/d.dat", 2 * 1024},
        {"\xe4\xb8\xad\xe6\x96\x87\xe5\x90\x8d-\xce\xa9.bin", 7},
    };
    int i = 0;
    for (const Spec& s : specs) {
        FileEntry e;
        e.relativePath = s.rel;
        e.isDirectory = false;
        e.fileSize = s.size;
        e.mtimeNs = ExpectedProbeMtimeNs(FixtureMtimeNs(i++));
        expected.push_back(std::move(e));
    }
    return BuildProbeDump(expected);
}

// R-06 fallback: when FASTCLONE_TEST_DUMP_DIR is set, also persist the actual dump to
// <dir>/<name>.txt so the L4 scripts can diff the two sides on coarse-granularity
// filesystems where the hardcoded expectation would be flaky.
inline void WriteDumpIfRequested(const char* name, const std::string& dump) {
    const char* dir = std::getenv("FASTCLONE_TEST_DUMP_DIR");
    if (dir == nullptr || *dir == '\0') {
        return;
    }
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::filesystem::path out =
        std::filesystem::path(dir) / (std::string(name) + ".txt");
    std::ofstream f(out, std::ios::binary | std::ios::trunc);
    f << dump;
}

}  // namespace fc::test
