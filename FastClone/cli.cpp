#include "cli.h"

#include <Windows.h>

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace fc {

namespace {

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

std::wstring ArgAt(const std::vector<std::wstring>& args, size_t index) {
    if (index >= args.size()) {
        throw std::runtime_error("Missing value for argument");
    }
    return args[index];
}

void PrintUsage() {
    std::cerr
        << "Usage:\n"
        << "  fastclone server [--dir <path>] [--port <n>] --password <pwd>\n"
        << "  fastclone client --server <host:port> --target <path> --password <pwd> [--streams <n>] [--chunk-kb <n>]\n";
}

std::pair<std::string, uint16_t> ParseHostPort(const std::wstring& input, uint16_t defaultPort) {
    const auto pos = input.find(L':');
    if (pos == std::wstring::npos) {
        if (input.empty()) {
            throw std::runtime_error("Invalid --server, host is empty");
        }
        return {ToUtf8(input), defaultPort};
    }
    const std::wstring hostW = input.substr(0, pos);
    const std::wstring portW = input.substr(pos + 1);
    if (hostW.empty() || portW.empty()) {
        throw std::runtime_error("Invalid --server, expected host:port");
    }
    const long port = std::wcstol(portW.c_str(), nullptr, 10);
    if (port <= 0 || port > 65535) {
        throw std::runtime_error("Port out of range");
    }
    return {ToUtf8(hostW), static_cast<uint16_t>(port)};
}

}  // namespace

CliOptions ParseCli(int argc, wchar_t** argv) {
    if (argc < 2) {
        PrintUsage();
        throw std::runtime_error("Mode is required");
    }

    std::vector<std::wstring> args(argv + 1, argv + argc);
    CliOptions options;

    if (args[0] == L"server") {
        options.mode = Mode::Server;
        options.rootDir = fs::current_path();
    } else if (args[0] == L"client") {
        options.mode = Mode::Client;
    } else {
        PrintUsage();
        throw std::runtime_error("First argument must be server or client");
    }

    for (size_t i = 1; i < args.size(); ++i) {
        const std::wstring& arg = args[i];
        if (arg == L"--dir") {
            options.rootDir = fs::path(ArgAt(args, ++i));
        } else if (arg == L"--target") {
            options.rootDir = fs::path(ArgAt(args, ++i));
        } else if (arg == L"--port") {
            const long port = std::wcstol(ArgAt(args, ++i).c_str(), nullptr, 10);
            if (port <= 0 || port > 65535) {
                throw std::runtime_error("Port out of range");
            }
            options.port = static_cast<uint16_t>(port);
        } else if (arg == L"--server") {
            auto hostPort = ParseHostPort(ArgAt(args, ++i), options.port);
            options.host = hostPort.first;
            options.port = hostPort.second;
        } else if (arg == L"--password") {
            options.password = ToUtf8(ArgAt(args, ++i));
        } else if (arg == L"--streams") {
            const long streams = std::wcstol(ArgAt(args, ++i).c_str(), nullptr, 10);
            if (streams <= 0 || streams > 1024) {
                throw std::runtime_error("Invalid --streams");
            }
            options.streamLimit = static_cast<uint32_t>(streams);
        } else if (arg == L"--chunk-kb") {
            const long chunkKb = std::wcstol(ArgAt(args, ++i).c_str(), nullptr, 10);
            if (chunkKb <= 0 || chunkKb > 4096) {
                throw std::runtime_error("Invalid --chunk-kb");
            }
            options.chunkSize = static_cast<uint32_t>(chunkKb * 1024);
        } else {
            throw std::runtime_error("Unknown argument: " + ToUtf8(arg));
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

}  // namespace fc
