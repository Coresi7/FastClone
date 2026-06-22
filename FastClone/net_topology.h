#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace fc {

// Multipath network topology helpers (design sec. 5.2, 6.1, 6.2, 6.4). All logic here is
// application-layer L3/L4 and OS-independent at the interface level; only the
// implementation bodies branch per-OS for address enumeration / randomness.

// Generate a >=128-bit cryptographically random session token, lowercase hex encoded
// (32 random bytes -> 64 hex chars). Unguessable + non-reused (FR-003 / NFR-007).
// Windows: BCryptGenRandom; POSIX: getrandom()/ /dev/urandom.
std::string GenerateSessionToken();

// Enumerate usable local source addresses, filtering loopback, link-local and
// not-up interfaces (FR-006 / AC-003). Returns textual IPv4/IPv6 literals.
// Backward-compatible flat view used for server address advertisement.
std::vector<std::string> EnumerateLocalAddresses();

// A candidate local source address tagged with the identity of the physical
// interface (NIC) it belongs to. A dual-stack NIC contributes several addresses
// (e.g. one IPv4 + one IPv6) that all share the SAME ifaceKey, which lets the
// auto-selection collapse them to a single connection per NIC (FR-009).
//   - Windows: per-adapter name (GUID) from IP_ADAPTER_ADDRESSES, identical for
//     every unicast address of the adapter regardless of address family (IfIndex
//     vs Ipv6IfIndex can differ per family, so the adapter name is the stable key).
//   - POSIX: if_nametoindex(ifa_name) (falls back to ifa_name when unavailable).
struct LocalAddress {
    std::string ip;           // textual IPv4/IPv6 literal (bind source)
    std::string ifaceKey;     // stable physical-interface identity
    std::string friendlyName; // human-readable adapter name (Windows FriendlyName; empty on POSIX)
    int prefixLen = 0;        // on-link prefix length (0 = unknown -> default /24 or /64, sec. 4.3)
};

// Same enumeration/filtering as EnumerateLocalAddresses, but each address keeps
// the stable key of the physical interface that owns it so callers can dedupe by
// NIC rather than by IP string (FR-009).
std::vector<LocalAddress> EnumerateLocalCandidates();

// Probe-specific candidate set: same as EnumerateLocalCandidates but additionally
// filters deprecated and temporary/privacy IPv6 addresses (Windows: DadState /
// SuffixOrigin), and keeps at most one stable address per (NIC, address-family) to
// reduce the number of probe attempts from ~60 to a small constant on machines with
// many deprecated/temporary IPv6 addresses. POSIX falls back to EnumerateLocalCandidates.
// Use only for the reachability-probe phase; do NOT use for SelectAutoLinks or
// server address advertisement (those semantics are unchanged).
std::vector<LocalAddress> EnumerateProbeCandidates();

// Map a textual local source IP back to the stable physical-interface key of the
// NIC that owns it (matching EnumerateLocalCandidates). Returns "" when the IP is
// not found on any usable interface. Used to seed the primary lane's NIC into the
// same-NIC dedup so auxiliaries cannot reuse it (FR-009 / design sec. 6.4).
std::string InterfaceKeyForLocalAddress(const std::string& ip);

// Filter predicate exposed for testing (AC-003): true if the textual IP must be
// EXCLUDED from the candidate set (loopback / link-local / wildcard).
bool IsExcludedLocalAddress(const std::string& ip);

struct ServerEndpoint {
    std::string host;
    uint16_t port = 0;
    // Server physical-NIC identity (L-r6-01). Empty = unknown (e.g. came from a CLI
    // --server, not from the AuthOk advertisement); an empty group degrades to per-endpoint
    // dedup (each endpoint unique). Advertised endpoints carry "g<n>".
    std::string nicGroup;
};

// Parse "host:port" into host + port. Supported forms (review D-01):
//   - "ipv4:port" / "host:port"      -> split on the single ':'
//   - "ipv4" / "host"                -> defaultPort
//   - "[ipv6]:port"                  -> host = bare ipv6 literal (brackets stripped)
//   - "[ipv6]"                       -> bare ipv6 literal, defaultPort
//   - "ipv6" (bare literal, '::1')   -> kept whole, defaultPort (no split)
// The returned host is always the bare literal/name (no brackets) so it can be fed
// directly to getaddrinfo.
std::pair<std::string, uint16_t> SplitServerKey(const std::string& key, uint16_t defaultPort);

