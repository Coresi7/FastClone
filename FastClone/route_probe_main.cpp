// FastCloneRouteProbe — read-only multipath route diagnostics tool.
//
// Design: docs/design/route-probe-tool.md (Plan B). The shared client handshake primitives
// now live in client_handshake.h/.cpp (fc namespace) and SplitServerKey in net_topology;
// this tool links those small self-contained translation units instead of the transfer
// engine. The tool performs a real client handshake against the first --server, builds the
// (CLI + advertised) server endpoint set, probes reachability for every local source
// address, and prints the auto-selected link plan. It NEVER transfers files.

#include "client_handshake.h"
#include "net_topology.h"
#include "protocol.h"
#include "protocol_codec.h"
#include "win_socket.h"

#ifdef _WIN32
#include <Windows.h>
#endif

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fc {

// ---------------------------------------------------------------------------------------
// Tool configuration (design §3).
// ---------------------------------------------------------------------------------------
struct RouteProbeOptions {
    std::vector<std::pair<std::string, uint16_t>> servers;  // --server (repeatable / comma)
    std::string password;                                   // --password (required)
    uint16_t port = 27842;                                  // --port default (matches main program)
    int timeoutMs = 2000;                                   // --timeout-ms (default 2000)
    uint32_t maxConnections = 8;                            // --max-connections (default 8)
};

namespace {

// Handshake primitives (kProtocolVersion / NegotiateHelloAsClient / HandshakeClientNew) and
// SplitServerKey are now shared: the former via client_handshake.h, the latter via
// net_topology.h. The probe links those translation units rather than re-inlining copies.

// ---------------------------------------------------------------------------------------
// CLI parsing (design §3). Independent of cli.cpp but follows the same style: UTF-8 on
// Windows, ArgAt value fetch, std::runtime_error on bad input, PrintUsage to stderr.
// ---------------------------------------------------------------------------------------
#ifdef _WIN32
std::string ToUtf8(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }
    const int len = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (len <= 0) {
        throw std::runtime_error("WideCharToMultiByte failed");
    }
    std::string output(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), output.data(), len, nullptr, nullptr);
    return output;
}
#endif

const std::string& ArgAt(const std::vector<std::string>& args, size_t index) {
    if (index >= args.size()) {
        throw std::runtime_error("Missing value for argument");
    }
    return args[index];
}

void PrintUsage() {
    std::cerr
        << "Usage:\n"
        << "  FastCloneRouteProbe --server <host[:port]> [--server <host[:port]>...] --password <pwd>\n"
        << "                      [--port <n>] [--timeout-ms <n>] [--max-connections <n>]\n"
        << "\n"
        << "  Read-only multipath route diagnostics. Performs a real client handshake against the\n"
        << "  first --server, enumerates local source addresses, probes reachability to every\n"
        << "  (CLI + advertised) server endpoint, and prints the auto-selected link plan.\n"
        << "  It NEVER transfers files.\n"
        << "\n"
        << "  --server          server endpoint; repeatable and/or comma-separated (host or host:port)\n"
        << "  --password        session password (required)\n"
        << "  --port            default port for --server entries without one (default 27842)\n"
        << "  --timeout-ms      per-probe connect timeout in ms (default 2000)\n"
        << "  --max-connections cap on the selected link count (default 8)\n";
}

long ParseLongStrict(const std::string& value, const char* name) {
    if (value.empty()) {
        throw std::runtime_error(std::string(name) + " is empty");
    }
    char* end = nullptr;
    const long parsed = std::strtol(value.c_str(), &end, 10);
    if (end == nullptr || *end != '\0') {
        throw std::runtime_error(std::string("Invalid ") + name);
    }
    return parsed;
}

std::pair<std::string, uint16_t> ParseHostPort(const std::string& input, uint16_t defaultPort) {
    const auto pos = input.find(':');
    if (pos == std::string::npos) {
        if (input.empty()) {
            throw std::runtime_error("Invalid --server, host is empty");
        }
        return {input, defaultPort};
    }
    const std::string host = input.substr(0, pos);
    const std::string port = input.substr(pos + 1);
    if (host.empty() || port.empty()) {
        throw std::runtime_error("Invalid --server, expected host:port");
    }
    const long parsedPort = ParseLongStrict(port, "--server");
    if (parsedPort <= 0 || parsedPort > 65535) {
        throw std::runtime_error("Port out of range");
    }
    return {host, static_cast<uint16_t>(parsedPort)};
}

