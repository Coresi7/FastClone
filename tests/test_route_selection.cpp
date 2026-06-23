// Unit tests for the r6-route-quality route selection rewrite (design §8).
//   - SelectAutoLinks: edge-scored greedy matching with client+server NIC dedup, primary
//     two-sided preemption, address-family preference and same-subnet scoring.
//   - AuthOk encode/decode round-trip carrying (endpoint, nicGroup) pairs (§2 / §6.3).
// SelectAutoLinks is a pure function over a ReachabilityMatrix, so these tests construct
// synthetic matrices and assert the chosen LinkPlans without touching the network.

#include "net_topology.h"
#include "protocol_codec.h"

#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr uint16_t kPort = 27842;

void Require(bool cond, const char* msg) {
    if (!cond) {
        throw std::runtime_error(std::string("route-selection: ") + msg);
    }
}

// Small builder for a reachability matrix. row()/col() append parallel rows/cols; finalize()
// sizes the cell grid (all unreachable); reach() marks one cell reachable with an RTT.
struct MatrixBuilder {
    fc::ReachabilityMatrix m;

    void row(const std::string& ip, const std::string& iface, int prefix) {
        m.localAddrs.push_back(ip);
        m.localIfaces.push_back(iface);
        m.localPrefixLens.push_back(prefix);
    }
    void col(const std::string& host, const std::string& group, uint16_t port = kPort) {
        m.serverEndpoints.push_back(fc::ServerEndpoint{host, port, group});
    }
    void finalize() {
        m.cells.assign(m.localAddrs.size(),
                       std::vector<fc::ReachabilityCell>(m.serverEndpoints.size()));
    }
    void reach(size_t i, size_t j, long rtt) {
        m.cells[i][j].reachable = true;
        m.cells[i][j].rttMs = rtt;
    }
};

std::string ServerKey(const std::string& host, uint16_t port = kPort) {
    return host + ":" + std::to_string(port);
}

// ---- TC-01: acceptance topology replica (V-01) -----------------------------------------
// Dual-NIC dual-subnet; primary A -> 30.29.53.25 (server NIC g0). Expect exactly one aux
// B_v4 -> 30.29.89.47 (server NIC g1); no g0 column may be reused.
void TC01_AcceptanceReplica() {
    MatrixBuilder b;
    b.row("30.29.53.12", "A", 24);    // 0 A_v4
    b.row("2402:dead::12", "A", 64);  // 1 A_v6
    b.row("30.29.89.37", "B", 24);    // 2 B_v4
    b.row("2402:dead::37", "B", 64);  // 3 B_v6
    b.col("30.29.53.25", "g0");       // 0 NIC4 v4
    b.col("2402:dead::25", "g0");     // 1 NIC4 v6
    b.col("30.29.89.47", "g1");       // 2 NIC5 v4
    b.col("2402:dead::47", "g1");     // 3 NIC5 v6
    b.finalize();
    // IPv4 is physically isolated: same-subnet cells only.
    b.reach(0, 0, 5);
    b.reach(2, 2, 6);
    // IPv6 /64 full mesh: every IPv6xIPv6 cell reachable with close RTT.
    b.reach(1, 1, 20);
    b.reach(1, 3, 21);
    b.reach(3, 1, 22);
    b.reach(3, 3, 20);

    const std::vector<fc::LinkPlan> plans =
        fc::SelectAutoLinks(b.m, 4, "A", ServerKey("30.29.53.25"));
    Require(plans.size() == 1, "TC-01 expected exactly one auxiliary lane");
    Require(plans[0].localAddr == "30.29.89.37", "TC-01 aux must bind B_v4");
    Require(plans[0].serverHost == "30.29.89.47", "TC-01 aux must reach NIC5 (g1), not g0");
}

