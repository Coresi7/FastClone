#include "file_index.h"

#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <thread>

#pragma comment(lib, "Bcrypt.lib")

namespace fs = std::filesystem;

namespace fc {

namespace {

bool IsUnderRoot(const fs::path& root, const fs::path& path) {
    const fs::path canonicalRoot = fs::weakly_canonical(root);
    const fs::path canonicalPath = fs::weakly_canonical(path);
    auto rootIt = canonicalRoot.begin();
    auto pathIt = canonicalPath.begin();
    while (rootIt != canonicalRoot.end() && pathIt != canonicalPath.end()) {
        if (*rootIt != *pathIt) {
            return false;
        }
        ++rootIt;
        ++pathIt;
    }
    return rootIt == canonicalRoot.end();
}

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
    return duration_cast<nanoseconds>(value.time_since_epoch()).count();
}

fs::file_time_type FromUnixNs(int64_t valueNs) {
    using namespace std::chrono;
    const auto d = duration_cast<fs::file_time_type::duration>(nanoseconds(valueNs));
    return fs::file_time_type(d);
}

std::vector<FileEntry> BuildIndex(const fs::path& root, const std::optional<fs::path>& excludeAbsPath) {
    std::vector<FileEntry> output;
    const fs::path canonicalRoot = fs::weakly_canonical(root);
    const std::optional<fs::path> canonicalExclude = excludeAbsPath.has_value() ? std::optional<fs::path>(fs::weakly_canonical(*excludeAbsPath))
                                                                                 : std::nullopt;
    if (!fs::exists(canonicalRoot)) {
        throw std::runtime_error("Root directory does not exist");
    }
    output.push_back(FileEntry{".", true, 0, 0, 0});

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
        if (canonicalExclude.has_value() && item.is_directory() && IsUnderRoot(absPath, *canonicalExclude)) {
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
                    HANDLE handle = CreateFileW(c.absPath.wstring().c_str(), GENERIC_READ,
                                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                                nullptr, OPEN_EXISTING,
                                                FILE_FLAG_BACKUP_SEMANTICS, nullptr);
                    if (handle != INVALID_HANDLE_VALUE) {
                        FILETIME createFt{}, accessFt{}, writeFt{};
                        if (GetFileTime(handle, &createFt, &accessFt, &writeFt) != 0) {
                            entry.ctimeNs = ToNsFromFileTime(createFt);
                            entry.mtimeNs = ToNsFromFileTime(writeFt);
                        }
                        CloseHandle(handle);
                    }
                    if (entry.mtimeNs == 0) {
                        entry.mtimeNs = ToUnixNs(fs::last_write_time(c.absPath));
                        entry.ctimeNs = entry.mtimeNs;
                    }
                } else if (c.isRegular) {
                    std::error_code sec;
                    entry.fileSize = static_cast<uint64_t>(fs::file_size(c.absPath, sec));
                    if (sec) {
                        continue;
                    }
                    HANDLE handle = CreateFileW(c.absPath.wstring().c_str(), GENERIC_READ,
                                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                    if (handle != INVALID_HANDLE_VALUE) {
                        FILETIME createFt{}, accessFt{}, writeFt{};
                        if (GetFileTime(handle, &createFt, &accessFt, &writeFt) != 0) {
                            entry.ctimeNs = ToNsFromFileTime(createFt);
                            entry.mtimeNs = ToNsFromFileTime(writeFt);
                        } else {
                            entry.mtimeNs = ToUnixNs(fs::last_write_time(c.absPath));
                            entry.ctimeNs = entry.mtimeNs;
                        }
                        CloseHandle(handle);
                    } else {
                        entry.mtimeNs = ToUnixNs(fs::last_write_time(c.absPath));
                        entry.ctimeNs = entry.mtimeNs;
                    }
                } else {
                    continue;
                }
                std::lock_guard<std::mutex> lock(pushMu);
                output.push_back(std::move(entry));
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }

    std::sort(output.begin(), output.end(), [](const FileEntry& a, const FileEntry& b) {
        if (a.isDirectory != b.isDirectory) {
            return a.isDirectory;
        }
        return a.relativePath < b.relativePath;
    });
    return output;
}

Hash256 ComputeFileSha256(const fs::path& path) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD hashObjectLen = 0;
    DWORD hashLen = 0;
    DWORD bytesDone = 0;

    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) {
        throw std::runtime_error("BCryptOpenAlgorithmProvider failed");
    }
    if (BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&hashObjectLen), sizeof(hashObjectLen), &bytesDone, 0) < 0) {
        BCryptCloseAlgorithmProvider(alg, 0);
        throw std::runtime_error("BCryptGetProperty OBJECT_LENGTH failed");
    }
    if (BCryptGetProperty(alg, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hashLen), sizeof(hashLen), &bytesDone, 0) < 0) {
        BCryptCloseAlgorithmProvider(alg, 0);
        throw std::runtime_error("BCryptGetProperty HASH_LENGTH failed");
    }

    std::vector<uint8_t> hashObject(hashObjectLen);
    if (BCryptCreateHash(alg, &hash, hashObject.data(), hashObjectLen, nullptr, 0, 0) < 0) {
        BCryptCloseAlgorithmProvider(alg, 0);
        throw std::runtime_error("BCryptCreateHash failed");
    }

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(alg, 0);
        throw std::runtime_error("Open file failed for hash");
    }
    std::vector<uint8_t> buf(1 << 20);
    while (in) {
        in.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
        const std::streamsize got = in.gcount();
        if (got > 0) {
            if (BCryptHashData(hash, buf.data(), static_cast<ULONG>(got), 0) < 0) {
                BCryptDestroyHash(hash);
                BCryptCloseAlgorithmProvider(alg, 0);
                throw std::runtime_error("BCryptHashData failed");
            }
        }
    }
    Hash256 output{};
    if (hashLen != output.size()) {
        BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(alg, 0);
        throw std::runtime_error("Unexpected SHA256 length");
    }
    if (BCryptFinishHash(hash, output.data(), static_cast<ULONG>(output.size()), 0) < 0) {
        BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(alg, 0);
        throw std::runtime_error("BCryptFinishHash failed");
    }
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);
    return output;
}

bool HashEquals(const Hash256& a, const Hash256& b) {
    return std::equal(a.begin(), a.end(), b.begin(), b.end());
}

void SetFileCreateAndModifyTime(const fs::path& path, int64_t createNs, int64_t modifyNs) {
    HANDLE handle = CreateFileW(path.wstring().c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return;
    }
    const FILETIME createFt = ToFileTimeFromNs(createNs);
    const FILETIME writeFt = ToFileTimeFromNs(modifyNs);
    SetFileTime(handle, nullptr, nullptr, &writeFt);
    SetFileTime(handle, &createFt, nullptr, nullptr);
    CloseHandle(handle);
}

}  // namespace fc
