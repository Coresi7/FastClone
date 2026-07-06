#include "file_index.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>

namespace fs = std::filesystem;

namespace {

void Expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error("test_manifest_dirent: " + message);
    }
}

fs::path MakeTempDir() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path dir = fs::temp_directory_path() / ("fastclone_md_" + std::to_string(stamp));
    fs::create_directories(dir);
    return dir;
}

void WriteFile(const fs::path& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << content;
}

fs::directory_entry FindEntry(const fs::path& dir, const std::string& name) {
    for (const auto& e : fs::directory_iterator(dir)) {
        if (e.path().filename().string() == name) {
            return e;
        }
    }
    throw std::runtime_error("test_manifest_dirent: entry not found: " + name);
}

}  // namespace

// fastcheck-perf-tune Change 1 (V-02a): the server manifest now reads size/mtime from the current
// directory_entry (it->file_size(ec) / it->last_write_time(ec)) instead of a second independent stat
// on absPath. Because sync_engine_server.cpp is not linked into FastCloneTests (existing test-closure
// boundary), this proves the *API replacement* is byte-for-byte equivalent to the former calls:
// directory_entry cached values must equal fs::file_size / fs::last_write_time for the same path, the
// ec outcomes must agree, and ToUnixNs must produce the same mtime (AC-06/07/08/09/10).
void RunManifestDirentTests() {
    const fs::path dir = MakeTempDir();
    WriteFile(dir / "normal.bin", "fastclone-manifest-dirent-normal-content");
    WriteFile(dir / "empty.bin", "");
    fs::create_directories(dir / "subdir");

    for (const auto& e : fs::directory_iterator(dir)) {
        const fs::path abs = e.path();

        if (e.is_regular_file()) {
            std::error_code deSizeEc;
            std::error_code fsSizeEc;
            const auto deSize = e.file_size(deSizeEc);
            const auto fsSize = fs::file_size(abs, fsSizeEc);
            Expect(static_cast<bool>(deSizeEc) == static_cast<bool>(fsSizeEc),
                   "dirent vs fs file_size ec agreement for " + abs.filename().string());
            if (!deSizeEc && !fsSizeEc) {
                Expect(deSize == fsSize,
                       "dirent file_size == fs::file_size for " + abs.filename().string());
            }
        }

        std::error_code deMtEc;
        std::error_code fsMtEc;
        const auto deMt = e.last_write_time(deMtEc);
        const auto fsMt = fs::last_write_time(abs, fsMtEc);
        Expect(static_cast<bool>(deMtEc) == static_cast<bool>(fsMtEc),
               "dirent vs fs last_write_time ec agreement for " + abs.filename().string());
        if (!deMtEc && !fsMtEc) {
            // The manifest feeds both values through the SAME ToUnixNs; the results must be identical
            // (FR-06: mtime unit/rounding/sign unchanged).
            Expect(fc::ToUnixNs(deMt) == fc::ToUnixNs(fsMt),
                   "ToUnixNs(dirent mtime) == ToUnixNs(fs mtime) for " + abs.filename().string());
        }
    }

    // Directory entries: the manifest sets fileSize=0 and never calls file_size on them; confirm the
    // directory_entry classification is stable (special-type boundary, AC-10).
    {
        const fs::directory_entry sub = FindEntry(dir, "subdir");
        Expect(sub.is_directory(), "subdir classified as directory via directory_entry");
    }

    // Error-path equivalence (AC-10): enumerate, delete the file, then re-query. Both the refreshed
    // directory_entry::file_size(ec) and fs::file_size(path, ec) must report an error -> the manifest
    // skips the entry identically on either API.
    {
        const fs::path gone = dir / "normal.bin";
        fs::directory_entry de = FindEntry(dir, "normal.bin");
        std::error_code rmEc;
        fs::remove(gone, rmEc);
        Expect(!rmEc, "removed normal.bin for error-path check");

        std::error_code refreshEc;
        de.refresh(refreshEc);
        std::error_code deSizeEc;
        (void)de.file_size(deSizeEc);
        std::error_code fsSizeEc;
        (void)fs::file_size(gone, fsSizeEc);
        Expect(static_cast<bool>(deSizeEc) && static_cast<bool>(fsSizeEc),
               "deleted file: both dirent and fs file_size report an error (AC-10)");
    }

    std::error_code ec;
    fs::remove_all(dir, ec);
}