// ---- TC-02: IPv6 collapse counter-example (V-02) ---------------------------------------
// Same topology but only IPv6 full mesh is reachable. Server NIC dedup must still hold: the
// single aux must NOT land on the primary's server NIC g0 (would be the original bug).
void TC02_Ipv6CollapseCounterExample() {
    MatrixBuilder b;
    b.row("30.29.53.12", "A", 24);
    b.row("2402:dead::12", "A", 64);
    b.row("30.29.89.37", "B", 24);
    b.row("2402:dead::37", "B", 64);
    b.col("30.29.53.25", "g0");
    b.col("2402:dead::25", "g0");
    b.col("30.29.89.47", "g1");
    b.col("2402:dead::47", "g1");
    b.finalize();
    // Only IPv6 full mesh reachable (no IPv4 at all).
    b.reach(1, 1, 20);
    b.reach(1, 3, 21);
    b.reach(3, 1, 22);
    b.reach(3, 3, 20);

    const std::vector<fc::LinkPlan> plans =
        fc::SelectAutoLinks(b.m, 4, "A", ServerKey("30.29.53.25"));
    Require(plans.size() == 1, "TC-02 expected exactly one auxiliary lane");
    Require(plans[0].serverHost == "2402:dead::47",
            "TC-02 aux must land on server NIC g1, never on the primary's g0");
}

// ---- TC-03: IPv4 preferred over same-subnet IPv6 (V-06) --------------------------------
// One client NIC reaches one server NIC over both an IPv4 (different subnet) and an IPv6
// (same /64) edge. IPv4 must win even though IPv6 is same-subnet (family rank leads).
void TC03_Ipv4Preference() {
    MatrixBuilder b;
    b.row("10.0.0.5", "C", 24);     // 0 v4 (diff subnet from server v4)
    b.row("2402:abc::5", "C", 64);  // 1 v6 (same /64 as server v6)
    b.col("10.9.9.9", "g7");        // 0 v4, different subnet
    b.col("2402:abc::9", "g7");     // 1 v6, same /64
    b.finalize();
    b.reach(0, 0, 30);  // IPv4 edge
    b.reach(1, 1, 10);  // IPv6 edge (lower RTT, same subnet) but still must lose to IPv4

    const std::vector<fc::LinkPlan> plans = fc::SelectAutoLinks(b.m, 2);
    Require(plans.size() == 1, "TC-03 expected one lane (single server NIC)");
    Require(plans[0].localAddr == "10.0.0.5", "TC-03 must prefer the IPv4 edge");
    Require(plans[0].serverHost == "10.9.9.9", "TC-03 must reach the IPv4 server address");
}

// ---- TC-04: same-subnet preferred over lower RTT (V-03) --------------------------------
void TC04_SameSubnetBeatsRtt() {
    MatrixBuilder b;
    b.row("30.29.53.12", "C", 24);  // 0
    b.col("30.29.53.25", "g0");     // 0 same subnet (high RTT)
    b.col("40.0.0.25", "g0");       // 1 different subnet (low RTT), same server NIC
    b.finalize();
    b.reach(0, 0, 100);  // same subnet, high RTT
    b.reach(0, 1, 5);    // diff subnet, low RTT

    const std::vector<fc::LinkPlan> plans = fc::SelectAutoLinks(b.m, 2);
    Require(plans.size() == 1, "TC-04 expected one lane");
    Require(plans[0].serverHost == "30.29.53.25",
            "TC-04 must prefer the same-subnet edge over the lower-RTT one");
}

// ---- TC-05: RTT tie-break (V-04) -------------------------------------------------------
void TC05_RttTieBreak() {
    MatrixBuilder b;
    b.row("30.29.53.12", "C", 24);  // 0
    b.col("30.29.53.25", "g0");     // 0 same subnet, RTT 50
    b.col("30.29.53.26", "g0");     // 1 same subnet, RTT 10
    b.finalize();
    b.reach(0, 0, 50);
    b.reach(0, 1, 10);

    const std::vector<fc::LinkPlan> plans = fc::SelectAutoLinks(b.m, 2);
    Require(plans.size() == 1, "TC-05 expected one lane");
    Require(plans[0].serverHost == "30.29.53.26",
            "TC-05 must fall back to the lower-RTT edge when family+subnet tie");
}

