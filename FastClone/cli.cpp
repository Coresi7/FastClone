#include "cli.h"

#ifdef _WIN32
#include <Windows.h>
#endif

#include <algorithm>
#include <cstdlib>
#include <cctype>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace fc {

namespace {

#ifdef _WIN32
std::string ToUtf8(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }
    const int len = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (len <= 0) {
        throw std::runtime_error("WideCharToMultiByte failed");
    }
    std::string output(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), output.data(), len, nullptr, nullptr);
    return output;
}
#endif

const std::string& ArgAt(const std::vector<std::string>& args, size_t index) {
    if (index >= args.size()) {
        throw std::runtime_error("Missing value for argument");
    }
    return args[index];
}

void PrintUsage() {
    std::cerr
        << "Usage:\n"
        << "  fastclone server [--dir <path>] [--port <n>] [--server-hash-workers <n>] [--enable-hash-memcache] [--once] --password <pwd>\n"
        << "  fastclone client --server <host:port>[,host:port...] --target <path> --password <pwd>\n"
        << "      [--streams <n>] [--chunk-kb <n>] [--queued-file-size <size>]\n"
        << "      [--large-file-threshold <size>] [--link <localIP|iface>=<serverIP[:port]>]...\n"
        << "      [--reconnect-retries <n>] [--reconnect-window <duration>] [--diag]\n"
        << "  (When --streams or --chunk-kb is omitted, FastClone auto-tunes that parameter.)\n"
        << "  --server accepts a comma-separated list and/or may be repeated (multipath endpoints).\n"
        << "  --large-file-threshold pins files >= <size> to the primary link (default 1G, suffix K|M|G).\n"
        << "  --link forces an explicit source->server pairing; the first --link is the primary link.\n";
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

uint64_t ParseSizeBytesStrict(const std::string& value, const char* name) {
    if (value.empty()) {
        throw std::runtime_error(std::string(name) + " is empty");
    }
    size_t idx = 0;
    unsigned long long base = 0;
    try {
        base = std::stoull(value, &idx, 10);
    } catch (const std::exception&) {
        // std::stoull throws raw std::invalid_argument / std::out_of_range on non-numeric or
        // overflowing input; surface a name-tagged error instead (matches ParseLongStrict).
        throw std::runtime_error(std::string("Invalid ") + name);
    }
    std::string suffix = value.substr(idx);
    for (char& c : suffix) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    uint64_t multiplier = 1;
    if (suffix.empty() || suffix == "b") {
        multiplier = 1;
    } else if (suffix == "k" || suffix == "kb") {
        multiplier = 1024ULL;
    } else if (suffix == "m" || suffix == "mb") {
        multiplier = 1024ULL * 1024ULL;
    } else if (suffix == "g" || suffix == "gb") {
        multiplier = 1024ULL * 1024ULL * 1024ULL;
    } else {
        throw std::runtime_error(std::string("Invalid ") + name + ", expected suffix [K|M|G]");
    }

    const unsigned long long maxValue = (std::numeric_limits<uint64_t>::max)() / multiplier;
    if (base > maxValue) {
        throw std::runtime_error(std::string(name) + " is too large");
    }
    return static_cast<uint64_t>(base * multiplier);
}

uint64_t ParseDurationMsStrict(const std::string& value, const char* name) {
    if (value.empty()) {
        throw std::runtime_error(std::string(name) + " is empty");
    }
    size_t idx = 0;
    unsigned long long base = 0;
    try {
        base = std::stoull(value, &idx, 10);
    } catch (const std::exception&) {
        // std::stoull throws raw std::invalid_argument / std::out_of_range on non-numeric or
        // overflowing input; surface a name-tagged error instead (matches ParseLongStrict).
        throw std::runtime_error(std::string("Invalid ") + name);
    }
    std::string suffix = value.substr(idx);
    for (char& c : suffix) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    uint64_t multiplier = 1;
    // Bare number (no suffix) is treated as seconds, same as an explicit "s" suffix.
    if (suffix.empty() || suffix == "s" || suffix == "sec") {
        multiplier = 1000ULL;
    } else if (suffix == "m" || suffix == "min") {
        multiplier = 60ULL * 1000ULL;
    } else if (suffix == "h" || suffix == "hr") {
        multiplier = 60ULL * 60ULL * 1000ULL;
    } else {
        throw std::runtime_error(std::string("Invalid ") + name + ", expected suffix [s|m|h]");
    }

    const unsigned long long maxValue = (std::numeric_limits<uint64_t>::max)() / multiplier;
    if (base > maxValue) {
        throw std::runtime_error(std::string(name) + " is too large");
    }
    return static_cast<uint64_t>(base * multiplier);
}

std::pair<std::string, uint16_t> ParseHostPort(const std::string& input, uint16_t defaultPort) {
    const auto pos = input.find(':');
    if (pos == std::string::npos) {
        if (input.empty()) {
            throw std::runtime_error("Invalid --server, host is empty");
        }
        return {input, defaultPort};
    }
    const std::string host = input.substr(0, pos);
    const std::string port = input.substr(pos + 1);
    if (host.empty() || port.empty()) {
        throw std::runtime_error("Invalid --server, expected host:port");
    }
    const long parsedPort = ParseLongStrict(port, "--server");
    if (parsedPort <= 0 || parsedPort > 65535) {
        throw std::runtime_error("Port out of range");
    }
    return {host, static_cast<uint16_t>(parsedPort)};
}

// Split a comma-separated endpoint list ("A:port,B,C:port") into host/port pairs,
// each parsed via ParseHostPort with the current default port (FR-005 / design §9.2).
std::vector<std::pair<std::string, uint16_t>> ParseServerList(const std::string& input,
                                                              uint16_t defaultPort) {
    std::vector<std::pair<std::string, uint16_t>> endpoints;
    size_t start = 0;
    while (start <= input.size()) {
        const size_t comma = input.find(',', start);
        const std::string token = (comma == std::string::npos)
                                      ? input.substr(start)
                                      : input.substr(start, comma - start);
        if (!token.empty()) {
            endpoints.push_back(ParseHostPort(token, defaultPort));
        }
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }
    if (endpoints.empty()) {
        throw std::runtime_error("Invalid --server, no endpoints parsed");
    }
    return endpoints;
}

// Parse "<localIP|iface>=<serverIP[:port]>" into a LinkPin (FR-008 / design §9.3).
LinkPin ParseLinkPin(const std::string& input, uint16_t defaultPort) {
    const size_t eq = input.find('=');
    if (eq == std::string::npos || eq == 0 || eq + 1 >= input.size()) {
        throw std::runtime_error("Invalid --link, expected <localIP|iface>=<serverIP[:port]>");
    }
    LinkPin pin;
    pin.local = input.substr(0, eq);
    const auto serverHostPort = ParseHostPort(input.substr(eq + 1), defaultPort);
    pin.server = serverHostPort.first;
    pin.port = serverHostPort.second;
    return pin;
}

CliOptions ParseCliArgs(const std::vector<std::string>& args) {
    if (args.empty()) {
        PrintUsage();
        throw std::runtime_error("Mode is required");
    }

    CliOptions options;

    if (args[0] == "server") {
        options.mode = Mode::Server;
        options.rootDir = fs::current_path();
    } else if (args[0] == "client") {
        options.mode = Mode::Client;
    } else {
        PrintUsage();
        throw std::runtime_error("First argument must be server or client");
    }

    for (size_t i = 1; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (arg == "--dir") {
            options.rootDir = fs::path(ArgAt(args, ++i));
        } else if (arg == "--target") {
            options.rootDir = fs::path(ArgAt(args, ++i));
        } else if (arg == "--port") {
            const long port = ParseLongStrict(ArgAt(args, ++i), "--port");
            if (port <= 0 || port > 65535) {
                throw std::runtime_error("Port out of range");
            }
            options.port = static_cast<uint16_t>(port);
        } else if (arg == "--server") {
            auto endpoints = ParseServerList(ArgAt(args, ++i), options.port);
            // Accumulate across repeated --server flags (FR-005), de-duplicating exact
            // host:port repeats while preserving first-seen order.
            for (auto& ep : endpoints) {
                const bool dup = std::any_of(options.servers.begin(), options.servers.end(),
                                             [&](const auto& e) { return e == ep; });
                if (!dup) {
                    options.servers.push_back(ep);
                }
            }
            // host/port mirror servers[0] for call-site compatibility.
            options.host = options.servers.front().first;
            options.port = options.servers.front().second;
        } else if (arg == "--large-file-threshold") {
            const uint64_t sizeBytes = ParseSizeBytesStrict(ArgAt(args, ++i), "--large-file-threshold");
            constexpr uint64_t kMin = 1ULL * 1024ULL * 1024ULL;            // 1M lower bound
            constexpr uint64_t kMax = 1024ULL * 1024ULL * 1024ULL * 1024ULL;  // 1T upper bound
            if (sizeBytes < kMin || sizeBytes > kMax) {
                throw std::runtime_error("Invalid --large-file-threshold (range: 1M..1T)");
            }
            options.largeFileThresholdBytes = sizeBytes;
        } else if (arg == "--link") {
            options.linkPins.push_back(ParseLinkPin(ArgAt(args, ++i), options.port));
        } else if (arg == "--password") {
            options.password = ArgAt(args, ++i);
        } else if (arg == "--streams") {
            const long streams = ParseLongStrict(ArgAt(args, ++i), "--streams");
            if (streams <= 0 || streams > 1024) {
                throw std::runtime_error("Invalid --streams");
            }
            options.streamLimit = static_cast<uint32_t>(streams);
            options.streamAutoTune = false;
        } else if (arg == "--chunk-kb") {
            const long chunkKb = ParseLongStrict(ArgAt(args, ++i), "--chunk-kb");
            if (chunkKb <= 0 || chunkKb > 65536) {
                throw std::runtime_error("Invalid --chunk-kb");
            }
            options.chunkSize = static_cast<uint32_t>(chunkKb * 1024);
            options.chunkAutoTune = false;
        } else if (arg == "--queued-file-size") {
            const uint64_t sizeBytes = ParseSizeBytesStrict(ArgAt(args, ++i), "--queued-file-size");
            constexpr uint64_t kMin = 256ULL * 1024ULL * 1024ULL;
            constexpr uint64_t kMax = 64ULL * 1024ULL * 1024ULL * 1024ULL;
            if (sizeBytes < kMin || sizeBytes > kMax) {
                throw std::runtime_error("Invalid --queued-file-size (range: 256M..64G)");
            }
            options.queuedFileSizeBytes = sizeBytes;
        } else if (arg == "--server-hash-workers") {
            const long workers = ParseLongStrict(ArgAt(args, ++i), "--server-hash-workers");
            if (workers < 0 || workers > 512) {
                throw std::runtime_error("Invalid --server-hash-workers (range: 0..512)");
            }
            options.serverHashWorkers = static_cast<uint32_t>(workers);
        } else if (arg == "--enable-hash-memcache") {
            options.enableHashMemcache = true;
        } else if (arg == "--once") {
            options.exitAfterSync = true;
        } else if (arg == "--diag") {
            options.diagnostics = true;
        } else if (arg == "--reconnect-retries") {
            const long retries = ParseLongStrict(ArgAt(args, ++i), "--reconnect-retries");
            if (retries < 0 || retries > 1000000) {
                throw std::runtime_error("Invalid --reconnect-retries");
            }
            options.reconnectRetries = static_cast<uint32_t>(retries);
        } else if (arg == "--reconnect-window") {
            options.reconnectWindowMs = ParseDurationMsStrict(ArgAt(args, ++i), "--reconnect-window");
            if (options.reconnectWindowMs == 0) {
                throw std::runtime_error("Invalid --reconnect-window (must be > 0)");
            }
        } else {
            throw std::runtime_error("Unknown argument: " + arg);
        }
    }

    if (options.password.empty()) {
        throw std::runtime_error("--password is required");
    }
    if (options.rootDir.empty()) {
        throw std::runtime_error("Directory path is required");
    }
    if (options.mode == Mode::Client && options.serverHashWorkers != 0) {
        throw std::runtime_error("--server-hash-workers is server-only");
    }
    if (options.mode == Mode::Client && options.enableHashMemcache) {
        throw std::runtime_error("--enable-hash-memcache is server-only");
    }
    if (options.mode == Mode::Client && options.exitAfterSync) {
        throw std::runtime_error("--once is server-only");
    }
    if (options.exitAfterSync && options.enableHashMemcache) {
        throw std::runtime_error("--once and --enable-hash-memcache are mutually exclusive");
    }
    // Normalize the endpoint list so the engine always sees servers[0] == host/port.
    if (options.servers.empty()) {
        options.servers.push_back({options.host, options.port});
    }
    options.rootDir = fs::weakly_canonical(options.rootDir);
    return options;
}

}  // namespace

#if defined(_WIN32) && defined(_MSC_VER)
CliOptions ParseCli(int argc, wchar_t** argv) {
    if (argc < 2) {
        PrintUsage();
        throw std::runtime_error("Mode is required");
    }

    std::vector<std::string> args;
    args.reserve(static_cast<size_t>(argc - 1));
    for (int i = 1; i < argc; ++i) {
        args.push_back(ToUtf8(argv[i]));
    }
    return ParseCliArgs(args);
}
#else
CliOptions ParseCli(int argc, char** argv) {
    if (argc < 2) {
        PrintUsage();
        throw std::runtime_error("Mode is required");
    }

    std::vector<std::string> args;
    args.reserve(static_cast<size_t>(argc - 1));
    for (int i = 1; i < argc; ++i) {
        args.emplace_back(argv[i] == nullptr ? "" : argv[i]);
    }
    return ParseCliArgs(args);
}
#endif

}  // namespace fc
