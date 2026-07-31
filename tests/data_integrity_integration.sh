#!/usr/bin/env bash
#
# FastClone data integrity integration test (GNU/Linux & macOS).
# Bash equivalent of the Windows data_integrity_integration.ps1.
#
# Verifies that bytes transferred by the server are byte-for-byte identical to
# the source (SHA256) after a full sync. This is the automated regression guard
# for the C2 "transferred bytes are byte-for-byte unchanged" guarantee that A1
# (a1-sendloop-buffer-reuse) relies on but previously had only a one-shot manual
# SHA256 check.
#
# Covers the gaps identified in the post-A1 static review:
#   DI-1  post-transfer SHA256 byte-for-byte comparison (the core C2 guarantee)
#   DI-2  empty file (0 bytes) -- skips read path entirely
#   DI-3  EOF boundary (last chunk < effectiveChunkSize) -- single-file stream path
#   DI-4  batch multi-file switch -- readBuffer reuse across files in one batch
#   DI-5  single-file stream vs batch stream paths both exercised
#         (threshold: fileSize <= 1920 KiB -> batch, > 1920 KiB -> single)
#   DI-6  small file (< effectiveChunkSize) -- single chunk + EOF
#   DI-7  multiple large files concurrent -- multiple activeStreams round-robin,
#         each with its own readBuffer; verifies no cross-stream buffer leakage
#   DI-8  batch mixed file sizes -- readBuffer grows then does not shrink; the
#         smaller trailing file's copy must exclude residual bytes
#
# Two variants are run to cover different effectiveChunkSize values:
#   variant A: default chunk size (auto-tuned to 4 MiB at streamLimit<=8)
#   variant B: --chunk-kb 256 (256 KiB) -- forces many chunks on the large file,
#              exercising the EOF tail boundary repeatedly
#
# Usage: ./data_integrity_integration.sh [-ExePath path/to/FastClone] [-Port 27894]

set -uo pipefail

EXE_PATH=""
PORT=27894

while [ $# -gt 0 ]; do
    case "$1" in
        -ExePath) EXE_PATH="$2"; shift 2;;
        -Port)    PORT="$2"; shift 2;;
        *) echo "Unknown argument: $1" >&2; exit 2;;
    esac
done

# Locate project root (parent of the tests/ dir this script lives in).
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" 2>/dev/null && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." 2>/dev/null && pwd)"

PASSWORD="fc-di-pw"

# Pick a SHA256 tool; macOS lacks sha256sum but has shasum -a 256.
if command -v sha256sum >/dev/null 2>&1; then
    SHA_TOOL="sha256sum"
elif command -v shasum >/dev/null 2>&1; then
    SHA_TOOL="shasum -a 256"
else
    echo "No SHA256 tool found (need sha256sum or shasum)" >&2
    exit 1
fi

# Resolve the FastClone binary.
resolve_exe() {
    if [ -n "$EXE_PATH" ] && [ -x "$EXE_PATH" ]; then
        echo "$EXE_PATH"; return 0
    fi
    local candidates="
${PROJECT_DIR}/build/tlinux/FastClone
${PROJECT_DIR}/build/mac_arm64/FastClone
${PROJECT_DIR}/build/mac_amd64/FastClone
${PROJECT_DIR}/build/Release/FastClone
${PROJECT_DIR}/build/FastClone"
    local c
    for c in $candidates; do
        if [ -x "$c" ]; then echo "$c"; return 0; fi
    done
    echo "FastClone binary not found (tried common build dirs); pass -ExePath" >&2
    return 1
}

EXE="$(resolve_exe)" || exit 1
echo "Using FastClone binary: $EXE"

# Temp workspace.
TMP_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/fastclone-di.XXXXXX")"
SRC="${TMP_ROOT}/src"
TGT="${TMP_ROOT}/tgt"
LOG_DIR="${TMP_ROOT}/logs"
mkdir -p "$SRC" "$TGT" "$LOG_DIR"

