#include "net_topology.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstring>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>

#ifdef _WIN32
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <iphlpapi.h>
#include <bcrypt.h>
#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "bcrypt.lib")
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <netinet/in.h>
#include <net/if.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/random.h>
#endif
#include <fstream>
#endif

namespace fc {

namespace {

std::string ToLowerCopy(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

std::string HexEncode(const unsigned char* data, size_t len) {
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        out.push_back(kHex[(data[i] >> 4) & 0xF]);
        out.push_back(kHex[data[i] & 0xF]);
    }
    return out;
}

// Resolve a textual numeric address (host[:no port]) into a sockaddr for bind(). Returns
// false if the literal is not a valid numeric IP. family is set on success.
bool ResolveNumericAddr(const std::string& ip, addrinfo*& out) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_NUMERICHOST | AI_PASSIVE;
    out = nullptr;
    return getaddrinfo(ip.c_str(), nullptr, &hints, &out) == 0 && out != nullptr;
}

}  // namespace

std::string GenerateSessionToken() {
    constexpr size_t kTokenBytes = 32;  // 256-bit (> 128-bit requirement)
    unsigned char buf[kTokenBytes];
#ifdef _WIN32
    const NTSTATUS rc = BCryptGenRandom(nullptr, buf, static_cast<ULONG>(kTokenBytes),
                                        BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (rc != 0) {
        // Fail closed: an unguessable token is a security requirement (NFR-007).
        throw std::runtime_error("BCryptGenRandom failed");
    }
#else
    bool ok = false;
#if defined(__linux__)
    ssize_t got = getrandom(buf, kTokenBytes, 0);
    ok = (got == static_cast<ssize_t>(kTokenBytes));
#endif
    if (!ok) {
        std::ifstream urandom("/dev/urandom", std::ios::binary);
        if (urandom && urandom.read(reinterpret_cast<char*>(buf), kTokenBytes)) {
            ok = true;
        }
    }
    if (!ok) {
        throw std::runtime_error("Failed to read cryptographic random bytes");
    }
#endif
    return HexEncode(buf, kTokenBytes);
}

bool IsExcludedLocalAddress(const std::string& ipRaw) {
    if (ipRaw.empty()) {
        return true;
    }
    // Drop any IPv6 zone suffix ("fe80::1%eth0") before classification.
    std::string ip = ipRaw;
    const size_t zone = ip.find('%');
    if (zone != std::string::npos) {
        ip = ip.substr(0, zone);
    }
    const std::string lower = ToLowerCopy(ip);
    if (lower.rfind("127.", 0) == 0) {
        return true;  // IPv4 loopback
    }
    if (lower.rfind("169.254.", 0) == 0) {
        return true;  // IPv4 link-local
    }
    if (lower == "0.0.0.0" || lower == "::" || lower == "::1") {
        return true;  // wildcard / IPv6 loopback
    }
    // IPv6 link-local fe80::/10 -> fe8x .. febx.
    if (lower.rfind("fe8", 0) == 0 || lower.rfind("fe9", 0) == 0 ||
        lower.rfind("fea", 0) == 0 || lower.rfind("feb", 0) == 0) {
        return true;
    }
    return false;
}

std::vector<LocalAddress> EnumerateLocalCandidates() {
    std::vector<LocalAddress> result;
    std::set<std::string> seen;
    auto add = [&](const std::string& ip, const std::string& ifaceKey,
                   const std::string& friendlyName = {}, int prefixLen = 0) {
        if (IsExcludedLocalAddress(ip)) {
            return;
        }
        if (seen.insert(ip).second) {
            result.push_back(LocalAddress{ip, ifaceKey, friendlyName, prefixLen});
        }
    };

#ifdef _WIN32
    ULONG bufLen = 16 * 1024;
    std::vector<unsigned char> buffer(bufLen);
    const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG rc = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr,
                                    reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &bufLen);
    if (rc == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(bufLen);
        rc = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr,
                                  reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &bufLen);
    }
    if (rc != NO_ERROR) {
        return result;
    }
    for (auto* adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
         adapter != nullptr; adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp) {
            continue;  // not-up interface (AC-003)
        }
        if (adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) {
            continue;
        }
        // Adapter name (GUID) is the per-physical-NIC stable key: identical for every
        // unicast address of this adapter across families, so v4+v6 collapse to one NIC.
        const std::string ifaceKey =
            adapter->AdapterName != nullptr ? std::string(adapter->AdapterName) : std::string();
        // Human-readable adapter name for diagnostic logging.
        std::string friendlyName;
        if (adapter->FriendlyName != nullptr) {
            const int len = WideCharToMultiByte(CP_UTF8, 0, adapter->FriendlyName, -1,
                                                nullptr, 0, nullptr, nullptr);
            if (len > 0) {
                friendlyName.resize(static_cast<size_t>(len - 1));
                WideCharToMultiByte(CP_UTF8, 0, adapter->FriendlyName, -1,
                                    friendlyName.data(), len, nullptr, nullptr);
            }
        }
        for (auto* unicast = adapter->FirstUnicastAddress; unicast != nullptr;
             unicast = unicast->Next) {
            const sockaddr* sa = unicast->Address.lpSockaddr;
            if (sa == nullptr) {
                continue;
            }
            // OnLinkPrefixLength is the on-link subnet prefix of this address; feed it into
            // SelectAutoLinks' same-subnet scoring (§4.3). LH fields exist on Vista+.
            const IP_ADAPTER_UNICAST_ADDRESS_LH* lh =
                reinterpret_cast<const IP_ADAPTER_UNICAST_ADDRESS_LH*>(unicast);
            const int prefixLen = static_cast<int>(lh->OnLinkPrefixLength);
            char host[NI_MAXHOST] = {0};
            if (getnameinfo(sa, unicast->Address.iSockaddrLength, host, sizeof(host),
                            nullptr, 0, NI_NUMERICHOST) == 0) {
                add(host, ifaceKey, friendlyName, prefixLen);
            }
        }
    }
