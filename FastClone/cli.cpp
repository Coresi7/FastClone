#include "cli.h"

#ifdef _WIN32
#include <Windows.h>
#endif

#include <cstdlib>
#include <cctype>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
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
        << "  fastclone server [--dir <path>] [--port <n>] --password <pwd>\n"
        << "  fastclone client --server <host:port> --target <path> --password <pwd> [--streams <n>] [--chunk-kb <n>] [--queued-file-size <size>]\n"
        << "  (When --streams or --chunk-kb is omitted, FastClone auto-tunes that parameter.)\n";
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
    const unsigned long long base = std::stoull(value, &idx, 10);
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
            auto hostPort = ParseHostPort(ArgAt(args, ++i), options.port);
            options.host = hostPort.first;
            options.port = hostPort.second;
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
