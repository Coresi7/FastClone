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

// Join a relative path (UTF-8, forward slashes) onto the local root directory. Self-contained, does not depend on
// sync_util::JoinRel (which is not in the FastCheck link closure).
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

// Replicates sync_engine_client's probeLocalFile: one syscall to get size+mtime, yielding optional<FileEntry>.
// The mtime unit matches the manifest side (Win=FILETIME ticks, POSIX=Unix ns), for DecideCompare's normalized comparison.
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

    // Files that need a hash (needHash decided in the metadata phase). Those already sent and awaiting a response go into awaiting (matched by relPath).
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

    // Record a file with a decided category into the counters and (per filter/summaryOnly) the per-file listing.
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
        // localSize: present only when the local file exists (Same/Diff/Extra); Missing means local absent -> local empty -> null (FR-24).
        if (local.has_value()) {
            entry.localSize = local->fileSize;
        }
        // remoteSize: present only when the remote exists (Same/Diff/Missing all come from manifest entries, remote.fileSize valid);
        // Extra means remote absent -> leave nullopt -> JSON null (FR-24/AC-31). The remote passed for EXTRA is a temporary
        // construct with fileSize=0 and must never be filled into remoteSize.
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
    // Keep the in-flight HashRequest count <= --checkers (pipeline depth, not lane count, FR-06).
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
                    continue;  // Directories do not participate in file comparison (EXTRA also counts files only).
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
                // Progress hint, ignore.
            } else if (frame.type == MsgType::ManifestEnd) {
                manifestDone = true;
            } else if (frame.type == MsgType::HashResponse) {
                const std::pair<std::string, Hash256> resp = DecodeHashResponse(frame.payload);
                auto it = awaiting.find(resp.first);
                if (it == awaiting.end()) {
                    continue;  // Unknown/duplicate response, ignore.
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
                    // Local file unreadable in the hash phase: abort the comparison, return exit code 3 (boundary condition).
                    localReadFailed = true;
                    errorText = ex.what();
                    break;
                }
                const fc::CompareCategory cat =
                    ClassifyByHash(localReadable, localHash, resp.second);
                record(cat, need.remote, need.local, true);
                pump();
            } else {
                // Check should not receive transfer frames like File*/Delta*/BlockSig*; log a diagnostic and ignore.
                std::cerr << "[check] unexpected frame in Check session type="
                          << static_cast<int>(static_cast<uint8_t>(frame.type)) << std::endl;
            }
        }
    } catch (const std::exception& ex) {
        // Disconnect (recv throws) or send failure: mark partial, exit code 2 (NFR-07/AC-48).
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
        // Full comparison: enumerate local extras (FR-19), then decide exit code 0/1 (FR-14).
        const std::vector<std::string> extras = CollectExtraLocal(targetRoot, manifestPaths);
        for (const std::string& rel : extras) {
            FileEntry extraEntry;
            extraEntry.relativePath = rel;
            const std::optional<FileEntry> local = ProbeLocal(targetRoot, rel);
            // record fills remoteSize from remote.fileSize (not set for EXTRA) and localSize from local.
            record(fc::CompareCategory::Extra, extraEntry, local, false);
        }
        outcome.exit = (counters.diff == 0 && counters.missing == 0 && counters.extraLocal == 0)
                           ? kIdentical
                           : kDiffFound;
    }

    // Wrap-up: best-effort send SyncDone to reuse the server's clean early-exit path (section 8.1). On disconnect the socket is already dead, so ignore failures.
    if (!disconnected) {
        try {
            ch.send(Frame{MsgType::SyncDone, 0, {}});
        } catch (const std::exception&) {
            // Best-effort; a wrap-up send failure does not change the already-decided exit code.
        }
    }

    const auto endTime = std::chrono::steady_clock::now();
    result.durationMs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count());
    outcome.result = std::move(result);
    return outcome;
}

}  // namespace fc::check
