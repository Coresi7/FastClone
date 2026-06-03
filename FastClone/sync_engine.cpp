#include "sync_engine.h"

#include "file_index.h"
#include "hash_memcache.h"
#include "path_utils.h"
#include "protocol.h"
#include "win_socket.h"

#ifdef _WIN32
#include <Windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <unistd.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <condition_variable>
#include <cctype>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <queue>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;

namespace fc {

namespace {

constexpr const char* kProtocolVersion = "FC4";

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

struct ServerStream {
    std::ifstream input;
    std::string relativePath;
};

struct BatchFileRecord {
    std::string relativePath;
    uint64_t fileSize = 0;
    int64_t mtimeNs = 0;
    bool ok = false;
    fs::path absPath;
};

struct ServerBatchStream {
    std::vector<BatchFileRecord> files;
    size_t index = 0;
    bool headerSent = false;
    std::ifstream input;
    uint64_t remainingBytes = 0;
};

struct LocalState {
    std::unordered_map<std::string, FileEntry> files;
    std::unordered_set<std::string> directories;
};

struct ClientHashTask {
    std::string relPath;
    fs::path absPath;
};

enum class CompareAction {
    Skip,
    TransferNow,
    FallbackHash
};

struct TunedTransferOptions {
    uint32_t streamLimit = 16;
    uint32_t chunkSize = 256 * 1024;
};

TunedTransferOptions ResolveTransferOptions(const CliOptions& options) {
    TunedTransferOptions tuned;
    tuned.streamLimit = options.streamLimit;
    tuned.chunkSize = options.chunkSize;

    const uint32_t hw = std::max<uint32_t>(1, std::thread::hardware_concurrency());

    if (options.streamAutoTune) {
        // Keep default stream count conservative to reduce failure rate
        // on weak SSD/controllers when user doesn't explicitly set --streams.
        tuned.streamLimit = 4;
    }

    if (options.chunkAutoTune) {
        if (tuned.streamLimit <= 8) {
            tuned.chunkSize = 4 * 1024 * 1024;
        } else if (tuned.streamLimit <= 16) {
            tuned.chunkSize = 2 * 1024 * 1024;
        } else if (tuned.streamLimit <= 32) {
            tuned.chunkSize = 1024 * 1024;
        } else if (tuned.streamLimit <= 64) {
            tuned.chunkSize = 512 * 1024;
        } else {
            tuned.chunkSize = 256 * 1024;
        }
    }

    tuned.streamLimit = std::clamp<uint32_t>(tuned.streamLimit, 1, 1024);
    tuned.chunkSize = std::clamp<uint32_t>(tuned.chunkSize, 64 * 1024, 64 * 1024 * 1024);
    return tuned;
}

uint32_t EffectiveChunkSizeForStreams(uint32_t configuredChunkSize, uint32_t streamLimit);
size_t DownloadFlushThresholdForStreams(uint32_t streamLimit, uint32_t effectiveChunkSize);

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

CompareAction DecideCompareAction(const std::optional<FileEntry>& localFile, const FileEntry& remoteFile) {
    constexpr int64_t kMtimeToleranceNs = 2LL * 1000LL * 1000LL;  // 2ms tolerance
    constexpr int64_t kLegacyRawTolerance = 2LL * 1000LL * 1000LL;
    if (!localFile.has_value()) {
        return CompareAction::TransferNow;
    }
    if (localFile->fileSize != remoteFile.fileSize) {
        return CompareAction::TransferNow;
    }

    // Cross-platform compare: normalize both sides to Unix ns before tolerance check.
    int64_t localUnixNs = 0;
    int64_t remoteUnixNs = 0;
    const bool localNormalized = TryNormalizeMtimeToUnixNs(localFile->mtimeNs, localUnixNs);
    const bool remoteNormalized = TryNormalizeMtimeToUnixNs(remoteFile.mtimeNs, remoteUnixNs);
    if (localNormalized && remoteNormalized) {
        const int64_t mtimeDeltaNs = std::llabs(static_cast<long long>(localUnixNs - remoteUnixNs));
        if (mtimeDeltaNs <= kMtimeToleranceNs) {
            return CompareAction::Skip;
        }
    } else {
        // Compatibility fallback for legacy/invalid timestamp payloads.
        const int64_t rawDelta = std::llabs(static_cast<long long>(localFile->mtimeNs - remoteFile.mtimeNs));
        if (rawDelta <= kLegacyRawTolerance) {
            return CompareAction::Skip;
        }
    }
    return CompareAction::FallbackHash;
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

void SendSimple(const SocketHandle& socket, MsgType type, const std::string& text = {}) {
    Frame frame;
    frame.type = type;
    frame.streamId = 0;
    frame.payload.assign(text.begin(), text.end());
    SendFrame(socket, frame);
}

void EnsureHandshakeAsServer(const SocketHandle& socket, const std::string& password) {
    const Frame hello = RecvFrame(socket);
    if (hello.type != MsgType::Hello) {
        throw std::runtime_error("Expected HELLO");
    }
    const std::string clientVersion(reinterpret_cast<const char*>(hello.payload.data()), hello.payload.size());
    if (clientVersion != kProtocolVersion) {
        SendSimple(socket, MsgType::Error, "Protocol version mismatch: server=" + std::string(kProtocolVersion) + " client=" + clientVersion);
        throw std::runtime_error("Protocol version mismatch: server=" + std::string(kProtocolVersion) + " client=" + clientVersion);
    }
    SendSimple(socket, MsgType::Hello, kProtocolVersion);

    const Frame auth = RecvFrame(socket);
    if (auth.type != MsgType::Auth) {
        throw std::runtime_error("Expected AUTH");
    }
    const std::string got(reinterpret_cast<const char*>(auth.payload.data()), auth.payload.size());
    if (got != password) {
        SendSimple(socket, MsgType::AuthFail, "bad password");
        throw std::runtime_error("Authentication failed");
    }
    SendSimple(socket, MsgType::AuthOk, "ok");
}

void EnsureHandshakeAsClient(const SocketHandle& socket, const std::string& password) {
    SendSimple(socket, MsgType::Hello, kProtocolVersion);
    Frame helloBack = RecvFrame(socket);
    if (helloBack.type == MsgType::Error) {
        throw std::runtime_error(std::string(reinterpret_cast<const char*>(helloBack.payload.data()), helloBack.payload.size()));
    }
    if (helloBack.type != MsgType::Hello) {
        throw std::runtime_error("Server HELLO missing");
    }
    const std::string serverVersion(reinterpret_cast<const char*>(helloBack.payload.data()), helloBack.payload.size());
    if (serverVersion != kProtocolVersion) {
        throw std::runtime_error("Protocol version mismatch: client=" + std::string(kProtocolVersion) + " server=" + serverVersion);
    }
    SendSimple(socket, MsgType::Auth, password);
    Frame authResult = RecvFrame(socket);
    if (authResult.type != MsgType::AuthOk) {
        throw std::runtime_error("Server authentication rejected");
    }
}

std::vector<uint8_t> EncodeManifestEntry(const FileEntry& entry);

void EnumerateManifestEntriesFast(
    const fs::path& root,
    const std::optional<fs::path>& selfPath,
    const std::atomic<bool>& done,
    const std::function<void(Frame&&)>& enqueueManifestFrame) {
#ifdef _WIN32
    auto FileTimeToTicks = [](FILETIME ft) -> int64_t {
        ULARGE_INTEGER v{};
        v.LowPart = ft.dwLowDateTime;
        v.HighPart = ft.dwHighDateTime;
        return static_cast<int64_t>(v.QuadPart);
    };

    struct PendingDir {
        std::wstring absDir;
        std::string relDir;
    };

    const std::wstring rootW = root.wstring();
    const std::wstring selfW = selfPath.has_value() ? selfPath->wstring() : L"";

    std::vector<PendingDir> stack;
    stack.push_back(PendingDir{rootW, ""});

    size_t fileCount = 0;
    while (!stack.empty()) {
        if (done.load()) {
            return;
        }
        PendingDir current = std::move(stack.back());
        stack.pop_back();

        std::wstring pattern = current.absDir;
        if (!pattern.empty() && pattern.back() != L'\\' && pattern.back() != L'/') {
            pattern.push_back(L'\\');
        }
        pattern.append(L"*");

        WIN32_FIND_DATAW fd{};
        HANDLE hFind = FindFirstFileExW(
            pattern.c_str(),
            FindExInfoBasic,
            &fd,
            FindExSearchNameMatch,
            nullptr,
            FIND_FIRST_EX_LARGE_FETCH);
        if (hFind == INVALID_HANDLE_VALUE) {
            continue;
        }

        do {
            if (done.load()) {
                FindClose(hFind);
                return;
            }

            const wchar_t* name = fd.cFileName;
            if ((name[0] == L'.' && name[1] == L'\0') ||
                (name[0] == L'.' && name[1] == L'.' && name[2] == L'\0')) {
                continue;
            }

            const bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            std::wstring absPath = current.absDir;
            if (!absPath.empty() && absPath.back() != L'\\' && absPath.back() != L'/') {
                absPath.push_back(L'\\');
            }
            absPath.append(name);

            if (!isDir && !selfW.empty() && _wcsicmp(absPath.c_str(), selfW.c_str()) == 0) {
                continue;
            }

            const std::string nameUtf8 = WideToUtf8(name);
            std::string relPath = current.relDir.empty() ? nameUtf8 : (current.relDir + "/" + nameUtf8);

            FileEntry entry;
            entry.relativePath = relPath;
            entry.isDirectory = isDir;
            entry.fileSize = isDir ? 0 : (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
            entry.mtimeNs = FileTimeToTicks(fd.ftLastWriteTime);
            enqueueManifestFrame(Frame{MsgType::ManifestEntry, 0, EncodeManifestEntry(entry)});

            if (isDir) {
                stack.push_back(PendingDir{absPath, relPath});
            } else {
                ++fileCount;
                if (fileCount % 2048 == 0) {
                    std::vector<uint8_t> payload;
                    AppendU64(payload, static_cast<uint64_t>(fileCount));
                    enqueueManifestFrame(Frame{MsgType::ManifestProgress, 0, std::move(payload)});
                }
            }
        } while (FindNextFileW(hFind, &fd) != 0);

        FindClose(hFind);
    }

    std::vector<uint8_t> payload;
    AppendU64(payload, static_cast<uint64_t>(fileCount));
    enqueueManifestFrame(Frame{MsgType::ManifestProgress, 0, std::move(payload)});
    enqueueManifestFrame(Frame{MsgType::ManifestEnd, 0, {}});
#else
    size_t fileCount = 0;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
         it != end;
         it.increment(ec)) {
        if (done.load()) {
            return;
        }
        if (ec) {
            ec.clear();
            continue;
        }

        const fs::path absPath = it->path();
        if (selfPath.has_value()) {
            std::error_code eqec;
            if (fs::equivalent(absPath, *selfPath, eqec) && !eqec) {
                continue;
            }
        }

        const bool isDir = it->is_directory(ec);
        if (ec) {
            ec.clear();
            continue;
        }
        const bool isRegular = it->is_regular_file(ec);
        if (ec) {
            ec.clear();
            continue;
        }
        if (!isDir && !isRegular) {
            continue;
        }

        FileEntry entry;
        entry.relativePath = NormalizeRelativePath(fs::relative(absPath, root, ec));
        if (ec || entry.relativePath.empty()) {
            ec.clear();
            continue;
        }
        entry.isDirectory = isDir;
        if (isDir) {
            entry.fileSize = 0;
        } else {
            entry.fileSize = static_cast<uint64_t>(fs::file_size(absPath, ec));
            if (ec) {
                ec.clear();
                continue;
            }
            ++fileCount;
            if (fileCount % 2048 == 0) {
                std::vector<uint8_t> payload;
                AppendU64(payload, static_cast<uint64_t>(fileCount));
                enqueueManifestFrame(Frame{MsgType::ManifestProgress, 0, std::move(payload)});
            }
        }
        entry.mtimeNs = ToUnixNs(fs::last_write_time(absPath, ec));
        if (ec) {
            entry.mtimeNs = 0;
            ec.clear();
        }
        enqueueManifestFrame(Frame{MsgType::ManifestEntry, 0, EncodeManifestEntry(entry)});
    }

    std::vector<uint8_t> payload;
    AppendU64(payload, static_cast<uint64_t>(fileCount));
    enqueueManifestFrame(Frame{MsgType::ManifestProgress, 0, std::move(payload)});
    enqueueManifestFrame(Frame{MsgType::ManifestEnd, 0, {}});
#endif
}

std::vector<uint8_t> EncodeManifestEntry(const FileEntry& entry) {
    std::vector<uint8_t> payload;
    payload.push_back(entry.isDirectory ? 1 : 0);
    AppendString(payload, entry.relativePath);
    AppendU64(payload, entry.fileSize);
    AppendI64(payload, entry.mtimeNs);
    return payload;
}

FileEntry DecodeManifestEntry(const std::vector<uint8_t>& payload) {
    size_t cursor = 0;
    if (payload.empty()) {
        throw std::runtime_error("Manifest payload too short");
    }
    FileEntry entry;
    entry.isDirectory = payload[cursor++] != 0;
    entry.relativePath = ReadString(payload, cursor);
    entry.fileSize = ReadU64(payload, cursor);
    entry.mtimeNs = ReadI64(payload, cursor);
    return entry;
}

std::vector<uint8_t> EncodeHashRequest(const std::string& relPath) {
    std::vector<uint8_t> payload;
    AppendString(payload, relPath);
    return payload;
}

std::string DecodeHashRequest(const std::vector<uint8_t>& payload) {
    size_t cursor = 0;
    return ReadString(payload, cursor);
}

std::vector<uint8_t> EncodeHashResponse(const std::string& relPath, const Hash256& hash) {
    std::vector<uint8_t> payload;
    AppendString(payload, relPath);
    payload.insert(payload.end(), hash.begin(), hash.end());
    return payload;
}

std::pair<std::string, Hash256> DecodeHashResponse(const std::vector<uint8_t>& payload) {
    size_t cursor = 0;
    std::pair<std::string, Hash256> value;
    value.first = ReadString(payload, cursor);
    if (cursor + value.second.size() > payload.size()) {
        throw std::runtime_error("Hash response payload invalid");
    }
    std::copy(payload.begin() + static_cast<std::ptrdiff_t>(cursor), payload.begin() + static_cast<std::ptrdiff_t>(cursor + value.second.size()), value.second.begin());
    return value;
}

std::vector<uint8_t> EncodeFileOpen(const std::string& relPath) {
    std::vector<uint8_t> payload;
    AppendString(payload, relPath);
    return payload;
}

std::string DecodeFileOpen(const std::vector<uint8_t>& payload) {
    size_t cursor = 0;
    return ReadString(payload, cursor);
}

std::vector<uint8_t> EncodeFileBatchRequest(const std::vector<std::string>& relPaths) {
    if (relPaths.size() > UINT16_MAX) {
        throw std::runtime_error("Batch request too large");
    }
    std::vector<uint8_t> payload;
    AppendU16(payload, static_cast<uint16_t>(relPaths.size()));
    for (const std::string& rel : relPaths) {
        AppendString(payload, rel);
    }
    return payload;
}

std::vector<std::string> DecodeFileBatchRequest(const std::vector<uint8_t>& payload) {
    size_t cursor = 0;
    const uint16_t count = ReadU16(payload, cursor);
    std::vector<std::string> relPaths;
    relPaths.reserve(count);
    for (uint16_t i = 0; i < count; ++i) {
        relPaths.push_back(ReadString(payload, cursor));
    }
    return relPaths;
}

std::vector<uint8_t> EncodeFileBatchOpenResponse(const std::vector<BatchFileRecord>& files) {
    if (files.size() > UINT16_MAX) {
        throw std::runtime_error("Batch response too large");
    }
    std::vector<uint8_t> payload;
    AppendU16(payload, static_cast<uint16_t>(files.size()));
    for (const auto& file : files) {
        payload.push_back(file.ok ? 1 : 0);
        AppendString(payload, file.relativePath);
        AppendU64(payload, file.fileSize);
        AppendI64(payload, file.mtimeNs);
    }
    return payload;
}

std::vector<BatchFileRecord> DecodeFileBatchOpenResponse(const std::vector<uint8_t>& payload) {
    size_t cursor = 0;
    const uint16_t count = ReadU16(payload, cursor);
    std::vector<BatchFileRecord> files;
    files.reserve(count);
    for (uint16_t i = 0; i < count; ++i) {
        if (cursor >= payload.size()) {
            throw std::runtime_error("Batch open response invalid");
        }
        BatchFileRecord file;
        file.ok = payload[cursor++] != 0;
        file.relativePath = ReadString(payload, cursor);
        file.fileSize = ReadU64(payload, cursor);
        file.mtimeNs = ReadI64(payload, cursor);
        files.push_back(std::move(file));
    }
    return files;
}

LocalState BuildLocalState(const fs::path& root, const std::optional<fs::path>& exclude) {
    LocalState st;
#ifdef _WIN32
    struct PendingDir {
        std::wstring absDir;
        std::string relDir;
    };

    const std::wstring rootW = root.wstring();
    const std::wstring excludeW = exclude.has_value() ? exclude->wstring() : L"";
    std::vector<PendingDir> stack;
    stack.push_back(PendingDir{rootW, ""});

    while (!stack.empty()) {
        PendingDir current = std::move(stack.back());
        stack.pop_back();

        std::wstring pattern = current.absDir;
        if (!pattern.empty() && pattern.back() != L'\\' && pattern.back() != L'/') {
            pattern.push_back(L'\\');
        }
        pattern.append(L"*");

        WIN32_FIND_DATAW fd{};
        HANDLE hFind = FindFirstFileExW(
            pattern.c_str(),
            FindExInfoBasic,
            &fd,
            FindExSearchNameMatch,
            nullptr,
            FIND_FIRST_EX_LARGE_FETCH);
        if (hFind == INVALID_HANDLE_VALUE) {
            continue;
        }

        do {
            const wchar_t* name = fd.cFileName;
            if ((name[0] == L'.' && name[1] == L'\0') ||
                (name[0] == L'.' && name[1] == L'.' && name[2] == L'\0')) {
                continue;
            }

            std::wstring absPath = current.absDir;
            if (!absPath.empty() && absPath.back() != L'\\' && absPath.back() != L'/') {
                absPath.push_back(L'\\');
            }
            absPath.append(name);
            if (!excludeW.empty() && _wcsicmp(absPath.c_str(), excludeW.c_str()) == 0) {
                continue;
            }

            const bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            const std::string nameUtf8 = WideToUtf8(name);
            const std::string relPath = current.relDir.empty() ? nameUtf8 : (current.relDir + "/" + nameUtf8);
            if (isDir) {
                st.directories.insert(relPath);
                stack.push_back(PendingDir{absPath, relPath});
            } else {
                st.files.emplace(relPath, FileEntry{relPath, false, 0, 0});
            }
        } while (FindNextFileW(hFind, &fd) != 0);

        FindClose(hFind);
    }
#else
    std::error_code ec;
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
         it != end;
         it.increment(ec)) {
        if (ec) {
            ec.clear();
            continue;
        }
        const fs::path absPath = it->path();
        if (exclude.has_value()) {
            std::error_code eqec;
            if (fs::equivalent(absPath, *exclude, eqec) && !eqec) {
                continue;
            }
        }

        const bool isDir = it->is_directory(ec);
        if (ec) {
            ec.clear();
            continue;
        }
        const bool isRegular = it->is_regular_file(ec);
        if (ec) {
            ec.clear();
            continue;
        }
        if (!isDir && !isRegular) {
            continue;
        }
        const std::string relPath = NormalizeRelativePath(fs::relative(absPath, root, ec));
        if (ec || relPath.empty()) {
            ec.clear();
            continue;
        }
        if (isDir) {
            st.directories.insert(relPath);
        } else {
            st.files.emplace(relPath, FileEntry{relPath, false, 0, 0});
        }
    }
#endif
    return st;
}

void EnsureParentDir(const fs::path& filePath) {
    const fs::path parent = filePath.parent_path();
    if (!parent.empty()) {
        std::error_code ec;
        fs::create_directories(parent, ec);
    }
}

struct RemoveLocalExtrasResult {
    size_t deletedFiles = 0;
    size_t failedOps = 0;
};

RemoveLocalExtrasResult RemoveLocalExtras(const fs::path& root,
                                          const std::unordered_set<std::string>& remoteDirs,
                                          const std::unordered_map<std::string, FileEntry>& remoteFiles,
                                          const std::optional<fs::path>& exclude) {
    RemoveLocalExtrasResult result;
    LocalState local = BuildLocalState(root, exclude);
    for (const auto& kv : local.files) {
        if (!remoteFiles.contains(kv.first)) {
            std::error_code ec;
            const bool removed = fs::remove(JoinRel(root, kv.first), ec);
            if (removed) {
                ++result.deletedFiles;
            } else if (ec) {
                ++result.failedOps;
            }
        }
    }
    std::vector<std::string> dirs(local.directories.begin(), local.directories.end());
    std::sort(dirs.begin(), dirs.end(), [](const std::string& a, const std::string& b) {
        return a.size() > b.size();
    });
    for (const std::string& dir : dirs) {
        if (!remoteDirs.contains(dir)) {
            std::error_code ec;
            fs::remove(JoinRel(root, dir), ec);
            if (ec) {
                ++result.failedOps;
            }
        }
    }
    return result;
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

uint32_t ResolveServerHashWorkerCount(const CliOptions& options) {
    if (options.serverHashWorkers != 0) {
        return options.serverHashWorkers;
    }
    const uint32_t hw = std::max<uint32_t>(1, std::thread::hardware_concurrency());
    uint32_t workers = hw / 2;
    if (workers == 0) {
        workers = 1;
    }
    if (workers < 2 && hw > 1) {
        workers = 2;
    }
    workers = std::min<uint32_t>(workers, 16);
    return workers;
}

class ServerHashThreadPool {
public:
    ~ServerHashThreadPool() {
        Stop();
    }

    void Configure(uint32_t workerCount) {
        if (workerCount == 0) {
            throw std::runtime_error("Server hash worker count must be > 0");
        }
        std::lock_guard<std::mutex> lock(mu_);
        if (!workers_.empty()) {
            if (configuredWorkers_ != workerCount) {
                throw std::runtime_error("Server hash pool already configured with different worker count");
            }
            return;
        }
        configuredWorkers_ = workerCount;
        stop_ = false;
        for (uint32_t i = 0; i < workerCount; ++i) {
            workers_.emplace_back([this]() {
                while (true) {
                    std::function<void()> task;
                    {
                        std::unique_lock<std::mutex> lock(mu_);
                        cv_.wait(lock, [&]() { return stop_ || !tasks_.empty(); });
                        if (stop_ && tasks_.empty()) {
                            return;
                        }
                        task = std::move(tasks_.front());
                        tasks_.pop_front();
                    }
                    task();
                }
            });
        }
    }

    void Enqueue(std::function<void()> task) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (workers_.empty()) {
                throw std::runtime_error("Server hash pool is not configured");
            }
            tasks_.push_back(std::move(task));
        }
        cv_.notify_one();
    }

