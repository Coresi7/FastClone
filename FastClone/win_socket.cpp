#include "win_socket.h"

#include <algorithm>
#include <stdexcept>
#include <string>

#pragma comment(lib, "Ws2_32.lib")

namespace fc {

namespace {

std::string LastSocketError(const char* prefix) {
    return std::string(prefix) + " WSA=" + std::to_string(WSAGetLastError());
}

void TuneSocketForThroughput(SOCKET s) {
    if (s == INVALID_SOCKET) {
        return;
    }
    int noDelay = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));

    // A larger kernel buffer helps keep a 2.5G link fed.
    int bufSize = 4 * 1024 * 1024;
    setsockopt(s, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&bufSize), sizeof(bufSize));
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&bufSize), sizeof(bufSize));
}

}  // namespace

WsaContext::WsaContext() {
    WSADATA wsaData{};
    const int rc = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (rc != 0) {
        throw std::runtime_error("WSAStartup failed: " + std::to_string(rc));
    }
}

WsaContext::~WsaContext() {
    WSACleanup();
}

SocketHandle::~SocketHandle() {
    Reset();
}

SocketHandle::SocketHandle(SocketHandle&& other) noexcept {
    raw_ = other.raw_;
    other.raw_ = INVALID_SOCKET;
}

SocketHandle& SocketHandle::operator=(SocketHandle&& other) noexcept {
    if (this != &other) {
        Reset();
        raw_ = other.raw_;
        other.raw_ = INVALID_SOCKET;
    }
    return *this;
}

SOCKET SocketHandle::Release() {
    SOCKET raw = raw_;
    raw_ = INVALID_SOCKET;
    return raw;
}

void SocketHandle::Reset(SOCKET raw) {
    if (raw_ != INVALID_SOCKET) {
        closesocket(raw_);
    }
    raw_ = raw;
}

SocketHandle ConnectTo(const std::string& host, uint16_t port) {
    struct addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* result = nullptr;
    const std::string portStr = std::to_string(port);
    const int rc = getaddrinfo(host.c_str(), portStr.c_str(), &hints, &result);
    if (rc != 0) {
        throw std::runtime_error("getaddrinfo failed: " + std::to_string(rc));
    }

    SocketHandle connected;
    for (addrinfo* p = result; p != nullptr; p = p->ai_next) {
        SocketHandle attempt(socket(p->ai_family, p->ai_socktype, p->ai_protocol));
        if (!attempt.Valid()) {
            continue;
        }
        TuneSocketForThroughput(attempt.Get());
        if (connect(attempt.Get(), p->ai_addr, static_cast<int>(p->ai_addrlen)) == 0) {
            connected = std::move(attempt);
            break;
        }
    }
    freeaddrinfo(result);

    if (!connected.Valid()) {
        throw std::runtime_error("connect failed to " + host + ":" + std::to_string(port));
    }
    return connected;
}

SocketHandle CreateServer(uint16_t port) {
    SocketHandle listener(socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP));
    if (!listener.Valid()) {
        throw std::runtime_error(LastSocketError("socket failed"));
    }

    int no = 0;
    setsockopt(listener.Get(), IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&no), sizeof(no));

    int reuse = 1;
    setsockopt(listener.Get(), SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in6 addr{};
    addr.sin6_family = AF_INET6;
    addr.sin6_addr = in6addr_any;
    addr.sin6_port = htons(port);
    if (bind(listener.Get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        throw std::runtime_error(LastSocketError("bind failed"));
    }
    if (listen(listener.Get(), 1) != 0) {
        throw std::runtime_error(LastSocketError("listen failed"));
    }
    return listener;
}

SocketHandle AcceptClient(const SocketHandle& listener) {
    SOCKET client = accept(listener.Get(), nullptr, nullptr);
    if (client == INVALID_SOCKET) {
        throw std::runtime_error(LastSocketError("accept failed"));
    }
    TuneSocketForThroughput(client);
    return SocketHandle(client);
}

void SendAll(const SocketHandle& socket, const void* data, size_t length) {
    const uint8_t* cursor = static_cast<const uint8_t*>(data);
    size_t remaining = length;
    while (remaining > 0) {
        const int sent = send(socket.Get(), reinterpret_cast<const char*>(cursor), static_cast<int>(remaining), 0);
        if (sent <= 0) {
            throw std::runtime_error(LastSocketError("send failed"));
        }
        cursor += sent;
        remaining -= static_cast<size_t>(sent);
    }
}

void SendBuffersAll(const SocketHandle& socket, const WSABUF* buffers, size_t count) {
    if (count == 0) {
        return;
    }
    std::vector<WSABUF> pending(buffers, buffers + count);
    size_t cursor = 0;
    while (cursor < pending.size()) {
        DWORD sent = 0;
        const DWORD chunkCount = static_cast<DWORD>(std::min<size_t>(pending.size() - cursor, 1024));
        const int rc = WSASend(socket.Get(), pending.data() + cursor, chunkCount, &sent, 0, nullptr, nullptr);
        if (rc == SOCKET_ERROR) {
            throw std::runtime_error(LastSocketError("WSASend failed"));
        }
        size_t consumed = static_cast<size_t>(sent);
        while (cursor < pending.size() && consumed >= pending[cursor].len) {
            consumed -= pending[cursor].len;
            ++cursor;
        }
        if (cursor < pending.size() && consumed > 0) {
            pending[cursor].buf += consumed;
            pending[cursor].len -= static_cast<ULONG>(consumed);
        }
    }
}

void RecvAll(const SocketHandle& socket, void* data, size_t length) {
    uint8_t* cursor = static_cast<uint8_t*>(data);
    size_t remaining = length;
    while (remaining > 0) {
        const int recved = recv(socket.Get(), reinterpret_cast<char*>(cursor), static_cast<int>(remaining), 0);
        if (recved <= 0) {
            throw std::runtime_error(LastSocketError("recv failed"));
        }
        cursor += recved;
        remaining -= static_cast<size_t>(recved);
    }
}

}  // namespace fc
