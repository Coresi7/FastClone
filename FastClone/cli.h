#pragma once

#include <cstdint>
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
    uint64_t queuedFileSizeBytes = 5ULL * 1024ULL * 1024ULL * 1024ULL;
    uint32_t serverHashWorkers = 0;
    bool enableHashMemcache = false;
    bool streamAutoTune = true;
    bool chunkAutoTune = true;
    bool diagnostics = false;
};

#if defined(_WIN32) && defined(_MSC_VER)
CliOptions ParseCli(int argc, wchar_t** argv);
#else
CliOptions ParseCli(int argc, char** argv);
#endif

}  // namespace fc