#else
    ifaddrs* ifaddr = nullptr;
    if (getifaddrs(&ifaddr) != 0 || ifaddr == nullptr) {
        return result;
    }
    for (ifaddrs* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == nullptr) {
            continue;
        }
        if ((ifa->ifa_flags & IFF_UP) == 0 || (ifa->ifa_flags & IFF_RUNNING) == 0) {
            continue;  // not-up / not-running interface (AC-003)
        }
        if ((ifa->ifa_flags & IFF_LOOPBACK) != 0) {
            continue;
        }
        const int family = ifa->ifa_addr->sa_family;
        if (family != AF_INET && family != AF_INET6) {
            continue;
        }
        // if_nametoindex is the per-physical-NIC stable key (same for v4+v6 on one NIC);
        // fall back to the interface name when the index is unavailable.
        std::string ifaceKey;
        if (ifa->ifa_name != nullptr) {
            const unsigned int idx = if_nametoindex(ifa->ifa_name);
            ifaceKey = (idx != 0) ? std::to_string(idx) : std::string(ifa->ifa_name);
        }
        // Best-effort on-link prefix length from the netmask (§4.3); 0 when unavailable.
        int prefixLen = 0;
        if (ifa->ifa_netmask != nullptr) {
            if (family == AF_INET) {
                const auto* m = reinterpret_cast<const sockaddr_in*>(ifa->ifa_netmask);
                uint32_t mask = ntohl(m->sin_addr.s_addr);
                while (mask & 0x80000000u) {
                    ++prefixLen;
                    mask <<= 1;
                }
            } else {
                const auto* m = reinterpret_cast<const sockaddr_in6*>(ifa->ifa_netmask);
                for (int b = 0; b < 16; ++b) {
                    unsigned char byte = m->sin6_addr.s6_addr[b];
                    while (byte & 0x80u) {
                        ++prefixLen;
                        byte = static_cast<unsigned char>(byte << 1);
                    }
                }
            }
        }
        char host[NI_MAXHOST] = {0};
        const socklen_t salen = (family == AF_INET) ? sizeof(sockaddr_in) : sizeof(sockaddr_in6);
        if (getnameinfo(ifa->ifa_addr, salen, host, sizeof(host), nullptr, 0,
                        NI_NUMERICHOST) == 0) {
            add(host, ifaceKey, std::string(), prefixLen);
        }
    }
    freeifaddrs(ifaddr);
#endif
    return result;
}

std::vector<std::string> EnumerateLocalAddresses() {
    std::vector<std::string> ips;
    for (const LocalAddress& cand : EnumerateLocalCandidates()) {
        ips.push_back(cand.ip);
    }
    return ips;
}

std::string InterfaceKeyForLocalAddress(const std::string& ip) {
    if (ip.empty()) {
        return std::string();
    }
    for (const LocalAddress& cand : EnumerateLocalCandidates()) {
        if (cand.ip == ip) {
            return cand.ifaceKey;
        }
    }
    return std::string();
}

std::vector<LocalAddress> EnumerateProbeCandidates() {
#ifdef _WIN32
    ULONG bufLen = 16 * 1024;
    std::vector<unsigned char> buffer(bufLen);
    const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG rc = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr,
                                    reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &bufLen);
    if (rc == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(bufLen);
        rc = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr,
                                  reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &bufLen);
    }
    if (rc != NO_ERROR) {
        return EnumerateLocalCandidates();  // fallback
    }

    std::vector<LocalAddress> result;
    std::set<std::string> seenIps;
    // Track (ifaceKey, address-family) pairs to keep at most one stable address per NIC per family.
    std::set<std::pair<std::string, int>> seenNicFamily;

    for (auto* adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
         adapter != nullptr; adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp) {
            continue;
        }
        if (adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) {
            continue;
        }
        const std::string ifaceKey =
            adapter->AdapterName != nullptr ? std::string(adapter->AdapterName) : std::string();
        std::string friendlyName;
        if (adapter->FriendlyName != nullptr) {
            const int len = WideCharToMultiByte(CP_UTF8, 0, adapter->FriendlyName, -1,
                                                nullptr, 0, nullptr, nullptr);
            if (len > 0) {
                friendlyName.resize(static_cast<size_t>(len - 1));
                WideCharToMultiByte(CP_UTF8, 0, adapter->FriendlyName, -1,
                                    friendlyName.data(), len, nullptr, nullptr);
            }
        }
        for (auto* unicast = adapter->FirstUnicastAddress; unicast != nullptr;
             unicast = unicast->Next) {
            const sockaddr* sa = unicast->Address.lpSockaddr;
            if (sa == nullptr) {
                continue;
            }
            // IP_ADAPTER_UNICAST_ADDRESS_LH fields available on Vista+ (NTDDI_LONGHORN).
            const IP_ADAPTER_UNICAST_ADDRESS_LH* lh =
                reinterpret_cast<const IP_ADAPTER_UNICAST_ADDRESS_LH*>(unicast);
            // Skip deprecated addresses (their lifetime has expired).
            if (lh->DadState == IpDadStateDeprecated) {
                continue;
            }
            // Skip temporary / privacy-extension IPv6 addresses (random suffix).
            if (lh->SuffixOrigin == IpSuffixOriginRandom) {
                continue;
            }
            char host[NI_MAXHOST] = {0};
            if (getnameinfo(sa, unicast->Address.iSockaddrLength, host, sizeof(host),
                            nullptr, 0, NI_NUMERICHOST) != 0) {
                continue;
            }
            // Apply address-level exclusion BEFORE the per-(NIC, family) dedup so that
            // a link-local address (169.254.x.x / fe80::) appearing first in the adapter
            // list does NOT consume the NIC slot and block the valid stable address that
            // follows on the same adapter.
            if (IsExcludedLocalAddress(host)) {
                continue;
            }
            const int family = sa->sa_family;
            // Keep only the first stable, non-excluded address per (NIC, address-family);
            // subsequent addresses on the same NIC+family would produce redundant probe rows.
            if (!seenNicFamily.insert({ifaceKey, family}).second) {
                continue;
            }
            if (seenIps.insert(host).second) {
                result.push_back(LocalAddress{host, ifaceKey, friendlyName,
                                              static_cast<int>(lh->OnLinkPrefixLength)});
            }
        }
    }
    return result;
