#include "disk_io_align.h"

#include <algorithm>
#include <cstdlib>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <malloc.h>  // _aligned_malloc / _aligned_free
#else
#include <unistd.h>
#include <cstdlib>  // posix_memalign / free
#if defined(__linux__)
#include <sys/ioctl.h>
#include <sys/statvfs.h>
#include <fcntl.h>
#include <linux/fs.h>  // BLKSSZGET
#include <cerrno>
#endif
#if defined(__APPLE__)
#include <sys/statvfs.h>
#endif
#endif

namespace fc::io {

AlignInfo MakeAlignInfo(uint32_t pageSize, uint32_t deviceBlockSize) {
    AlignInfo info;
    // Normalize a missing field to the other one at runtime; never substitute a literal size.
    if (pageSize == 0) {
        pageSize = deviceBlockSize;
    }
    if (deviceBlockSize == 0) {
        deviceBlockSize = pageSize;
    }
    info.pageSize = pageSize;
    info.deviceBlockSize = deviceBlockSize;
    info.ioGranularity = std::max(pageSize, deviceBlockSize);
    return info;
}

uint32_t QueryPageSize() {
#if defined(_WIN32)
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return static_cast<uint32_t>(si.dwPageSize);
#else
    long ps = sysconf(_SC_PAGESIZE);
    if (ps <= 0) {
        ps = sysconf(_SC_PAGE_SIZE);  // legacy spelling fallback (still runtime, no literal)
    }
    return ps > 0 ? static_cast<uint32_t>(ps) : 0u;
#endif
}

namespace {

#if defined(_WIN32)
// Device logical sector size for the volume hosting `path`, obtained at runtime. Returns 0 when
// the query fails so MakeAlignInfo falls back to the page size (never a literal).
uint32_t QueryDeviceBlockSize(const std::string& path) {
    // Resolve the root path ("D:\") of the file's volume; GetDiskFreeSpaceW reports the sector
    // size at runtime. This is a metadata query (not file content), so it is outside the driver.
    // F5: convert the UTF-8 path to UTF-16 via MultiByteToWideChar (CP_UTF8) instead of a byte-wise
    // widen, so non-ASCII paths resolve correctly (matches disk_io_backend_win.cpp Widen).
    std::wstring wpath;
    if (!path.empty()) {
        const int n = ::MultiByteToWideChar(CP_UTF8, 0, path.c_str(),
                                            static_cast<int>(path.size()), nullptr, 0);
        if (n > 0) {
            wpath.resize(static_cast<size_t>(n));
            ::MultiByteToWideChar(CP_UTF8, 0, path.c_str(),
                                  static_cast<int>(path.size()), wpath.data(), n);
        }
    }
    wchar_t root[MAX_PATH] = {0};
    if (GetVolumePathNameW(wpath.c_str(), root, MAX_PATH) == 0) {
        return 0;
    }
    DWORD sectorsPerCluster = 0, bytesPerSector = 0, freeClusters = 0, totalClusters = 0;
    if (GetDiskFreeSpaceW(root, &sectorsPerCluster, &bytesPerSector, &freeClusters,
                          &totalClusters) == 0) {
        return 0;
    }
    return static_cast<uint32_t>(bytesPerSector);
}
#elif defined(__linux__)
uint32_t QueryDeviceBlockSize(const std::string& path) {
    // Prefer the block device logical sector size (BLKSSZGET); fall back to the filesystem block
    // size from statvfs. Both are runtime queries; no literal is used.
    int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        int ssz = 0;
        const int rc = ::ioctl(fd, BLKSSZGET, &ssz);
        ::close(fd);
        if (rc == 0 && ssz > 0) {
            return static_cast<uint32_t>(ssz);
        }
    }
    struct statvfs vfs {};
    if (::statvfs(path.c_str(), &vfs) == 0 && vfs.f_bsize > 0) {
        return static_cast<uint32_t>(vfs.f_bsize);
    }
    return 0;
}
#else  // __APPLE__ and other POSIX
uint32_t QueryDeviceBlockSize(const std::string& path) {
    struct statvfs vfs {};
    if (::statvfs(path.c_str(), &vfs) == 0 && vfs.f_bsize > 0) {
        return static_cast<uint32_t>(vfs.f_bsize);
    }
    return 0;
}
#endif

}  // namespace

AlignInfo QueryAlign(const std::string& path) {
    const uint32_t page = QueryPageSize();
    const uint32_t block = QueryDeviceBlockSize(path);
    return MakeAlignInfo(page, block);
}

uint64_t AlignDown(uint64_t value, uint32_t alignment) {
    if (alignment == 0) {
        return value;
    }
    return value - (value % alignment);
}

uint64_t AlignUp(uint64_t value, uint32_t alignment) {
    if (alignment == 0) {
        return value;
    }
    const uint64_t rem = value % alignment;
    return rem == 0 ? value : value + (alignment - rem);
}

bool IsAligned(uint64_t value, uint32_t alignment) {
    return alignment != 0 && (value % alignment) == 0;
}

void* AlignedAlloc(size_t alignment, size_t size) {
    if (alignment == 0) {
        return nullptr;
    }
#if defined(_WIN32)
    return _aligned_malloc(size, alignment);
#else
    // posix_memalign requires alignment to be a power of two multiple of sizeof(void*).
    size_t a = alignment;
    if (a < sizeof(void*)) {
        a = sizeof(void*);
    }
    void* p = nullptr;
    if (::posix_memalign(&p, a, size) != 0) {
        return nullptr;
    }
    return p;
#endif
}

void AlignedFree(void* p) {
    if (p == nullptr) {
        return;
    }
#if defined(_WIN32)
    _aligned_free(p);
#else
    ::free(p);
#endif
}

}  // namespace fc::io
