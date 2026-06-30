#pragma once

#include "cli.h"

#include <cstddef>
#include <cstdint>

namespace fc {

struct TunedTransferOptions {
    uint32_t streamLimit = 16;
    uint32_t chunkSize = 256 * 1024;
};

TunedTransferOptions ResolveTransferOptions(const CliOptions& options);

uint32_t EffectiveChunkSizeForStreams(uint32_t configuredChunkSize, uint32_t streamLimit);
size_t DownloadFlushThresholdForStreams(uint32_t streamLimit, uint32_t effectiveChunkSize);

// --- WAN small-file tuning (design docs/design/wan-smallfile-perf.md §3.2/§3.3) ----------
// Everything below is a pure function of the measured session RTT plus the already-resolved
// base tuning, so it is unit-testable without the network (V-01/V-02/V-09). All thresholds
// are compile-time tunable (constexpr). The whole WAN behavior is gated by RTT > the WAN
// threshold (OQ-07=B): at or below it every value is byte-for-byte identical to the legacy
// path so LAN / 同城 / 弱 SSD sessions are zero-regression (HC-04/NFR-03).
inline constexpr long kWanRttThresholdMs = 10;       // RTT > this => WAN mode gate
inline constexpr long kWanRttFloorMs = 1;            // floor to avoid div-by-zero in BDP
inline constexpr uint32_t kWanStreamCap = 16;        // hard cap on auto streams (FR-12/13)
inline constexpr size_t kHashFloorDepth = 1024;      // legacy in-flight floor (zero-regression)
inline constexpr size_t kHashLanCeilDepth = 8192;    // legacy hard ceiling (LAN unchanged)
inline constexpr size_t kHashWanCeilDepth = 65536;   // raised WAN ceiling (FR-02 / AC-02)
inline constexpr size_t kDeltaSigWanCeilDepth = 8192;  // WAN delta-signal in-flight ceiling
inline constexpr uint64_t kWanDefaultAvgFileBytes = 16ull * 1024;   // used when unobserved
inline constexpr uint64_t kWanMinAvgFileBytes = 4ull * 1024;
inline constexpr uint64_t kWanMaxAvgFileBytes = 1024ull * 1024;
inline constexpr uint64_t kWanDefaultAvgDeltaBytes = 256ull * 1024;
// Target throughput used to size the BDP-in-files term: 2x the 200Mbps NFR-01 floor as
// headroom (OQ-03=A). It only contributes a lower bound, never relaxes the ceilings.
inline constexpr uint64_t kWanTargetThroughputBps = 400ull * 1000 * 1000;
// Sliding-window transfer failure rate above which the WAN auto stream count is halved
// (floor 4) so weak SSD/controllers self-heal (design §3.3 / FR-13 / OQ-05=C).
inline constexpr double kStreamBackoffErrRate = 0.02;

struct WanTuning {
    uint32_t streamLimit = 4;
    size_t maxInFlightHashRequests = kHashFloorDepth;
    size_t maxInFlightDeltaSig = 0;  // 0 sentinel => unbounded (legacy "send all" semantics)
    bool wanMode = false;
};

// True when the measured RTT crosses the WAN gate. Negative/zero RTT (probe missing, B7) is
// treated as LAN so a failed RTT probe never enables WAN behavior (AC-15).
bool IsWanRtt(long rttMs);

// RTT ladder for the auto stream count (design §3.3): <=threshold->4, then 8/12/16 by RTT.
uint32_t WanStreamsForRtt(long rttMs);

// hash in-flight depth: legacy clamp on LAN; RTT-adaptive (breaks the old 8192 cap) on WAN.
size_t ComputeHashInflightDepth(long rttMs, uint32_t streamLimit, uint64_t avgFileBytes);

// delta-signal in-flight depth: 0 (unbounded/legacy) on LAN; bounded RTT-adaptive on WAN.
size_t ComputeDeltaSigInflightDepth(long rttMs, uint64_t avgDeltaFileBytes);

// B-01: decide whether an incoming DeltaError must release the WAN delta-signature in-flight
// slot that its BlockSigRequest consumed. A BlockSigRequest is answered by EXACTLY ONE of
// {BlockSigResponse, sig-level DeltaError}; BlockSigResponse already releases the slot. So only
// a *sig-level* error (no matching active range) for a managed, not-yet-answered signature is
// the substitute that must release here. Range-tagged errors arrive after the response already
// settled the count, and an already-answered (sigReceived) signature must not be double-released
// -- mirroring the BlockSigResponse pairing keeps the pipeline gate from leaking and wedging the
// sync on WAN. Pure predicate so the truth table is unit-testable (V-12).
bool DeltaErrorReleasesSigSlot(bool rangeTagged, bool deltaStateExists, bool sigReceived);

// Second-pass tuning once the session RTT is known (design D-02). Does NOT touch
// ResolveTransferOptions; the WAN stream bump only applies when the user did not pass an
// explicit --streams (cli.streamAutoTune), preserving explicit configuration.
WanTuning ResolveWanTuning(const TunedTransferOptions& base, const CliOptions& cli,
                           long sessionRttMs, uint32_t hwConcurrency);

}  // namespace fc
