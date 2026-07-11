#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#ifdef _WIN32
#include <WinSock2.h>
#include <WS2tcpip.h>
#else
#include <sys/types.h>
#endif

namespace fc {

#ifdef _WIN32
using SocketNative = SOCKET;
constexpr SocketNative kInvalidSocket = INVALID_SOCKET;
#else
using SocketNative = int;
constexpr SocketNative kInvalidSocket = -1;
#endif

struct SocketBuffer {
    const uint8_t* data = nullptr;
    size_t len = 0;
};

class WsaContext {
public:
    WsaContext();
    ~WsaContext();
    WsaContext(const WsaContext&) = delete;
    WsaContext& operator=(const WsaContext&) = delete;
};

class SocketHandle {
public:
    SocketHandle() = default;
    explicit SocketHandle(SocketNative raw) : raw_(raw) {}
    ~SocketHandle();

    SocketHandle(SocketHandle&& other) noexcept;
    SocketHandle& operator=(SocketHandle&& other) noexcept;

    SocketHandle(const SocketHandle&) = delete;
    SocketHandle& operator=(const SocketHandle&) = delete;

    [[nodiscard]] bool Valid() const { return raw_ != kInvalidSocket; }
    [[nodiscard]] SocketNative Get() const { return raw_; }
    SocketNative Release();
    void Reset(SocketNative raw = kInvalidSocket);

private:
    SocketNative raw_ = kInvalidSocket;
};

// Optional source binding for a multipath connection (design section 6.5). Empty fields mean
// "do not bind". localAddr pins the source IP (strong-host on Windows); ifaceName pins
// the egress interface on weak-host platforms (macOS IP_BOUND_IF / Linux SO_BINDTODEVICE).
struct ConnectBinding {
    std::string localAddr;
    std::string ifaceName;
};

// Explicit TCP socket-buffer overrides in bytes; 0 = leave the buffer to kernel autotuning
// (recommended: receive-window autotuning scales the window to the BDP on high-RTT links). A
// positive value pins the buffer (which DISABLES receive-window autotuning for that direction)
// and is intended for experimentation via --tcp-send-buffer / --tcp-recv-buffer. Set once at
// startup before any socket is tuned; applies to every subsequently connected/accepted socket.
void SetSocketBufferOverrides(int sndBufBytes, int rcvBufBytes);

// Snapshot of kernel TCP state for one connection (diagnostics only, design wan-single-tcp section 3.2).
// retrans unit differs by platform (Linux: retransmitted segments; Windows: retransmitted bytes);
// the trend, not the absolute unit, is what diagnoses loss-driven cwnd sawtooth. valid=false when
// the OS cannot supply it (unsupported, not a TCP socket, or the query failed).
struct TcpDiag {
    bool valid = false;
    uint64_t cwndBytes = 0;      // congestion window in bytes
    uint32_t rttUs = 0;          // smoothed RTT in microseconds
    uint64_t retrans = 0;        // cumulative retransmits (segments on Linux, bytes on Windows)
    uint64_t bytesInFlight = 0;  // unacknowledged bytes currently in flight
    uint32_t mss = 0;            // sender MSS
};
TcpDiag QueryTcpDiag(SocketNative s);

SocketHandle ConnectTo(const std::string& host, uint16_t port);
SocketHandle ConnectTo(const std::string& host, uint16_t port, const ConnectBinding& binding);
// Return the actual source IP the OS bound a connected socket to (getsockname), as a
// numeric literal. Empty on failure. Used so the primary lane's real NIC participates in
// the multipath same-side dedup even when it was connected via the OS default route.
std::string LocalAddressOf(const SocketHandle& socket);
// Return the peer (client) IP a connected/accepted socket is talking to (getpeername), as
// a numeric literal. Empty on failure. Used by the server to show who connected. Non-blocking:
// getpeername + NI_NUMERICHOST only format the already-known socket address, no DNS lookup. The
// result is a numeric address literal (no C0/C1 control characters), safe to splice into logs.
std::string PeerAddressOf(const SocketHandle& socket);
SocketHandle CreateServer(uint16_t port);
SocketHandle AcceptClient(const SocketHandle& listener);
// Like AcceptClient but waits at most timeoutMs for an incoming connection. Returns nullopt
// when the wait elapses with no pending connection (used by --once-multi so the accept loop
// can periodically evaluate idle-grace, design section 6.2). Throws on a hard accept/poll error.
std::optional<SocketHandle> AcceptClientTimeout(const SocketHandle& listener, int timeoutMs);
void SendAll(const SocketHandle& socket, const void* data, size_t length);
void SendBuffersAll(const SocketHandle& socket, const SocketBuffer* buffers, size_t count);
void RecvAll(const SocketHandle& socket, void* data, size_t length);
void ShutdownBoth(const SocketHandle& socket);

}  // namespace fc