// Split a comma-separated endpoint list ("A:port,B,C:port") into host/port pairs.
std::vector<std::pair<std::string, uint16_t>> ParseServerList(const std::string& input,
                                                              uint16_t defaultPort) {
    std::vector<std::pair<std::string, uint16_t>> endpoints;
    size_t start = 0;
    while (start <= input.size()) {
        const size_t comma = input.find(',', start);
        const std::string token = (comma == std::string::npos)
                                      ? input.substr(start)
                                      : input.substr(start, comma - start);
        if (!token.empty()) {
            endpoints.push_back(ParseHostPort(token, defaultPort));
        }
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }
    if (endpoints.empty()) {
        throw std::runtime_error("Invalid --server, no endpoints parsed");
    }
    return endpoints;
}

RouteProbeOptions ParseRouteProbeArgs(const std::vector<std::string>& args) {
    RouteProbeOptions options;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (arg == "--port") {
            const long port = ParseLongStrict(ArgAt(args, ++i), "--port");
            if (port <= 0 || port > 65535) {
                throw std::runtime_error("Port out of range");
            }
            options.port = static_cast<uint16_t>(port);
        } else if (arg == "--server") {
            auto endpoints = ParseServerList(ArgAt(args, ++i), options.port);
            // Accumulate across repeated --server flags, de-duplicating exact host:port
            // repeats while preserving first-seen order (same as cli.cpp FR-005).
            for (auto& ep : endpoints) {
                const bool dup = std::any_of(options.servers.begin(), options.servers.end(),
                                             [&](const auto& e) { return e == ep; });
                if (!dup) {
                    options.servers.push_back(ep);
                }
            }
        } else if (arg == "--password") {
            options.password = ArgAt(args, ++i);
        } else if (arg == "--timeout-ms") {
            const long timeout = ParseLongStrict(ArgAt(args, ++i), "--timeout-ms");
            if (timeout <= 0 || timeout > 600000) {
                throw std::runtime_error("Invalid --timeout-ms (range: 1..600000)");
            }
            options.timeoutMs = static_cast<int>(timeout);
        } else if (arg == "--max-connections") {
            const long maxConn = ParseLongStrict(ArgAt(args, ++i), "--max-connections");
            if (maxConn < 1 || maxConn > 1024) {
                throw std::runtime_error("Invalid --max-connections (range: 1..1024)");
            }
            options.maxConnections = static_cast<uint32_t>(maxConn);
        } else {
            throw std::runtime_error("Unknown argument: " + arg);
        }
    }

    if (options.servers.empty()) {
        throw std::runtime_error("--server is required (at least one endpoint)");
    }
    if (options.password.empty()) {
        throw std::runtime_error("--password is required");
    }
    return options;
}

// ---------------------------------------------------------------------------------------
// Output helpers (design §5). Diagnostics go to stdout; errors / usage go to stderr.
// ---------------------------------------------------------------------------------------
std::string PadRight(const std::string& text, size_t width) {
    std::string out = text;
    if (out.size() > width) {
        if (width >= 2) {
            out = out.substr(0, width - 2) + "..";
        } else {
            out = out.substr(0, width);
        }
    }
    if (out.size() < width) {
        out.append(width - out.size(), ' ');
    }
    return out;
}

std::string DisplayLocal(const std::string& ip) {
    return ip.empty() ? std::string("OS-default") : ip;
}

std::string NicGroupOrDash(const std::string& nicGroup) {
    return nicGroup.empty() ? std::string("-") : nicGroup;
}

