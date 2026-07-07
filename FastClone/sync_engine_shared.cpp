#include "sync_engine_internal.h"

namespace fs = std::filesystem;

namespace fc {

namespace detail {

// JoinDiag / ResolveDirWalkWorkerCount / BuildRelPath / OpenDirFind were migrated to
// parallel_dir_walk.cpp (task fastcheck-extra-parallel-walk, D-01 candidate B) so the parallel
// directory walker is a standalone component linkable without the sync engine. Only
// PercentileNearestRank (not a walker dependency) remains here.

// Nearest-rank percentile value on an already-sorted, non-empty vector. Sorting and
// empty handling stay with the caller so the server (sort-per-call) and client (sort-once)
// keep their existing sort cadence; only the index math is shared here.
int64_t PercentileNearestRank(const std::vector<int64_t>& sorted, double p) {
    size_t idx = static_cast<size_t>(p * static_cast<double>(sorted.size() - 1) + 0.5);
    if (idx >= sorted.size()) {
        idx = sorted.size() - 1;
    }
    return sorted[idx];
}

}  // namespace detail

}  // namespace fc
