// Unit tests for the multipath transfer scheduler's lane selection (design section 8).
// SelectLeastLoadedLane is a pure function over a per-lane load snapshot, so these tests
// construct synthetic lane vectors and assert which lane is chosen, with no network / threads.
//   - shortest-queue (least outstanding streams) selection,
//   - tie-break to the lowest index (primary),
//   - streamLimit saturation skipping,
//   - health filtering,
//   - forcePrimary hard-pin semantics (wait vs. fall back when primary is down).

#include "link_scheduler.h"

#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void Require(bool cond, const char* msg) {
    if (!cond) {
        throw std::runtime_error(std::string("link-scheduler: ") + msg);
    }
}

using fc::LaneLoad;
using fc::SelectLeastLoadedLane;

constexpr uint32_t kLimit = 8;

// Steady-state weighted-allocation simulator (aux-weight V-02/V-03). Each lane drains at a rate
// equal to its weight (a fractional accumulator handles non-integer weights), and the pool is
// refilled to saturation every tick via SelectLeastLoadedLane. In steady state allocations ==
// drain throughput, so each lane's allocation count converges to its weight, i.e. the per-lane
// allocation ratio equals the weight ratio (FR-01/FR-04). Returns per-lane allocation counts
// accumulated after `warmup` ticks.
std::vector<long> SimulateWeightedAllocation(const std::vector<double>& weights,
                                             uint32_t streamLimit, int ticks, int warmup) {
    const size_t n = weights.size();
    std::vector<uint32_t> inFlight(n, 0);
    std::vector<double> acc(n, 0.0);
    std::vector<long> counts(n, 0);
    for (int t = 0; t < ticks; ++t) {
        // Drain proportional to weight.
        for (size_t i = 0; i < n; ++i) {
            acc[i] += weights[i];
            const uint32_t drain = static_cast<uint32_t>(acc[i]);
            acc[i] -= static_cast<double>(drain);
            inFlight[i] = (drain >= inFlight[i]) ? 0u : (inFlight[i] - drain);
        }
        // Refill to saturation, counting each allocation once past warmup.
        for (;;) {
            std::vector<LaneLoad> lanes;
            lanes.reserve(n);
            for (size_t i = 0; i < n; ++i) {
                LaneLoad ld;
                ld.healthy = true;
                ld.inFlight = inFlight[i];
                ld.weight = weights[i];
                lanes.push_back(ld);
            }
            const int idx = SelectLeastLoadedLane(lanes, streamLimit, false);
            if (idx < 0) {
                break;
            }
            ++inFlight[static_cast<size_t>(idx)];
            if (t >= warmup) {
                ++counts[static_cast<size_t>(idx)];
            }
        }
    }
    return counts;
}

// Spec replica of the large-file lane decision (aux-weight FR-13~FR-16). Mirrors the inline
// logic in sync_engine.cpp's regular-dispatch path so the truth table is guarded as a
// regression spec (design V-05). Kept in lock-step with production by AC-05 below.
enum class LargeFileLaneSpec { Primary, Aux, Auto };
bool ComputeForcePrimary(LargeFileLaneSpec lane, double auxWeight, bool isLarge) {
    bool prefersAux = false;
    switch (lane) {
        case LargeFileLaneSpec::Primary: prefersAux = false; break;
        case LargeFileLaneSpec::Aux:     prefersAux = true; break;
        case LargeFileLaneSpec::Auto:    prefersAux = (auxWeight >= 2.0); break;
    }
    return isLarge && !prefersAux;
}

// ---- Shortest queue wins -----------------------------------------------------------------
void TC_ShortestQueueWins() {
    std::vector<LaneLoad> lanes = {
        LaneLoad{true, 5},  // primary, busier
        LaneLoad{true, 2},  // aux, shortest queue
    };
    Require(SelectLeastLoadedLane(lanes, kLimit, false) == 1,
            "must pick the lane with the fewest in-flight streams");
}

// ---- Tie breaks to the lowest index (primary) --------------------------------------------
void TC_TieBreakToPrimary() {
    std::vector<LaneLoad> lanes = {
        LaneLoad{true, 3},
        LaneLoad{true, 3},
        LaneLoad{true, 3},
    };
    Require(SelectLeastLoadedLane(lanes, kLimit, false) == 0,
            "equal queues must resolve to the lowest index (primary)");
}

