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

// --- Write-worker pool + adaptive active cap (optimize-small-file-write-path W-03) --------------
// W-03 has NO public tuning knob (NFR-07): there is no --write-workers / FASTCLONE_WRITE_WORKERS
// and no CliOptions field. Write concurrency is a fixed max worker pool whose physical size derives
// from the hardware thread count, gated at runtime by an internal adaptive "active cap" the client
// converges on its own from the observed write backlog / in-flight bytes / completion rate /
// latency / backpressure signals (design section 3.3).

// Adaptive active-cap bounds (FR-07). The active cap is the number of workers allowed to execute a
// write task at the same instant; it starts at kActiveCapInitial and moves inside
// [kActiveCapMin, min(kActiveCapMax, poolMax)] where poolMax is the physical pool size.
inline constexpr uint32_t kActiveCapInitial = 8;   // FR-07 initial value
inline constexpr uint32_t kActiveCapMin     = 1;   // FR-07 lower bound
inline constexpr uint32_t kActiveCapMax     = 32;  // FR-07 upper bound

// Controller sampling interval (FR-08): the main loop samples the write signals once per window.
inline constexpr long kWriteCapSampleIntervalMs = 500;

// Adaptive thresholds (all constexpr-tunable, FR-08). The halve path uses ABSOLUTE thresholds
// (kWriteLatencyHalveAbsNs / kWriteRateDropAbsMinFilesPerSec, see below); the healthy/grow branch
// additionally requires the latency EWMA to stay at or below kWriteCapLatencyGrowFactor (<=+25%)
// of the previous window.
inline constexpr double kWriteCapLatencyGrowFactor = 1.25;
// FR-08 / D-12: the cap only grows after this many CONSECUTIVE healthy windows, then +1 per window.
inline constexpr uint32_t kWriteCapHealthyWindowsToGrow = 2;
// EWMA smoothing factor for the write-completion latency signal (UpdateEwma alpha).
inline constexpr double kWriteLatencyEwmaAlpha = 0.2;
// Absolute latency gate for grow: when the EWMA exceeds this threshold the controller
// refuses to grow the cap (only halve/Hold are allowed). Prevents oscillation on slow
// disks where grow -> concurrent IOPS collapse -> halve -> recover -> grow loops forever.
// 30s: calibrated from real-lib post data — fast NVMe max=12.3s (can grow), slow SSD p50=65.9s (blocked).
inline constexpr uint64_t kMaxLatencyForGrowNs = 30ULL * 1000ULL * 1000ULL * 1000ULL;

// --- D-1a/D-3a/D-5: small-file cap desensitization (root-cause §7) ---
// The OLD relative thresholds (1.5x latency / 25% rate drop) over-reacted to the natural jitter
// of small-file write latency (NTFS MFT lock contention) and cap-switch rate oscillation, collapsing
// cap 8->1 and never recovering (root-cause.md §3). They have been REMOVED and replaced by ABSOLUTE
// thresholds below. The absolute thresholds are calibrated from real-lib 230M-file measurements:
// fast NVMe latency p99=11.4s/max=12.3s; slow SSD p50=65.9s/max=273.5s. Real IOPS storms
// (rate≈0, latency≫15s) are still caught.

// D-1a: absolute latency halve threshold when desensitization gate is open.
// Calibrated from real-lib 230M-file post data (root-cause §2.5):
//   fast NVMe p99=11.4s, max=12.3s; slow SSD p50=65.9s, max=273.5s.
// 15s sits above fast-disk burst peaks (no false halve) but far below slow-disk saturation (halve fires).
inline constexpr uint64_t kWriteLatencyHalveAbsNs = 15ULL * 1000ULL * 1000ULL * 1000ULL;

// D-3a: absolute rate halve threshold when desensitization gate is open (files/sec).
// Real paralysis persists at rate 0.0 for multiple windows; cap-switch transients briefly
// dip low but are absorbed by the D-5 protection window.
// Known limitation: for large-file-only workloads (e.g. 8MB files at 110MB/s → ~14 files/s),
// this threshold will pin cap at 1. That is acceptable — sequential large-file writes are
// I/O-bound, not concurrency-bound, and cap=1 avoids seek thrashing. If future large-file
// scenarios need concurrent growth, lower this threshold (e.g. to 2 files/s).
inline constexpr double kWriteRateDropAbsMinFilesPerSec = 20.0;

// D-5: protection windows after a cap change (halve or grow). During this window, the RATE
// absolute threshold is suppressed (cap just moved, rate is transient). The LATENCY absolute
// threshold is NOT suppressed — real device saturation must halve immediately.
inline constexpr uint32_t kWriteCapProtectAfterChangeWindows = 1;

// Physical write-worker pool size (max concurrency ceiling, D-04): clamp(max(1,hw), 8, 32). This is
// the active cap's hard upper bound (poolMax); when hw < 32 the effective cap ceiling becomes poolMax
// (FR-07 "must not exceed the real worker capacity" / AC-07 "capacity below the ceiling"). Replaces
// the removed ResolveWriteWorkerCount (no explicit-config path anymore, NFR-07).
uint32_t ResolveWriteWorkerPoolMax(uint32_t hardwareThreads);

// Exponentially-weighted moving average update (pure, unit-testable). `prev <= 0` seeds the EWMA
// with the sample verbatim (write latency is strictly positive, so 0 is the "unseeded" sentinel).
double UpdateEwma(double prev, double sample, double alpha);

