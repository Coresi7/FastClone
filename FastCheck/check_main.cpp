// FastCheck - standalone read-only directory comparison tool (fastcheck). Connects to an existing FastClone server,
// only enumerates, compares and outputs a report; zero side effects on local/remote directories (except the explicit
// --output report file). Exit codes 0-4 are one value one meaning.
//
// Entry flow (AC-01/21): parse CLI (failure->usage+2) -> precondition local/output checks (failure->3, no connect) ->
// install Ctrl+C -> ConnectTo (failure->2) -> HandshakeClientCheck (failure/auth failure->2) -> RunCheck ->
// WriteReport -> return the engine's suggested exit code. Disconnect/protocol exceptions all map to exit code 2.

#include "check_cli.h"
#include "check_engine.h"
#include "check_options.h"
#include "check_report.h"
#include "client_handshake.h"
#include "protocol.h"
#include "win_socket.h"

#ifdef _WIN32
#include <Windows.h>
#endif

#include <atomic>
#include <exception>
#include <iostream>
#include <string>
#include <vector>

namespace fc::check {
namespace {

// Ctrl+C interrupt flag: the signal/console handler only sets it; the engine observes it each round and cleanly finishes (FR-15/AC-21).
std::atomic<bool> g_interrupted{false};

#ifdef _WIN32
BOOL WINAPI ConsoleCtrlHandler(DWORD ctrlType) {
    if (ctrlType == CTRL_C_EVENT || ctrlType == CTRL_BREAK_EVENT ||
        ctrlType == CTRL_CLOSE_EVENT) {
        g_interrupted.store(true);
        return TRUE;
    }
    return FALSE;
}

std::string ToUtf8(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }
    const int len = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()),
                                        nullptr, 0, nullptr, nullptr);
    if (len <= 0) {
        throw std::runtime_error("WideCharToMultiByte failed");
    }
    std::string output(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), output.data(),
                        len, nullptr, nullptr);
    return output;
}
#endif

void InstallInterruptHandler() {
#ifdef _WIN32
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);
#endif
}

// Main flow. Returns the check-specific exit code.
int RunMain(const CheckOptions& options) {
    // Precondition local/output checks (before any TCP, FR-12).
    if (!CheckLocalPreconditions(options)) {
        return kLocalPrecondFailed;
    }

    InstallInterruptHandler();

    SocketHandle socket;
    try {
        socket = ConnectTo(options.server, options.port);
    } catch (const std::exception& ex) {
        std::cerr << "error: connect failed: " << ex.what() << std::endl;
        return kConnFailed;
    }
    if (!socket.Valid()) {
        std::cerr << "error: connect failed: could not connect to " << options.server << ":"
                  << options.port << std::endl;
        return kConnFailed;
    }

    try {
        HandshakeClientCheck(socket, options.password);
    } catch (const std::exception& ex) {
        std::cerr << "error: handshake/authentication failed: " << ex.what() << std::endl;
        ShutdownBoth(socket);
        return kConnFailed;
    }

    FrameChannel channel;
    channel.send = [&socket](const Frame& frame) { SendFrame(socket, frame); };
    channel.recv = [&socket]() { return RecvFrame(socket); };

    EngineOutcome outcome;
    try {
        outcome = RunCheck(options, channel, g_interrupted);
    } catch (const std::exception& ex) {
        // The engine already catches disconnects internally; this is a fallback for other protocol exceptions: exit code 2.
        std::cerr << "error: check aborted: " << ex.what() << std::endl;
        ShutdownBoth(socket);
        return kConnFailed;
    }

    WriteReport(options, outcome.result);
    ShutdownBoth(socket);
    return outcome.exit;
}

}  // namespace
}  // namespace fc::check

#if defined(_WIN32) && defined(_MSC_VER)
int wmain(int argc, wchar_t** argv) {
#else
int main(int argc, char** argv) {
#endif
    std::vector<std::string> args;
    if (argc > 1) {
        args.reserve(static_cast<size_t>(argc - 1));
        for (int i = 1; i < argc; ++i) {
#if defined(_WIN32) && defined(_MSC_VER)
            args.push_back(fc::check::ToUtf8(argv[i]));
#else
            args.emplace_back(argv[i] == nullptr ? "" : argv[i]);
#endif
        }
    }

    fc::check::CheckOptions options;
    try {
        options = fc::check::ParseCheckArgs(args);
    } catch (const std::exception& ex) {
        fc::check::PrintUsage();
        std::cerr << "argument error: " << ex.what() << std::endl;
        return fc::check::kConnFailed;  // parameter error: not 0/1 (FR-11), use 2 (D-03).
    }

    // Winsock is initialized only after argument parsing succeeds, so a bare
    // `FastCheck --version` (which exits inside ParseCheckArgs) never touches
    // Winsock -- matching the design intent documented in check_cli.cpp.
    fc::WsaContext wsa;  // RAII Winsock init.

    return fc::check::RunMain(options);
}
