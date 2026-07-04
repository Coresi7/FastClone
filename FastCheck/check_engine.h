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
#include <functional>

namespace fc::check {

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
