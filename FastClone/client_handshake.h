#pragma once

#include "protocol.h"
#include "protocol_codec.h"
#include "win_socket.h"

#include <cstdlib>
#include <iostream>
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

// --- Task ci-integration-test-timeout: bounded handshake recv (RC-5b, design section 3.9) ---
// A handshake is a strict single request/response with no disk, hash or enumeration work on
// either side: a healthy reply arrives in milliseconds. Bound the recv so a peer that completed
// the TCP connect but never speaks cannot park this thread forever (RC-5b). The default (30s) is
// ~1e3x the healthy latency, so the healthy path is bit-for-bit unchanged.
// Env override is forensic-only; 0 disables (restores unbounded behavior).
inline int HandshakeRecvTimeoutMs() {
    if (const char* v = std::getenv("FASTCLONE_HANDSHAKE_RECV_TIMEOUT_MS")) {
        if (v[0]) {
            const int ms = std::atoi(v);
            if (ms > 0) { return ms; }
            // Explicit "0" disables the bound (restores unbounded behavior, R-4 rollback);
            // garbage that does not parse to a number falls back to the default.
            if (ms == 0 && v[0] == '0') { return 0; }
        }
    }
    return 30000;
}

// v2 (R-01 / gate H-2): SAVE/RESTORE, not "reset to 0". v1's destructor unconditionally
// restored 0, which silently cancelled an ENCLOSING guard: HandshakeClientNew installs 30s,
// the nested NegotiateHelloAsClient guard then restores 0 on its way out, leaving the AuthOk
// recv (JoinAck / CheckAuth reply) completely unbounded -- i.e. 2 of the 3 wrap sites were
// effectively dead and the one stall shape we care about most was uncovered.
// Save/restore is value-based, therefore composable at any nesting depth: the innermost
// restores what the outer installed, and only the outermost restores the real default (0).
// NOTE (gate H-2): the full class definition lives in THIS header (namespace fc) so the
// server TU (sync_engine_server.cpp -> sync_engine_internal.h -> client_handshake.h) shares
// the single definition; putting it in an anonymous namespace of the .cpp would leave the
// server wrap site without a complete type.
class HandshakeRecvGuard {
public:
    explicit HandshakeRecvGuard(const SocketHandle& h) {
        if (!h.Valid()) { return; }
        s_ = h.Get();
        int prev = 0;
        // If we cannot READ the current value we must not WRITE one we cannot undo: degrade to
        // a no-op and say so. Writing blind is exactly what cancelled the outer bound in v1.
        if (!GetRecvTimeout(s_, prev)) {
            std::cerr << "[warn][hs] get SO_RCVTIMEO failed; handshake recv left unbounded"
                      << std::endl;
            return;
        }
        savedMs_ = prev;
        installed_ = SetRecvTimeout(s_, HandshakeRecvTimeoutMs());
        if (!installed_) {
            std::cerr << "[warn][hs] set SO_RCVTIMEO failed; handshake recv left unbounded"
                      << std::endl;
        }
    }
    ~HandshakeRecvGuard() {
        if (s_ == kInvalidSocket || !installed_) { return; }
        SetRecvTimeout(s_, savedMs_);   // restore exactly what was there before
    }
    HandshakeRecvGuard(const HandshakeRecvGuard&) = delete;
    HandshakeRecvGuard& operator=(const HandshakeRecvGuard&) = delete;
private:
    SocketNative s_ = kInvalidSocket;
    int savedMs_ = 0;
    bool installed_ = false;
};

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
