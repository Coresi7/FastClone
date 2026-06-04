#pragma once

#include "file_index.h"
#include "protocol.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace fc {

// Wire record for a single file in a batch transfer. relativePath/fileSize/mtimeNs/ok
// travel on the wire; absPath is a server-side convenience that is never serialized.
struct BatchFileRecord {
    std::string relativePath;
    uint64_t fileSize = 0;
    int64_t mtimeNs = 0;
    bool ok = false;
    std::filesystem::path absPath;
};

std::vector<uint8_t> EncodeManifestEntry(const FileEntry& entry);
FileEntry DecodeManifestEntry(const std::vector<uint8_t>& payload);

std::vector<uint8_t> EncodeHashRequest(const std::string& relPath);
std::string DecodeHashRequest(const std::vector<uint8_t>& payload);

std::vector<uint8_t> EncodeHashResponse(const std::string& relPath, const Hash256& hash);
std::pair<std::string, Hash256> DecodeHashResponse(const std::vector<uint8_t>& payload);

std::vector<uint8_t> EncodeFileOpen(const std::string& relPath);
std::string DecodeFileOpen(const std::vector<uint8_t>& payload);

std::vector<uint8_t> EncodeFileBatchRequest(const std::vector<std::string>& relPaths);
std::vector<std::string> DecodeFileBatchRequest(const std::vector<uint8_t>& payload);

std::vector<uint8_t> EncodeFileBatchOpenResponse(const std::vector<BatchFileRecord>& files);
std::vector<BatchFileRecord> DecodeFileBatchOpenResponse(const std::vector<uint8_t>& payload);

}  // namespace fc
