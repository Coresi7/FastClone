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

    // ---- T-largefile-block-auto-default V-13~V-18 (AC-13~AC-18): --large-file-block
    // ---- three-state parsing (C-1/C-4: value domain {auto, off, <size>}) -----------------
    // AC-13(a): flag absent -> Auto, keeps the 32 MiB reference block size, intent allowed.
    {
        const fc::CliOptions opt = Parse(clientArgs({}));
        Require(opt.largeFileBlockMode == fc::LargeFileBlockMode::Auto,
                "Expected block mode Auto by default");
        Require(opt.largeFileBlockBytes == 32ULL * 1024 * 1024,
                "Expected default 32 MiB block size");
        Require(opt.largeFileBlockAllowed(), "Expected default intent allowed (Auto, no lane)");
        Require(!opt.largeFileLaneFlagged, "Expected no lane signal by default");
    }
    // AC-13(c): no-value form -> Auto with the default 32 MiB block (NOT force-on).
    {
        const fc::CliOptions opt = Parse(clientArgs({"--large-file-block"}));
        Require(opt.largeFileBlockMode == fc::LargeFileBlockMode::Auto,
                "Expected no-value form classified as Auto (not On)");
        Require(opt.largeFileBlockBytes == 32ULL * 1024 * 1024,
                "Expected default 32 MiB when size omitted");
        Require(opt.largeFileBlockAllowed(), "Expected no-value Auto intent allowed");
    }
    // AC-06/AC-13(c): no-value form followed by another flag (heuristic: next token starts
    // with --) -> still Auto; the following flag is not consumed as a size.
    {
        const fc::CliOptions opt = Parse(clientArgs({"--large-file-block", "--diag"}));
        Require(opt.largeFileBlockMode == fc::LargeFileBlockMode::Auto,
                "Expected no-value form before another flag classified as Auto");
        Require(opt.largeFileBlockBytes == 32ULL * 1024 * 1024,
                "Expected default 32 MiB when size omitted");
        Require(opt.diagnostics, "Expected --diag still parsed (not swallowed as a size)");
    }
    // AC-13(d)/AC-17: explicit "auto" keyword -> Auto (self-documenting, identical to the
    // flag being absent); the keyword is special-cased BEFORE size parsing, so it is never
    // swallowed by the next-token size heuristic.
    {
        const fc::CliOptions opt = Parse(clientArgs({"--large-file-block", "auto"}));
        Require(opt.largeFileBlockMode == fc::LargeFileBlockMode::Auto,
                "Expected explicit auto keyword classified as Auto");
        Require(opt.largeFileBlockBytes == 32ULL * 1024 * 1024,
                "Expected default 32 MiB with explicit auto");
        Require(opt.largeFileBlockAllowed(), "Expected explicit auto intent allowed");
    }
    // AC-13(b)/AC-17: explicit "off" keyword -> Off, intent never allowed; likewise
    // special-cased before size parsing.
    {
        const fc::CliOptions opt = Parse(clientArgs({"--large-file-block", "off"}));
        Require(opt.largeFileBlockMode == fc::LargeFileBlockMode::Off,
                "Expected explicit off keyword classified as Off");
        Require(!opt.largeFileBlockAllowed(), "Expected off intent never allowed");
    }
    // AC-13(e): a <size> value -> On (the only force-on entry, C-2); K|M|G suffix accepted;
    // lower bound 1M, 32M reference, upper bound 4G. (Bare numbers are interpreted as bytes,
    // NOT KiB -- so a value below 1M is rejected.)
    {
        const fc::CliOptions opt = Parse(clientArgs({"--large-file-block", "1M"}));
        Require(opt.largeFileBlockMode == fc::LargeFileBlockMode::On,
                "Expected size form classified as On");
        Require(opt.largeFileBlockBytes == 1ULL * 1024 * 1024, "Expected 1M = 1 MiB");
        Require(opt.largeFileBlockAllowed(), "Expected On intent allowed");
    }
    {
        const fc::CliOptions opt = Parse(clientArgs({"--large-file-block", "32M"}));
        Require(opt.largeFileBlockMode == fc::LargeFileBlockMode::On &&
                    opt.largeFileBlockBytes == 32ULL * 1024 * 1024,
                "Expected 32M = On + 32 MiB (force-on with the default block)");
    }
    {
        const fc::CliOptions opt = Parse(clientArgs({"--large-file-block", "4G"}));
        Require(opt.largeFileBlockMode == fc::LargeFileBlockMode::On &&
                    opt.largeFileBlockBytes == 4ULL * 1024 * 1024 * 1024,
                "Expected 4G = On + 4 GiB (upper bound)");
    }
    // Strict rejection: 0, below/above range, non power-of-two, non-numeric, empty, bad suffix.
    ExpectThrowWith(clientArgs({"--large-file-block", "0"}), "--large-file-block");
    ExpectThrowWith(clientArgs({"--large-file-block", "1024"}), "--large-file-block");
    ExpectThrowWith(clientArgs({"--large-file-block", "512"}), "--large-file-block");
    ExpectThrowWith(clientArgs({"--large-file-block", "8G"}), "--large-file-block");
    ExpectThrowWith(clientArgs({"--large-file-block", "7M"}), "--large-file-block");
    ExpectThrowWith(clientArgs({"--large-file-block", "-1024"}), "--large-file-block");
    ExpectThrowWith(clientArgs({"--large-file-block", "abc"}), "--large-file-block");
    ExpectThrowWith(clientArgs({"--large-file-block", ""}), "--large-file-block");
    ExpectThrowWith(clientArgs({"--large-file-block", "8X"}), "--large-file-block");
    // AC-15: "on" is NOT a legal value -- rejected like an illegal size, and the message must
    // spell out the legal value set {auto, off, <size>}.
    ExpectThrowWith(clientArgs({"--large-file-block", "on"}), "--large-file-block");
    ExpectThrowWith(clientArgs({"--large-file-block", "on"}), "auto|off|<size>");

    // AC-16 (FR-07): repeated --large-file-block occurrences resolve last-one-wins; all of
    // these parse successfully (distinct from the FR-06 off-then-bare-size rejection below).
    {
        const fc::CliOptions opt =
            Parse(clientArgs({"--large-file-block", "8M", "--large-file-block", "off"}));
        Require(opt.largeFileBlockMode == fc::LargeFileBlockMode::Off,
                "Expected last occurrence wins: 8M then off -> Off");
    }
    {
        const fc::CliOptions opt =
            Parse(clientArgs({"--large-file-block", "off", "--large-file-block"}));
        Require(opt.largeFileBlockMode == fc::LargeFileBlockMode::Auto,
                "Expected trailing no-value occurrence wins: off then no-value -> Auto");
        Require(opt.largeFileBlockBytes == 32ULL * 1024 * 1024,
                "Expected Auto to keep the 32 MiB reference block");
    }
    {
        const fc::CliOptions opt =
            Parse(clientArgs({"--large-file-block", "auto", "--large-file-block", "8M"}));
        Require(opt.largeFileBlockMode == fc::LargeFileBlockMode::On,
                "Expected last occurrence wins: auto then 8M -> On");
        Require(opt.largeFileBlockBytes == 8ULL * 1024 * 1024, "Expected On + 8 MiB");
    }
    {
        const fc::CliOptions opt =
            Parse(clientArgs({"--large-file-block", "off", "--large-file-block", "off"}));
        Require(opt.largeFileBlockMode == fc::LargeFileBlockMode::Off,
                "Expected repeated off idempotent (Off)");
    }
    // AC-14 (FR-06/D-04): once `off` consumed its keyword the branch ends; a following bare
    // size token is NOT consumed by the flag and falls to the Unknown argument branch
    // (non-zero exit, message points at the stray token).
    ExpectThrowWith(clientArgs({"--large-file-block", "off", "8M"}), "Unknown argument");
    ExpectThrowWith(clientArgs({"--large-file-block", "off", "8M"}), "8M");

    // AC-18 (FR-17~FR-20, C-3): three-state x lane mutual-exclusion matrix. (The lane value
    // domain is primary|aux|auto; the requirements' shorthand `--large-file-lane 1` maps to
    // `primary` -- the first/primary lane.)
    // (a) Auto (no-value form) + explicit lane -> parses fine; Auto stays observable, the
    //     lane signal is set, and the intent folds to not-allowed at the runtime gate
    //     (C-3 case 1, lane wins).
    {
        const fc::CliOptions opt =
            Parse(clientArgs({"--large-file-block", "--large-file-lane", "primary"}));
        Require(opt.largeFileBlockMode == fc::LargeFileBlockMode::Auto,
                "Expected auto+lane keeps Auto classification");
        Require(opt.largeFileLaneFlagged, "Expected lane signal set");
        Require(!opt.largeFileBlockAllowed(),
                "Expected auto+lane intent folded to not-allowed (lane wins)");
    }
    // (b) Explicit auto + explicit `--large-file-lane auto` (value equals the default) ->
    //     same as (a): the PRESENCE of the lane flag is the exclusion signal (B-16).
    {
        const fc::CliOptions opt =
            Parse(clientArgs({"--large-file-block", "auto", "--large-file-lane", "auto"}));
        Require(opt.largeFileBlockMode == fc::LargeFileBlockMode::Auto,
                "Expected explicit auto + lane auto keeps Auto classification");
        Require(opt.largeFileLaneFlagged, "Expected lane signal set for explicit lane auto");
        Require(!opt.largeFileBlockAllowed(),
                "Expected explicit auto+lane folded to not-allowed (B-16)");
    }
    // (c) On (forced via <size>) + explicit lane -> parse-time rejection naming BOTH flags
    //     (C-3 case 2, D-05).
    ExpectThrowWith(clientArgs({"--large-file-block", "8M", "--large-file-lane", "primary"}),
                    "--large-file-block");
    ExpectThrowWith(clientArgs({"--large-file-block", "8M", "--large-file-lane", "primary"}),
                    "--large-file-lane");
    // (d) Off + explicit lane -> legal combination (C-3 case 3): parses fine, Off kept, the
    //     lane signal is set, and the intent stays not-allowed.
    {
        const fc::CliOptions opt =
            Parse(clientArgs({"--large-file-block", "off", "--large-file-lane", "primary"}));
        Require(opt.largeFileBlockMode == fc::LargeFileBlockMode::Off,
                "Expected off+lane keeps Off classification");
        Require(opt.largeFileLaneFlagged, "Expected lane signal set with off+lane");
        Require(!opt.largeFileBlockAllowed(), "Expected off+lane intent not allowed");
    }

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
