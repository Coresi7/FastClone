#include "client_handshake.h"

#include <stdexcept>
#include <string>

namespace fc {

void SendSimple(const SocketHandle& socket, MsgType type, const std::string& text) {
    Frame frame;
    frame.type = type;
    frame.streamId = 0;
    frame.payload.assign(text.begin(), text.end());
    SendFrame(socket, frame);
}

// --- FC6 handshake: version negotiation is shared; session claim differs (design §3, §5) ---

// Server side: exchange Hello and validate the protocol version (FR-018 / AC-013). On
// mismatch sends Error and throws so the version-reject path is identical to FC5.
void NegotiateHelloAsServer(const SocketHandle& socket) {
    const Frame hello = RecvFrame(socket);
    if (hello.type != MsgType::Hello) {
        throw std::runtime_error("Expected HELLO");
    }
    const std::string clientVersion(reinterpret_cast<const char*>(hello.payload.data()), hello.payload.size());
    if (clientVersion != kProtocolVersion) {
        SendSimple(socket, MsgType::Error, "Protocol version mismatch: server=" + std::string(kProtocolVersion) + " client=" + clientVersion);
        throw std::runtime_error("Protocol version mismatch: server=" + std::string(kProtocolVersion) + " client=" + clientVersion);
    }
    SendSimple(socket, MsgType::Hello, kProtocolVersion);
}

// Client side: Hello negotiation (FR-018 / AC-013). Throws on mismatch / server error.
void NegotiateHelloAsClient(const SocketHandle& socket) {
    SendSimple(socket, MsgType::Hello, kProtocolVersion);
    Frame helloBack = RecvFrame(socket);
    if (helloBack.type == MsgType::Error) {
        const std::string payload(reinterpret_cast<const char*>(helloBack.payload.data()), helloBack.payload.size());
        throw std::runtime_error("Server error: " + payload);
    }
    if (helloBack.type != MsgType::Hello) {
        throw std::runtime_error("Server HELLO missing");
    }
    const std::string serverVersion(reinterpret_cast<const char*>(helloBack.payload.data()), helloBack.payload.size());
    if (serverVersion != kProtocolVersion) {
        throw std::runtime_error("Protocol version mismatch: client=" + std::string(kProtocolVersion) + " server=" + serverVersion);
    }
}

// Client first connection: Hello -> Auth -> AuthOk(NewSession). Returns the session
// identity + server-advertised endpoint list (design §3.2 / FR-003/005/017).
AuthOkInfo HandshakeClientNew(const SocketHandle& socket, const std::string& password) {
    NegotiateHelloAsClient(socket);
    SendSimple(socket, MsgType::Auth, password);
    Frame authResult = RecvFrame(socket);
    if (authResult.type != MsgType::AuthOk) {
        const std::string payload(reinterpret_cast<const char*>(authResult.payload.data()), authResult.payload.size());
        throw std::runtime_error("Server authentication rejected: " + payload);
    }
    AuthOkInfo info = DecodeAuthOk(authResult.payload);
    // The first connection must receive a NewSession AuthOk carrying a session id; any
    // other role (or an empty id) is a non-retryable protocol violation (review B-04).
    if (info.role != AuthOkRole::NewSession) {
        throw std::runtime_error(
            "Protocol error: expected AuthOk role NewSession on first connection, got JoinAck");
    }
    if (info.sessionId.empty()) {
        throw std::runtime_error("Protocol error: AuthOk(NewSession) carried an empty sessionId");
    }
    return info;
}

// Client follow-up connection: Hello -> SessionJoin(sessionId,pwd) -> AuthOk(JoinAck).
void HandshakeClientJoin(const SocketHandle& socket, const std::string& password,
                         const std::string& sessionId) {
    NegotiateHelloAsClient(socket);
    SessionJoinInfo join;
    join.sessionId = sessionId;
    join.password = password;
    SendFrame(socket, Frame{MsgType::SessionJoin, 0, EncodeSessionJoin(join)});
    Frame authResult = RecvFrame(socket);
    if (authResult.type != MsgType::AuthOk) {
        const std::string payload(reinterpret_cast<const char*>(authResult.payload.data()), authResult.payload.size());
        throw std::runtime_error("Server rejected session join: " + payload);
    }
    // A follow-up connection must be acknowledged with role JoinAck; receiving NewSession
    // here means the server treated us as a brand-new session and the lane cannot be joined
    // to the pool. Non-retryable protocol violation (review B-04).
    const AuthOkInfo info = DecodeAuthOk(authResult.payload);
    if (info.role != AuthOkRole::JoinAck) {
        throw std::runtime_error(
            "Protocol error: expected AuthOk role JoinAck on session join, got NewSession");
    }
    // The JoinAck must echo back the session id we asked to join. An empty id, or one that
    // does not match our request, means the lane is not bound to the intended session pool
    // (a misrouted/buggy server reply). Non-retryable protocol violation (review B-04 / B4-R1).
    if (info.sessionId.empty()) {
        throw std::runtime_error("Protocol error: AuthOk(JoinAck) carried an empty sessionId");
    }
    if (info.sessionId != sessionId) {
        throw std::runtime_error(
            "Protocol error: AuthOk(JoinAck) sessionId mismatch, requested=" + sessionId +
            " got=" + info.sessionId);
    }
}

}  // namespace fc
