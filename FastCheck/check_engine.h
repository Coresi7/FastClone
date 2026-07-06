#pragma once

// FastCheck single-connection light orchestration engine (fastcheck, M7/M11/FR-27/28). After the handshake:
// request manifest -> dispatch frame by frame -> decide by mode -> pipeline HashRequests when needed (in-flight
// cap=--checkers) -> enumerate local extras -> assemble the report. Contains no sync multi-lane/transfer-queue/
// delta/reconnect logic. Network I/O goes through the injectable FrameChannel abstraction, so unit tests can use an
// in-memory scripted double (no real socket).

#include "check_options.h"
#include "check_report.h"
#include "protocol.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>

namespace fc::check {

// AIMD step rules for the two independent concurrency dimensions (fastcheck-parallel-hash M8/FR-12/
// FR-13). Extracted as pure functions so the increase/decrease behavior is unit-testable (AC-10/AC-11)
// without spinning up the worker pool.

// Local hash worker cap (--hash-workers dimension). Multiplicative decrease on any read-fail signal;
// additive increase when the task backlog is non-empty and all active workers are busy (localInFlight
// has caught up to the cap) and no failures occurred in the sample. Bounded to [1, maxCap].
std::size_t NextLocalWorkerCap(std::size_t currentCap, std::size_t maxCap, std::size_t taskQueueLen,
                               std::size_t localInFlight, std::uint64_t readFailDelta);

// Network in-flight window (--checkers dimension). Additive increase when the RTT sample is at or
// below the EWMA * stable factor; multiplicative decrease when it exceeds the EWMA * spike factor.
// A non-positive ewma means "not enough samples yet" and leaves the window unchanged. Bounded to
// [windowMin, windowMax].
std::size_t NextNetWindow(std::size_t currentWindow, double rttSampleUs, double rttEwmaUs,
                          std::size_t windowMin, std::size_t windowMax);

// Resolve the maximum hash-worker pool size from the requested --hash-workers value and the machine's
// hardware thread count. auto (hashWorkers == 0) pins the pool to hardwareThreads so small-file
// workloads do not thrash the NTFS/cache-manager kernel locks with 4x-core overshoot; an explicit
// --hash-workers N keeps the historical 4x headroom (max(N, 4*hardwareThreads)) for IO-bound tuning.
// Pure function so the cap policy is unit-testable without spinning up the worker pool (AC-03/AC-04).
std::size_t ResolveMaxHashWorkers(std::size_t hashWorkers, std::size_t hardwareThreads);

// Frame send/receive abstraction. send sends one frame; recv blocks reading one frame and throws on disconnect (engine uses this to decide partial + exit code 2).
struct FrameChannel {
    std::function<void(const Frame&)> send;
    std::function<Frame()> recv;
};

// Orchestration result: report + suggested exit code. The engine itself does not exit(); the exit code is returned to check_main.
struct EngineOutcome {
    CheckResult result;
    ExitCode exit = kIdentical;
};

// Run one full (or interrupted) comparison. interrupted is the Ctrl+C atomic flag, observed by the engine each round.
EngineOutcome RunCheck(const CheckOptions& o, FrameChannel& ch,
                       const std::atomic<bool>& interrupted);

}  // namespace fc::check
