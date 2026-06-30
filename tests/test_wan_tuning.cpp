// Unit tests for the WAN small-file tuning pure functions
// (docs/design/wan-smallfile-perf.md §3.2/§3.3, verification points V-01/V-02/V-09/V-11).
//
// These cover the RTT-gated decisions WITHOUT touching the network: RTT injection drives the
// hash in-flight depth, the delta-signal in-flight depth and the WAN stream ladder, while the
// LAN / 同城 path (RTT <= threshold) must stay byte-for-byte on the legacy values so the
// zero-regression guarantee (HC-04/NFR-03) is machine-checked.

#include "cli.h"
#include "transfer_tuning.h"

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace {

void Require(bool cond, const char* msg) {
    if (!cond) {
        throw std::runtime_error(std::string("wan-tuning: ") + msg);
    }
}

// Legacy hash depth at the default auto stream count (streamLimit=4): the original
// clamp(max(1024, 4*256), 1024, 8192) == 1024. Every LAN/同城 RTT must reproduce it exactly.
void TC_HashDepthLanUnchanged() {
    for (long rtt : {-1L, 0L, 1L, 5L, 10L}) {  // probe-missing, LAN, 同城, exactly at the gate
        const size_t depth = fc::ComputeHashInflightDepth(rtt, 4, 0);
        Require(depth == 1024, "LAN/同城 hash depth must equal the legacy 1024 (zero regression)");
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

}  // namespace

void RunWanTuningTests() {
    TC_HashDepthLanUnchanged();
    TC_HashDepthWanBreaks8192();
    TC_DeltaSigDepth();
    TC_WanStreamLadder();
    TC_ResolveWanTuning();
    TC_DeltaErrorReleasesSigSlot();
}
