#include "file_index.h"
#include "disk_io_driver.h"
#include "path_utils.h"
#include "sync_util.h"

#ifdef _WIN32
#include <Windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#include <xxhash.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <thread>

namespace fs = std::filesystem;

namespace fc {

namespace {

#ifdef _WIN32
int64_t ToNsFromFileTime(FILETIME ft) {
    ULARGE_INTEGER v{};
    v.LowPart = ft.dwLowDateTime;
    v.HighPart = ft.dwHighDateTime;
    // Keep raw Windows FILETIME ticks (100ns since 1601) for lossless round-trip.
    return static_cast<int64_t>(v.QuadPart);
}

FILETIME ToFileTimeFromNs(int64_t unixNs) {
    constexpr int64_t kLikelyUnixNsThreshold = 500000000000000000LL;
    constexpr int64_t kWindowsEpochDiff100ns = 116444736000000000LL;
    int64_t ticks = unixNs;
    if (unixNs > kLikelyUnixNsThreshold) {
        // Compatibility: convert legacy Unix-ns values to FILETIME ticks.
        ticks = (unixNs / 100LL) + kWindowsEpochDiff100ns;
    }
    ULARGE_INTEGER v{};
    v.QuadPart = static_cast<ULONGLONG>(ticks < 0 ? 0 : ticks);
    FILETIME ft{};
    ft.dwLowDateTime = v.LowPart;
    ft.dwHighDateTime = v.HighPart;
    return ft;
}
#endif

}  // namespace

// S-02 (FR-16 / NFR-09 / M8 / R7 / C3): normalize a manifest mtime to Unix ns before handing it to a
// POSIX filesystem. The threshold 5e17 sits BETWEEN the two encodings, so the direction matters:
//   - Unix ns for any date after ~1985 is ~1.7e18 and ABOVE 5e17  -> pass through unchanged.
//   - Windows FILETIME ticks (100ns since 1601) for any modern date is ~1.3e17 and BELOW 5e17, but
//     at/above the Windows-Unix epoch delta 1.16e17 -> convert: (raw - 1.16e17) * 100.
// This mirrors the authoritative compare_phase.cpp::TryNormalizeMtimeToUnixNs and the Windows-side
// ToFileTimeFromNs (which treats >5e17 as "Unix ns, convert to ticks"). 0 and small values (< the
// epoch delta, including the 0 "unknown" sentinel) pass through untouched; negative clamps to 0.
// Pure int64 arithmetic, NO <windows.h> dependency; compiled on all platforms so it is unit-testable
// and value-for-value identical to WriteSmallFileFastPath's POSIX branch and the backend
// ToTimespecFromNs (SetFileModifyTime / driver write / small-file fast path share one mtime, D-14-A/C3).
int64_t NormalizeManifestMtimeToUnixNs(int64_t modifyNs) {
    constexpr int64_t kLikelyUnixNsThreshold = 500000000000000000LL;      // 5e17
    constexpr int64_t kWindowsEpochDiff100ns = 116444736000000000LL;      // 1.16e17
    if (modifyNs > kLikelyUnixNsThreshold) {
        return modifyNs;  // genuine Unix ns (POSIX peer), pass through
    }
    if (modifyNs < kWindowsEpochDiff100ns) {
        return modifyNs < 0 ? 0 : modifyNs;  // 0 sentinel / pre-1970 small values: pass through
    }
    // [1.16e17, 5e17]: Windows FILETIME ticks -> Unix ns.
    const int64_t unixNs = (modifyNs - kWindowsEpochDiff100ns) * 100LL;
    return unixNs < 0 ? 0 : unixNs;
}

namespace {

// Streaming read chunk / read-ahead window for ComputeFileHashViaDriver. Values match the server's
// signature/hash single-pass constants (kServerSigChunkBytes / kServerReadAhead in
// sync_engine_server.cpp): 1 MiB chunk, 4-deep read-ahead. XXH3 streaming is chunk-size independent
// so the resulting Hash256 is identical to ComputeFileHash's inline read (fastcheck-parallel-hash
// NFR-02/AC-14).
constexpr uint32_t kHashChunkBytes = 1u << 20;
constexpr uint32_t kHashReadAhead = 4;

// Per-phase timing accumulators for ComputeFileHashViaDriver (diagnostics only, relaxed atomics).
// Fetch-add on each call; negligible cost. Read via GetHashPhaseTimings() (FastCheck progress line).
std::atomic<uint64_t> g_hashCount{0};
std::atomic<uint64_t> g_hashTotalUs{0};
std::atomic<uint64_t> g_hashFileSizeUs{0};
std::atomic<uint64_t> g_hashOpenUs{0};
std::atomic<uint64_t> g_hashReadUs{0};
std::atomic<uint64_t> g_hashXxhUs{0};
std::atomic<uint64_t> g_hashCloseUs{0};
constexpr uint64_t kSmallFileDirectThreshold = 256u * 1024u;

fs::file_time_type::duration FileToSystemEpochDelta() {
    using namespace std::chrono;
    const auto fileNow = fs::file_time_type::clock::now().time_since_epoch();
    const auto sysNow = time_point_cast<fs::file_time_type::duration>(system_clock::now()).time_since_epoch();
    return sysNow - fileNow;
}

}  // namespace

std::string NormalizeRelativePath(const fs::path& relativePath) {
    const auto u8 = relativePath.generic_u8string();
    std::string p(u8.begin(), u8.end());
    if (!p.empty() && p[0] == '.') {
        if (p.size() == 1) {
            return {};
        }
        if (p.size() > 1 && p[1] == '/') {
            return p.substr(2);
        }
    }
    return p;
}

int64_t ToUnixNs(const fs::file_time_type& value) {
    using namespace std::chrono;
    static const fs::file_time_type::duration kFileToSystemDelta = FileToSystemEpochDelta();
    const auto sysDur = value.time_since_epoch() + kFileToSystemDelta;
    return duration_cast<nanoseconds>(sysDur).count();
}

fs::file_time_type FromUnixNs(int64_t valueNs) {
    using namespace std::chrono;
    static const fs::file_time_type::duration kFileToSystemDelta = FileToSystemEpochDelta();
    const auto sysDur = duration_cast<fs::file_time_type::duration>(nanoseconds(valueNs));
    return fs::file_time_type(sysDur - kFileToSystemDelta);
}

int64_t ReadFileMtimeCanonical(const fs::path& path) {
#ifdef _WIN32
    // FILE_FLAG_BACKUP_SEMANTICS lets a single open path serve both regular files
    // and directories, returning FILETIME ticks consistent with the manifest writer.
    HANDLE handle = CreateFileW(path.wstring().c_str(), GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING,
                                FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle != INVALID_HANDLE_VALUE) {
        FILETIME accessFt{}, writeFt{};
        const BOOL ok = GetFileTime(handle, nullptr, &accessFt, &writeFt);
        CloseHandle(handle);
        if (ok != 0) {
            return ToNsFromFileTime(writeFt);
        }
    }
    // Fallback keeps the previous best-effort behavior; individual files that fall
    // back may diverge in unit and merely re-enter FallbackHash (correct, slower).
    std::error_code ec;
    const int64_t ns = ToUnixNs(fs::last_write_time(path, ec));
    return ec ? 0 : ns;
#else
    std::error_code ec;
    const int64_t ns = ToUnixNs(fs::last_write_time(path, ec));
    return ec ? 0 : ns;
#endif
}

std::vector<FileEntry> BuildIndex(const fs::path& root, const std::optional<fs::path>& excludeAbsPath) {
    std::vector<FileEntry> output;
    const fs::path canonicalRoot = fs::weakly_canonical(root);
    const std::optional<fs::path> canonicalExclude = excludeAbsPath.has_value() ? std::optional<fs::path>(fs::weakly_canonical(*excludeAbsPath))
                                                                                 : std::nullopt;
    if (!fs::exists(canonicalRoot)) {
        throw std::runtime_error("Root directory does not exist");
    }
    output.push_back(FileEntry{".", true, 0, 0});

    struct Candidate {
        fs::path absPath;
        bool isDirectory = false;
        bool isRegular = false;
        // Change 3 (fastcheck-perf-tune, FR-15): file size captured during directory iteration and
        // reused by the worker, so the worker no longer re-stats the file with fs::file_size.
        uint64_t fileSize = 0;
    };
    std::vector<Candidate> candidates;
    for (const auto& item : fs::recursive_directory_iterator(canonicalRoot, fs::directory_options::skip_permission_denied)) {
        const fs::path absPath = item.path();
        if (canonicalExclude.has_value() && fs::exists(*canonicalExclude) && absPath == *canonicalExclude) {
            continue;
        }
        const bool isDir = item.is_directory();
        if (canonicalExclude.has_value() && isDir && IsPathUnderRoot(absPath, *canonicalExclude)) {
            continue;
        }
        const bool isRegular = item.is_regular_file();
        if (!isDir && !isRegular) {
            continue;
        }
        uint64_t fileSize = 0;
        if (isRegular) {
#ifdef _WIN32
            // MSVC directory_entry caches size from the enumeration WIN32_FIND_DATA (no extra
            // syscall), so capture it here and reuse in the worker. On failure skip the candidate
            // -- byte-for-byte the same user-visible result as the former worker-stage
            // fs::file_size failure -> continue.
            std::error_code sizeEc;
            fileSize = static_cast<uint64_t>(item.file_size(sizeEc));
            if (sizeEc) {
                continue;
            }
#else
            // POSIX readdir/getdents does not return file size, so item.file_size would issue a
            // stat in this serial iteration loop. Defer the stat to the parallel worker (below) to
            // keep enumeration cheap; fileSize stays 0 as a not-captured sentinel here.
#endif
        }
        candidates.push_back(Candidate{absPath, isDir, isRegular, fileSize});
    }

    std::mutex pushMu;
    std::atomic<size_t> nextIdx = 0;
    const uint32_t workerCount = std::max<uint32_t>(1, std::thread::hardware_concurrency());
    std::vector<std::thread> workers;
    workers.reserve(workerCount);
    for (uint32_t i = 0; i < workerCount; ++i) {
        workers.emplace_back([&]() {
            while (true) {
                const size_t idx = nextIdx.fetch_add(1);
                if (idx >= candidates.size()) {
                    break;
                }
                const Candidate& c = candidates[idx];
                FileEntry entry;
                entry.relativePath = NormalizeRelativePath(fs::relative(c.absPath, canonicalRoot));
                entry.isDirectory = c.isDirectory;

                if (c.isDirectory) {
                    entry.fileSize = 0;
                    entry.mtimeNs = ReadFileMtimeCanonical(c.absPath);
                } else if (c.isRegular) {
#ifdef _WIN32
                    // Change 3 (FR-16): reuse the size captured at iteration time (no second
                    // fs::file_size on c.absPath). mtime path is unchanged (FR-18): still canonical.
                    entry.fileSize = c.fileSize;
#else
                    // POSIX: size was not captured at iteration (readdir gives no size); stat here
                    // in the worker (parallel). Same failure contract as the pre-Change-3 path.
                    std::error_code sec;
                    entry.fileSize = static_cast<uint64_t>(fs::file_size(c.absPath, sec));
                    if (sec) {
                        continue;
                    }
#endif
                    entry.mtimeNs = ReadFileMtimeCanonical(c.absPath);
                } else {
                    continue;
                }
                std::lock_guard<std::mutex> lock(pushMu);
                output.push_back(std::move(entry));
            }
        });
    }
    for (auto& worker : workers) {
        if (!worker.joinable()) {
            continue;
        }
        if (worker.get_id() == std::this_thread::get_id()) {
            // SELF-JOIN: unreachable by design; if it fires, thread ownership is already
            // corrupted. Fail fast -- detaching would not be safe recovery (the worker
            // body still uses [&]-captured stack state). See JoinDiag in sync_engine.cpp.
            std::cerr << "[deadlock-diag] FATAL SELF-JOIN at site=buildfileindex thread_id="
                      << worker.get_id() << " -- aborting (state not trustworthy)" << std::endl;
            std::cerr.flush();
            std::abort();
        }

        try {
            worker.join();
        } catch (const std::system_error& e) {
            std::cerr << "[deadlock-diag] join FAILED at site=buildfileindex code="
                      << e.code().value() << " msg=\"" << e.code().message() << "\""
                      << " caller_thread=" << std::this_thread::get_id()
                      << " target_thread=" << worker.get_id() << std::endl;
            throw;
        }
    }

    std::sort(output.begin(), output.end(), [](const FileEntry& a, const FileEntry& b) {
        if (a.isDirectory != b.isDirectory) {
            return a.isDirectory;
        }
        return a.relativePath < b.relativePath;
    });
    return output;
}

Hash256 ComputeFileHash(const fs::path& path) {
    XXH3_state_t* state = XXH3_createState();
    if (state == nullptr) {
        throw std::runtime_error("XXH3_createState failed");
    }
    if (XXH3_128bits_reset(state) == XXH_ERROR) {
        XXH3_freeState(state);
        throw std::runtime_error("XXH3_128bits_reset failed");
    }
#ifdef _WIN32
    HANDLE handle = CreateFileW(path.wstring().c_str(),
                                GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr,
                                OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                                nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        throw std::runtime_error("Open file failed for hash");
    }

    thread_local std::vector<uint8_t> readBuf;
    if (readBuf.empty()) {
        readBuf.resize(256 * 1024);
    }

    DWORD bytesRead = 0;
    while (ReadFile(handle, readBuf.data(), static_cast<DWORD>(readBuf.size()), &bytesRead, nullptr) != 0) {
        if (bytesRead == 0) {
            break;
        }
        if (XXH3_128bits_update(state, readBuf.data(), static_cast<size_t>(bytesRead)) == XXH_ERROR) {
            CloseHandle(handle);
            XXH3_freeState(state);
            throw std::runtime_error("XXH3_128bits_update failed");
        }
    }

    const DWORD readErr = GetLastError();
    CloseHandle(handle);
    if (readErr != ERROR_SUCCESS && readErr != ERROR_HANDLE_EOF) {
        XXH3_freeState(state);
        throw std::runtime_error("Read file failed for hash");
    }
#else
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        XXH3_freeState(state);
        throw std::runtime_error("Open file failed for hash");
    }
    thread_local std::vector<uint8_t> readBuf;
    if (readBuf.empty()) {
        readBuf.resize(256 * 1024);
    }
    while (input) {
        input.read(reinterpret_cast<char*>(readBuf.data()), static_cast<std::streamsize>(readBuf.size()));
        const std::streamsize got = input.gcount();
        if (got <= 0) {
            break;
        }
        if (XXH3_128bits_update(state, readBuf.data(), static_cast<size_t>(got)) == XXH_ERROR) {
            XXH3_freeState(state);
            throw std::runtime_error("XXH3_128bits_update failed");
        }
    }
    if (!input.eof() && input.fail()) {
        XXH3_freeState(state);
        throw std::runtime_error("Read file failed for hash");
    }
#endif

