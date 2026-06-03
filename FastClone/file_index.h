#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace fc {

struct FileEntry {
    std::string relativePath;
    bool isDirectory = false;
    uint64_t fileSize = 0;
    int64_t mtimeNs = 0;
};

// Protocol name is kept as Hash256 for compatibility, but value is XXH3_128 (16 bytes).
using Hash256 = std::array<uint8_t, 16>;

std::vector<FileEntry> BuildIndex(const std::filesystem::path& root, const std::optional<std::filesystem::path>& excludeAbsPath);
Hash256 ComputeFileHash(const std::filesystem::path& path);
bool HashEquals(const Hash256& a, const Hash256& b);
std::string NormalizeRelativePath(const std::filesystem::path& relativePath);
int64_t ToUnixNs(const std::filesystem::file_time_type& value);
std::filesystem::file_time_type FromUnixNs(int64_t valueNs);
void SetFileModifyTime(const std::filesystem::path& path, int64_t modifyNs);

// Canonical mtime read used by both manifest build and client local probe so the
// two sides compare values in the same unit/epoch.
//  - Windows: raw FILETIME ticks (100ns since 1601), matching the manifest writer;
//    on read failure falls back to ToUnixNs(fs::last_write_time).
//  - POSIX:   ToUnixNs(fs::last_write_time) (both ends already share this unit).
// Returns 0 when the timestamp cannot be read (same failure semantics as the
// previous inline probe logic).
int64_t ReadFileMtimeCanonical(const std::filesystem::path& path);

}  // namespace fc
