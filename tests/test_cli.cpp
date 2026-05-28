#include "cli.h"

#include <stdexcept>
#include <string>
#include <vector>

namespace {

void Require(bool cond, const char* msg) {
    if (!cond) {
        throw std::runtime_error(msg);
    }
}

fc::CliOptions Parse(const std::vector<std::string>& args) {
    std::vector<char*> argv;
    argv.reserve(args.size());
    for (const std::string& arg : args) {
        argv.push_back(const_cast<char*>(arg.c_str()));
    }
    return fc::ParseCli(static_cast<int>(argv.size()), argv.data());
}

void ExpectThrowWith(const std::vector<std::string>& args, const std::string& token) {
    try {
        (void)Parse(args);
        throw std::runtime_error("Expected ParseCli to throw");
    } catch (const std::exception& ex) {
        Require(std::string(ex.what()).find(token) != std::string::npos, "Unexpected exception text");
    }
}

}  // namespace

void RunCliTests() {
    {
        const fc::CliOptions opt = Parse({
            "FastClone",
            "server",
            "--password",
            "pw",
            "--server-hash-workers",
            "7",
            "--enable-hash-memcache",
        });
        Require(opt.mode == fc::Mode::Server, "Expected server mode");
        Require(opt.serverHashWorkers == 7, "Expected parsed server hash worker count");
        Require(opt.enableHashMemcache, "Expected hash memcache flag enabled");
    }

    ExpectThrowWith({
                        "FastClone",
                        "client",
                        "--server",
                        "127.0.0.1:27842",
                        "--target",
                        ".",
                        "--password",
                        "pw",
                        "--enable-hash-memcache",
                    },
                    "server-only");
}
