// Source-level sharing gate (task unify-probe-extra-shared, design §8.2.3; FR-12 /
// AC-05, form A). Scans the two former probe/extra implementations for RESIDUAL
// platform primitives that would indicate a second, non-shared implementation:
//   * GetFileAttributesExW            (Windows per-file metadata probe primitive)
//   * time_since_epoch().count()      (raw file_clock ticks - FR-02 banned pattern)
//   * FindFirstFileW                  (local directory enumeration primitive)
// A file that cannot be opened is a FAIL (not a SKIP) so a wrong FASTCLONE_SOURCE_DIR
// cannot silently neutralize the gate. No exclusion list is currently needed: after the
// unify-probe-extra-shared refactor both files are clean of all three tokens (comments
// included); if a legitimate future use appears, it must be registered here as an
// explicit exclusion with a line reference and re-reviewed (R-07) - never a silent
// relaxation.

#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void Expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error("test_shared_source_gate: " + message);
    }
}

std::string ReadWholeFile(const char* macroPath, const std::string& label) {
    std::ifstream in(macroPath, std::ios::binary);
    if (!in) {
        throw std::runtime_error("test_shared_source_gate: cannot open " + label + " at \"" +
                                 std::string(macroPath) + "\" (FAIL, not SKIP - fix the build macro)");
    }
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void ExpectNoToken(const std::string& content, const std::string& token, const std::string& fileLabel) {
    const size_t pos = content.find(token);
    if (pos != std::string::npos) {
        // Count the line for the failure message to speed up triage.
        size_t line = 1;
        for (size_t i = 0; i < pos && i < content.size(); ++i) {
            if (content[i] == '\n') {
                ++line;
            }
        }
        throw std::runtime_error("test_shared_source_gate: banned token \"" + token +
                                 "\" found in " + fileLabel + " near line " + std::to_string(line) +
                                 " (residual non-shared probe/enumeration primitive, AC-05)");
    }
}

// Positive pin (design D-02 / AC-09 / B-07, divergence point C deliberately NOT unified):
// the two failure-handling behaviors must each keep existing. If either token disappears,
// the two sides' read-failure semantics have drifted and this gate must fail.
void ExpectToken(const std::string& content, const std::string& token, const std::string& fileLabel,
                 const std::string& why) {
    if (content.find(token) == std::string::npos) {
        throw std::runtime_error("test_shared_source_gate: required token \"" + token +
                                 "\" missing from " + fileLabel + " (" + why + ")");
    }
}

}  // namespace

namespace fc::test {

void RunSharedSourceGateTests() {
#ifndef FASTCLONE_SOURCE_DIR
    throw std::runtime_error(
        "test_shared_source_gate: FASTCLONE_SOURCE_DIR is not defined - the gate cannot "
        "locate the sources (build misconfiguration, FAIL not SKIP)");
#else
    const std::string clientSrc =
        ReadWholeFile(FASTCLONE_SOURCE_DIR "/FastClone/sync_engine_client.cpp",
                      "FastClone/sync_engine_client.cpp");
    const std::string checkSrc = ReadWholeFile(FASTCLONE_SOURCE_DIR "/FastCheck/check_engine.cpp",
                                               "FastCheck/check_engine.cpp");

    const std::vector<std::string> bannedTokens = {
        "GetFileAttributesExW",
        "time_since_epoch().count()",
        "FindFirstFileW",
    };
    for (const std::string& token : bannedTokens) {
        ExpectNoToken(clientSrc, token, "FastClone/sync_engine_client.cpp");
        ExpectNoToken(checkSrc, token, "FastCheck/check_engine.cpp");
    }

    // AC-09 positive pins (divergence point C, B-07 - intentionally NOT unified, D-02):
    //   * FastCheck: a local read failure aborts the whole run with kLocalPrecondFailed=3
    //     (behaviorally pinned by FastCheck/tests/test_check_engine.cpp's TOCTOU case).
    //   * FastClone: a failed file degrades per-file through retryOrFail, never aborting
    //     the whole run.
    ExpectToken(checkSrc, "kLocalPrecondFailed", "FastCheck/check_engine.cpp",
                "AC-09: FastCheck must keep the whole-run abort exit code 3");
    ExpectToken(clientSrc, "retryOrFail(", "FastClone/sync_engine_client.cpp",
                "AC-09: FastClone must keep the per-file retryOrFail degradation");
#endif
}

}  // namespace fc::test
