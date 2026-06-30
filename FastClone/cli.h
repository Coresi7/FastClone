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

// Explicit client->server link pin (design §9.3 / FR-008). Established verbatim; the
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
    // OneShot 服务端模式（--once）。仅服务端有效；服务完一个真实会话后进程退出。
    bool exitAfterSync = false;
    // 服务端专用：多真实会话并发 + 空闲宽限后优雅退出（与 --once 互斥）。映射 --once-multi（FR-01）。
    bool onceMulti = false;
    // 空闲宽限毫秒数，默认 5s；仅在 onceMulti 下有效（FR-04）。--once-idle-grace。
    uint64_t onceIdleGraceMs = 5000;
    // 首连等待超时毫秒数，默认 300s；仅在 --once / --once-multi 下有效。--wait-connect-timeout。
    // 仅覆盖「进入监听到首个有效连接达成之前」这一连续区间；首连后永久失效（FR-01/M3/FR-09）。
    uint64_t waitConnectTimeoutMs = 300000;
    bool streamAutoTune = true;
    bool chunkAutoTune = true;
    bool diagnostics = false;
    uint32_t reconnectRetries = 10;
    uint64_t reconnectWindowMs = 30ULL * 60ULL * 1000ULL;

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
};

#if defined(_WIN32) && defined(_MSC_VER)
CliOptions ParseCli(int argc, wchar_t** argv);
#else
CliOptions ParseCli(int argc, char** argv);
#endif

}  // namespace fc
