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
}
