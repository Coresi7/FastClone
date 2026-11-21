#include "protocol_codec.h"

#include "protocol.h"
#include "win_socket.h"

#ifndef _WIN32
#include <netinet/in.h>
#include <sys/socket.h>
#endif

#include <cstdint>
#include <cstring>
#include <chrono>
#include <functional>
#include <future>
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

// --- T-largefile-block-multinic additions -----------------------------------------------

// The four new wire types must be registered so EncodeHeader/RecvFrame accept them (same
// defect class as the T-01 CheckAuth regression above).
void TestFileRangeKnownTypes() {
    Expect(static_cast<uint8_t>(fc::MsgType::FileRangeOpen) == 37, "FileRangeOpen wire byte is 37");
    Expect(static_cast<uint8_t>(fc::MsgType::FileRangeData) == 38, "FileRangeData wire byte is 38");
    Expect(static_cast<uint8_t>(fc::MsgType::FileRangeEnd) == 39, "FileRangeEnd wire byte is 39");
    Expect(static_cast<uint8_t>(fc::MsgType::FileRangeError) == 41, "FileRangeError wire byte is 41");
    for (uint8_t b : {37, 38, 39, 41}) {
        Expect(fc::IsKnownMsgType(b), "IsKnownMsgType must accept FileRange* bytes");
        Expect(std::string(fc::MsgTypeName(b)).find("FileRange") == 0,
               "MsgTypeName must name FileRange* bytes");
    }
    // The isolation neighbours stay distinct: 36=FileBatchEnd, 40=SyncDone, delta 24-27.
    Expect(static_cast<uint8_t>(fc::MsgType::FileBatchEnd) == 36, "FileBatchEnd stays 36");
    Expect(static_cast<uint8_t>(fc::MsgType::SyncDone) == 40, "SyncDone stays 40");
}

void TestFileRangeOpenRoundTrip() {
    fc::FileRangeOpenReq req;
    req.relPath = "dir/sub/large.bin";
    req.offset = 0x1122334455667788ULL;
    req.length = 0x100000000ULL;  // 4 GiB upper bound must survive the u64 wire field
    const fc::FileRangeOpenReq decoded = fc::DecodeFileRangeOpen(fc::EncodeFileRangeOpen(req));
    Expect(decoded.relPath == req.relPath, "FileRangeOpen relPath round-trip");
    Expect(decoded.offset == req.offset, "FileRangeOpen offset round-trip");
    Expect(decoded.length == req.length, "FileRangeOpen length round-trip");

    // Zero range (client never sends it; the server answers FileRangeError) still round-trips.
    fc::FileRangeOpenReq zero;
    zero.relPath = "x";
    const fc::FileRangeOpenReq dz = fc::DecodeFileRangeOpen(fc::EncodeFileRangeOpen(zero));
    Expect(dz.relPath == "x" && dz.offset == 0 && dz.length == 0, "FileRangeOpen zero round-trip");

    Expect(fc::DecodeFileRangeError(fc::EncodeFileRangeError("a/b.bin")) == "a/b.bin",
           "FileRangeError relPath round-trip");
}

