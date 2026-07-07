#include "parallel_dir_walk.h"

// Definitions for the shared parallel directory-walk helpers (task fastcheck-extra-parallel-walk,
// D-01 candidate B). JoinDiag / ResolveDirWalkWorkerCount / BuildRelPath / OpenDirFind were migrated
// verbatim out of sync_engine_shared.cpp so the parallel walker is a self-contained component that
// FastClone, FastCloneTests, FastCheck and FastCheckTests can all link without pulling in the sync
// engine. PercentileNearestRank stayed in sync_engine_shared.cpp (it is not a walker dependency).

#include <iostream>

namespace fc {
namespace detail {

// Diagnostic wrapper around std::thread::join(). The exception
// "resource deadlock would occur" is thrown by join() in exactly one case: a
// thread tries to join ITSELF (t.get_id() == this_thread::get_id()), which on
// MSVC surfaces as std::system_error{errc::resource_deadlock_would_occur}.
// Behaviour:
//   - Normal join: wrapped in try/catch so the throwing call SITE is logged.
//   - Self-join: this signals corrupted thread ownership; we FAIL FAST (log the
//     site + std::abort). We deliberately do NOT skip or detach -- neither is a
//     safe recovery (see the detailed rationale at the self-join branch below),
//     and hiding the error would carry the corruption into later, harder-to-debug
//     failures. A run that hits the self-join branch is NOT a successful sync.
void JoinDiag(std::thread& t, const char* site) {
    if (!t.joinable()) {
        return;
    }
    const std::thread::id self = std::this_thread::get_id();
    const std::thread::id target = t.get_id();
    if (target == self) {
        // SELF-JOIN: a thread is trying to join itself -- the exact condition that
        // raises errc::resource_deadlock_would_occur. With the current design this is
        // UNREACHABLE: every join runs on a thread that does not own the target. If it
        // ever fires, thread ownership has already been violated (e.g. memory
        // corruption, or a captured std::thread reused across threads), so the process
        // state is no longer trustworthy.
        //
        // We FAIL FAST rather than pretend to recover:
        //   - join() is physically impossible here (would deadlock).
        //   - detach() is NOT a safe recovery: it only releases the std::thread handle.
        //     The thread body still references [&]-captured stack objects (mutex/cv/
        //     queue/map/socket); once the owning scope unwinds they are destroyed,
        //     producing use-after-free, silent corruption, false "success", or a hang
        //     -- strictly worse than a loud, immediate abort.
        // The site label pinpoints exactly which join detected the violation; any run
        // that reaches here must NOT be treated as a successful sync.
        std::cerr << "[deadlock-diag] FATAL SELF-JOIN at site=" << site
                  << " thread_id=" << target
                  << " -- thread ownership violated; aborting (state not trustworthy)"
                  << std::endl;
        std::cerr.flush();
        std::abort();
    }
    try {
        t.join();
    } catch (const std::system_error& e) {
        std::cerr << "[deadlock-diag] join FAILED at site=" << site
                  << " code=" << e.code().value()
                  << " msg=\"" << e.code().message() << "\""
                  << " caller_thread=" << self
                  << " target_thread=" << target
                  << std::endl;
        throw;
    }
}

// Worker count for the parallel directory walks (enumeration + deletion). These are
// latency-bound metadata reads, so oversubscribe the cores to keep the device queue
// deep, but cap the fan-out: past ~16 concurrent listings the queue is saturated and more
// threads only add work-queue lock churn.
unsigned ResolveDirWalkWorkerCount() {
    const unsigned hw = std::max<unsigned>(1u, std::thread::hardware_concurrency());
    return std::min<unsigned>(hw * 2, 16u);
}

// rel = relDir + "/" + name (or just name at the root). Single definition for every walk.
std::string BuildRelPath(const std::string& relDir, const std::string& name) {
    return relDir.empty() ? name : (relDir + "/" + name);
}

#ifdef _WIN32
// Open a FindFirstFileExW enumeration handle for a directory's children. Centralises the
// "append \\*" pattern build and the (basic-info + large-fetch) flags both walks use.
HANDLE OpenDirFind(const std::wstring& absDir, WIN32_FIND_DATAW& fd) {
    std::wstring pattern = absDir;
    if (!pattern.empty() && pattern.back() != L'\\' && pattern.back() != L'/') {
        pattern.push_back(L'\\');
    }
    pattern.append(L"*");
    return FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd,
                            FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
}
#endif

}  // namespace detail
}  // namespace fc
