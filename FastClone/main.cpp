#include "cli.h"
#include "sync_engine.h"

#include <exception>
#include <iostream>
#include <system_error>
#include <thread>

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
    } catch (const std::system_error& ex) {
        // Surface the underlying error code so e.g. "resource deadlock would occur"
        // (errc::resource_deadlock_would_occur, raised by a thread joining itself)
        // is unambiguous and tied to the throwing thread.
        std::cerr << "FastClone error (system_error): " << ex.what()
                  << " | code=" << ex.code().value()
                  << " category=" << ex.code().category().name()
                  << " message=\"" << ex.code().message() << "\""
                  << " main_thread=" << std::this_thread::get_id()
                  << std::endl;
        return fc::kExitUsage;
    } catch (const std::exception& ex) {
        std::cerr << "FastClone error: " << ex.what() << std::endl;
        return fc::kExitUsage;
    }
}
