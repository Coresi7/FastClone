#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include <WinSock2.h>
#include <WS2tcpip.h>

namespace fc {

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
    explicit SocketHandle(SOCKET raw) : raw_(raw) {}
    ~SocketHandle();

    SocketHandle(SocketHandle&& other) noexcept;
    SocketHandle& operator=(SocketHandle&& other) noexcept;

    SocketHandle(const SocketHandle&) = delete;
    SocketHandle& operator=(const SocketHandle&) = delete;

    [[nodiscard]] bool Valid() const { return raw_ != INVALID_SOCKET; }
    [[nodiscard]] SOCKET Get() const { return raw_; }
    SOCKET Release();
    void Reset(SOCKET raw = INVALID_SOCKET);

private:
    SOCKET raw_ = INVALID_SOCKET;
};

SocketHandle ConnectTo(const std::string& host, uint16_t port);
SocketHandle CreateServer(uint16_t port);
SocketHandle AcceptClient(const SocketHandle& listener);
void SendAll(const SocketHandle& socket, const void* data, size_t length);
void RecvAll(const SocketHandle& socket, void* data, size_t length);

}  // namespace fc