// ---- TC-06: server NIC dedup, v4+v6 on one server NIC (V-05) ---------------------------
void TC06_ServerNicDedup() {
    MatrixBuilder b;
    b.row("30.29.53.12", "C", 24);    // 0 v4
    b.row("2402:dead::12", "C", 64);  // 1 v6
    b.col("30.29.53.25", "g0");       // 0 v4 (server NIC g0)
    b.col("2402:dead::25", "g0");     // 1 v6 (same server NIC g0)
    b.finalize();
    b.reach(0, 0, 5);
    b.reach(1, 1, 6);

    const std::vector<fc::LinkPlan> plans = fc::SelectAutoLinks(b.m, 4);
    Require(plans.size() == 1, "TC-06 must collapse one server NIC to a single lane");
    Require(plans[0].serverHost == "30.29.53.25",
            "TC-06 must keep the IPv4 edge for the shared server NIC");
}

// ---- TC-07: client NIC dedup, dual-stack collapses to one lane (V-07) ------------------
void TC07_ClientNicDedup() {
    MatrixBuilder b;
    b.row("10.0.0.5", "C", 24);     // 0 v4
    b.row("2402:abc::5", "C", 64);  // 1 v6 (same client NIC)
    b.col("10.0.0.9", "g0");        // 0 v4 (server NIC g0)
    b.col("2402:abc::9", "g1");     // 1 v6 (different server NIC g1)
    b.finalize();
    b.reach(0, 0, 5);
    b.reach(1, 1, 6);

    const std::vector<fc::LinkPlan> plans = fc::SelectAutoLinks(b.m, 4);
    Require(plans.size() == 1, "TC-07 one client NIC must yield at most one lane");
    Require(plans[0].localAddr == "10.0.0.5", "TC-07 must keep the IPv4 edge for the NIC");
}

// ---- TC-08: maxConnections truncation (primary occupies one slot) ----------------------
void TC08_MaxConnectionsTruncation() {
    MatrixBuilder b;
    b.row("10.0.1.2", "A", 24);  // 0
    b.row("10.0.2.2", "B", 24);  // 1
    b.row("10.0.3.2", "C", 24);  // 2
    b.col("1.1.1.1", "gP");      // 0 primary server NIC
    b.col("10.0.1.9", "g1");     // 1
    b.col("10.0.2.9", "g2");     // 2
    b.col("10.0.3.9", "g3");     // 3
    b.finalize();
    b.reach(0, 1, 5);
    b.reach(1, 2, 5);
    b.reach(2, 3, 5);

    // Three auxiliaries are possible, but maxConnections=2 with the primary occupying one
    // slot leaves room for exactly one auxiliary.
    const std::vector<fc::LinkPlan> plans =
        fc::SelectAutoLinks(b.m, 2, "P", ServerKey("1.1.1.1"));
    Require(plans.size() == 1, "TC-08 maxConnections=2 with a primary must yield one aux");
}

// ---- TC-09: legacy degradation (no ifaces/groups/prefixes) -----------------------------
// Empty localIfaces / localPrefixLens and empty server groups must degrade to IP-granular
// dedup + default subnet, not weaker than the legacy behavior.
void TC09_LegacyDegradation() {
    fc::ReachabilityMatrix m;
    m.localAddrs = {"1.0.0.1", "1.0.0.2"};
    // localIfaces / localPrefixLens intentionally left empty (legacy flat matrix).
    m.serverEndpoints = {fc::ServerEndpoint{"2.0.0.1", kPort, std::string()},
                         fc::ServerEndpoint{"2.0.0.2", kPort, std::string()}};
    m.cells.assign(2, std::vector<fc::ReachabilityCell>(2));
    auto reach = [&](size_t i, size_t j, long rtt) {
        m.cells[i][j].reachable = true;
        m.cells[i][j].rttMs = rtt;
    };
    reach(0, 0, 10);
    reach(0, 1, 20);
    reach(1, 0, 30);
    reach(1, 1, 5);

    const std::vector<fc::LinkPlan> plans = fc::SelectAutoLinks(m, 3);
    Require(plans.size() == 2, "TC-09 legacy matrix must still produce two distinct lanes");
    Require(plans[0].localAddr != plans[1].localAddr, "TC-09 lanes must use distinct sources");
    Require(plans[0].serverHost != plans[1].serverHost,
            "TC-09 lanes must use distinct server endpoints (IP-granular dedup)");
}