void PrintReachabilityMatrix(const ReachabilityMatrix& matrix, int timeoutMs) {
    constexpr size_t kLocalWidth = 24;
    constexpr size_t kIfaceWidth = 14;
    constexpr size_t kServerWidth = 18;

    const size_t rows = matrix.localAddrs.size();
    const size_t cols = matrix.serverEndpoints.size();

    std::cout << "Reachability matrix (timeout=" << timeoutMs << "ms, " << rows
              << " local x " << cols << " server):\n";

    const std::string rowHeaderPad(kLocalWidth + 1 + kIfaceWidth, ' ');

    // Header line 1: server endpoint host:port per column.
    std::cout << rowHeaderPad;
    for (size_t j = 0; j < cols; ++j) {
        const ServerEndpoint& ep = matrix.serverEndpoints[j];
        const std::string key = ep.host + ":" + std::to_string(ep.port);
        std::cout << " | " << PadRight(key, kServerWidth);
    }
    std::cout << "\n";

    // Header line 2: nicGroup per column.
    std::cout << rowHeaderPad;
    for (size_t j = 0; j < cols; ++j) {
        std::cout << " | " << PadRight(NicGroupOrDash(matrix.serverEndpoints[j].nicGroup), kServerWidth);
    }
    std::cout << "\n";

    // Separator.
    std::cout << std::string(kLocalWidth + 1 + kIfaceWidth, '-');
    for (size_t j = 0; j < cols; ++j) {
        std::cout << "-+-" << std::string(kServerWidth, '-');
    }
    std::cout << "\n";

    // Rows.
    for (size_t i = 0; i < rows; ++i) {
        const std::string iface =
            (i < matrix.localIfaces.size() && !matrix.localIfaces[i].empty())
                ? ("[if:" + matrix.localIfaces[i] + "]")
                : "[if:-]";
        std::cout << PadRight(DisplayLocal(matrix.localAddrs[i]), kLocalWidth) << " "
                  << PadRight(iface, kIfaceWidth);
        for (size_t j = 0; j < cols && j < matrix.cells[i].size(); ++j) {
            const ReachabilityCell& cell = matrix.cells[i][j];
            const std::string text = cell.reachable ? (std::to_string(cell.rttMs) + "ms") : "X";
            std::cout << " | " << PadRight(text, kServerWidth);
        }
        std::cout << "\n";
    }
    std::cout << std::endl;
}

// Resolve the advertised nicGroup of a server "host:port" by scanning the matrix columns.
std::string ServerNicGroupFor(const ReachabilityMatrix& matrix, const std::string& serverKey) {
    for (const ServerEndpoint& ep : matrix.serverEndpoints) {
        if (ep.host + ":" + std::to_string(ep.port) == serverKey) {
            return ep.nicGroup;
        }
    }
    return std::string();
}

// Resolve the physical-interface key for a local source IP via the matrix rows; fall back
// to InterfaceKeyForLocalAddress and finally to the IP literal itself.
std::string ClientIfaceFor(const ReachabilityMatrix& matrix, const std::string& localIp) {
    for (size_t i = 0; i < matrix.localAddrs.size(); ++i) {
        if (matrix.localAddrs[i] == localIp && i < matrix.localIfaces.size() &&
            !matrix.localIfaces[i].empty()) {
            return matrix.localIfaces[i];
        }
    }
    const std::string iface = InterfaceKeyForLocalAddress(localIp);
    return !iface.empty() ? iface : localIp;
}

