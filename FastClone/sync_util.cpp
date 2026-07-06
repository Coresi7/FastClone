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

// Design section A.3: --diag must also honour the FASTCLONE_DIAG environment variable,
// reusing the same boolean parsing as FASTCLONE_DEBUG.
bool IsDiagEnabled() {
    static const bool enabled = ParseBoolEnv("FASTCLONE_DIAG");
    return enabled;
}

bool IsFatalClientDisconnectReason(const std::string& reason) {
    if (reason.empty()) {
        return false;
    }
    // Maintenance contract (string-matched throw sites - keep in sync when editing):
    //   NegotiateHelloAsClient / HandshakeClientNew (FC6): "Server error: ...",
    //     "Server HELLO missing", "Protocol version mismatch: ...",
    //     "Server authentication rejected"
    //   HandshakeClientNew / HandshakeClientJoin (FC6 AuthOk role check): "Protocol error: ..."
    //   HandshakeAndResolveSession (FC6 server): "Authentication failed",
    //     "Protocol version mismatch: ..."
    //   recv thread (sync_engine.cpp): "client received wrong-direction/unknown frame: ..."
    //   RecvFrame (protocol.cpp): "RecvFrame desync: ..."
    // Future: replace with DisconnectReason enum at throw sites (see sync_util.h).
    // Handshake / configuration errors - retrying cannot succeed.
    if (reason.find("Protocol version mismatch") != std::string::npos) {
        return true;
    }
    if (reason.find("Server authentication rejected") != std::string::npos) {
        return true;
    }
    if (reason.find("Authentication failed") != std::string::npos) {
        return true;
    }
    if (reason.find("Server HELLO missing") != std::string::npos) {
        return true;
    }
    // AuthOk role/sessionId violations (review B-04 / B4-R1: JoinAck sessionId echo check)
    // - a malformed handshake reply cannot be fixed by retrying the same server.
    if (reason.find("Protocol error: ") == 0) {
        return true;
    }
    if (reason.find("Server error: ") == 0) {
        return true;
    }
    // Wire desync / wrong-direction frames - reconnecting will not realign the stream.
    if (reason.find("client received wrong-direction/unknown frame") != std::string::npos) {
        return true;
    }
    if (reason.find("RecvFrame desync:") != std::string::npos) {
        return true;
    }
    return false;
}

// The TryNormalizeMtimeToUnixNs implementation has moved to compare_phase.cpp (M1 fix: single source of
// truth for mtime normalization, shared by sync and FastCheck). This TU no longer redefines it; the symbol
// is provided by compare_phase.cpp, which is satisfied automatically by targets whose link closure includes
// compare_phase (FastClone / FastCloneTests).

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

#ifdef _WIN32
namespace {
bool LooksAbsoluteWindows(const std::wstring& w) {
    // Drive-absolute "X:\" / "X:/" or UNC "\\".
    if (w.size() >= 3 && w[1] == L':' && (w[2] == L'\\' || w[2] == L'/')) {
        return true;
    }
    if (w.size() >= 2 && w[0] == L'\\' && w[1] == L'\\') {
        return true;
    }
    return false;
}
}  // namespace

std::wstring ToExtendedLengthPath(const fs::path& path) {
    std::wstring w = path.wstring();
    // Already an extended ("\\?\") or device ("\\.\") path -> leave untouched.
    if (w.size() >= 4 && w[0] == L'\\' && w[1] == L'\\' &&
        (w[2] == L'?' || w[2] == L'.') && w[3] == L'\\') {
        return w;
    }
    // The \\?\ prefix requires a fully-qualified path; leave relative paths alone so a
    // relative root never gets corrupted (it simply keeps the legacy MAX_PATH limit).
    if (!LooksAbsoluteWindows(w)) {
        return w;
    }
    for (wchar_t& c : w) {
        if (c == L'/') {
            c = L'\\';
        }
    }
    if (w.size() >= 2 && w[0] == L'\\' && w[1] == L'\\') {
        return L"\\\\?\\UNC\\" + w.substr(2);  // \\server\share -> \\?\UNC\server\share
    }
    return L"\\\\?\\" + w;
}
#endif

fs::path JoinRel(const fs::path& root, const std::string& relPath) {
#ifdef _WIN32
    std::wstring full = root.wstring();
    for (wchar_t& c : full) {
        if (c == L'/') {
            c = L'\\';
        }
    }
    if (!(relPath == "." || relPath.empty())) {
        if (!full.empty() && full.back() != L'\\') {
            full.push_back(L'\\');
        }
        std::wstring relW = Utf8ToWide(relPath);
        for (wchar_t& c : relW) {
            if (c == L'/') {
                c = L'\\';
            }
        }
        full += relW;
    }
    return fs::path(ToExtendedLengthPath(fs::path(full)));
#else
    if (relPath == "." || relPath.empty()) {
        return root;
    }
    return root / fs::path(relPath);
#endif
}