// ---- Saturated lanes are skipped ---------------------------------------------------------
void TC_SaturationSkipped() {
    std::vector<LaneLoad> lanes = {
        LaneLoad{true, kLimit},      // saturated (== limit), ineligible despite lowest index
        LaneLoad{true, kLimit - 1},  // only eligible lane
    };
    Require(SelectLeastLoadedLane(lanes, kLimit, false) == 1,
            "a lane at streamLimit must be skipped");
}

// ---- All saturated / none healthy -> -1 --------------------------------------------------
void TC_NoneEligible() {
    std::vector<LaneLoad> allFull = {LaneLoad{true, kLimit}, LaneLoad{true, kLimit}};
    Require(SelectLeastLoadedLane(allFull, kLimit, false) == -1,
            "all lanes saturated must return -1");
    std::vector<LaneLoad> allDown = {LaneLoad{false, 0}, LaneLoad{false, 0}};
    Require(SelectLeastLoadedLane(allDown, kLimit, false) == -1,
            "no healthy lane must return -1");
    Require(SelectLeastLoadedLane(std::vector<LaneLoad>{}, kLimit, false) == -1,
            "empty pool must return -1");
}

// ---- Unhealthy lanes are filtered out ----------------------------------------------------
void TC_HealthFiltering() {
    std::vector<LaneLoad> lanes = {
        LaneLoad{false, 0},  // dead but shortest queue -> must be ignored
        LaneLoad{true, 4},   // only healthy lane
    };
    Require(SelectLeastLoadedLane(lanes, kLimit, false) == 1,
            "a dead lane must never be chosen even with the fewest streams");
}

// ---- forcePrimary pins to lane 0 while it has a free slot --------------------------------
void TC_ForcePrimaryPinsWhenFree() {
    std::vector<LaneLoad> lanes = {
        LaneLoad{true, 6},  // primary has room
        LaneLoad{true, 0},  // aux is emptier, but large files must NOT spill here
    };
    Require(SelectLeastLoadedLane(lanes, kLimit, true) == 0,
            "forcePrimary must pin a large file to the primary lane");
}

// ---- forcePrimary waits (returns -1) when primary is healthy but saturated ---------------
void TC_ForcePrimaryWaitsWhenSaturated() {
    std::vector<LaneLoad> lanes = {
        LaneLoad{true, kLimit},  // primary saturated but alive
        LaneLoad{true, 0},       // aux idle, but large file must wait, not spill
    };
    Require(SelectLeastLoadedLane(lanes, kLimit, true) == -1,
            "forcePrimary on a saturated-but-healthy primary must wait, not spill to aux");
}

// ---- forcePrimary falls back to least-loaded survivor only when primary is down ----------
void TC_ForcePrimaryFallsBackWhenDown() {
    std::vector<LaneLoad> lanes = {
        LaneLoad{false, 0},  // primary dead
        LaneLoad{true, 5},
        LaneLoad{true, 1},   // least-loaded survivor
    };
    Require(SelectLeastLoadedLane(lanes, kLimit, true) == 2,
            "a dead primary must degrade forcePrimary to the least-loaded surviving lane");
}

// ==========================================================================================
// aux-weight: weighted shortest-queue, manifest bias, large-file tristate, disconnect regress.
// ==========================================================================================

// ---- V-01 (AC-01/FR-06): default weight=1.0/bias=0 reproduces least-inFlight exactly --------
void TC_DefaultWeightMatchesLeastInFlight() {
    // Explicit weight/bias defaults must give the same pick as the legacy integer comparison.
    std::vector<LaneLoad> lanes = {
        LaneLoad{true, 4, 1.0, 0},
        LaneLoad{true, 2, 1.0, 0},  // fewest -> chosen
        LaneLoad{true, 3, 1.0, 0},
    };
    Require(SelectLeastLoadedLane(lanes, kLimit, false) == 1,
            "default weight/bias must reproduce the least-inFlight pick");
}