    const XXH128_hash_t digest = XXH3_128bits_digest(state);
    XXH3_freeState(state);
    Hash256 output{};
    std::memcpy(output.data(), &digest, output.size());
    return output;
}

Hash256 ComputeHashFromSource(const std::function<size_t(uint8_t*, size_t)>& source) {
    XXH3_state_t* state = XXH3_createState();
    if (state == nullptr) {
        throw std::runtime_error("XXH3_createState failed");
    }
    if (XXH3_128bits_reset(state) == XXH_ERROR) {
        XXH3_freeState(state);
        throw std::runtime_error("XXH3_128bits_reset failed");
    }
    // Same 256 KiB pull granularity as ComputeFileHash; XXH3 streaming is chunk-size independent so
    // the digest is identical regardless of how the driver delivers the bytes.
    thread_local std::vector<uint8_t> readBuf;
    if (readBuf.size() != 256 * 1024) {
        readBuf.resize(256 * 1024);
    }
    for (;;) {
        const size_t got = source(readBuf.data(), readBuf.size());
        if (got == 0) {
            break;
        }
        if (XXH3_128bits_update(state, readBuf.data(), got) == XXH_ERROR) {
            XXH3_freeState(state);
            throw std::runtime_error("XXH3_128bits_update failed");
        }
    }
    const XXH128_hash_t digest = XXH3_128bits_digest(state);
    XXH3_freeState(state);
    Hash256 output{};
    std::memcpy(output.data(), &digest, output.size());
    return output;
}