void PrintLinkPlan(const std::vector<LinkPlan>& plans, const ReachabilityMatrix& matrix,
                   uint32_t maxConnections, const std::string& primaryActualLocal,
                   const std::string& primaryServerKey) {
    // Total selected lanes = the primary (always established) + the auxiliaries returned
    // by SelectAutoLinks (the primary is seeded but never echoed back in the plan list).
    const size_t totalLanes = plans.size() + 1;

    std::cout << "Selected links (max-connections=" << maxConnections << "): " << totalLanes
              << " lane(s)\n";

    struct Lane {
        std::string local;
        std::string serverKey;
        std::string nicGroup;
        bool primary = false;
    };
    std::vector<Lane> lanes;

    Lane primaryLane;
    primaryLane.local = primaryActualLocal;
    primaryLane.serverKey = primaryServerKey;
    primaryLane.nicGroup = ServerNicGroupFor(matrix, primaryServerKey);
    primaryLane.primary = true;
    lanes.push_back(primaryLane);

    for (const LinkPlan& lp : plans) {
        Lane lane;
        lane.local = lp.localAddr;
        lane.serverKey = lp.serverHost + ":" + std::to_string(lp.serverPort);
        lane.nicGroup = ServerNicGroupFor(matrix, lane.serverKey);
        lanes.push_back(lane);
    }

    for (size_t i = 0; i < lanes.size(); ++i) {
        const Lane& lane = lanes[i];
        std::cout << "  #" << (i + 1) << " " << (lane.primary ? "[primary]" : "         ")
                  << " local=" << PadRight(DisplayLocal(lane.local), 16) << " -> " << lane.serverKey
                  << "  (nicGroup " << NicGroupOrDash(lane.nicGroup) << ")\n";
    }
    std::cout << "\n";

    if (plans.empty()) {
        std::cout << "Conclusion: single-link only (no exploitable multipath topology)."
                  << std::endl;
        return;
    }

    // Distinct client NICs (by interface key) and distinct server NIC groups across all lanes.
    std::set<std::string> clientNics;
    std::set<std::string> serverGroups;
    for (const Lane& lane : lanes) {
        clientNics.insert(ClientIfaceFor(matrix, lane.local));
        serverGroups.insert(!lane.nicGroup.empty() ? lane.nicGroup : lane.serverKey);
    }

    std::cout << "Conclusion: " << totalLanes << " distinct lane(s) over " << clientNics.size()
              << " client NIC(s) / " << serverGroups.size() << " server NIC group(s)." << std::endl;

    // Same-NIC warning: two selected lanes resolving to the same non-empty server nicGroup.
    std::unordered_map<std::string, size_t> groupFirstLane;
    for (size_t i = 0; i < lanes.size(); ++i) {
        const std::string& group = lanes[i].nicGroup;
        if (group.empty()) {
            continue;  // CLI-only endpoints (empty group) degrade to per-endpoint dedup.
        }
        auto it = groupFirstLane.find(group);
        if (it == groupFirstLane.end()) {
            groupFirstLane.emplace(group, i + 1);
        } else {
            std::cout << "WARNING: lanes #" << it->second << " and #" << (i + 1)
                      << " share server nicGroup \"" << group
                      << "\"; they may not provide independent server-side bandwidth."
                      << std::endl;
        }
    }
}

