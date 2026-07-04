// glibc hides `struct tcp_info` / `TCP_INFO` behind __USE_MISC in <netinet/tcp.h>; a strict
// -std=c++20 build (CMAKE_CXX_EXTENSIONS OFF) leaves _DEFAULT_SOURCE off, so request the GNU
// feature set on Linux BEFORE any system header is pulled in (QueryTcpDiag needs the full
// struct). Must precede the win_socket.h include, which transitively includes <sys/types.h>.
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE 1
#endif

#include "win_socket.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#include <mstcpip.h>  // SIO_TCP_INFO / TCP_INFO_v0 (Win10 1703+)
#pragma comment(lib, "Ws2_32.lib")
#else
#include <arpa/inet.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>
#endif

namespace fc {

namespace {

// TCP socket-buffer overrides (bytes); 0 = leave to kernel autotuning. Configured once at
// startup via SetSocketBufferOverrides and read by TuneSocketForThroughput on every socket.
std::atomic<int> g_sndBufOverride{0};
std::atomic<int> g_rcvBufOverride{0};

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

    // Socket window policy (design wan-single-tcp section 3.1). The old code unconditionally pinned
    // SO_SNDBUF=SO_RCVBUF=4MB. Pinning SO_RCVBUF is what DISABLED the kernel's receive-window
    // autotuning (Linux tcp_moderate_rcvbuf / Windows Receive Window Auto-Tuning), capping the
    // receive window at 4MB and the single-connection throughput at ~4MB/RTT on high-RTT links.
    //
    // New default: leave SO_RCVBUF UNSET so autotuning scales the window to the BDP. Send side
    // differs by platform: Linux autotunes SO_SNDBUF too (leave unset), but Windows does NOT
    // autotune the send buffer and defaults to ~64KB -- which would throttle a blocking sender
    // to 64KB in flight -- so Windows keeps an explicit, generous send buffer. Either direction
    // can still be pinned via SetSocketBufferOverrides (--tcp-send-buffer / --tcp-recv-buffer).
    const int sndOverride = g_sndBufOverride.load(std::memory_order_relaxed);
    const int rcvOverride = g_rcvBufOverride.load(std::memory_order_relaxed);

#ifdef _WIN32
    constexpr int kWinDefaultSndBuf = 16 * 1024 * 1024;
    const int sndBuf = (sndOverride > 0) ? sndOverride : kWinDefaultSndBuf;
    setsockopt(s, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&sndBuf), sizeof(sndBuf));
    if (rcvOverride > 0) {
        setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&rcvOverride), sizeof(rcvOverride));
    }
#else
    if (sndOverride > 0) {
        setsockopt(s, SOL_SOCKET, SO_SNDBUF, &sndOverride, static_cast<socklen_t>(sizeof(sndOverride)));
    }
    if (rcvOverride > 0) {
        setsockopt(s, SOL_SOCKET, SO_RCVBUF, &rcvOverride, static_cast<socklen_t>(sizeof(rcvOverride)));
    }
#endif
}

}  // namespace

void SetSocketBufferOverrides(int sndBufBytes, int rcvBufBytes) {
    g_sndBufOverride.store(sndBufBytes > 0 ? sndBufBytes : 0, std::memory_order_relaxed);
    g_rcvBufOverride.store(rcvBufBytes > 0 ? rcvBufBytes : 0, std::memory_order_relaxed);
}