Hash256 ComputeFileHashViaDriver(fc::io::DiskIoDriver& driver, const fs::path& path,
                                 std::optional<uint64_t> knownSize) {
    // Shared hash read path for FastCheck + server hash-miss:
    //  - <=256 KiB: one driver read request + ComputeBufferHash over real bytes
    //  - >256 KiB : existing SequentialReader streaming path
    const auto fnStart = std::chrono::steady_clock::now();
    g_hashCount.fetch_add(1, std::memory_order_relaxed);

    // FR-19: when the caller already probed the size upstream (FastCheck compare pipeline's
    // ProbeLocal), skip the redundant fs::file_size stat here. nullopt = caller has no size -> fall
    // back to the stat (server hash-miss path, tests). A genuine 0-byte file arrives as knownSize==0
    // and is handled by the early-return below exactly as before.
    uint64_t fileSize = 0;
    if (knownSize.has_value()) {
        fileSize = *knownSize;
    } else {
        const auto fsStart = std::chrono::steady_clock::now();
        std::error_code ec;
        fileSize = static_cast<uint64_t>(fs::file_size(path, ec));
        const auto fsEnd = std::chrono::steady_clock::now();
        g_hashFileSizeUs.fetch_add(static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(fsEnd - fsStart).count()),
            std::memory_order_relaxed);
        if (ec) {
            throw std::runtime_error("ComputeFileHashViaDriver: file_size failed");
        }
    }

    const auto openStart = std::chrono::steady_clock::now();
    // Change 3a (fastcheck-redundant-syscall-elim, FR-19): pass fileSize as the read-open expectedSize
    // so the backend skips the redundant Windows FileSizeOnDisk query. fileSize came either from the
    // caller's knownSize (no stat) or from the fs::file_size stat just above. A 0-byte file passes
    // fileSize==0 (unknown/legacy path), which is fine (early-returns below).
    const uint64_t fid =
        driver.openFile(fc::PathToUtf8(path), fc::io::OpKind::Read, /*unbuffered=*/true, fileSize);
    const auto openEnd = std::chrono::steady_clock::now();
    g_hashOpenUs.fetch_add(static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(openEnd - openStart).count()),
        std::memory_order_relaxed);
    if (fid == 0) {
        throw std::runtime_error("ComputeFileHashViaDriver: open failed");
    }

    struct FileCloser {
        fc::io::DiskIoDriver& driver;
        uint64_t fileId = 0;
        ~FileCloser() {
            const auto c0 = std::chrono::steady_clock::now();
            driver.closeFile(fileId);
            const auto c1 = std::chrono::steady_clock::now();
            g_hashCloseUs.fetch_add(static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(c1 - c0).count()),
                std::memory_order_relaxed);
        }
    } closer{driver, fid};

    Hash256 hash{};
    if (fileSize == 0) {
        const auto xxhStart = std::chrono::steady_clock::now();
        hash = ComputeBufferHash(nullptr, 0);
        const auto xxhEnd = std::chrono::steady_clock::now();
        g_hashXxhUs.fetch_add(static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(xxhEnd - xxhStart).count()),
            std::memory_order_relaxed);
        g_hashTotalUs.fetch_add(static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - fnStart).count()), std::memory_order_relaxed);
        return hash;
    }

    if (fileSize <= kSmallFileDirectThreshold) {
        const auto readStart = std::chrono::steady_clock::now();
        const fc::io::AlignInfo align = driver.queryAlign(fc::PathToUtf8(path));
        if (align.ioGranularity == 0) {
            throw std::runtime_error("ComputeFileHashViaDriver: invalid io granularity");
        }
        const uint64_t reqOffset = fc::io::AlignDown(0, align.ioGranularity);
        const uint64_t reqLength = fc::io::AlignUp(fileSize, align.ioGranularity);
        if (reqLength == 0 || reqLength > (std::numeric_limits<uint32_t>::max)()) {
            throw std::runtime_error("ComputeFileHashViaDriver: request too large");
        }

        fc::io::IoRequest req;
        req.kind = fc::io::OpKind::Read;
        req.fileId = fid;
        req.offset = reqOffset;
        req.length = static_cast<uint32_t>(reqLength);
        req.prio = fc::io::Prio::Small;
        req.userTag = 0;

        std::vector<fc::io::IoRequest> batch;
        batch.push_back(std::move(req));
        const size_t accepted = driver.submit(batch);
        if (accepted != 1 || !batch.empty()) {
            throw std::runtime_error("ComputeFileHashViaDriver: submit failed");
        }

        std::vector<fc::io::IoCompletion> comps;
        driver.drainCompletionsForFile(fid, comps);
        if (comps.empty()) {
            driver.waitForFile(fid, 1000);
            driver.drainCompletionsForFile(fid, comps);
        }
        const auto readEnd = std::chrono::steady_clock::now();
        g_hashReadUs.fetch_add(static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(readEnd - readStart).count()),
            std::memory_order_relaxed);
        if (comps.size() != 1) {
            throw std::runtime_error("ComputeFileHashViaDriver: completion count mismatch");
        }
        const fc::io::IoCompletion& comp = comps.front();
        if (comp.status == fc::io::IoStatus::Error || comp.status == fc::io::IoStatus::Cancelled) {
            throw std::runtime_error("ComputeFileHashViaDriver: read failed");
        }
        if (comp.status == fc::io::IoStatus::Eof && comp.transferred < fileSize) {
            throw std::runtime_error("ComputeFileHashViaDriver: early eof");
        }
        if (comp.transferred < fileSize || comp.data.size() < static_cast<size_t>(fileSize)) {
            throw std::runtime_error("ComputeFileHashViaDriver: short read");
        }
        const auto xxhStart = std::chrono::steady_clock::now();
        hash = ComputeBufferHash(comp.data.data(), static_cast<size_t>(fileSize));
        const auto xxhEnd = std::chrono::steady_clock::now();
        g_hashXxhUs.fetch_add(static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(xxhEnd - xxhStart).count()),
            std::memory_order_relaxed);
        g_hashTotalUs.fetch_add(static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - fnStart).count()), std::memory_order_relaxed);
        return hash;
    }

    bool readErr = false;
    std::vector<uint8_t> chunkBuf;
    size_t chunkPos = 0;
    const auto readStart = std::chrono::steady_clock::now();
    fc::io::SequentialReader reader(driver, fid, fileSize, kHashChunkBytes, kHashReadAhead);
    auto source = [&](uint8_t* dst, size_t maxLen) -> size_t {
        size_t written = 0;
        while (written < maxLen) {
            if (chunkPos < chunkBuf.size()) {
                const size_t take = std::min<size_t>(chunkBuf.size() - chunkPos, maxLen - written);
                std::memcpy(dst + written, chunkBuf.data() + chunkPos, take);
                chunkPos += take;
                written += take;
                continue;
            }
            bool ok = true;
            chunkBuf.clear();
            chunkPos = 0;
            const uint32_t n = reader.next(chunkBuf, ok);
            if (!ok) {
                readErr = true;
                break;
            }
            if (n == 0) {
                break;  // clean EOF
            }
        }
        return written;
    };
    const auto xxhStart = std::chrono::steady_clock::now();
    hash = ComputeHashFromSource(source);
    const auto xxhEnd = std::chrono::steady_clock::now();
    // Slow path: ComputeHashFromSource interleaves reader.next (IO) and XXH3 update, so the read
    // and hash time are folded here into g_hashReadUs (g_hashXxhUs left for the fast path). The
    // slow path is the minority (>256 KiB); precise split is not worth a second timer here.
    g_hashReadUs.fetch_add(static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(xxhEnd - readStart).count()),
        std::memory_order_relaxed);
    if (readErr) {
        throw std::runtime_error("ComputeFileHashViaDriver: read failed");
    }
    g_hashTotalUs.fetch_add(static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - fnStart).count()), std::memory_order_relaxed);
    return hash;
}