#else
    // POSIX: no reliable cross-platform way to detect temporary/privacy addresses;
    // fall back to the full candidate list (best-effort).
    return EnumerateLocalCandidates();
#endif
}

namespace {

// Portable probe-socket primitives. Keeps the rest of the concurrent core free of per-OS
// socket-type / close / poll branching.
#ifdef _WIN32
using probe_socket_t = SOCKET;
using probe_pollfd_t = WSAPOLLFD;
inline bool ProbeSocketValid(probe_socket_t s) { return s != INVALID_SOCKET; }
inline void CloseProbeSocket(probe_socket_t s) { closesocket(s); }
inline void SetProbeNonBlocking(probe_socket_t s) {
    u_long nonBlock = 1;
    ioctlsocket(s, FIONBIO, &nonBlock);
}
// True when a non-blocking connect() has not yet completed (still in progress).
inline bool ConnectInProgress() { return WSAGetLastError() == WSAEWOULDBLOCK; }
inline bool PollWasInterrupted() { return WSAGetLastError() == WSAEINTR; }
inline int ProbePoll(probe_pollfd_t* fds, size_t n, int timeoutMs) {
    return WSAPoll(fds, static_cast<ULONG>(n), timeoutMs);
}
inline int ProbeConnect(probe_socket_t s, const sockaddr* addr, size_t addrlen) {
    return connect(s, addr, static_cast<int>(addrlen));
}
inline int ProbeBind(probe_socket_t s, const sockaddr* addr, size_t addrlen) {
    return bind(s, addr, static_cast<int>(addrlen));
}
inline bool ProbeSoError(probe_socket_t s) {
    int soErr = 0;
    int len = sizeof(soErr);
    return getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soErr), &len) == 0 &&
           soErr == 0;
}
#else
using probe_socket_t = int;
using probe_pollfd_t = struct pollfd;
inline bool ProbeSocketValid(probe_socket_t s) { return s >= 0; }
inline void CloseProbeSocket(probe_socket_t s) { close(s); }
inline void SetProbeNonBlocking(probe_socket_t s) {
    const int fl = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, fl | O_NONBLOCK);
}
inline bool ConnectInProgress() { return errno == EINPROGRESS; }
inline bool PollWasInterrupted() { return errno == EINTR; }
inline int ProbePoll(probe_pollfd_t* fds, size_t n, int timeoutMs) {
    return poll(fds, static_cast<nfds_t>(n), timeoutMs);
}
inline int ProbeConnect(probe_socket_t s, const sockaddr* addr, size_t addrlen) {
    return connect(s, addr, static_cast<socklen_t>(addrlen));
}
inline int ProbeBind(probe_socket_t s, const sockaddr* addr, size_t addrlen) {
    return bind(s, addr, static_cast<socklen_t>(addrlen));
}
inline bool ProbeSoError(probe_socket_t s) {
    int soErr = 0;
    socklen_t len = sizeof(soErr);
    return getsockopt(s, SOL_SOCKET, SO_ERROR, &soErr, &len) == 0 && soErr == 0;
}
#endif

// One concurrent "socket-level" probe attempt. A single matrix cell (i,j) expands into one
// attempt per same-family server address (D-01: all candidates raced, first to connect wins).
struct ProbeAttempt {
    size_t i = 0;                                       // row (localAddr)
    size_t j = 0;                                       // column (serverEndpoint)
    probe_socket_t fd{};                                // non-blocking socket
    std::chrono::steady_clock::time_point startTime;    // connect() instant (rtt origin)
    bool pending = false;                               // true while waiting for POLLOUT verdict
};

// Stamp a successful attempt into its cell: first success sets reachable+rtt; a later success
// for the same cell keeps the smaller rtt ("best latency" semantics, never re-clears).
void RecordReachable(ReachabilityMatrix& matrix, const ProbeAttempt& a) {
    const long rtt = static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - a.startTime).count());
    ReachabilityCell& cell = matrix.cells[a.i][a.j];
    if (!cell.reachable) {
        cell.reachable = true;
        cell.rttMs = rtt;
    } else if (rtt < cell.rttMs) {
        cell.rttMs = rtt;
    }
}