// ---- TC-10: primary server NIC group preemption (V-01 core) ----------------------------
// primaryServerKey resolves to group g0; ALL g0 columns (incl. the IPv6 one) must be
// excluded so the aux lands on g1 even when a g0 IPv6 edge has the best RTT.
void TC10_PrimaryGroupPreemption() {
    MatrixBuilder b;
    b.row("30.29.89.37", "B", 24);    // 0 B_v4
    b.row("2402:dead::37", "B", 64);  // 1 B_v6
    b.col("30.29.53.25", "g0");       // 0 primary's v4 (server NIC g0)
    b.col("2402:dead::25", "g0");     // 1 server NIC g0 v6
    b.col("30.29.89.47", "g1");       // 2 server NIC g1 v4
    b.finalize();
    b.reach(0, 2, 50);  // B_v4 -> g1 v4 (higher RTT)
    b.reach(1, 1, 1);   // B_v6 -> g0 v6 (lowest RTT, but g0 is the primary's NIC)

    const std::vector<fc::LinkPlan> plans =
        fc::SelectAutoLinks(b.m, 4, "A", ServerKey("30.29.53.25"));
    Require(plans.size() == 1, "TC-10 expected one aux");
    Require(plans[0].serverHost == "30.29.89.47",
            "TC-10 must exclude every g0 column (primary NIC), landing on g1");
}

// ---- TC-11: greedy counter-example, max-cardinality matching (V-01) --------------------
// The single best-scored edge (Ca-Sx, same /24) shares both NICs with the only other
// reachable edges; a plain greedy that takes it first collapses to one lane. The optimal
// matcher must skip it and pick Ca-Sy + Cb-Sx for two lanes. All-IPv4 isolates the family
// dimension so only the cardinality guard decides the outcome.
void TC11_GreedyCounterExample() {
    MatrixBuilder b;
    b.row("10.0.0.2", "Ca", 24);  // 0
    b.row("10.0.1.2", "Cb", 24);  // 1
    b.col("10.0.0.9", "gx");      // 0 Sx: same /24 as Ca
    b.col("10.9.9.9", "gy");      // 1 Sy: different subnet
    b.finalize();
    b.reach(0, 0, 5);  // Ca-Sx: family0, subnet0 (best-scored edge)
    b.reach(0, 1, 5);  // Ca-Sy: family0, subnet1
    b.reach(1, 0, 5);  // Cb-Sx: family0, subnet1 (Cb is a different /24 than Sx)
    // (Cb,Sy) intentionally unreachable.

    const std::vector<fc::LinkPlan> plans = fc::SelectAutoLinks(b.m, 4);
    Require(plans.size() == 2, "TC-11 must build two lanes, not collapse to the greedy one");
    // best-first acceptance order: Ca-Sy (subnet1, clientNic 'Ca') before Cb-Sx.
    bool sawCaSy = false;
    bool sawCbSx = false;
    for (const fc::LinkPlan& p : plans) {
        if (p.localAddr == "10.0.0.2" && p.serverHost == "10.9.9.9") sawCaSy = true;
        if (p.localAddr == "10.0.1.2" && p.serverHost == "10.0.0.9") sawCbSx = true;
    }
    Require(sawCaSy, "TC-11 must keep Ca->Sy (10.0.0.2 -> 10.9.9.9)");
    Require(sawCbSx, "TC-11 must keep Cb->Sx (10.0.1.2 -> 10.0.0.9)");
}