HashPhaseTimings GetHashPhaseTimings() {
    HashPhaseTimings t;
    t.count = g_hashCount.load(std::memory_order_relaxed);
    t.totalUs = g_hashTotalUs.load(std::memory_order_relaxed);
    t.fileSizeUs = g_hashFileSizeUs.load(std::memory_order_relaxed);
    t.openUs = g_hashOpenUs.load(std::memory_order_relaxed);
    t.readUs = g_hashReadUs.load(std::memory_order_relaxed);
    t.xxhUs = g_hashXxhUs.load(std::memory_order_relaxed);
    t.closeUs = g_hashCloseUs.load(std::memory_order_relaxed);
    return t;
}

Hash256 ComputeBufferHash(const uint8_t* data, size_t len) {
    // XXH3_128bits one-shot is bit-identical to the streaming path in ComputeFileHash for the
    // same bytes; the raw memcpy of XXH128_hash_t reproduces the exact same Hash256 layout, so
    // a value produced here verifies equal against ComputeFileHash on the reconstructed file.
    const XXH128_hash_t digest = XXH3_128bits(data, len);
    Hash256 output{};
    std::memcpy(output.data(), &digest, output.size());
    return output;
}

bool HashEquals(const Hash256& a, const Hash256& b) {
    return std::equal(a.begin(), a.end(), b.begin(), b.end());
}

