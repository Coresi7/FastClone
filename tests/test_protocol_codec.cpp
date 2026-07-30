#include "protocol_codec.h"

#include "protocol.h"
#include "win_socket.h"

#ifndef _WIN32
#include <netinet/in.h>
#include <sys/socket.h>
#endif

#include <cstdint>
#include <cstring>
#include <optional>
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

// T-01 regression: CheckAuth must be recognized as a legal frame type by protocol.cpp's IsKnownMsgType/MsgTypeName,
// otherwise EncodeHeader refuses to send it (type_byte=6 judged corrupted) and the CheckAuth handshake fails on a real network.
void TestCheckAuthKnownType() {
    const uint8_t checkAuthByte = static_cast<uint8_t>(fc::MsgType::CheckAuth);
    Expect(checkAuthByte == 6, "CheckAuth wire byte is 6");
    Expect(fc::IsKnownMsgType(checkAuthByte), "IsKnownMsgType must accept CheckAuth (T-01)");
    Expect(std::string(fc::MsgTypeName(checkAuthByte)) == "CheckAuth", "MsgTypeName(6) == CheckAuth (T-01)");
}

// T-01 regression (real wire path, not via MockChannel): send a CheckAuth frame with SendFrame over loopback,
// and RecvFrame receives it on the other end. SendFrame internally goes through EncodeHeader; if CheckAuth is unregistered it throws
// "EncodeHeader refusing to send corrupted frame". This test asserts that defect does not recur.
void TestCheckAuthWireRoundTrip() {
    fc::WsaContext wsa;  // RAII Winsock init (the FastCloneTests main entry does not initialize it).
    fc::SocketHandle listener = fc::CreateServer(0);  // port 0 -> OS assigns an ephemeral port.

    // Query the actual bound port (CreateServer uses AF_INET6 dual-stack).
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
            // Timed accept: if the client never manages to connect (e.g. loopback
            // misconfiguration), the test fails with a clear error instead of
            // blocking forever in accept() and hanging join() below.
            std::optional<fc::SocketHandle> client = fc::AcceptClientTimeout(listener, 10000);
            if (!client) {
                serverThrew = true;
                serverErr = "AcceptClientTimeout: no client connection within 10s";
                return;
            }
            received = fc::RecvFrame(*client);
            fc::ShutdownBoth(*client);
        } catch (const std::exception& ex) {
            serverThrew = true;
            serverErr = ex.what();
        }
    });

    bool clientThrew = false;
    std::string clientErr;
    try {
        // CreateServer listens dual-stack; prefer IPv6 loopback but fall back to
        // IPv4 when ::1 is unavailable (e.g. ipv6 disabled on lo).
        fc::SocketHandle conn = [&]() {
            try {
                return fc::ConnectTo("::1", port);
            } catch (const std::exception&) {
                return fc::ConnectTo("127.0.0.1", port);
            }
        }();
        fc::CheckAuthInfo info;
        info.password = "loopback-pw";
        info.flags = 0;
        // Real SendFrame -> EncodeHeader (the T-01 defect point).
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
    // Various password forms: empty / ASCII / containing UTF-8 / long string (AC-33).
    RoundTrip("", 0);
    RoundTrip("hunter2", 0);
    RoundTrip(std::string("\xE4\xB8\xAD\xE6\x96\x87pass"), 0);  // contains a UTF-8 byte sequence
    RoundTrip(std::string(4096, 'x'), 0);
    // flags round-trip (non-zero).
    RoundTrip("pwd", 1);
    RoundTrip("pwd", 255);

    // Legacy payload (no flags byte, only AppendString(password)) decodes as flags=0: manually construct a payload with only password.
    std::vector<uint8_t> legacy;
    fc::AppendString(legacy, "legacy");
    const fc::CheckAuthInfo decoded = fc::DecodeCheckAuth(legacy);
    Expect(decoded.password == "legacy", "legacy payload password decodes");
    Expect(decoded.flags == 0, "legacy payload (no flags byte) decodes flags=0");

    // T-01 regression: CheckAuth frame-type registration + real SendFrame/EncodeHeader wire path.
    TestCheckAuthKnownType();
    TestCheckAuthWireRoundTrip();
}