void TestAuthOkExtensionString() {
    // New shape: capabilities + extension string round-trip.
    fc::AuthOkInfo info;
    info.role = fc::AuthOkRole::NewSession;
    info.sessionId = "sess-1";
    info.capabilities = 0x03;  // kCapDelta | kCapFileRange
    info.extensionString = "ext-meta";
    fc::AdvertisedEndpoint ep;
    ep.endpoint = "10.0.0.2:27842";
    ep.nicGroup = 7;
    info.serverAddrs.push_back(ep);
    const fc::AuthOkInfo decoded = fc::DecodeAuthOk(fc::EncodeAuthOk(info));
    Expect(decoded.role == fc::AuthOkRole::NewSession, "AuthOk role round-trip");
    Expect(decoded.sessionId == "sess-1", "AuthOk sessionId round-trip");
    Expect(decoded.serverAddrs.size() == 1 && decoded.serverAddrs[0].endpoint == "10.0.0.2:27842" &&
               decoded.serverAddrs[0].nicGroup == 7,
           "AuthOk endpoints round-trip");
    Expect(decoded.capabilities == 0x03, "AuthOk capabilities round-trip");
    Expect(decoded.extensionString == "ext-meta", "AuthOk extensionString round-trip");

    // Legacy payload WITHOUT the extension string (ends at the capability byte) decodes "".
    {
        std::vector<uint8_t> legacy;
        legacy.push_back(static_cast<uint8_t>(fc::AuthOkRole::NewSession));
        fc::AppendString(legacy, "sess-2");
        fc::AppendU16(legacy, 0);       // no endpoints
        legacy.push_back(0x01);         // capabilities only
        const fc::AuthOkInfo d = fc::DecodeAuthOk(legacy);
        Expect(d.capabilities == 0x01, "legacy AuthOk capabilities decode");
        Expect(d.extensionString.empty(), "legacy AuthOk (no ext) decodes extensionString=\"\"");
    }
    // Even older payload WITHOUT the capability byte at all decodes defaults.
    {
        std::vector<uint8_t> ancient;
        ancient.push_back(static_cast<uint8_t>(fc::AuthOkRole::NewSession));
        fc::AppendString(ancient, "sess-3");
        fc::AppendU16(ancient, 0);
        const fc::AuthOkInfo d = fc::DecodeAuthOk(ancient);
        Expect(d.capabilities == 0, "ancient AuthOk (no caps) decodes capabilities=0");
        Expect(d.extensionString.empty(), "ancient AuthOk decodes extensionString=\"\"");
    }
}

void TestSessionJoinCapabilities() {
    fc::SessionJoinInfo join;
    join.sessionId = "sess-9";
    join.password = "pw";
    join.capabilities = 0x02;  // kCapFileRange
    join.extensionString = "client-ext";
    const fc::SessionJoinInfo decoded = fc::DecodeSessionJoin(fc::EncodeSessionJoin(join));
    Expect(decoded.sessionId == "sess-9" && decoded.password == "pw",
           "SessionJoin id/password round-trip");
    Expect(decoded.capabilities == 0x02, "SessionJoin capabilities round-trip");
    Expect(decoded.extensionString == "client-ext", "SessionJoin extensionString round-trip");

    // Legacy payload (sessionId + password only) decodes defaults (tolerant parse).
    std::vector<uint8_t> legacy;
    fc::AppendString(legacy, "sess-8");
    fc::AppendString(legacy, "pw2");
    const fc::SessionJoinInfo d = fc::DecodeSessionJoin(legacy);
    Expect(d.sessionId == "sess-8" && d.password == "pw2", "legacy SessionJoin id/password decode");
    Expect(d.capabilities == 0, "legacy SessionJoin decodes capabilities=0");
    Expect(d.extensionString.empty(), "legacy SessionJoin decodes extensionString=\"\"");
}

void TestAuthClaim() {
    // New client shape: bare password + capability byte + extension string.
    {
        const std::vector<uint8_t> payload = fc::EncodeAuthClaim("pw", 0x02, "meta");
        fc::AuthClaimInfo out;
        Expect(fc::DecodeAuthClaim(payload, "pw", out), "AuthClaim accepts correct password");
        Expect(out.capabilities == 0x02, "AuthClaim capabilities round-trip");
        Expect(out.extensionString == "meta", "AuthClaim extensionString round-trip");
        fc::AuthClaimInfo bad;
        Expect(!fc::DecodeAuthClaim(payload, "wrong", bad), "AuthClaim rejects wrong password");
    }
    // Legacy bare-password payload (old client) decodes with defaults against a new server.
    {
        const std::string bare = "hunter2";
        const std::vector<uint8_t> payload(bare.begin(), bare.end());
        fc::AuthClaimInfo out;
        Expect(fc::DecodeAuthClaim(payload, "hunter2", out), "legacy bare Auth accepts");
        Expect(out.capabilities == 0 && out.extensionString.empty(),
               "legacy bare Auth decodes defaults");
    }
    // Empty extension string still carries the capability byte.
    {
        const std::vector<uint8_t> payload = fc::EncodeAuthClaim("pw", 0x03, "");
        fc::AuthClaimInfo out;
        Expect(fc::DecodeAuthClaim(payload, "pw", out), "AuthClaim empty-ext accepts");
        Expect(out.capabilities == 0x03 && out.extensionString.empty(),
               "AuthClaim empty-ext round-trip");
    }
    // Malformed trailing bytes after a valid password prefix: tolerant defaults, still authed.
    {
        std::vector<uint8_t> payload = fc::EncodeAuthClaim("pw", 0x01, "x");
        payload.push_back(0xFF);  // corrupt the tail
        fc::AuthClaimInfo out;
        Expect(fc::DecodeAuthClaim(payload, "pw", out), "malformed tail still authenticates");
        Expect(out.capabilities == 0x01, "malformed tail keeps the capability byte");
    }
    // Password that is a strict prefix of the payload with no room for caps: still bare-auth.
    {
        const std::vector<uint8_t> payload = {'p'};
        fc::AuthClaimInfo out;
        Expect(!fc::DecodeAuthClaim(payload, "pw", out), "short payload rejected");
    }
}