// Phase B: resolve every (i,j), create a non-blocking socket per same-family server address,
// optionally bind the source, and fire connect(). Immediate successes are recorded in-place;
// in-progress connects are appended to `pending`. socket/bind/cross-family failures contribute
// nothing (the cell keeps its default reachable=false / rttMs=-1).
void LaunchAttempts(ReachabilityMatrix& matrix, std::vector<ProbeAttempt>& pending) {
    const size_t rows = matrix.localAddrs.size();
    const size_t cols = matrix.serverEndpoints.size();
    for (size_t i = 0; i < rows; ++i) {
        const std::string& localAddr = matrix.localAddrs[i];
        const bool haveLocal = !localAddr.empty();
        for (size_t j = 0; j < cols; ++j) {
            const ServerEndpoint& server = matrix.serverEndpoints[j];
            addrinfo hints{};
            hints.ai_family = AF_UNSPEC;
            hints.ai_socktype = SOCK_STREAM;
            hints.ai_protocol = IPPROTO_TCP;
            addrinfo* servInfo = nullptr;
            const std::string portStr = std::to_string(server.port);
            if (getaddrinfo(server.host.c_str(), portStr.c_str(), &hints, &servInfo) != 0 ||
                servInfo == nullptr) {
                continue;  // unresolved server -> cell stays unreachable
            }
            // Resolve the source once per cell; it must stay valid for every bind() below.
            addrinfo* localInfo = nullptr;
            if (haveLocal && !ResolveNumericAddr(localAddr, localInfo)) {
                freeaddrinfo(servInfo);
                continue;  // unparseable source -> cell stays unreachable
            }
            for (addrinfo* p = servInfo; p != nullptr; p = p->ai_next) {
                // Cross-family pairs are never connected (== unreachable contribution).
                if (haveLocal && localInfo->ai_family != p->ai_family) {
                    continue;
                }
                probe_socket_t s = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
                if (!ProbeSocketValid(s)) {
                    continue;  // immediate unreachable contribution
                }
                SetProbeNonBlocking(s);
                if (haveLocal &&
                    ProbeBind(s, localInfo->ai_addr, localInfo->ai_addrlen) != 0) {
                    CloseProbeSocket(s);
                    continue;  // bind failure -> immediate unreachable contribution
                }
                ProbeAttempt attempt;
                attempt.i = i;
                attempt.j = j;
                attempt.fd = s;
                attempt.startTime = std::chrono::steady_clock::now();
                const int cr = ProbeConnect(s, p->ai_addr, p->ai_addrlen);
                if (cr == 0) {
                    // Immediate success (e.g. loopback): record and close, never poll.
                    RecordReachable(matrix, attempt);
                    CloseProbeSocket(s);
                } else if (ConnectInProgress()) {
                    attempt.pending = true;
                    pending.push_back(attempt);
                } else {
                    CloseProbeSocket(s);  // other immediate error -> attempt failed
                }
            }
            if (localInfo != nullptr) {
                freeaddrinfo(localInfo);
            }
            freeaddrinfo(servInfo);
        }
    }
}

// Phase C: a single WSAPoll/poll loop over all pending attempts, bounded by one shared
// deadline (wall-clock ~= one timeoutMs window regardless of unreachable-cell count). Phase D:
// any attempt still pending when the window closes is force-closed (no fd leak).
void HarvestAttempts(ReachabilityMatrix& matrix, std::vector<ProbeAttempt>& pending,
                     int timeoutMs) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    std::vector<probe_pollfd_t> pfds;
    std::vector<size_t> backref;  // pfds[k] -> index into `pending`
    while (true) {
        bool anyPending = false;
        for (const ProbeAttempt& a : pending) {
            if (a.pending) {
                anyPending = true;
                break;
            }
        }
        if (!anyPending) {
            break;
        }
        long remaining = static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count());
        if (remaining <= 0) {
            break;  // window exhausted
        }
        if (remaining > INT_MAX) {
            remaining = INT_MAX;
        }
        // Rebuild pfds + backref from the current pending set each round (R-07: keep the
        // pfd<->attempt mapping consistent, never re-poll a closed fd).
        pfds.clear();
        backref.clear();
        for (size_t k = 0; k < pending.size(); ++k) {
            if (!pending[k].pending) {
                continue;
            }
            probe_pollfd_t pfd{};
            pfd.fd = pending[k].fd;
            pfd.events = POLLOUT;
            pfd.revents = 0;
            pfds.push_back(pfd);
            backref.push_back(k);
        }
        const int n = ProbePoll(pfds.data(), pfds.size(), static_cast<int>(remaining));
        if (n < 0) {
            if (PollWasInterrupted()) {
                continue;  // EINTR/WSAEINTR: retry with a freshly computed remaining (R-05)
            }
            break;  // rare hard error: remaining attempts stay unreachable
        }
        if (n == 0) {
            break;  // timeout: remaining attempts stay unreachable
        }
        for (size_t k = 0; k < pfds.size(); ++k) {
            // R-01: a failed connect may surface only as POLLERR/POLLHUP/POLLNVAL (not
            // POLLOUT), so any non-zero revents triggers the SO_ERROR verdict.
            if (pfds[k].revents == 0) {
                continue;
            }
            ProbeAttempt& a = pending[backref[k]];
            if (ProbeSoError(a.fd)) {
                RecordReachable(matrix, a);
            }
            CloseProbeSocket(a.fd);
            a.pending = false;
        }
    }
    // Phase D fallback: close any fd left open by a deadline/hard-error exit.
    for (ProbeAttempt& a : pending) {
        if (a.pending) {
            CloseProbeSocket(a.fd);
            a.pending = false;
        }
    }
}

}  // namespace

