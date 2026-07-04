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
- **FastCheck**: a separate read-only comparison binary that reports differences between a local directory and a running server without transferring, deleting, or writing anything (see [FastCheck](#fastcheck-read-only-comparison))



## Expected Performance

- On i9-9900K + SATA SSD, syncing a ~3 million-file Library can finish in around 20 seconds when delta changes are limited (otherwise mainly bounded by network bandwidth).



## Usage



### Server

```bash
FastClone server [--dir <path>] [--port <n>] [--server-hash-workers <n>] [--enable-hash-memcache] [--once | --once-multi [--once-idle-grace <duration>]] [--wait-connect-timeout <duration>] --password <pwd>
```

- `--dir`: server root directory (default: current directory)
- `--port`: listening port (default: `27842`)
- `--server-hash-workers`: global hash worker threads for all sessions (`0` = auto, range `0..512`, default auto)
- `--enable-hash-memcache`: enable in-memory hash cache on server; reuses hash when `path + mtime + size` match
- `--once`: OneShot server mode — serve exactly one real session then exit (server-only; mutually exclusive with `--enable-hash-memcache` and `--once-multi`)
- `--once-multi`: multi-client OneShot mode — serve any number of sessions, then exit once all sessions have drained and no new connection arrives within the idle-grace window (server-only; mutually exclusive with `--once`; **compatible** with `--enable-hash-memcache`, which is useful across clients)
- `--once-idle-grace`: idle-grace window for `--once-multi` (default `5s`, suffixes `s`/`m`/`h`); valid only together with `--once-multi`
- `--wait-connect-timeout`: first-connect wait window for `--once` / `--once-multi` (default `300s`, suffixes `s`/`m`/`h`, must be `> 0`); if no valid client connection is established before it elapses, the server exits with code `6`. Once the first valid connection arrives the timer is permanently disabled. Valid only together with `--once` or `--once-multi`
- `--password`: pre-shared password (required)

At startup the server prints its configuration banner followed by the list of advertised endpoints (`[mp]   <ip>:<port>` per line, one per local NIC address) so the operator can copy one to pass to the client. If the requested port is already in use by another process, the server prints a clear `cannot listen on port ...` error and exits with code `7` instead of silently appearing to listen.



### Client

```bash
FastClone client --server <host[:port]>[,host:port...] --target <path> --password <pwd> [--streams <n>] [--chunk-kb <n>] [--queued-file-size <size>] [--large-file-threshold <size>] [--aux-weight <float>] [--large-file-lane <primary|aux|auto>] [--delta-min-size <size>] [--tcp-send-buffer <size>] [--tcp-recv-buffer <size>] [--link <localIP|iface>=<serverIP[:port]>]... [--reconnect-retries <n>] [--reconnect-window <duration>]
```

- `--server`: accepts `10.0.0.8:27842` or `10.0.0.8` (default port `27842` if omitted); accepts a comma-separated list and/or may be repeated to supply multiple multipath endpoints
- `--target`: local target directory
- `--password`: password (must match server)
- `--streams`: concurrent stream count; auto-tuned when omitted (defaults to `4`; on high-RTT links the auto value is lifted along an RTT ladder — `4` on LAN/同城, then `8`/`12`/`16` as RTT grows, capped by CPU concurrency — to fight RTT-bound throughput on massive small-file sets; explicit values above `8` print a reliability warning)
- `--chunk-kb`: chunk size in KB; auto-tuned when omitted, valid range `1..65536`
- `--queued-file-size`: soft receive-queue memory target for adaptive throttling (default: `5G`, range: `256M..64G`, supports suffixes `K/M/G`)
- `--large-file-threshold`: pins files `>=` this size to the primary link; default `1G`, range `1M..1T`, suffixes `K/M/G` (independent of the small-file batch threshold and the receive-queue target)
- `--aux-weight`: per-link ordering weight applied to every auxiliary link in transfer scheduling; default `1.0`, range `(0,16]` (the primary link is fixed at `1.0`). Higher values pull proportionally more file transfers onto aux links
- `--large-file-lane`: how large files (`>= --large-file-threshold`) are routed across links: `primary` (pin to the primary link), `aux` (schedule by weight like any other file), or `auto` (prefer aux when `--aux-weight >= 2.0`, otherwise pin to primary); default `auto`
- `--delta-min-size`: enable **binary-delta** transfer for changed files `>=` this size — only the changed byte ranges are downloaded instead of the whole file, matched against the existing local copy (Inspired by rsync. Rolling checksum + XXH3-128, independent MIT implementation). Default `0` (**disabled**); a positive value (range `1M..1T`, suffixes `K/M/G`) enables it. Independent of `--large-file-threshold`. Requires protocol FC7 on both ends (see Binary Delta Transfer below)
- `--tcp-send-buffer` / `--tcp-recv-buffer`: pin `SO_SNDBUF` / `SO_RCVBUF` in bytes (range `64K..1G`, suffixes `K/M/G`); default `0` = let the kernel autotune the window. Leaving these at `0` is recommended on high-RTT links: the kernel's receive-window auto-tuning (Linux `tcp_moderate_rcvbuf`, Windows Receive Window Auto-Tuning) scales the window to the bandwidth-delay product so a single connection can fill a high-BDP link. Pinning a value **disables** auto-tuning for that direction and fixes the window. **Windows caveat:** if receive-window auto-tuning has been disabled system-wide (some "optimizer" tools or group policy set it to `disabled`; check with `netsh interface tcp show global`), the default `0` falls back to the small Windows system default (~64KB) and will throttle high-RTT throughput. On such machines, set `--tcp-recv-buffer` explicitly (e.g. `--tcp-recv-buffer 32M`) to restore a large fixed window, or re-enable auto-tuning with `netsh int tcp set global autotuninglevel=normal`
- `--link`: explicit `<localIP|iface>=<serverIP[:port]>` pairing (repeatable); bypasses automatic selection, and the first `--link` is the primary link
- `--reconnect-retries`: max session reconnect attempts on transient drops (default `10`, `0` disables)
- `--reconnect-window`: total reconnect time window (default `30m`, suffixes `s`/`m`/`h`)



## FastCheck: Read-Only Comparison

`FastCheck` is a **separate, standalone binary** that connects to a running FastClone server and compares a local directory against the server's manifest. It only enumerates and compares — it never transfers, deletes, renames, or writes to the target directory (the only write is the optional `--output` report file). Use it for pre-sync dry-runs, periodic backup audits, or read-only auditing.

```bash
FastCheck --server <host[:port]> --target <path> --password <pwd> [--mode fast|strict|size-only] [--checkers <n>] [--output <file>] [--format text|json] [--summary-only] [--filter DIFF,MISSING,EXTRA,SAME] [--port <n>]
```

- `--server`: server endpoint (`host` or `host:port`; default port `27842`)
- `--target`: local directory to compare (read-only)
- `--password`: pre-shared password (must match server)
- `--mode`: compare strategy (default `fast`)
- `--checkers`: max in-flight concurrent hash requests on the single connection (positive integer, default `8`). Replaces `--streams` from the sync client — FastCheck has no transfer streams
- `--output`: write the full report to a file (default: terminal only). When set, the terminal gets only the summary line and the file gets the full report in the chosen format
- `--format`: `text` (default) or `json`
- `--summary-only`: emit only the final summary, no per-file lines
- `--filter`: comma-separated subset of `DIFF,MISSING,EXTRA,SAME` to list (default `DIFF,MISSING,EXTRA`; `SAME` is opt-in for debugging)
- `--port`: default port for `--server` without one (default `27842`)

### Compare Modes

| Mode | Uses mtime | Uses hash | Speed | Accuracy |
|------|-----------|----------|------|--------|
| `size-only` | no | no | fastest | lowest |
| `fast` (default) | yes | only on conflict (same size, mtime differs) | fast | high |
| `strict` | ignored | always (every same-size file) | slow | highest |

`fast` is identical to the sync client's comparison stage: same size + mtime within 2ms tolerance → `SAME`; same size but mtime differs → fall back to `XXH3_128` hash. `strict` ignores mtime and hashes every same-size file (for FAT32 / cross-timezone / mtime-mangled backups). `size-only` skips both mtime and hashing.

### Output

Terminal progress (one line): `same=<n> diff=<n> missing=<n> extra_local=<n> total=<n> mode=<mode>`. Final summary is always printed; per-file lines are gated by `--summary-only` / `--filter`.

Per-file categories:

- `DIFF` — exists on both sides, content/size differs (line includes `local=<bytes> remote=<bytes>`)
- `MISSING` — exists on server, missing locally
- `EXTRA` — exists locally, not on server (mirror semantics: would be deleted by a sync)
- `SAME` — only listed with `--filter SAME`

Paths in the report are relative to the root, with forward slashes.

### Exit Codes

- `0`: comparison complete, both sides identical (`diff=0, missing=0, extra=0`)
- `1`: comparison complete, differences found
- `2`: connection / handshake / authentication failure, or mid-compare disconnect, or argument error
- `3`: local `--target` / `--output` precondition failed (missing dir, non-existent `--output` parent), or local file read failure during hashing — raised before any TCP connection for preconditions
- `4`: interrupted by Ctrl+C; a `[PARTIAL]` report is emitted with whatever was collected

### Protocol & Compatibility

FastCheck introduces a new `CheckAuth` message (FC7 additive — no protocol version bump). The server marks the session as `Check`, serves the manifest and hash requests as usual, but **skips the transfer phase** and treats the client's close as a clean exit (no reconnect noise, no `--once` budget consumption). Existing sync clients and older servers are unaffected: an old server cleanly rejects a `CheckAuth` claim, and FastCheck reports a clear connection error.

### Build

FastCheck is part of the Visual Studio solution (`FastClone.slnx` — the `FastCheck` project builds by default; `FastCheckTests` is opt-in) and the CMake build (`add_executable(FastCheck ...)` / `add_executable(FastCheckTests ...)`). It links only the self-contained shared units (handshake, protocol, codec, sockets, file index, hash, and the shared `compare_phase` comparison logic) — not the transfer engine, delta, or disk-IO driver.



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

## Multi-Client OneShot Mode (`--once-multi`)

`--once-multi` is a sibling of `--once` for CI/CD jobs where several clients must be served from one ephemeral server: the server accepts **any number of sessions** and exits once they have all finished and the listener has been idle for a grace window.

**Behavior:**

- Sessions are served concurrently/sequentially. A session may span multiple multipath connections (lanes); the **session**, not the connection, is the unit of accounting.
- Once at least one real session has been served, the server exits when the active-session count reaches `0` **and** no new (handshake-completed) connection arrives within the idle-grace window (`--once-idle-grace`, default `5s`). A new session arriving during the window cancels/resets the timer.
- Pre-handshake probes do not count as sessions and do not reset the grace window.
- If no client ever connects, the server self-exits with code `6` once `--wait-connect-timeout` (default `300s`) elapses; see [First-Connect Wait Timeout](#first-connect-wait-timeout---wait-connect-timeout).
- Exit code aggregates across all served sessions: `0` if every session completed cleanly, `5` if any session failed or was aborted.
- Unlike `--once`, `--once-multi` is **compatible with** `--enable-hash-memcache` — a shared hash cache benefits later clients in the same run.
- Mutually exclusive with `--once`.



## First-Connect Wait Timeout (`--wait-connect-timeout`)

For `--once` and `--once-multi`, the server arms a **first-connect wait timer** at startup (default `300s`, override with `--wait-connect-timeout <duration>`, must be `> 0`). It bounds CI/CD resource usage when a one-shot server is started but no client ever connects.

**Behavior:**

- The timer covers only the interval from entering the listen state up to the **first valid connection** (an application-layer handshake completing / a valid client request). A bare TCP probe that never handshakes does not count.
- If the window elapses with no valid connection, the server logs the threshold and exits with code `6`.
- Once the first valid connection arrives, the timer is **permanently disabled** for the life of the process; subsequent exit is governed solely by the existing `--once` (single-session terminal) or `--once-multi` (`--once-idle-grace`) rules.
- Valid only together with `--once` or `--once-multi`; passing it in resident-server or client mode is a CLI error (exit `1`).



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
- **File-to-link scheduling**: a full-file transfer is not split across links. Regular files and small-file batches are distributed across healthy links by a weighted shortest-queue policy (the link with the fewest outstanding streams wins; `--aux-weight` biases the choice toward aux links). Large files (`>= --large-file-threshold`, default `1G`) follow `--large-file-lane`: pinned to the primary link by default (assumed best), or scheduled by weight like other files when aux is preferred. (Binary-delta is the exception — a delta'd file's changed ranges *are* spread across links; see below.)
- With a single NIC / endpoint it degrades to a single connection, identical to prior behavior.



#### Binary Delta Transfer (opt-in)

For large files that already exist locally and changed only partially, FastClone can transfer **only the changed byte ranges** instead of the whole file:

- Enable with `--delta-min-size <size>` (default `0` = disabled); a changed file `>=` this size uses delta, everything else is unaffected.
- The client matches the server's new file against its local old copy with an rsync-style rolling checksum + XXH3-128 block hashes (independent implementation — no rsync code, MIT-clean), downloads only the missing ranges (across the multipath links), and reconstructs locally. The result is XXH3-128 verified against the server; a hash mismatch or a low-benefit match (would download most of the file anyway) falls back to a full transfer automatically.
- **Protocol FC7**: delta requires protocol version FC7. FC7 and the older FC6 do **not** interoperate, so client and server must be upgraded together.
- Disabled by default: it trades local disk reads + CPU for fewer network bytes — a net win on bandwidth-constrained / WAN links, but often not worth it on high-bandwidth LANs.
- Large-file delta is fully **streamed** (no whole-file buffering) and uses **unbuffered direct I/O** (`FILE_FLAG_NO_BUFFERING` on Windows, `O_DIRECT` on Linux, `F_NOCACHE` on macOS) for both server-side signature generation and the client-side old-file scan. Delta therefore works on multi-GB files without inflating memory, and bulk reads/writes bypass the OS file cache (robocopy-style) for predictable throughput. Small files and sub-sector tails fall back to buffered I/O. Server-side file-read concurrency is capped to protect disk IOPS, and the unified async disk-IO driver overlaps IO with hashing.



#### How to Enable / Disable Reachability Probing

**Server side** — endpoint advertisement is always on. When a client connects for the first time, the server automatically enumerates all local NIC addresses, groups them by physical interface, and sends them in the `AuthOk` frame. There is currently no CLI option to suppress advertisement; to prevent clients from probing extra endpoints, restrict reachability at the network/firewall level so that only one NIC is reachable from the client.

**Client side** — reachability probing is controlled by whether `--link` is specified:


| Mode                            | Condition                                                                                 | Behavior                                                                                                   |
| ------------------------------- | ----------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------- |
| Auto (probing enabled)          | No `--link` given, and server advertises > 1 endpoint **or** client has > 1 NIC candidate | Client enumerates local NICs, probes reachability to all server endpoints, then selects optimal link pairs |
| Auto-degraded (probing skipped) | No `--link` given, but only 1 server endpoint + 1 local candidate                         | Probing is skipped; falls back to single-link behavior                                                     |
| Explicit (probing bypassed)     | One or more `--link` given                                                                | Client uses the pinned link(s) directly; no probing occurs                                                 |


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

**FastCheck:**

- `0`: comparison complete, both sides identical
- `1`: comparison complete, differences found
- `2`: connection / handshake / authentication failure, mid-compare disconnect, or argument error
- `3`: local `--target` / `--output` precondition failed, or local file read failure during hashing
- `4`: interrupted by Ctrl+C (partial report emitted)

**Server** `--once` **/** `--once-multi`**:**

- `0`: the served session(s) completed cleanly (`--once`: the single session; `--once-multi`: every session)
- `1`: argument/usage error (e.g. `--once` with `--enable-hash-memcache`, `--once` together with `--once-multi`, `--once-idle-grace` without `--once-multi`, `--wait-connect-timeout` without `--once`/`--once-multi`, or any of these on the client)
- `5`: a served session failed or was aborted (any lane error; `--once-multi` aggregates: `5` if any session failed) — distinct from the client's `2` so every exit code has one unambiguous meaning
- `6`: no valid client connection was established before `--wait-connect-timeout` elapsed (server first-connect wait timeout); see [First-Connect Wait Timeout](#first-connect-wait-timeout---wait-connect-timeout)
- `7`: the server could not bind/listen on the requested port (e.g., port already in use by another process)



## Cross-Platform Build (Linux/macOS)

Dependencies:

- C++20 compiler
- CMake 3.16+
- xxhash development package
- Optional (Linux only): liburing development package — enables the io_uring disk-IO backend. Without it the build still succeeds and Linux disk IO transparently uses a bounded pread/pwrite thread pool (same results, just without io_uring). macOS and Windows never use it.

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

Optional io_uring backend (Linux only): install liburing before configuring to enable the io_uring
disk-IO backend. This is purely optional — if it is absent, CMake prints
`liburing not found; Linux disk IO uses the pread/pwrite pool` and the build proceeds with the
thread-pool fallback.

- Debian / Ubuntu

```bash
sudo apt install -y liburing-dev
```

- Fedora / RHEL / Rocky / Alma / CentOS Stream / TencentOS

```bash
sudo dnf install -y liburing-devel
```

- Arch / Manjaro

```bash
sudo pacman -S --needed liburing
```

- openSUSE

```bash
sudo zypper install -y liburing-devel
```

At configure time CMake reports which backend is active:

- `FastClone: io_uring backend enabled (liburing found)` — the io_uring backend is compiled in.
- `FastClone: liburing not found; Linux disk IO uses the pread/pwrite pool` — the thread-pool fallback is used.

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