    size_t PendingTasks() const {
        std::lock_guard<std::mutex> lock(mu_);
        return tasks_.size();
    }

private:
    void Stop() {
        {
            std::lock_guard<std::mutex> lock(mu_);
            stop_ = true;
        }
        cv_.notify_all();
        for (auto& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        workers_.clear();
    }

    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> tasks_;
    std::vector<std::thread> workers_;
    uint32_t configuredWorkers_ = 0;
    bool stop_ = false;
};

ServerHashThreadPool& GetServerHashPool() {
    static ServerHashThreadPool pool;
    return pool;
}

bool TryReadHashFingerprint(const fs::path& absPath, HashFingerprint& out) {
    std::error_code ec;
    if (!fs::exists(absPath, ec) || ec) {
        return false;
    }
    if (!fs::is_regular_file(absPath, ec) || ec) {
        return false;
    }
    const uint64_t size = static_cast<uint64_t>(fs::file_size(absPath, ec));
    if (ec) {
        return false;
    }
    const int64_t mtimeNs = ToUnixNs(fs::last_write_time(absPath, ec));
    if (ec) {
        return false;
    }
    out.fileSize = size;
    out.mtimeNs = mtimeNs;
    return true;
}

ServerHashMemCache& GetServerHashMemCache() {
    static ServerHashMemCache cache;
    return cache;
}

void RunSessionServer(const SocketHandle& client, const CliOptions& options) {
    EnsureHandshakeAsServer(client, options.password);
    const std::optional<fs::path> selfPath = CurrentExePath();
    const bool debugEnabled = IsDebugEnabled();
    const bool hashMemcacheEnabled = GetServerHashMemCache().Enabled();
    const TunedTransferOptions tuned = ResolveTransferOptions(options);
    const uint32_t streamLimit = tuned.streamLimit;
    const uint32_t effectiveChunkSize = EffectiveChunkSizeForStreams(tuned.chunkSize, streamLimit);

    static std::atomic<uint64_t> g_sessionIdCounter{0};
    const uint64_t sessionId = g_sessionIdCounter.fetch_add(1, std::memory_order_relaxed) + 1;

    // Lock ordering (must never nest in the other direction):
    //   mu  >  sessionHashMu
    // i.e. it is forbidden to acquire sessionHashMu while holding mu, or vice versa.
    // `mu` protects only the outbound queues and the hand-off queues below; it must
    // NOT cover file I/O. The active*Streams containers are owned exclusively by the
    // main send loop thread (adopted from pendingNew*Streams) and are accessed without
    // mu, so all open()/read() happens outside any lock.
    std::unordered_map<uint32_t, ServerStream> activeStreams;        // main-loop private
    std::unordered_map<uint32_t, ServerBatchStream> activeBatchStreams;  // main-loop private
    std::mutex mu;
    std::condition_variable outboundCv;
    std::queue<Frame> outboundHigh;
    std::queue<Frame> outboundManifest;
    // Hand-off queues: receiver pushes newly opened streams here (cheap move under mu);
    // the main loop adopts them into its private containers, then does I/O lock-free.
    std::vector<std::pair<uint32_t, ServerStream>> pendingNewStreams;
    std::vector<std::pair<uint32_t, ServerBatchStream>> pendingNewBatchStreams;
    const size_t maxQueuedManifestFrames = 512;
    std::atomic<bool> done = false;
    std::atomic<bool> failed = false;
    std::string errorText;
    std::thread manifestThread;
    std::atomic<bool> manifestStarted = false;
    std::mutex sessionHashMu;  // lock order: acquire only when NOT holding mu (mu > sessionHashMu)
    std::condition_variable sessionHashCv;
    size_t sessionPendingHashJobs = 0;

    auto enqueueHigh = [&](Frame frame) {
        std::lock_guard<std::mutex> lock(mu);
        outboundHigh.push(std::move(frame));
        outboundCv.notify_one();
    };
    auto enqueueManifest = [&](Frame frame) {
        std::unique_lock<std::mutex> lock(mu);
        outboundCv.wait(lock, [&]() {
            return done.load() || outboundManifest.size() < maxQueuedManifestFrames;
        });
        if (done.load()) {
            return;
        }
        outboundManifest.push(std::move(frame));
        outboundCv.notify_one();
    };

    std::thread receiver([&]() {
        try {
            while (!done.load()) {
                Frame frame = RecvFrame(client);
                if (frame.type == MsgType::ManifestRequest) {
                    if (manifestStarted.exchange(true)) {
                        continue;
                    }
                    manifestThread = std::thread([&]() {
                        try {
                            EnumerateManifestEntriesFast(options.rootDir, selfPath, done, enqueueManifest);
                        } catch (const std::exception& ex) {
                            failed.store(true);
                            done.store(true);
                            sessionHashCv.notify_all();
                            errorText = ex.what();
                        }
                    });
                } else if (frame.type == MsgType::HashRequest) {
                    const std::string rel = DecodeHashRequest(frame.payload);
                    const fs::path abs = JoinRel(options.rootDir, rel);
                    HashFingerprint fingerprint;
                    const bool fingerprintValid = TryReadHashFingerprint(abs, fingerprint);
                    if (hashMemcacheEnabled && fingerprintValid) {
                        Hash256 cachedHash{};
                        if (GetServerHashMemCache().TryGet(rel, fingerprint, cachedHash)) {
                            enqueueHigh(Frame{MsgType::HashResponse, 0, EncodeHashResponse(rel, cachedHash)});
                            continue;
                        }
                    }
                    {
                        std::lock_guard<std::mutex> lock(sessionHashMu);
                        ++sessionPendingHashJobs;
                    }
                    try {
                        GetServerHashPool().Enqueue([&, rel, abs, fingerprint, fingerprintValid]() {
                            Hash256 hash{};
                            bool hashOk = true;
                            try {
                                hash = ComputeFileHash(abs);
                            } catch (...) {
                                hashOk = false;
                                hash.fill(0xFF);
                            }
                            if (hashMemcacheEnabled && hashOk) {
                                HashFingerprint afterFingerprint;
                                if (TryReadHashFingerprint(abs, afterFingerprint) &&
                                    (!fingerprintValid ||
                                     (afterFingerprint.fileSize == fingerprint.fileSize &&
                                      afterFingerprint.mtimeNs == fingerprint.mtimeNs))) {
                                    GetServerHashMemCache().Upsert(rel, afterFingerprint, hash);
                                }
                            }
                            if (!done.load()) {
                                try {
                                    enqueueHigh(Frame{MsgType::HashResponse, 0, EncodeHashResponse(rel, hash)});
                                } catch (...) {
                                    failed.store(true);
                                    done.store(true);
                                    outboundCv.notify_all();
                                }
                            }
                            {
                                std::lock_guard<std::mutex> lock(sessionHashMu);
                                if (sessionPendingHashJobs > 0) {
                                    --sessionPendingHashJobs;
                                }
                            }
                            sessionHashCv.notify_all();
                        });
                    } catch (...) {
                        {
                            std::lock_guard<std::mutex> lock(sessionHashMu);
                            if (sessionPendingHashJobs > 0) {
                                --sessionPendingHashJobs;
                            }
                        }
                        sessionHashCv.notify_all();
                        throw;
                    }
                } else if (frame.type == MsgType::FileOpen) {
                    const std::string rel = DecodeFileOpen(frame.payload);
                    const fs::path abs = JoinRel(options.rootDir, rel);
                    ServerStream st;
                    st.relativePath = rel;
                    st.input.open(abs, std::ios::binary);
                    if (!st.input) {
                        enqueueHigh(Frame{MsgType::FileError, frame.streamId, std::vector<uint8_t>(rel.begin(), rel.end())});
                        continue;
                    }
                    {
                        std::lock_guard<std::mutex> lock(mu);
                        pendingNewStreams.emplace_back(frame.streamId, std::move(st));
                    }
                    outboundCv.notify_one();
                } else if (frame.type == MsgType::FileBatchOpen) {
                    std::vector<std::string> relPaths = DecodeFileBatchRequest(frame.payload);
                    ServerBatchStream batch;
                    batch.files.reserve(relPaths.size());
                    for (const std::string& rel : relPaths) {
                        BatchFileRecord record;
                        record.relativePath = rel;
                        record.absPath = JoinRel(options.rootDir, rel);
                        std::error_code ec;
                        if (fs::exists(record.absPath, ec) && !ec &&
                            fs::is_regular_file(record.absPath, ec) && !ec) {
                            record.fileSize = static_cast<uint64_t>(fs::file_size(record.absPath, ec));
                            if (!ec) {
                                // Same canonical mtime unit as the manifest path.
                                record.mtimeNs = ReadFileMtimeCanonical(record.absPath);
                                record.ok = true;
                            }
                        }
                        batch.files.push_back(std::move(record));
                    }
                    {
                        std::lock_guard<std::mutex> lock(mu);
                        pendingNewBatchStreams.emplace_back(frame.streamId, std::move(batch));
                    }
                    outboundCv.notify_one();
                } else if (frame.type == MsgType::SyncDone) {
                    done.store(true);
                    sessionHashCv.notify_all();
                    outboundCv.notify_all();
                } else {
                    throw std::runtime_error("Unknown message in server session");
                }
            }
        } catch (const std::exception& ex) {
            failed.store(true);
            done.store(true);
            sessionHashCv.notify_all();
            outboundCv.notify_all();
            errorText = ex.what();
        }
    });

    // Diagnostics-only lock metrics (samples since last debug print), gated by debugEnabled.
    // Declared at function scope so the exception-window snapshot below (AC-B3) can summarise
    // the most recent lock-wait / critical-section samples after the main loop unwinds.
    std::vector<int64_t> muWaitUs;
    std::vector<int64_t> muHoldUs;
    try {
        auto lastDebugPrint = std::chrono::steady_clock::now();
        const size_t perStreamBurstBytes = (streamLimit <= 8)
                                               ? std::max<size_t>(2 * 1024 * 1024, static_cast<size_t>(effectiveChunkSize) * 2)
                                               : static_cast<size_t>(effectiveChunkSize);
        while (!done.load()) {
            bool didWork = false;
            std::vector<Frame> sendFrames;
            sendFrames.reserve(std::max<size_t>(256, static_cast<size_t>(streamLimit) * 8));
            // Critical section: drain outbound queues and adopt newly opened streams only.
            // No file I/O happens under `mu` (see lock-ordering note at declarations).
            {
                const auto muWaitStart = debugEnabled ? std::chrono::steady_clock::now()
                                                      : std::chrono::steady_clock::time_point{};
                std::unique_lock<std::mutex> lock(mu);
                const auto muAcquired = debugEnabled ? std::chrono::steady_clock::now()
                                                     : std::chrono::steady_clock::time_point{};
                size_t highBudget = 256;
                while (!outboundHigh.empty() && highBudget > 0) {
                    sendFrames.push_back(std::move(outboundHigh.front()));
                    outboundHigh.pop();
                    didWork = true;
                    --highBudget;
                }
                size_t manifestBudget = 0;
                if (!outboundManifest.empty()) {
                    // Keep manifest draining even under hash/file pressure so ManifestEnd is not starved.
                    manifestBudget = outboundHigh.empty() ? 16 : 4;
                    if (outboundManifest.size() > (maxQueuedManifestFrames / 2)) {
                        manifestBudget = outboundHigh.empty() ? 32 : 8;
                    }
                }
                while (!outboundManifest.empty() && manifestBudget > 0) {
                    sendFrames.push_back(std::move(outboundManifest.front()));
                    outboundManifest.pop();
                    outboundCv.notify_one();
                    didWork = true;
                    --manifestBudget;
                }
                // Adopt any newly opened streams into main-loop-private containers.
                for (auto& kv : pendingNewStreams) {
                    activeStreams.emplace(kv.first, std::move(kv.second));
                    didWork = true;
                }
                pendingNewStreams.clear();
                for (auto& kv : pendingNewBatchStreams) {
                    activeBatchStreams.emplace(kv.first, std::move(kv.second));
                    didWork = true;
                }
                pendingNewBatchStreams.clear();
                if (debugEnabled) {
                    muWaitUs.push_back(std::chrono::duration_cast<std::chrono::microseconds>(muAcquired - muWaitStart).count());
                    const auto muReleased = std::chrono::steady_clock::now();
                    muHoldUs.push_back(std::chrono::duration_cast<std::chrono::microseconds>(muReleased - muAcquired).count());
                }
            }
            // ---- Lock-free section: all file open()/read() runs without holding mu ----
            {
                const bool hasRegularStreams = !activeStreams.empty();
                const size_t batchSendQuotaBytes = (streamLimit <= 8) ? (24 * 1024 * 1024) : (12 * 1024 * 1024);
                size_t batchBytesSentThisRound = 0;
                for (auto it = activeBatchStreams.begin(); it != activeBatchStreams.end();) {
                    if (hasRegularStreams && batchBytesSentThisRound >= batchSendQuotaBytes) {
                        break;
                    }
                    ServerBatchStream& batch = it->second;
                    if (!batch.headerSent) {
                        sendFrames.push_back(Frame{MsgType::FileBatchOpen, it->first, EncodeFileBatchOpenResponse(batch.files)});
                        batch.headerSent = true;
                        didWork = true;
                    }

                    size_t burstBytes = 0;
                    while (burstBytes < perStreamBurstBytes) {
                        while (batch.index < batch.files.size() && !batch.files[batch.index].ok) {
                            ++batch.index;
                        }
                        if (batch.index >= batch.files.size()) {
                            break;
                        }
                        BatchFileRecord& file = batch.files[batch.index];
                        if (!batch.input.is_open()) {
                            batch.input.open(file.absPath, std::ios::binary);
                            if (!batch.input) {
                                throw std::runtime_error("Cannot open batch file for read: " + file.relativePath);
                            }
                            batch.remainingBytes = file.fileSize;
                            if (batch.remainingBytes == 0) {
                                batch.input.close();
                                ++batch.index;
                                continue;
                            }
                        }
                        const uint64_t toRead = std::min<uint64_t>(batch.remainingBytes, static_cast<uint64_t>(effectiveChunkSize));
                        std::vector<uint8_t> chunk(static_cast<size_t>(toRead));
                        batch.input.read(reinterpret_cast<char*>(chunk.data()), static_cast<std::streamsize>(chunk.size()));
                        const std::streamsize got = batch.input.gcount();
                        if (got <= 0) {
                            throw std::runtime_error("Failed while reading batch file: " + file.relativePath);
                        }
                        chunk.resize(static_cast<size_t>(got));
                        burstBytes += static_cast<size_t>(got);
                        batchBytesSentThisRound += static_cast<size_t>(got);
                        batch.remainingBytes -= static_cast<uint64_t>(got);
                        sendFrames.push_back(Frame{MsgType::FileBatchChunk, it->first, std::move(chunk)});
                        didWork = true;
                        if (batch.remainingBytes == 0) {
                            batch.input.close();
                            ++batch.index;
                        }
                    }

                    bool batchDone = true;
                    for (size_t idx = batch.index; idx < batch.files.size(); ++idx) {
                        if (batch.files[idx].ok) {
                            batchDone = false;
                            break;
                        }
                    }
                    if (batchDone && !batch.input.is_open()) {
                        sendFrames.push_back(Frame{MsgType::FileBatchEnd, it->first, {}});
                        it = activeBatchStreams.erase(it);
                        didWork = true;
                    } else {
                        ++it;
                    }
                }

                for (auto it = activeStreams.begin(); it != activeStreams.end();) {
                    size_t burstBytes = 0;
                    bool streamClosed = false;
                    while (!streamClosed && burstBytes < perStreamBurstBytes) {
                        std::vector<uint8_t> chunk(effectiveChunkSize);
                        it->second.input.read(reinterpret_cast<char*>(chunk.data()), static_cast<std::streamsize>(chunk.size()));
                        const std::streamsize got = it->second.input.gcount();
                        if (got > 0) {
                            chunk.resize(static_cast<size_t>(got));
                            burstBytes += static_cast<size_t>(got);
                            sendFrames.push_back(Frame{MsgType::FileChunk, it->first, std::move(chunk)});
                            didWork = true;
                        }
                        if (!it->second.input || got == 0) {
                            sendFrames.push_back(Frame{MsgType::FileEnd, it->first, {}});
                            it = activeStreams.erase(it);
                            streamClosed = true;
                            didWork = true;
                        }
                    }
                    if (!streamClosed) {
                        ++it;
                    }
                }
            }
            if (!sendFrames.empty()) {
                SendFrameBatch(client, sendFrames);
            }
            if (debugEnabled) {
                const auto now = std::chrono::steady_clock::now();
                if ((now - lastDebugPrint) >= std::chrono::seconds(1)) {
                    size_t highQueued = 0;
                    size_t manifestQueued = 0;
                    size_t pendingAdopt = 0;
                    {
                        std::lock_guard<std::mutex> lock(mu);
                        highQueued = outboundHigh.size();
                        manifestQueued = outboundManifest.size();
                        pendingAdopt = pendingNewStreams.size() + pendingNewBatchStreams.size();
                    }
                    // active*Streams are main-loop private; safe to read without mu.
                    const size_t activeStreamCount = activeStreams.size();
                    const size_t activeBatchCount = activeBatchStreams.size();
                    size_t pendingHashes = 0;
                    {
                        std::lock_guard<std::mutex> lock(sessionHashMu);
                        pendingHashes = sessionPendingHashJobs;
                    }
                    const size_t globalPendingHashes = GetServerHashPool().PendingTasks();
                    const size_t hashMemcacheEntries = GetServerHashMemCache().EntryCount();
                    const uint64_t hashMemcacheHits = GetServerHashMemCache().HitCount();
                    const uint64_t hashMemcacheMisses = GetServerHashMemCache().MissCount();
                    auto pct = [](std::vector<int64_t>& v, double p) -> int64_t {
                        if (v.empty()) {
                            return 0;
                        }
                        std::sort(v.begin(), v.end());
                        size_t idx = static_cast<size_t>(p * static_cast<double>(v.size() - 1) + 0.5);
                        if (idx >= v.size()) {
                            idx = v.size() - 1;
                        }
                        return v[idx];
                    };
                    const int64_t muWaitP50 = pct(muWaitUs, 0.50);
                    const int64_t muWaitP95 = pct(muWaitUs, 0.95);
                    const int64_t muWaitP99 = pct(muWaitUs, 0.99);
                    const int64_t critHoldP50 = pct(muHoldUs, 0.50);
                    const int64_t critHoldP95 = pct(muHoldUs, 0.95);
                    const int64_t critHoldP99 = pct(muHoldUs, 0.99);
                    std::cerr << "[debug][server][sid=" << sessionId << "] queued_high=" << highQueued
                              << " queued_manifest=" << manifestQueued
                              << " pending_adopt=" << pendingAdopt
                              << " pending_hash_jobs=" << pendingHashes
                              << " global_hash_queue=" << globalPendingHashes
                              << " hash_memcache=" << (hashMemcacheEnabled ? 1 : 0)
                              << " hash_memcache_entries=" << hashMemcacheEntries
                              << " hash_memcache_hits=" << hashMemcacheHits
                              << " hash_memcache_misses=" << hashMemcacheMisses
                              << " active_streams=" << activeStreamCount
                              << " active_batches=" << activeBatchCount
                              << " mu_wait_us_p50=" << muWaitP50
                              << " mu_wait_us_p95=" << muWaitP95
                              << " mu_wait_us_p99=" << muWaitP99
                              << " crit_hold_us_p50=" << critHoldP50
                              << " crit_hold_us_p95=" << critHoldP95
                              << " crit_hold_us_p99=" << critHoldP99
                              << std::endl;
                    muWaitUs.clear();
                    muHoldUs.clear();
                    lastDebugPrint = now;
                }
            }
            if (!didWork) {
                std::unique_lock<std::mutex> lock(mu);
                outboundCv.wait_for(lock, std::chrono::milliseconds(2), [&]() {
                    return done.load() || !outboundHigh.empty() || !outboundManifest.empty() ||
                           !pendingNewStreams.empty() || !pendingNewBatchStreams.empty();
                });
            }
        }
    } catch (const std::exception& ex) {
        failed.store(true);
        done.store(true);
        sessionHashCv.notify_all();
        outboundCv.notify_all();
        errorText = ex.what();
    }

    if (debugEnabled && failed.load()) {
        // Exception-window context snapshot for post-mortem (AC-B3 / design §B.4):
        // session id + lock-wait/critical-section stats + outbound & hash queues + in-flight tasks.
        size_t highQueued = 0, manifestQueued = 0, pendingAdopt = 0, pendingHashes = 0;
        {
            std::lock_guard<std::mutex> lock(mu);
            highQueued = outboundHigh.size();
            manifestQueued = outboundManifest.size();
            pendingAdopt = pendingNewStreams.size() + pendingNewBatchStreams.size();
        }
        {
            std::lock_guard<std::mutex> lock(sessionHashMu);
            pendingHashes = sessionPendingHashJobs;
        }
        const size_t globalPendingHashes = GetServerHashPool().PendingTasks();
        const size_t activeStreamCount = activeStreams.size();
        const size_t activeBatchCount = activeBatchStreams.size();
        // In-flight tasks: active transfer streams + batch streams + pending hash jobs.
        const size_t inFlight = activeStreamCount + activeBatchCount + pendingHashes;
        auto pct = [](std::vector<int64_t>& v, double p) -> int64_t {
            if (v.empty()) {
                return 0;
            }
            std::sort(v.begin(), v.end());
            size_t idx = static_cast<size_t>(p * static_cast<double>(v.size() - 1) + 0.5);
            if (idx >= v.size()) {
                idx = v.size() - 1;
            }
            return v[idx];
        };
        const int64_t muWaitP50 = pct(muWaitUs, 0.50);
        const int64_t muWaitP95 = pct(muWaitUs, 0.95);
        const int64_t muWaitP99 = pct(muWaitUs, 0.99);
        const int64_t critHoldP50 = pct(muHoldUs, 0.50);
        const int64_t critHoldP95 = pct(muHoldUs, 0.95);
        const int64_t critHoldP99 = pct(muHoldUs, 0.99);
        std::cerr << "[debug][server][sid=" << sessionId << "] session_failed=1"
                  << " error=\"" << errorText << "\""
                  << " queued_high=" << highQueued
                  << " queued_manifest=" << manifestQueued
                  << " pending_adopt=" << pendingAdopt
                  << " pending_hash_jobs=" << pendingHashes
                  << " global_hash_queue=" << globalPendingHashes
                  << " active_streams=" << activeStreamCount
                  << " active_batches=" << activeBatchCount
                  << " in_flight=" << inFlight
                  << " mu_wait_us_p50=" << muWaitP50
                  << " mu_wait_us_p95=" << muWaitP95
                  << " mu_wait_us_p99=" << muWaitP99
                  << " crit_hold_us_p50=" << critHoldP50
                  << " crit_hold_us_p95=" << critHoldP95
                  << " crit_hold_us_p99=" << critHoldP99
                  << std::endl;
    }

    // FR-05 / AC-B2: bound receiver teardown. If the main loop failed while the
    // receiver thread is blocked in RecvFrame, joining it could wait unbounded for
    // the peer. Shutting down the socket forces the blocking recv to return so the
    // receiver loop observes `done` and exits promptly (bounded convergence).
    if (failed.load()) {
        ShutdownBoth(client);
    }
    if (receiver.joinable()) {
        receiver.join();
    }
    if (manifestThread.joinable()) {
        manifestThread.join();
    }
    {
        // Bounded wait so a leaked/stuck hash job can never produce an unbounded hang
        // (FR-05 / AC-B2). On timeout we converge as a controlled failure.
        constexpr auto kHashDrainTimeout = std::chrono::minutes(10);
        std::unique_lock<std::mutex> lock(sessionHashMu);
        const bool drained = sessionHashCv.wait_for(lock, kHashDrainTimeout,
                                                    [&]() { return sessionPendingHashJobs == 0; });
        if (!drained) {
            failed.store(true);
            if (errorText.empty()) {
                errorText = "hash drain wait timed out after 10min (bounded-wait guard)";
            }
            if (debugEnabled) {
                std::cerr << "[debug][server][sid=" << sessionId << "] hash_drain_timeout=1"
                          << " pending_hash_jobs=" << sessionPendingHashJobs << std::endl;
            }
        }
    }
    if (failed.load()) {
        throw std::runtime_error("Server session failed: " + errorText);
    }
}

struct DownloadState {
    std::ofstream output;
    std::string relPath;
    std::vector<uint8_t> writeBuffer;
    size_t flushThreshold = 0;
};

struct BatchDownloadEntry {
    std::string relPath;
    uint64_t fileSize = 0;
    int64_t mtimeNs = 0;
    bool serverOk = false;
    bool shouldWrite = false;
    bool finalized = false;
    uint64_t received = 0;
    std::ofstream output;
};

struct BatchDownloadState {
    bool headerReady = false;
    std::vector<BatchDownloadEntry> entries;
    size_t currentIndex = 0;
};

uint32_t EffectiveChunkSizeForStreams(uint32_t configuredChunkSize, uint32_t streamLimit) {
    if (streamLimit <= 16) {
        return std::max<uint32_t>(configuredChunkSize, 1024 * 1024);
    }
    if (streamLimit <= 32) {
        return std::max<uint32_t>(configuredChunkSize, 512 * 1024);
    }
    return configuredChunkSize;
}

size_t DownloadFlushThresholdForStreams(uint32_t streamLimit, uint32_t effectiveChunkSize) {
    if (streamLimit <= 16) {
        return std::max<size_t>(4 * 1024 * 1024, static_cast<size_t>(effectiveChunkSize) * 4);
    }
    if (streamLimit <= 32) {
        return std::max<size_t>(2 * 1024 * 1024, static_cast<size_t>(effectiveChunkSize) * 2);
    }
    return std::max<size_t>(512 * 1024, static_cast<size_t>(effectiveChunkSize));
}

void PrintClientCounters(size_t enumerated,
                         size_t compared,
                         size_t unchanged,
                         size_t failed,
                         size_t transferred,
                         size_t deleted,
                         size_t& lastEnumerated,
                         size_t& lastCompared,
                         size_t& lastUnchanged,
                         size_t& lastFailed,
                         size_t& lastTransferred,
                         size_t& lastDeleted,
                         bool force = false) {
    using clock = std::chrono::steady_clock;
    static clock::time_point lastPrint = clock::now();
    const auto now = clock::now();
    const bool tickReached = (now - lastPrint) >= std::chrono::seconds(1);
    const bool changedEnough = (enumerated != lastEnumerated) ||
                               (compared != lastCompared) ||
                               (unchanged != lastUnchanged) ||
                               (failed != lastFailed) ||
                               (transferred != lastTransferred) ||
                               (deleted != lastDeleted);
    if (!force && (!tickReached || !changedEnough)) {
        return;
    }
    lastPrint = now;
    lastEnumerated = enumerated;
    lastCompared = compared;
    lastUnchanged = unchanged;
    lastFailed = failed;
    lastTransferred = transferred;
    lastDeleted = deleted;
    // CONSTRAINT-C1: each progress update occupies its own line (no '\r' overwrite),
    // so output stays consistent across Windows/Linux/macOS terminals and log files.
    std::cout << "Enumrated: " << enumerated
              << "  Compared: " << compared
              << "  Unchanged: " << unchanged
              << "  Failed: " << failed
              << "  Transfered: " << transferred
              << "  Deleted: " << deleted
              << std::endl;
}

void TransferFileBatch(const SocketHandle& socket,
                       const CliOptions& options,
                       const fs::path& rootDir,
                       const std::unordered_map<std::string, FileEntry>& remoteFiles,
                       const std::vector<std::string>& filesToTransfer,
                       size_t& transferredTotal) {
    if (filesToTransfer.empty()) {
        return;
    }
    std::unordered_map<uint32_t, DownloadState> activeDownloads;
    std::unordered_map<uint32_t, std::string> streamToPath;
    std::queue<std::pair<uint32_t, std::string>> pending;
    uint32_t streamId = 1;
    for (const std::string& rel : filesToTransfer) {
        pending.push({streamId++, rel});
    }

    const TunedTransferOptions tuned = ResolveTransferOptions(options);
    const uint32_t streamLimit = tuned.streamLimit;
    const uint32_t effectiveChunkSize = EffectiveChunkSizeForStreams(tuned.chunkSize, streamLimit);
    const size_t downloadFlushThreshold = DownloadFlushThresholdForStreams(streamLimit, effectiveChunkSize);

    auto flushBufferedWrites = [&](DownloadState& d) {
        if (d.writeBuffer.empty()) {
            return;
        }
        d.output.write(reinterpret_cast<const char*>(d.writeBuffer.data()), static_cast<std::streamsize>(d.writeBuffer.size()));
        d.writeBuffer.clear();
    };

    auto openPendingStream = [&](uint32_t sid, const std::string& rel) {
        const fs::path abs = JoinRel(rootDir, rel);
        EnsureParentDir(abs);
        DownloadState d;
        d.relPath = rel;
        d.output.open(abs, std::ios::binary | std::ios::trunc);
        if (!d.output) {
            throw std::runtime_error("Cannot open local file for write: " + rel);
        }
        d.flushThreshold = downloadFlushThreshold;
        d.writeBuffer.reserve(d.flushThreshold);
        activeDownloads.emplace(sid, std::move(d));
        streamToPath.emplace(sid, rel);
        SendFrame(socket, Frame{MsgType::FileOpen, sid, EncodeFileOpen(rel)});
    };

    while (!pending.empty() && activeDownloads.size() < streamLimit) {
        auto [sid, rel] = pending.front();
        pending.pop();
        openPendingStream(sid, rel);
    }

    size_t completed = 0;
    while (completed < filesToTransfer.size()) {
        Frame frame = RecvFrame(socket);
        if (frame.type == MsgType::FileChunk) {
            auto it = activeDownloads.find(frame.streamId);
            if (it == activeDownloads.end()) {
                throw std::runtime_error("Received chunk for unknown stream");
            }
            DownloadState& d = it->second;
            d.writeBuffer.insert(d.writeBuffer.end(), frame.payload.begin(), frame.payload.end());
            if (d.writeBuffer.size() >= d.flushThreshold) {
                flushBufferedWrites(d);
            }
        } else if (frame.type == MsgType::FileEnd) {
            auto it = activeDownloads.find(frame.streamId);
            if (it == activeDownloads.end()) {
                throw std::runtime_error("Received end for unknown stream");
            }
            flushBufferedWrites(it->second);
            it->second.output.flush();
            it->second.output.close();
            const std::string rel = it->second.relPath;
            const FileEntry& meta = remoteFiles.at(rel);
            SetFileModifyTime(JoinRel(rootDir, rel), meta.mtimeNs);
            activeDownloads.erase(it);
            streamToPath.erase(frame.streamId);
            ++completed;
            ++transferredTotal;
            if (transferredTotal % 200 == 0) {
                std::cout << "[progress] transfer total_completed=" << transferredTotal << std::endl;
            }
            if (!pending.empty()) {
                auto [sid, nextRel] = pending.front();
                pending.pop();
                openPendingStream(sid, nextRel);
            }
        } else if (frame.type == MsgType::FileError) {
            auto it = streamToPath.find(frame.streamId);
            const std::string rel = it == streamToPath.end() ? "<unknown>" : it->second;
            throw std::runtime_error("Server cannot open file: " + rel);
        } else {
            throw std::runtime_error("Unexpected frame during file transfer");
        }
    }
}

}  // namespace

int RunServer(const CliOptions& options) {
    WsaContext wsa;
    const TunedTransferOptions tuned = ResolveTransferOptions(options);
    const uint32_t hashWorkerCount = ResolveServerHashWorkerCount(options);
    GetServerHashPool().Configure(hashWorkerCount);
    GetServerHashMemCache().Configure(options.enableHashMemcache);
    std::cout << "FastClone server root=" << options.rootDir.string() << " port=" << options.port << std::endl;
    std::cout << "[hash-pool] workers=" << hashWorkerCount
              << (options.serverHashWorkers == 0 ? " (auto)" : " (manual)")
              << std::endl;
    std::cout << "[hash-memcache] enabled=" << (options.enableHashMemcache ? 1 : 0) << std::endl;
    if (options.streamAutoTune || options.chunkAutoTune) {
        std::cout << "[auto-tune] streams=" << tuned.streamLimit
                  << " chunk-kb=" << (tuned.chunkSize / 1024)
                  << std::endl;
    }
    SocketHandle listener = CreateServer(options.port);
    std::atomic<uint64_t> sessionIdCounter{0};
    std::atomic<uint32_t> activeSessions{0};
    while (true) {
        std::cout << "Waiting for client... active_sessions=" << activeSessions.load() << std::endl;
        SocketHandle client = AcceptClient(listener);
        const uint64_t sessionId = sessionIdCounter.fetch_add(1) + 1;
        activeSessions.fetch_add(1);
        std::thread([sessionId, &activeSessions, options, client = std::move(client)]() mutable {
            try {
                std::cout << "Session#" << sessionId << " started" << std::endl;
                RunSessionServer(client, options);
                std::cout << "Session#" << sessionId << " completed" << std::endl;
            } catch (const std::exception& ex) {
                std::cerr << "Session#" << sessionId << " error: " << ex.what() << std::endl;
            }
            activeSessions.fetch_sub(1);
        }).detach();
    }
    return 0;
}

int RunClient(const CliOptions& options) {
    WsaContext wsa;
    const bool debugEnabled = IsDebugEnabled();
    // --diag flag OR FASTCLONE_DIAG env var enables diagnostics (design §A.3).
    const bool diagnostics = options.diagnostics || IsDiagEnabled();
    const TunedTransferOptions tuned = ResolveTransferOptions(options);
    const uint32_t streamLimit = tuned.streamLimit;
    const uint32_t effectiveChunkSize = EffectiveChunkSizeForStreams(tuned.chunkSize, streamLimit);
    const size_t downloadFlushThreshold = DownloadFlushThresholdForStreams(streamLimit, effectiveChunkSize);
    std::error_code ec;
    fs::create_directories(options.rootDir, ec);

    std::optional<fs::path> selfPath = CurrentExePath();
    if (selfPath.has_value()) {
        std::error_code sec;
        const fs::path canonicalRoot = fs::weakly_canonical(options.rootDir, sec);
        if (sec) {
            selfPath = std::nullopt;
        } else {
            const fs::path canonicalSelf = fs::weakly_canonical(*selfPath, sec);
            if (sec || !IsPathUnderRoot(canonicalRoot, canonicalSelf)) {
                selfPath = std::nullopt;
            } else {
                selfPath = canonicalSelf;
            }
        }
    }

    SocketHandle socket = ConnectTo(options.host, options.port);
    EnsureHandshakeAsClient(socket, options.password);
    if (options.streamAutoTune || options.chunkAutoTune) {
        std::cout << "[auto-tune] streams=" << streamLimit
                  << " chunk-kb=" << (tuned.chunkSize / 1024)
                  << std::endl;
    }
    if (!options.streamAutoTune && streamLimit > 8) {
        std::cerr << "[warning] streams=" << streamLimit
                  << " may increase file transfer failure probability on unstable disks/controllers."
                  << std::endl;
    }

    SendFrame(socket, Frame{MsgType::ManifestRequest, 0, {}});
    std::unordered_map<std::string, FileEntry> remoteFiles;
    std::unordered_set<std::string> remoteDirs;
    std::unordered_map<uint32_t, DownloadState> activeDownloads;
    std::unordered_map<uint32_t, BatchDownloadState> activeBatchDownloads;
    std::unordered_map<uint32_t, std::string> streamToPath;
    std::deque<std::string> pendingTransfers;
    std::deque<std::string> pendingBatchTransfers;
    std::deque<std::string> pendingRetryTransfers;
    std::deque<std::string> pendingRetryBatchTransfers;
    std::unordered_set<std::string> scheduledTransfers;
    std::unordered_map<std::string, uint8_t> transferRetryCounts;

    std::unordered_map<std::string, Hash256> remoteHashes;
    std::unordered_map<std::string, Hash256> localHashes;
    std::unordered_set<std::string> hashResolved;
    std::unordered_set<std::string> hashRequested;
    std::deque<std::string> pendingHashRequests;
    std::unordered_set<std::string> localHashFailed;
    std::mutex fallbackReadyMu;
    std::deque<std::string> fallbackReadyQueue;
    struct CompareTask {
        FileEntry remote;
    };
    struct CompareResult {
        std::string relPath;
        CompareAction action = CompareAction::Skip;
    };
    std::mutex compareTaskMu;
    std::condition_variable compareTaskCv;
    std::deque<CompareTask> compareTasks;
    std::mutex compareResultMu;
    std::deque<CompareResult> compareResults;
    // Diagnostics-only: |local.mtimeNs - remote.mtimeNs| samples for size-equal files.
    std::mutex mtimeDeltaMu;
    std::vector<int64_t> mtimeDeltas;
    std::atomic<bool> compareStop = false;
    std::atomic<size_t> compareTasksIssued = 0;
    std::atomic<size_t> compareResultsHandled = 0;

    std::mutex hashTaskMu;
    std::mutex hashResultMu;
    std::condition_variable hashTaskCv;
    std::deque<ClientHashTask> hashTaskQueue;
    std::atomic<bool> hashStop = false;
    std::atomic<size_t> localHashInFlight = 0;

    const uint32_t workerCount = std::max<uint32_t>(1, std::thread::hardware_concurrency());
    std::vector<std::thread> hashWorkers;
    hashWorkers.reserve(workerCount);
    for (uint32_t i = 0; i < workerCount; ++i) {
        hashWorkers.emplace_back([&]() {
            while (true) {
                ClientHashTask task;
                {
                    std::unique_lock<std::mutex> lock(hashTaskMu);
                    hashTaskCv.wait(lock, [&]() { return hashStop.load() || !hashTaskQueue.empty(); });
                    if (hashStop.load() && hashTaskQueue.empty()) {
                        return;
                    }
                    task = std::move(hashTaskQueue.front());
                    hashTaskQueue.pop_front();
                }
                localHashInFlight.fetch_add(1, std::memory_order_relaxed);
                try {
                    Hash256 hash = ComputeFileHash(task.absPath);
                    {
                        std::lock_guard<std::mutex> lock(hashResultMu);
                        localHashes[task.relPath] = hash;
                    }
                } catch (...) {
                    {
                        std::lock_guard<std::mutex> lock(hashResultMu);
                        localHashFailed.insert(task.relPath);
                    }
                }
                localHashInFlight.fetch_sub(1, std::memory_order_relaxed);
                {
                    std::lock_guard<std::mutex> lock(fallbackReadyMu);
                    fallbackReadyQueue.push_back(task.relPath);
                }
            }
        });
    }

    bool manifestDone = false;
    uint32_t nextStreamId = 1;
    std::deque<FileEntry> delayedCompareEntries;

    size_t enumerated = 0;
    size_t compared = 0;
    size_t unchanged = 0;
    size_t failed = 0;
    size_t transferred = 0;
    size_t deleted = 0;
    size_t fallbackCount = 0;
    size_t fallbackResolved = 0;
    size_t hashRequestsSent = 0;
    size_t hashResponsesReceived = 0;
    const size_t maxInFlightHashRequests = std::clamp<size_t>(
        std::max<size_t>(1024, static_cast<size_t>(streamLimit) * 256), 1024, 8192);
    constexpr uint8_t kMaxTransferRetries = 3;
    size_t lastEnum = 0;
    size_t lastCompared = 0;
    size_t lastUnchanged = 0;
    size_t lastFailed = 0;
    size_t lastTransferred = 0;
    size_t lastDeleted = 0;
    size_t reservedEntryCapacity = 0;
    using steady_clock = std::chrono::steady_clock;
    const auto stallWarnThreshold = std::chrono::minutes(1);
    auto lastForwardProgressAt = steady_clock::now();
    auto lastStallWarnAt = steady_clock::time_point{};
    size_t lastProgressEnumerated = 0;
    size_t lastProgressCompared = 0;
    size_t lastProgressUnchanged = 0;
    size_t lastProgressFailed = 0;
    size_t lastProgressTransferred = 0;
    size_t lastProgressFallbackResolved = 0;
    size_t lastProgressHashResponses = 0;
    size_t lastProgressCompareHandled = 0;

    auto ensureEntryReserve = [&](size_t expectedEntries) {
        const size_t target = expectedEntries + (expectedEntries / 2) + 1024;
        if (target <= reservedEntryCapacity) {
            return;
        }
        remoteFiles.reserve(target);
        remoteHashes.reserve(target);
        hashResolved.reserve(target);
        hashRequested.reserve(target);
        scheduledTransfers.reserve(target);
        transferRetryCounts.reserve(target);
        remoteDirs.reserve((target / 4) + 256);
        {
            // localHashes/localHashFailed are also updated by hash worker threads,
            // so reserve must hold the same mutex to avoid concurrent rehash UB.
            std::lock_guard<std::mutex> lock(hashResultMu);
            localHashes.reserve(target);
            localHashFailed.reserve(target);
        }
        reservedEntryCapacity = target;
    };

    std::mutex incomingMu;
    std::condition_variable incomingDataCv;
    std::deque<Frame> incomingPriorityFrames;
    std::deque<Frame> incomingManifestFrames;
    uint64_t incomingQueuedBytes = 0;
    const uint64_t incomingSoftLimitBytes = std::max<uint64_t>(256ULL * 1024ULL * 1024ULL, options.queuedFileSizeBytes);
    auto frameWireBytes = [](const Frame& f) -> uint64_t {
        return 9ULL + static_cast<uint64_t>(f.payload.size());
    };
    auto isManifestFrame = [](MsgType t) -> bool {
        return t == MsgType::ManifestEntry || t == MsgType::ManifestProgress || t == MsgType::ManifestEnd;
    };
    std::atomic<bool> recvStop = false;
    std::atomic<bool> recvClosed = false;
    std::string recvError;
    std::thread recvThread([&]() {
        try {
            while (!recvStop.load()) {
                Frame f = RecvFrame(socket);
                const bool manifestFrame = isManifestFrame(f.type);
                uint64_t queuedBytesSnapshot = 0;
                {
                    std::lock_guard<std::mutex> lock(incomingMu);
                    if (manifestFrame) {
                        incomingManifestFrames.push_back(std::move(f));
                    } else {
                        incomingPriorityFrames.push_back(std::move(f));
                    }
                    incomingQueuedBytes += frameWireBytes(manifestFrame ? incomingManifestFrames.back() : incomingPriorityFrames.back());
                    queuedBytesSnapshot = incomingQueuedBytes;
                }
                incomingDataCv.notify_one();

                if (queuedBytesSnapshot > incomingSoftLimitBytes) {
                    const uint64_t over = queuedBytesSnapshot - incomingSoftLimitBytes;
                    uint64_t sleepUs = 300;
                    if (over > incomingSoftLimitBytes * 2) {
                        sleepUs = 5000;
                    } else if (over > incomingSoftLimitBytes) {
                        sleepUs = 3000;
                    } else if (over > (incomingSoftLimitBytes / 2)) {
                        sleepUs = 1500;
                    } else if (over > (incomingSoftLimitBytes / 4)) {
                        sleepUs = 700;
                    }
                    std::this_thread::sleep_for(std::chrono::microseconds(sleepUs));
                }
            }
        } catch (const std::exception& ex) {
            if (!recvStop.load()) {
                recvError = ex.what();
                recvClosed.store(true);
                incomingDataCv.notify_all();
            }
        }
    });

    uint64_t smallFileBatchThreshold = 192 * 1024;
    size_t smallBatchMaxFiles = 768;
    size_t smallBatchMaxBytes = 32 * 1024 * 1024;
    auto enqueueTransfer = [&](const std::string& rel, bool isRetry) {
        const auto it = remoteFiles.find(rel);
        const bool useBatch = (it != remoteFiles.end() && it->second.fileSize <= smallFileBatchThreshold);
        if (isRetry) {
            if (useBatch) {
                pendingRetryBatchTransfers.push_back(rel);
            } else {
                pendingRetryTransfers.push_back(rel);
            }
        } else {
            if (useBatch) {
                pendingBatchTransfers.push_back(rel);
            } else {
                pendingTransfers.push_back(rel);
            }
        }
    };

    auto scheduleTransfer = [&](const std::string& rel) {
        if (!scheduledTransfers.insert(rel).second) {
            return;
        }
        enqueueTransfer(rel, false);
    };

    auto markTransferFailed = [&](const std::string& rel) {
        ++compared;
        ++failed;
        if (debugEnabled) {
            std::cerr << "[debug][client] transfer_failed path=" << rel << std::endl;
        }
    };

    auto retryOrFail = [&](const std::string& rel) {
        uint8_t& retries = transferRetryCounts[rel];
        if (retries < kMaxTransferRetries) {
            ++retries;
            if (debugEnabled) {
                std::cerr << "[debug][client] transfer_retry path=" << rel
                          << " attempt=" << static_cast<uint32_t>(retries)
                          << "/" << static_cast<uint32_t>(kMaxTransferRetries)
                          << std::endl;
            }
            enqueueTransfer(rel, true);
        } else {
            markTransferFailed(rel);
            transferRetryCounts.erase(rel);
        }
    };

    auto refreshSmallBatchTuning = [&]() {
        const size_t backlogBatch = pendingBatchTransfers.size() + pendingRetryBatchTransfers.size();
        const size_t backlogRegular = pendingTransfers.size() + pendingRetryTransfers.size();
        const size_t backlog = backlogBatch + backlogRegular;
        uint64_t threshold = 192 * 1024;
        size_t maxFiles = 768;
        size_t maxBytes = 32 * 1024 * 1024;

        if (backlog > 120000) {
            threshold = 512 * 1024;
            maxFiles = 2048;
            maxBytes = 96 * 1024 * 1024;
        } else if (backlog > 60000) {
            threshold = 384 * 1024;
            maxFiles = 1536;
            maxBytes = 64 * 1024 * 1024;
        } else if (backlog > 16000) {
            threshold = 256 * 1024;
            maxFiles = 1024;
            maxBytes = 48 * 1024 * 1024;
        } else if (backlog < 1000) {
            threshold = 128 * 1024;
            maxFiles = 512;
            maxBytes = 20 * 1024 * 1024;
        }

        // Low stream mode needs larger per-batch payload to amortize per-file
        // transaction overhead (open/end/mtime) without raising stream count.
        if (streamLimit <= 8) {
            threshold = std::max<uint64_t>(threshold, 320 * 1024);
            maxBytes = std::max<size_t>(maxBytes, 64 * 1024 * 1024);
            maxFiles = std::max<size_t>(maxFiles, 1280);
        }

        smallFileBatchThreshold = threshold;
        smallBatchMaxFiles = maxFiles;
        smallBatchMaxBytes = maxBytes;
    };

    auto rebalanceTransfersTowardBatch = [&]() {
        // Reclassify part of regular queues into batch queues based on current
        // dynamic threshold. This fixes early queueing decisions when threshold
        // increases later under heavy backlog.
        auto moveEligible = [&](std::deque<std::string>& from, std::deque<std::string>& to, size_t budget) {
            if (from.empty() || budget == 0) {
                return size_t{0};
            }
            std::deque<std::string> remain;
            size_t moved = 0;
            while (!from.empty()) {
                std::string rel = std::move(from.front());
                from.pop_front();
                bool eligible = false;
                const auto it = remoteFiles.find(rel);
                if (it != remoteFiles.end()) {
                    eligible = (it->second.fileSize <= smallFileBatchThreshold);
                }
                if (eligible && moved < budget) {
                    to.push_back(std::move(rel));
                    ++moved;
                } else {
                    remain.push_back(std::move(rel));
                }
            }
            from.swap(remain);
            return moved;
        };

        // Keep per-loop reclassification bounded.
        const size_t moveBudget = std::max<size_t>(512, smallBatchMaxFiles);
        const size_t moved = moveEligible(pendingTransfers, pendingBatchTransfers, moveBudget);
        const size_t retryBudgetBase = std::max<size_t>(moveBudget / 2, size_t{256});
        const size_t retryBudget = (moved >= retryBudgetBase) ? size_t{0} : (retryBudgetBase - moved);
        moveEligible(pendingRetryTransfers, pendingRetryBatchTransfers, retryBudget);
    };

    auto probeLocalFile = [&](const std::string& relPath) -> std::optional<FileEntry> {
        const fs::path abs = JoinRel(options.rootDir, relPath);
        std::error_code ec;
        if (!fs::exists(abs, ec) || ec) {
            return std::nullopt;
        }
        if (!fs::is_regular_file(abs, ec) || ec) {
            return std::nullopt;
        }
        FileEntry entry;
        entry.relativePath = relPath;
        entry.isDirectory = false;
        entry.fileSize = static_cast<uint64_t>(fs::file_size(abs, ec));
        if (ec) {
            return std::nullopt;
        }
        // Compare hot path: use last_write_time directly to avoid per-file handle
        // open/close overhead from ReadFileMtimeCanonical on huge unchanged sets.
        // DecideCompareAction() already normalizes Unix-ns vs FILETIME ticks before
        // applying tolerance, so cross-platform correctness is preserved.
        entry.mtimeNs = ToUnixNs(fs::last_write_time(abs, ec));
        if (ec) {
            return std::nullopt;
        }
        return entry;
    };

    // Compare workers follow logical CPU concurrency (hardware_concurrency),
    // with a small safety floor for low-core machines.
    const uint32_t compareWorkerCount = std::max<uint32_t>(4, workerCount);
    const size_t maxInFlightCompareTasks = std::max<size_t>(1024, static_cast<size_t>(compareWorkerCount) * 64);
    std::vector<std::thread> compareWorkers;
    compareWorkers.reserve(compareWorkerCount);
    for (uint32_t i = 0; i < compareWorkerCount; ++i) {
        compareWorkers.emplace_back([&]() {
            std::vector<int64_t> localMtimeDeltas;
            while (true) {
                CompareTask task;
                {
                    std::unique_lock<std::mutex> lock(compareTaskMu);
                    compareTaskCv.wait(lock, [&]() { return compareStop.load() || !compareTasks.empty(); });
                    if (compareStop.load() && compareTasks.empty()) {
                        break;
                    }
                    task = std::move(compareTasks.front());
                    compareTasks.pop_front();
                }
                CompareResult result;
                result.relPath = task.remote.relativePath;
                const std::optional<FileEntry> localProbe = probeLocalFile(task.remote.relativePath);
                if (diagnostics && localProbe.has_value() && localProbe->fileSize == task.remote.fileSize) {
                    localMtimeDeltas.push_back(std::llabs(static_cast<long long>(localProbe->mtimeNs - task.remote.mtimeNs)));
                }
                result.action = DecideCompareAction(localProbe, task.remote);
                {
                    std::lock_guard<std::mutex> lock(compareResultMu);
                    compareResults.push_back(std::move(result));
                }
            }
            if (diagnostics && !localMtimeDeltas.empty()) {
                std::lock_guard<std::mutex> lock(mtimeDeltaMu);
                mtimeDeltas.insert(mtimeDeltas.end(), localMtimeDeltas.begin(), localMtimeDeltas.end());
            }
        });
    }

    auto handleCompareResult = [&](const CompareResult& r) {
        const CompareAction action = r.action;
        if (action == CompareAction::TransferNow) {
            scheduleTransfer(r.relPath);
        } else if (action == CompareAction::Skip) {
            ++compared;
            ++unchanged;
        } else {
            if (!hashRequested.contains(r.relPath)) {
                hashRequested.insert(r.relPath);
                ++fallbackCount;
                pendingHashRequests.push_back(r.relPath);
            }
        }
        PrintClientCounters(enumerated, compared, unchanged, failed, transferred, deleted, lastEnum, lastCompared, lastUnchanged, lastFailed, lastTransferred, lastDeleted);
    };

    auto dispatchHashRequests = [&]() {
        std::vector<Frame> outboundFrames;
        outboundFrames.reserve(256);
        while (!pendingHashRequests.empty() && (hashRequestsSent - hashResponsesReceived) < maxInFlightHashRequests) {
            const std::string rel = pendingHashRequests.front();
            pendingHashRequests.pop_front();
            ++hashRequestsSent;
            outboundFrames.push_back(Frame{MsgType::HashRequest, 0, EncodeHashRequest(rel)});
            {
                std::lock_guard<std::mutex> lock(hashTaskMu);
                hashTaskQueue.push_back(ClientHashTask{rel, JoinRel(options.rootDir, rel)});
            }
            hashTaskCv.notify_one();
            if (outboundFrames.size() >= 256) {
                SendFrameBatch(socket, outboundFrames);
                outboundFrames.clear();
            }
        }
        if (!outboundFrames.empty()) {
            SendFrameBatch(socket, outboundFrames);
        }
    };

    auto tryStartTransfers = [&]() {
        auto activeTransferSlots = [&]() -> size_t {
            return activeDownloads.size() + activeBatchDownloads.size();
        };
        while (activeTransferSlots() < streamLimit) {
            bool started = false;
            std::deque<std::string>* batchQueue = nullptr;
            std::deque<std::string>* regularQueue = nullptr;
            if (!pendingBatchTransfers.empty()) {
                batchQueue = &pendingBatchTransfers;
            } else if (!pendingTransfers.empty()) {
                regularQueue = &pendingTransfers;
            } else if (!pendingRetryBatchTransfers.empty()) {
                batchQueue = &pendingRetryBatchTransfers;
            } else if (!pendingRetryTransfers.empty()) {
                regularQueue = &pendingRetryTransfers;
            }

            if (batchQueue != nullptr) {
                std::vector<std::string> batchPaths;
                batchPaths.reserve(smallBatchMaxFiles);
                size_t batchBytes = 0;
                while (!batchQueue->empty() && batchPaths.size() < smallBatchMaxFiles && batchBytes < smallBatchMaxBytes) {
                    const std::string rel = batchQueue->front();
                    batchQueue->pop_front();
                    batchPaths.push_back(rel);
                    const auto it = remoteFiles.find(rel);
                    if (it != remoteFiles.end()) {
                        batchBytes += static_cast<size_t>(it->second.fileSize);
                    }
                    if (batchBytes >= smallBatchMaxBytes) {
                        break;
                    }
                }
                if (!batchPaths.empty()) {
                    const uint32_t sid = nextStreamId++;
                    BatchDownloadState batchState;
                    activeBatchDownloads.emplace(sid, std::move(batchState));
                    SendFrame(socket, Frame{MsgType::FileBatchOpen, sid, EncodeFileBatchRequest(batchPaths)});
                    started = true;
                }
            } else if (regularQueue != nullptr) {
                const std::string rel = regularQueue->front();
                regularQueue->pop_front();
                const fs::path abs = JoinRel(options.rootDir, rel);
                EnsureParentDir(abs);
                DownloadState d;
                d.relPath = rel;
                d.output.open(abs, std::ios::binary | std::ios::trunc);
                if (!d.output) {
                    retryOrFail(rel);
                    PrintClientCounters(enumerated, compared, unchanged, failed, transferred, deleted, lastEnum, lastCompared, lastUnchanged, lastFailed, lastTransferred, lastDeleted);
                } else {
                    const uint32_t sid = nextStreamId++;
                    d.flushThreshold = downloadFlushThreshold;
                    d.writeBuffer.reserve(d.flushThreshold);
                    activeDownloads.emplace(sid, std::move(d));
                    streamToPath.emplace(sid, rel);
                    SendFrame(socket, Frame{MsgType::FileOpen, sid, EncodeFileOpen(rel)});
                }
                started = true;
            }
            if (!started) {
                break;
            }
        }
    };

    auto flushBufferedWrites = [&](DownloadState& d) {
        if (d.writeBuffer.empty()) {
            return;
        }
        d.output.write(reinterpret_cast<const char*>(d.writeBuffer.data()), static_cast<std::streamsize>(d.writeBuffer.size()));
        d.writeBuffer.clear();
    };

    auto finalizeBatchEntry = [&](BatchDownloadEntry& entry) {
        if (entry.finalized) {
            return;
        }
        if (entry.shouldWrite) {
            if (entry.output.is_open()) {
                entry.output.flush();
                entry.output.close();
            }
            SetFileModifyTime(JoinRel(options.rootDir, entry.relPath), entry.mtimeNs);
            ++compared;
            ++transferred;
            transferRetryCounts.erase(entry.relPath);
        } else {
            if (entry.output.is_open()) {
                entry.output.close();
            }
            if (!entry.serverOk) {
                // Server explicitly reported this entry as unavailable; retrying won't help.
                markTransferFailed(entry.relPath);
                transferRetryCounts.erase(entry.relPath);
            } else {
                retryOrFail(entry.relPath);
            }
        }
        entry.finalized = true;
        PrintClientCounters(enumerated, compared, unchanged, failed, transferred, deleted, lastEnum, lastCompared, lastUnchanged, lastFailed, lastTransferred, lastDeleted);
    };

    auto resolveFallbackIfReady = [&]() {
        std::deque<std::string> readyCandidates;
        {
            std::lock_guard<std::mutex> lock(fallbackReadyMu);
            readyCandidates.swap(fallbackReadyQueue);
        }
        for (const std::string& rel : readyCandidates) {
            if (hashResolved.contains(rel)) {
                continue;
            }
            Hash256 localHash{};
            Hash256 remoteHash{};
            bool remoteHashReady = false;
            bool localHashReady = false;
            bool localFailed = false;
            {
                std::lock_guard<std::mutex> lock(hashResultMu);
                remoteHashReady = remoteHashes.contains(rel);
                if (remoteHashReady) {
                    remoteHash = remoteHashes.at(rel);
                }
                localHashReady = localHashes.contains(rel);
                localFailed = localHashFailed.contains(rel);
                if (localHashReady) {
                    localHash = localHashes.at(rel);
                }
            }
            if (!remoteHashReady || (!localHashReady && !localFailed)) {
                continue;
            }
            if (localFailed || !localHashReady || !HashEquals(localHash, remoteHash)) {
                scheduleTransfer(rel);
            } else {
                const FileEntry& meta = remoteFiles.at(rel);
                SetFileModifyTime(JoinRel(options.rootDir, rel), meta.mtimeNs);
                ++compared;
                ++unchanged;
            }
            hashResolved.insert(rel);
            ++fallbackResolved;
            PrintClientCounters(enumerated, compared, unchanged, failed, transferred, deleted, lastEnum, lastCompared, lastUnchanged, lastFailed, lastTransferred, lastDeleted);
        }
    };

    auto sweepUnresolvedFallbackIfQuiescent = [&]() {
        if (fallbackResolved >= fallbackCount) {
            return;
        }
        if (!pendingHashRequests.empty()) {
            return;
        }
        if (hashRequestsSent != hashResponsesReceived) {
            return;
        }
        if (localHashInFlight.load(std::memory_order_relaxed) != 0) {
            return;
        }
        std::vector<std::string> unresolved;
        unresolved.reserve(hashRequested.size());
        for (const std::string& rel : hashRequested) {
            if (!hashResolved.contains(rel)) {
                unresolved.push_back(rel);
            }
        }
        if (unresolved.empty()) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(fallbackReadyMu);
            for (const std::string& rel : unresolved) {
                fallbackReadyQueue.push_back(rel);
            }
        }
        resolveFallbackIfReady();

        std::vector<std::string> stillUnresolved;
        stillUnresolved.reserve(unresolved.size());
        for (const std::string& rel : unresolved) {
            if (!hashResolved.contains(rel)) {
                stillUnresolved.push_back(rel);
            }
        }
        if (stillUnresolved.empty()) {
            return;
        }

        for (const std::string& rel : stillUnresolved) {
            if (hashResolved.contains(rel)) {
                continue;
            }
            // Fail-safe: if fallback cannot make progress in a fully quiescent state,
            // force transfer to guarantee forward progress and avoid infinite stall.
            scheduleTransfer(rel);
            hashResolved.insert(rel);
            ++fallbackResolved;
            std::cerr << "[warn][client] fallback_force_transfer path=" << rel << std::endl;
            PrintClientCounters(enumerated, compared, unchanged, failed, transferred, deleted,
                                lastEnum, lastCompared, lastUnchanged, lastFailed, lastTransferred, lastDeleted);
        }
    };

