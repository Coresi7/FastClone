// FastCheck — 独立只读目录比对工具（fastcheck）。连接现有 FastClone server，仅枚举、比对、
// 输出报告；对本地/远端目录零副作用（除显式 --output 报告文件）。退出码 0-4 一值一义。
//
// 入口流程（AC-01/21）：解析 CLI（失败→usage+2）→ 前置本地/输出检查（失败→3，不连接）→
// 安装 Ctrl+C → ConnectTo（失败→2）→ HandshakeClientCheck（失败/认证失败→2）→ RunCheck →
// WriteReport → 返回 engine 建议退出码。断连/协议异常统一映射退出码 2。

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

// Ctrl+C 中断标志：信号/控制台处理器仅置位，engine 每轮观察后干净收尾（FR-15/AC-21）。
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

// 主流程。返回 check 专用退出码。
int RunMain(const CheckOptions& options) {
    // 前置本地/输出检查（在任何 TCP 之前，FR-12）。
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
        // engine 内部已捕获断连，此处兜底其它协议异常：退出码 2。
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
    fc::WsaContext wsa;  // RAII Winsock init。

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
        return fc::check::kConnFailed;  // 参数错误：非 0/1（FR-11），取 2（D-03）。
    }

    return fc::check::RunMain(options);
}