// --- ci-once-multi-test-budget (59 design section 3.6): real-socket recv cancellation
// and EOF classification. Every lane uses the SAME loopback building blocks as
// TestCheckAuthWireRoundTrip above (WsaContext is the caller's; CreateServer(0) +
// AcceptClientTimeout + ConnectTo). A blocked RecvFrame runs inside std::async and is
// NEVER joined directly: a bounded wait_for, then a MANDATORY peer-side rescue
// (ShutdownBothRc => peer FIN => the blocked recv returns 0 => RecvEof), then get()
// (the std::async future joins the thread). No kill, no detach, no socket delete, no
// sleep-as-oracle; each case stays under 2s of wall clock.

struct RecvLaneResult {
    std::string kind;   // "ok" | "eof" | "error"
    std::string what;
    bool rescued = false;  // completed only after the peer-side ShutdownBothRc rescue
    int rescueRc = 0;
};

uint16_t LoopbackListenerPort(const fc::SocketHandle& listener) {
    sockaddr_in6 bound{};
#ifdef _WIN32
    int len = static_cast<int>(sizeof(bound));
#else
    socklen_t len = static_cast<socklen_t>(sizeof(bound));
#endif
    if (getsockname(listener.Get(), reinterpret_cast<sockaddr*>(&bound), &len) != 0) {
        throw std::runtime_error("test_protocol_codec: getsockname failed");
    }
    return ntohs(bound.sin6_port);
}

// Reader body: connect (IPv6 loopback, IPv4 fallback) and block in ONE RecvFrame.
// The result records the exception classification, mirroring the client recv thread's
// RecvEof / std::exception split (59 section 3.3).
std::future<RecvLaneResult> SpawnBlockedReader(uint16_t port) {
    return std::async(std::launch::async, [port]() {
        RecvLaneResult r;
        try {
            fc::SocketHandle conn = [&]() {
                try {
                    return fc::ConnectTo("::1", port);
                } catch (const std::exception&) {
                    return fc::ConnectTo("127.0.0.1", port);
                }
            }();
            fc::Frame f = fc::RecvFrame(conn);
            r.kind = "ok";
            r.what = std::to_string(static_cast<int>(static_cast<uint8_t>(f.type)));
        } catch (const fc::RecvEof& ex) {
            r.kind = "eof";
            r.what = ex.what();
        } catch (const std::exception& ex) {
            r.kind = "error";
            r.what = ex.what();
        }
        return r;
    });
}

// Bounded wait + mandatory rescue (59 section 3.6): the rescue is the PEER-side
// ShutdownBothRc, which is exactly how the production teardown wakes its blocked recv
// threads. rescued=true means the reader only finished because of the rescue.
RecvLaneResult WaitReaderWithRescue(std::future<RecvLaneResult>& fut, fc::SocketHandle& peer) {
    if (fut.wait_for(std::chrono::seconds(2)) == std::future_status::ready) {
        return fut.get();
    }
    const int rc = fc::ShutdownBothRc(peer);
    if (fut.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        throw std::runtime_error(
            "test_protocol_codec: blocked RecvFrame did not wake within 2s after the peer rescue");
    }
    RecvLaneResult r = fut.get();
    r.rescued = true;
    r.rescueRc = rc;
    return r;
}