// One cell of the reachability matrix (FR-007 / AC-004).
struct ReachabilityCell {
    bool reachable = false;
    long rttMs = -1;  // connect latency on success; -1 when unreachable
};

struct ReachabilityMatrix {
    std::vector<std::string> localAddrs;            // rows (bind source IPs)
    std::vector<std::string> localIfaces;           // per-row physical-interface key (parallel to localAddrs)
    std::vector<int> localPrefixLens;               // per-row on-link prefix length (parallel; 0 = unknown)
    std::vector<ServerEndpoint> serverEndpoints;    // cols (with nicGroup)
    std::vector<std::vector<ReachabilityCell>> cells;  // cells[row][col]
};

// Probe "bind(localAddr) -> connect(server)" for every (local, server) pair with a
// per-attempt timeout (FR-007). Probe sockets are control-plane only and closed
// immediately. An empty localAddrs is treated as a single OS-default row ("").
// The flat-string overload tags each row's interface key with the address itself
// (IP-granular), preserving legacy behavior; prefer the LocalAddress overload so
// the matrix carries true physical-NIC identity for same-NIC dedup (FR-009).
ReachabilityMatrix ProbeReachability(const std::vector<std::string>& localAddrs,
                                     const std::vector<ServerEndpoint>& serverEndpoints,
                                     int timeoutMs = 2000);
ReachabilityMatrix ProbeReachability(const std::vector<LocalAddress>& localAddrs,
                                     const std::vector<ServerEndpoint>& serverEndpoints,
                                     int timeoutMs = 2000);

// A chosen client->server pairing for one connection (design sec. 6.4).
struct LinkPlan {
    std::string localAddr;    // source bind ("" = OS default)
    std::string serverHost;
    uint16_t serverPort = 0;
};

// Address-family preference rank exposed for testing (L-r6-02, sec. 4.3): IPv4 -> 0, IPv6 -> 1
// (an empty/OS-default literal ranks as 0). IPv4 is preferred but NEVER hard-filtered, so an
// IPv6-only reachable NIC still produces a lane. Detection is purely textual (':' => IPv6).
int FamilyRank(const std::string& ip);

// Same-subnet test exposed for testing (D-02 / sec. 4.3). True when clientIp and serverHost are
// in the same on-link prefix. prefixLen is the client row's on-link prefix length; 0/unknown
// degrades to the family default (/24 for IPv4, /64 for IPv6). Returns false when either
// literal is unparseable or the families differ.
bool SameSubnet(const std::string& clientIp, int prefixLen, const std::string& serverHost);

// Automatic selection heuristic over a reachability matrix (FR-008 / FR-009 / AC-004,
// rewritten per r6-route-quality sec. 4 to edge-scored greedy bipartite matching):
//   - at most one connection per physical CLIENT NIC (rows sharing an ifaceKey collapse;
//     a dual-stack NIC's v4+v6 rows are one NIC),
//   - at most one connection per physical SERVER NIC (columns sharing a ServerEndpoint.nicGroup
//     collapse; an empty group degrades to per-endpoint dedup) -- strictly stronger than the
//     old per-host:port dedup and fixes the IPv6-full-mesh collapse onto one server NIC,
//   - score order per reachable edge: address family (IPv4 first) > same-subnet > lowest RTT,
//   - deterministic tie-breaking so the result is independent of enumeration order,
//   - total lanes (including the seeded primary) capped at maxConnections; only reachable
//     cells are considered, and a conflicting edge is dropped rather than forced onto a
//     shared NIC (B-01).
// The already-established primary lane is seeded via primaryInterface (the NIC key of the
// primary's local source IP) and primaryServerKey ("host:port"): the primary's client NIC and
// its server NIC group (resolved by locating primaryServerKey's column) are both pre-occupied
// so auxiliaries never reuse the primary's NIC on EITHER side (FR-009 / sec. 4.4).
std::vector<LinkPlan> SelectAutoLinks(const ReachabilityMatrix& matrix, size_t maxConnections,
                                      const std::string& primaryInterface = std::string(),
                                      const std::string& primaryServerKey = std::string());

}  // namespace fc
