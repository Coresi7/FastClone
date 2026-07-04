#pragma once

#include <cstdint>
#include <vector>

namespace fc {

// Per-lane load snapshot used by the multipath transfer scheduler (design section 8). A "lane" is
// one client->server transport connection in the pool; `inFlight` is the number of download
// streams (files / small-file batches) opened on it but not yet completed -- i.e. the lane's
// outstanding-request queue depth.
//
// weight/bias drive the weighted shortest-queue rule (aux-weight design section 3.1): the comparison
// value is effectiveLoad = (inFlight + bias) / weight. Both carry defaults that reproduce the
// original least-inFlight behavior exactly (weight=1.0, bias=0), so existing aggregate
// initializers like LaneLoad{true, 5} stay valid and zero-regression (FR-06).
//   - weight: ordering weight only; primary lane = 1.0, aux lanes = CLI auxWeight. CONTRACT:
//             weight > 0 (guaranteed by CLI validation (0,16] + this default). It NEVER relaxes
//             the streamLimit cap (FR-03/FR-05).
//   - bias:   ordering bias only; primary lane gets +1 while the manifest is still downloading
//             (manifestDone==false), 0 otherwise (FR-07/FR-08). Like weight, it never relaxes
//             the streamLimit cap.
struct LaneLoad {
    bool healthy = false;     // lane usable (recvThread alive)
    uint32_t inFlight = 0;    // outstanding (opened, not-yet-finished) streams on this lane
    double weight = 1.0;      // ordering weight (>0); primary 1.0, aux = auxWeight
    uint32_t bias = 0;        // ordering bias; primary +1 during manifest download, else 0
};

// Pick the lane that should carry the next transfer, by WEIGHTED SHORTEST QUEUE: among healthy
// lanes still below `streamLimit`, choose the one with the smallest effectiveLoad =
// (inFlight + bias) / weight; ties resolve to the lowest index (lane 0 is the primary). With
// the default weight=1.0 / bias=0 this is identical to the original least-outstanding-requests
// policy (effectiveLoad == inFlight), which is self-correcting on lane speed (a slow lane holds
// its streams longer, so its inFlight stays high and it stops drawing new work, while a fast
// lane drains and is refilled) without any throughput measurement or feedback loop. Returns the
// chosen lane index, or -1 when no lane is eligible.
//
// CONTRACT: every lane's weight must be > 0 (CLI validates auxWeight in (0,16]; the LaneLoad
// default is 1.0). The function does no divide-by-zero guard, to keep this pure helper simple.
//
// Candidate filtering and the streamLimit cap always use the RAW inFlight (never inFlight+bias),
// so weight/bias only reorder eligible lanes and can NEVER let a saturated lane be picked
// (FR-05/FR-17/AC-07).
//
// forcePrimary (large files pinned to the primary, FR-012): hard-pin to lane 0. If lane 0 is
// healthy but already at streamLimit, returns -1 so the large file waits rather than spilling
// to an auxiliary lane; only when lane 0 is down does it fall back to the weighted least-loaded
// surviving lane (weight/bias apply only on that fallback path, not to the hard-pin itself).
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
    // Small absolute epsilon for the floating-point tie rule: only a STRICTLY smaller load
    // switches the winner, so equal / near-equal effectiveLoad keeps the earlier (lower) index
    // (FR-03/AC-08/NFR-04). On the default path (weight=1.0/bias=0) effectiveLoad is an exact
    // integer and adjacent candidates differ by >= 1 >> kEps, so the comparison is bit-for-bit
    // equivalent to the original `c.inFlight < fewest` (FR-06/AC-01).
    constexpr double kEps = 1e-9;
    int best = -1;
    double bestLoad = 0.0;
    for (int i = 0; i < static_cast<int>(lanes.size()); ++i) {
        const LaneLoad& c = lanes[i];
        if (!c.healthy || c.inFlight >= streamLimit) {
            continue;
        }
        const double load = static_cast<double>(c.inFlight + c.bias) / c.weight;
        if (best < 0 || load < bestLoad - kEps) {
            bestLoad = load;
            best = i;
        }
    }
    return best;
}

}  // namespace fc
