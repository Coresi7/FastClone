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
    int64_t ctimeNs = 0;
};

using Hash256 = std::array<uint8_t, 32>;

std::vector<FileEntry> BuildIndex(const std::filesystem::path& root, const std::optional<std::filesystem::path>& excludeAbsPath);
Hash256 ComputeFileHash(const std::filesystem::path& path);
bool HashEquals(const Hash256& a, const Hash256& b);
std::string NormalizeRelativePath(const std::filesystem::path& relativePath);
int64_t ToUnixNs(const std::filesystem::file_time_type& value);
std::filesystem::file_time_type FromUnixNs(int64_t valueNs);
void SetFileCreateAndModifyTime(const std::filesystem::path& path, int64_t createNs, int64_t modifyNs);

}  // namespace fc
