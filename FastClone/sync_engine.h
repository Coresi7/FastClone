#pragma once

#include "cli.h"

namespace fc {

// Process exit codes (oneshot-server FR-06/07, NFR-01). Client reconnect codes (3/4) live
// in the client path and are intentionally disjoint from these server-only values.
inline constexpr int kExitOk                = 0;  // real session completed cleanly (FR-06)
inline constexpr int kExitUsage             = 1;  // CLI/usage error (FR-02/03/04 via main catch)
inline constexpr int kExitOnceSessionFailed = 2;  // served real session failed/aborted (FR-07)

int RunServer(const CliOptions& options);
int RunClient(const CliOptions& options);

}  // namespace fc
