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
#endif

// Normalize a raw manifest mtime (which may be Unix ns or Windows FILETIME ticks) to
// Unix nanoseconds. Returns false when the value cannot be meaningfully converted.
bool TryNormalizeMtimeToUnixNs(int64_t rawMtime, int64_t& outUnixNs);

std::filesystem::path JoinRel(const std::filesystem::path& root, const std::string& relPath);
void EnsureParentDir(const std::filesystem::path& filePath);
std::optional<std::filesystem::path> CurrentExePath();

}  // namespace fc
