// Unit tests for the WAN small-file tuning pure functions
// (docs/design/wan-smallfile-perf.md section 3.2/section 3.3, verification points V-01/V-02/V-09/V-11).
//
// These cover the RTT-gated decisions WITHOUT touching the network: RTT injection drives the
// hash in-flight depth, the delta-signal in-flight depth and the WAN stream ladder, while the
// LAN / metro path (RTT <= threshold) must stay byte-for-byte on the legacy values so the
// zero-regression guarantee (HC-04/NFR-03) is machine-checked.

#include "cli.h"
#include "transfer_tuning.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>

namespace {

void Require(bool cond, const char* msg) {
    if (!cond) {
        throw std::runtime_error(std::string("wan-tuning: ") + msg);
    }
}

// Legacy hash depth at the default auto stream count (streamLimit=4): the original
// clamp(max(1024, 4*256), 1024, 8192) == 1024. Every LAN/metro RTT must reproduce it exactly.
void TC_HashDepthLanUnchanged() {
    for (long rtt : {-1L, 0L, 1L, 5L, 10L}) {  // probe-missing, LAN, metro, exactly at the gate
        const size_t depth = fc::ComputeHashInflightDepth(rtt, 4, 0);
        Require(depth == 1024, "LAN/metro hash depth must equal the legacy 1024 (zero regression)");
    }
    // Explicit higher stream counts still reproduce the legacy clamp on LAN.
    Require(fc::ComputeHashInflightDepth(5, 16, 0) == 4096, "legacy clamp: 16*256=4096 on LAN");
    Require(fc::ComputeHashInflightDepth(5, 64, 0) == 8192, "legacy clamp ceiling 8192 on LAN");
}

// AC-02: at the acceptance RTT (25ms) with the representative 16KiB average file, the hash
// in-flight target must break the old fixed 8192 cap, and grow monotonically with RTT.
void TC_HashDepthWanBreaks8192() {
    const size_t d25 = fc::ComputeHashInflightDepth(25, 8, 16 * 1024);
    Require(d25 > 8192, "WAN(25ms) hash depth must break the legacy 8192 cap (AC-02)");
    const size_t d50 = fc::ComputeHashInflightDepth(50, 8, 16 * 1024);
    Require(d50 > d25, "deeper RTT must target a deeper hash pipeline (FR-02)");
    // The raised ceiling bounds the depth (resource gate, NFR-08).
    const size_t dHuge = fc::ComputeHashInflightDepth(100000, 8, 16 * 1024);
    Require(dHuge <= fc::kHashWanCeilDepth, "hash depth must never exceed the WAN ceiling");
}

// V-05/AC-05 supporting unit: delta-signal depth is unbounded (0 sentinel) on LAN and a
// bounded positive value on WAN.
void TC_DeltaSigDepth() {
    Require(fc::ComputeDeltaSigInflightDepth(5, 0) == 0,
            "LAN delta-sig depth must stay unbounded (legacy send-all)");
    const size_t w = fc::ComputeDeltaSigInflightDepth(25, 0);
    Require(w > 0 && w <= fc::kDeltaSigWanCeilDepth,
            "WAN delta-sig depth must be a bounded positive pipeline");
}

// V-09/AC-09: the WAN stream ladder lifts the auto count above the legacy 4 streams, gated by
// RTT; LAN stays at 4.
void TC_WanStreamLadder() {
    Require(fc::WanStreamsForRtt(5) == 4, "LAN must keep 4 streams");
    Require(fc::WanStreamsForRtt(10) == 4, "at the gate must keep 4 streams");
    Require(fc::WanStreamsForRtt(25) == 8, "10-30ms must lift to 8 streams");
    Require(fc::WanStreamsForRtt(45) == 12, "30-60ms must lift to 12 streams");
    Require(fc::WanStreamsForRtt(120) == 16, ">60ms must lift to 16 streams");
    Require(fc::IsWanRtt(25) && !fc::IsWanRtt(10) && !fc::IsWanRtt(-1),
            "WAN gate: >10ms is WAN; <=10ms and missing-RTT are LAN");
}

// V-01/V-11: ResolveWanTuning ties it together. LAN reproduces the base; WAN lifts streams +
// hash depth only for auto-tuned sessions, and never overrides an explicit --streams.
void TC_ResolveWanTuning() {
    fc::TunedTransferOptions base;  // base.streamLimit defaults; set explicitly for clarity
    base.streamLimit = 4;
    base.chunkSize = 4 * 1024 * 1024;

    fc::CliOptions autoCli;  // streamAutoTune defaults to true
    autoCli.streamAutoTune = true;

    // LAN: identical to base, no WAN mode, legacy hash depth, unbounded delta sig.
    const fc::WanTuning lan = fc::ResolveWanTuning(base, autoCli, 5, 8);
    Require(!lan.wanMode, "5ms must not be WAN mode");
    Require(lan.streamLimit == 4, "LAN auto streams must stay 4 (NFR-03)");
    Require(lan.maxInFlightHashRequests == 1024, "LAN hash depth must stay legacy 1024");
    Require(lan.maxInFlightDeltaSig == 0, "LAN delta-sig must stay unbounded");

    // WAN auto-tuned: streams lifted, hash depth broken past 8192, delta sig bounded.
    const fc::WanTuning wan = fc::ResolveWanTuning(base, autoCli, 25, 8);
    Require(wan.wanMode, "25ms must be WAN mode");
    Require(wan.streamLimit > 4 && wan.streamLimit <= fc::kWanStreamCap,
            "WAN auto streams must lift above 4 within the cap (FR-12/13)");
    Require(wan.maxInFlightHashRequests > 8192, "WAN hash depth must break 8192 (AC-02)");
    Require(wan.maxInFlightDeltaSig > 0, "WAN delta-sig depth must be bounded positive");

    // hwConcurrency caps the stream lift (resource gate): 1 core => hw*2=2, floored to >=4.
    const fc::WanTuning wanLowHw = fc::ResolveWanTuning(base, autoCli, 120, 1);
    Require(wanLowHw.streamLimit <= 4 ||
                wanLowHw.streamLimit == std::max<uint32_t>(4, base.streamLimit),
            "low hardware concurrency must cap the WAN stream lift");

    // Explicit --streams (streamAutoTune=false) is never overridden, even on WAN.
    fc::CliOptions explicitCli;
    explicitCli.streamAutoTune = false;
    fc::TunedTransferOptions explicitBase;
    explicitBase.streamLimit = 6;
    const fc::WanTuning wanExplicit = fc::ResolveWanTuning(explicitBase, explicitCli, 25, 8);
    Require(wanExplicit.streamLimit == 6, "explicit --streams must not be overridden on WAN");
    Require(wanExplicit.maxInFlightHashRequests > 8192,
            "WAN hash depth still deepens for explicit-streams sessions (RTT-driven)");
}

// B-01 (V-12): the DeltaError sig-slot release decision. A BlockSigRequest is answered by
// exactly one of {BlockSigResponse, sig-level DeltaError}; only the sig-level error releases
// the WAN pipeline slot here, and only for a managed, not-yet-answered signature. Getting this
// wrong either leaks the in-flight count (sync wedges on WAN) or double-releases it.
void TC_DeltaErrorReleasesSigSlot() {
    // Sig-level error (no active range) for a managed, unanswered signature -> RELEASE.
    Require(fc::DeltaErrorReleasesSigSlot(/*rangeTagged=*/false, /*deltaStateExists=*/true,
                                          /*sigReceived=*/false),
            "sig-level DeltaError for an outstanding signature must release the slot (B-01)");

    // Range-tagged error arrives AFTER BlockSigResponse already settled the count -> NO release.
    Require(!fc::DeltaErrorReleasesSigSlot(/*rangeTagged=*/true, true, true),
            "range-tagged DeltaError must not double-release the slot");
    Require(!fc::DeltaErrorReleasesSigSlot(/*rangeTagged=*/true, true, false),
            "range-tagged DeltaError never touches the sig slot");

    // Already-answered signature (BlockSigResponse seen) -> NO release even if it looks sig-level.
    Require(!fc::DeltaErrorReleasesSigSlot(false, true, true),
            "an already-answered signature must not be released again");

    // No managed delta state (untracked rel) -> NO release.
    Require(!fc::DeltaErrorReleasesSigSlot(false, false, false),
            "an untracked rel must not release a slot it never held");
}

// optimize-small-file-write-path W-03 (FR-07/FR-08/FR-09, AC-07/AC-08/AC-10): write concurrency is
// an internal adaptive active cap with NO public knob. The pool-size derivation, the per-window
// cap controller and the pressure ladder are all pure, so the FR-08 rules are unit-testable without
// the network / disk.

// V-07/AC-07/B9: the fixed write pool size (poolMax = active cap physical ceiling) is
// clamp(max(1,hw), 8, 32). hw < 32 yields poolMax < 32, i.e. a real "capacity below the ceiling".
void TC_ResolveWriteWorkerPoolMax() {
    Require(fc::ResolveWriteWorkerPoolMax(1) == 8, "W-03/AC-07: hw=1 -> pool floor 8");
    Require(fc::ResolveWriteWorkerPoolMax(4) == 8, "W-03/AC-07: hw=4 -> pool floor 8");
    Require(fc::ResolveWriteWorkerPoolMax(8) == 8, "W-03/AC-07: hw=8 -> 8");
    Require(fc::ResolveWriteWorkerPoolMax(16) == 16, "W-03/AC-07: hw=16 -> 16");
    Require(fc::ResolveWriteWorkerPoolMax(32) == 32, "W-03/AC-07: hw=32 -> 32");
    Require(fc::ResolveWriteWorkerPoolMax(128) == 32, "W-03/AC-07: hw=128 -> pool ceiling 32");
    Require(fc::ResolveWriteWorkerPoolMax(0) == 8, "W-03/AC-07: hw=0 -> pool floor 8");
    // FR-07 active cap bounds.
    Require(fc::kActiveCapInitial == 8 && fc::kActiveCapMin == 1 && fc::kActiveCapMax == 32,
            "W-03/FR-07: active cap bounds are [1,32], initial 8");
}

// UpdateEwma is a pure helper feeding the latency signal; the unseeded (prev<=0) case takes the
// sample verbatim, otherwise it blends with alpha.
void TC_UpdateEwma() {
    Require(fc::UpdateEwma(0.0, 500.0, 0.2) == 500.0, "W-03: unseeded EWMA takes the first sample");
    Require(fc::UpdateEwma(-1.0, 42.0, 0.5) == 42.0, "W-03: negative-seed EWMA takes the sample");
    // 0.2*200 + 0.8*100 = 120 (compared with a tolerance for binary FP).
    Require(std::abs(fc::UpdateEwma(100.0, 200.0, 0.2) - 120.0) < 1e-9,
            "W-03: EWMA blends prev and sample");
}

// V-07/V-08 (AC-07/AC-08/FR-07/FR-08/NFR-08/B9): the per-window active-cap controller. Every step
// asserts the cap equals the FR-08 rule output and stays inside [1, min(32, capacity)].
void TC_NextWriteActiveCap() {
    const uint64_t kBudget = 1000;  // recv soft budget bytes (writePressure must stay <= this)
    // A healthy window: backlog above the cap, no backpressure, pressure well under budget, and
    // (for havePrev windows) rate not dropping / latency rise <= 25%.
    auto healthy = [&](uint64_t backlog, double rate, double lat) {
        fc::WriteCapSample s;
        s.backlog = backlog;
        s.ioInFlightBytes = 100;
        s.driverWriteOutstandingBytes = 100;  // pressure = 200 <= budget
        s.recvSoftBudgetBytes = kBudget;
        s.completionRate = rate;
        s.latencyEwmaNs = lat;
        s.backpressureSleep = false;
        s.writeFailures = 0;
        return s;
    };

    // --- Initial + growth: first healthy window Holds, then +1 per consecutive healthy window ---
    {
        fc::WriteCapControllerState st;  // activeCap defaults to kActiveCapInitial (8)
        Require(st.activeCap == 8, "W-03/FR-07: initial active cap is 8");
        st = fc::NextWriteActiveCap(st, healthy(20, 100.0, 1000.0), 32);
        Require(st.activeCap == 8 && st.lastReason == fc::WriteCapAdjustReason::Hold,
                "W-03/AC-08: first healthy window only accumulates (Hold at 8)");
        st = fc::NextWriteActiveCap(st, healthy(20, 100.0, 1000.0), 32);
        Require(st.activeCap == 9 && st.lastReason == fc::WriteCapAdjustReason::Grow,
                "W-03/AC-08: second consecutive healthy window grows +1 (8->9)");
        st = fc::NextWriteActiveCap(st, healthy(20, 100.0, 1000.0), 32);
        Require(st.activeCap == 10 && st.lastReason == fc::WriteCapAdjustReason::Grow,
                "W-03/AC-08/NFR-08: each further healthy window grows exactly +1 (9->10)");
        // A window that is neither healthy nor deteriorating (latency +30%: >25% but <=50%) holds
        // and resets the healthy streak, so the NEXT healthy window only Holds again.
        st = fc::NextWriteActiveCap(st, healthy(20, 100.0, 1300.0), 32);
        Require(st.activeCap == 10 && st.lastReason == fc::WriteCapAdjustReason::Hold,
                "W-03/FR-08: latency +30% (between 25% and 50%) holds and resets the streak");
    }

    // --- Each deterioration signal halves (floor-halve) and tags the right reason (AC-08) ---
    {
        fc::WriteCapControllerState st; st.activeCap = 8;
        fc::WriteCapSample s = healthy(20, 100.0, 1000.0); s.backpressureSleep = true;
        st = fc::NextWriteActiveCap(st, s, 32);
        Require(st.activeCap == 4 && st.lastReason == fc::WriteCapAdjustReason::HalveBackpressure,
                "W-03/AC-08: backpressure sleep halves 8->4");
    }
    {
        fc::WriteCapControllerState st; st.activeCap = 8;
        fc::WriteCapSample s = healthy(20, 100.0, 1000.0);
        s.ioInFlightBytes = 600; s.driverWriteOutstandingBytes = 600;  // pressure 1200 > budget
        st = fc::NextWriteActiveCap(st, s, 32);
        Require(st.activeCap == 4 && st.lastReason == fc::WriteCapAdjustReason::HalveBudget,
                "W-03/AC-08: write pressure over the soft budget halves 8->4");
    }
    {
        fc::WriteCapControllerState st; st.activeCap = 8;
        fc::WriteCapSample s = healthy(20, 100.0, 1000.0); s.writeFailures = 1;
        st = fc::NextWriteActiveCap(st, s, 32);
        Require(st.activeCap == 4 && st.lastReason == fc::WriteCapAdjustReason::HalveFailure,
                "W-03/AC-08: any write failure halves 8->4");
    }
    // --- S-01 (AC-21/AC-22/AC-23 / B10): backlog==0 is NOT a deterioration signal ---
    {
        // AC-21: an empty backlog with no other deterioration Holds the cap and resets the healthy
        // streak; the reason is Hold (the HalveBacklogEmpty reason no longer exists).
        fc::WriteCapControllerState st; st.activeCap = 8; st.consecutiveHealthy = 1;
        st = fc::NextWriteActiveCap(st, healthy(0, 100.0, 1000.0), 32);  // backlog empty
        Require(st.activeCap == 8 && st.consecutiveHealthy == 0 &&
                    st.lastReason == fc::WriteCapAdjustReason::Hold,
                "W-03/S-01/AC-21: empty backlog holds cap at 8 and resets the healthy streak");
    }
    {
        // AC-22: backlog==0 combined with a deterioration signal still halves, tagged with the
        // deterioration reason (never a backlog reason) since those checks precede backlog.
        {
            fc::WriteCapControllerState st; st.activeCap = 8;
            fc::WriteCapSample s = healthy(0, 100.0, 1000.0); s.backpressureSleep = true;
            st = fc::NextWriteActiveCap(st, s, 32);
            Require(st.activeCap == 4 && st.lastReason == fc::WriteCapAdjustReason::HalveBackpressure,
                    "W-03/S-01/AC-22: empty backlog + backpressure halves with backpressure reason");
        }
        {
            fc::WriteCapControllerState st; st.activeCap = 8;
            fc::WriteCapSample s = healthy(0, 100.0, 1000.0);
            s.ioInFlightBytes = 600; s.driverWriteOutstandingBytes = 600;  // pressure 1200 > budget
            st = fc::NextWriteActiveCap(st, s, 32);
            Require(st.activeCap == 4 && st.lastReason == fc::WriteCapAdjustReason::HalveBudget,
                    "W-03/S-01/AC-22: empty backlog + budget overflow halves with budget reason");
        }
        {
            fc::WriteCapControllerState st; st.activeCap = 8;
            fc::WriteCapSample s = healthy(0, 100.0, 1000.0); s.writeFailures = 1;
            st = fc::NextWriteActiveCap(st, s, 32);
            Require(st.activeCap == 4 && st.lastReason == fc::WriteCapAdjustReason::HalveFailure,
                    "W-03/S-01/AC-22: empty backlog + write failure halves with failure reason");
        }
        {
            // bp=1 triggers HalveBackpressure (priority 1), regardless of rate/latency.
            // (B-01 fix: relative-threshold path removed — bp==1 always hits HalveBackpressure first.)
            fc::WriteCapControllerState st; st.activeCap = 8;
            st = fc::NextWriteActiveCap(st, healthy(20, 100.0, 1000.0), 32);  // seed
            fc::WriteCapSample bp = healthy(20, 70.0, 1000.0); bp.backpressureSleep = true;
            st = fc::NextWriteActiveCap(st, bp, 32);
            Require(st.activeCap == 4 && st.lastReason == fc::WriteCapAdjustReason::HalveBackpressure,
                    "W-03/S-01/AC-22: bp=1 triggers HalveBackpressure (priority over rate)");
        }
        {
            // D-3a: rate=70 (> 20 abs threshold) does NOT halve.
            fc::WriteCapControllerState st; st.activeCap = 8;
            st = fc::NextWriteActiveCap(st, healthy(20, 100.0, 1000.0), 32);  // seed, Hold
            st = fc::NextWriteActiveCap(st, healthy(20, 70.0, 1000.0), 32);   // 70 > 20, no halve
            Require(st.activeCap == 8, "D-3a: rate 70 > abs threshold 20 -> no halve");
        }
        {
            // D-3a: rate=15 (< 20 abs threshold) DOES halve (real paralysis).
            fc::WriteCapControllerState st; st.activeCap = 8;
            st = fc::NextWriteActiveCap(st, healthy(20, 100.0, 1000.0), 32);  // seed, Hold
            st = fc::NextWriteActiveCap(st, healthy(20, 15.0, 1000.0), 32);   // 15 < 20, halve
            Require(st.activeCap == 4 && st.lastReason == fc::WriteCapAdjustReason::HalveRateDrop,
                    "D-3a: rate 15 < abs threshold 20 -> halve (real paralysis)");
        }
        {
            // D-1a: lat=1600ns (<< 15s abs threshold) does NOT halve.
            fc::WriteCapControllerState st; st.activeCap = 8;
            st = fc::NextWriteActiveCap(st, healthy(20, 100.0, 1000.0), 32);  // seed, Hold
            st = fc::NextWriteActiveCap(st, healthy(20, 100.0, 1600.0), 32);   // 1600ns << 15s, no halve
            Require(st.activeCap == 8, "D-1a: lat 1600ns << 15s abs threshold -> no halve");
        }
        {
            // D-1a: lat > 15s absolute threshold DOES halve (real saturation).
            fc::WriteCapControllerState st; st.activeCap = 8;
            st = fc::NextWriteActiveCap(st, healthy(20, 100.0, 1000.0), 32);  // seed, Hold
            const double absHalve = static_cast<double>(fc::kWriteLatencyHalveAbsNs) + 1.0;
            st = fc::NextWriteActiveCap(st, healthy(20, 100.0, absHalve), 32);
            Require(st.activeCap == 4 && st.lastReason == fc::WriteCapAdjustReason::HalveLatency,
                    "D-1a: lat > 15s abs threshold -> halve (real saturation)");
        }
        {
            // Trickle-rate-drop guard (regression for the real 1.22M-file run): empty backlog + rate
            // oscillation must NOT halve. A trickle workload (few files/sec) naturally alternates
            // rate 0<->~2; the previous logic halved on every zero-rate window and collapsed cap
            // 8->1, never recovering. With the backlog>0 gate, cap stays at 8 across the oscillation.
            fc::WriteCapControllerState st; st.activeCap = 8;
            for (int i = 0; i < 6; ++i) {
                const double rate = (i % 2 == 0) ? 2.0 : 0.0;  // alternate 2,0,2,0,...
                st = fc::NextWriteActiveCap(st, healthy(0, rate, 46000.0), 32);  // backlog=0, lat 46us
                Require(st.activeCap == 8 && st.lastReason == fc::WriteCapAdjustReason::Hold,
                        "W-03/S-01/trickle: empty-backlog rate oscillation holds cap at 8 (no false halve)");
            }
        }
        {
            // Empty backlog + latency rise must also NOT halve (a single high-latency completion
            // with no queued work is meaningless).
            fc::WriteCapControllerState st; st.activeCap = 8;
            st = fc::NextWriteActiveCap(st, healthy(0, 100.0, 1000.0), 32);   // seed prevLat=1000
            st = fc::NextWriteActiveCap(st, healthy(0, 100.0, 1600.0), 32);   // backlog=0, lat 1600>1500
            Require(st.activeCap == 8 && st.lastReason == fc::WriteCapAdjustReason::Hold,
                    "W-03/S-01: empty-backlog latency rise holds (no unmet demand, no false halve)");
        }
    }
    {
        // AC-23 / B10: inserting a backlog==0 window between healthy windows resets the streak, so a
        // later single healthy window only Holds; two more consecutive healthy windows are needed
        // before the cap grows again.
        fc::WriteCapControllerState st; st.activeCap = 8;
        st = fc::NextWriteActiveCap(st, healthy(20, 100.0, 1000.0), 32);  // healthy #1 -> Hold
        Require(st.activeCap == 8 && st.lastReason == fc::WriteCapAdjustReason::Hold,
                "W-03/S-01/AC-23: first healthy window holds");
        st = fc::NextWriteActiveCap(st, healthy(0, 100.0, 1000.0), 32);   // empty backlog -> Hold+reset
        Require(st.activeCap == 8 && st.consecutiveHealthy == 0 &&
                    st.lastReason == fc::WriteCapAdjustReason::Hold,
                "W-03/S-01/AC-23: inserted empty-backlog window holds and resets the streak");
        st = fc::NextWriteActiveCap(st, healthy(20, 100.0, 1000.0), 32);  // healthy again -> Hold
        Require(st.activeCap == 8 && st.lastReason == fc::WriteCapAdjustReason::Hold,
                "W-03/S-01/AC-23: first healthy window after the gap only holds (streak restarted)");
        st = fc::NextWriteActiveCap(st, healthy(20, 100.0, 1000.0), 32);  // 2nd consecutive -> Grow
        Require(st.activeCap == 9 && st.lastReason == fc::WriteCapAdjustReason::Grow,
                "W-03/S-01/AC-23: two consecutive healthy windows after the gap grow +1");
    }
    {
        // Rate drop >25% (needs a prev baseline): window1 seeds prevRate=100, window2 rate=70.
        // D-3a: backlog>0 + bp=0 => desensitized => absolute threshold. rate 70 > 20 => no halve.
        // This test now verifies the desensitized path does NOT false-halve on relative drops.
        fc::WriteCapControllerState st; st.activeCap = 8;
        st = fc::NextWriteActiveCap(st, healthy(20, 100.0, 1000.0), 32);  // Hold, prevRate=100
        st = fc::NextWriteActiveCap(st, healthy(20, 70.0, 1000.0), 32);   // 70 > 20 abs => no halve
        Require(st.activeCap == 8, "D-3a: relative rate drop 100->70 does NOT halve (desensitized, 70 > 20 abs)");
    }
    {
        // Latency rise >50% (needs a prev baseline): window1 prevLat=1000, window2 lat=1600.
        // D-1a: backlog>0 + bp=0 => desensitized => absolute threshold. 1600ns << 15s => no halve.
        fc::WriteCapControllerState st; st.activeCap = 8;
        st = fc::NextWriteActiveCap(st, healthy(20, 100.0, 1000.0), 32);  // Hold, prevLat=1000
        st = fc::NextWriteActiveCap(st, healthy(20, 100.0, 1600.0), 32);   // 1600ns << 15s => no halve
        Require(st.activeCap == 8, "D-1a: relative latency rise 1000->1600 does NOT halve (desensitized, 1600ns << 15s abs)");
    }
    {
        // Startup window: a previous sample may have no latency EWMA yet (0.0). The first real
        // latency sample must seed comparison state, not look like an infinite latency regression.
        fc::WriteCapControllerState st; st.activeCap = 8;
        st = fc::NextWriteActiveCap(st, healthy(20, 0.0, 0.0), 32);
        Require(st.activeCap == 8 && st.lastReason == fc::WriteCapAdjustReason::Hold,
                "W-03/B9: zero-latency startup window holds at initial cap");
        // D-3a note: rate must be >= 20 (abs threshold) when desensitized (backlog>0, bp=0)
        st = fc::NextWriteActiveCap(st, healthy(20, 100.0, 1000.0), 32);
        Require(st.activeCap == 9 && st.lastReason == fc::WriteCapAdjustReason::Grow,
                "W-03/B9: first real latency sample after zero baseline does not halve");
    }
    {
        // Startup rate baseline: prevRate==0 is a valid unseeded / no-completion state. A later
        // zero-completion window with healthy backlog must not be classified as a rate drop.
        // D-3a note: with desensitized gate open (backlog>0, bp=0) and abs threshold 20,
        // rate=0 (< 20) WILL halve on the 2nd window. So this test now uses rate=0 first
        // (havePrev=false, so no halve), then a healthy rate to confirm grow works.
        fc::WriteCapControllerState st; st.activeCap = 8;
        st = fc::NextWriteActiveCap(st, healthy(20, 0.0, 0.0), 32);  // havePrev=false, Hold
        st = fc::NextWriteActiveCap(st, healthy(20, 100.0, 1000.0), 32);  // healthy, grow
        Require(st.activeCap == 9 && st.lastReason == fc::WriteCapAdjustReason::Grow,
                "W-03/B9: zero-rate baseline does not falsely halve; healthy rate grows");
    }

    // --- Floor: halving at cap=1 stays at 1 (never 0) ---
    {
        fc::WriteCapControllerState st; st.activeCap = 1;
        fc::WriteCapSample s = healthy(20, 100.0, 1000.0); s.writeFailures = 1;
        st = fc::NextWriteActiveCap(st, s, 32);
        Require(st.activeCap == 1, "W-03/AC-08/B9: halving floors at 1 (never 0)");
    }

    // --- Ceiling: at cap == effectiveMax (32) two healthy windows Hold, never exceed 32 ---
    {
        fc::WriteCapControllerState st; st.activeCap = 32;
        st = fc::NextWriteActiveCap(st, healthy(40, 100.0, 1000.0), 32);
        st = fc::NextWriteActiveCap(st, healthy(40, 100.0, 1000.0), 32);
        Require(st.activeCap == 32, "W-03/B9: active cap never exceeds the 32 ceiling");
    }

    // --- Real worker capacity below the ceiling: effectiveMax = min(32, capacity) ---
    {
        // capacity 8: two healthy windows hold at 8 (cannot grow past capacity).
        fc::WriteCapControllerState st; st.activeCap = 8;
        st = fc::NextWriteActiveCap(st, healthy(40, 100.0, 1000.0), 8);
        st = fc::NextWriteActiveCap(st, healthy(40, 100.0, 1000.0), 8);
        Require(st.activeCap == 8, "W-03/AC-07/B9: cap never exceeds a poolMax below 32");
    }
    {
        // capacity 4 with a starting cap above it: the clamp pulls the cap down to the capacity.
        fc::WriteCapControllerState st; st.activeCap = 8;
        st = fc::NextWriteActiveCap(st, healthy(40, 100.0, 1000.0), 4);
        Require(st.activeCap == 4, "W-03/AC-07/B9: cap clamped down to worker capacity (4)");
    }

    // --- B9: no-completion window (rate 0 with prevRate 0) is NOT a rate-drop; pressure 0 is fine ---
    {
        fc::WriteCapControllerState st; st.activeCap = 8;
        fc::WriteCapSample s = healthy(20, 0.0, 1000.0);
        s.ioInFlightBytes = 0; s.driverWriteOutstandingBytes = 0;  // driver outstanding 0
        st = fc::NextWriteActiveCap(st, s, 32);  // havePrev=false -> healthy Hold
        Require(st.activeCap == 8 && st.lastReason != fc::WriteCapAdjustReason::HalveRateDrop,
                "W-03/B9: a zero-completion window with zero pressure does not halve on rate");
    }
    {
        // Recovery sequence: after a deterioration halves the cap, the controller should require one
        // healthy accumulation window, then grow by exactly +1 per subsequent healthy window.
        fc::WriteCapControllerState st; st.activeCap = 8;
        fc::WriteCapSample failed = healthy(20, 100.0, 1000.0);
        failed.writeFailures = 1;
        st = fc::NextWriteActiveCap(st, failed, 32);
        Require(st.activeCap == 4 && st.lastReason == fc::WriteCapAdjustReason::HalveFailure,
                "W-03/B9: failure halves 8->4 before recovery");
        st = fc::NextWriteActiveCap(st, healthy(20, 100.0, 1000.0), 32);
        Require(st.activeCap == 4 && st.lastReason == fc::WriteCapAdjustReason::Hold,
                "W-03/B9: first healthy recovery window holds");
        st = fc::NextWriteActiveCap(st, healthy(20, 100.0, 1000.0), 32);
        Require(st.activeCap == 5 && st.lastReason == fc::WriteCapAdjustReason::Grow,
                "W-03/B9: second healthy recovery window grows 4->5");
        st = fc::NextWriteActiveCap(st, healthy(20, 100.0, 1000.0), 32);
        Require(st.activeCap == 6 && st.lastReason == fc::WriteCapAdjustReason::Grow,
                "W-03/B9: continued healthy recovery grows by exactly +1");
    }

    // --- Latency gate: absolute thresholds (D-1a halve=15s, grow-gate=30s) ---
    {
        // Latency well below the D-1a halve absolute threshold (15s): grow still works.
        fc::WriteCapControllerState st; st.activeCap = 8;
        const double belowGate = 100.0 * 1000.0 * 1000.0;  // 100ms << 15s
        st = fc::NextWriteActiveCap(st, healthy(20, 100.0, belowGate), 32);  // Hold (streak=1)
        st = fc::NextWriteActiveCap(st, healthy(20, 100.0, belowGate), 32);  // Grow
        Require(st.activeCap == 9 && st.lastReason == fc::WriteCapAdjustReason::Grow,
                "latency-gate: below-threshold latency allows grow");
    }
    {
        // Latency above D-1a halve absolute threshold (15s) with desensitized gate:
        // HalveLatency fires (abs path), cap drops. This replaces the old "Hold" expectation.
        // Need 2 windows: first seeds havePrev, second triggers abs halve.
        fc::WriteCapControllerState st; st.activeCap = 8;
        const double aboveGate = static_cast<double>(fc::kWriteLatencyHalveAbsNs) + 1.0;
        st = fc::NextWriteActiveCap(st, healthy(20, 100.0, aboveGate), 32);  // Hold (havePrev=false, seed)
        st = fc::NextWriteActiveCap(st, healthy(20, 100.0, aboveGate), 32);  // halve (abs, havePrev=true)
        Require(st.activeCap == 4 && st.lastReason == fc::WriteCapAdjustReason::HalveLatency,
                "latency-gate: above D-1a abs threshold (15s) halves (desensitized abs path)");
    }
    {
        // Latency above the threshold does NOT block halve: deterioration still halves.
        // Use a latency below the D-1a abs threshold so HalveLatency doesn't fire first,
        // isolating the HalveFailure path.
        fc::WriteCapControllerState st; st.activeCap = 8;
        const double lowLat = 100.0 * 1000.0 * 1000.0;  // 100ms << 15s
        fc::WriteCapSample s = healthy(20, 100.0, lowLat);
        s.writeFailures = 1;
        st = fc::NextWriteActiveCap(st, s, 32);
        Require(st.activeCap == 4 && st.lastReason == fc::WriteCapAdjustReason::HalveFailure,
                "latency-gate: halve still works above threshold");
    }
    {
        // Recovery: after latency drops below the D-1a abs threshold, grow resumes.
        fc::WriteCapControllerState st; st.activeCap = 4;
        const double aboveAbs = static_cast<double>(fc::kWriteLatencyHalveAbsNs) + 1.0;  // > 15s
        const double belowAbs = 100.0 * 1000.0 * 1000.0;  // 100ms << 15s
        st = fc::NextWriteActiveCap(st, healthy(20, 100.0, aboveAbs), 32);  // Hold (havePrev=false, seed)
        st = fc::NextWriteActiveCap(st, healthy(20, 100.0, aboveAbs), 32);  // halve 4->2 (abs)
        Require(st.activeCap == 2, "latency-gate: above abs threshold halves during seed");
        st = fc::NextWriteActiveCap(st, healthy(20, 100.0, belowAbs), 32);  // Hold (streak=1, healthy)
        st = fc::NextWriteActiveCap(st, healthy(20, 100.0, belowAbs), 32);  // Grow (streak=2)
        Require(st.activeCap == 3 && st.lastReason == fc::WriteCapAdjustReason::Grow,
                "latency-gate: grow resumes after latency drops below threshold");
    }

    // --- D-5: protection window after cap change (root-cause §3.2 grow->immediate halve fix) ---
    {
        // D-5a: protection window suppresses RATE absolute threshold during cap-switch transient.
        // Scenario: halve 8->4 via failure, then rate=0.7 (real slow-disk transient from
        // real-slow-client.err:8) with backlog>0 && bp=0.
        //   Window 0: failure -> halve 8->4 (capChanged, windowsSinceLastChange=0, protection ON)
        //   Window 1: rate=0.7 < 20 BUT in protection window -> NOT halved (Hold)
        //   Window 2: rate=0.7 < 20, protection window expired -> halved 4->2
        // This pair is the V-01 critical assertion: if kWriteCapProtectAfterChangeWindows=0,
        // window 1 would halve (4->2) and the first assertion FAILS.
        fc::WriteCapControllerState st; st.activeCap = 8;
        fc::WriteCapSample fail = healthy(20, 100.0, 1000.0); fail.writeFailures = 1;
        st = fc::NextWriteActiveCap(st, fail, 32);  // halve 8->4, protection window starts
        Require(st.activeCap == 4, "D-5a: failure halves 8->4");
        Require(st.windowsSinceLastChange == 0, "D-5a: windowsSinceLastChange reset to 0 after halve");
        // Window 1: in protection window, rate=0.7 < 20 -> NOT halved
        st = fc::NextWriteActiveCap(st, healthy(20, 0.7, 1000.0), 32);
        Require(st.activeCap == 4 && st.lastReason == fc::WriteCapAdjustReason::Hold,
                "D-5a: rate 0.7 < 20 held during protection window (cap-switch transient)");
        Require(st.windowsSinceLastChange == 1, "D-5a: protection window counting up");
        // Window 2: protection window expired, rate=0.7 < 20 -> halved 4->2
        st = fc::NextWriteActiveCap(st, healthy(20, 0.7, 1000.0), 32);
        Require(st.activeCap == 2 && st.lastReason == fc::WriteCapAdjustReason::HalveRateDrop,
                "D-5a: rate 0.7 < 20 halves after protection window expires (real paralysis)");
    }
    {
        // D-5: LATENCY absolute threshold is NOT suppressed by protection window.
        // After grow, if latency exceeds 15s absolute, halve still fires immediately.
        fc::WriteCapControllerState st; st.activeCap = 1;
        st = fc::NextWriteActiveCap(st, healthy(20, 100.0, 1000.0), 32);
        st = fc::NextWriteActiveCap(st, healthy(20, 100.0, 1000.0), 32);  // Grow 1->2
        Require(st.activeCap == 2 && st.lastReason == fc::WriteCapAdjustReason::Grow,
                "D-5: grow 1->2 (protection window starts)");
        const double absHalve = static_cast<double>(fc::kWriteLatencyHalveAbsNs) + 1.0;
        st = fc::NextWriteActiveCap(st, healthy(20, 100.0, absHalve), 32);  // lat > 15s => halve
        Require(st.activeCap == 1 && st.lastReason == fc::WriteCapAdjustReason::HalveLatency,
                "D-5: absolute latency threshold fires even in protection window (slow-disk safety net)");
    }
}

// AC-10: pressure is a saturating byte sum, independent of the worker count.
void TC_ComposeWritePressure() {
    Require(fc::ComposeWritePressure(0, 0, 0) == 0, "zero pressure");
    Require(fc::ComposeWritePressure(100, 200, 300) == 600, "plain sum of the three byte terms");
    const uint64_t kMax = (std::numeric_limits<uint64_t>::max)();
    Require(fc::ComposeWritePressure(kMax, 1, 0) == kMax, "saturating add never wraps (queued)");
    Require(fc::ComposeWritePressure(kMax, kMax, kMax) == kMax, "saturating add stays at max");
}

// AC-10: the sleep ladder is a pure function of pressure vs the soft limit (never the worker count).
void TC_NextWriteBackpressureSleepUs() {
    const uint64_t L = 1000;
    Require(fc::NextWriteBackpressureSleepUs(0, L) == 0, "no pressure -> no sleep");
    Require(fc::NextWriteBackpressureSleepUs(L, L) == 0, "at the limit -> no sleep");
    Require(fc::NextWriteBackpressureSleepUs(L + 1, L) == 300, "just over -> 300us base");
    Require(fc::NextWriteBackpressureSleepUs(L + L / 4, L) == 300, "over <= limit/4 -> 300us");
    Require(fc::NextWriteBackpressureSleepUs(L + L / 4 + 1, L) == 700, "over > limit/4 -> 700us");
    Require(fc::NextWriteBackpressureSleepUs(L + L / 2 + 1, L) == 1500, "over > limit/2 -> 1500us");
    Require(fc::NextWriteBackpressureSleepUs(L + L + 1, L) == 3000, "over > limit -> 3000us");
    Require(fc::NextWriteBackpressureSleepUs(L + 2 * L + 1, L) == 5000, "over > 2*limit -> 5000us");
    // NFR-04: the sleep depends only on the byte inputs; there is no worker-count parameter to vary.
    Require(fc::NextWriteBackpressureSleepUs(5000, 1000) == 5000, "far over -> ladder top 5000us");
    // Saturating double guard: with softLimitBytes = 2^63 (kMax/2+1), an unsaturated limit*2 wraps
    // to 0, so any over>=1 would mis-route to the 5000us top tier. The saturating guard maps 2*limit
    // to kMax, so a tiny over (1 byte) correctly lands in the 300us base tier. This pins the guard.
    const uint64_t kMax = (std::numeric_limits<uint64_t>::max)();
    const uint64_t huge = (kMax / 2) + 1;  // 2^63; huge*2 would wrap to 0 without saturation
    Require(fc::NextWriteBackpressureSleepUs(huge + 1, huge) == 300,
            "near-2^63 soft limit: tiny over -> 300us base tier (saturated 2*limit, no wrap to 5000us)");
}

}  // namespace

void RunWanTuningTests() {
    TC_HashDepthLanUnchanged();
    TC_HashDepthWanBreaks8192();
    TC_DeltaSigDepth();
    TC_WanStreamLadder();
    TC_ResolveWanTuning();
    TC_DeltaErrorReleasesSigSlot();
    TC_ResolveWriteWorkerPoolMax();
    TC_UpdateEwma();
    TC_NextWriteActiveCap();
    TC_ComposeWritePressure();
    TC_NextWriteBackpressureSleepUs();
}