// ---- V-02 (AC-02/FR-01/FR-04): auxWeight=2.0 -> aux:primary allocation ~2:1 -----------------
void TC_WeightedRatioTwoToOne() {
    // Lane0 = primary (weight 1.0), lane1 = aux (weight 2.0); streamLimit ample.
    const std::vector<long> counts = SimulateWeightedAllocation({1.0, 2.0}, kLimit, 4000, 1000);
    Require(counts[0] > 0 && counts[1] > 0, "both lanes must receive allocations");
    const double ratio = static_cast<double>(counts[1]) / static_cast<double>(counts[0]);
    Require(ratio > 1.8 && ratio < 2.2,
            "auxWeight=2.0 must drive aux:primary allocation to about 2:1");
}

// ---- V-03 (AC-03/FR-01/FR-04): auxWeight=0.5 -> primary outpaces aux -----------------------
void TC_WeightedRatioBelowOne() {
    // Lane0 = primary (weight 1.0), lane1 = aux (weight 0.5): primary should win ~2:1.
    const std::vector<long> counts = SimulateWeightedAllocation({1.0, 0.5}, kLimit, 4000, 1000);
    Require(counts[0] > counts[1],
            "auxWeight<1.0 must give the primary a higher allocation share than aux");
    const double ratio = static_cast<double>(counts[0]) / static_cast<double>(counts[1]);
    Require(ratio > 1.8 && ratio < 2.2,
            "auxWeight=0.5 must drive primary:aux allocation to about 2:1");
}

// ---- V-04 (AC-04/FR-07/FR-08): manifest bias flips the pick, recycling restores it ----------
void TC_ManifestBiasShiftsThenRecovers() {
    // Equal inFlight, equal weight: with primary bias=+1 (manifest in flight) the next slot
    // goes to aux; with bias=0 (manifest done) the tie resolves back to the primary.
    std::vector<LaneLoad> biased = {
        LaneLoad{true, 2, 1.0, 1},  // primary, eff=(2+1)/1=3
        LaneLoad{true, 2, 1.0, 0},  // aux,     eff=2/1=2 -> chosen
    };
    Require(SelectLeastLoadedLane(biased, kLimit, false) == 1,
            "manifest bias on the primary must hand the next slot to aux");
    std::vector<LaneLoad> unbiased = {
        LaneLoad{true, 2, 1.0, 0},  // primary, eff=2 -> tie wins on lowest index
        LaneLoad{true, 2, 1.0, 0},  // aux,     eff=2
    };
    Require(SelectLeastLoadedLane(unbiased, kLimit, false) == 0,
            "with bias recycled to 0 the tie must resolve back to the primary");
}

// ---- V-05 (AC-05/FR-12~FR-16): large-file tristate truth table -----------------------------
void TC_LargeFileTristateTruthTable() {
    using L = LargeFileLaneSpec;
    // Non-large files never force primary regardless of policy/weight.
    Require(!ComputeForcePrimary(L::Primary, 1.0, false), "small file: primary policy -> weighted");
    Require(!ComputeForcePrimary(L::Aux, 4.0, false), "small file: aux policy -> weighted");
    Require(!ComputeForcePrimary(L::Auto, 4.0, false), "small file: auto policy -> weighted");
    // primary: large file is always hard-pinned (legacy FR-012).
    Require(ComputeForcePrimary(L::Primary, 1.0, true), "primary policy must pin large files");
    Require(ComputeForcePrimary(L::Primary, 8.0, true), "primary policy pins regardless of weight");
    // aux: large file uses weighted selection (never forced primary).
    Require(!ComputeForcePrimary(L::Aux, 1.0, true), "aux policy must not pin large files");
    Require(!ComputeForcePrimary(L::Aux, 0.5, true), "aux policy weighted even at low weight");
    // auto: prefers aux only when auxWeight >= 2.0, else behaves like primary.
    Require(ComputeForcePrimary(L::Auto, 1.0, true), "auto+low weight pins large files");
    Require(ComputeForcePrimary(L::Auto, 1.99, true), "auto just below 2.0 still pins");
    Require(!ComputeForcePrimary(L::Auto, 2.0, true), "auto at 2.0 prefers aux (weighted)");
    Require(!ComputeForcePrimary(L::Auto, 4.0, true), "auto above 2.0 prefers aux (weighted)");
}

