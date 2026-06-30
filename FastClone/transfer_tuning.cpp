#include "transfer_tuning.h"

#include <algorithm>
#include <cstdint>
#include <thread>

namespace fc {

TunedTransferOptions ResolveTransferOptions(const CliOptions& options) {
    TunedTransferOptions tuned;
    tuned.streamLimit = options.streamLimit;
    tuned.chunkSize = options.chunkSize;

    const uint32_t hw = std::max<uint32_t>(1, std::thread::hardware_concurrency());
    (void)hw;

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
    if (streamLimit <= 16) {
        return std::max<uint32_t>(configuredChunkSize, 1024 * 1024);
    }
    if (streamLimit <= 32) {
        return std::max<uint32_t>(configuredChunkSize, 512 * 1024);
    }
    return configuredChunkSize;
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

bool IsWanRtt(long rttMs) {
    return rttMs > kWanRttThresholdMs;
}

uint32_t WanStreamsForRtt(long rttMs) {
    if (rttMs <= kWanRttThresholdMs) {
        return 4;  // LAN / 同城: unchanged legacy auto count
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
// pipeline fed across one round trip (design §3.2). Saturates instead of overflowing.
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
        return legacy;  // LAN / 同城: byte-for-byte legacy (HC-04 / NFR-03)
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