namespace {

ReachabilityMatrix ProbeReachabilityCore(std::vector<std::string> localAddrs,
                                         std::vector<std::string> localIfaces,
                                         std::vector<int> localPrefixLens,
                                         const std::vector<ServerEndpoint>& serverEndpoints,
                                         int timeoutMs) {
    ReachabilityMatrix matrix;
    if (localAddrs.empty()) {
        localAddrs.push_back(std::string());  // single OS-default row ("")
    }
    localIfaces.resize(localAddrs.size());      // keep interface keys parallel to rows
    localPrefixLens.resize(localAddrs.size());  // keep prefix lengths parallel to rows (0 = unknown)
    matrix.localAddrs = std::move(localAddrs);
    matrix.localIfaces = std::move(localIfaces);
    matrix.localPrefixLens = std::move(localPrefixLens);
    matrix.serverEndpoints = serverEndpoints;
    // Phase A: matrix skeleton. Every cell defaults to unreachable (reachable=false,
    // rttMs=-1); only a successful attempt overwrites it, so socket/bind/cross-family/timeout
    // failures need no explicit write.
    matrix.cells.resize(matrix.localAddrs.size());
    for (size_t i = 0; i < matrix.localAddrs.size(); ++i) {
        matrix.cells[i].resize(matrix.serverEndpoints.size());
    }

    // Phase B/C/D: fire every same-family (local, server) connect non-blocking, then wait for
    // all of them under one shared deadline. Wall-clock collapses from (failed cells x
    // timeoutMs) to ~one timeoutMs window.
    std::vector<ProbeAttempt> pending;
    LaunchAttempts(matrix, pending);
    HarvestAttempts(matrix, pending, timeoutMs);
    return matrix;
}

}  // namespace

ReachabilityMatrix ProbeReachability(const std::vector<std::string>& localAddrsIn,
                                     const std::vector<ServerEndpoint>& serverEndpoints,
                                     int timeoutMs) {
    // Legacy flat view: each row's interface key is the address literal itself, so dedup
    // stays IP-granular (no physical-NIC identity available from a bare IP list). Prefix
    // lengths are unknown (0), so same-subnet scoring uses the family default (§4.5).
    return ProbeReachabilityCore(localAddrsIn, localAddrsIn, std::vector<int>(),
                                 serverEndpoints, timeoutMs);
}

ReachabilityMatrix ProbeReachability(const std::vector<LocalAddress>& localAddrsIn,
                                     const std::vector<ServerEndpoint>& serverEndpoints,
                                     int timeoutMs) {
    std::vector<std::string> ips;
    std::vector<std::string> ifaces;
    std::vector<int> prefixLens;
    ips.reserve(localAddrsIn.size());
    ifaces.reserve(localAddrsIn.size());
    prefixLens.reserve(localAddrsIn.size());
    for (const LocalAddress& cand : localAddrsIn) {
        ips.push_back(cand.ip);
        ifaces.push_back(cand.ifaceKey);
        prefixLens.push_back(cand.prefixLen);
    }
    return ProbeReachabilityCore(std::move(ips), std::move(ifaces), std::move(prefixLens),
                                 serverEndpoints, timeoutMs);
}

