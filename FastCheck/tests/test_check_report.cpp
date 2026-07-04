#include "check_report.h"

#include "compare_phase.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;
using namespace fc::check;

namespace {

void Expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error("test_check_report: " + message);
    }
}

bool Contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

// Build a 2 same + 1 diff + 1 missing + 1 extra scenario (AC-29 count shape + per-file entries).
CheckResult MakeSampleResult() {
    CheckResult r;
    r.mode = Mode::Fast;
    r.durationMs = 5;
    r.partial = false;
    r.counters.same = 2;
    r.counters.diff = 1;
    r.counters.missing = 1;
    r.counters.extraLocal = 1;

    DiffEntry diff;
    diff.type = fc::CompareCategory::Diff;
    diff.path = "a/diff.txt";
    diff.localSize = 10;
    diff.remoteSize = 20;
    diff.hashCompared = true;
    r.entries.push_back(diff);

    DiffEntry missing;
    missing.type = fc::CompareCategory::Missing;
    missing.path = "a/missing.txt";
    missing.remoteSize = 30;  // local missing -> localSize empty
    r.entries.push_back(missing);

    DiffEntry extra;
    extra.type = fc::CompareCategory::Extra;
    extra.path = "a/extra.txt";
    extra.localSize = 40;  // remote missing -> remoteSize empty
    r.entries.push_back(extra);

    DiffEntry same;
    same.type = fc::CompareCategory::Same;
    same.path = "a/same.txt";
    same.localSize = 50;
    same.remoteSize = 50;
    r.entries.push_back(same);

    return r;
}

void TestTextFullReport() {
    const CheckResult r = MakeSampleResult();
    FilterSet f;  // default diff/missing/extra
    std::ostringstream os;
    RenderText(os, r, f, /*summaryOnly=*/false);
    const std::string text = os.str();
    Expect(Contains(text, "Check completed"), "text complete report has 'Check completed' (AC-30)");
    Expect(Contains(text, "[DIFF]"), "text lists DIFF");
    Expect(Contains(text, "[MISSING]"), "text lists MISSING");
    Expect(Contains(text, "[EXTRA]"), "text lists EXTRA");
    Expect(!Contains(text, "[SAME]"), "default filter excludes SAME from listing");
    Expect(Contains(text, "same=2 diff=1 missing=1 extra_local=1"), "summary counts full");
}

void TestTextSummaryOnly() {
    const CheckResult r = MakeSampleResult();
    FilterSet f;
    std::ostringstream os;
    RenderText(os, r, f, /*summaryOnly=*/true);
    const std::string text = os.str();
    Expect(Contains(text, "Check completed"), "summary-only still has summary line");
    Expect(!Contains(text, "[DIFF]"), "summary-only has no per-file DIFF (AC-11)");
    Expect(!Contains(text, "[MISSING]"), "summary-only has no per-file MISSING");
    Expect(!Contains(text, "[EXTRA]"), "summary-only has no per-file EXTRA");
}

void TestFilterDiffOnly() {
    const CheckResult r = MakeSampleResult();
    FilterSet f{true, false, false, false};  // DIFF only
    std::ostringstream os;
    RenderText(os, r, f, /*summaryOnly=*/false);
    const std::string text = os.str();
    Expect(Contains(text, "[DIFF]"), "filter DIFF lists DIFF");
    Expect(!Contains(text, "[MISSING]"), "filter DIFF hides MISSING (AC-12)");
    Expect(!Contains(text, "[EXTRA]"), "filter DIFF hides EXTRA (AC-12)");
    // The summary still shows all three category counts (FR-21).
    Expect(Contains(text, "diff=1 missing=1 extra_local=1"), "summary still shows all counts (AC-12)");
}

void TestFilterSame() {
    const CheckResult r = MakeSampleResult();
    FilterSet f{false, false, false, true};  // SAME only
    std::ostringstream os;
    RenderText(os, r, f, /*summaryOnly=*/false);
    Expect(Contains(os.str(), "[SAME]"), "filter SAME lists SAME entry (AC-13)");
}

void TestJsonReport() {
    const CheckResult r = MakeSampleResult();
    FilterSet f{true, true, true, false};
    std::ostringstream os;
    RenderJson(os, r, f, /*summaryOnly=*/false);
    const std::string json = os.str();
    Expect(Contains(json, "\"summary\""), "json has summary object");
    Expect(Contains(json, "\"differences\""), "json has differences array");
    Expect(Contains(json, "\"mode\": \"fast\""), "json mode = fast (AC-31)");
    Expect(Contains(json, "\"total_compared\": 4"), "json total_compared = same+diff+missing = 4");
    Expect(Contains(json, "\"partial\": false"), "json partial=false for complete run");
    Expect(Contains(json, "\"type\": \"DIFF\""), "json diff entry type DIFF");
    // The missing side's size is null (FR-24/AC-31).
    Expect(Contains(json, "\"local_size\": null"), "missing entry local_size null");
    Expect(Contains(json, "\"remote_size\": null"), "extra entry remote_size null");
}

