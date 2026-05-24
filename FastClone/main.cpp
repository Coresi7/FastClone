#include "cli.h"
#include "sync_engine.h"

#include <exception>
#include <iostream>

#if defined(_WIN32) && defined(_MSC_VER)
int wmain(int argc, wchar_t** argv) {
#else
int main(int argc, char** argv) {
#endif
    try {
        const fc::CliOptions options = fc::ParseCli(argc, argv);
        if (options.mode == fc::Mode::Server) {
            return fc::RunServer(options);
        }
        return fc::RunClient(options);
    } catch (const std::exception& ex) {
        std::cerr << "FastClone error: " << ex.what() << std::endl;
        return 1;
    }
}