TcpDiag QueryTcpDiag(SocketNative s) {
    TcpDiag d;
    if (s == kInvalidSocket) {
        return d;
    }
#ifdef _WIN32
    TCP_INFO_v0 info{};
    DWORD infoVersion = 0;
    DWORD bytesReturned = 0;
    if (WSAIoctl(s, SIO_TCP_INFO, &infoVersion, sizeof(infoVersion), &info, sizeof(info),
                 &bytesReturned, nullptr, nullptr) == 0) {
        d.valid = true;
        d.cwndBytes = info.Cwnd;
        d.rttUs = info.RttUs;
        d.retrans = info.BytesRetrans;
        d.bytesInFlight = info.BytesInFlight;
        d.mss = info.Mss;
    }
#elif defined(__linux__)
    struct tcp_info ti{};
    socklen_t len = static_cast<socklen_t>(sizeof(ti));
    if (getsockopt(s, IPPROTO_TCP, TCP_INFO, &ti, &len) == 0) {
        d.valid = true;
        d.mss = ti.tcpi_snd_mss;
        d.cwndBytes = static_cast<uint64_t>(ti.tcpi_snd_cwnd) * ti.tcpi_snd_mss;  // cwnd is in segments
        d.rttUs = ti.tcpi_rtt;                                                    // microseconds
        d.retrans = ti.tcpi_total_retrans;                                        // segments
        d.bytesInFlight = static_cast<uint64_t>(ti.tcpi_unacked) * ti.tcpi_snd_mss;
    }
#elif defined(__APPLE__)
    // macOS/BSD exposes TCP_CONNECTION_INFO + struct tcp_connection_info instead of Linux's
    // TCP_INFO/tcp_info. cwnd is already in bytes here; srtt is in milliseconds; retrans is a
    // packet count (matches the "seg" unit label). In-flight bytes are not exposed -> left 0.
    struct tcp_connection_info ti{};
    socklen_t len = static_cast<socklen_t>(sizeof(ti));
    if (getsockopt(s, IPPROTO_TCP, TCP_CONNECTION_INFO, &ti, &len) == 0) {
        d.valid = true;
        d.mss = ti.tcpi_maxseg;
        d.cwndBytes = ti.tcpi_snd_cwnd;                            // already bytes on macOS
        d.rttUs = ti.tcpi_srtt * 1000u;                            // srtt is in milliseconds
        d.retrans = ti.tcpi_txretransmitpackets;                   // retransmitted packets
        d.bytesInFlight = 0;                                       // not exposed by this struct
    }
#else
    (void)s;  // other platforms: kernel TCP diagnostics unavailable -> valid stays false
#endif
    return d;
}

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

namespace {

// Apply the per-OS multipath source binding to a fresh socket, between socket() and
// connect() (design section 6.5). Returns false only on a hard failure that should disqualify
// this address-family attempt. addrFamily is the family of the socket being bound.
bool ApplyConnectBinding(SocketNative s, int addrFamily, const ConnectBinding& binding) {
    // Egress interface pin (weak-host platforms). Windows uses strong-host source-IP
    // binding instead, so ifaceName is a no-op there.
    if (!binding.ifaceName.empty()) {
#if defined(__APPLE__)
        const unsigned int idx = if_nametoindex(binding.ifaceName.c_str());
        if (idx != 0) {
            if (addrFamily == AF_INET6) {
                setsockopt(s, IPPROTO_IPV6, IPV6_BOUND_IF, &idx, sizeof(idx));
            } else {
                setsockopt(s, IPPROTO_IP, IP_BOUND_IF, &idx, sizeof(idx));
            }
        }
#elif defined(__linux__)
        // SO_BINDTODEVICE needs CAP_NET_RAW/root; on failure we degrade to source-IP
        // binding below and only warn (design risk R-03, gatekeeper-approved default).
        if (setsockopt(s, SOL_SOCKET, SO_BINDTODEVICE, binding.ifaceName.c_str(),
                       static_cast<socklen_t>(binding.ifaceName.size())) != 0) {
            // Non-fatal: continue with source-IP bind / default route.
        }
#endif
    }

    // Source-IP bind (strong-host on Windows; supplemental elsewhere).
    if (!binding.localAddr.empty()) {
        addrinfo hints{};
        hints.ai_family = addrFamily;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;
        hints.ai_flags = AI_NUMERICHOST | AI_PASSIVE;
        addrinfo* localInfo = nullptr;
        if (getaddrinfo(binding.localAddr.c_str(), nullptr, &hints, &localInfo) != 0 ||
            localInfo == nullptr) {
            return false;  // local address not valid for this family -> skip attempt
        }
        const int rc = bind(s, localInfo->ai_addr, static_cast<int>(localInfo->ai_addrlen));
        freeaddrinfo(localInfo);
        if (rc != 0) {
            return false;  // cannot bind the requested source on this family
        }
    }
    return true;
}

SocketHandle ConnectToImpl(const std::string& host, uint16_t port, const ConnectBinding& binding) {
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
        if (!ApplyConnectBinding(attempt.Get(), p->ai_family, binding)) {
            continue;  // binding incompatible with this address family; try the next one
        }
        TuneSocketForThroughput(attempt.Get());
        if (connect(attempt.Get(), p->ai_addr, static_cast<int>(p->ai_addrlen)) == 0) {
            connected = std::move(attempt);
            break;
        }
    }
    freeaddrinfo(result);