namespace {

// Strip an IPv6 zone suffix ("fe80::1%eth0") and surrounding brackets ("[ip]") so the
// remaining literal can be parsed by the family-specific parsers below.
std::string StripZoneAndBrackets(const std::string& in) {
    std::string s = in;
    if (!s.empty() && s.front() == '[') {
        const size_t close = s.find(']');
        if (close != std::string::npos) {
            s = s.substr(1, close - 1);
        }
    }
    const size_t pct = s.find('%');
    if (pct != std::string::npos) {
        s = s.substr(0, pct);
    }
    return s;
}

bool ParseIPv4Bytes(const std::string& s, std::array<uint8_t, 4>& out) {
    int part = 0;
    int digits = 0;
    int octet = 0;
    for (char c : s) {
        if (c == '.') {
            if (digits == 0 || part >= 3) {
                return false;
            }
            out[static_cast<size_t>(part++)] = static_cast<uint8_t>(octet);
            octet = 0;
            digits = 0;
        } else if (c >= '0' && c <= '9') {
            octet = octet * 10 + (c - '0');
            if (++digits > 3 || octet > 255) {
                return false;
            }
        } else {
            return false;
        }
    }
    if (digits == 0 || part != 3) {
        return false;
    }
    out[3] = static_cast<uint8_t>(octet);
    return true;
}

// Parse a (non-embedded-IPv4) IPv6 literal, handling a single "::" zero-run compression.
bool ParseIPv6Bytes(const std::string& s, std::array<uint8_t, 16>& out) {
    out.fill(0);
    auto parseGroups = [](const std::string& part, std::vector<uint16_t>& groups) -> bool {
        if (part.empty()) {
            return true;
        }
        size_t start = 0;
        while (true) {
            const size_t colon = part.find(':', start);
            const std::string tok =
                (colon == std::string::npos) ? part.substr(start) : part.substr(start, colon - start);
            if (tok.empty() || tok.size() > 4) {
                return false;
            }
            uint16_t val = 0;
            for (char c : tok) {
                int d;
                if (c >= '0' && c <= '9') {
                    d = c - '0';
                } else if (c >= 'a' && c <= 'f') {
                    d = c - 'a' + 10;
                } else if (c >= 'A' && c <= 'F') {
                    d = c - 'A' + 10;
                } else {
                    return false;
                }
                val = static_cast<uint16_t>((val << 4) | d);
            }
            groups.push_back(val);
            if (colon == std::string::npos) {
                break;
            }
            start = colon + 1;
        }
        return true;
    };

    const size_t dc = s.find("::");
    const std::string head = (dc == std::string::npos) ? s : s.substr(0, dc);
    const std::string tail = (dc == std::string::npos) ? std::string() : s.substr(dc + 2);
    std::vector<uint16_t> headGroups;
    std::vector<uint16_t> tailGroups;
    if (!parseGroups(head, headGroups) || !parseGroups(tail, tailGroups)) {
        return false;
    }
    std::array<uint16_t, 8> words{};
    if (dc == std::string::npos) {
        if (headGroups.size() != 8) {
            return false;
        }
        for (size_t i = 0; i < 8; ++i) {
            words[i] = headGroups[i];
        }
    } else {
        if (headGroups.size() + tailGroups.size() > 7) {
            return false;  // "::" must stand for at least one zero group
        }
        for (size_t i = 0; i < headGroups.size(); ++i) {
            words[i] = headGroups[i];
        }
        for (size_t i = 0; i < tailGroups.size(); ++i) {
            words[8 - tailGroups.size() + i] = tailGroups[i];
        }
    }
    for (size_t i = 0; i < 8; ++i) {
        out[i * 2] = static_cast<uint8_t>(words[i] >> 8);
        out[i * 2 + 1] = static_cast<uint8_t>(words[i] & 0xFF);
    }
    return true;
}

bool SamePrefixBits(const uint8_t* a, const uint8_t* b, int bits) {
    const int fullBytes = bits / 8;
    const int remBits = bits % 8;
    for (int i = 0; i < fullBytes; ++i) {
        if (a[i] != b[i]) {
            return false;
        }
    }
    if (remBits != 0) {
        const uint8_t mask = static_cast<uint8_t>(0xFF << (8 - remBits));
        if ((a[fullBytes] & mask) != (b[fullBytes] & mask)) {
            return false;
        }
    }
    return true;
}

}  // namespace

int FamilyRank(const std::string& ip) {
    // ':' only appears in IPv6 literals; an empty/OS-default literal ranks as IPv4 (neutral).
    return ip.find(':') != std::string::npos ? 1 : 0;
}

bool SameSubnet(const std::string& clientIpRaw, int prefixLen, const std::string& serverHostRaw) {
    const std::string clientIp = StripZoneAndBrackets(clientIpRaw);
    const std::string serverHost = StripZoneAndBrackets(serverHostRaw);
    if (clientIp.empty() || serverHost.empty()) {
        return false;
    }
    const bool clientV6 = clientIp.find(':') != std::string::npos;
    const bool serverV6 = serverHost.find(':') != std::string::npos;
    if (clientV6 != serverV6) {
        return false;  // cross-family pairs never share a subnet
    }
    if (!clientV6) {
        std::array<uint8_t, 4> a{};
        std::array<uint8_t, 4> b{};
        if (!ParseIPv4Bytes(clientIp, a) || !ParseIPv4Bytes(serverHost, b)) {
            return false;
        }
        const int bits = (prefixLen > 0 && prefixLen <= 32) ? prefixLen : 24;  // default /24
        return SamePrefixBits(a.data(), b.data(), bits);
    }
    std::array<uint8_t, 16> a{};
    std::array<uint8_t, 16> b{};
    if (!ParseIPv6Bytes(clientIp, a) || !ParseIPv6Bytes(serverHost, b)) {
        return false;
    }
    const int bits = (prefixLen > 0 && prefixLen <= 128) ? prefixLen : 64;  // default /64
    return SamePrefixBits(a.data(), b.data(), bits);
}

