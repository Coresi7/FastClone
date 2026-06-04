#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace fc {

std::optional<std::string> ReadEnvVar(const char* name);
bool ParseBoolEnv(const char* name);
bool IsDebugEnabled();
bool IsDiagEnabled();

#ifdef _WIN32
std::wstring Utf8ToWide(const std::string& value);
std::string WideToUtf8(const std::wstring& value);
// Convert an absolute path to a Windows extended-length ("\\?\") path so file operations
// bypass the legacy MAX_PATH (260) limit. Forward slashes are converted to backslashes
// (the prefix disables path normalization). Relative or already-prefixed paths are
// returned unchanged.
std::wstring ToExtendedLengthPath(const std::filesystem::path& path);
#endif

// create_directories() that is safe on Windows extended-length ("\\?\") paths (and on
// paths exceeding MAX_PATH). On POSIX this is just std::filesystem::create_directories.
void CreateDirectoriesLong(const std::filesystem::path& dir);

// Normalize a raw manifest mtime (which may be Unix ns or Windows FILETIME ticks) to
// Unix nanoseconds. Returns false when the value cannot be meaningfully converted.
bool TryNormalizeMtimeToUnixNs(int64_t rawMtime, int64_t& outUnixNs);

std::filesystem::path JoinRel(const std::filesystem::path& root, const std::string& relPath);
void EnsureParentDir(const std::filesystem::path& filePath);
std::optional<std::filesystem::path> CurrentExePath();

}  // namespace fc
