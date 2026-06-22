#include "sync_util.h"

#include <stdexcept>

namespace {

void Require(bool cond, const char* msg) {
    if (!cond) {
        throw std::runtime_error(msg);
    }
}

}  // namespace

void RunReconnectClassifierTests() {
    Require(!fc::IsFatalClientDisconnectReason(""), "empty reason is transient");
    Require(!fc::IsFatalClientDisconnectReason("connection_closed"), "connection_closed is transient");
    Require(!fc::IsFatalClientDisconnectReason("recv failed WSA=10054"), "recv reset is transient");
    Require(!fc::IsFatalClientDisconnectReason("connect failed to 127.0.0.1:27842"),
            "connect refused is transient");
    Require(fc::IsFatalClientDisconnectReason("Protocol version mismatch: client=FC5 server=FC4"),
            "protocol mismatch is fatal");
    Require(fc::IsFatalClientDisconnectReason("Server authentication rejected"), "auth rejected is fatal");
    Require(fc::IsFatalClientDisconnectReason("Authentication failed"), "auth failed is fatal");
    Require(fc::IsFatalClientDisconnectReason("Server HELLO missing"), "missing hello is fatal");
    Require(fc::IsFatalClientDisconnectReason(
                "Protocol error: expected AuthOk role NewSession on first connection, got JoinAck"),
            "AuthOk role mismatch is fatal");
    Require(fc::IsFatalClientDisconnectReason(
                "Protocol error: AuthOk(NewSession) carried an empty sessionId"),
            "empty sessionId is fatal");
    Require(fc::IsFatalClientDisconnectReason(
                "Protocol error: AuthOk(JoinAck) carried an empty sessionId"),
            "empty JoinAck sessionId is fatal");
    Require(fc::IsFatalClientDisconnectReason(
                "Protocol error: AuthOk(JoinAck) sessionId mismatch, requested=abc got=xyz"),
            "JoinAck sessionId mismatch is fatal");
    Require(fc::IsFatalClientDisconnectReason("Server error: Protocol version mismatch: server=FC5 client=FC4"),
            "server error frame is fatal");
    Require(fc::IsFatalClientDisconnectReason("RecvFrame desync: bad header type_byte=255"),
            "desync is fatal");
}
