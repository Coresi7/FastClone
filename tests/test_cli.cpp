#include "cli.h"

#include <cstdlib>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

#ifndef FC_STRINGIZE_DETAIL
#define FC_STRINGIZE_DETAIL(x) #x
#define FC_STRINGIZE(x) FC_STRINGIZE_DETAIL(x)
#endif

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
        });
        Require(opt.mode == fc::Mode::Client, "Expected client mode");
        Require(opt.reconnectRetries == 0, "Expected reconnect retries 0");
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
                    "Unknown argument");

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

    // OM-CLI-1 (AC-01): server --once-multi parses into onceMulti.
    {
        const fc::CliOptions opt = Parse({
            "FastClone",
            "server",
            "--password",
            "pw",
            "--once-multi",
        });
        Require(opt.mode == fc::Mode::Server, "Expected server mode for --once-multi");
        Require(opt.onceMulti, "Expected onceMulti set by --once-multi");
        Require(!opt.exitAfterSync, "Expected --once-multi to leave exitAfterSync false");
    }

    // OM-CLI-2 (AC-02): --once and --once-multi are mutually exclusive.
    ExpectThrowWith({
                        "FastClone",
                        "server",
                        "--password",
                        "pw",
                        "--once",
                        "--once-multi",
                    },
                    "mutually exclusive");

    // OM-CLI-3 (AC-03): --once-multi is server-only.
    ExpectThrowWith({
                        "FastClone",
                        "client",
                        "--server",
                        "127.0.0.1:27842",
                        "--target",
                        ".",
                        "--password",
                        "pw",
                        "--once-multi",
                    },
                    "server-only");

    // OM-CLI-4 (AC-04): --once-idle-grace parses s/m/h via the shared duration parser.
    {
        const fc::CliOptions opt = Parse({
            "FastClone", "server", "--password", "pw", "--once-multi",
            "--once-idle-grace", "7s",
        });
        Require(opt.onceIdleGraceMs == 7000ULL, "Expected --once-idle-grace 7s -> 7000ms");
    }
    {
        const fc::CliOptions opt = Parse({
            "FastClone", "server", "--password", "pw", "--once-multi",
            "--once-idle-grace", "2m",
        });
        Require(opt.onceIdleGraceMs == 120000ULL, "Expected --once-idle-grace 2m -> 120000ms");
    }
    {
        const fc::CliOptions opt = Parse({
            "FastClone", "server", "--password", "pw", "--once-multi",
            "--once-idle-grace", "1h",
        });
        Require(opt.onceIdleGraceMs == 3600000ULL, "Expected --once-idle-grace 1h -> 3600000ms");
    }

    // OM-CLI-5 (AC-05): default idle-grace is 5s when not specified.
    {
        const fc::CliOptions opt = Parse({
            "FastClone", "server", "--password", "pw", "--once-multi",
        });
        Require(opt.onceIdleGraceMs == 5000ULL, "Expected default --once-idle-grace 5000ms");
    }

    // OM-CLI-6 (AC-06): --once-idle-grace requires --once-multi.
    ExpectThrowWith({
                        "FastClone",
                        "server",
                        "--password",
                        "pw",
                        "--once-idle-grace",
                        "5s",
                    },
                    "requires --once-multi");

    // OM-CLI-7 (AC-07): --once-multi and --enable-hash-memcache may be combined.
    {
        const fc::CliOptions opt = Parse({
            "FastClone", "server", "--password", "pw", "--once-multi",
            "--enable-hash-memcache",
        });
        Require(opt.onceMulti, "Expected onceMulti set with memcache");
        Require(opt.enableHashMemcache, "Expected enableHashMemcache set with once-multi");
    }

    // WCT-CLI-1 (AC-01): default wait-connect-timeout is 300s for both --once and --once-multi.
    {
        const fc::CliOptions opt = Parse({
            "FastClone", "server", "--password", "pw", "--once",
        });
        Require(opt.waitConnectTimeoutMs == 300000ULL,
                "Expected default --wait-connect-timeout 300000ms under --once");
    }
    {
        const fc::CliOptions opt = Parse({
            "FastClone", "server", "--password", "pw", "--once-multi",
        });
        Require(opt.waitConnectTimeoutMs == 300000ULL,
                "Expected default --wait-connect-timeout 300000ms under --once-multi");
    }

    // WCT-CLI-2 (AC-02): --wait-connect-timeout parses bare-seconds and s/m/h via the shared parser.
    {
        const fc::CliOptions opt = Parse({
            "FastClone", "server", "--password", "pw", "--once",
            "--wait-connect-timeout", "30",
        });
        Require(opt.waitConnectTimeoutMs == 30000ULL, "Expected --wait-connect-timeout 30 -> 30000ms");
    }
    {
        const fc::CliOptions opt = Parse({
            "FastClone", "server", "--password", "pw", "--once",
            "--wait-connect-timeout", "30s",
        });
        Require(opt.waitConnectTimeoutMs == 30000ULL, "Expected --wait-connect-timeout 30s -> 30000ms");
    }
    {
        const fc::CliOptions opt = Parse({
            "FastClone", "server", "--password", "pw", "--once",
            "--wait-connect-timeout", "2m",
        });
        Require(opt.waitConnectTimeoutMs == 120000ULL, "Expected --wait-connect-timeout 2m -> 120000ms");
    }
    {
        const fc::CliOptions opt = Parse({
            "FastClone", "server", "--password", "pw", "--once",
            "--wait-connect-timeout", "1h",
        });
        Require(opt.waitConnectTimeoutMs == 3600000ULL, "Expected --wait-connect-timeout 1h -> 3600000ms");
    }

    // WCT-CLI-3 (AC-03): --wait-connect-timeout 0 is rejected ("must be > 0").
    ExpectThrowWith({
                        "FastClone", "server", "--password", "pw", "--once",
                        "--wait-connect-timeout", "0",
                    },
                    "must be > 0");

    // WCT-CLI-4 (AC-04): a bad suffix is rejected by the shared duration parser.
    ExpectThrowWith({
                        "FastClone", "server", "--password", "pw", "--once",
                        "--wait-connect-timeout", "10x",
                    },
                    "expected suffix [s|m|h]");

    // WCT-CLI-5 (AC-05): --wait-connect-timeout requires --once / --once-multi (resident server).
    ExpectThrowWith({
                        "FastClone", "server", "--password", "pw",
                        "--wait-connect-timeout", "30s",
                    },
                    "requires --once or --once-multi");

    // WCT-CLI-6 (AC-06): --wait-connect-timeout is rejected for clients (neither once flag set).
    ExpectThrowWith({
                        "FastClone", "client",
                        "--server", "127.0.0.1:27842",
                        "--target", ".",
                        "--password", "pw",
                        "--wait-connect-timeout", "30s",
                    },
                    "requires --once or --once-multi");

    // ---- aux-weight V-09 (AC-09/FR-09~FR-11): --aux-weight parsing ----------------------------
    auto clientArgs = [](const std::vector<std::string>& extra) {
        std::vector<std::string> args = {
            "FastClone", "client", "--server", "127.0.0.1:27842",
            "--target", ".", "--password", "pw",
        };
        args.insert(args.end(), extra.begin(), extra.end());
        return args;
    };

    // Default is 1.0 when --aux-weight is absent.
    {
        const fc::CliOptions opt = Parse(clientArgs({}));
        Require(opt.auxWeight == 1.0, "Expected default --aux-weight 1.0");
        Require(opt.largeFileLane == fc::LargeFileLane::Auto, "Expected default --large-file-lane auto");
    }
    // Legal values across the (0,16] range parse exactly.
    {
        const fc::CliOptions opt = Parse(clientArgs({"--aux-weight", "2"}));
        Require(opt.auxWeight == 2.0, "Expected --aux-weight 2 -> 2.0");
    }
    {
        const fc::CliOptions opt = Parse(clientArgs({"--aux-weight", "0.5"}));
        Require(opt.auxWeight == 0.5, "Expected --aux-weight 0.5 -> 0.5");
    }
    {
        const fc::CliOptions opt = Parse(clientArgs({"--aux-weight", "16"}));
        Require(opt.auxWeight == 16.0, "Expected --aux-weight 16 -> 16.0 (upper bound inclusive)");
    }
    // Out-of-range values are rejected.
    ExpectThrowWith(clientArgs({"--aux-weight", "0"}), "--aux-weight");
    ExpectThrowWith(clientArgs({"--aux-weight", "-1"}), "--aux-weight");
    ExpectThrowWith(clientArgs({"--aux-weight", "16.1"}), "--aux-weight");
    ExpectThrowWith(clientArgs({"--aux-weight", "100"}), "--aux-weight");
    // Non-numeric / malformed text is rejected.
    ExpectThrowWith(clientArgs({"--aux-weight", "abc"}), "--aux-weight");
    ExpectThrowWith(clientArgs({"--aux-weight", "2x"}), "--aux-weight");
    ExpectThrowWith(clientArgs({"--aux-weight", ""}), "--aux-weight");
    ExpectThrowWith(clientArgs({"--aux-weight", "nan"}), "--aux-weight");
    ExpectThrowWith(clientArgs({"--aux-weight", "inf"}), "--aux-weight");

    // ---- aux-weight V-10 (AC-10/FR-12): --large-file-lane parsing ----------------------------
    {
        const fc::CliOptions opt = Parse(clientArgs({"--large-file-lane", "primary"}));
        Require(opt.largeFileLane == fc::LargeFileLane::Primary, "Expected --large-file-lane primary");
    }
    {
        const fc::CliOptions opt = Parse(clientArgs({"--large-file-lane", "aux"}));
        Require(opt.largeFileLane == fc::LargeFileLane::Aux, "Expected --large-file-lane aux");
    }
    {
        const fc::CliOptions opt = Parse(clientArgs({"--large-file-lane", "auto"}));
        Require(opt.largeFileLane == fc::LargeFileLane::Auto, "Expected --large-file-lane auto");
    }
    ExpectThrowWith(clientArgs({"--large-file-lane", "foo"}), "--large-file-lane");
    ExpectThrowWith(clientArgs({"--large-file-lane", ""}), "--large-file-lane");

    // ---- binary delta V-13 (AC-13/FR-01~FR-03): --delta-min-size parsing -----------------------
    // Default 0 (delta disabled, zero regression).
    {
        const fc::CliOptions opt = Parse(clientArgs({}));
        Require(opt.deltaMinSizeBytes == 0, "Expected default --delta-min-size 0");
    }
    // Explicit 0 stays disabled.
    {
        const fc::CliOptions opt = Parse(clientArgs({"--delta-min-size", "0"}));
        Require(opt.deltaMinSizeBytes == 0, "Expected --delta-min-size 0 accepted");
    }
    // Legal value + K/M/G suffix handling.
    {
        const fc::CliOptions opt = Parse(clientArgs({"--delta-min-size", "16M"}));
        Require(opt.deltaMinSizeBytes == 16ULL * 1024 * 1024, "Expected 16M parsed");
    }
    {
        const fc::CliOptions opt = Parse(clientArgs({"--delta-min-size", "1M"}));
        Require(opt.deltaMinSizeBytes == 1ULL * 1024 * 1024, "Expected 1M (lower bound) parsed");
    }
    {
        const fc::CliOptions opt = Parse(clientArgs({"--delta-min-size", "1G"}));
        Require(opt.deltaMinSizeBytes == 1ULL * 1024 * 1024 * 1024, "Expected 1G parsed");
    }
    {
        // 1T upper bound is expressed via the G suffix (ParseSizeBytesStrict supports K|M|G).
        const fc::CliOptions opt = Parse(clientArgs({"--delta-min-size", "1024G"}));
        Require(opt.deltaMinSizeBytes == 1024ULL * 1024 * 1024 * 1024, "Expected 1024G (=1T upper bound) parsed");
    }
    {
        const fc::CliOptions opt = Parse(clientArgs({"--delta-min-size", "2048K"}));
        Require(opt.deltaMinSizeBytes == 2048ULL * 1024, "Expected 2048K parsed");
    }
    // Out-of-range: positive but below 1M, and above 1T.
    ExpectThrowWith(clientArgs({"--delta-min-size", "512K"}), "--delta-min-size");
    ExpectThrowWith(clientArgs({"--delta-min-size", "2048G"}), "--delta-min-size");
    // Malformed / empty / bad suffix.
    ExpectThrowWith(clientArgs({"--delta-min-size", "abc"}), "--delta-min-size");
    ExpectThrowWith(clientArgs({"--delta-min-size", ""}), "--delta-min-size");
    ExpectThrowWith(clientArgs({"--delta-min-size", "5X"}), "--delta-min-size");

    // ---- T-largefile-block-multinic V-13 (AC-13/AC-07): --large-file-block-kb parsing --------
    // Default: flag absent -> mode OFF (opt-in), field keeps the 32 MiB reference value.
    {
        const fc::CliOptions opt = Parse(clientArgs({}));
        Require(!opt.largeFileBlockFlagged, "Expected block mode OFF by default (opt-in)");
        Require(opt.largeFileBlockBytes == 32ULL * 1024 * 1024,
                "Expected default 32 MiB block size");
    }
    // Legal values: lower bound 1024 KiB (1 MiB), 32768 (32 MiB), upper bound 4194304 (4 GiB).
    {
        const fc::CliOptions opt = Parse(clientArgs({"--large-file-block-kb", "1024"}));
        Require(opt.largeFileBlockFlagged, "Expected flag set when given");
        Require(opt.largeFileBlockBytes == 1ULL * 1024 * 1024, "Expected 1024 KiB = 1 MiB");
    }
    {
        const fc::CliOptions opt = Parse(clientArgs({"--large-file-block-kb", "32768"}));
        Require(opt.largeFileBlockFlagged && opt.largeFileBlockBytes == 32ULL * 1024 * 1024,
                "Expected 32768 KiB = 32 MiB");
    }
    {
        const fc::CliOptions opt = Parse(clientArgs({"--large-file-block-kb", "4194304"}));
        Require(opt.largeFileBlockFlagged &&
                    opt.largeFileBlockBytes == 4ULL * 1024 * 1024 * 1024,
                "Expected 4194304 KiB = 4 GiB (upper bound)");
    }
    // Strict rejection (V-13): 0, below/above range, non power-of-two, non-numeric, empty.
    ExpectThrowWith(clientArgs({"--large-file-block-kb", "0"}), "--large-file-block-kb");
    ExpectThrowWith(clientArgs({"--large-file-block-kb", "512"}), "--large-file-block-kb");
    ExpectThrowWith(clientArgs({"--large-file-block-kb", "8388608"}), "--large-file-block-kb");
    ExpectThrowWith(clientArgs({"--large-file-block-kb", "1536"}), "--large-file-block-kb");
    ExpectThrowWith(clientArgs({"--large-file-block-kb", "-1024"}), "--large-file-block-kb");
    ExpectThrowWith(clientArgs({"--large-file-block-kb", "abc"}), "--large-file-block-kb");
    ExpectThrowWith(clientArgs({"--large-file-block-kb", ""}), "--large-file-block-kb");

    // ---- unbuffered-writes V-01/V-02/V-03 (AC-01/AC-02/AC-03): --unbuffered-writes parsing ------
    // V-01 (AC-01): absent -> false (default, zero regression).
    {
        const fc::CliOptions opt = Parse(clientArgs({}));
        Require(!opt.unbufferedWrites, "Expected default --unbuffered-writes off");
    }
    // V-02 (AC-02): present -> true; and repeated occurrences stay idempotent (still true).
    {
        const fc::CliOptions opt = Parse(clientArgs({"--unbuffered-writes"}));
        Require(opt.unbufferedWrites, "Expected --unbuffered-writes on");
    }
    {
        const fc::CliOptions opt =
            Parse(clientArgs({"--unbuffered-writes", "--unbuffered-writes"}));
        Require(opt.unbufferedWrites, "Expected repeated --unbuffered-writes idempotent (on)");
    }
    // V-03 (AC-03): server-side --unbuffered-writes is a client-only CLI error.
    ExpectThrowWith({
                        "FastClone", "server", "--password", "pw", "--unbuffered-writes",
                    },
                    "client-only");

    // ---- optimize-small-file-write-path W-03 (NFR-07 / AC-09): NO public write-worker knob -------
    // Write concurrency is an internal adaptive active cap; the client exposes no --write-workers
    // flag and ignores FASTCLONE_WRITE_WORKERS. These are negative assertions.
    auto setEnv = [](const char* name, const char* val) {
#if defined(_WIN32)
        _putenv_s(name, val);
#else
        setenv(name, val, 1);
#endif
    };
    auto clearEnv = [](const char* name) {
#if defined(_WIN32)
        _putenv_s(name, "");  // empty value removes the variable on Windows
#else
        unsetenv(name);
#endif
    };

    // AC-09: --write-workers is no longer a valid client argument -> it must throw as an unknown
    // argument, and the message must carry the flag text proving it is not accepted anywhere.
    ExpectThrowWith(clientArgs({"--write-workers", "8"}), "write-workers");
    ExpectThrowWith(clientArgs({"--write-workers"}), "write-workers");

    // AC-09: FASTCLONE_WRITE_WORKERS must NOT change parsing behavior (it is ignored). A normal
    // client parses successfully with the env set, and there is no write-worker field to read.
    setEnv("FASTCLONE_WRITE_WORKERS", "12");
    {
        const fc::CliOptions opt = Parse(clientArgs({}));
        Require(opt.mode == fc::Mode::Client, "Expected FASTCLONE_WRITE_WORKERS to be ignored");
    }
    // Even a garbage env value is ignored (no throw, no parse change).
    setEnv("FASTCLONE_WRITE_WORKERS", "not-a-number");
    {
        const fc::CliOptions opt = Parse(clientArgs({}));
        Require(opt.mode == fc::Mode::Client, "Expected invalid FASTCLONE_WRITE_WORKERS ignored");
    }
    clearEnv("FASTCLONE_WRITE_WORKERS");

    // AC-09: the server also rejects --write-workers as an unknown argument (not "client-only").
    ExpectThrowWith({
                        "FastClone", "server", "--password", "pw", "--write-workers", "4",
                    },
                    "Unknown argument");

    // ---- S-03 (AC-26 / FR-17 / NFR-10 / B12): --unbuffered-writes help wording is accurate --------
    // The help text must express an unbuffered write INTENT and name the small-file / unaligned /
    // tail buffered-fallback cases, and must NOT promise that all writes bypass the OS page cache.
    {
        const std::string usage = fc::BuildUsageText();
        Require(usage.find("small file") != std::string::npos,
                "S-03/AC-26: help must mention small-file buffered fallback");
        Require(usage.find("unaligned") != std::string::npos,
                "S-03/AC-26: help must mention unaligned-write buffered fallback");
        Require(usage.find("tail") != std::string::npos,
                "S-03/AC-26: help must mention tail-write buffered fallback");
        Require(usage.find("buffered") != std::string::npos,
                "S-03/AC-26: help must mention the buffered fallback");
        Require(usage.find("all writes bypass OS cache") == std::string::npos,
                "S-03/AC-26: help must NOT promise all writes bypass the OS cache");
    }

    // ---- S-04 (AC-27 / FR-18 / NFR-11 / B13): cli.cpp must not include sync_util.h ---------------
    // Static source scan: the CLI translation unit must carry no unused #include "sync_util.h". The
    // source path is injected by CMake (FASTCLONE_CLI_SRC). Presence of the include is a failure.
#ifdef FASTCLONE_CLI_SRC
    {
        std::ifstream in(FC_STRINGIZE(FASTCLONE_CLI_SRC), std::ios::binary);
        Require(in.good(), "S-04/AC-27: cli.cpp source is readable for the static include scan");
        const std::string src((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
        Require(src.find("#include \"sync_util.h\"") == std::string::npos,
                "S-04/AC-27/B13: cli.cpp must not #include \"sync_util.h\"");
    }
#endif
}
