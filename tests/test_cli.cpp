#include "cli.h"

#include <stdexcept>
#include <string>
#include <vector>

namespace {

void Require(bool cond, const char* msg) {
    if (!cond) {
        throw std::runtime_error(msg);
    }
}

fc::CliOptions Parse(const std::vector<std::string>& args) {
#if defined(_WIN32) && defined(_MSC_VER)
    // On MSVC fc::ParseCli takes wchar_t** (wmain). Test tokens are ASCII, so a direct
    // widen is lossless. This keeps the same harness buildable under the Visual Studio
    // CMake generator (MSVC) as well as the POSIX char** path below.
    std::vector<std::wstring> wide;
    wide.reserve(args.size());
    for (const std::string& arg : args) {
        wide.emplace_back(arg.begin(), arg.end());
    }
    std::vector<wchar_t*> argv;
    argv.reserve(wide.size());
    for (std::wstring& warg : wide) {
        argv.push_back(warg.data());
    }
    return fc::ParseCli(static_cast<int>(argv.size()), argv.data());
#else
    std::vector<char*> argv;
    argv.reserve(args.size());
    for (const std::string& arg : args) {
        argv.push_back(const_cast<char*>(arg.c_str()));
    }
    return fc::ParseCli(static_cast<int>(argv.size()), argv.data());
#endif
}

void ExpectThrowWith(const std::vector<std::string>& args, const std::string& token) {
    try {
        (void)Parse(args);
        throw std::runtime_error("Expected ParseCli to throw");
    } catch (const std::exception& ex) {
        Require(std::string(ex.what()).find(token) != std::string::npos, "Unexpected exception text");
    }
}

}  // namespace

void RunCliTests() {
    {
        const fc::CliOptions opt = Parse({
            "FastClone",
            "server",
            "--password",
            "pw",
            "--server-hash-workers",
            "7",
            "--enable-hash-memcache",
        });
        Require(opt.mode == fc::Mode::Server, "Expected server mode");
        Require(opt.serverHashWorkers == 7, "Expected parsed server hash worker count");
        Require(opt.enableHashMemcache, "Expected hash memcache flag enabled");
    }

    ExpectThrowWith({
                        "FastClone",
                        "client",
                        "--server",
                        "127.0.0.1:27842",
                        "--target",
                        ".",
                        "--password",
                        "pw",
                        "--enable-hash-memcache",
                    },
                    "server-only");

    {
        const fc::CliOptions opt = Parse({
            "FastClone",
            "client",
            "--server",
            "127.0.0.1:27842",
            "--target",
            ".",
            "--password",
            "pw",
            "--reconnect-retries",
            "0",
            "--reconnect-window",
            "5m",
        });
        Require(opt.mode == fc::Mode::Client, "Expected client mode");
        Require(opt.reconnectRetries == 0, "Expected reconnect retries 0");
        Require(opt.reconnectWindowMs == 5ULL * 60ULL * 1000ULL, "Expected reconnect window 5m");
    }

    ExpectThrowWith({
                        "FastClone",
                        "client",
                        "--server",
                        "127.0.0.1:27842",
                        "--target",
                        ".",
                        "--password",
                        "pw",
                        "--reconnect-window",
                        "bad",
                    },
                    "reconnect-window");

    // V-01 (AC-01/AC-11.1): server --once parses cleanly into exitAfterSync.
    {
        const fc::CliOptions opt = Parse({
            "FastClone",
            "server",
            "--password",
            "pw",
            "--once",
        });
        Require(opt.mode == fc::Mode::Server, "Expected server mode for --once");
        Require(opt.exitAfterSync, "Expected exitAfterSync set by --once");
    }

    // V-02 (AC-02/AC-11.2): --once is server-only; a client must be rejected.
    ExpectThrowWith({
                        "FastClone",
                        "client",
                        "--server",
                        "127.0.0.1:27842",
                        "--target",
                        ".",
                        "--password",
                        "pw",
                        "--once",
                    },
                    "server-only");

    // V-03 (AC-03/AC-11.3): --once and --enable-hash-memcache are mutually exclusive.
    ExpectThrowWith({
                        "FastClone",
                        "server",
                        "--password",
                        "pw",
                        "--once",
                        "--enable-hash-memcache",
                    },
                    "mutually exclusive");
}