std::vector<LinkPlan> SelectAutoLinks(const ReachabilityMatrix& matrix, size_t maxConnections,
                                      const std::string& primaryInterface,
                                      const std::string& primaryServerKey) {
    std::vector<LinkPlan> plans;
    if (maxConnections == 0) {
        return plans;
    }
    auto serverKey = [](const ServerEndpoint& ep) {
        return ep.host + ":" + std::to_string(ep.port);
    };
    // Server physical-NIC identity for column j: the advertised group when known, else the
    // endpoint key itself (each unknown endpoint is its own unique "NIC" => per-endpoint
    // dedup, never weaker than the legacy host:port dedup, §4.5).
    auto groupOf = [&](size_t j) -> std::string {
        const ServerEndpoint& ep = matrix.serverEndpoints[j];
        return !ep.nicGroup.empty() ? ep.nicGroup : serverKey(ep);
    };
    // Per-row client physical-NIC key; fall back to the address literal for the legacy flat
    // matrix that carries no NIC identity (degrades to IP-granular dedup, §4.5).
    auto ifaceKeyOf = [&](size_t i) -> std::string {
        if (i < matrix.localIfaces.size() && !matrix.localIfaces[i].empty()) {
            return matrix.localIfaces[i];
        }
        return matrix.localAddrs[i];
    };
    auto prefixLenOf = [&](size_t i) -> int {
        return i < matrix.localPrefixLens.size() ? matrix.localPrefixLens[i] : 0;
    };

    // Seed dedup with the primary lane on BOTH sides (§4.4). Client NIC = primaryInterface.
    // Server NIC = the group of the column whose endpoint matches primaryServerKey; when no
    // column matches (or it has no group) fall back to primaryServerKey as a synthetic group
    // so the primary's server endpoint is still blocked.
    std::set<std::string> usedClient;
    std::set<std::string> usedServer;
    bool hasPrimary = false;
    if (!primaryInterface.empty()) {
        usedClient.insert(primaryInterface);
        hasPrimary = true;
    }
    if (!primaryServerKey.empty()) {
        std::string primaryGroup = primaryServerKey;
        for (size_t j = 0; j < matrix.serverEndpoints.size(); ++j) {
            if (serverKey(matrix.serverEndpoints[j]) == primaryServerKey) {
                primaryGroup = groupOf(j);
                break;
            }
        }
        usedServer.insert(primaryGroup);
        hasPrimary = true;
    }

    // Total lanes including the primary must not exceed maxConnections (§4.2); the primary,
    // when present, already consumes one slot.
    const size_t reserved = hasPrimary ? 1u : 0u;
    if (maxConnections <= reserved) {
        return plans;
    }
    const size_t auxBudget = maxConnections - reserved;

    // Score tuple (familyRank, subnetRank, rttMs), compared lexicographically; lower is
    // better. familyRank (IPv4 first) leads so the IPv6 full-mesh "false reachability" is
    // demoted as a whole before same-subnet/RTT refine within a family (§4.3).
    struct Edge {
        int family = 0;
        int subnet = 1;
        long rtt = 0;
        size_t row = 0;
        size_t col = 0;
        std::string clientNic;
        std::string serverNic;
    };
    auto better = [](const Edge& a, const Edge& b) -> bool {
        if (a.family != b.family) return a.family < b.family;
        if (a.subnet != b.subnet) return a.subnet < b.subnet;
        if (a.rtt != b.rtt) return a.rtt < b.rtt;
        // Deterministic tie-break independent of row/column enumeration order.
        if (a.clientNic != b.clientNic) return a.clientNic < b.clientNic;
        if (a.serverNic != b.serverNic) return a.serverNic < b.serverNic;
        if (a.row != b.row) return a.row < b.row;
        return a.col < b.col;
    };

    // Keep the single best edge per (clientNIC, serverNIC) pair (e.g. an IPv4 row beats the
    // IPv6 row to the same server NIC ��� the L-r6-02 landing spot).
    std::map<std::pair<std::string, std::string>, Edge> bestEdge;
    for (size_t i = 0; i < matrix.localAddrs.size(); ++i) {
        const std::string cN = ifaceKeyOf(i);
        if (usedClient.count(cN) != 0) {
            continue;  // primary's client NIC pre-occupied
        }
        for (size_t j = 0; j < matrix.serverEndpoints.size() && j < matrix.cells[i].size(); ++j) {
            if (!matrix.cells[i][j].reachable) {
                continue;
            }
            const std::string sN = groupOf(j);
            if (usedServer.count(sN) != 0) {
                continue;  // primary's server NIC pre-occupied
            }
            Edge e;
            e.family = FamilyRank(matrix.localAddrs[i]);
            e.subnet = SameSubnet(matrix.localAddrs[i], prefixLenOf(i),
                                  matrix.serverEndpoints[j].host)
                           ? 0
                           : 1;
            e.rtt = matrix.cells[i][j].rttMs;
            e.row = i;
            e.col = j;
            e.clientNic = cN;
            e.serverNic = sN;
            const auto key = std::make_pair(cN, sN);
            auto it = bestEdge.find(key);
            if (it == bestEdge.end() || better(e, it->second)) {
                bestEdge[key] = std::move(e);
            }
        }
    }

    // Selection phase: maximum-cardinality bipartite matching with a best-first cost
    // refinement (greedy + cardinality-protection guard). Unlike the old plain greedy, a
    // best-scored edge that would shrink the achievable channel count is skipped, so the
    // result is a maximum-cardinality matching whose edge-cost vector is lexicographically
    // minimal (design §3.2/§3.3).

    // Collect the per-pair best edges and order them best-first by `better`. The ordering is
    // a strict total order independent of row/column enumeration (design §3.5/D-03).
    std::vector<Edge> edges;
    edges.reserve(bestEdge.size());
    for (auto& kv : bestEdge) {
        edges.push_back(kv.second);
    }
    std::sort(edges.begin(), edges.end(), better);

    // Assign stable bipartite-vertex ids to the client/server NICs by sorted string, so the
    // graph (and Kuhn's traversal order) is deterministic regardless of input order (D-03 A).
    std::vector<std::string> clientNics;
    std::vector<std::string> serverNics;
    clientNics.reserve(edges.size());
    serverNics.reserve(edges.size());
    for (const Edge& e : edges) {
        clientNics.push_back(e.clientNic);
        serverNics.push_back(e.serverNic);
    }
    std::sort(clientNics.begin(), clientNics.end());
    clientNics.erase(std::unique(clientNics.begin(), clientNics.end()), clientNics.end());
    std::sort(serverNics.begin(), serverNics.end());
    serverNics.erase(std::unique(serverNics.begin(), serverNics.end()), serverNics.end());
    std::map<std::string, int> clientId;
    std::map<std::string, int> serverId;
    for (int i = 0; i < static_cast<int>(clientNics.size()); ++i) {
        clientId[clientNics[i]] = i;
    }
    for (int j = 0; j < static_cast<int>(serverNics.size()); ++j) {
        serverId[serverNics[j]] = j;
    }
    const int nC = static_cast<int>(clientNics.size());
    const int nS = static_cast<int>(serverNics.size());

    // adjacency[c] lists the server vertices client vertex c can reach; built in best-first
    // edge order so adjacency traversal is deterministic.
    std::vector<std::vector<int>> adjacency(nC);
    for (const Edge& e : edges) {
        adjacency[clientId[e.clientNic]].push_back(serverId[e.serverNic]);
    }

    // Kuhn augmenting-path routine, restricted to the not-yet-blocked vertices. Returns the
    // matching cardinality of the residual graph (committed vertices passed in as blocked).
    struct Kuhn {
        const std::vector<std::vector<int>>& adjacency;
        const std::vector<char>& blockedServer;
        std::vector<int>& matchS;
        std::vector<char>& visited;
        bool augment(int c) {
            for (int s : adjacency[c]) {
                if (blockedServer[s] != 0 || visited[s] != 0) {
                    continue;
                }
                visited[s] = 1;
                if (matchS[s] == -1 || augment(matchS[s])) {
                    matchS[s] = c;
                    return true;
                }
            }
            return false;
        }
    };
    auto maxMatching = [&](const std::vector<char>& blockedClient,
                           const std::vector<char>& blockedServer) -> int {
        std::vector<int> matchS(nS, -1);  // server vertex -> matched client vertex
        std::vector<char> visited(nS, 0);
        Kuhn kuhn{adjacency, blockedServer, matchS, visited};
        int matched = 0;
        for (int c = 0; c < nC; ++c) {
            if (blockedClient[c] != 0) {
                continue;
            }
            std::fill(visited.begin(), visited.end(), 0);
            if (kuhn.augment(c)) {
                ++matched;
            }
        }
        return matched;
    };

    // kMax = min(auxBudget, maximum matching cardinality): the channel-count ceiling.
    std::vector<char> committedClient(nC, 0);
    std::vector<char> committedServer(nS, 0);
    const int m = maxMatching(committedClient, committedServer);
    // Avoid std::min here: this TU is also built without NOMINMAX, where the Windows `min`
    // macro would mangle the call.
    const size_t matchMax = static_cast<size_t>(m);
    const size_t kMax = auxBudget < matchMax ? auxBudget : matchMax;

    // best-first edge greedy with a cardinality guard: tentatively commit edge e; if the
    // residual graph can still reach kMax, keep it, otherwise skip e (it lies in no kMax
    // matching, so dropping it loses no cardinality and improves cost lexicographically).
    for (const Edge& e : edges) {
        if (plans.size() >= kMax) {
            break;
        }
        const int c = clientId[e.clientNic];
        const int s = serverId[e.serverNic];
        if (committedClient[c] != 0 || committedServer[s] != 0) {
            continue;
        }
        committedClient[c] = 1;
        committedServer[s] = 1;
        const int residual = maxMatching(committedClient, committedServer);
        if (plans.size() + 1u + static_cast<size_t>(residual) >= kMax) {
            LinkPlan plan;
            plan.localAddr = matrix.localAddrs[e.row];
            plan.serverHost = matrix.serverEndpoints[e.col].host;
            plan.serverPort = matrix.serverEndpoints[e.col].port;
            plans.push_back(std::move(plan));
        } else {
            committedClient[c] = 0;
            committedServer[s] = 0;
        }
    }
    return plans;
}