// ---------------------------------------------------------------------------------------
// Core probe flow (design §4).
// ---------------------------------------------------------------------------------------
int RunRouteProbe(const RouteProbeOptions& options) {
    // ---- (1) Connect to the first --server (OS default route, no source binding) ----
    const std::string& primaryHost = options.servers.front().first;
    const uint16_t primaryPort = options.servers.front().second;
    SocketHandle primarySocket;
    try {
        primarySocket = ConnectTo(primaryHost, primaryPort);
    } catch (const std::exception& ex) {
        std::cerr << "connect failed: " << ex.what() << std::endl;
        return 3;
    }
    if (!primarySocket.Valid()) {
        std::cerr << "connect failed: could not connect to " << primaryHost << ":" << primaryPort
                  << std::endl;
        return 3;
    }

    // ---- (2) Real handshake Hello -> Auth -> AuthOk (shared HandshakeClientNew) ----
    AuthOkInfo authInfo;
    try {
        authInfo = HandshakeClientNew(primarySocket, options.password);
    } catch (const std::exception& ex) {
        std::cerr << "handshake failed: " << ex.what() << std::endl;
        ShutdownBoth(primarySocket);
        return 4;
    }

    std::cout << "sessionId=" << authInfo.sessionId << "\n";
    std::cout << "advertised endpoints: " << authInfo.serverAddrs.size() << "\n";
    for (const AdvertisedEndpoint& adv : authInfo.serverAddrs) {
        std::cout << "  " << adv.endpoint << "  (nicGroup g" << adv.nicGroup << ")\n";
    }
    std::cout << std::endl;

    // ---- (3) Build serverEndpoints = CLI servers + advertised (design §4 addServer) ----
    std::vector<ServerEndpoint> serverEndpoints;
    std::unordered_map<std::string, size_t> serverIndex;  // "host:port" -> index
    auto addServer = [&](const std::string& host, uint16_t port, const std::string& nicGroup) {
        const std::string key = host + ":" + std::to_string(port);
        auto it = serverIndex.find(key);
        if (it == serverIndex.end()) {
            serverIndex.emplace(key, serverEndpoints.size());
            serverEndpoints.push_back(ServerEndpoint{host, port, nicGroup});
        } else if (!nicGroup.empty() && serverEndpoints[it->second].nicGroup.empty()) {
            serverEndpoints[it->second].nicGroup = nicGroup;  // backfill the real NIC group
        }
    };
    for (const auto& ep : options.servers) {
        addServer(ep.first, ep.second, std::string());  // CLI endpoints: nicGroup unknown
    }
    for (const AdvertisedEndpoint& adv : authInfo.serverAddrs) {
        const auto hp = SplitServerKey(adv.endpoint, options.port);
        addServer(hp.first, hp.second, "g" + std::to_string(adv.nicGroup));
    }

    // ---- (4) Enumerate candidates + probe reachability + print the matrix ----
    const std::vector<LocalAddress> localCands = EnumerateProbeCandidates();
    const ReachabilityMatrix matrix = ProbeReachability(localCands, serverEndpoints, options.timeoutMs);
    PrintReachabilityMatrix(matrix, options.timeoutMs);

    // ---- (5) Selection (design §4 step 5, mirrors sync_engine.cpp:2011-2019) ----
    const std::string primaryActualLocal = LocalAddressOf(primarySocket);
    const std::string primaryIface = InterfaceKeyForLocalAddress(primaryActualLocal);
    const std::string primaryDedupIface =
        !primaryIface.empty() ? primaryIface : primaryActualLocal;
    const std::string primaryServerKey = primaryHost + ":" + std::to_string(primaryPort);
    const std::vector<LinkPlan> plans =
        SelectAutoLinks(matrix, options.maxConnections, primaryDedupIface, primaryServerKey);
    PrintLinkPlan(plans, matrix, options.maxConnections, primaryActualLocal, primaryServerKey);

    // ---- (6)(7) Close the connection and return success (read-only, no transfer) ----
    ShutdownBoth(primarySocket);
    return 0;
}

}  // namespace

#if defined(_WIN32) && defined(_MSC_VER)
RouteProbeOptions ParseRouteProbeCli(int argc, wchar_t** argv) {
    std::vector<std::string> args;
    if (argc > 1) {
        args.reserve(static_cast<size_t>(argc - 1));
        for (int i = 1; i < argc; ++i) {
            args.push_back(ToUtf8(argv[i]));
        }
    }
    return ParseRouteProbeArgs(args);
}
#else
RouteProbeOptions ParseRouteProbeCli(int argc, char** argv) {
    std::vector<std::string> args;
    if (argc > 1) {
        args.reserve(static_cast<size_t>(argc - 1));
        for (int i = 1; i < argc; ++i) {
            args.emplace_back(argv[i] == nullptr ? "" : argv[i]);
        }
    }
    return ParseRouteProbeArgs(args);
}
#endif

}  // namespace fc

#if defined(_WIN32) && defined(_MSC_VER)
int wmain(int argc, wchar_t** argv) {
#else
int main(int argc, char** argv) {
#endif
    fc::WsaContext wsa;  // RAII Winsock init for the whole process.

    fc::RouteProbeOptions options;
    try {
        options = fc::ParseRouteProbeCli(argc, argv);
    } catch (const std::exception& ex) {
        fc::PrintUsage();
        std::cerr << "argument error: " << ex.what() << std::endl;
        return 2;
    }

    try {
        return fc::RunRouteProbe(options);
    } catch (const std::exception& ex) {
        std::cerr << "FastCloneRouteProbe error: " << ex.what() << std::endl;
        return 1;
    }
}
