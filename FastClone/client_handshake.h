#pragma once

#include "protocol.h"
#include "protocol_codec.h"
#include "win_socket.h"

#include <string>

namespace fc {

// Protocol version (inline variable, unique across TUs, avoids ODR conflicts). FC6 -> FC7: introduces binary delta.
// FC6 and FC7 cleanly reject each other at the strict equality check in the Hello phase (reusing the existing
// version-mismatch semantics, no unknown-frame risk, binary-delta D-04 candidate C).
inline constexpr const char* kProtocolVersion = "FC7";

// Connection-level capability bits (FC7, AuthOk.capabilities payload, binary-delta section 8.1). bit0 = the
// server has binary delta capability. The client sends delta-specific messages only when "its own
// --delta-min-size>0 and the server advertises bit0" (AC-17).
inline constexpr uint8_t kCapDelta = 0x01;
// bit1 (FC7 additive, T-largefile-block-multinic): the peer can serve byte-range reads via
// the FileRangeOpen/FileRangeData/FileRangeEnd frame family. The client enables large-file
// block mode only when "its own --large-file-block was given AND the server advertises
// bit1"; otherwise it stays on the legacy FileOpen/FileChunk path (AC-08).
inline constexpr uint8_t kCapFileRange = 0x02;

// Send a simple frame carrying only a text payload (streamId=0).
void SendSimple(const SocketHandle& socket, MsgType type, const std::string& text = {});

// Client-side Hello negotiation (throws std::runtime_error on failure/version mismatch).
void NegotiateHelloAsClient(const SocketHandle& socket);

// Server-side Hello negotiation (on version mismatch, replies with an Error frame and throws). Version negotiation only, no session state.
void NegotiateHelloAsServer(const SocketHandle& socket);

// Client first connection: Hello -> Auth -> AuthOk(NewSession), returns session identity + advertised endpoints.
AuthOkInfo HandshakeClientNew(const SocketHandle& socket, const std::string& password);

// Client auxiliary link: Hello -> SessionJoin -> AuthOk(JoinAck).
void HandshakeClientJoin(const SocketHandle& socket, const std::string& password,
                         const std::string& sessionId);

// FastCheck read-only session handshake (fastcheck): Hello -> CheckAuth -> AuthOk(NewSession).
// Isomorphic to HandshakeClientNew, only the claim frame is swapped to MsgType::CheckAuth, marking to the server that this is a Check session.
// Throws std::runtime_error on failure/auth failure/version mismatch (the caller maps this to exit code 2).
AuthOkInfo HandshakeClientCheck(const SocketHandle& socket, const std::string& password);

}  // namespace fc
