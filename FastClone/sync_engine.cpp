#include "sync_engine.h"

#include "file_index.h"
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

bool IsDebugEnabled() {
    static const bool enabled = []() {
        const std::optional<std::string> env = ReadEnvVar("FASTCLONE_DEBUG");
        if (!env.has_value()) {
            return false;
        }
        std::string v = *env;
        std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return v == "1" || v == "true" || v == "yes" || v == "on";
    }();
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

struct HashTask {
    std::string relPath;
    fs::path absPath;
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

CompareAction DecideCompareAction(const std::optional<FileEntry>& localFile, const FileEntry& remoteFile) {
    if (!localFile.has_value()) {
        return CompareAction::TransferNow;
    }
    if (localFile->fileSize != remoteFile.fileSize) {
        return CompareAction::TransferNow;
    }
    if (localFile->mtimeNs == remoteFile.mtimeNs) {
        return CompareAction::Skip;
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

void RunSessionServer(const SocketHandle& client, const CliOptions& options) {
    EnsureHandshakeAsServer(client, options.password);
    const std::optional<fs::path> selfPath = CurrentExePath();
    const bool debugEnabled = IsDebugEnabled();
    const TunedTransferOptions tuned = ResolveTransferOptions(options);
    const uint32_t streamLimit = tuned.streamLimit;
    const uint32_t effectiveChunkSize = EffectiveChunkSizeForStreams(tuned.chunkSize, streamLimit);

    std::unordered_map<uint32_t, ServerStream> activeStreams;
    std::unordered_map<uint32_t, ServerBatchStream> activeBatchStreams;
    std::mutex mu;
    std::condition_variable outboundCv;
    std::mutex hashMu;
    std::condition_variable hashCv;
    std::deque<HashTask> hashTasks;
    std::queue<Frame> outboundHigh;
    std::queue<Frame> outboundManifest;
    const size_t maxQueuedManifestFrames = 512;
    std::atomic<bool> done = false;
    std::atomic<bool> failed = false;
    std::string errorText;
    std::thread manifestThread;
    std::atomic<bool> manifestStarted = false;

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

    const uint32_t hashWorkerCount = std::max<uint32_t>(1, std::thread::hardware_concurrency());
    std::vector<std::thread> hashWorkers;
    hashWorkers.reserve(hashWorkerCount);
    for (uint32_t i = 0; i < hashWorkerCount; ++i) {
        hashWorkers.emplace_back([&]() {
            while (true) {
                HashTask task;
                {
                    std::unique_lock<std::mutex> lock(hashMu);
                    hashCv.wait(lock, [&]() { return done.load() || !hashTasks.empty(); });
                    if (done.load()) {
                        return;
                    }
                    task = std::move(hashTasks.front());
                    hashTasks.pop_front();
                }
                try {
                    Hash256 hash = ComputeFileHash(task.absPath);
                    enqueueHigh(Frame{MsgType::HashResponse, 0, EncodeHashResponse(task.relPath, hash)});
                } catch (...) {
                    Hash256 fallbackHash{};
                    fallbackHash.fill(0xFF);
                    enqueueHigh(Frame{MsgType::HashResponse, 0, EncodeHashResponse(task.relPath, fallbackHash)});
                }
            }
        });
    }

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
                            hashCv.notify_all();
                            errorText = ex.what();
                        }
                    });
                } else if (frame.type == MsgType::HashRequest) {
                    const std::string rel = DecodeHashRequest(frame.payload);
                    const fs::path abs = JoinRel(options.rootDir, rel);
                    {
                        std::lock_guard<std::mutex> lock(hashMu);
                        hashTasks.push_back(HashTask{rel, abs});
                    }
                    hashCv.notify_one();
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
                        activeStreams.emplace(frame.streamId, std::move(st));
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
                                std::error_code tec;
                                record.mtimeNs = ToUnixNs(fs::last_write_time(record.absPath, tec));
                                if (tec) {
                                    record.mtimeNs = 0;
                                }
                                record.ok = true;
                            }
                        }
                        batch.files.push_back(std::move(record));
                    }
                    {
                        std::lock_guard<std::mutex> lock(mu);
                        activeBatchStreams.emplace(frame.streamId, std::move(batch));
                    }
                    outboundCv.notify_one();
                } else if (frame.type == MsgType::SyncDone) {
                    done.store(true);
                    hashCv.notify_all();
                    outboundCv.notify_all();
                } else {
                    throw std::runtime_error("Unknown message in server session");
                }
            }
        } catch (const std::exception& ex) {
            failed.store(true);
            done.store(true);
            hashCv.notify_all();
            outboundCv.notify_all();
            errorText = ex.what();
        }
    });

    try {
        auto lastDebugPrint = std::chrono::steady_clock::now();
        const size_t perStreamBurstBytes = (streamLimit <= 8)
                                               ? std::max<size_t>(2 * 1024 * 1024, static_cast<size_t>(effectiveChunkSize) * 2)
                                               : static_cast<size_t>(effectiveChunkSize);
        while (!done.load()) {
            bool didWork = false;
            std::vector<Frame> sendFrames;
            sendFrames.reserve(std::max<size_t>(256, static_cast<size_t>(streamLimit) * 8));
            {
                std::lock_guard<std::mutex> lock(mu);
                size_t highBudget = 256;
                while (!outboundHigh.empty() && highBudget > 0) {
                    sendFrames.push_back(std::move(outboundHigh.front()));
                    outboundHigh.pop();
                    didWork = true;
                    --highBudget;
                }
                size_t manifestBudget = outboundHigh.empty() ? 8 : 0;
                while (!outboundManifest.empty() && manifestBudget > 0) {
                    sendFrames.push_back(std::move(outboundManifest.front()));
                    outboundManifest.pop();
                    outboundCv.notify_one();
                    didWork = true;
                    --manifestBudget;
                }
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
                    size_t activeStreamCount = 0;
                    size_t activeBatchCount = 0;
                    {
                        std::lock_guard<std::mutex> lock(mu);
                        highQueued = outboundHigh.size();
                        manifestQueued = outboundManifest.size();
                        activeStreamCount = activeStreams.size();
                        activeBatchCount = activeBatchStreams.size();
                    }
                    size_t pendingHashes = 0;
                    {
                        std::lock_guard<std::mutex> lock(hashMu);
                        pendingHashes = hashTasks.size();
                    }
                    std::cerr << "[debug][server] queued_high=" << highQueued
                              << " queued_manifest=" << manifestQueued
                              << " pending_hash_tasks=" << pendingHashes
                              << " active_streams=" << activeStreamCount
                              << " active_batches=" << activeBatchCount
                              << std::endl;
                    lastDebugPrint = now;
                }
            }
            if (!didWork) {
                std::unique_lock<std::mutex> lock(mu);
                outboundCv.wait_for(lock, std::chrono::milliseconds(2), [&]() {
                    return done.load() || !outboundHigh.empty() || !outboundManifest.empty() || !activeStreams.empty() || !activeBatchStreams.empty();
                });
            }
        }
    } catch (const std::exception& ex) {
        failed.store(true);
        done.store(true);
        hashCv.notify_all();
        outboundCv.notify_all();
        errorText = ex.what();
    }

    if (receiver.joinable()) {
        receiver.join();
    }
    if (manifestThread.joinable()) {
        manifestThread.join();
    }
    hashCv.notify_all();
    for (auto& worker : hashWorkers) {
        if (worker.joinable()) {
            worker.join();
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
    std::cout << "\r"
              << "Enumrated: " << enumerated
              << "  Compared: " << compared
              << "  Unchanged: " << unchanged
              << "  Failed: " << failed
              << "  Transfered: " << transferred
              << "  Deleted: " << deleted
              << "      " << std::flush;
    if (force) {
        std::cout << std::endl;
    }
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
    std::cout << "FastClone server root=" << options.rootDir.string() << " port=" << options.port << std::endl;
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
    std::atomic<bool> compareStop = false;
    std::atomic<size_t> compareTasksIssued = 0;
    std::atomic<size_t> compareResultsHandled = 0;

    std::mutex hashTaskMu;
    std::mutex hashResultMu;
    std::condition_variable hashTaskCv;
    std::deque<ClientHashTask> hashTaskQueue;
    std::atomic<bool> hashStop = false;

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
    const size_t maxInFlightHashRequests = std::max<size_t>(256, static_cast<size_t>(streamLimit) * 32);
    constexpr uint8_t kMaxTransferRetries = 3;
    size_t lastEnum = 0;
    size_t lastCompared = 0;
    size_t lastUnchanged = 0;
    size_t lastFailed = 0;
    size_t lastTransferred = 0;
    size_t lastDeleted = 0;
    size_t reservedEntryCapacity = 0;

    auto ensureEntryReserve = [&](size_t expectedEntries) {
        const size_t target = expectedEntries + (expectedEntries / 2) + 1024;
        if (target <= reservedEntryCapacity) {
            return;
        }
        remoteFiles.reserve(target);
        remoteHashes.reserve(target);
        localHashes.reserve(target);
        hashResolved.reserve(target);
        hashRequested.reserve(target);
        localHashFailed.reserve(target);
        scheduledTransfers.reserve(target);
        transferRetryCounts.reserve(target);
        remoteDirs.reserve((target / 4) + 256);
        reservedEntryCapacity = target;
    };

    std::mutex incomingMu;
    std::condition_variable incomingDataCv;
    std::deque<Frame> incomingPriorityFrames;
    std::deque<Frame> incomingManifestFrames;
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
                {
                    std::lock_guard<std::mutex> lock(incomingMu);
                    if (isManifestFrame(f.type)) {
                        incomingManifestFrames.push_back(std::move(f));
                    } else {
                        incomingPriorityFrames.push_back(std::move(f));
                    }
                }
                incomingDataCv.notify_one();
            }
        } catch (const std::exception& ex) {
            if (!recvStop.load()) {
                recvError = ex.what();
                recvClosed.store(true);
                incomingDataCv.notify_all();
            }
        }
    });

    uint64_t smallFileBatchThreshold = 128 * 1024;
    size_t smallBatchMaxFiles = 256;
    size_t smallBatchMaxBytes = 8 * 1024 * 1024;
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
        const size_t backlog = pendingBatchTransfers.size();
        uint64_t threshold = 128 * 1024;
        size_t maxFiles = 256;
        size_t maxBytes = 8 * 1024 * 1024;

        if (backlog > 40000) {
            threshold = 256 * 1024;
            maxFiles = 512;
            maxBytes = 16 * 1024 * 1024;
        } else if (backlog > 12000) {
            threshold = 192 * 1024;
            maxFiles = 384;
            maxBytes = 12 * 1024 * 1024;
        } else if (backlog < 1000) {
            threshold = 96 * 1024;
            maxFiles = 192;
            maxBytes = 6 * 1024 * 1024;
        }

        if (streamLimit <= 8) {
            maxBytes = std::max<size_t>(maxBytes, 16 * 1024 * 1024);
            maxFiles = std::max<size_t>(maxFiles, 384);
        }

        smallFileBatchThreshold = threshold;
        smallBatchMaxFiles = maxFiles;
        smallBatchMaxBytes = maxBytes;
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
        entry.mtimeNs = ToUnixNs(fs::last_write_time(abs, ec));
        if (ec) {
            entry.mtimeNs = 0;
        }
        return entry;
    };

    const uint32_t compareWorkerCount = std::min<uint32_t>(16, std::max<uint32_t>(4, workerCount));
    const size_t maxInFlightCompareTasks = std::max<size_t>(1024, static_cast<size_t>(compareWorkerCount) * 64);
    std::vector<std::thread> compareWorkers;
    compareWorkers.reserve(compareWorkerCount);
    for (uint32_t i = 0; i < compareWorkerCount; ++i) {
        compareWorkers.emplace_back([&]() {
            while (true) {
                CompareTask task;
                {
                    std::unique_lock<std::mutex> lock(compareTaskMu);
                    compareTaskCv.wait(lock, [&]() { return compareStop.load() || !compareTasks.empty(); });
                    if (compareStop.load() && compareTasks.empty()) {
                        return;
                    }
                    task = std::move(compareTasks.front());
                    compareTasks.pop_front();
                }
                CompareResult result;
                result.relPath = task.remote.relativePath;
                result.action = DecideCompareAction(probeLocalFile(task.remote.relativePath), task.remote);
                {
                    std::lock_guard<std::mutex> lock(compareResultMu);
                    compareResults.push_back(std::move(result));
                }
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

    try {
        auto lastDebugPrint = std::chrono::steady_clock::now();
        while (true) {
            resolveFallbackIfReady();
            dispatchHashRequests();
            refreshSmallBatchTuning();
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
                    {
                        std::lock_guard<std::mutex> lock(incomingMu);
                        queuedIncomingPriorityFrames = incomingPriorityFrames.size();
                        queuedIncomingManifestFrames = incomingManifestFrames.size();
                        queuedIncomingFrames = queuedIncomingPriorityFrames + queuedIncomingManifestFrames;
                    }
                    size_t queuedHashTasks = 0;
                    {
                        std::lock_guard<std::mutex> lock(hashTaskMu);
                        queuedHashTasks = hashTaskQueue.size();
                    }
                    const size_t compareInflight = compareTasksIssued.load() - compareResultsHandled.load();
                    const size_t hashInflight = hashRequestsSent - hashResponsesReceived;
                    std::cerr << "[debug][client] enum=" << enumerated
                              << " compared=" << compared
                              << " unchanged=" << unchanged
                              << " failed=" << failed
                              << " transferred=" << transferred
                              << " in_flight_hash=" << hashInflight
                              << " pending_hash_req=" << pendingHashRequests.size()
                              << " pending_hash_local=" << queuedHashTasks
                              << " in_flight_compare=" << compareInflight
                              << " queued_compare_tasks=" << queuedCompareTasks
                              << " ready_compare_results=" << readyCompareResults
                              << " delayed_compare_entries=" << delayedCompareEntries.size()
                              << " queued_incoming_frames=" << queuedIncomingFrames
                              << " queued_incoming_prio=" << queuedIncomingPriorityFrames
                              << " queued_incoming_manifest=" << queuedIncomingManifestFrames
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
            if (!needNetworkFrame) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            std::deque<Frame> readyFrames;
            {
                std::lock_guard<std::mutex> lock(incomingMu);
                constexpr size_t kDrainBudget = 512;
                constexpr size_t kPriorityBudget = 384;
                const size_t prioCount = std::min<size_t>(incomingPriorityFrames.size(), kPriorityBudget);
                for (size_t i = 0; i < prioCount; ++i) {
                    readyFrames.push_back(std::move(incomingPriorityFrames.front()));
                    incomingPriorityFrames.pop_front();
                }
                const size_t remaining = kDrainBudget - readyFrames.size();
                const size_t manifestCount = std::min<size_t>(incomingManifestFrames.size(), remaining);
                for (size_t i = 0; i < manifestCount; ++i) {
                    readyFrames.push_back(std::move(incomingManifestFrames.front()));
                    incomingManifestFrames.pop_front();
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
                continue;
            }
            for (auto& frame : readyFrames) {
                processIncomingFrame(frame);
            }
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