// Reason the active cap last changed (diagnostics FR-09; the halve reasons map 1:1 to the FR-08
// deterioration list, evaluated in this priority order). S-01 (FR-08/FR-09/M7): an empty backlog is
// no longer a deterioration signal, so there is deliberately no HalveBacklogEmpty reason -- the
// diagnostics can never emit it (D-13-B: the constraint is enforced at the type level).
enum class WriteCapAdjustReason {
    Init,
    Grow,
    Hold,
    HalveBackpressure,
    HalveBudget,
    HalveRateDrop,
    HalveLatency,
    HalveFailure
};

// Stable string for a reason (diagnostics + test assertions).
const char* WriteCapAdjustReasonName(WriteCapAdjustReason reason);

// One sampling-window observation fed to the controller (FR-08). All fields are read-only signals;
// the active cap and the real worker count are NEVER inputs to backpressure (FR-09 decoupling).
struct WriteCapSample {
    uint64_t backlog = 0;                     // ioTasks.size() snapshot
    uint64_t ioInFlightBytes = 0;             // write-pool buffered-but-unwritten bytes
    uint64_t driverWriteOutstandingBytes = 0; // driver submitted-not-reaped write bytes
    uint64_t recvSoftBudgetBytes = 0;         // receive-side soft budget (read-only observation)
    double   completionRate = 0.0;            // files completed this window / window seconds
    double   latencyEwmaNs = 0.0;             // current write-completion latency EWMA
    bool     backpressureSleep = false;       // receive side slept at least once this window
    uint64_t writeFailures = 0;               // write failures this window (delta)
};

// Explicit controller state carried across windows (main-thread only, D-10). NextWriteActiveCap is a
// pure function of (prev state, sample, pool capacity), so the FR-08 rules are unit-testable with no
// network / disk (AC-07 / AC-08).
struct WriteCapControllerState {
    uint32_t activeCap = kActiveCapInitial;
    uint32_t consecutiveHealthy = 0;
    double   prevCompletionRate = 0.0;
    double   prevLatencyEwmaNs = 0.0;
    bool     havePrev = false;
    WriteCapAdjustReason lastReason = WriteCapAdjustReason::Init;
    uint32_t windowsSinceLastChange = 0;  // D-5: windows since last halve/grow (protection window)
};

// Advance the active-cap controller by exactly one sampling window (FR-08 / NFR-08 / AC-07 / AC-08 /
// B9). `workerCapacity` is the physical pool size (poolMax); the effective ceiling is
// min(kActiveCapMax, workerCapacity). Rules (evaluated in order):
//   1. deterioration (ANY true -> halve, floor kActiveCapMin, reset healthy streak): backpressure
//      sleep; write pressure (ioInFlight + driverOutstanding) over the soft budget; completion rate
//      drop >25%; latency EWMA rise >50%; write failures > 0. (S-01: an empty backlog is NOT a
//      deterioration signal; it Holds the cap and resets the healthy streak via rule 3.)
//   2. healthy (ALL true): backlog > current cap; no backpressure sleep; write pressure <= soft
//      budget; completion rate not dropping; latency EWMA rise <=25%. First healthy window only
//      accumulates the streak (Hold); from the 2nd consecutive window on, +1 per window (Grow),
//      never above the effective ceiling.
//   3. otherwise Hold (reset healthy streak).
// The result cap is always clamped to [kActiveCapMin, min(kActiveCapMax, workerCapacity)].
WriteCapControllerState NextWriteActiveCap(const WriteCapControllerState& prev,
                                           const WriteCapSample& sample, uint32_t workerCapacity);

// Single write-backpressure pressure term (FR-09 / AC-10): the sum of the network receive queue,
// the write-pool buffered-but-unwritten bytes, and the driver's outstanding (submitted-not-reaped)
// write bytes. A byte budget, independent of the worker COUNT, so raising write workers never
// changes the pressure for a given set of byte inputs (NFR-04). Saturating add.
uint64_t ComposeWritePressure(uint64_t queuedBytes, uint64_t ioInFlightBytes,
                              uint64_t driverWriteOutstandingBytes);

// Backpressure sleep (microseconds) for a given pressure vs the soft byte limit (FR-09). Byte-for-
// byte the legacy ladder: at or below the limit -> 0 (no sleep); otherwise 300us base, stepping to
// 700/1500/3000/5000us as the overshoot crosses limit/4, limit/2, limit, 2*limit. Pure so AC-10 can
// assert it depends only on the byte inputs (never on the worker count).
uint32_t NextWriteBackpressureSleepUs(uint64_t pressure, uint64_t softLimitBytes);

// --- WAN small-file tuning (design docs/design/wan-smallfile-perf.md section 3.2/section 3.3) ----------
// Everything below is a pure function of the measured session RTT plus the already-resolved
// base tuning, so it is unit-testable without the network (V-01/V-02/V-09). All thresholds
// are compile-time tunable (constexpr). The whole WAN behavior is gated by RTT > the WAN
// threshold (OQ-07=B): at or below it every value is byte-for-byte identical to the legacy
// path so LAN / metro / weak-SSD sessions are zero-regression (HC-04/NFR-03).
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
// (floor 4) so weak SSD/controllers self-heal (design section 3.3 / FR-13 / OQ-05=C).
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

// RTT ladder for the auto stream count (design section 3.3): <=threshold->4, then 8/12/16 by RTT.
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