std::pair<std::string, uint16_t> SplitServerKey(const std::string& key, uint16_t defaultPort) {
    // Bracketed IPv6 literal, optionally followed by ":port".
    if (!key.empty() && key.front() == '[') {
        const size_t close = key.find(']');
        if (close != std::string::npos) {
            const std::string host = key.substr(1, close - 1);
            // Bare "[ipv6]" with no trailing port.
            if (close + 1 >= key.size()) {
                return {host, defaultPort};
            }
            if (key[close + 1] == ':') {
                const std::string portStr = key.substr(close + 2);
                try {
                    const int port = std::stoi(portStr);
                    if (port > 0 && port <= 65535) {
                        return {host, static_cast<uint16_t>(port)};
                    }
                } catch (...) {
                }
            }
            // Malformed trailing data: fall back to the bracketed host on the default port.
            return {host, defaultPort};
        }
    }
    const size_t colon = key.rfind(':');
    if (colon == std::string::npos) {
        return {key, defaultPort};
    }
    const std::string portStr = key.substr(colon + 1);
    // A bare IPv6 literal (multiple ':') without an explicit port should not be split.
    if (key.find(':') != colon) {
        return {key, defaultPort};
    }
    try {
        const int port = std::stoi(portStr);
        if (port > 0 && port <= 65535) {
            return {key.substr(0, colon), static_cast<uint16_t>(port)};
        }
    } catch (...) {
    }
    return {key, defaultPort};
}

}  // namespace fc
