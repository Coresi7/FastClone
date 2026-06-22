#pragma once

#include <cstddef>
#include <cstdint>
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

// Optional source binding for a multipath connection (design §6.5). Empty fields mean
// "do not bind". localAddr pins the source IP (strong-host on Windows); ifaceName pins
// the egress interface on weak-host platforms (macOS IP_BOUND_IF / Linux SO_BINDTODEVICE).
struct ConnectBinding {
    std::string localAddr;
    std::string ifaceName;
};

SocketHandle ConnectTo(const std::string& host, uint16_t port);
SocketHandle ConnectTo(const std::string& host, uint16_t port, const ConnectBinding& binding);
// Return the actual source IP the OS bound a connected socket to (getsockname), as a
// numeric literal. Empty on failure. Used so the primary lane's real NIC participates in
// the multipath same-side dedup even when it was connected via the OS default route.
std::string LocalAddressOf(const SocketHandle& socket);
SocketHandle CreateServer(uint16_t port);
SocketHandle AcceptClient(const SocketHandle& listener);
void SendAll(const SocketHandle& socket, const void* data, size_t length);
void SendBuffersAll(const SocketHandle& socket, const SocketBuffer* buffers, size_t count);
void RecvAll(const SocketHandle& socket, void* data, size_t length);
void ShutdownBoth(const SocketHandle& socket);

}  // namespace fc
