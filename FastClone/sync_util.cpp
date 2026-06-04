#include "sync_util.h"

#ifdef _WIN32
#include <Windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <unistd.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <vector>

namespace fs = std::filesystem;

namespace fc {

std::optional<std::string> ReadEnvVar(const char* name) {
#if defined(_WIN32) && defined(_MSC_VER)
    char* raw = nullptr;
    size_t len = 0;
    const errno_t rc = _dupenv_s(&raw, &len, name);
    if (rc != 0 || raw == nullptr) {
        return std::nullopt;
    }
    std::string value(raw);
    std::free(raw);
    return value;
#else
    const char* env = std::getenv(name);
    if (env == nullptr) {
        return std::nullopt;
    }
    return std::string(env);
#endif
}

bool ParseBoolEnv(const char* name) {
    const std::optional<std::string> env = ReadEnvVar(name);
    if (!env.has_value()) {
        return false;
    }
    std::string v = *env;
    std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return v == "1" || v == "true" || v == "yes" || v == "on";
}

bool IsDebugEnabled() {
    static const bool enabled = ParseBoolEnv("FASTCLONE_DEBUG");
    return enabled;
}

// Design §A.3: --diag must also honour the FASTCLONE_DIAG environment variable,
// reusing the same boolean parsing as FASTCLONE_DEBUG.
bool IsDiagEnabled() {
    static const bool enabled = ParseBoolEnv("FASTCLONE_DIAG");
    return enabled;
}

bool TryNormalizeMtimeToUnixNs(int64_t rawMtime, int64_t& outUnixNs) {
    // Heuristic shared with file_index.cpp compatibility logic:
    // values above this threshold are very likely Unix nanoseconds.
    constexpr int64_t kLikelyUnixNsThreshold = 500000000000000000LL;
    // 1970-01-01 UTC offset in FILETIME 100ns ticks since 1601-01-01.
    constexpr int64_t kWindowsEpochDiff100ns = 116444736000000000LL;

    if (rawMtime > kLikelyUnixNsThreshold) {
        outUnixNs = rawMtime;
        return true;
    }
    // Treat lower values as FILETIME ticks. If conversion is not meaningful
    // (e.g. unknown/legacy sentinel), caller can fall back to raw compare.
    if (rawMtime < kWindowsEpochDiff100ns) {
        return false;
    }
    const int64_t ticksSinceUnixEpoch = rawMtime - kWindowsEpochDiff100ns;
    if (ticksSinceUnixEpoch > (std::numeric_limits<int64_t>::max)() / 100LL ||
        ticksSinceUnixEpoch < (std::numeric_limits<int64_t>::min)() / 100LL) {
        return false;
    }
    outUnixNs = ticksSinceUnixEpoch * 100LL;
    return true;
}

#ifdef _WIN32
std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) {
        return {};
    }
    const int len = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0);
    if (len <= 0) {
        throw std::runtime_error("MultiByteToWideChar failed");
    }
    std::wstring output(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), output.data(), len);
    return output;
}

std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }
    const int len = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (len <= 0) {
        throw std::runtime_error("WideCharToMultiByte failed");
    }
    std::string output(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), output.data(), len, nullptr, nullptr);
    return output;
}
#endif

fs::path JoinRel(const fs::path& root, const std::string& relPath) {
    if (relPath == "." || relPath.empty()) {
        return root;
    }
#ifdef _WIN32
    return root / fs::path(Utf8ToWide(relPath));
#else
    return root / fs::path(relPath);
#endif
}

void EnsureParentDir(const fs::path& filePath) {
    const fs::path parent = filePath.parent_path();
    if (!parent.empty()) {
        std::error_code ec;
        fs::create_directories(parent, ec);
    }
}

std::optional<fs::path> CurrentExePath() {
#ifdef _WIN32
    wchar_t pathBuf[MAX_PATH];
    const DWORD len = GetModuleFileNameW(nullptr, pathBuf, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return std::nullopt;
    }
    return fs::weakly_canonical(fs::path(pathBuf));
#elif defined(__linux__)
    std::vector<char> buf(4096);
    const ssize_t len = readlink("/proc/self/exe", buf.data(), buf.size() - 1);
    if (len <= 0) {
        return std::nullopt;
    }
    buf[static_cast<size_t>(len)] = '\0';
    return fs::weakly_canonical(fs::path(buf.data()));
#elif defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    if (size == 0) {
        return std::nullopt;
    }
    std::vector<char> buf(size + 1, '\0');
    if (_NSGetExecutablePath(buf.data(), &size) != 0) {
        return std::nullopt;
    }
    return fs::weakly_canonical(fs::path(buf.data()));
#else
    return std::nullopt;
#endif
}

}  // namespace fc