void TestPartialText() {
    CheckResult r = MakeSampleResult();
    r.partial = true;
    FilterSet f;
    std::ostringstream os;
    RenderText(os, r, f, /*summaryOnly=*/true);
    const std::string text = os.str();
    Expect(Contains(text, "[PARTIAL]"), "partial text has [PARTIAL] (AC-21)");
    Expect(!Contains(text, "Check completed"), "partial must NOT say Check completed (NFR-07)");

    std::ostringstream osj;
    RenderJson(osj, r, f, /*summaryOnly=*/true);
    Expect(Contains(osj.str(), "\"partial\": true"), "partial json summary.partial=true (AC-21)");
}

// G2: --output write-to-disk path. The terminal gets only the summary line; the file gets the full report (per format); write failure returns non-zero.
fs::path MakeTempReportDir() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path dir = fs::temp_directory_path() / ("fastclone_rep_" + std::to_string(stamp));
    fs::create_directories(dir);
    return dir;
}

void TestWriteReportTextToFile() {
    const fs::path dir = MakeTempReportDir();
    const fs::path outFile = dir / "report.txt";
    const CheckResult r = MakeSampleResult();
    CheckOptions o;
    o.output = outFile.string();
    o.format = Format::Text;
    o.filter = FilterSet{};  // default diff/missing/extra

    // Captured terminal (cout): with --output, the terminal shows only the summary line and no per-file [DIFF] lines.
    std::ostringstream captured;
    std::streambuf* oldCout = std::cout.rdbuf(captured.rdbuf());
    const int rc = WriteReport(o, r);
    std::cout.rdbuf(oldCout);

    Expect(rc == 0, "WriteReport(text, --output) returns 0 (G2)");
    Expect(Contains(captured.str(), "Check completed"), "terminal still gets summary line (G2)");
    Expect(!Contains(captured.str(), "[DIFF]"), "terminal has no per-file lines when --output set (G2)");

    // The file gets the full text report (including per-file lines).
    std::ifstream in(outFile, std::ios::binary);
    Expect(in.good(), "report file created (G2)");
    std::ostringstream fileContent;
    fileContent << in.rdbuf();
    const std::string fc = fileContent.str();
    Expect(Contains(fc, "Check completed"), "file report has summary (G2)");
    Expect(Contains(fc, "[DIFF]"), "file report has per-file DIFF line (G2)");
    Expect(Contains(fc, "[MISSING]"), "file report has MISSING line (G2)");

    std::error_code ec;
    fs::remove_all(dir, ec);
}

void TestWriteReportJsonToFile() {
    const fs::path dir = MakeTempReportDir();
    const fs::path outFile = dir / "report.json";
    const CheckResult r = MakeSampleResult();
    CheckOptions o;
    o.output = outFile.string();
    o.format = Format::Json;
    o.filter = FilterSet{true, true, true, false};

    std::ostringstream captured;
    std::streambuf* oldCout = std::cout.rdbuf(captured.rdbuf());
    const int rc = WriteReport(o, r);
    std::cout.rdbuf(oldCout);

    Expect(rc == 0, "WriteReport(json, --output) returns 0 (G2)");
    // The terminal summary is in text form (by design: with --output the terminal is always a text summary).
    Expect(Contains(captured.str(), "Check completed"), "terminal gets text summary even for json --output (G2)");

    std::ifstream in(outFile, std::ios::binary);
    Expect(in.good(), "json report file created (G2)");
    std::ostringstream fileContent;
    fileContent << in.rdbuf();
    const std::string fc = fileContent.str();
    Expect(Contains(fc, "\"summary\""), "json file has summary object (G2)");
    Expect(Contains(fc, "\"differences\""), "json file has differences array (G2)");
    Expect(Contains(fc, "\"type\": \"DIFF\""), "json file has DIFF entry (G2)");

    std::error_code ec;
    fs::remove_all(dir, ec);
}

void TestWriteReportNoOutputGoesToCout() {
    // No --output: the full report goes to cout (per format). Here we verify the full text goes to cout.
    const fs::path dir = MakeTempReportDir();
    const CheckResult r = MakeSampleResult();
    CheckOptions o;
    o.output = "";
    o.format = Format::Text;
    o.filter = FilterSet{};

    std::ostringstream captured;
    std::streambuf* oldCout = std::cout.rdbuf(captured.rdbuf());
    const int rc = WriteReport(o, r);
    std::cout.rdbuf(oldCout);

    Expect(rc == 0, "WriteReport(no --output) returns 0 (G2)");
    Expect(Contains(captured.str(), "Check completed"), "no --output: cout gets summary (G2)");
    Expect(Contains(captured.str(), "[DIFF]"), "no --output: cout gets per-file lines (G2)");

    std::error_code ec;
    fs::remove_all(dir, ec);
}

}  // namespace

void RunCheckReportTests() {
    TestTextFullReport();
    TestTextSummaryOnly();
    TestFilterDiffOnly();
    TestFilterSame();
    TestJsonReport();
    TestPartialText();
    TestWriteReportTextToFile();
    TestWriteReportJsonToFile();
    TestWriteReportNoOutputGoesToCout();
}