    if (!connected.Valid()) {
        std::string detail = host + ":" + std::to_string(port);
        if (!binding.localAddr.empty()) {
            detail += " (bind " + binding.localAddr + ")";
        }
        throw std::runtime_error("connect failed to " + detail);
    }
    return connected;
}

}  // namespace

SocketHandle ConnectTo(const std::string& host, uint16_t port) {
    return ConnectToImpl(host, port, ConnectBinding{});
}

SocketHandle ConnectTo(const std::string& host, uint16_t port, const ConnectBinding& binding) {
    return ConnectToImpl(host, port, binding);
}

std::string LocalAddressOf(const SocketHandle& socket) {
    if (!socket.Valid()) {
        return std::string();
    }
    sockaddr_storage ss{};
#ifdef _WIN32
    int len = sizeof(ss);
#else
    socklen_t len = sizeof(ss);
#endif
    if (getsockname(socket.Get(), reinterpret_cast<sockaddr*>(&ss), &len) != 0) {
        return std::string();
    }
    char host[NI_MAXHOST] = {0};
    if (getnameinfo(reinterpret_cast<sockaddr*>(&ss), len, host, sizeof(host), nullptr, 0,
                    NI_NUMERICHOST) != 0) {
        return std::string();
    }
    return std::string(host);
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

#ifdef _WIN32
    // SO_EXCLUSIVEADDRUSE: bind fails with WSAEADDRINUSE if another socket already holds
    // the port. The previous SO_REUSEADDR let a second fastclone server silently bind over
    // an in-use port, making it appear to listen while actually conflicting with the holder
    // (two listeners on one port). Exclusive use makes port-in-use a hard, visible error.
    int exclusive = 1;
    setsockopt(listener.Get(), SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
               reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
#else
    // POSIX SO_REUSEADDR only allows reusing a port in TIME_WAIT; bind still fails with
    // EADDRINUSE against a live listener, so it is safe and standard for server restart.
    int reuse = 1;
    setsockopt(listener.Get(), SOL_SOCKET, SO_REUSEADDR, &reuse, static_cast<socklen_t>(sizeof(reuse)));
#endif

    sockaddr_in6 addr{};
    addr.sin6_family = AF_INET6;
    addr.sin6_addr = in6addr_any;
    addr.sin6_port = htons(port);
    if (bind(listener.Get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        throw std::runtime_error(LastSocketError("bind failed"));
    }
    if (listen(listener.Get(), SOMAXCONN) != 0) {
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

std::optional<SocketHandle> AcceptClientTimeout(const SocketHandle& listener, int timeoutMs) {
#ifdef _WIN32
    WSAPOLLFD pfd{};
    pfd.fd = listener.Get();
    pfd.events = POLLRDNORM;
    const int rc = WSAPoll(&pfd, 1, timeoutMs);
    if (rc == SOCKET_ERROR) {
        throw std::runtime_error(LastSocketError("WSAPoll failed"));
    }
#else
    struct pollfd pfd{};
    pfd.fd = listener.Get();
    pfd.events = POLLIN;
    int rc;
    do {
        rc = poll(&pfd, 1, timeoutMs);
    } while (rc < 0 && errno == EINTR);
    if (rc < 0) {
        throw std::runtime_error(LastSocketError("poll failed"));
    }
#endif
    if (rc == 0) {
        return std::nullopt;  // timeout: no pending connection this tick
    }
    return AcceptClient(listener);
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
