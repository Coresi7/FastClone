#pragma once

#include <cstdint>
#include <vector>

namespace fc {

// Per-lane load snapshot used by the multipath transfer scheduler (design §8). A "lane" is
// one client->server transport connection in the pool; `inFlight` is the number of download
// streams (files / small-file batches) opened on it but not yet completed -- i.e. the lane's
// outstanding-request queue depth.
struct LaneLoad {
    bool healthy = false;     // lane usable (recvThread alive)
    uint32_t inFlight = 0;    // outstanding (opened, not-yet-finished) streams on this lane
};

// Pick the lane that should carry the next transfer, by SHORTEST QUEUE: among healthy lanes
// still below `streamLimit`, choose the one with the fewest outstanding streams; ties resolve
// to the lowest index (lane 0 is the primary). This is a least-outstanding-requests policy:
// it is self-correcting on lane speed (a slow lane holds its streams longer, so its inFlight
// stays high and it stops drawing new work, while a fast lane drains and is refilled) without
// any throughput measurement or feedback loop. Returns the chosen lane index, or -1 when no
// lane is eligible.
//
// forcePrimary (large files pinned to the primary, FR-012): hard-pin to lane 0. If lane 0 is
// healthy but already at streamLimit, returns -1 so the large file waits rather than spilling
// to an auxiliary lane; only when lane 0 is down does it fall back to the least-loaded
// surviving lane.
inline int SelectLeastLoadedLane(const std::vector<LaneLoad>& lanes,
                                 uint32_t streamLimit, bool forcePrimary) {
    if (lanes.empty()) {
        return -1;
    }
    if (forcePrimary) {
        const LaneLoad& primary = lanes[0];
        if (primary.healthy && primary.inFlight < streamLimit) {
            return 0;
        }
        if (primary.healthy) {
            return -1;  // primary saturated: large file waits (FR-020 degenerate)
        }
        // primary dead: degrade to best-effort placement on a surviving lane below.
    }
    int best = -1;
    uint32_t fewest = 0;
    for (int i = 0; i < static_cast<int>(lanes.size()); ++i) {
        const LaneLoad& c = lanes[i];
        if (!c.healthy || c.inFlight >= streamLimit) {
            continue;
        }
        if (best < 0 || c.inFlight < fewest) {
            fewest = c.inFlight;
            best = i;
        }
    }
    return best;
}

}  // namespace fc
