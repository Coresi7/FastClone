#include "local_probe.h"

// Shared local-file metadata probe - single implementation for FastClone + FastCheck
// (task unify-probe-extra-shared, design §4.2). Platform primitives below are verbatim
// migrations of the two former probe implementations (sync_engine_client.cpp's
// probeLocalFile lambda and check_engine.cpp's ProbeLocal/StrictProbe); the only
// intentional change is divergence-point A's fix: the POSIX branch writes Unix ns via
// fc::ToUnixNs instead of raw file_clock ticks (design §4.5, FR-02).

#include "sync_util.h"  // fc::JoinRel (FR-05: the only path-joining primitive)

#include <cstdint>
#include <system_error>
#include <utility>

#ifdef _WIN32
#include <Windows.h>
#endif

namespace fs = std::filesystem;

namespace fc {

namespace detail {

void EnumerateDirFileEntries(const fs::path& absDir,
                             std::unordered_map<std::string, FileEntry>& out) {
#ifdef _WIN32
    // Verbatim migration of the sync client's probeLocalFile slow path: one
    // FindFirstFile/FindNextFile pass yields every file's size+mtime in a single
    // sequential MFT read. mtime stays in RAW FILETIME ticks (100ns since 1601),
    // matching the server manifest writer (sync_engine_server.cpp FileTimeToTicks) and
    // TryNormalizeMtimeToUnixNs's tick-range handling (design §4.5 / D-08).
    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW((absDir.wstring() + L"\\*").c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) {
        return;  // unreadable dir -> empty set; caller caches it (B-02, no per-file retry)
    }
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) continue;
        FileEntry entry;
        entry.isDirectory = false;
        entry.fileSize = (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) |
                         static_cast<uint64_t>(fd.nFileSizeLow);
        ULARGE_INTEGER mt{};
        mt.LowPart = fd.ftLastWriteTime.dwLowDateTime;
        mt.HighPart = fd.ftLastWriteTime.dwHighDateTime;
        entry.mtimeNs = static_cast<int64_t>(mt.QuadPart);
        std::string fileName = fs::path(fd.cFileName).string();
        out.emplace(std::move(fileName), std::move(entry));
    } while (FindNextFileW(hFind, &fd));
    FindClose(hFind);
#else
    // POSIX: the same unit as the FastCheck probe and the server manifest writer -
    // fc::ToUnixNs. This is divergence-point A's fix (design §4.5): the former sync
    // client slow path wrote raw file_clock ticks (time_since_epoch().count()), which
    // is implementation-defined and, on some standard libraries, falls into
    // TryNormalizeMtimeToUnixNs's FILETIME tick range and gets mis-scaled. Raw
    // file_clock ticks are banned in this TU (FR-02 / AC-05 token gate).
    std::error_code itec;
    fs::directory_iterator it(absDir, fs::directory_options::skip_permission_denied, itec);
    if (itec) {
        return;  // unreadable dir -> empty set; caller caches it (B-02)
    }
    for (const fs::directory_iterator end; it != end; it.increment(itec)) {
        if (itec) {
            itec.clear();
            continue;  // per-entry failure -> skip, not fatal (B-02)
        }
        std::error_code ec;
        if (!it->is_regular_file(ec) || ec) continue;
        const auto lwt = it->last_write_time(ec);
        if (ec) continue;
        const auto sz = it->file_size(ec);
        if (ec) continue;
        FileEntry entry;
        entry.isDirectory = false;
        entry.fileSize = static_cast<uint64_t>(sz);
        entry.mtimeNs = ToUnixNs(lwt);
        out.emplace(it->path().filename().string(), std::move(entry));
    }
#endif
}

}  // namespace detail

// ---- DirProbeCache: verbatim double-checked-lock migration of probeLocalFile ----------

