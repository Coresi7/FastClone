#pragma once

// A-1: this internal header owns the single preamble previously duplicated verbatim
// at the top of sync_engine_{shared,server,client}.cpp. Each of those TUs now includes
// only this header. The public header is pulled in here (it merely depends on cli.h and
// never includes this file, so there is no cycle); no new public symbols are introduced.
#include "sync_engine.h"

#include "client_handshake.h"
#include "delta.h"
#include "disk_io_driver.h"
#include "file_index.h"
#include "hash_memcache.h"
#include "link_scheduler.h"
#include "net_topology.h"
// Re-export the shared parallel directory-walk component (task fastcheck-extra-parallel-walk,
// D-01 candidate B). The ParallelDirWalk template + JoinDiag/ResolveDirWalkWorkerCount/BuildRelPath/
// OpenDirFind were migrated into parallel_dir_walk.{h,cpp}; including it here keeps every sync-engine
// TU (notably sync_engine_client.cpp / RemoveLocalExtras) source-unchanged.
#include "parallel_dir_walk.h"
#include "path_utils.h"
#include "protocol.h"
#include "protocol_codec.h"
#include "sync_util.h"
#include "transfer_tuning.h"
#include "win_socket.h"

#ifdef _WIN32
#include <Windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <sys/socket.h>
#include <unistd.h>
#elif defined(__linux__)
#include <sys/socket.h>
#include <unistd.h>
#endif
#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace fc {
namespace detail {

// Cross-TU internal tools shared by sync_engine_{shared,server,client}.cpp.
// ReadWholeFile is defined in sync_engine_server.cpp. The parallel-walk helpers
// (JoinDiag/ResolveDirWalkWorkerCount/BuildRelPath/OpenDirFind + the ParallelDirWalk
// template + the kDirPopBatch/kDeleteDirPopBatch/kFrameFlushThreshold constants) now
// live in parallel_dir_walk.h (re-exported above) and are defined in parallel_dir_walk.cpp.
bool ReadWholeFile(const std::filesystem::path& abs, std::vector<uint8_t>& out);

// B-1: nearest-rank percentile value from an ALREADY-SORTED, NON-EMPTY vector.
// idx = round(p*(n-1)), clamped to n-1. Callers own sorting and empty handling
// (server pct: sort-per-call + empty->0; client: sort once, guarded non-empty).
// Definition in sync_engine_shared.cpp.
int64_t PercentileNearestRank(const std::vector<int64_t>& sorted, double p);

}  // namespace detail
}  // namespace fc
