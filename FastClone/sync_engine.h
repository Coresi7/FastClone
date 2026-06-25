#pragma once

#include "cli.h"

namespace fc {

// Process exit codes: a single authoritative scheme shared by both roles so values never
// collide across server/client semantics (oneshot-server FR-06/07, NFR-01). Each value has
// exactly one meaning regardless of role.
inline constexpr int kExitOk                    = 0;  // success: client sync done, or --once session completed cleanly
inline constexpr int kExitUsage                 = 1;  // CLI/usage error, fatal client error, or connect failure with reconnect disabled
inline constexpr int kExitFailedFiles           = 2;  // client: sync finished but some files failed (try lowering --streams)
inline constexpr int kExitIncompleteNoReconnect = 3;  // client: session dropped mid-sync and auto-reconnect is disabled
inline constexpr int kExitReconnectExhausted    = 4;  // client: auto-reconnect budget exhausted, sync still incomplete
inline constexpr int kExitOnceSessionFailed     = 5;  // server --once: the served session failed/aborted (distinct from client's 2)
inline constexpr int kExitWaitConnectTimeout    = 6;  // server --once/--once-multi: no valid connection before --wait-connect-timeout

int RunServer(const CliOptions& options);
int RunClient(const CliOptions& options);

}  // namespace fc
