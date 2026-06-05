# FastClone

`FastClone` is a high-throughput directory sync tool built for extreme-scale trees: it rapidly syncs **tens of millions of files** and **TB-scale data**, handling a mix of huge files and enormous numbers of tiny files in a single pass (e.g. Unity/Unreal project folders, game build outputs, and large asset/dataset repositories).

Supported platforms:
- Windows: WinAPI fast path (highest performance)
- GNU/Linux / macOS: cross-platform implementation (fully functional; performance may be lower than Windows in some cases)

## Core Features

- Single executable: same binary can run as `server` or `client`
- Multi-stream transfer over a single TCP connection
- Mirror sync: files/directories deleted on server are also deleted on client
- Fallback verification: uses `XXH3_128` when `size + mtime` do not match
- Scales to tens-of-millions-file, TB-scale trees: tiny files are batched together while large files are split across multiple chunked streams
- Strict protocol version check (version mismatch is rejected immediately)

## Expected Performance

- On i9-9900K + SATA SSD, syncing a ~3 million-file Library can finish in around 20 seconds when delta changes are limited (otherwise mainly bounded by network bandwidth).

## Usage

### Server

```bash
FastClone server [--dir <path>] [--port <n>] [--server-hash-workers <n>] [--enable-hash-memcache] --password <pwd>
```

- `--dir`: server root directory (default: current directory)
- `--port`: listening port (default: `27842`)
- `--server-hash-workers`: global hash worker threads for all sessions (`0` = auto, range `0..512`, default auto)
- `--enable-hash-memcache`: enable in-memory hash cache on server; reuses hash when `path + mtime + size` match
- `--password`: pre-shared password (required)

### Client

```bash
FastClone client --server <host[:port]> --target <path> --password <pwd> [--streams <n>] [--chunk-kb <n>] [--queued-file-size <size>]
```

- `--server`: accepts `10.0.0.8:27842` or `10.0.0.8` (default port `27842` if omitted)
- `--target`: local target directory
- `--password`: password (must match server)
- `--streams`: concurrent stream count; auto-tuned when omitted (defaults to `4`, and explicit values above `8` print a reliability warning)
- `--chunk-kb`: chunk size in KB; auto-tuned when omitted, valid range `1..65536`
- `--queued-file-size`: soft receive-queue memory target for adaptive throttling (default: `5G`, range: `256M..64G`, supports suffixes `K/M/G`)

## Progress Counters

The client prints and updates these counters in one line:

- `Enumrated`: remote files enumerated
- `Compared`: files fully decided
- `Unchanged`: files that do not need transfer
- `Failed`: files still failed after retries (up to 3 attempts)
- `Transfered`: files transferred successfully
- `Deleted`: files removed during mirror-delete phase

## Notes

- Current transport is plain TCP + password; use only in trusted networks
- Mirror mode removes extra files/directories on the client
- No resume support; rerun after interruption (unchanged files are still skipped by comparison)

## Exit Codes

- `0`: sync succeeded (no failed files)
- `1`: argument error or runtime exception
- `2`: sync completed with failed files (try lowering concurrency, e.g. `--streams`)

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

