// Unit tests for the multipath transfer scheduler's lane selection (design §8).
// SelectLeastLoadedLane is a pure function over a per-lane load snapshot, so these tests
// construct synthetic lane vectors and assert which lane is chosen, with no network / threads.
//   - shortest-queue (least outstanding streams) selection,
//   - tie-break to the lowest index (primary),
//   - streamLimit saturation skipping,
//   - health filtering,
//   - forcePrimary hard-pin semantics (wait vs. fall back when primary is down).

#include "link_scheduler.h"

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
}
