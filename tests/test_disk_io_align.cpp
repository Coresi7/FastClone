// Tests for the runtime alignment + aligned allocation module (unified-disk-io-driver C1,
// FR-08/FR-09, AC-14/15/16). These assert (a) alignment math is driven purely by the injected
// runtime page/device-block sizes (never a literal 512/4096), (b) 16 KiB / 64 KiB page replicas
// compute offsets/padding correctly (AC-15), and (c) aligned alloc/free are platform-matched and
// return correctly aligned memory (AC-16). No disk IO, no threads.

#include "disk_io_align.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>

namespace {

void Require(bool cond, const std::string& msg) {
    if (!cond) {
        throw std::runtime_error("disk_io_align: " + msg);
    }
}

void TestAlignMath() {
    using namespace fc::io;
    // Pure helpers over arbitrary power-of-two alignments (no hard-coded page assumption).
    Require(AlignDown(0, 4096) == 0, "aligndown 0");
    Require(AlignUp(0, 4096) == 0, "alignup 0");
    Require(AlignDown(4097, 4096) == 4096, "aligndown 4097/4096");
    Require(AlignUp(4097, 4096) == 8192, "alignup 4097/4096");
    Require(IsAligned(8192, 4096), "8192 aligned to 4096");
    Require(!IsAligned(8191, 4096), "8191 not aligned");

    // 16 KiB page replica (Apple Silicon) - AC-15.
    const uint32_t p16 = 16u * 1024;
    Require(AlignDown(20000, p16) == p16, "16k aligndown");
    Require(AlignUp(20000, p16) == 2u * p16, "16k alignup");
    // 64 KiB page replica (ARM64 Linux) - AC-15.
    const uint32_t p64 = 64u * 1024;
    Require(AlignUp(1, p64) == p64, "64k alignup tiny");
    Require(AlignDown(p64 + 1, p64) == p64, "64k aligndown");

    // Zero alignment is a no-op (defensive; never used as a real alignment).
    Require(AlignUp(123, 0) == 123, "alignup 0-alignment no-op");
    Require(!IsAligned(123, 0), "IsAligned false for 0 alignment");
}

void TestMakeAlignInfo() {
    using namespace fc::io;
    // ioGranularity is max(page, block); a zero field normalizes to the other (no literal).
    AlignInfo a = MakeAlignInfo(4096, 512);
    Require(a.pageSize == 4096 && a.deviceBlockSize == 512 && a.ioGranularity == 4096,
            "granularity = max(page, block)");
    AlignInfo b = MakeAlignInfo(4096, 65536);  // 64k device block dominates
    Require(b.ioGranularity == 65536, "granularity picks larger block");
    AlignInfo c = MakeAlignInfo(0, 8192);  // missing page -> use block
    Require(c.pageSize == 8192 && c.ioGranularity == 8192, "missing page normalizes to block");
    AlignInfo d = MakeAlignInfo(16384, 0);  // missing block -> use page
    Require(d.deviceBlockSize == 16384 && d.ioGranularity == 16384, "missing block -> page");
}

void TestRuntimeQuery() {
    using namespace fc::io;
    // Page size must come from the OS at runtime and be a power of two >= 1 (no literal assumed).
    const uint32_t page = QueryPageSize();
    Require(page > 0, "runtime page size > 0");
    Require((page & (page - 1)) == 0, "runtime page size is power of two");

    // QueryAlign on the current directory returns a coherent, runtime-derived AlignInfo.
    AlignInfo info = QueryAlign(".");
    Require(info.pageSize > 0, "query page > 0");
    Require(info.deviceBlockSize > 0, "query block > 0");
    Require(info.ioGranularity >= info.pageSize && info.ioGranularity >= info.deviceBlockSize,
            "granularity dominates");
}

void TestAlignedAlloc() {
    using namespace fc::io;
    for (size_t align : {size_t{512}, size_t{4096}, size_t{16384}, size_t{65536}}) {
        void* p = AlignedAlloc(align, align * 4);
        Require(p != nullptr, "aligned alloc non-null");
        Require((reinterpret_cast<uintptr_t>(p) % align) == 0, "pointer aligned");
        // Touch the whole buffer to catch under-allocation under ASan/CRT debug heap.
        for (size_t i = 0; i < align * 4; ++i) {
            static_cast<uint8_t*>(p)[i] = static_cast<uint8_t>(i & 0xFF);
        }
        AlignedFree(p);  // must pair with AlignedAlloc (AC-16)
    }
    AlignedFree(nullptr);  // no-op, must not crash
}

// F5 (AC-16): QueryAlign on an ASCII path returns a coherent, runtime-derived AlignInfo.
void TestQueryAlignAscii() {
    using namespace fc::io;
    namespace fs = std::filesystem;
    const fs::path tmp = fs::temp_directory_path() / "fc_align_ascii.bin";
    std::error_code ec;
    { std::ofstream f(tmp); f << 'x'; }
    AlignInfo info = QueryAlign(tmp.string());
    Require(info.pageSize > 0, "F5 ascii: page > 0");
    Require(info.deviceBlockSize > 0, "F5 ascii: block > 0");
    Require(info.ioGranularity >= info.pageSize && info.ioGranularity >= info.deviceBlockSize,
            "F5 ascii: granularity dominates");
    fs::remove(tmp, ec);
}

#if defined(_WIN32)
// F5 (AC-15): a file under a non-ASCII UTF-8 path must resolve (deviceBlockSize > 0), proving the
// MultiByteToWideChar(CP_UTF8) conversion, not the byte-wise widen which fails for such paths.
void TestQueryAlignNonAsciiWindows() {
    using namespace fc::io;
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / L"fc_align_\u6d4b\u8bd5_\u03b4\u03bf\u03ba\u03b9\u03bc\u03ae";
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        return;  // environment gate (design R-05): cannot create the wide dir -> skip
    }
    const fs::path file = dir / L"f.bin";
    { std::ofstream f(file); f << 'x'; }
    // u8string() is the UTF-8 encoding expected by QueryAlign's std::string input.
    const std::u8string u8 = file.u8string();
    const std::string path(u8.begin(), u8.end());
    AlignInfo info = QueryAlign(path);
    Require(info.deviceBlockSize > 0, "F5 non-ascii: block > 0 (UTF-8 path resolved)");
    Require(info.ioGranularity >= info.deviceBlockSize, "F5 non-ascii: granularity >= block");
    fs::remove_all(dir, ec);
}
#endif

}  // namespace

void RunDiskIoAlignTests() {
    TestAlignMath();
    TestMakeAlignInfo();
    TestRuntimeQuery();
    TestAlignedAlloc();
    TestQueryAlignAscii();
#if defined(_WIN32)
    TestQueryAlignNonAsciiWindows();
#endif
}
