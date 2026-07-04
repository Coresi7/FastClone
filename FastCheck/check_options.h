#pragma once

// FastCheck CLI options and exit codes (fastcheck). Independent of sync's CliOptions; exit codes 0-4 are check-specific,
// one value one meaning (M12/FR-12/13/14/15), so scripts can branch on the exit code alone.

#include <cstdint>
#include <string>

namespace fc::check {

// Compare mode (FR-05). Default Fast.
enum class Mode { Fast, Strict, SizeOnly };

// Report format (FR-07). Default Text.
enum class Format { Text, Json };

// Per-file listing filter bit set (FR-10). By default lists DIFF/MISSING/EXTRA, not SAME.
struct FilterSet {
    bool diff = true;
    bool missing = true;
    bool extra = true;
    bool same = false;
};

struct CheckOptions {
    std::string server;         // host (no port), required (FR-04)
    uint16_t port = 27842;      // default port, same as the main program
    std::string target;         // local directory, required (FR-04)
    std::string password;       // required (FR-04)
    Mode mode = Mode::Fast;
    uint32_t checkers = 8;      // --checkers: in-flight HashRequest cap within a single connection, positive integer (FR-06)
    std::string output;         // empty=terminal only (FR-08)
    Format format = Format::Text;
    bool summaryOnly = false;   // FR-09
    FilterSet filter;           // FR-10
};

// check-specific exit codes (M12). Does not reuse sync's kExit*.
enum ExitCode {
    kIdentical = 0,           // full comparison: both sides identical
    kDiffFound = 1,           // full comparison: differences exist
    kConnFailed = 2,          // connection/handshake/auth failure, mid-comparison disconnect, parameter error
    kLocalPrecondFailed = 3,  // local target/output precondition failure, local file read failure
    kInterrupted = 4          // user interrupt (Ctrl+C), report marked [PARTIAL]
};

}  // namespace fc::check