// ---- V-06 (AC-06/FR-18): single lane -> primary path unchanged for any weight/bias ----------
void TC_SingleLaneAlwaysPrimary() {
    std::vector<LaneLoad> ready = {LaneLoad{true, 3, 16.0, 1}};
    Require(SelectLeastLoadedLane(ready, kLimit, false) == 0,
            "single healthy lane must always be chosen (weighted path)");
    Require(SelectLeastLoadedLane(ready, kLimit, true) == 0,
            "single healthy lane must always be chosen (forcePrimary path)");
    std::vector<LaneLoad> full = {LaneLoad{true, kLimit, 16.0, 0}};
    Require(SelectLeastLoadedLane(full, kLimit, false) == -1,
            "single saturated lane must return -1 regardless of weight");
    std::vector<LaneLoad> down = {LaneLoad{false, 0, 16.0, 0}};
    Require(SelectLeastLoadedLane(down, kLimit, false) == -1,
            "single dead lane must return -1 regardless of weight");
}

// ---- V-07 (AC-07/FR-05/FR-17): a saturated lane is never picked, even with huge weight -------
void TC_SaturatedLaneNeverPickedDespiteWeight() {
    std::vector<LaneLoad> lanes = {
        LaneLoad{true, kLimit, 16.0, 0},  // saturated but maximum weight
        LaneLoad{true, 5, 1.0, 0},        // only eligible lane
    };
    Require(SelectLeastLoadedLane(lanes, kLimit, false) == 1,
            "weight must never relax the streamLimit cap (raw inFlight filters candidates)");
}

// ---- V-08 (AC-08/FR-03/NFR-04): floating-point ties resolve to the lowest index, stably -----
void TC_FloatTieLowestIndexStable() {
    // eff0 = 2/2 = 1.0, eff1 = 1/1 = 1.0 -> exact tie -> lowest index.
    std::vector<LaneLoad> lanes = {
        LaneLoad{true, 2, 2.0, 0},
        LaneLoad{true, 1, 1.0, 0},
    };
    for (int rep = 0; rep < 5; ++rep) {
        Require(SelectLeastLoadedLane(lanes, kLimit, false) == 0,
                "a floating-point tie must resolve stably to the lowest index");
    }
}

// ---- section 5.1 disconnect / unhealthy-lane selection regression (V-12~V-15) ----------------------

// V-12 / scenario 1: a dead aux is skipped even if it is the emptiest + highest weight.
void TC_DeadAuxSkippedWeighted() {
    std::vector<LaneLoad> lanes = {
        LaneLoad{true, 2, 1.0, 0},
        LaneLoad{false, 0, 2.0, 0},  // dead aux: lowest inFlight, highest weight -> skipped
        LaneLoad{true, 1, 2.0, 0},
    };
    Require(SelectLeastLoadedLane(lanes, kLimit, false) == 2,
            "dead aux must be skipped; choice falls between surviving lanes by weight");
}

// V-12 / scenario 1: the only survivor is the primary; the dead aux is never chosen.
void TC_DeadAuxNotChosenEvenIfEmptiest() {
    std::vector<LaneLoad> lanes = {
        LaneLoad{true, 1, 1.0, 0},
        LaneLoad{false, 0, 2.0, 0},  // dead aux at inFlight=0 must not be picked
    };
    Require(SelectLeastLoadedLane(lanes, kLimit, false) == 0,
            "an empty but dead aux must never be chosen over a healthy primary");
}

// V-13 / scenario 2: prefer-aux (forcePrimary=false) falls back to primary when no aux survives.
void TC_OnlyPrimaryPreferAuxFallsToPrimary() {
    std::vector<LaneLoad> lanes = {
        LaneLoad{true, 3, 1.0, 0},
        LaneLoad{false, 0, 2.0, 0},
    };
    Require(SelectLeastLoadedLane(lanes, kLimit, false) == 0,
            "prefer-aux must fall back to the primary when no aux lane survives");
}

