#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace fc {

enum class Mode {
    Server,
    Client
};

// Large-file lane policy (aux-weight FR-12). Selects how files >= largeFileThresholdBytes are
// routed: Primary hard-pins to the primary link (legacy FR-012 behavior), Aux lets them flow
// through normal weighted selection (aux preferred via weight), Auto prefers aux only when
// auxWeight >= 2.0 and otherwise behaves like Primary.
enum class LargeFileLane {
    Primary,
    Aux,
    Auto
};

// Explicit client->server link pin (design section 9.3 / FR-008). Established verbatim; the
// list order defines the primary link. local may be a source IP or an interface name.
struct LinkPin {
    std::string local;
    std::string server;
    uint16_t port = 0;
};

struct CliOptions {
    Mode mode = Mode::Server;
    std::filesystem::path rootDir;
    std::string host = "127.0.0.1";
    uint16_t port = 27842;
    std::string password;
    uint32_t streamLimit = 16;
    uint32_t chunkSize = 256 * 1024;
    uint64_t queuedFileSizeBytes = 5ULL * 1024ULL * 1024ULL * 1024ULL;
    uint32_t serverHashWorkers = 0;
    bool enableHashMemcache = false;
    // OneShot server mode (--once). Server-only; the process exits after serving one real session.
    bool exitAfterSync = false;
    // Server-only: multiple real sessions plus graceful exit after idle grace (mutually exclusive with --once). Maps --once-multi (FR-01).
    bool onceMulti = false;
    // Idle grace in milliseconds, default 5s; only effective under onceMulti (FR-04). --once-idle-grace.
    uint64_t onceIdleGraceMs = 5000;
    // First-connect wait timeout in milliseconds, default 300s; only effective under --once / --once-multi. --wait-connect-timeout.
    // Covers only the continuous interval "from entering listen until the first valid connection is established"; permanently disabled after the first connection (FR-01/M3/FR-09).
    uint64_t waitConnectTimeoutMs = 300000;
    bool streamAutoTune = true;
    bool chunkAutoTune = true;
    bool diagnostics = false;
    // Max reconnect attempts per network drop (FR-016 / NFR-002). Default 10; 0 disables
    // auto-reconnect. The count is reset to 0 every time a session is successfully
    // established, so each drop independently gets up to reconnectRetries tries -- there is
    // no total time window; a long transfer that drops after >30min still retries its full
    // budget (the legacy 30-minute reconnect-window cap was removed).
    uint32_t reconnectRetries = 10;

    // --- Multipath transfer (FC6) ---
    // Files >= this size are pinned to the primary link (FR-011/012). Default 1GB,
    // semantically independent from smallFileBatchThreshold / queuedFileSizeBytes.
    uint64_t largeFileThresholdBytes = 1ULL << 30;
    // Uniform ordering weight for all aux lanes in the weighted shortest-queue rule
    // (aux-weight FR-09). Primary stays 1.0; range (0,16]; default 1.0 = zero regression.
    double auxWeight = 1.0;
    // Large-file lane policy (aux-weight FR-12). Default Auto; with default auxWeight (<2.0)
    // Auto keeps the legacy primary-pin behavior, so the default is zero regression.
    LargeFileLane largeFileLane = LargeFileLane::Auto;
    // Server endpoints for the connection pool (FR-005). servers[0] mirrors host/port for
    // call-site compatibility; multi-value via comma-separated or repeated --server.
    std::vector<std::pair<std::string, uint16_t>> servers;
    // Explicit link pins (FR-008). When non-empty, automatic selection is bypassed.
    std::vector<LinkPin> linkPins;
    // Hard cap on the connection pool size (gatekeeper default <= 8, design R-05).
    uint32_t maxConnections = 8;

    // --- TCP socket-buffer overrides (WAN single-TCP tuning) ---
    // 0 (default) = let the kernel autotune the window (recommended; receive-window autotuning
    // scales to the BDP on high-RTT links). A positive value pins the buffer (disabling
    // receive-window autotuning for that direction); range [64K, 1G]. Experimentation knobs
    // for high-BDP single-TCP links, applied to every connected/accepted socket.
    uint64_t tcpSendBufferBytes = 0;
    uint64_t tcpRecvBufferBytes = 0;

    // --- Binary delta (FC7) ---
    // Client-only minimum file size to attempt block-level binary delta (binary-delta FR-01).
    // 0 (default) disables delta entirely (zero regression). A positive value must be in
    // [1M, 1T]. Parsed with ParseSizeBytesStrict (K|M|G suffix), fully orthogonal to
    // --large-file-threshold (FR-04).
    uint64_t deltaMinSizeBytes = 0;

    // --- Unbuffered client writes (unbuffered-writes M1/FR-01/FR-12) ---
    // Client-only. When set, all client file-content writes (whole-file, small-file batch,
    // single-file download, delta copy/range) express unbuffered write intent to the unified disk
    // IO driver so downloaded data bypasses the OS page cache. Default false = zero regression
    // (M5/FR-03); the switch never changes final file content, only the write intent.
    bool unbufferedWrites = false;

    // NOTE (optimize-small-file-write-path W-03 / NFR-07): write concurrency has NO public tuning
    // knob. There is intentionally no writeWorkers field, no --write-workers flag and no
    // FASTCLONE_WRITE_WORKERS environment variable; the client converges an internal adaptive write
    // active cap on its own (see transfer_tuning.h NextWriteActiveCap). --write-workers is therefore
    // an unknown argument.
};

#if defined(_WIN32) && defined(_MSC_VER)
CliOptions ParseCli(int argc, wchar_t** argv);
#else
CliOptions ParseCli(int argc, char** argv);
#endif

// Full CLI usage/help text (S-03 / FR-17 / D-15-A). Exposed so the help wording can be asserted in
// tests without redirecting std::cerr; PrintUsage() simply streams this string. The
// --unbuffered-writes section expresses an unbuffered write INTENT and names the small-file /
// unaligned / tail buffered-fallback cases, never promising that all writes bypass the OS cache.
std::string BuildUsageText();

}  // namespace fc