    auto processIncomingFrame = [&](Frame& frame) {
        if (frame.type == MsgType::ManifestEntry) {
            FileEntry e = DecodeManifestEntry(frame.payload);
            if (e.isDirectory) {
                remoteDirs.insert(e.relativePath);
                std::error_code mkec;
                fs::create_directories(JoinRel(options.rootDir, e.relativePath), mkec);
                return;
            }
            remoteFiles[e.relativePath] = e;
            ++enumerated;
            if ((enumerated % 2048) == 0) {
                ensureEntryReserve(enumerated + 2048);
            }
            if ((compareTasksIssued.load() - compareResultsHandled.load()) >= maxInFlightCompareTasks) {
                delayedCompareEntries.push_back(e);
            } else {
                {
                    std::lock_guard<std::mutex> lock(compareTaskMu);
                    compareTasks.push_back(CompareTask{e});
                }
                ++compareTasksIssued;
                compareTaskCv.notify_one();
            }
            PrintClientCounters(enumerated, compared, unchanged, failed, transferred, deleted, lastEnum, lastCompared, lastUnchanged, lastFailed, lastTransferred, lastDeleted);
        } else if (frame.type == MsgType::ManifestProgress) {
            size_t cursor = 0;
            const uint64_t serverEnumerated = ReadU64(frame.payload, cursor);
            ensureEntryReserve(static_cast<size_t>(serverEnumerated));
            if (serverEnumerated > enumerated) {
                enumerated = static_cast<size_t>(serverEnumerated);
            }
        } else if (frame.type == MsgType::ManifestEnd) {
            manifestDone = true;
        } else if (frame.type == MsgType::HashResponse) {
            auto value = DecodeHashResponse(frame.payload);
            {
                std::lock_guard<std::mutex> lock(hashResultMu);
                remoteHashes[value.first] = value.second;
            }
            {
                std::lock_guard<std::mutex> lock(fallbackReadyMu);
                fallbackReadyQueue.push_back(value.first);
            }
            ++hashResponsesReceived;
        } else if (frame.type == MsgType::FileBatchOpen) {
            auto itBatch = activeBatchDownloads.find(frame.streamId);
            if (itBatch == activeBatchDownloads.end()) {
                throw std::runtime_error("Received batch open for unknown stream");
            }
            BatchDownloadState& batch = itBatch->second;
            batch.entries.clear();
            batch.currentIndex = 0;
            std::vector<BatchFileRecord> response = DecodeFileBatchOpenResponse(frame.payload);
            batch.entries.reserve(response.size());
            for (auto& rec : response) {
                BatchDownloadEntry entry;
                entry.relPath = rec.relativePath;
                entry.fileSize = rec.fileSize;
                entry.mtimeNs = rec.mtimeNs;
                entry.serverOk = rec.ok;
                entry.shouldWrite = entry.serverOk;
                if (entry.serverOk && entry.fileSize == 0) {
                    const fs::path abs = JoinRel(options.rootDir, entry.relPath);
                    EnsureParentDir(abs);
                    std::ofstream out(abs, std::ios::binary | std::ios::trunc);
                    entry.shouldWrite = out.good();
                }
                if (!entry.serverOk || (entry.fileSize == 0 && entry.serverOk)) {
                    finalizeBatchEntry(entry);
                }
                batch.entries.push_back(std::move(entry));
            }
            while (batch.currentIndex < batch.entries.size() &&
                   (batch.entries[batch.currentIndex].finalized || batch.entries[batch.currentIndex].fileSize == 0)) {
                ++batch.currentIndex;
            }
            batch.headerReady = true;
        } else if (frame.type == MsgType::FileBatchChunk) {
            auto itBatch = activeBatchDownloads.find(frame.streamId);
            if (itBatch == activeBatchDownloads.end()) {
                throw std::runtime_error("Received batch chunk for unknown stream");
            }
            BatchDownloadState& batch = itBatch->second;
            if (!batch.headerReady) {
                throw std::runtime_error("Received batch chunk before batch open");
            }
            size_t offset = 0;
            while (offset < frame.payload.size()) {
                while (batch.currentIndex < batch.entries.size() &&
                       (batch.entries[batch.currentIndex].finalized || batch.entries[batch.currentIndex].fileSize == 0 ||
                        batch.entries[batch.currentIndex].received >= batch.entries[batch.currentIndex].fileSize)) {
                    if (batch.currentIndex < batch.entries.size() &&
                        !batch.entries[batch.currentIndex].finalized &&
                        batch.entries[batch.currentIndex].received >= batch.entries[batch.currentIndex].fileSize) {
                        finalizeBatchEntry(batch.entries[batch.currentIndex]);
                    }
                    ++batch.currentIndex;
                }
                if (batch.currentIndex >= batch.entries.size()) {
                    break;
                }
                BatchDownloadEntry& entry = batch.entries[batch.currentIndex];
                const uint64_t remainingForEntry = entry.fileSize - entry.received;
                const size_t available = frame.payload.size() - offset;
                const size_t take = static_cast<size_t>(std::min<uint64_t>(remainingForEntry, static_cast<uint64_t>(available)));
                if (entry.shouldWrite) {
                    if (!entry.output.is_open()) {
                        const fs::path abs = JoinRel(options.rootDir, entry.relPath);
                        EnsureParentDir(abs);
                        entry.output.open(abs, std::ios::binary | std::ios::trunc);
                        if (!entry.output.good()) {
                            entry.shouldWrite = false;
                        }
                    }
                    if (entry.shouldWrite) {
                        entry.output.write(reinterpret_cast<const char*>(frame.payload.data() + offset), static_cast<std::streamsize>(take));
                    }
                }
                entry.received += take;
                offset += take;
                if (entry.received >= entry.fileSize) {
                    finalizeBatchEntry(entry);
                    ++batch.currentIndex;
                }
            }
        } else if (frame.type == MsgType::FileBatchEnd) {
            auto itBatch = activeBatchDownloads.find(frame.streamId);
            if (itBatch == activeBatchDownloads.end()) {
                throw std::runtime_error("Received batch end for unknown stream");
            }
            BatchDownloadState& batch = itBatch->second;
            for (auto& entry : batch.entries) {
                if (!entry.finalized) {
                    if (entry.received < entry.fileSize) {
                        entry.shouldWrite = false;
                    }
                    finalizeBatchEntry(entry);
                }
            }
            activeBatchDownloads.erase(itBatch);
        } else if (frame.type == MsgType::FileChunk) {
            auto it = activeDownloads.find(frame.streamId);
            if (it == activeDownloads.end()) {
                throw std::runtime_error("Received chunk for unknown stream");
            }
            DownloadState& d = it->second;
            d.writeBuffer.insert(d.writeBuffer.end(), frame.payload.begin(), frame.payload.end());
            if (d.writeBuffer.size() >= d.flushThreshold) {
                flushBufferedWrites(d);
            }
        } else if (frame.type == MsgType::FileEnd) {
            auto it = activeDownloads.find(frame.streamId);
            if (it == activeDownloads.end()) {
                throw std::runtime_error("Received end for unknown stream");
            }
            flushBufferedWrites(it->second);
            it->second.output.flush();
            it->second.output.close();
            const std::string rel = it->second.relPath;
            const FileEntry& meta = remoteFiles.at(rel);
            SetFileModifyTime(JoinRel(options.rootDir, rel), meta.mtimeNs);
            activeDownloads.erase(it);
            streamToPath.erase(frame.streamId);
            ++compared;
            ++transferred;
            transferRetryCounts.erase(rel);
            PrintClientCounters(enumerated, compared, unchanged, failed, transferred, deleted, lastEnum, lastCompared, lastUnchanged, lastFailed, lastTransferred, lastDeleted);
        } else if (frame.type == MsgType::FileError) {
            auto itBatch = activeBatchDownloads.find(frame.streamId);
            if (itBatch != activeBatchDownloads.end()) {
                for (auto& entry : itBatch->second.entries) {
                    if (!entry.finalized) {
                        entry.shouldWrite = false;
                        finalizeBatchEntry(entry);
                    }
                }
                activeBatchDownloads.erase(itBatch);
                return;
            }
            auto itPath = streamToPath.find(frame.streamId);
            auto itDl = activeDownloads.find(frame.streamId);
            if (itDl != activeDownloads.end()) {
                itDl->second.writeBuffer.clear();
                itDl->second.output.close();
                activeDownloads.erase(itDl);
            }
            std::string relPath;
            bool hasRelPath = false;
            if (itPath != streamToPath.end()) {
                relPath = itPath->second;
                hasRelPath = true;
                streamToPath.erase(itPath);
            }
            if (hasRelPath) {
                retryOrFail(relPath);
            } else {
                ++compared;
                ++failed;
                if (debugEnabled) {
                    std::cerr << "[debug][client] transfer_failed path=<unknown> stream=" << frame.streamId << std::endl;
                }
            }
            PrintClientCounters(enumerated, compared, unchanged, failed, transferred, deleted, lastEnum, lastCompared, lastUnchanged, lastFailed, lastTransferred, lastDeleted);
        } else {
            throw std::runtime_error("Unexpected frame in client stream loop");
        }
    };

