#include "check_cli.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>

#include "version.h"

namespace fs = std::filesystem;

namespace fc::check {

namespace {

const std::string& ArgAt(const std::vector<std::string>& args, size_t index) {
    if (index >= args.size()) {
        throw std::runtime_error("Missing value for argument");
    }
    return args[index];
}

long ParseLongStrict(const std::string& value, const char* name) {
    if (value.empty()) {
        throw std::runtime_error(std::string(name) + " is empty");
    }
    char* end = nullptr;
    const long parsed = std::strtol(value.c_str(), &end, 10);
    if (end == nullptr || *end != '\0') {
        throw std::runtime_error(std::string("Invalid ") + name);
    }
    return parsed;
}

// Parse host or host:port; use defaultPort when no port is given.
void ParseHostPort(const std::string& input, std::string& host, uint16_t& port, uint16_t defaultPort) {
    const auto pos = input.find(':');
    if (pos == std::string::npos) {
        if (input.empty()) {
            throw std::runtime_error("Invalid --server, host is empty");
        }
        host = input;
        port = defaultPort;
        return;
    }
    host = input.substr(0, pos);
    const std::string portStr = input.substr(pos + 1);
    if (host.empty() || portStr.empty()) {
        throw std::runtime_error("Invalid --server, expected host:port");
    }
    const long parsedPort = ParseLongStrict(portStr, "--server");
    if (parsedPort <= 0 || parsedPort > 65535) {
        throw std::runtime_error("Port out of range");
    }
    port = static_cast<uint16_t>(parsedPort);
}

Mode ParseMode(const std::string& value) {
    if (value == "fast") {
        return Mode::Fast;
    }
    if (value == "strict") {
        return Mode::Strict;
    }
    if (value == "size-only") {
        return Mode::SizeOnly;
    }
    throw std::runtime_error("Invalid --mode (expected fast|strict|size-only): " + value);
}

Format ParseFormat(const std::string& value) {
    if (value == "text") {
        return Format::Text;
    }
    if (value == "json") {
        return Format::Json;
    }
    throw std::runtime_error("Invalid --format (expected text|json): " + value);
}

// --filter: comma-separated subset of DIFF/MISSING/EXTRA/SAME. Once passed, reset all to false then set bits by value.
FilterSet ParseFilter(const std::string& value) {
    FilterSet filter{false, false, false, false};
    size_t start = 0;
    bool any = false;
    while (start <= value.size()) {
        const size_t comma = value.find(',', start);
        const std::string token = (comma == std::string::npos)
                                      ? value.substr(start)
                                      : value.substr(start, comma - start);
        if (!token.empty()) {
            any = true;
            if (token == "DIFF") {
                filter.diff = true;
            } else if (token == "MISSING") {
                filter.missing = true;
            } else if (token == "EXTRA") {
                filter.extra = true;
            } else if (token == "SAME") {
                filter.same = true;
            } else {
                throw std::runtime_error("Invalid --filter value (expected DIFF,MISSING,EXTRA,SAME): " + token);
            }
        }
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }
    if (!any) {
        throw std::runtime_error("Invalid --filter, no category parsed");
    }
    return filter;
}

}  // namespace

void PrintUsage() {
    std::cerr
        << "Usage:\n"
        << "  FastCheck --server <host[:port]> --target <path> --password <pwd>\n"
        << "            [--mode fast|strict|size-only] [--checkers <n>] [--output <file>]\n"
        << "            [--format text|json] [--summary-only] [--filter DIFF,MISSING,EXTRA,SAME]\n"
        << "            [--port <n>]\n"
        << "\n"
        << "  Read-only directory comparison against a running FastClone server. Never transfers,\n"
        << "  deletes, renames, or writes to the target directory (except the --output report).\n"
        << "\n"
        << "  --server        server endpoint (host or host:port), required\n"
        << "  --target        local directory to compare, required\n"
        << "  --password      session password, required\n"
        << "  --mode          compare mode: fast (default), strict, size-only\n"
        << "  --checkers      initial in-flight hash request network window, AIMD-tuned (1..4096, default 32)\n"
        << "  --hash-workers  initial local hash worker count: 0=auto (default), or positive (1..4096)\n"
        << "  --no-diskio-driver  keep parallel hashing but read local files without the disk IO driver\n"
        << "  --output        write full report to file (default: terminal only)\n"
        << "  --format        report format: text (default), json\n"
        << "  --summary-only  emit only the final summary (no per-file lines)\n"
        << "  --filter        per-file listing categories (default DIFF,MISSING,EXTRA)\n"
        << "  --port          default port for --server without one (default 27842)\n"
        << "\n"
        << "  Exit codes: 0=identical, 1=differences, 2=connection/argument error,\n"
        << "              3=local path precondition failed, 4=interrupted (partial report)\n"
        << "  --version / -v / version  print \"FastCheck <version>\" and exit.\n";
}

CheckOptions ParseCheckArgs(const std::vector<std::string>& args) {
    // --version / -v / version: print and exit before any validation. Handled up front
    // so a bare `FastCheck --version` probe never touches Winsock or the network.
    for (const std::string& a : args) {
        if (a == "--version" || a == "-v" || a == "version") {
            std::cout << "FastCheck " << ::fc::kFastCloneVersion << std::endl;
            std::exit(0);
        }
    }

    CheckOptions options;
    std::string serverArg;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (arg == "--server") {
            serverArg = ArgAt(args, ++i);
        } else if (arg == "--target") {
            options.target = ArgAt(args, ++i);
        } else if (arg == "--password") {
            options.password = ArgAt(args, ++i);
        } else if (arg == "--mode") {
            options.mode = ParseMode(ArgAt(args, ++i));
        } else if (arg == "--checkers") {
            const long n = ParseLongStrict(ArgAt(args, ++i), "--checkers");
            if (n < 1 || n > 4096) {
                throw std::runtime_error("Invalid --checkers (range: 1..4096)");
            }
            options.checkers = static_cast<uint32_t>(n);
        } else if (arg == "--hash-workers") {
            // 0 = auto (hardware_concurrency); positive = fixed initial worker count. Negative,
            // non-integer, or missing value fails here, before connecting to the server (AC-08).
            const long n = ParseLongStrict(ArgAt(args, ++i), "--hash-workers");
            if (n < 0 || n > 4096) {
                throw std::runtime_error("Invalid --hash-workers (range: 0..4096, 0=auto)");
            }
            options.hashWorkers = static_cast<uint32_t>(n);
        } else if (arg == "--no-diskio-driver") {
            options.noDiskioDriver = true;
        } else if (arg == "--output") {
            options.output = ArgAt(args, ++i);
        } else if (arg == "--format") {
            options.format = ParseFormat(ArgAt(args, ++i));
        } else if (arg == "--summary-only") {
            options.summaryOnly = true;
        } else if (arg == "--filter") {
            options.filter = ParseFilter(ArgAt(args, ++i));
        } else if (arg == "--port") {
            const long port = ParseLongStrict(ArgAt(args, ++i), "--port");
            if (port <= 0 || port > 65535) {
                throw std::runtime_error("Port out of range");
            }
            options.port = static_cast<uint16_t>(port);
        } else {
            // Unknown argument (including --streams / --chunk-kb): fail before comparison (FR-06/07/11, AC-08).
            throw std::runtime_error("Unknown argument: " + arg);
        }
    }