std::optional<FileEntry> DirProbeCache::Probe(const fs::path& root, const std::string& relPath) {
    // Split relPath into directory + filename (relPath uses forward slashes).
    const auto slashPos = relPath.find_last_of('/');
    const std::string dirPart = (slashPos == std::string::npos) ? "." : relPath.substr(0, slashPos);
    const std::string filePart = (slashPos == std::string::npos) ? relPath : relPath.substr(slashPos + 1);

    // Fast path: directory already cached (shared lock, concurrent reads).
    {
        std::shared_lock<std::shared_mutex> lock(mu_);
        auto dirIt = cache_.find(dirPart);
        if (dirIt != cache_.end()) {
            auto fileIt = dirIt->second.find(filePart);
            if (fileIt != dirIt->second.end()) {
                FileEntry entry = fileIt->second;
                entry.relativePath = relPath;
                return entry;
            }
            return std::nullopt;  // directory cached, file not found -> missing
        }
    }

    // Slow path: enumerate the directory once (exclusive lock).
    {
        std::unique_lock<std::shared_mutex> lock(mu_);
        // Double-check after acquiring exclusive lock.
        auto dirIt = cache_.find(dirPart);
        if (dirIt != cache_.end()) {
            auto fileIt = dirIt->second.find(filePart);
            if (fileIt != dirIt->second.end()) {
                FileEntry entry = fileIt->second;
                entry.relativePath = relPath;
                return entry;
            }
            return std::nullopt;
        }

        slowPathCount_.fetch_add(1, std::memory_order_relaxed);  // once per DIRECTORY (AC-10)
        const fs::path absDir = (dirPart == ".") ? root : JoinRel(root, dirPart);
        std::unordered_map<std::string, FileEntry> files;
        detail::EnumerateDirFileEntries(absDir, files);
        auto [insertedIt, inserted] = cache_.emplace(dirPart, std::move(files));
        (void)inserted;
        auto fileIt = insertedIt->second.find(filePart);
        if (fileIt != insertedIt->second.end()) {
            FileEntry entry = fileIt->second;
            entry.relativePath = relPath;
            return entry;
        }
        return std::nullopt;
    }
}

// ---- Unified entries -------------------------------------------------------------------

std::optional<FileEntry> ProbeLocalFile(const fs::path& root, const std::string& relPath,
                                        DirProbeCache* ctx) {
    if (ctx != nullptr) {
        return ctx->Probe(root, relPath);  // batch form (FastClone): cached dir probe
    }
    // On-demand form (FastCheck): verbatim migration of check_engine.cpp's ProbeLocal -
    // one metadata syscall, nothing retained across calls (I-6).
    const fs::path abs = JoinRel(root, relPath);
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (GetFileAttributesExW(abs.wstring().c_str(), GetFileExInfoStandard, &data) == 0) {
        return std::nullopt;
    }
    if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return std::nullopt;
    }
    FileEntry entry;
    entry.relativePath = relPath;
    entry.isDirectory = false;
    entry.fileSize = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) |
                     static_cast<uint64_t>(data.nFileSizeLow);
    ULARGE_INTEGER mt{};
    mt.LowPart = data.ftLastWriteTime.dwLowDateTime;
    mt.HighPart = data.ftLastWriteTime.dwHighDateTime;
    entry.mtimeNs = static_cast<int64_t>(mt.QuadPart);  // raw FILETIME ticks (§4.5 / D-08)
    return entry;
#else
    std::error_code ec;
    if (!fs::exists(abs, ec) || ec) {
        return std::nullopt;
    }
    if (!fs::is_regular_file(abs, ec) || ec) {
        return std::nullopt;  // missing / directory / symlink->dir -> nullopt (B-03)
    }
    FileEntry entry;
    entry.relativePath = relPath;
    entry.isDirectory = false;
    entry.fileSize = static_cast<uint64_t>(fs::file_size(abs, ec));
    if (ec) {
        return std::nullopt;
    }
    entry.mtimeNs = ToUnixNs(fs::last_write_time(abs, ec));  // divergence-point A fix
    if (ec) {
        return std::nullopt;
    }
    return entry;
#endif
}

std::optional<FileEntry> ProbeLocalFileSizeOnly(const fs::path& root, const std::string& relPath) {
    // On-demand form: verbatim migration of check_engine.cpp's StrictProbe (design D-06) -
    // a single directory_entry cached-metadata query yields both the type and the size
    // (preserving the redundant-syscall-elim single-query semantics, dev-map RS-01).
    // mtime is deliberately 0 because Strict never consults it.
    const fs::path abs = JoinRel(root, relPath);
    std::error_code ec;
    const fs::directory_entry entry(abs, ec);
    if (ec) {
        return std::nullopt;
    }
    std::error_code tec;
    if (!entry.is_regular_file(tec) || tec) {
        return std::nullopt;  // not found / directory / special -> Missing
    }
    std::error_code sec;
    const uint64_t localSize = static_cast<uint64_t>(entry.file_size(sec));
    if (sec) {
        return std::nullopt;  // size unreadable -> Missing
    }
    FileEntry fe;
    fe.relativePath = relPath;
    fe.isDirectory = false;
    fe.fileSize = localSize;
    fe.mtimeNs = 0;  // Strict ignores mtime
    return fe;
}

}  // namespace fc