void ExpectRecvEofLane(const RecvLaneResult& r, const std::string& tag) {
    Expect(r.kind == "eof",
           tag + ": expected RecvEof classification, got kind=" + r.kind + " what=" + r.what);
    Expect(r.what.rfind("recv failed", 0) == 0,
           tag + ": RecvEof message must keep the recv-failed text: " + r.what);
}

void TestRecvCancelWakesBlockedReader() {
    fc::WsaContext wsa;  // RAII Winsock init (same as TestCheckAuthWireRoundTrip).
    // Contract probe: ShutdownBothRc returns -1 for an invalid socket, 0 for a live one.
    Expect(fc::ShutdownBothRc(fc::SocketHandle{}) == -1,
           "ShutdownBothRc(invalid) must return -1");

    // P1: half header -- 4 of the 9 header bytes, then the peer shuts down. The blocked
    // reader must wake with RecvEof (orderly close mid-header), not hang, not desync.
    {
        std::vector<uint8_t> wire;
        fc::AppendEncodedFrame(wire, fc::Frame{fc::MsgType::FileChunk, 7,
                                               std::vector<uint8_t>(64, 0xAB)});
        fc::SocketHandle listener = fc::CreateServer(0);
        const uint16_t port = LoopbackListenerPort(listener);
        std::future<RecvLaneResult> fut = SpawnBlockedReader(port);
        std::optional<fc::SocketHandle> peer = fc::AcceptClientTimeout(listener, 10000);
        Expect(peer.has_value(), "P1: reader never connected within 10s");
        fc::SendAll(*peer, wire.data(), 4);
        const int rc = fc::ShutdownBothRc(*peer);
        Expect(rc == 0, "P1: peer ShutdownBothRc rc=" + std::to_string(rc));
        RecvLaneResult r = WaitReaderWithRescue(fut, *peer);
        Expect(!r.rescued, "P1: the peer shutdown itself must complete the reader (no rescue needed)");
        ExpectRecvEofLane(r, "P1");
    }

    // P2: half payload -- a complete 9-byte header declaring a 64-byte payload, only 10
    // payload bytes, then the peer shuts down. RecvFrame must wake with RecvEof.
    {
        std::vector<uint8_t> wire;
        fc::AppendEncodedFrame(wire, fc::Frame{fc::MsgType::FileChunk, 9,
                                               std::vector<uint8_t>(64, 0xCD)});
        fc::SocketHandle listener = fc::CreateServer(0);
        const uint16_t port = LoopbackListenerPort(listener);
        std::future<RecvLaneResult> fut = SpawnBlockedReader(port);
        std::optional<fc::SocketHandle> peer = fc::AcceptClientTimeout(listener, 10000);
        Expect(peer.has_value(), "P2: reader never connected within 10s");
        fc::SendAll(*peer, wire.data(), 9 + 10);
        const int rc = fc::ShutdownBothRc(*peer);
        Expect(rc == 0, "P2: peer ShutdownBothRc rc=" + std::to_string(rc));
        RecvLaneResult r = WaitReaderWithRescue(fut, *peer);
        Expect(!r.rescued, "P2: the peer shutdown itself must complete the reader (no rescue needed)");
        ExpectRecvEofLane(r, "P2");
    }

    // P3: zero-byte EOF -- the peer sends nothing and half-closes its send side (raw
    // Shutdown(Send), the closesocket/FIN shape). The blocked reader wakes with RecvEof.
    {
        fc::SocketHandle listener = fc::CreateServer(0);
        const uint16_t port = LoopbackListenerPort(listener);
        std::future<RecvLaneResult> fut = SpawnBlockedReader(port);
        std::optional<fc::SocketHandle> peer = fc::AcceptClientTimeout(listener, 10000);
        Expect(peer.has_value(), "P3: reader never connected within 10s");
#ifdef _WIN32
        const int rc = shutdown(peer->Get(), SD_SEND);
#else
        const int rc = shutdown(peer->Get(), SHUT_WR);
#endif
        Expect(rc == 0, "P3: raw shutdown(Send) rc=" + std::to_string(rc));
        RecvLaneResult r = WaitReaderWithRescue(fut, *peer);
        Expect(!r.rescued, "P3: the peer Shutdown(Send) itself must complete the reader");
        ExpectRecvEofLane(r, "P3");
    }

    // P4: two lanes at once -- two connections with two RecvFrame readers blocked
    // simultaneously; lane A gets the P1 shape, lane B the P2 shape; each wakes and
    // joins independently (per-lane rescue if ever needed), each within its 2s bound.
    {
        std::vector<uint8_t> wireA;
        fc::AppendEncodedFrame(wireA, fc::Frame{fc::MsgType::FileChunk, 1,
                                                std::vector<uint8_t>(64, 0x11)});
        std::vector<uint8_t> wireB;
        fc::AppendEncodedFrame(wireB, fc::Frame{fc::MsgType::FileChunk, 2,
                                                std::vector<uint8_t>(64, 0x22)});
        fc::SocketHandle listenerA = fc::CreateServer(0);
        fc::SocketHandle listenerB = fc::CreateServer(0);
        const uint16_t portA = LoopbackListenerPort(listenerA);
        const uint16_t portB = LoopbackListenerPort(listenerB);
        std::future<RecvLaneResult> futA = SpawnBlockedReader(portA);
        std::future<RecvLaneResult> futB = SpawnBlockedReader(portB);
        std::optional<fc::SocketHandle> peerA = fc::AcceptClientTimeout(listenerA, 10000);
        std::optional<fc::SocketHandle> peerB = fc::AcceptClientTimeout(listenerB, 10000);
        Expect(peerA.has_value() && peerB.has_value(), "P4: a reader never connected within 10s");
        fc::SendAll(*peerA, wireA.data(), 4);
        fc::SendAll(*peerB, wireB.data(), 9 + 10);
        const int rcA = fc::ShutdownBothRc(*peerA);
        const int rcB = fc::ShutdownBothRc(*peerB);
        Expect(rcA == 0 && rcB == 0,
               "P4: peer ShutdownBothRc rcA=" + std::to_string(rcA) + " rcB=" + std::to_string(rcB));
        RecvLaneResult rA = WaitReaderWithRescue(futA, *peerA);
        RecvLaneResult rB = WaitReaderWithRescue(futB, *peerB);
        Expect(!rA.rescued && !rB.rescued, "P4: each lane's own peer shutdown must complete it");
        ExpectRecvEofLane(rA, "P4/A");
        ExpectRecvEofLane(rB, "P4/B");
    }

    // P5: negative -- NO peer signal: the reader must STILL be blocked after 300ms
    // (wait_for timeout is the blocked-proof), and only the peer rescue shutdown lets
    // the thread collect the EOF and JOIN. No permanent thread, no kill.
    {
        fc::SocketHandle listener = fc::CreateServer(0);
        const uint16_t port = LoopbackListenerPort(listener);
        std::future<RecvLaneResult> fut = SpawnBlockedReader(port);
        std::optional<fc::SocketHandle> peer = fc::AcceptClientTimeout(listener, 10000);
        Expect(peer.has_value(), "P5: reader never connected within 10s");
        Expect(fut.wait_for(std::chrono::milliseconds(300)) == std::future_status::timeout,
               "P5: with no peer signal the RecvFrame reader must still be blocked after 300ms");
        RecvLaneResult r = WaitReaderWithRescue(fut, *peer);
        Expect(r.rescued, "P5: only the peer rescue shutdown may complete the blocked reader");
        Expect(r.rescueRc == 0, "P5: rescue ShutdownBothRc rc=" + std::to_string(r.rescueRc));
        ExpectRecvEofLane(r, "P5");
    }
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

    // T-largefile-block-multinic: new frame types, FileRange codecs, and the handshake
    // capability/extension-string evolution (tolerant trailing-field parsing).
    TestFileRangeKnownTypes();
    TestFileRangeOpenRoundTrip();
    TestAuthOkExtensionString();
    TestSessionJoinCapabilities();
    TestAuthClaim();

    // ci-once-multi-test-budget (59 section 3.6): real loopback recv cancellation
    // (P1 half header / P2 half payload / P3 zero-byte EOF / P4 two lanes / P5 negative
    // blocked-proof + peer rescue).
    TestRecvCancelWakesBlockedReader();
}