// V-13 / scenario 2: multiple dead aux lanes -> everything stays on the primary.
void TC_OnlyPrimaryAllAuxDown() {
    std::vector<LaneLoad> lanes = {
        LaneLoad{true, 5, 1.0, 0},
        LaneLoad{false, 0, 2.0, 0},
        LaneLoad{false, 0, 2.0, 0},
    };
    Require(SelectLeastLoadedLane(lanes, kLimit, false) == 0,
            "with all aux lanes down everything must stay on the primary");
}

// V-14 / scenario 3: primary down + manifest done (bias 0) -> weighted pick among aux.
void TC_PrimaryDownManifestDoneAuxWeighted() {
    std::vector<LaneLoad> lanes = {
        LaneLoad{false, 0, 1.0, 0},  // primary down
        LaneLoad{true, 2, 2.0, 0},   // eff = 1.0
        LaneLoad{true, 1, 2.0, 0},   // eff = 0.5 -> chosen
    };
    Require(SelectLeastLoadedLane(lanes, kLimit, false) == 2,
            "primary down: pick the lowest weighted load among surviving aux lanes");
}

// V-14 / scenario 3: primary down + tied aux -> lowest surviving index.
void TC_PrimaryDownAuxTieLowestIndex() {
    std::vector<LaneLoad> lanes = {
        LaneLoad{false, 0, 1.0, 0},  // primary down
        LaneLoad{true, 2, 2.0, 0},   // eff = 1.0
        LaneLoad{true, 2, 2.0, 0},   // eff = 1.0 -> tie -> lower index 1
    };
    Require(SelectLeastLoadedLane(lanes, kLimit, false) == 1,
            "primary down with tied aux must resolve to the lowest surviving index");
}

// V-15: forcePrimary fallback (primary down) honors weight in the degraded path.
void TC_ForcePrimaryFallbackHonorsWeight() {
    std::vector<LaneLoad> lanes = {
        LaneLoad{false, 0, 1.0, 0},  // primary down -> forcePrimary degrades to weighted
        LaneLoad{true, 4, 1.0, 0},   // eff = 4.0
        LaneLoad{true, 1, 2.0, 0},   // eff = 0.5 -> chosen
    };
    Require(SelectLeastLoadedLane(lanes, kLimit, true) == 2,
            "forcePrimary fallback must use weighted load among survivors");
}

// V-15 / zero regression: with default weights the forcePrimary fallback matches legacy.
void TC_ForcePrimaryFallbackDefaultWeightUnchanged() {
    std::vector<LaneLoad> lanes = {
        LaneLoad{false, 0, 1.0, 0},  // primary down
        LaneLoad{true, 5, 1.0, 0},
        LaneLoad{true, 1, 1.0, 0},   // least-loaded survivor
    };
    Require(SelectLeastLoadedLane(lanes, kLimit, true) == 2,
            "default-weight forcePrimary fallback must match the legacy least-inFlight pick");
}

}  // namespace

void RunLinkSchedulerTests() {
    TC_ShortestQueueWins();
    TC_TieBreakToPrimary();
    TC_SaturationSkipped();
    TC_NoneEligible();
    TC_HealthFiltering();
    TC_ForcePrimaryPinsWhenFree();
    TC_ForcePrimaryWaitsWhenSaturated();
    TC_ForcePrimaryFallsBackWhenDown();
    // aux-weight additions
    TC_DefaultWeightMatchesLeastInFlight();
    TC_WeightedRatioTwoToOne();
    TC_WeightedRatioBelowOne();
    TC_ManifestBiasShiftsThenRecovers();
    TC_LargeFileTristateTruthTable();
    TC_SingleLaneAlwaysPrimary();
    TC_SaturatedLaneNeverPickedDespiteWeight();
    TC_FloatTieLowestIndexStable();
    TC_DeadAuxSkippedWeighted();
    TC_DeadAuxNotChosenEvenIfEmptiest();
    TC_OnlyPrimaryPreferAuxFallsToPrimary();
    TC_OnlyPrimaryAllAuxDown();
    TC_PrimaryDownManifestDoneAuxWeighted();
    TC_PrimaryDownAuxTieLowestIndex();
    TC_ForcePrimaryFallbackHonorsWeight();
    TC_ForcePrimaryFallbackDefaultWeightUnchanged();
}