void SetFileModifyTime(const fs::path& path, int64_t modifyNs) {
#ifdef _WIN32
    HANDLE handle = CreateFileW(path.wstring().c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return;
    }
    const FILETIME writeFt = ToFileTimeFromNs(modifyNs);
    SetFileTime(handle, nullptr, nullptr, &writeFt);
    CloseHandle(handle);
#else
    // S-02: normalize a possible Windows FILETIME-ticks input to Unix ns first (FR-16 / C3), so this
    // path matches the driver write path and the small-file fast path. master did FromUnixNs(modifyNs)
    // directly, which was correct for POSIX-peer (Unix-ns) inputs but wrote raw ticks when the manifest
    // came from a Windows peer. NormalizeManifestMtimeToUnixNs fixes that without breaking Unix-ns.
    std::error_code ec;
    fs::last_write_time(path, FromUnixNs(NormalizeManifestMtimeToUnixNs(modifyNs)), ec);
#endif
}

bool ShouldUseSmallFileFastPath(uint64_t size) {
    return size <= kSmallFileFastPathMax;
}

bool WriteSmallFileFastPath(const fs::path& path, const uint8_t* data, size_t size,
                            int64_t modifyNs) {
    // W-05: a single-shot synchronous write that mirrors the DiskIoDriver whole-file path's on-disk
    // result exactly (content, exact size, mtime, truncating overwrite) without the driver's
    // scheduling / aligned-bounce / completion-gate overhead. Parent dir is the caller's job (B6).
#ifdef _WIN32
    HANDLE h = CreateFileW(ToExtendedLengthPath(path).c_str(), GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    bool ok = true;
    size_t written = 0;
    while (ok && written < size) {
        const DWORD chunk =
            static_cast<DWORD>(std::min<size_t>(size - written, static_cast<size_t>(1u << 20)));
        DWORD wrote = 0;
        if (!WriteFile(h, data + written, chunk, &wrote, nullptr) || wrote != chunk) {
            ok = false;
            break;
        }
        written += wrote;
    }
    // Exact final size (CREATE_ALWAYS already truncated to 0; writing `size` bytes leaves it at size,
    // but keep SetEndOfFile for parity with the driver close path, incl. the size==0 empty file).
    if (ok) {
        LARGE_INTEGER li;
        li.QuadPart = static_cast<LONGLONG>(size);
        if (!SetFilePointerEx(h, li, nullptr, FILE_BEGIN) || !SetEndOfFile(h)) {
            ok = false;
        }
    }
    if (ok) {
        const FILETIME writeFt = ToFileTimeFromNs(modifyNs);
        if (!SetFileTime(h, nullptr, nullptr, &writeFt)) {
            ok = false;
        }
    }
    if (!CloseHandle(h)) {
        ok = false;
    }
    return ok;
#else
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        return false;
    }
    bool ok = true;
    size_t written = 0;
    while (ok && written < size) {
        const ssize_t w = ::write(fd, data + written, size - written);
        if (w < 0) {
            ok = false;
            break;
        }
        written += static_cast<size_t>(w);
    }
    if (ok && ::ftruncate(fd, static_cast<off_t>(size)) != 0) {
        ok = false;
    }
    if (ok) {
        timespec ts[2];
        ts[0].tv_sec = 0;
        ts[0].tv_nsec = UTIME_OMIT;  // leave atime untouched
        // S-02 / D-14-A: single normalization source shared with SetFileModifyTime. Direction mirrors
        // compare_phase.cpp::TryNormalizeMtimeToUnixNs: >5e17 is Unix ns (passthrough), [1.16e17,5e17]
        // is FILETIME ticks (-> ns), 0/negative pass through (clamped to 0).
        const int64_t unixNs = NormalizeManifestMtimeToUnixNs(modifyNs);
        ts[1].tv_sec = static_cast<time_t>(unixNs / 1000000000LL);
        ts[1].tv_nsec = static_cast<long>(unixNs % 1000000000LL);
        if (::futimens(fd, ts) != 0) {
            ok = false;
        }
    }
    if (::close(fd) != 0) {
        ok = false;
    }
    return ok;
#endif
}

}  // namespace fc
