#include "win_socket.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#pragma comment(lib, "Ws2_32.lib")
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>
#endif

namespace fc {

namespace {

std::string LastSocketError(const char* prefix) {
#ifdef _WIN32
    return std::string(prefix) + " WSA=" + std::to_string(WSAGetLastError());
#else
    return std::string(prefix) + " errno=" + std::to_string(errno) + " (" + std::strerror(errno) + ")";
#endif
}

void TuneSocketForThroughput(SocketNative s) {
    if (s == kInvalidSocket) {
        return;
    }
    int noDelay = 1;
#ifdef _WIN32
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
#else
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &noDelay, static_cast<socklen_t>(sizeof(noDelay)));
#if defined(SO_NOSIGPIPE)
    int noSigPipe = 1;
    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &noSigPipe, static_cast<socklen_t>(sizeof(noSigPipe)));
#endif
#endif

    // A larger kernel buffer helps keep a 2.5G link fed.
    int bufSize = 4 * 1024 * 1024;
#ifdef _WIN32
    setsockopt(s, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&bufSize), sizeof(bufSize));
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&bufSize), sizeof(bufSize));
#else
    setsockopt(s, SOL_SOCKET, SO_SNDBUF, &bufSize, static_cast<socklen_t>(sizeof(bufSize)));
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, &bufSize, static_cast<socklen_t>(sizeof(bufSize)));
#endif
}

}  // namespace

WsaContext::WsaContext() {
#ifdef _WIN32
    WSADATA wsaData{};
    const int rc = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (rc != 0) {
        throw std::runtime_error("WSAStartup failed: " + std::to_string(rc));
    }
#endif
}

WsaContext::~WsaContext() {
#ifdef _WIN32
    WSACleanup();
#endif
}

SocketHandle::~SocketHandle() {
    Reset();
}

SocketHandle::SocketHandle(SocketHandle&& other) noexcept {
    raw_ = other.raw_;
    other.raw_ = kInvalidSocket;
}

SocketHandle& SocketHandle::operator=(SocketHandle&& other) noexcept {
    if (this != &other) {
        Reset();
        raw_ = other.raw_;
        other.raw_ = kInvalidSocket;
    }
    return *this;
}

SocketNative SocketHandle::Release() {
    SocketNative raw = raw_;
    raw_ = kInvalidSocket;
    return raw;
}

void SocketHandle::Reset(SocketNative raw) {
    if (raw_ != kInvalidSocket) {
#ifdef _WIN32
        closesocket(raw_);
#else
        close(raw_);
#endif
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
#ifdef _WIN32
    setsockopt(listener.Get(), IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&no), sizeof(no));
#else
    setsockopt(listener.Get(), IPPROTO_IPV6, IPV6_V6ONLY, &no, static_cast<socklen_t>(sizeof(no)));
#endif

    int reuse = 1;
#ifdef _WIN32
    setsockopt(listener.Get(), SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
#else
    setsockopt(listener.Get(), SOL_SOCKET, SO_REUSEADDR, &reuse, static_cast<socklen_t>(sizeof(reuse)));
#endif

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
    SocketNative client = accept(listener.Get(), nullptr, nullptr);
    if (client == kInvalidSocket) {
        throw std::runtime_error(LastSocketError("accept failed"));
    }
    TuneSocketForThroughput(client);
    return SocketHandle(client);
}

void SendAll(const SocketHandle& socket, const void* data, size_t length) {
    const uint8_t* cursor = static_cast<const uint8_t*>(data);
    size_t remaining = length;
    while (remaining > 0) {
#ifdef _WIN32
        const int sent = send(socket.Get(), reinterpret_cast<const char*>(cursor), static_cast<int>(remaining), 0);
#else
        const ssize_t sent = send(socket.Get(), cursor, remaining,
#if defined(MSG_NOSIGNAL)
                                  MSG_NOSIGNAL
#else
                                  0
#endif
        );
#endif
        if (sent <= 0) {
            throw std::runtime_error(LastSocketError("send failed"));
        }
        cursor += sent;
        remaining -= static_cast<size_t>(sent);
    }
}

void SendBuffersAll(const SocketHandle& socket, const SocketBuffer* buffers, size_t count) {
    if (count == 0) {
        return;
    }
#ifdef _WIN32
    std::vector<WSABUF> pending;
    pending.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        pending.push_back(WSABUF{
            static_cast<ULONG>(buffers[i].len),
            reinterpret_cast<char*>(const_cast<uint8_t*>(buffers[i].data))});
    }
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
#else
    constexpr size_t kIovLimit =
#if defined(IOV_MAX)
        static_cast<size_t>(IOV_MAX);
#elif defined(UIO_MAXIOV)
        static_cast<size_t>(UIO_MAXIOV);
#else
        1024;
#endif
    std::vector<iovec> pending;
    pending.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        if (buffers[i].len == 0) {
            continue;
        }
        pending.push_back(iovec{
            const_cast<void*>(reinterpret_cast<const void*>(buffers[i].data)),
            buffers[i].len});
    }
    size_t cursor = 0;
    while (cursor < pending.size()) {
        const int iovMax = static_cast<int>(std::min<size_t>(pending.size() - cursor, kIovLimit));
        const ssize_t sent = writev(socket.Get(), pending.data() + cursor, iovMax);
        if (sent <= 0) {
            throw std::runtime_error(LastSocketError("writev failed"));
        }
        size_t consumed = static_cast<size_t>(sent);
        while (cursor < pending.size() && consumed >= pending[cursor].iov_len) {
            consumed -= pending[cursor].iov_len;
            ++cursor;
        }
        if (cursor < pending.size() && consumed > 0) {
            pending[cursor].iov_base = static_cast<char*>(pending[cursor].iov_base) + consumed;
            pending[cursor].iov_len -= consumed;
        }
    }
#endif
}

void RecvAll(const SocketHandle& socket, void* data, size_t length) {
    uint8_t* cursor = static_cast<uint8_t*>(data);
    size_t remaining = length;
    while (remaining > 0) {
#ifdef _WIN32
        const int recved = recv(socket.Get(), reinterpret_cast<char*>(cursor), static_cast<int>(remaining), 0);
#else
        const ssize_t recved = recv(socket.Get(), cursor, remaining, 0);
#endif
        if (recved <= 0) {
            throw std::runtime_error(LastSocketError("recv failed"));
        }
        cursor += recved;
        remaining -= static_cast<size_t>(recved);
    }
}

void ShutdownBoth(const SocketHandle& socket) {
    if (!socket.Valid()) {
        return;
    }
#ifdef _WIN32
    shutdown(socket.Get(), SD_BOTH);
#else
    shutdown(socket.Get(), SHUT_RDWR);
#endif
}

}  // namespace fc
