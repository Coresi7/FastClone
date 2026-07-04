#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
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
// Same XXH3-128 digest + raw byte layout as ComputeFileHash, but over an in-memory buffer.
// Lets callers that already hold the file bytes (e.g. the delta block-signature path) avoid a
// second full-file read while producing a value the client's ComputeFileHash verify matches.
Hash256 ComputeBufferHash(const uint8_t* data, size_t len);
// Streaming XXH3-128 over a sequential byte source (fills dst up to maxLen, returns the count
// written; 0 means EOF). Produces the exact same Hash256 as ComputeFileHash over the same byte
// sequence -- XXH3 streaming is chunking-independent -- so the unified disk IO driver can supply
// the file content instead of an inline file read (unified-disk-io-driver C9/C10). Read errors are
// surfaced by the caller's source (it returns short/0 and sets its own flag); this function never
// throws on a short source, only on XXH3 state failures.
Hash256 ComputeHashFromSource(const std::function<size_t(uint8_t*, size_t)>& source);
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