    if (serverArg.empty()) {
        throw std::runtime_error("--server is required");
    }
    if (options.target.empty()) {
        throw std::runtime_error("--target is required");
    }
    if (options.password.empty()) {
        throw std::runtime_error("--password is required");
    }
    ParseHostPort(serverArg, options.server, options.port, options.port);
    return options;
}

bool CheckLocalPreconditions(const CheckOptions& o) {
    // --target must exist, be a directory, and be enumerable (FR-12/AC-15).
    std::error_code ec;
    const fs::path target(std::filesystem::path(o.target));
    if (!fs::exists(target, ec) || ec) {
        std::cerr << "error: --target does not exist: " << o.target << std::endl;
        return false;
    }
    if (!fs::is_directory(target, ec) || ec) {
        std::cerr << "error: --target is not a directory: " << o.target << std::endl;
        return false;
    }
    {
        fs::directory_iterator probe(target, fs::directory_options::none, ec);
        if (ec) {
            std::cerr << "error: --target is not enumerable: " << o.target << std::endl;
            return false;
        }
    }
    // --output parent directory must exist (not created, FR-12/AC-16).
    if (!o.output.empty()) {
        const fs::path outPath(std::filesystem::path(o.output));
        fs::path parent = outPath.parent_path();
        if (parent.empty()) {
            parent = fs::path(".");
        }
        if (!fs::exists(parent, ec) || ec || !fs::is_directory(parent, ec)) {
            std::cerr << "error: --output parent directory does not exist: " << o.output << std::endl;
            return false;
        }
    }
    return true;
}

}  // namespace fc::check
