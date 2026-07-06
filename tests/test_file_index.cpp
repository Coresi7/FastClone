#include "file_index.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;

namespace {

void Expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error("test_file_index: " + message);
    }
}

fs::path MakeTempDir() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path dir = fs::temp_directory_path() / ("fastclone_fi_" + std::to_string(stamp));
    fs::create_directories(dir);
    return dir;
}

void WriteFile(const fs::path& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << content;
}

}  // namespace

void RunBuildIndexFileSizeTests();

void RunFileIndexTests() {
    const fs::path dir = MakeTempDir();
    const fs::path file = dir / "sample.bin";
    WriteFile(file, "fastclone-mtime-canonical");

    // A readable, existing file must produce a non-zero canonical mtime.
    const int64_t mtime = fc::ReadFileMtimeCanonical(file);
    Expect(mtime != 0, "canonical mtime should be non-zero for an existing file");

    // The reader must be deterministic when the file is untouched.
    const int64_t mtimeAgain = fc::ReadFileMtimeCanonical(file);
    Expect(mtime == mtimeAgain, "canonical mtime must be stable across repeated reads");

    // BuildIndex must report the same canonical mtime unit for the same file,
    // i.e. the manifest side and the probe side now agree (FR-01 regression guard).
    const std::vector<fc::FileEntry> index = fc::BuildIndex(dir, std::nullopt);
    bool foundFile = false;
    for (const fc::FileEntry& entry : index) {
        if (!entry.isDirectory && entry.relativePath == "sample.bin") {
            foundFile = true;
            Expect(entry.mtimeNs == mtime,
                   "BuildIndex mtime must match ReadFileMtimeCanonical for the same file");
        }
    }
    Expect(foundFile, "BuildIndex should enumerate the sample file");

    // A non-existent path returns the documented failure sentinel (0).
    const int64_t missing = fc::ReadFileMtimeCanonical(dir / "does_not_exist.bin");
    Expect(missing == 0, "missing file must yield 0 canonical mtime");

    std::error_code ec;
    fs::remove_all(dir, ec);

    RunBuildIndexFileSizeTests();
}

// fastcheck-perf-tune Change 3 (V-04a, AC-21): BuildIndex now captures directory_entry::file_size at
// iteration time and reuses it in the worker (no second fs::file_size on the path). The reported
// fileSize must match the real content length across empty/small/large files, byte-for-byte with the
// former worker-stage fs::file_size result; directories must keep fileSize=0.
void RunBuildIndexFileSizeTests() {
    const fs::path dir = MakeTempDir();
    struct Case {
        std::string name;
        size_t size;
    };
    const std::vector<Case> cases = {
        {"empty.bin", 0u},
        {"one.bin", 1u},
        {"small.bin", 4096u},
        {"large.bin", (2u << 20) + 123u},  // > 1 MiB, unaligned
    };
    for (const Case& c : cases) {
        std::string content;
        content.resize(c.size);
        for (size_t i = 0; i < c.size; ++i) {
            content[i] = static_cast<char>((i * 37u + 11u) & 0xFF);
        }
        WriteFile(dir / c.name, content);
    }
    fs::create_directories(dir / "child");

    const std::vector<fc::FileEntry> index = fc::BuildIndex(dir, std::nullopt);
    for (const Case& c : cases) {
        bool found = false;
        for (const fc::FileEntry& entry : index) {
            if (!entry.isDirectory && entry.relativePath == c.name) {
                found = true;
                Expect(entry.fileSize == c.size,
                       "BuildIndex captured fileSize must equal the real content length: " + c.name);
            }
        }
        Expect(found, "BuildIndex should enumerate " + c.name);
    }
    // Directories always report fileSize=0 (FR-15 boundary: file_size never queried for dirs).
    for (const fc::FileEntry& entry : index) {
        if (entry.isDirectory && entry.relativePath == "child") {
            Expect(entry.fileSize == 0, "directory entry keeps fileSize=0");
        }
    }

    std::error_code ec;
    fs::remove_all(dir, ec);
}
