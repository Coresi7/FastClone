#pragma once

#include <filesystem>
#include <string>

namespace fc {

enum class Mode {
    Server,
    Client
};

struct CliOptions {
    Mode mode = Mode::Server;
    std::filesystem::path rootDir;
    std::string host = "127.0.0.1";
    uint16_t port = 27842;
    std::string password;
    uint32_t streamLimit = 16;
    uint32_t chunkSize = 256 * 1024;
};

CliOptions ParseCli(int argc, wchar_t** argv);

}  // namespace fc
