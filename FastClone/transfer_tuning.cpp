#include "transfer_tuning.h"

#include <algorithm>
#include <cstdint>
#include <limits>

namespace fc {

TunedTransferOptions ResolveTransferOptions(const CliOptions& options) {
    TunedTransferOptions tuned;
    tuned.streamLimit = options.streamLimit;
    tuned.chunkSize = options.chunkSize;

    if (options.streamAutoTune) {
        // Keep default stream count conservative to reduce failure rate
        // on weak SSD/controllers when user doesn't explicitly set --streams.
        tuned.streamLimit = 4;
    }

    if (options.chunkAutoTune) {
        if (tuned.streamLimit <= 8) {
            tuned.chunkSize = 4 * 1024 * 1024;
        } else if (tuned.streamLimit <= 16) {
            tuned.chunkSize = 2 * 1024 * 1024;
        } else if (tuned.streamLimit <= 32) {
            tuned.chunkSize = 1024 * 1024;
        } else if (tuned.streamLimit <= 64) {
            tuned.chunkSize = 512 * 1024;
        } else {
            tuned.chunkSize = 256 * 1024;
        }
    }

    tuned.streamLimit = std::clamp<uint32_t>(tuned.streamLimit, 1, 1024);
    tuned.chunkSize = std::clamp<uint32_t>(tuned.chunkSize, 64 * 1024, 64 * 1024 * 1024);
    return tuned;
}

uint32_t EffectiveChunkSizeForStreams(uint32_t configuredChunkSize, uint32_t streamLimit) {
    uint32_t base;
    if (streamLimit <= 16) {
        base = std::max<uint32_t>(configuredChunkSize, 1024 * 1024);
    } else if (streamLimit <= 32) {
        base = std::max<uint32_t>(configuredChunkSize, 512 * 1024);
    } else {
        base = configuredChunkSize;
    }
    // Align down to 4 KiB (the common sector/page granularity) so that nextReadOffset
    // stays sector-aligned across sequential chunk reads in the unbuffered transfer path
    // (transfer-unbuffered). 1 MiB and 512 KiB are already aligned; this only affects an
    // oddly-configured chunkSize, preventing silent fallback to buffered IO when the
    // offset drifts off-sector after the first chunk.
    return std::max<uint32_t>(base & ~static_cast<uint32_t>(4095), 4096u);
}

size_t DownloadFlushThresholdForStreams(uint32_t streamLimit, uint32_t effectiveChunkSize) {
    if (streamLimit <= 16) {
        return std::max<size_t>(4 * 1024 * 1024, static_cast<size_t>(effectiveChunkSize) * 4);
    }
    if (streamLimit <= 32) {
        return std::max<size_t>(2 * 1024 * 1024, static_cast<size_t>(effectiveChunkSize) * 2);
    }
    return std::max<size_t>(512 * 1024, static_cast<size_t>(effectiveChunkSize));
}

uint32_t ResolveWriteWorkerPoolMax(uint32_t hardwareThreads) {
    const uint32_t hw = std::max<uint32_t>(1, hardwareThreads);
    // Physical pool size (poolMax) = clamp(hw, 8, 32) (D-04): never below the initial active cap so
    // it is always reachable, never above 32 so AV / filter-driver jitter stays bounded (R3).
    return std::clamp<uint32_t>(hw, kActiveCapInitial, kActiveCapMax);
}

double UpdateEwma(double prev, double sample, double alpha) {
    if (prev <= 0.0) {
        return sample;  // unseeded -> take the first sample verbatim
    }
    return alpha * sample + (1.0 - alpha) * prev;
}

const char* WriteCapAdjustReasonName(WriteCapAdjustReason reason) {
    switch (reason) {
        case WriteCapAdjustReason::Init:               return "init";
        case WriteCapAdjustReason::Grow:               return "grow";
        case WriteCapAdjustReason::Hold:               return "hold";
        case WriteCapAdjustReason::HalveBackpressure:  return "halve_backpressure";
        case WriteCapAdjustReason::HalveBudget:        return "halve_budget";
        case WriteCapAdjustReason::HalveRateDrop:      return "halve_rate_drop";
        case WriteCapAdjustReason::HalveLatency:       return "halve_latency";
        case WriteCapAdjustReason::HalveFailure:       return "halve_failure";
    }
    return "unknown";
}

WriteCapControllerState NextWriteActiveCap(const WriteCapControllerState& prev,
                                           const WriteCapSample& s, uint32_t workerCapacity) {
    // Effective ceiling: never above kActiveCapMax, never above the physical pool (FR-07 / AC-07).
    const uint32_t effectiveMax =
        std::clamp<uint32_t>(workerCapacity, kActiveCapMin, kActiveCapMax);

    // Write pressure for the controller = write-pool bytes + driver outstanding bytes (saturating).
    // Note: this is only a controller INPUT; the active cap / worker count never enter the
    // backpressure pressure (FR-09 decoupling lives in ComposeWritePressure).
    const uint64_t kMax = (std::numeric_limits<uint64_t>::max)();
    uint64_t writePressure = s.ioInFlightBytes;
    writePressure = (writePressure > kMax - s.driverWriteOutstandingBytes)
                        ? kMax
                        : writePressure + s.driverWriteOutstandingBytes;

    WriteCapControllerState next = prev;

    // 1) Deterioration (any true -> halve). Priority order maps 1:1 to the FR-08 halve list.
    WriteCapAdjustReason reason = WriteCapAdjustReason::Hold;
    bool deteriorated = true;
    if (s.backpressureSleep) {
        reason = WriteCapAdjustReason::HalveBackpressure;
    } else if (writePressure > s.recvSoftBudgetBytes) {
        reason = WriteCapAdjustReason::HalveBudget;
    } else if (prev.havePrev && s.backlog > 0 &&
               s.completionRate < prev.prevCompletionRate * kWriteCapRateDropFactor) {
        // Gated on backlog > 0 (unmet demand): a rate drop only means the write side is failing to
        // keep up when there is queued work. With backlog == 0 a zero-rate window is just demand
        // oscillation in a trickle workload (few files/sec), NOT deterioration. Without this gate the
        // cap self-collapses to kActiveCapMin=1 via repeated halve_rate_drop on natural 0<->rate
        // oscillation and can never recover (backlog==0 is not healthy) -- a throughput cliff on the
        // next burst. Observed in a real 1.22M-file run: cap 8->1 pinned, reason=halve_rate_drop.
        reason = WriteCapAdjustReason::HalveRateDrop;
    } else if (prev.havePrev && s.backlog > 0 && prev.prevLatencyEwmaNs > 0.0 &&
               s.latencyEwmaNs > prev.prevLatencyEwmaNs * kWriteCapLatencyHalveFactor) {
        // Same backlog>0 gate: a single high-latency completion with no queued work is meaningless.
        reason = WriteCapAdjustReason::HalveLatency;
    } else if (s.writeFailures > 0) {
        reason = WriteCapAdjustReason::HalveFailure;
    } else {
        // S-01 (FR-08 / M7 / R6): backlog == 0 is the normal gap between batches, NOT a
        // deterioration signal. With no other deterioration, it falls through to the healthy check
        // (which fails on `backlog > cap`) and lands in the Hold branch below -- keeping the cap
        // and resetting the healthy streak. The HalveBacklogEmpty reason was removed entirely.
        deteriorated = false;
    }

    uint32_t newCap = prev.activeCap;
    if (deteriorated) {
        newCap = std::max<uint32_t>(kActiveCapMin, prev.activeCap / 2);  // floor-halve (FR-08)
        next.consecutiveHealthy = 0;
        next.lastReason = reason;
    } else {
        // 2) Healthy (all true) -> accumulate streak, grow +1 from the 2nd consecutive window.
        const bool healthy =
            s.backlog > prev.activeCap &&
            !s.backpressureSleep &&
            writePressure <= s.recvSoftBudgetBytes &&
            (!prev.havePrev || s.completionRate >= prev.prevCompletionRate) &&
            (!prev.havePrev || prev.prevLatencyEwmaNs <= 0.0 ||
             s.latencyEwmaNs <= prev.prevLatencyEwmaNs * kWriteCapLatencyGrowFactor) &&
            s.latencyEwmaNs < kMaxLatencyForGrowNs;  // absolute gate: don't grow when device is saturated
        if (healthy) {
            next.consecutiveHealthy = prev.consecutiveHealthy + 1;
            if (next.consecutiveHealthy >= kWriteCapHealthyWindowsToGrow &&
                prev.activeCap < effectiveMax) {
                newCap = prev.activeCap + 1;  // NFR-08: at most +1 per window
                next.lastReason = WriteCapAdjustReason::Grow;
            } else {
                newCap = prev.activeCap;  // first healthy window (or already at ceiling) -> Hold
                next.lastReason = WriteCapAdjustReason::Hold;
            }
        } else {
            // 3) Neither deteriorating nor healthy -> Hold, reset the streak.
            newCap = prev.activeCap;
            next.consecutiveHealthy = 0;
            next.lastReason = WriteCapAdjustReason::Hold;
        }
    }

    next.activeCap = std::clamp<uint32_t>(newCap, kActiveCapMin, effectiveMax);
    next.prevCompletionRate = s.completionRate;
    next.prevLatencyEwmaNs = s.latencyEwmaNs;
    next.havePrev = true;
    return next;
}

uint64_t ComposeWritePressure(uint64_t queuedBytes, uint64_t ioInFlightBytes,
                              uint64_t driverWriteOutstandingBytes) {
    // Saturating add so a pathological set of inputs never wraps to a tiny pressure (FR-09/NFR-04).
    const uint64_t kMax = (std::numeric_limits<uint64_t>::max)();
    uint64_t p = queuedBytes;
    p = (p > kMax - ioInFlightBytes) ? kMax : p + ioInFlightBytes;
    p = (p > kMax - driverWriteOutstandingBytes) ? kMax : p + driverWriteOutstandingBytes;
    return p;
}

uint32_t NextWriteBackpressureSleepUs(uint64_t pressure, uint64_t softLimitBytes) {
    if (pressure <= softLimitBytes) {
        return 0;
    }
    const uint64_t over = pressure - softLimitBytes;
    // Saturating double: a pathological softLimitBytes near 2^63 must not wrap softLimitBytes*2 to a
    // tiny value and mis-route to a lighter sleep tier (defensive; real budgets are far below 2^63).
    const uint64_t kMax = (std::numeric_limits<uint64_t>::max)();
    const uint64_t doubleLimit = (softLimitBytes > (kMax / 2)) ? kMax : (softLimitBytes * 2);
    if (over > doubleLimit) {
        return 5000;
    }
    if (over > softLimitBytes) {
        return 3000;
    }
    if (over > (softLimitBytes / 2)) {
        return 1500;
    }
    if (over > (softLimitBytes / 4)) {
        return 700;
    }
    return 300;
}

bool IsWanRtt(long rttMs) {
    return rttMs > kWanRttThresholdMs;
}

uint32_t WanStreamsForRtt(long rttMs) {
    if (rttMs <= kWanRttThresholdMs) {
        return 4;  // LAN / metro: unchanged legacy auto count
    }
    if (rttMs <= 30) {
        return 8;
    }
    if (rttMs <= 60) {
        return 12;
    }
    return 16;
}

namespace {

// Bandwidth-delay product expressed in *files*: how many average-sized files fit inside
// (targetThroughput x RTT). This is the count that must stay in flight to keep the data
// pipeline fed across one round trip (design section 3.2). Saturates instead of overflowing.
size_t BdpDepthFiles(long rttMs, uint64_t avgFileBytes) {
    const long rtt = std::max<long>(rttMs, kWanRttFloorMs);
    const double rttSec = static_cast<double>(rtt) / 1000.0;
    const double bdpBytes = (static_cast<double>(kWanTargetThroughputBps) / 8.0) * rttSec;
    const uint64_t avg = std::clamp<uint64_t>(
        avgFileBytes != 0 ? avgFileBytes : kWanDefaultAvgFileBytes,
        kWanMinAvgFileBytes, kWanMaxAvgFileBytes);
    const double depth = bdpBytes / static_cast<double>(avg);
    if (depth <= 0.0) {
        return 0;
    }
    if (depth >= static_cast<double>(SIZE_MAX)) {
        return SIZE_MAX;
    }
    return static_cast<size_t>(depth + 0.999);  // ceil
}

}  // namespace

size_t ComputeHashInflightDepth(long rttMs, uint32_t streamLimit, uint64_t avgFileBytes) {
    // Legacy formula (sync_engine.cpp:3093 original): clamp(max(1024, streamLimit*256),1024,8192).
    const size_t legacy = std::clamp<size_t>(
        std::max<size_t>(kHashFloorDepth, static_cast<size_t>(streamLimit) * 256),
        kHashFloorDepth, kHashLanCeilDepth);
    if (!IsWanRtt(rttMs)) {
        return legacy;  // LAN / metro: byte-for-byte legacy (HC-04 / NFR-03)
    }
    // WAN: deepen the request pipeline so it never drains within one RTT. Two lower-bound
    // terms, take the max then clamp to the raised ceiling:
    //   (a) BDP-in-files: enough files in flight to fill the data pipe behind hashing;
    //   (b) RTT pipeline floor: legacy hard cap scaled by RTT/threshold, so a 25ms link
    //       targets a depth well above the old 8192 cap (AC-02). Both constexpr-tunable.
    const size_t bdp = BdpDepthFiles(rttMs, avgFileBytes);
    const long mult = std::max<long>(1, rttMs / kWanRttThresholdMs);
    const size_t rttFloor = kHashLanCeilDepth * static_cast<size_t>(mult);
    const size_t depth = std::max<size_t>(
        std::max<size_t>(static_cast<size_t>(streamLimit) * 256, bdp), rttFloor);
    return std::clamp<size_t>(depth, kHashFloorDepth, kHashWanCeilDepth);
}

bool DeltaErrorReleasesSigSlot(bool rangeTagged, bool deltaStateExists, bool sigReceived) {
    // Only a sig-level error (no active range) for a managed, not-yet-answered signature.
    return !rangeTagged && deltaStateExists && !sigReceived;
}

size_t ComputeDeltaSigInflightDepth(long rttMs, uint64_t avgDeltaFileBytes) {
    if (!IsWanRtt(rttMs)) {
        return 0;  // 0 => unbounded: preserves the legacy "send the whole queue" semantics
    }
    const uint64_t avg = avgDeltaFileBytes != 0 ? avgDeltaFileBytes : kWanDefaultAvgDeltaBytes;
    const size_t bdp = BdpDepthFiles(rttMs, avg);
    const long mult = std::max<long>(1, rttMs / kWanRttThresholdMs);
    const size_t rttFloor = static_cast<size_t>(256) * static_cast<size_t>(mult);
    const size_t depth = std::max<size_t>(std::max<size_t>(bdp, rttFloor), size_t{256});
    return std::clamp<size_t>(depth, size_t{1}, kDeltaSigWanCeilDepth);
}

WanTuning ResolveWanTuning(const TunedTransferOptions& base, const CliOptions& cli,
                           long sessionRttMs, uint32_t hwConcurrency) {
    WanTuning t;
    t.wanMode = IsWanRtt(sessionRttMs);

    uint32_t streams = base.streamLimit;
    // The WAN stream bump applies only to auto-tuned sessions (user did NOT pass --streams),
    // so explicit configuration is preserved (design D-05 / gatekeeper #7).
    if (cli.streamAutoTune && t.wanMode) {
        const uint32_t hw = std::max<uint32_t>(1, hwConcurrency);
        const uint32_t hwCap = std::max<uint32_t>(4, hw * 2);
        streams = WanStreamsForRtt(sessionRttMs);
        streams = std::min<uint32_t>(streams, kWanStreamCap);
        streams = std::min<uint32_t>(streams, hwCap);
        streams = std::max<uint32_t>(streams, base.streamLimit);  // never below the base
    }
    t.streamLimit = streams;
    // hash depth is RTT-driven regardless of explicit --streams (FR-02 is about in-flight
    // requests, not lane count); it still falls back to the legacy value on LAN.
    t.maxInFlightHashRequests = ComputeHashInflightDepth(sessionRttMs, streams, 0);
    t.maxInFlightDeltaSig = ComputeDeltaSigInflightDepth(sessionRttMs, 0);
    return t;
}

}  // namespace fc
