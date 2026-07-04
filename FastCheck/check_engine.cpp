#include "check_engine.h"

#include "compare_phase.h"
#include "file_index.h"
#include "protocol_codec.h"

#ifdef _WIN32
#include <Windows.h>
#endif

#include <chrono>
#include <deque>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace fs = std::filesystem;

namespace fc::check {

namespace {

CompareMode ToCompareMode(Mode mode) {
    switch (mode) {
        case Mode::Strict:
            return CompareMode::Strict;
        case Mode::SizeOnly:
            return CompareMode::SizeOnly;
        case Mode::Fast:
        default:
            return CompareMode::Fast;
    }
}

#ifdef _WIN32
std::wstring Utf8ToWideLocal(const std::string& value) {
    if (value.empty()) {
        return {};
    }
    const int len = MultiByteToWideChar(CP_UTF8, 0, value.c_str(),
                                        static_cast<int>(value.size()), nullptr, 0);
    if (len <= 0) {
        return {};
    }
    std::wstring output(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()),
                        output.data(), len);
    return output;
}
#endif

// 相对路径（UTF-8、正斜杠）拼到本地根目录。自包含，不依赖 sync_util::JoinRel（不在 FastCheck
// link 闭包内）。
fs::path JoinLocal(const fs::path& root, const std::string& rel) {
#ifdef _WIN32
    std::wstring full = root.wstring();
    if (!full.empty() && full.back() != L'\\' && full.back() != L'/') {
        full.push_back(L'\\');
    }
    std::wstring relW = Utf8ToWideLocal(rel);
    for (wchar_t& c : relW) {
        if (c == L'/') {
            c = L'\\';
        }
    }
    full += relW;
    return fs::path(full);
#else
    return root / fs::path(rel);
#endif
}

// 复刻 sync_engine_client 的 probeLocalFile：一次 syscall 取 size+mtime，得 optional<FileEntry>。
// mtime 单位与 manifest 侧一致（Win=FILETIME ticks，POSIX=Unix ns），供 DecideCompare 归一比对。
std::optional<FileEntry> ProbeLocal(const fs::path& root, const std::string& rel) {
    const fs::path abs = JoinLocal(root, rel);
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (GetFileAttributesExW(abs.wstring().c_str(), GetFileExInfoStandard, &data) == 0) {
        return std::nullopt;
    }
    if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return std::nullopt;
    }
    FileEntry entry;
    entry.relativePath = rel;
    entry.isDirectory = false;
    entry.fileSize = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) |
                     static_cast<uint64_t>(data.nFileSizeLow);
    ULARGE_INTEGER mt{};
    mt.LowPart = data.ftLastWriteTime.dwLowDateTime;
    mt.HighPart = data.ftLastWriteTime.dwHighDateTime;
    entry.mtimeNs = static_cast<int64_t>(mt.QuadPart);
    return entry;
#else
    std::error_code ec;
    if (!fs::exists(abs, ec) || ec) {
        return std::nullopt;
    }
    if (!fs::is_regular_file(abs, ec) || ec) {
        return std::nullopt;
    }
    FileEntry entry;
    entry.relativePath = rel;
    entry.isDirectory = false;
    entry.fileSize = static_cast<uint64_t>(fs::file_size(abs, ec));
    if (ec) {
        return std::nullopt;
    }
    entry.mtimeNs = ToUnixNs(fs::last_write_time(abs, ec));
    if (ec) {
        return std::nullopt;
    }
    return entry;
#endif
}

}  // namespace

