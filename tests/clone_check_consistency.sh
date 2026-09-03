#!/usr/bin/env bash
# FastClone/FastCheck consistency invariant - end-to-end test (POSIX).
# Task unify-probe-extra-shared (design §8.3, FR-13; OQ-05: this is divergence-point A's
# only end-to-end observable surface - the POSIX probe now reports Unix ns via ToUnixNs).
# Same scenarios S1..S6 as tests/clone_check_consistency.ps1. Not registered via add_test
# on Windows; invoke manually / in POSIX CI:
#   ./tests/clone_check_consistency.sh /path/to/FastClone /path/to/FastCheck [port]

set -u

EXE_HINT="${1:-}"
CHECK_HINT="${2:-}"
PORT="${3:-27941}"
PW="cc-invariant-pw"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

resolve_tool() {
    local hint="$1" name="$2" c
    if [ -n "$hint" ] && [ -x "$hint" ]; then echo "$hint"; return 0; fi
    for c in "$SCRIPT_DIR/../build/$name" "$SCRIPT_DIR/../build/Release/$name"; do
        if [ -x "$c" ]; then echo "$c"; return 0; fi
    done
    return 1
}

EXE=$(resolve_tool "$EXE_HINT" FastClone) || { echo "SKIP: FastClone not built"; exit 0; }
CHECK=$(resolve_tool "$CHECK_HINT" FastCheck) || { echo "SKIP: FastCheck not built"; exit 0; }
echo "using FastClone=$EXE FastCheck=$CHECK port=$PORT"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/fc_cc_XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

fail() { echo "FAIL: $*" >&2; exit 1; }

write_binary() { # path size
    mkdir -p "$(dirname "$1")"
    python3 - "$1" "$2" <<'EOF' || fail "python3 required to build fixtures"
import sys
path, size = sys.argv[1], int(sys.argv[2])
with open(path, "wb") as f:
    f.write(bytes((i * 7 + 13) & 0xFF for i in range(size)))
EOF
}

count_files() { find "$1" -type f | wc -l; }

start_server() { # src logtag
    "$EXE" server --dir "$1" --password "$PW" --port "$PORT" --once \
        >"$WORK/$2.out" 2>"$WORK/$2.err" &
    SRV_PID=$!
    sleep 1.5
}

wait_server() {
    wait "$SRV_PID" || fail "server exited nonzero"
}

run_check_clean() { # check_exe target mode expected_total tag
    local code
    "$1" --server "127.0.0.1:$PORT" --target "$2" --password "$PW" --mode "$3" \
        >"$WORK/$5.out" 2>"$WORK/$5.err"
    code=$?
    [ "$code" -eq 0 ] || fail "[$5] FastCheck ($3) exited $code (stdout: $(cat "$WORK/$5.out"))"
    local line
    line=$(grep -o 'same=[0-9]* diff=[0-9]* missing=[0-9]* extra_local=[0-9]* total=[0-9]*' "$WORK/$5.out" | tail -1)
    [ -n "$line" ] || fail "[$5] summary line not parseable: $(cat "$WORK/$5.out")"
    echo "$line" | grep -q "same=.*diff=0 missing=0 extra_local=0 total=$4" || \
        fail "[$5] not clean: $line (expected total=$4)"
}

sync_and_verify() { # src target expected_total tag
    start_server "$1" "$4-srv1"
    "$EXE" --server "127.0.0.1:$PORT" --target "$2" --password "$PW" \
        >"$WORK/$4-sync.out" 2>"$WORK/$4-sync.err" || fail "[$4] sync failed"
    wait_server
    start_server "$1" "$4-srv2"
    run_check_clean "$CHECK" "$2" fast "$3" "$4-fast"
    wait_server
    start_server "$1" "$4-srv3"
    if ! run_check_clean "$CHECK" "$2" strict "$3" "$4-strict"; then
        echo "[RETRY] [$4] strict failed once, retrying"
        start_server "$1" "$4-srv4"
        run_check_clean "$CHECK" "$2" strict "$3" "$4-strict2"
        wait_server
    else
        wait_server
    fi
}

# ---- S1: baseline tree ----
{
    SRC="$WORK/s1/src"; TGT="$WORK/s1/tgt"; mkdir -p "$SRC"
    write_binary "$SRC/empty.bin" 0
    write_binary "$SRC/a_50b.txt" 50
    write_binary "$SRC/b_100k.bin" $((100 * 1024))
    write_binary "$SRC/c_5m.bin" $((5 * 1024 * 1024))
    write_binary "$SRC/nested/deep/d.dat" 2048
    write_binary "$SRC/nested/中文名.txt" 7
    TOTAL=$(count_files "$SRC")
    sync_and_verify "$SRC" "$TGT" "$TOTAL" s1
    echo "S1 baseline: PASS"
}

