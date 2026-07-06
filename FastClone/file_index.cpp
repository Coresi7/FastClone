#include "file_index.h"
#include "disk_io_driver.h"
#include "path_utils.h"

#ifdef _WIN32
#include <Windows.h>
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
    };
    std::vector<Candidate> candidates;
    for (const auto& item : fs::recursive_directory_iterator(canonicalRoot, fs::directory_options::skip_permission_denied)) {
        const fs::path absPath = item.path();
        if (canonicalExclude.has_value() && fs::exists(*canonicalExclude) && absPath == *canonicalExclude) {
            continue;
        }
        if (canonicalExclude.has_value() && item.is_directory() && IsPathUnderRoot(absPath, *canonicalExclude)) {
            continue;
        }
        if (!item.is_directory() && !item.is_regular_file()) {
            continue;
        }
        candidates.push_back(Candidate{absPath, item.is_directory(), item.is_regular_file()});
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
                    std::error_code sec;
                    entry.fileSize = static_cast<uint64_t>(fs::file_size(c.absPath, sec));
                    if (sec) {
                        continue;
                    }
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

Hash256 ComputeFileHashViaDriver(fc::io::DiskIoDriver& driver, const fs::path& path) {
    // Shared hash read path for FastCheck + server hash-miss:
    //  - <=256 KiB: one driver read request + ComputeBufferHash over real bytes
    //  - >256 KiB : existing SequentialReader streaming path
    const auto fnStart = std::chrono::steady_clock::now();
    g_hashCount.fetch_add(1, std::memory_order_relaxed);

    const auto fsStart = std::chrono::steady_clock::now();
    std::error_code ec;
    const uint64_t fileSize = static_cast<uint64_t>(fs::file_size(path, ec));
    const auto fsEnd = std::chrono::steady_clock::now();
    g_hashFileSizeUs.fetch_add(static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(fsEnd - fsStart).count()),
        std::memory_order_relaxed);
    if (ec) {
        throw std::runtime_error("ComputeFileHashViaDriver: file_size failed");
    }

    const auto openStart = std::chrono::steady_clock::now();
    // Change 3a (fastcheck-redundant-syscall-elim, FR-19): reuse the file size we just read above as
    // the read-open expectedSize so the backend skips the redundant Windows FileSizeOnDisk query.
    // A 0-byte file passes fileSize==0 (unknown/legacy path), which is fine (early-returns below).
    const uint64_t fid =
        driver.openFile(path.string(), fc::io::OpKind::Read, /*unbuffered=*/true, fileSize);
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
        const fc::io::AlignInfo align = driver.queryAlign(path.string());
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
    std::error_code ec;
    fs::last_write_time(path, FromUnixNs(modifyNs), ec);
#endif
}

}  // namespace fc