EngineOutcome RunCheck(const CheckOptions& o, FrameChannel& ch,
                       const std::atomic<bool>& interrupted) {
    const auto startTime = std::chrono::steady_clock::now();
    const CompareMode mode = ToCompareMode(o.mode);
    const fs::path targetRoot(std::filesystem::path(o.target));

    CheckResult result;
    result.mode = o.mode;
    fc::CompareCounters& counters = result.counters;

    std::unordered_set<std::string> manifestPaths;

    // 需要 hash 的文件（元数据阶段判 needHash）。已发送等待响应的记入 awaiting（按 relPath 匹配）。
    struct HashNeed {
        FileEntry remote;
        std::optional<FileEntry> local;
    };
    std::deque<HashNeed> hashQueue;
    std::unordered_map<std::string, HashNeed> awaiting;
    size_t inFlight = 0;

    bool manifestDone = false;
    bool userInterrupted = false;
    bool disconnected = false;
    bool localReadFailed = false;
    std::string errorText;

    // 记录一条已定类别的文件到计数与（按 filter/summaryOnly）逐文件清单。
    auto record = [&](fc::CompareCategory category, const FileEntry& remote,
                      const std::optional<FileEntry>& local, bool hashCompared) {
        switch (category) {
            case fc::CompareCategory::Same:
                ++counters.same;
                break;
            case fc::CompareCategory::Diff:
                ++counters.diff;
                break;
            case fc::CompareCategory::Missing:
                ++counters.missing;
                break;
            case fc::CompareCategory::Extra:
                ++counters.extraLocal;
                break;
        }
        if (o.summaryOnly) {
            return;
        }
        const bool keep = (category == fc::CompareCategory::Diff && o.filter.diff) ||
                          (category == fc::CompareCategory::Missing && o.filter.missing) ||
                          (category == fc::CompareCategory::Extra && o.filter.extra) ||
                          (category == fc::CompareCategory::Same && o.filter.same);
        if (!keep) {
            return;
        }
        DiffEntry entry;
        entry.type = category;
        entry.path = remote.relativePath;
        entry.hashCompared = hashCompared;
        // localSize：本地存在才有（Same/Diff/Extra）；Missing 本地缺失 -> local 为空 -> null（FR-24）。
        if (local.has_value()) {
            entry.localSize = local->fileSize;
        }
        // remoteSize：远端存在才有（Same/Diff/Missing 均来自 manifest 条目，remote.fileSize 有效）；
        // Extra 远端不存在 -> 留 nullopt -> JSON null（FR-24/AC-31）。EXTRA 传入的 remote 是临时构造、
        // fileSize=0，绝不能填进 remoteSize。
        if (category != fc::CompareCategory::Extra) {
            entry.remoteSize = remote.fileSize;
        }
        result.entries.push_back(std::move(entry));
    };

    auto sendHashRequest = [&](const HashNeed& need) {
        ch.send(Frame{MsgType::HashRequest, 0, EncodeHashRequest(need.remote.relativePath)});
        awaiting.emplace(need.remote.relativePath, need);
        ++inFlight;
    };
    // 维持在飞 HashRequest 数 <= --checkers（流水线深度，非 lane 数，FR-06）。
    auto pump = [&]() {
        while (inFlight < o.checkers && !hashQueue.empty()) {
            const HashNeed need = hashQueue.front();
            hashQueue.pop_front();
            sendHashRequest(need);
        }
    };

    try {
        ch.send(Frame{MsgType::ManifestRequest, 0, {}});
        while (!(manifestDone && hashQueue.empty() && inFlight == 0)) {
            if (interrupted.load()) {
                userInterrupted = true;
                break;
            }
            const Frame frame = ch.recv();
            if (frame.type == MsgType::ManifestEntry) {
                FileEntry remote = DecodeManifestEntry(frame.payload);
                if (remote.isDirectory) {
                    continue;  // 目录不参与文件比对（EXTRA 也只统计文件）。
                }
                ++counters.enumerated;
                manifestPaths.insert(remote.relativePath);
                std::optional<FileEntry> local = ProbeLocal(targetRoot, remote.relativePath);
                const CompareOutcome out = DecideCompare(mode, local, remote);
                if (out.needHash) {
                    hashQueue.push_back(HashNeed{remote, local});
                    pump();
                } else {
                    record(out.category, remote, local, false);
                }
            } else if (frame.type == MsgType::ManifestProgress) {
                // 进度提示，忽略。
            } else if (frame.type == MsgType::ManifestEnd) {
                manifestDone = true;
            } else if (frame.type == MsgType::HashResponse) {
                const std::pair<std::string, Hash256> resp = DecodeHashResponse(frame.payload);
                auto it = awaiting.find(resp.first);
                if (it == awaiting.end()) {
                    continue;  // 未知/重复响应，忽略。
                }
                const HashNeed need = it->second;
                awaiting.erase(it);
                --inFlight;
                Hash256 localHash{};
                bool localReadable = false;
                try {
                    localHash = ComputeFileHash(JoinLocal(targetRoot, need.remote.relativePath));
                    localReadable = true;
                } catch (const std::exception& ex) {
                    // 本地文件在 hash 阶段不可读：终止比对，返回退出码 3（边界条件）。
                    localReadFailed = true;
                    errorText = ex.what();
                    break;
                }
                const fc::CompareCategory cat =
                    ClassifyByHash(localReadable, localHash, resp.second);
                record(cat, need.remote, need.local, true);
                pump();
            } else {
                // Check 不应收到 File*/Delta*/BlockSig* 等传输帧；记诊断并忽略。
                std::cerr << "[check] unexpected frame in Check session type="
                          << static_cast<int>(static_cast<uint8_t>(frame.type)) << std::endl;
            }
        }
    } catch (const std::exception& ex) {
        // 断连（recv 抛异常）或发送失败：标 partial，退出码 2（NFR-07/AC-48）。
        disconnected = true;
        errorText = ex.what();
    }

    EngineOutcome outcome;
    if (userInterrupted) {
        result.partial = true;
        outcome.exit = kInterrupted;
    } else if (disconnected) {
        result.partial = true;
        outcome.exit = kConnFailed;
        std::cerr << "error: connection lost during check: "
                  << (errorText.empty() ? "server disconnected" : errorText) << std::endl;
    } else if (localReadFailed) {
        result.partial = true;
        outcome.exit = kLocalPrecondFailed;
        std::cerr << "error: local file read failed during check: " << errorText << std::endl;
    } else {
        // 完整比对：枚举本地多余项（FR-19），再定退出码 0/1（FR-14）。
        const std::vector<std::string> extras = CollectExtraLocal(targetRoot, manifestPaths);
        for (const std::string& rel : extras) {
            FileEntry extraEntry;
            extraEntry.relativePath = rel;
            const std::optional<FileEntry> local = ProbeLocal(targetRoot, rel);
            // record 用 remote.fileSize 填 remoteSize（EXTRA 不设），local 填 localSize。
            record(fc::CompareCategory::Extra, extraEntry, local, false);
        }
        outcome.exit = (counters.diff == 0 && counters.missing == 0 && counters.extraLocal == 0)
                           ? kIdentical
                           : kDiffFound;
    }

    // 收尾：尽力发 SyncDone 复用服务端干净早退路径（§8.1）。断连时套接字已死，忽略失败。
    if (!disconnected) {
        try {
            ch.send(Frame{MsgType::SyncDone, 0, {}});
        } catch (const std::exception&) {
            // 尽力而为，收尾发送失败不改变已定退出码。
        }
    }

    const auto endTime = std::chrono::steady_clock::now();
    result.durationMs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count());
    outcome.result = std::move(result);
    return outcome;
}

}  // namespace fc::check