    auto updateStallWatchdog = [&](bool loopHadForwardProgress) {
        const size_t compareHandledNow = compareResultsHandled.load();
        const bool countersAdvanced = loopHadForwardProgress ||
                                      (enumerated != lastProgressEnumerated) ||
                                      (compared != lastProgressCompared) ||
                                      (unchanged != lastProgressUnchanged) ||
                                      (failed != lastProgressFailed) ||
                                      (transferred != lastProgressTransferred) ||
                                      (fallbackResolved != lastProgressFallbackResolved) ||
                                      (hashResponsesReceived != lastProgressHashResponses) ||
                                      (compareHandledNow != lastProgressCompareHandled);
        const auto now = steady_clock::now();
        if (countersAdvanced) {
            lastForwardProgressAt = now;
            lastProgressEnumerated = enumerated;
            lastProgressCompared = compared;
            lastProgressUnchanged = unchanged;
            lastProgressFailed = failed;
            lastProgressTransferred = transferred;
            lastProgressFallbackResolved = fallbackResolved;
            lastProgressHashResponses = hashResponsesReceived;
            lastProgressCompareHandled = compareHandledNow;
            lastStallWarnAt = steady_clock::time_point{};
            return;
        }
        const bool enoughToWarn = (now - lastForwardProgressAt) >= stallWarnThreshold;
        const bool warnCooldownPassed =
            (lastStallWarnAt == steady_clock::time_point{}) || ((now - lastStallWarnAt) >= stallWarnThreshold);
        if (!enoughToWarn || !warnCooldownPassed) {
            return;
        }
        size_t queuedIncomingPriorityFrames = 0;
        size_t queuedIncomingManifestFrames = 0;
        uint64_t queuedIncomingBytes = 0;
        {
            std::lock_guard<std::mutex> lock(incomingMu);
            queuedIncomingPriorityFrames = incomingPriorityFrames.size();
            queuedIncomingManifestFrames = incomingManifestFrames.size();
            queuedIncomingBytes = incomingQueuedBytes;
        }
        size_t queuedCompareTasks = 0;
        {
            std::lock_guard<std::mutex> lock(compareTaskMu);
            queuedCompareTasks = compareTasks.size();
        }
        size_t queuedHashTasks = 0;
        {
            std::lock_guard<std::mutex> lock(hashTaskMu);
            queuedHashTasks = hashTaskQueue.size();
        }
        const size_t hashLocalInflight = localHashInFlight.load(std::memory_order_relaxed);
        const size_t compareInflight = compareTasksIssued.load() - compareResultsHandled.load();
        const size_t hashInflight = hashRequestsSent - hashResponsesReceived;
        const size_t pendingTransfersTotal = pendingTransfers.size() + pendingBatchTransfers.size() +
                                             pendingRetryTransfers.size() + pendingRetryBatchTransfers.size();
        std::string stallClass;
        auto addStallTag = [&](const char* tag) {
            if (!stallClass.empty()) {
                stallClass.push_back('|');
            }
            stallClass += tag;
        };
        const size_t fallbackOpen = fallbackCount - fallbackResolved;
        if (pendingTransfersTotal > 0 || !activeDownloads.empty() || !activeBatchDownloads.empty()) {
            addStallTag("transfer_backlog");
        }
        if (compareInflight > 0 || queuedCompareTasks > 0 || !delayedCompareEntries.empty()) {
            addStallTag("compare_backlog");
        }
        if (fallbackOpen > 0 || hashInflight > 0 || queuedHashTasks > 0 || hashLocalInflight > 0 || !pendingHashRequests.empty()) {
            addStallTag("hash_wait");
        }
        if (hashLocalInflight > 0) {
            addStallTag("hash_local_compute");
        }
        if (!manifestDone || queuedIncomingManifestFrames > 0) {
            addStallTag("manifest_wait");
        }
        const bool waitingForNetwork = (!manifestDone) || !activeDownloads.empty() || !activeBatchDownloads.empty() ||
                                       (hashResponsesReceived < hashRequestsSent);
        const bool incomingEmpty = (queuedIncomingPriorityFrames == 0 && queuedIncomingManifestFrames == 0);
        if (waitingForNetwork && incomingEmpty) {
            addStallTag("network_idle");
        }
        if (stallClass.empty()) {
            addStallTag("unknown");
        }
        std::cerr << "[warn][client] suspected_stall stall_s="
                  << std::chrono::duration_cast<std::chrono::seconds>(now - lastForwardProgressAt).count()
                  << " stall_class=" << stallClass
                  << " enum=" << enumerated
                  << " compared=" << compared
                  << " unchanged=" << unchanged
                  << " failed=" << failed
                  << " transferred=" << transferred
                  << " manifest_done=" << (manifestDone ? 1 : 0)
                  << " compare_inflight=" << compareInflight
                  << " compare_queued=" << queuedCompareTasks
                  << " hash_inflight=" << hashInflight
                  << " hash_local_queued=" << queuedHashTasks
                  << " hash_local_inflight=" << hashLocalInflight
                  << " hash_pending_req=" << pendingHashRequests.size()
                  << " fallback_open=" << fallbackOpen
                  << " pending_transfers=" << pendingTransfersTotal
                  << " active_downloads=" << activeDownloads.size()
                  << " active_batches=" << activeBatchDownloads.size()
                  << " queued_incoming_prio=" << queuedIncomingPriorityFrames
                  << " queued_incoming_manifest=" << queuedIncomingManifestFrames
                  << " queued_incoming_mb=" << (queuedIncomingBytes / (1024ULL * 1024ULL))
                  << std::endl;
        lastStallWarnAt = now;
    };