// ---- FamilyRank / SameSubnet direct checks (L-r6-02 / D-02 scoring) --------------------
void TC_ScoringHelpers() {
    Require(fc::FamilyRank("30.29.53.12") == 0, "FamilyRank IPv4 must be 0");
    Require(fc::FamilyRank("2402:dead::1") == 1, "FamilyRank IPv6 must be 1");
    Require(fc::FamilyRank("") == 0, "FamilyRank empty must be neutral 0");
    Require(fc::SameSubnet("30.29.53.12", 24, "30.29.53.25"), "same /24 must match");
    Require(!fc::SameSubnet("30.29.53.12", 24, "30.29.89.47"), "different /24 must not match");
    Require(fc::SameSubnet("30.29.53.12", 0, "30.29.53.99"), "default /24 must match");
    Require(fc::SameSubnet("2402:dead::12", 64, "2402:dead::99"), "same /64 must match");
    Require(!fc::SameSubnet("2402:dead::12", 64, "2402:beef::99"), "different /64 must not match");
    Require(!fc::SameSubnet("30.29.53.12", 24, "2402:dead::25"), "cross-family must not match");
}

// ---- AuthOk encode/decode round-trip (§2 / §6.3) ---------------------------------------
void TC_AuthOkRoundTrip() {
    fc::AuthOkInfo in;
    in.role = fc::AuthOkRole::NewSession;
    in.sessionId = "session-abc-123";
    in.serverAddrs = {
        fc::AdvertisedEndpoint{"30.29.53.25:27842", 0},
        fc::AdvertisedEndpoint{"[2402:dead::25]:27842", 0},
        fc::AdvertisedEndpoint{"30.29.89.47:27842", 1},
        fc::AdvertisedEndpoint{"[2402:dead::47]:27842", 1},
    };
    const std::vector<uint8_t> bytes = fc::EncodeAuthOk(in);
    const fc::AuthOkInfo out = fc::DecodeAuthOk(bytes);
    Require(out.role == fc::AuthOkRole::NewSession, "round-trip role mismatch");
    Require(out.sessionId == in.sessionId, "round-trip sessionId mismatch");
    Require(out.serverAddrs.size() == in.serverAddrs.size(), "round-trip addr count mismatch");
    for (size_t i = 0; i < in.serverAddrs.size(); ++i) {
        Require(out.serverAddrs[i].endpoint == in.serverAddrs[i].endpoint,
                "round-trip endpoint mismatch");
        Require(out.serverAddrs[i].nicGroup == in.serverAddrs[i].nicGroup,
                "round-trip nicGroup mismatch");
    }

    // JoinAck carries addrCount=0 and must round-trip unchanged.
    fc::AuthOkInfo join;
    join.role = fc::AuthOkRole::JoinAck;
    join.sessionId = "session-xyz";
    const fc::AuthOkInfo joinOut = fc::DecodeAuthOk(fc::EncodeAuthOk(join));
    Require(joinOut.role == fc::AuthOkRole::JoinAck, "JoinAck role mismatch");
    Require(joinOut.sessionId == "session-xyz", "JoinAck sessionId mismatch");
    Require(joinOut.serverAddrs.empty(), "JoinAck must carry no advertised endpoints");
}

}  // namespace

void RunRouteSelectionTests() {
    TC01_AcceptanceReplica();
    TC02_Ipv6CollapseCounterExample();
    TC03_Ipv4Preference();
    TC04_SameSubnetBeatsRtt();
    TC05_RttTieBreak();
    TC06_ServerNicDedup();
    TC07_ClientNicDedup();
    TC08_MaxConnectionsTruncation();
    TC09_LegacyDegradation();
    TC10_PrimaryGroupPreemption();
    TC11_GreedyCounterExample();
    TC_ScoringHelpers();
    TC_AuthOkRoundTrip();
}
