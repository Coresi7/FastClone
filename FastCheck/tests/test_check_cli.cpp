#include "check_cli.h"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace fc::check;

namespace {

void Expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error("test_check_cli: " + message);
    }
}

bool ParseThrows(const std::vector<std::string>& args) {
    try {
        ParseCheckArgs(args);
        return false;
    } catch (const std::exception&) {
        return true;
    }
}

fs::path MakeTempDir() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path dir = fs::temp_directory_path() / ("fastclone_cli_" + std::to_string(stamp));
    fs::create_directories(dir);
    return dir;
}

std::vector<std::string> BaseArgs(const std::string& target) {
    return {"--server", "host", "--target", target, "--password", "p"};
}

void TestDefaults() {
    const fs::path dir = MakeTempDir();
    const CheckOptions o = ParseCheckArgs(BaseArgs(dir.string()));
    Expect(o.server == "host", "server parsed");
    Expect(o.port == 27842, "default port 27842");
    Expect(o.mode == Mode::Fast, "default mode fast (AC-05)");
    Expect(o.format == Format::Text, "default format text (AC-09)");
    Expect(o.checkers == 8, "default checkers 8");
    Expect(!o.summaryOnly, "default summary-only false");
    Expect(o.filter.diff && o.filter.missing && o.filter.extra && !o.filter.same,
           "default filter DIFF,MISSING,EXTRA (FR-10)");
    std::error_code ec;
    fs::remove_all(dir, ec);
}

void TestRequiredMissing() {
    // 缺 --server / --target / --password 任一 -> 抛（AC-04）。
    Expect(ParseThrows({"--target", "t", "--password", "p"}), "missing --server throws");
    Expect(ParseThrows({"--server", "h", "--password", "p"}), "missing --target throws");
    Expect(ParseThrows({"--server", "h", "--target", "t"}), "missing --password throws");
}

void TestModeValidation() {
    const std::string t = "t";
    CheckOptions o = ParseCheckArgs({"--server", "h", "--target", t, "--password", "p", "--mode", "strict"});
    Expect(o.mode == Mode::Strict, "--mode strict accepted");
    o = ParseCheckArgs({"--server", "h", "--target", t, "--password", "p", "--mode", "size-only"});
    Expect(o.mode == Mode::SizeOnly, "--mode size-only accepted");
    Expect(ParseThrows({"--server", "h", "--target", t, "--password", "p", "--mode", "deep"}),
           "--mode deep rejected (AC-06)");
}

void TestFormatValidation() {
    const std::string t = "t";
    CheckOptions o = ParseCheckArgs({"--server", "h", "--target", t, "--password", "p", "--format", "json"});
    Expect(o.format == Format::Json, "--format json accepted");
    Expect(ParseThrows({"--server", "h", "--target", t, "--password", "p", "--format", "xml"}),
           "--format xml rejected (AC-09)");
}

void TestCheckersValidation() {
    const std::string t = "t";
    const CheckOptions o = ParseCheckArgs({"--server", "h", "--target", t, "--password", "p", "--checkers", "1"});
    Expect(o.checkers == 1, "--checkers 1 accepted (AC-07)");
    Expect(ParseThrows({"--server", "h", "--target", t, "--password", "p", "--checkers", "0"}),
           "--checkers 0 rejected (AC-07)");
    Expect(ParseThrows({"--server", "h", "--target", t, "--password", "p", "--checkers", "abc"}),
           "--checkers abc rejected (AC-07)");
}

void TestStreamsRejected() {
    const std::string t = "t";
    Expect(ParseThrows({"--server", "h", "--target", t, "--password", "p", "--streams", "4"}),
           "--streams rejected as unknown arg (AC-08)");
    // usage 文本不出现 --streams（AC-08）。
    std::ostringstream captured;
    std::streambuf* old = std::cerr.rdbuf(captured.rdbuf());
    PrintUsage();
    std::cerr.rdbuf(old);
    Expect(captured.str().find("--streams") == std::string::npos, "usage must not mention --streams (AC-08)");
    Expect(captured.str().find("--checkers") != std::string::npos, "usage mentions --checkers");
}

void TestFilterParsing() {
    const std::string t = "t";
    CheckOptions o = ParseCheckArgs({"--server", "h", "--target", t, "--password", "p", "--filter", "DIFF"});
    Expect(o.filter.diff && !o.filter.missing && !o.filter.extra && !o.filter.same,
           "--filter DIFF sets only diff");
    o = ParseCheckArgs({"--server", "h", "--target", t, "--password", "p", "--filter", "SAME"});
    Expect(o.filter.same && !o.filter.diff, "--filter SAME sets same");
    o = ParseCheckArgs({"--server", "h", "--target", t, "--password", "p", "--filter", "DIFF,EXTRA"});
    Expect(o.filter.diff && o.filter.extra && !o.filter.missing && !o.filter.same, "--filter DIFF,EXTRA");
    Expect(ParseThrows({"--server", "h", "--target", t, "--password", "p", "--filter", "BOGUS"}),
           "--filter unknown value rejected");
}

void TestServerPort() {
    const std::string t = "t";
    const CheckOptions o = ParseCheckArgs({"--server", "example:9000", "--target", t, "--password", "p"});
    Expect(o.server == "example" && o.port == 9000, "--server host:port parsed");
}

void TestSummaryOnlyFlag() {
    const std::string t = "t";
    const CheckOptions o = ParseCheckArgs({"--server", "h", "--target", t, "--password", "p", "--summary-only"});
    Expect(o.summaryOnly, "--summary-only sets flag");
}

void TestPreconditions() {
    // target 不存在 -> false（AC-15）。
    CheckOptions bad;
    bad.target = (fs::temp_directory_path() / "fastclone_nonexistent_xyz_123").string();
    Expect(!CheckLocalPreconditions(bad), "nonexistent target -> precondition fail");

    // target 存在且为目录 -> true。
    const fs::path dir = MakeTempDir();
    CheckOptions ok;
    ok.target = dir.string();
    Expect(CheckLocalPreconditions(ok), "existing dir target -> precondition ok");

    // output 父目录不存在 -> false（AC-16）。
    CheckOptions badOut;
    badOut.target = dir.string();
    badOut.output = (fs::temp_directory_path() / "no_such_dir_abc" / "out.json").string();
    Expect(!CheckLocalPreconditions(badOut), "nonexistent --output parent -> precondition fail");

    // output 父目录存在 -> true。
    CheckOptions okOut;
    okOut.target = dir.string();
    okOut.output = (dir / "report.json").string();
    Expect(CheckLocalPreconditions(okOut), "existing --output parent -> precondition ok");

    std::error_code ec;
    fs::remove_all(dir, ec);
}

}  // namespace

void RunCheckCliTests() {
    TestDefaults();
    TestRequiredMissing();
    TestModeValidation();
    TestFormatValidation();
    TestCheckersValidation();
    TestStreamsRejected();
    TestFilterParsing();
    TestServerPort();
    TestSummaryOnlyFlag();
    TestPreconditions();
}
