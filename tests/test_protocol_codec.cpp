#include "protocol_codec.h"

#include "protocol.h"
#include "win_socket.h"

#ifndef _WIN32
#include <netinet/in.h>
#include <sys/socket.h>
#endif

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

void Expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error("test_protocol_codec: " + message);
    }
}

void RoundTrip(const std::string& password, uint8_t flags) {
    fc::CheckAuthInfo info;
    info.password = password;
    info.flags = flags;
    const std::vector<uint8_t> encoded = fc::EncodeCheckAuth(info);
    const fc::CheckAuthInfo decoded = fc::DecodeCheckAuth(encoded);
    Expect(decoded.password == password, "CheckAuth password round-trip");
    Expect(decoded.flags == flags, "CheckAuth flags round-trip");
}

// T-01 回归：CheckAuth 必须被 protocol.cpp 的 IsKnownMsgType/MsgTypeName 认作合法帧类型，
// 否则 EncodeHeader 会拒发（type_byte=6 被判 corrupted），CheckAuth 握手在真实网络上失败。
void TestCheckAuthKnownType() {
    const uint8_t checkAuthByte = static_cast<uint8_t>(fc::MsgType::CheckAuth);
    Expect(checkAuthByte == 6, "CheckAuth wire byte is 6");
    Expect(fc::IsKnownMsgType(checkAuthByte), "IsKnownMsgType must accept CheckAuth (T-01)");
    Expect(std::string(fc::MsgTypeName(checkAuthByte)) == "CheckAuth", "MsgTypeName(6) == CheckAuth (T-01)");
}

// T-01 回归（真实 wire 路径，不经 MockChannel）：loopback 上用 SendFrame 发一个 CheckAuth 帧，
// 另一端 RecvFrame 收回。SendFrame 内部走 EncodeHeader，若 CheckAuth 未登记会抛
// "EncodeHeader refusing to send corrupted frame"，此测试即断言该缺陷不复现。
void TestCheckAuthWireRoundTrip() {
    fc::WsaContext wsa;  // RAII Winsock init（FastCloneTests 主入口未初始化）。
    fc::SocketHandle listener = fc::CreateServer(0);  // 端口 0 -> OS 分配临时端口。

    // 查询实际绑定端口（CreateServer 用 AF_INET6 双栈）。
    sockaddr_in6 bound{};
#ifdef _WIN32
    int len = static_cast<int>(sizeof(bound));
#else
    socklen_t len = static_cast<socklen_t>(sizeof(bound));
#endif
    if (getsockname(listener.Get(), reinterpret_cast<sockaddr*>(&bound), &len) != 0) {
        throw std::runtime_error("test_protocol_codec: getsockname failed");
    }
    const uint16_t port = ntohs(bound.sin6_port);
    Expect(port != 0, "ephemeral port assigned");

    fc::Frame received;
    bool serverThrew = false;
    std::string serverErr;
    std::thread server([&]() {
        try {
            fc::SocketHandle client = fc::AcceptClient(listener);
            received = fc::RecvFrame(client);
            fc::ShutdownBoth(client);
        } catch (const std::exception& ex) {
            serverThrew = true;
            serverErr = ex.what();
        }
    });

    bool clientThrew = false;
    std::string clientErr;
    try {
        fc::SocketHandle conn = fc::ConnectTo("::1", port);
        fc::CheckAuthInfo info;
        info.password = "loopback-pw";
        info.flags = 0;
        // 真实 SendFrame -> EncodeHeader（T-01 缺陷点）。
        fc::SendFrame(conn, fc::Frame{fc::MsgType::CheckAuth, 0, fc::EncodeCheckAuth(info)});
        fc::ShutdownBoth(conn);
    } catch (const std::exception& ex) {
        clientThrew = true;
        clientErr = ex.what();
    }

    server.join();
    Expect(!clientThrew, "SendFrame(CheckAuth) must not throw on wire path (T-01): " + clientErr);
    Expect(!serverThrew, "RecvFrame(CheckAuth) must not throw: " + serverErr);
    Expect(received.type == fc::MsgType::CheckAuth, "received frame type is CheckAuth");
    const fc::CheckAuthInfo decoded = fc::DecodeCheckAuth(received.payload);
    Expect(decoded.password == "loopback-pw", "wire CheckAuth password round-trip");
}

}  // namespace

void RunProtocolCodecTests() {
    // password 各形态：空 / ASCII / 含 UTF-8 / 长串（AC-33）。
    RoundTrip("", 0);
    RoundTrip("hunter2", 0);
    RoundTrip(std::string("\xE4\xB8\xAD\xE6\x96\x87pass"), 0);  // 含 UTF-8 字节序列
    RoundTrip(std::string(4096, 'x'), 0);
    // flags 往返（非 0）。
    RoundTrip("pwd", 1);
    RoundTrip("pwd", 255);

    // 旧载荷（无 flags 字节，仅 AppendString(password)）解为 flags=0：手工构造只含 password 的载荷。
    std::vector<uint8_t> legacy;
    fc::AppendString(legacy, "legacy");
    const fc::CheckAuthInfo decoded = fc::DecodeCheckAuth(legacy);
    Expect(decoded.password == "legacy", "legacy payload password decodes");
    Expect(decoded.flags == 0, "legacy payload (no flags byte) decodes flags=0");

    // T-01 回归：CheckAuth 帧类型登记 + 真实 SendFrame/EncodeHeader wire 路径。
    TestCheckAuthKnownType();
    TestCheckAuthWireRoundTrip();
}
