# FastClone

`FastClone` is a high-throughput directory sync tool built for extreme-scale trees: it rapidly syncs **tens of millions of files** and **TB-scale data**, handling a mix of huge files and enormous numbers of tiny files in a single pass (e.g. Unity/Unreal project folders, game build outputs, and large asset/dataset repositories).

Supported platforms:
- Windows: WinAPI fast path (highest performance)
- GNU/Linux / macOS: cross-platform implementation (fully functional; performance may be lower than Windows in some cases)

## Core Features

- Single executable: same binary can run as `server` or `client`
- Multi-connection parallel transfer (multipath / FC6): aggregate bandwidth across multiple NICs; also works fine with a single NIC / connection
- Mirror sync: files/directories deleted on server are also deleted on client
- Fallback verification: uses `XXH3_128` when `size + mtime` do not match
- Scales to tens-of-millions-file, TB-scale trees: tiny files are batched together while many files run concurrently across multiple streams / links (a single file itself is never split across streams)
- Strict protocol version check (version mismatch is rejected immediately)

## Expected Performance

- On i9-9900K + SATA SSD, syncing a ~3 million-file Library can finish in around 20 seconds when delta changes are limited (otherwise mainly bounded by network bandwidth).

## Usage

### Server

```bash
FastClone server [--dir <path>] [--port <n>] [--server-hash-workers <n>] [--enable-hash-memcache] [--once] --password <pwd>
```

- `--dir`: server root directory (default: current directory)
- `--port`: listening port (default: `27842`)
- `--server-hash-workers`: global hash worker threads for all sessions (`0` = auto, range `0..512`, default auto)
- `--enable-hash-memcache`: enable in-memory hash cache on server; reuses hash when `path + mtime + size` match
- `--once`: OneShot server mode — serve exactly one real session then exit (server-only, mutually exclusive with `--enable-hash-memcache`)
- `--password`: pre-shared password (required)

### Client

```bash
FastClone client --server <host[:port]>[,host:port...] --target <path> --password <pwd> [--streams <n>] [--chunk-kb <n>] [--queued-file-size <size>] [--large-file-threshold <size>] [--link <localIP|iface>=<serverIP[:port]>]... [--reconnect-retries <n>] [--reconnect-window <duration>]
```

- `--server`: accepts `10.0.0.8:27842` or `10.0.0.8` (default port `27842` if omitted); accepts a comma-separated list and/or may be repeated to supply multiple multipath endpoints
- `--target`: local target directory
- `--password`: password (must match server)
- `--streams`: concurrent stream count; auto-tuned when omitted (defaults to `4`, and explicit values above `8` print a reliability warning)
- `--chunk-kb`: chunk size in KB; auto-tuned when omitted, valid range `1..65536`
- `--queued-file-size`: soft receive-queue memory target for adaptive throttling (default: `5G`, range: `256M..64G`, supports suffixes `K/M/G`)
- `--large-file-threshold`: pins files `>=` this size to the primary link; default `1G`, range `1M..1T`, suffixes `K/M/G` (independent of the small-file batch threshold and the receive-queue target)
- `--link`: explicit `<localIP|iface>=<serverIP[:port]>` pairing (repeatable); bypasses automatic selection, and the first `--link` is the primary link
- `--reconnect-retries`: max session reconnect attempts on transient drops (default `10`, `0` disables)
- `--reconnect-window`: total reconnect time window (default `30m`, suffixes `s`/`m`/`h`)

## OneShot Server Mode (`--once`)

The `--once` flag enables a single-use server mode designed for CI/CD pipelines: the server accepts **exactly one real session**, completes the transfer, and exits immediately.

**Behavior:**

- The server listens for incoming connections. Pre-handshake probes (e.g. reachability checks that close before sending bytes) are silently ignored and do not count as a real session.
- When a real session completes cleanly (all connections finish without any lane error), the server exits with code `0`.
- If any lane of the single session encounters an error, the server exits with code `5` (session failed/aborted).
- If a second independent session arrives while the first is still in flight, it is rejected (the server refuses to serve it).
- Mutually exclusive with `--enable-hash-memcache` (the server process exits after one session, so a long-lived cache is pointless).

**Exit workflow:**

1. A working thread on the final connection sets a terminal verdict and closes the listening socket.
2. The main accept loop is woken by the socket close, reads the recorded verdict, and returns the exit code.
3. The process exits cleanly — no `exit()` or `abort()` calls mid-flight.

This mode is **server-only**; passing `--once` on the client side produces a CLI error.

## Progress Counters

The client prints and updates these counters in one line:

- `Enumrated`: remote files enumerated
- `Compared`: files fully decided
- `Unchanged`: files that do not need transfer
- `Failed`: files still failed after retries (up to 3 attempts)
- `Transfered`: files transferred successfully
- `Connections`: number of currently established connections (links)

Mirror-delete runs after all transfers and is not part of the live counters above; the delete phase prints `Delete done, <n> files` when it finishes.

## Multi-NIC Parallel Transfer (Multipath, FC6)

When the server and client each have multiple NICs, FastClone can use **several physical links at once** in a single sync to aggregate bandwidth:

- A session is a **connection pool**: the primary link is established first (the first `--server` endpoint / the first `--link`), then auxiliary links are added best-effort.
- **Automatic selection**: the client enumerates local NICs, probes reachability to the server-advertised endpoints, and pairs them by "address family (IPv4 first) > same-subnet > RTT", keeping **at most one connection per physical NIC on each side** (a dual-stack NIC's v4/v6 are not treated as two links).
- **Explicit pinning**: `--link <localIP|iface>=<serverIP[:port]>` bypasses automatic selection; the first `--link` is the primary link.
- **Large files stay on the primary link**: a single file cannot be split across links, so files `>= --large-file-threshold` (default `1G`) are pinned to the primary link (assumed best); other files / small-file batches are spread across links adaptively by measured throughput.
- With a single NIC / endpoint it degrades to a single connection, identical to prior behavior.

#### How to Enable / Disable Reachability Probing

**Server side** — endpoint advertisement is always on. When a client connects for the first time, the server automatically enumerates all local NIC addresses, groups them by physical interface, and sends them in the `AuthOk` frame. There is currently no CLI option to suppress advertisement; to prevent clients from probing extra endpoints, restrict reachability at the network/firewall level so that only one NIC is reachable from the client.

**Client side** — reachability probing is controlled by whether `--link` is specified:

| Mode | Condition | Behavior |
|------|-----------|----------|
| Auto (probing enabled) | No `--link` given, and server advertises > 1 endpoint **or** client has > 1 NIC candidate | Client enumerates local NICs, probes reachability to all server endpoints, then selects optimal link pairs |
| Auto-degraded (probing skipped) | No `--link` given, but only 1 server endpoint + 1 local candidate | Probing is skipped; falls back to single-link behavior |
| Explicit (probing bypassed) | One or more `--link` given | Client uses the pinned link(s) directly; no probing occurs |

To **disable** probing entirely, use `--link` to explicitly pin link(s). Even a single `--link` (primary only) will bypass all automatic selection and probing.

Diagnostics: the solution ships an **optionally-built** `FastCloneRouteProbe` project that probes a given server and prints the reachability matrix and the final link selection, to help diagnose link assignment in the field.

## Reconnect on Transient Network Failures

If the connection drops before the manifest is fully received, the client automatically reconnects and resumes without rerunning the whole command:

- Up to **10** session reconnects within a **30-minute** window by default, with exponential backoff (1s → 2s → 4s … capped at 30s)
- Connect failures while the server is not yet ready also consume the reconnect budget and back off instead of exiting immediately
- `--reconnect-retries 0` disables auto-reconnect (legacy behavior: exit code `3` on interruption)
- Files already on disk with matching `size+mtime` are skipped after reconnect
- **No block-level resume**: an interrupted large file is re-sent from the start (the protocol has no offset field)
- The current wire protocol is **FC6** (multipath session: adds `SessionJoin`, and `AuthOk` carries the session id and the server endpoint list); with multiple connections, a single dropped link migrates its in-flight files to healthy links, and only a fully-down pool triggers a session-level reconnect
- Unrecoverable errors (auth/protocol mismatch, frame desync, server Error frame) exit immediately (code `1`) and do not consume the reconnect budget

## Notes

- Current transport is plain TCP + password; use only in trusted networks
- Mirror mode removes extra files/directories on the client
- No block-level resume; rely on auto-reconnect or rerun after interruption (unchanged files are still skipped by comparison)

## Exit Codes

**Client:**
- `0`: sync succeeded (no failed files)
- `1`: argument error or runtime exception
- `2`: sync completed with failed files (try lowering concurrency, e.g. `--streams`)
- `3`: sync incomplete and auto-reconnect disabled (`--reconnect-retries 0`), or connection dropped without reconnect enabled
- `4`: auto-reconnect budget exhausted with the sync still incomplete

**Server `--once`:**
- `0`: the single real session completed cleanly
- `1`: argument/usage error (e.g. `--once` together with `--enable-hash-memcache`, or `--once` on the client)
- `5`: the single real session failed or was aborted (any lane error) — distinct from the client's `2` so every exit code has one unambiguous meaning

## Cross-Platform Build (Linux/macOS)

Dependencies:
- C++20 compiler
- CMake 3.16+
- xxhash development package

Install dependencies on macOS:

```bash
brew install cmake xxhash
```

Install xxhash development package on Linux:

- Debian / Ubuntu

```bash
sudo apt update
sudo apt install -y libxxhash-dev
```

- Fedora / RHEL / Rocky / Alma / CentOS Stream

```bash
sudo dnf install -y xxhash-devel
```

If `xxhash-devel` is not found, enable extra repos and retry:

```bash
sudo dnf install -y epel-release
sudo dnf config-manager --set-enabled crb
sudo dnf makecache
sudo dnf install -y xxhash-devel
```

- Arch / Manjaro

```bash
sudo pacman -S --needed xxhash
```

- openSUSE

```bash
sudo zypper install -y xxhash-devel
```

Optional sanity checks:

```bash
pkg-config --modversion libxxhash
pkg-config --cflags --libs libxxhash
```

Build:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Install to system path:

```bash
cmake --install build
```

Custom install prefix example:

```bash
cmake --install build --prefix /usr/local
```

After installation:

```bash
FastClone --help
```