void CreateDirectoriesLong(const fs::path& dir) {
#ifdef _WIN32
    std::wstring w = dir.wstring();
    while (!w.empty() && (w.back() == L'\\' || w.back() == L'/')) {
        w.pop_back();
    }
    if (w.empty()) {
        return;
    }
    // Find the index just past the volume root so we never try to "create" the root
    // itself (\\?\C:\, C:\, \\?\UNC\server\share\, \\server\share\).
    size_t start = 0;
    auto afterShare = [&](size_t from) -> size_t {
        const size_t server = w.find(L'\\', from);
        if (server == std::wstring::npos) {
            return w.size();
        }
        const size_t share = w.find(L'\\', server + 1);
        return (share == std::wstring::npos) ? w.size() : share + 1;
    };
    if (w.compare(0, 8, L"\\\\?\\UNC\\") == 0) {
        start = afterShare(8);
    } else if (w.compare(0, 4, L"\\\\?\\") == 0) {
        start = (w.size() >= 7) ? 7 : w.size();  // extended drive root, e.g. \\?\C:
    } else if (w.size() >= 3 && w[1] == L':') {
        start = 3;  // drive root, e.g. C:
    } else if (w.size() >= 2 && w[0] == L'\\' && w[1] == L'\\') {
        start = afterShare(2);  // UNC share root
    }
    // Create each ancestor in order (shallow -> deep), then the directory itself.
    for (size_t i = start; i < w.size(); ++i) {
        if (w[i] == L'\\') {
            CreateDirectoryW(w.substr(0, i).c_str(), nullptr);  // ignore "exists"/root errors
        }
    }
    CreateDirectoryW(w.c_str(), nullptr);
#else
    std::error_code ec;
    fs::create_directories(dir, ec);
#endif
}

void EnsureParentDir(const fs::path& filePath) {
#ifdef _WIN32
    std::wstring full = filePath.wstring();
    while (!full.empty() && (full.back() == L'\\' || full.back() == L'/')) {
        full.pop_back();
    }
    const size_t sep = full.find_last_of(L"\\/");
    if (sep == std::wstring::npos || sep == 0) {
        return;
    }
    CreateDirectoriesLong(fs::path(full.substr(0, sep)));
#else
    const fs::path parent = filePath.parent_path();
    if (!parent.empty()) {
        std::error_code ec;
        fs::create_directories(parent, ec);
    }
#endif
}

namespace {
// True when dir currently exists as a directory. Used only on the EnsureParentDir cache MISS path
// (never on a hit), so it does not add syscalls to the fast path.
bool DirExists(const fs::path& dir) {
#ifdef _WIN32
    const DWORD attrs = GetFileAttributesW(dir.wstring().c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
    std::error_code ec;
    return fs::is_directory(dir, ec);
#endif
}
}  // namespace

std::optional<fs::path> PerWorkerDirCache::ParentDirOf(const fs::path& filePath) {
#ifdef _WIN32
    // Byte-for-byte the same parent computation as EnsureParentDir(filePath): strip trailing
    // separators, take everything before the last separator. sep==0 / npos means no real parent.
    std::wstring full = filePath.wstring();
    while (!full.empty() && (full.back() == L'\\' || full.back() == L'/')) {
        full.pop_back();
    }
    const size_t sep = full.find_last_of(L"\\/");
    if (sep == std::wstring::npos || sep == 0) {
        return std::nullopt;
    }
    return fs::path(full.substr(0, sep));
#else
    const fs::path parent = filePath.parent_path();
    if (parent.empty()) {
        return std::nullopt;
    }
    return parent;
#endif
}

PerWorkerDirCache::Key PerWorkerDirCache::KeyOf(const fs::path& dir) {
#ifdef _WIN32
    std::wstring w = dir.wstring();
    while (!w.empty() && (w.back() == L'\\' || w.back() == L'/')) {
        w.pop_back();
    }
    return w;
#else
    std::string s = dir.string();
    while (s.size() > 1 && s.back() == '/') {
        s.pop_back();
    }
    return s;
#endif
}

bool PerWorkerDirCache::contains(const fs::path& dir) const {
    return known_.find(KeyOf(dir)) != known_.end();
}

void PerWorkerDirCache::addWithAncestors(const fs::path& dir) {
#ifdef _WIN32
    // Reuse CreateDirectoriesLong's volume-root/segment logic so every ancestor key is identical to
    // what a later contains() query for a sibling/descendant path would produce.
    std::wstring w = dir.wstring();
    while (!w.empty() && (w.back() == L'\\' || w.back() == L'/')) {
        w.pop_back();
    }
    if (w.empty()) {
        return;
    }
    size_t start = 0;
    auto afterShare = [&](size_t from) -> size_t {
        const size_t server = w.find(L'\\', from);
        if (server == std::wstring::npos) {
            return w.size();
        }
        const size_t share = w.find(L'\\', server + 1);
        return (share == std::wstring::npos) ? w.size() : share + 1;
    };
    if (w.compare(0, 8, L"\\\\?\\UNC\\") == 0) {
        start = afterShare(8);
    } else if (w.compare(0, 4, L"\\\\?\\") == 0) {
        start = (w.size() >= 7) ? 7 : w.size();
    } else if (w.size() >= 3 && w[1] == L':') {
        start = 3;
    } else if (w.size() >= 2 && w[0] == L'\\' && w[1] == L'\\') {
        start = afterShare(2);
    }
    for (size_t i = start; i < w.size(); ++i) {
        if (w[i] == L'\\') {
            known_.insert(w.substr(0, i));
        }
    }
    known_.insert(w);
#else
    fs::path cur = dir;
    while (!cur.empty()) {
        const std::string key = KeyOf(cur);
        if (key.empty()) {
            break;
        }
        known_.insert(key);
        const fs::path parent = cur.parent_path();
        if (parent == cur) {
            break;  // reached the filesystem root
        }
        cur = parent;
    }
#endif
}

void EnsureParentDir(const fs::path& filePath, PerWorkerDirCache& cache) {
    EnsureParentDirCached(filePath, cache, [](const fs::path& parent) -> bool {
        CreateDirectoriesLong(parent);
        return DirExists(parent);
    });
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