# ---- S2: historical / boundary mtimes (Unix-ns view; 500000000 s == 5e17 ns) ----
{
    SRC="$WORK/s2/src"; TGT="$WORK/s2/tgt"; mkdir -p "$SRC"
    write_binary "$SRC/m_zero.bin" 1234;    touch -d @1 "$SRC/m_zero.bin"
    write_binary "$SRC/m_below.bin" 1234;   touch -d @499999999 "$SRC/m_below.bin"    # < 5e17 ns
    write_binary "$SRC/m_above.bin" 1234;   touch -d @500000001 "$SRC/m_above.bin"    # > 5e17 ns
    write_binary "$SRC/m_2001.bin" 1234;    touch -d "2001-01-01T00:00:00Z" "$SRC/m_2001.bin"
    write_binary "$SRC/m_future.bin" 1234;  touch -d "2099-06-01T00:00:00Z" "$SRC/m_future.bin"
    TOTAL=$(count_files "$SRC")
    sync_and_verify "$SRC" "$TGT" "$TOTAL" s2
    echo "S2 historical mtimes (divergence-point A surface): PASS"
}

# ---- S3: pre-existing extra ----
{
    SRC="$WORK/s3/src"; TGT="$WORK/s3/tgt"; mkdir -p "$SRC" "$TGT"
    write_binary "$SRC/keep.txt" 100
    write_binary "$TGT/stale_extra.bin" 55
    sync_and_verify "$SRC" "$TGT" 1 s3
    [ ! -e "$TGT/stale_extra.bin" ] || fail "[s3] stale extra must be deleted"
    echo "S3 extra pre-seeded: PASS"
}

# ---- S4: idempotent re-run ----
{
    SRC="$WORK/s4/src"; TGT="$WORK/s4/tgt"; mkdir -p "$SRC"
    write_binary "$SRC/f1.bin" 4096
    write_binary "$SRC/f2.bin" 8192
    sync_and_verify "$SRC" "$TGT" 2 s4a
    start_server "$SRC" s4b-srv
    "$EXE" --server "127.0.0.1:$PORT" --target "$TGT" --password "$PW" \
        >"$WORK/s4b-sync.out" 2>"$WORK/s4b-sync.err" || fail "[s4] second sync failed"
    wait_server
    grep -q 'transferred=0\($\|[^0-9]\)' "$WORK/s4b-sync.err" "$WORK/s4b-sync.out" 2>/dev/null || \
        fail "[s4] second sync must report transferred=0 ($(cat "$WORK/s4b-sync.err"))"
    start_server "$SRC" s4c-srv
    run_check_clean "$CHECK" "$TGT" fast 2 s4c
    wait_server
    echo "S4 idempotent re-run: PASS"
}

# ---- S5 (AC-08): FastClone client binary inside the target ----
{
    SRC="$WORK/s5/src"; TGT="$WORK/s5/tgt"; mkdir -p "$SRC" "$TGT"
    write_binary "$SRC/one.txt" 100
    cp "$EXE" "$TGT/FastClone"
    start_server "$SRC" s5-srv1
    "$EXE" --server "127.0.0.1:$PORT" --target "$TGT" --password "$PW" \
        >"$WORK/s5-sync.out" 2>"$WORK/s5-sync.err" || fail "[s5] sync must exit 0 (self-excluded)"
    wait_server
    [ -e "$TGT/FastClone" ] || fail "[s5] FastClone must not delete its own binary"
    start_server "$SRC" s5-srv2
    "$CHECK" --server "127.0.0.1:$PORT" --target "$TGT" --password "$PW" --mode fast \
        >"$WORK/s5-fast.out" 2>"$WORK/s5-fast.err"
    CODE=$?
    [ "$CODE" -eq 1 ] || fail "[s5] FastCheck must exit 1, got $CODE ($(cat "$WORK/s5-fast.out"))"
    grep -q 'extra_local=1' "$WORK/s5-fast.out" || fail "[s5] extra_local must be 1"
    grep -q 'FastClone$' "$WORK/s5-fast.out" || fail "[s5] extra detail must mention FastClone"
    wait_server
    echo "S5 client-binary-in-target: PASS (FastClone exit 0; FastCheck exit 1 extra_local=1)"
}

# ---- S6 (small change B anchor): FastCheck binary inside the target ----
{
    SRC="$WORK/s6/src"; TGT="$WORK/s6/tgt"; mkdir -p "$SRC"
    write_binary "$SRC/data.bin" 200
    start_server "$SRC" s6-srv1
    "$EXE" --server "127.0.0.1:$PORT" --target "$TGT" --password "$PW" \
        >"$WORK/s6-sync.out" 2>"$WORK/s6-sync.err" || fail "[s6] sync failed"
    wait_server
    cp "$CHECK" "$TGT/FastCheck"
    start_server "$SRC" s6-srv2
    run_check_clean "$TGT/FastCheck" "$TGT" fast 1 s6-fast
    wait_server
    start_server "$SRC" s6-srv3
    run_check_clean "$TGT/FastCheck" "$TGT" strict 1 s6-strict
    wait_server
    echo "S6 self-exclude parity: PASS"
}

echo "clone_check_consistency (POSIX): ALL SCENARIOS PASSED"
exit 0