    try {
        auto lastDebugPrint = std::chrono::steady_clock::now();
        while (true) {
            bool loopHadForwardProgress = false;
            resolveFallbackIfReady();
            dispatchHashRequests();
            refreshSmallBatchTuning();
            rebalanceTransfersTowardBatch();
            tryStartTransfers();
            {
                std::deque<CompareResult> ready;
                {
                    std::lock_guard<std::mutex> lock(compareResultMu);
                    ready.swap(compareResults);
                }
                for (const auto& r : ready) {
                    handleCompareResult(r);
                    ++compareResultsHandled;
                }
                if (!ready.empty()) {
                    loopHadForwardProgress = true;
                }
            }
            while (!delayedCompareEntries.empty() &&
                   (compareTasksIssued.load() - compareResultsHandled.load()) < maxInFlightCompareTasks) {
                FileEntry e = std::move(delayedCompareEntries.front());
                delayedCompareEntries.pop_front();
                {
                    std::lock_guard<std::mutex> lock(compareTaskMu);
                    compareTasks.push_back(CompareTask{e});
                }
                ++compareTasksIssued;
                compareTaskCv.notify_one();
            }
            dispatchHashRequests();

            if (debugEnabled) {
                const auto now = std::chrono::steady_clock::now();
                if ((now - lastDebugPrint) >= std::chrono::seconds(1)) {
                    size_t readyCompareResults = 0;
                    {
                        std::lock_guard<std::mutex> lock(compareResultMu);
                        readyCompareResults = compareResults.size();
                    }
                    size_t queuedCompareTasks = 0;
                    {
                        std::lock_guard<std::mutex> lock(compareTaskMu);
                        queuedCompareTasks = compareTasks.size();
                    }
                    size_t queuedIncomingFrames = 0;
                    size_t queuedIncomingPriorityFrames = 0;
                    size_t queuedIncomingManifestFrames = 0;
                    uint64_t queuedIncomingBytes = 0;
                    {
                        std::lock_guard<std::mutex> lock(incomingMu);
                        queuedIncomingPriorityFrames = incomingPriorityFrames.size();
                        queuedIncomingManifestFrames = incomingManifestFrames.size();
                        queuedIncomingFrames = queuedIncomingPriorityFrames + queuedIncomingManifestFrames;
                        queuedIncomingBytes = incomingQueuedBytes;
                    }
                    size_t queuedHashTasks = 0;
                    {
                        std::lock_guard<std::mutex> lock(hashTaskMu);
                        queuedHashTasks = hashTaskQueue.size();
                    }
                    const size_t compareInflight = compareTasksIssued.load() - compareResultsHandled.load();
                    const size_t hashInflight = hashRequestsSent - hashResponsesReceived;
                    const size_t hashLocalInflight = localHashInFlight.load(std::memory_order_relaxed);
                    std::cerr << "[debug][client] enum=" << enumerated
                              << " compared=" << compared
                              << " unchanged=" << unchanged
                              << " failed=" << failed
                              << " transferred=" << transferred
                              << " in_flight_hash=" << hashInflight
                              << " pending_hash_req=" << pendingHashRequests.size()
                              << " pending_hash_local=" << queuedHashTasks
                              << " in_flight_hash_local=" << hashLocalInflight
                              << " in_flight_compare=" << compareInflight
                              << " queued_compare_tasks=" << queuedCompareTasks
                              << " ready_compare_results=" << readyCompareResults
                              << " delayed_compare_entries=" << delayedCompareEntries.size()
                              << " queued_incoming_frames=" << queuedIncomingFrames
                              << " queued_incoming_prio=" << queuedIncomingPriorityFrames
                              << " queued_incoming_manifest=" << queuedIncomingManifestFrames
                              << " queued_incoming_mb=" << (queuedIncomingBytes / (1024ULL * 1024ULL))
                              << " queued_limit_mb=" << (incomingSoftLimitBytes / (1024ULL * 1024ULL))
                              << " pending_transfers=" << (pendingTransfers.size() + pendingBatchTransfers.size() +
                                                           pendingRetryTransfers.size() + pendingRetryBatchTransfers.size())
                              << " active_downloads=" << activeDownloads.size()
                              << " active_batches=" << activeBatchDownloads.size()
                              << " batch_thr_kb=" << (smallFileBatchThreshold / 1024)
                              << " batch_max_files=" << smallBatchMaxFiles
                              << " batch_max_kb=" << (smallBatchMaxBytes / 1024)
                              << " fallback_open=" << (fallbackCount - fallbackResolved)
                              << std::endl;
                    lastDebugPrint = now;
                }
            }

            const bool allHashDone = (fallbackResolved == fallbackCount);
            const bool allCompareDone = (compareResultsHandled.load() == compareTasksIssued.load());
            if (manifestDone && allCompareDone &&
                pendingTransfers.empty() && pendingBatchTransfers.empty() &&
                pendingRetryTransfers.empty() && pendingRetryBatchTransfers.empty() &&
                activeDownloads.empty() && activeBatchDownloads.empty() && allHashDone) {
                break;
            }
            const bool needNetworkFrame = !manifestDone || !activeDownloads.empty() || !activeBatchDownloads.empty() ||
                                          (hashResponsesReceived < hashRequestsSent);
            sweepUnresolvedFallbackIfQuiescent();
            if (!needNetworkFrame) {
                updateStallWatchdog(loopHadForwardProgress);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            std::deque<Frame> readyFrames;
            {
                std::lock_guard<std::mutex> lock(incomingMu);
                size_t kDrainBudget = 512;
                if (incomingQueuedBytes > incomingSoftLimitBytes * 3) {
                    kDrainBudget = 4096;
                } else if (incomingQueuedBytes > incomingSoftLimitBytes * 2) {
                    kDrainBudget = 3072;
                } else if (incomingQueuedBytes > incomingSoftLimitBytes) {
                    kDrainBudget = 2048;
                } else if (incomingQueuedBytes > (incomingSoftLimitBytes / 2)) {
                    kDrainBudget = 1024;
                }
                const size_t kPriorityBudget = (kDrainBudget * 3) / 4;
                const size_t prioCount = std::min<size_t>(incomingPriorityFrames.size(), kPriorityBudget);
                for (size_t i = 0; i < prioCount; ++i) {
                    Frame frame = std::move(incomingPriorityFrames.front());
                    incomingPriorityFrames.pop_front();
                    const uint64_t sz = frameWireBytes(frame);
                    incomingQueuedBytes = (incomingQueuedBytes >= sz) ? (incomingQueuedBytes - sz) : 0;
                    readyFrames.push_back(std::move(frame));
                }
                const size_t remaining = kDrainBudget - readyFrames.size();
                const size_t manifestCount = std::min<size_t>(incomingManifestFrames.size(), remaining);
                for (size_t i = 0; i < manifestCount; ++i) {
                    Frame frame = std::move(incomingManifestFrames.front());
                    incomingManifestFrames.pop_front();
                    const uint64_t sz = frameWireBytes(frame);
                    incomingQueuedBytes = (incomingQueuedBytes >= sz) ? (incomingQueuedBytes - sz) : 0;
                    readyFrames.push_back(std::move(frame));
                }
            }
            if (readyFrames.empty()) {
                std::unique_lock<std::mutex> lock(incomingMu);
                incomingDataCv.wait_for(lock, std::chrono::milliseconds(2), [&]() {
                    return recvClosed.load() || !incomingPriorityFrames.empty() || !incomingManifestFrames.empty();
                });
                if (recvClosed.load() && incomingPriorityFrames.empty() && incomingManifestFrames.empty()) {
                    break;
                }
                updateStallWatchdog(loopHadForwardProgress);
                continue;
            }
            for (auto& frame : readyFrames) {
                processIncomingFrame(frame);
            }
            if (!readyFrames.empty()) {
                loopHadForwardProgress = true;
            }
            updateStallWatchdog(loopHadForwardProgress);
        }
    } catch (...) {
        recvStop.store(true);
        incomingDataCv.notify_all();
        ShutdownBoth(socket);
        compareStop.store(true);
        compareTaskCv.notify_all();
        hashStop.store(true);
        hashTaskCv.notify_all();
        if (recvThread.joinable()) {
            recvThread.join();
        }
        for (auto& w : compareWorkers) {
            if (w.joinable()) {
                w.join();
            }
        }
        for (auto& w : hashWorkers) {
            if (w.joinable()) {
                w.join();
            }
        }
        throw;
    }

    compareStop.store(true);
    compareTaskCv.notify_all();
    hashStop.store(true);
    hashTaskCv.notify_all();
    for (auto& w : compareWorkers) {
        if (w.joinable()) {
            w.join();
        }
    }
    for (auto& w : hashWorkers) {
        if (w.joinable()) {
            w.join();
        }
    }
    PrintClientCounters(enumerated, compared, unchanged, failed, transferred, deleted, lastEnum, lastCompared, lastUnchanged, lastFailed, lastTransferred, lastDeleted, true);

    if (diagnostics) {
        int64_t mtimeDeltaP50 = 0, mtimeDeltaP95 = 0, mtimeDeltaP99 = 0, mtimeDeltaMax = 0;
        if (!mtimeDeltas.empty()) {
            std::sort(mtimeDeltas.begin(), mtimeDeltas.end());
            const size_t n = mtimeDeltas.size();
            auto percentile = [&](double p) -> int64_t {
                size_t idx = static_cast<size_t>(p * static_cast<double>(n - 1) + 0.5);
                if (idx >= n) {
                    idx = n - 1;
                }
                return mtimeDeltas[idx];
            };
            mtimeDeltaP50 = percentile(0.50);
            mtimeDeltaP95 = percentile(0.95);
            mtimeDeltaP99 = percentile(0.99);
            mtimeDeltaMax = mtimeDeltas.back();
        }
        // AC-A3 requires these exact field names to be directly readable in the log.
        // snake_case aliases are kept for backward compatibility with existing tooling.
        std::cout << "[diag] fallbackCount=" << fallbackCount
                  << " fallbackResolved=" << fallbackResolved
                  << " hashRequestsSent=" << hashRequestsSent
                  << " hashResponsesReceived=" << hashResponsesReceived
                  << " mtime_delta_samples=" << mtimeDeltas.size()
                  << " mtime_delta_p50=" << mtimeDeltaP50
                  << " mtime_delta_p95=" << mtimeDeltaP95
                  << " mtime_delta_p99=" << mtimeDeltaP99
                  << " mtime_delta_max=" << mtimeDeltaMax
                  << " fallback_count=" << fallbackCount
                  << " fallback_resolved=" << fallbackResolved
                  << " hash_req_sent=" << hashRequestsSent
                  << " hash_resp_recv=" << hashResponsesReceived
                  << std::endl;
    }

    std::cout << "Deleting obsoleted files (sync with server)..." << std::endl;
    const RemoveLocalExtrasResult deleteResult = RemoveLocalExtras(options.rootDir, remoteDirs, remoteFiles, selfPath);
    deleted = deleteResult.deletedFiles;
    failed += deleteResult.failedOps;
    compared += deleteResult.failedOps;
    compared += deleted;
    std::cout << "Delete done, " << deleted << " files" << std::endl;
    PrintClientCounters(enumerated, compared, unchanged, failed, transferred, deleted, lastEnum, lastCompared, lastUnchanged, lastFailed, lastTransferred, lastDeleted, true);
    SendFrame(socket, Frame{MsgType::SyncDone, 0, {}});
    recvStop.store(true);
    incomingDataCv.notify_all();
    ShutdownBoth(socket);
    if (recvThread.joinable()) {
        recvThread.join();
    }
    const bool success = (failed == 0);
    std::cout << "Sync completed. changed_files=" << transferred
              << " failed_files=" << failed << std::endl;
    return success ? 0 : 2;
}

}  // namespace fc
