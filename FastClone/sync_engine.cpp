#include "sync_engine.h"

#include "file_index.h"
#include "protocol.h"
#include "win_socket.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <condition_variable>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <queue>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace fs = std::filesystem;

namespace fc {

namespace {

bool IsDebugEnabled() {
    static const bool enabled = []() {
        char value[16] = {};
        const DWORD len = GetEnvironmentVariableA("FASTCLONE_DEBUG", value, static_cast<DWORD>(sizeof(value)));
        if (len == 0) {
            return false;
        }
        std::string v(value, value + std::min<DWORD>(len, static_cast<DWORD>(sizeof(value) - 1)));
        std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return v == "1" || v == "true" || v == "yes" || v == "on";
    }();
    return enabled;
}

struct ServerStream {
    std::ifstream input;
    std::string relativePath;
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
        if (options.chunkAutoTune) {
            tuned.streamLimit = std::clamp<uint32_t>(hw * 2, 8, 24);
        } else {
            const uint32_t chunkKb = tuned.chunkSize / 1024;
            if (chunkKb >= 8192) {
                tuned.streamLimit = 8;
            } else if (chunkKb >= 4096) {
                tuned.streamLimit = 10;
            } else if (chunkKb >= 2048) {
                tuned.streamLimit = 12;
            } else if (chunkKb >= 1024) {
                tuned.streamLimit = 16;
            } else {
                tuned.streamLimit = 24;
            }
        }
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

fs::path JoinRel(const fs::path& root, const std::string& relPath) {
    if (relPath == "." || relPath.empty()) {
        return root;
    }
    return root / fs::path(Utf8ToWide(relPath));
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
    SendSimple(socket, MsgType::Hello, "FC1");

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
    SendSimple(socket, MsgType::Hello, "FC1");
    Frame helloBack = RecvFrame(socket);
    if (helloBack.type != MsgType::Hello) {
        throw std::runtime_error("Server HELLO missing");
    }
    SendSimple(socket, MsgType::Auth, password);
    Frame authResult = RecvFrame(socket);
    if (authResult.type != MsgType::AuthOk) {
        throw std::runtime_error("Server authentication rejected");
    }
}

std::vector<uint8_t> EncodeManifestEntry(const FileEntry& entry) {
    std::vector<uint8_t> payload;
    payload.push_back(entry.isDirectory ? 1 : 0);
    AppendString(payload, entry.relativePath);
    AppendU64(payload, entry.fileSize);
    AppendI64(payload, entry.mtimeNs);
    AppendI64(payload, entry.ctimeNs);
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
    entry.ctimeNs = ReadI64(payload, cursor);
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

LocalState BuildLocalState(const fs::path& root, const std::optional<fs::path>& exclude) {
    LocalState st;
    const std::vector<FileEntry> all = BuildIndex(root, exclude);
    for (const FileEntry& e : all) {
        if (e.relativePath == ".") {
            continue;
        }
        if (e.isDirectory) {
            st.directories.insert(e.relativePath);
        } else {
            st.files.emplace(e.relativePath, e);
        }
    }
    return st;
}

void EnsureParentDir(const fs::path& filePath) {
    const fs::path parent = filePath.parent_path();
    if (!parent.empty()) {
        std::error_code ec;
        fs::create_directories(parent, ec);
    }
}

size_t RemoveLocalExtras(const fs::path& root,
                         const std::unordered_set<std::string>& remoteDirs,
                         const std::unordered_map<std::string, FileEntry>& remoteFiles,
                         const std::optional<fs::path>& exclude) {
    size_t deletedFiles = 0;
    LocalState local = BuildLocalState(root, exclude);
    for (const auto& kv : local.files) {
        if (!remoteFiles.contains(kv.first)) {
            std::error_code ec;
            if (fs::remove(JoinRel(root, kv.first), ec)) {
                ++deletedFiles;
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
        }
    }
    return deletedFiles;
}

std::optional<fs::path> CurrentExePath() {
    wchar_t pathBuf[MAX_PATH];
    const DWORD len = GetModuleFileNameW(nullptr, pathBuf, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return std::nullopt;
    }
    return fs::weakly_canonical(fs::path(pathBuf));
}

void RunSessionServer(const SocketHandle& client, const CliOptions& options) {
    EnsureHandshakeAsServer(client, options.password);
    const std::optional<fs::path> selfPath = CurrentExePath();
    const bool debugEnabled = IsDebugEnabled();
    const TunedTransferOptions tuned = ResolveTransferOptions(options);
    const uint32_t streamLimit = tuned.streamLimit;
    const uint32_t effectiveChunkSize = EffectiveChunkSizeForStreams(tuned.chunkSize, streamLimit);

    std::unordered_map<uint32_t, ServerStream> activeStreams;
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
                    Hash256 hash = ComputeFileSha256(task.absPath);
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
                            size_t fileCount = 0;
                            for (const auto& item : fs::recursive_directory_iterator(options.rootDir, fs::directory_options::skip_permission_denied)) {
                                if (done.load()) {
                                    return;
                                }
                                const fs::path absPath = item.path();
                                if (selfPath.has_value() && fs::exists(*selfPath) && absPath == *selfPath) {
                                    continue;
                                }
                                if (!item.is_directory() && !item.is_regular_file()) {
                                    continue;
                                }
                                FileEntry entry;
                                entry.relativePath = NormalizeRelativePath(fs::relative(absPath, options.rootDir));
                                entry.isDirectory = item.is_directory();
                                if (entry.isDirectory) {
                                    entry.fileSize = 0;
                                } else {
                                    std::error_code sec;
                                    entry.fileSize = static_cast<uint64_t>(fs::file_size(absPath, sec));
                                    if (sec) {
                                        continue;
                                    }
                                    ++fileCount;
                                }
                                std::error_code tec;
                                entry.mtimeNs = ToUnixNs(fs::last_write_time(absPath, tec));
                                if (tec) {
                                    entry.mtimeNs = 0;
                                }
                                entry.ctimeNs = entry.mtimeNs;
                                enqueueManifest(Frame{MsgType::ManifestEntry, 0, EncodeManifestEntry(entry)});
                                if (!entry.isDirectory && fileCount % 2048 == 0) {
                                    std::vector<uint8_t> payload;
                                    AppendU64(payload, static_cast<uint64_t>(fileCount));
                                    enqueueManifest(Frame{MsgType::ManifestProgress, 0, std::move(payload)});
                                }
                            }
                            std::vector<uint8_t> payload;
                            AppendU64(payload, static_cast<uint64_t>(fileCount));
                            enqueueManifest(Frame{MsgType::ManifestProgress, 0, std::move(payload)});
                            enqueueManifest(Frame{MsgType::ManifestEnd, 0, {}});
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
            std::vector<uint8_t> sendBatch;
            sendBatch.reserve(std::max<size_t>(1024 * 1024, static_cast<size_t>(effectiveChunkSize) * streamLimit));
            {
                std::lock_guard<std::mutex> lock(mu);
                size_t highBudget = 256;
                while (!outboundHigh.empty() && highBudget > 0) {
                    AppendEncodedFrame(sendBatch, outboundHigh.front());
                    outboundHigh.pop();
                    didWork = true;
                    --highBudget;
                }
                size_t manifestBudget = outboundHigh.empty() ? 8 : 0;
                while (!outboundManifest.empty() && manifestBudget > 0) {
                    AppendEncodedFrame(sendBatch, outboundManifest.front());
                    outboundManifest.pop();
                    outboundCv.notify_one();
                    didWork = true;
                    --manifestBudget;
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
                            AppendEncodedFrame(sendBatch, Frame{MsgType::FileChunk, it->first, std::move(chunk)});
                            didWork = true;
                        }
                        if (!it->second.input || got == 0) {
                            AppendEncodedFrame(sendBatch, Frame{MsgType::FileEnd, it->first, {}});
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
            if (!sendBatch.empty()) {
                SendAll(client, sendBatch.data(), sendBatch.size());
            }
            if (debugEnabled) {
                const auto now = std::chrono::steady_clock::now();
                if ((now - lastDebugPrint) >= std::chrono::seconds(1)) {
                    size_t highQueued = 0;
                    size_t manifestQueued = 0;
                    size_t activeStreamCount = 0;
                    {
                        std::lock_guard<std::mutex> lock(mu);
                        highQueued = outboundHigh.size();
                        manifestQueued = outboundManifest.size();
                        activeStreamCount = activeStreams.size();
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
                              << std::endl;
                    lastDebugPrint = now;
                }
            }
            if (!didWork) {
                std::unique_lock<std::mutex> lock(mu);
                outboundCv.wait_for(lock, std::chrono::milliseconds(2), [&]() {
                    return done.load() || !outboundHigh.empty() || !outboundManifest.empty() || !activeStreams.empty();
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
                         size_t skipped,
                         size_t transferred,
                         size_t deleted,
                         size_t& lastEnumerated,
                         size_t& lastCompared,
                         size_t& lastSkipped,
                         size_t& lastTransferred,
                         size_t& lastDeleted,
                         bool force = false) {
    using clock = std::chrono::steady_clock;
    static clock::time_point lastPrint = clock::now();
    const auto now = clock::now();
    const bool tickReached = (now - lastPrint) >= std::chrono::seconds(1);
    const bool changedEnough = (enumerated != lastEnumerated) ||
                               (compared != lastCompared) ||
                               (skipped != lastSkipped) ||
                               (transferred != lastTransferred) ||
                               (deleted != lastDeleted);
    if (!force && (!tickReached || !changedEnough)) {
        return;
    }
    lastPrint = now;
    lastEnumerated = enumerated;
    lastCompared = compared;
    lastSkipped = skipped;
    lastTransferred = transferred;
    lastDeleted = deleted;
    std::cout << "\r"
              << "Enumrated: " << enumerated
              << "  Compared: " << compared
              << "  Skipped: " << skipped
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
            SetFileCreateAndModifyTime(JoinRel(rootDir, rel), meta.ctimeNs, meta.mtimeNs);
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
        if (!selfPath->empty() && selfPath->native().find(options.rootDir.native()) != std::wstring::npos) {
            selfPath = fs::weakly_canonical(*selfPath);
        } else {
            selfPath = std::nullopt;
        }
    }

    SocketHandle socket = ConnectTo(options.host, options.port);
    EnsureHandshakeAsClient(socket, options.password);
    if (options.streamAutoTune || options.chunkAutoTune) {
        std::cout << "[auto-tune] streams=" << streamLimit
                  << " chunk-kb=" << (tuned.chunkSize / 1024)
                  << std::endl;
    }

    SendFrame(socket, Frame{MsgType::ManifestRequest, 0, {}});
    std::unordered_map<std::string, FileEntry> remoteFiles;
    std::unordered_set<std::string> remoteDirs;
    std::unordered_map<uint32_t, DownloadState> activeDownloads;
    std::unordered_map<uint32_t, std::string> streamToPath;
    std::queue<std::pair<uint32_t, std::string>> pendingTransfers;
    std::unordered_set<std::string> scheduledTransfers;

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
                    Hash256 hash = ComputeFileSha256(task.absPath);
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
    size_t skipped = 0;
    size_t transferred = 0;
    size_t deleted = 0;
    size_t fallbackCount = 0;
    size_t fallbackResolved = 0;
    size_t hashRequestsSent = 0;
    size_t hashResponsesReceived = 0;
    const size_t maxInFlightHashRequests = std::max<size_t>(256, static_cast<size_t>(streamLimit) * 32);
    size_t lastEnum = 0;
    size_t lastCompared = 0;
    size_t lastSkipped = 0;
    size_t lastTransferred = 0;
    size_t lastDeleted = 0;

    std::mutex incomingMu;
    std::condition_variable incomingCv;
    std::deque<Frame> incomingFrames;
    std::atomic<bool> recvStop = false;
    std::atomic<bool> recvClosed = false;
    std::string recvError;
    std::thread recvThread([&]() {
        try {
            while (!recvStop.load()) {
                Frame f = RecvFrame(socket);
                {
                    std::lock_guard<std::mutex> lock(incomingMu);
                    incomingFrames.push_back(std::move(f));
                }
                incomingCv.notify_one();
            }
        } catch (const std::exception& ex) {
            if (!recvStop.load()) {
                recvError = ex.what();
                recvClosed.store(true);
                incomingCv.notify_all();
            }
        }
    });

    auto scheduleTransfer = [&](const std::string& rel) {
        if (scheduledTransfers.insert(rel).second) {
            pendingTransfers.push({nextStreamId++, rel});
        }
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
        entry.ctimeNs = entry.mtimeNs;
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
            ++skipped;
        } else {
            if (!hashRequested.contains(r.relPath)) {
                hashRequested.insert(r.relPath);
                ++fallbackCount;
                pendingHashRequests.push_back(r.relPath);
            }
        }
        PrintClientCounters(enumerated, compared, skipped, transferred, deleted, lastEnum, lastCompared, lastSkipped, lastTransferred, lastDeleted);
    };

    auto dispatchHashRequests = [&]() {
        std::vector<uint8_t> outboundBatch;
        outboundBatch.reserve(256 * 1024);
        while (!pendingHashRequests.empty() && (hashRequestsSent - hashResponsesReceived) < maxInFlightHashRequests) {
            const std::string rel = pendingHashRequests.front();
            pendingHashRequests.pop_front();
            ++hashRequestsSent;
            AppendEncodedFrame(outboundBatch, Frame{MsgType::HashRequest, 0, EncodeHashRequest(rel)});
            {
                std::lock_guard<std::mutex> lock(hashTaskMu);
                hashTaskQueue.push_back(ClientHashTask{rel, JoinRel(options.rootDir, rel)});
            }
            hashTaskCv.notify_one();
            if (outboundBatch.size() >= (1024 * 1024)) {
                SendAll(socket, outboundBatch.data(), outboundBatch.size());
                outboundBatch.clear();
            }
        }
        if (!outboundBatch.empty()) {
            SendAll(socket, outboundBatch.data(), outboundBatch.size());
        }
    };

    auto tryStartTransfers = [&]() {
        while (!pendingTransfers.empty() && activeDownloads.size() < streamLimit) {
            auto [sid, rel] = pendingTransfers.front();
            pendingTransfers.pop();
            const fs::path abs = JoinRel(options.rootDir, rel);
            EnsureParentDir(abs);
            DownloadState d;
            d.relPath = rel;
            d.output.open(abs, std::ios::binary | std::ios::trunc);
            if (!d.output) {
                ++compared;
                ++skipped;
                PrintClientCounters(enumerated, compared, skipped, transferred, deleted, lastEnum, lastCompared, lastSkipped, lastTransferred, lastDeleted);
                continue;
            }
            d.flushThreshold = downloadFlushThreshold;
            d.writeBuffer.reserve(d.flushThreshold);
            activeDownloads.emplace(sid, std::move(d));
            streamToPath.emplace(sid, rel);
            SendFrame(socket, Frame{MsgType::FileOpen, sid, EncodeFileOpen(rel)});
        }
    };

    auto flushBufferedWrites = [&](DownloadState& d) {
        if (d.writeBuffer.empty()) {
            return;
        }
        d.output.write(reinterpret_cast<const char*>(d.writeBuffer.data()), static_cast<std::streamsize>(d.writeBuffer.size()));
        d.writeBuffer.clear();
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
                SetFileCreateAndModifyTime(JoinRel(options.rootDir, rel), meta.ctimeNs, meta.mtimeNs);
                ++compared;
                ++skipped;
            }
            hashResolved.insert(rel);
            ++fallbackResolved;
            PrintClientCounters(enumerated, compared, skipped, transferred, deleted, lastEnum, lastCompared, lastSkipped, lastTransferred, lastDeleted);
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
            PrintClientCounters(enumerated, compared, skipped, transferred, deleted, lastEnum, lastCompared, lastSkipped, lastTransferred, lastDeleted);
        } else if (frame.type == MsgType::ManifestProgress) {
            size_t cursor = 0;
            const uint64_t serverEnumerated = ReadU64(frame.payload, cursor);
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
            SetFileCreateAndModifyTime(JoinRel(options.rootDir, rel), meta.ctimeNs, meta.mtimeNs);
            activeDownloads.erase(it);
            streamToPath.erase(frame.streamId);
            ++compared;
            ++transferred;
            PrintClientCounters(enumerated, compared, skipped, transferred, deleted, lastEnum, lastCompared, lastSkipped, lastTransferred, lastDeleted);
        } else if (frame.type == MsgType::FileError) {
            auto itPath = streamToPath.find(frame.streamId);
            auto itDl = activeDownloads.find(frame.streamId);
            if (itDl != activeDownloads.end()) {
                itDl->second.writeBuffer.clear();
                itDl->second.output.close();
                activeDownloads.erase(itDl);
            }
            if (itPath != streamToPath.end()) {
                streamToPath.erase(itPath);
            }
            ++compared;
            ++skipped;
            PrintClientCounters(enumerated, compared, skipped, transferred, deleted, lastEnum, lastCompared, lastSkipped, lastTransferred, lastDeleted);
        } else {
            throw std::runtime_error("Unexpected frame in client stream loop");
        }
    };

    try {
        auto lastDebugPrint = std::chrono::steady_clock::now();
        while (true) {
            resolveFallbackIfReady();
            dispatchHashRequests();
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
                    {
                        std::lock_guard<std::mutex> lock(incomingMu);
                        queuedIncomingFrames = incomingFrames.size();
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
                              << " skipped=" << skipped
                              << " transferred=" << transferred
                              << " in_flight_hash=" << hashInflight
                              << " pending_hash_req=" << pendingHashRequests.size()
                              << " pending_hash_local=" << queuedHashTasks
                              << " in_flight_compare=" << compareInflight
                              << " queued_compare_tasks=" << queuedCompareTasks
                              << " ready_compare_results=" << readyCompareResults
                              << " delayed_compare_entries=" << delayedCompareEntries.size()
                              << " queued_incoming_frames=" << queuedIncomingFrames
                              << " pending_transfers=" << pendingTransfers.size()
                              << " active_downloads=" << activeDownloads.size()
                              << " fallback_open=" << (fallbackCount - fallbackResolved)
                              << std::endl;
                    lastDebugPrint = now;
                }
            }

            const bool allHashDone = (fallbackResolved == fallbackCount);
            const bool allCompareDone = (compareResultsHandled.load() == compareTasksIssued.load());
            if (manifestDone && allCompareDone && pendingTransfers.empty() && activeDownloads.empty() && allHashDone) {
                break;
            }
            const bool needNetworkFrame = !manifestDone || !activeDownloads.empty() || (hashResponsesReceived < hashRequestsSent);
            if (!needNetworkFrame) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            std::deque<Frame> readyFrames;
            {
                std::lock_guard<std::mutex> lock(incomingMu);
                const size_t budget = std::min<size_t>(incomingFrames.size(), 512);
                for (size_t i = 0; i < budget; ++i) {
                    readyFrames.push_back(std::move(incomingFrames.front()));
                    incomingFrames.pop_front();
                }
            }
            if (readyFrames.empty()) {
                std::unique_lock<std::mutex> lock(incomingMu);
                incomingCv.wait_for(lock, std::chrono::milliseconds(2), [&]() {
                    return recvClosed.load() || !incomingFrames.empty();
                });
                if (recvClosed.load() && incomingFrames.empty()) {
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
        shutdown(socket.Get(), SD_BOTH);
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
    recvStop.store(true);
    shutdown(socket.Get(), SD_BOTH);
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
    PrintClientCounters(enumerated, compared, skipped, transferred, deleted, lastEnum, lastCompared, lastSkipped, lastTransferred, lastDeleted, true);

    deleted = RemoveLocalExtras(options.rootDir, remoteDirs, remoteFiles, selfPath);
    compared += deleted;
    PrintClientCounters(enumerated, compared, skipped, transferred, deleted, lastEnum, lastCompared, lastSkipped, lastTransferred, lastDeleted, true);
    SendFrame(socket, Frame{MsgType::SyncDone, 0, {}});
    std::cout << "Sync completed. changed_files=" << transferred << std::endl;
    return 0;
}

}  // namespace fc