SERVER_PID=""
cleanup() {
    if [ -n "$SERVER_PID" ] && kill -0 "$SERVER_PID" 2>/dev/null; then
        kill "$SERVER_PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT

# Build the fixture (same shape as the Windows .ps1).
build_fixture() {
    local src="$1"
    # DI-2: empty file (0 bytes).
    : > "$src/empty.txt"
    # DI-6: tiny file (50 bytes).
    head -c 50 /dev/urandom > "$src/tiny.bin"
    # small file (100 KiB).
    head -c 102400 /dev/urandom > "$src/small_100k.bin"
    # DI-4 / DI-8: batch multi-file switch (4 files of different sizes).
    mkdir -p "$src/batchdir"
    head -c 1024   /dev/urandom > "$src/batchdir/file1.bin"
    head -c 65536  /dev/urandom > "$src/batchdir/file2.bin"
    head -c 204800 /dev/urandom > "$src/batchdir/file3.bin"
    head -c 4096   /dev/urandom > "$src/batchdir/file4.bin"
    # nested directory with a small file.
    mkdir -p "$src/nested/deep"
    printf 'leaf-payload' > "$src/nested/deep/leaf.txt"
    # DI-3 / DI-5: large file (5 MiB).
    head -c 5242880 /dev/urandom > "$src/large_5m.bin"
    # DI-7: two extra large files for concurrent single-file streams.
    head -c 5242880 /dev/urandom > "$src/large_5m_b.bin"
    head -c 5242880 /dev/urandom > "$src/large_5m_c.bin"
}

# SHA256 manifest of a directory tree (relative paths, sorted).
build_manifest() {
    local dir="$1" out="$2"
    ( cd "$dir" && find . -type f -print0 | sort -z | xargs -0 $SHA_TOOL ) > "$out"
}

# Run one sync variant and compare trees.
run_variant() {
    local label="$1"
    local extra="$2"
    echo "[$label] syncing with extra args: ${extra:-<none>}"

    rm -rf "$TGT"; mkdir -p "$TGT"

    local srv_out="$LOG_DIR/$label-server.out"
    local srv_err="$LOG_DIR/$label-server.err"
    local cli_out="$LOG_DIR/$label-client.out"
    local cli_err="$LOG_DIR/$label-client.err"

    FASTCLONE_DEBUG=1 "$EXE" server --dir "$SRC" --password "$PASSWORD" \
        --port "$PORT" --once >"$srv_out" 2>"$srv_err" &
    SERVER_PID=$!

    # Give the server a moment to bind and start listening (matches the Windows
    # .ps1 reference which uses a 1s Start-Sleep). No external tool dependency.
    sleep 1

    FASTCLONE_DEBUG=1 "$EXE" client --server "127.0.0.1:$PORT" --target "$TGT" \
        --password "$PASSWORD" $extra >"$cli_out" 2>"$cli_err"
    local cli_code=$?

    local srv_code=0
    if kill -0 "$SERVER_PID" 2>/dev/null; then
        wait "$SERVER_PID"; srv_code=$?
    fi
    SERVER_PID=""

    if [ "$cli_code" -ne 0 ]; then
        echo "FAILED: $label client exited with code $cli_code" >&2
        return 1
    fi
    if [ "$srv_code" -ne 0 ]; then
        echo "FAILED: $label server --once exited with code $srv_code" >&2
        return 1
    fi

    # DI-1: compare source/target SHA256 trees byte-for-byte.
    build_manifest "$SRC" "$LOG_DIR/$label-src.manifest"
    build_manifest "$TGT" "$LOG_DIR/$label-tgt.manifest"
    if ! diff -q "$LOG_DIR/$label-src.manifest" "$LOG_DIR/$label-tgt.manifest" >/dev/null; then
        echo "FAILED: $label DATA INTEGRITY FAILURE (file count or hash mismatch):" >&2
        diff "$LOG_DIR/$label-src.manifest" "$LOG_DIR/$label-tgt.manifest" >&2
        return 1
    fi

    local n
    n=$(wc -l < "$LOG_DIR/$label-src.manifest")
    echo "  OK $label: $n files, all SHA256 match"
    return 0
}

# --- Main --------------------------------------------------------------------
build_fixture "$SRC"

echo "Source files:"
( cd "$SRC" && find . -type f | sort )

if ! run_variant "DI-A-default" ""; then
    echo "Logs: $LOG_DIR" >&2
    exit 1
fi

if ! run_variant "DI-B-chunk256k" "--chunk-kb 256"; then
    echo "Logs: $LOG_DIR" >&2
    exit 1
fi

echo ""
echo "All data-integrity integration tests passed. Logs: $LOG_DIR"
rm -rf "$TMP_ROOT"
exit 0
