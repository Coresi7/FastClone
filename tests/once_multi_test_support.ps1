# once_multi_test_support.ps1 -- budget / wait / log-parse / frame-gate primitives for
# once_multi_integration.ps1.
#
# Task ci-once-multi-test-budget, design 10 section 4.1 + gate 11 section 4 (C1-C11) +
# implementation contract 12 (frozen). This rework REPLACES the rate-tick OM-4 abort gate
# (03 section 3.3 S2, disproved by 07 B-01 for fast machines) with the loopback frame-gate
# proxy: the ONLY parser implementation is the C# FcGate.Scanner below (single source of
# truth shared by the MemoryStream unit tests and the real loopback socket tests -- no
# copied or fake parsers, per 12 section 1).
#
# DEFINITIONS + shared-state initialization ONLY: no main body, no Invoke-Expression, no
# string/text eval, no process/port/file-system side effects. Dot-sourcing this file does
# NOT compile the C# type, does NOT start any thread and does NOT bind any port; the
# Add-Type compile happens only when Initialize-FcFrameGateType is called explicitly
# (by the isolated harness or by Start-FcFrameGateProxy), per 10 section 4.1 / 12 section 1.
#
# PS 5.1 compatibility: no ??, no ternary operator, no -AsHashtable, no
# Get-Content -Raw -AsByteStream. The embedded C# is C#5 (Add-Type / csc v5): no string
# interpolation, no null-conditional, no expression-bodied members.
#
# Shared state is addressed through the $script: scope from BOTH the top level and inside
# functions so that the dot-sourcing host and the isolated test harness share one instance.

# ---------------------------------------------------------------------------
# Shared state
# ---------------------------------------------------------------------------
$script:FcNowUtcProvider  = $null   # injectable clock; isolated tests fast-forward time
$script:FcSuiteDeadlineUtc = $null  # host sets this: suiteStart + SCRIPT_SUITE_DEADLINE(1320)
$script:FcCurrentCase      = $null  # current New-FcCaseBudget object (set via Enter-FcCase)
$script:FcGateTypeLoaded   = $false # Add-Type once-guard for the FcGate C# definitions

# ---------------------------------------------------------------------------
# [3.2] Clock injection (explicit; the suite/case budget math never reads Get-Date directly)
# ---------------------------------------------------------------------------

function Get-FcNowUtc {
    if ($null -ne $script:FcNowUtcProvider) { return (& $script:FcNowUtcProvider) }
    return (Get-Date).ToUniversalTime()
}

function Set-FcNowUtcProvider {
    param([scriptblock]$Provider)
    $script:FcNowUtcProvider = $Provider
}

function Reset-FcNowUtcProvider {
    $script:FcNowUtcProvider = $null
}

# ---------------------------------------------------------------------------
# [3.1] Budget primitives
# ---------------------------------------------------------------------------

function Test-FcBudgetInvariant {
    # Startup self-check: makes the budget relation (G-AC5, FR-02) an executable assertion
    # instead of a comment:
    #   (1) sum(case budgets) + FIXED_SUITE_OVERHEAD <= SCRIPT_SUITE_DEADLINE
    #   (2) SCRIPT_SUITE_DEADLINE + DIAG_RESERVE    <= CTEST_TIMEOUT
    # Throws CONFIG-BUDGET-INVARIANT (design failure code) on any violation.
    param([hashtable]$CaseBudgets, [int]$FixedOverheadSec, [int]$SuiteDeadlineSec,
          [int]$DiagReserveSec, [int]$CtestTimeoutSec)
    $sum = 0
    foreach ($v in $CaseBudgets.Values) { $sum += [int]$v }
    $detail = "sum(case)=$sum fixed=$FixedOverheadSec suite=$SuiteDeadlineSec diag=$DiagReserveSec ctest=$CtestTimeoutSec"
    if ($sum + $FixedOverheadSec -gt $SuiteDeadlineSec) {
        throw "CONFIG-BUDGET-INVARIANT: sum(case)+FIXED must be <= SCRIPT_SUITE_DEADLINE ($detail)"
    }
    if ($SuiteDeadlineSec + $DiagReserveSec -gt $CtestTimeoutSec) {
        throw "CONFIG-BUDGET-INVARIANT: SCRIPT_SUITE_DEADLINE+DIAG must be <= CTEST_TIMEOUT ($detail)"
    }
    return $true
}

function New-FcCaseBudget {
    # [05-gate C1, authoritative wording] CaseDeadlineUtc = min(caseStart + BudgetSec,
    # SCRIPT_SUITE_DEADLINE). It is NOT SuiteDeadline-DiagReserve: that literal reading of
    # design 03 section 2 would clamp late cases to an absolute 1140s and re-create the 99s
    # squeeze this task fixes. The invariant sum(case)+FIXED <= suite guarantees the suite
    # cap is never binding when cases start on schedule, so every case still receives its
    # FULL own budget (G-AC4). The deadline is fixed at case start: no extend-on-progress,
    # no dynamic renewal.
    param([string]$Name, [int]$BudgetSec, [DateTime]$SuiteDeadlineUtc)
    $start = Get-FcNowUtc
    $own = $start.AddSeconds($BudgetSec)
    $deadline = $own
    if ($SuiteDeadlineUtc -lt $deadline) { $deadline = $SuiteDeadlineUtc }
    return [pscustomobject]@{
        Name        = $Name
        StartUtc    = $start
        BudgetSec   = $BudgetSec
        DeadlineUtc = $deadline
    }
}

function Get-FcSuiteRemainingSec {
    if ($null -eq $script:FcSuiteDeadlineUtc) { return [int]::MaxValue }
    $r = ($script:FcSuiteDeadlineUtc - (Get-FcNowUtc)).TotalSeconds
    if ($r -lt 0) { $r = 0 }
    return [int][Math]::Floor($r)
}

function Get-FcCaseRemainingSec {
    # Remaining budget of the CURRENT case; falls back to the suite deadline when no case is
    # active (fixture / final-cleanup phases). All bounded waits in the host script clamp to
    # this value, which is what removes the old shared-deadline 99s clamp (G-AC4).
    param([int]$ReserveSec = 0)
    $deadline = $script:FcSuiteDeadlineUtc
    if ($null -ne $script:FcCurrentCase) { $deadline = $script:FcCurrentCase.DeadlineUtc }
    if ($null -eq $deadline) { return [int]::MaxValue }
    $r = ($deadline - (Get-FcNowUtc)).TotalSeconds - $ReserveSec
    if ($r -lt 0) { $r = 0 }
    return [int][Math]::Floor($r)
}

# ---------------------------------------------------------------------------
# [3.2] Bounded condition wait (NFR-1: no unbounded sleep carries semantics)
# ---------------------------------------------------------------------------

function Wait-FcCondition {
    # Re-evaluates $Condition every $PollMs until it returns $true or $TimeoutSec elapses.
    # Returns @{ Met = bool; ElapsedSec = int }. Uses Get-FcNowUtc so the isolated tests can
    # fast-forward the clock from inside the condition scriptblock.
    param([scriptblock]$Condition, [int]$TimeoutSec, [int]$PollMs = 250)
    $start = Get-FcNowUtc
    $deadline = $start.AddSeconds($TimeoutSec)
    while ($true) {
        $met = $false
        if (& $Condition) { $met = $true }
        $now = Get-FcNowUtc
        if ($met) {
            return [pscustomobject]@{ Met = $true;  ElapsedSec = [int][Math]::Floor(($now - $start).TotalSeconds) }
        }
        if ($now -ge $deadline) {
            return [pscustomobject]@{ Met = $false; ElapsedSec = [int][Math]::Floor(($now - $start).TotalSeconds) }
        }
        Start-Sleep -Milliseconds $PollMs
    }
}

# ---------------------------------------------------------------------------
# [3.2] Incremental log reading and parsing
# ---------------------------------------------------------------------------

function Get-FcLogNewLines {
    # Returns only the lines beyond the caller-tracked $Offset and advances it. Only COMPLETE
    # lines (terminated by a newline in the file) count: a partially-written last line (the
    # unbuffered child stderr emits one line as several writes) is NOT counted and NOT
    # consumed, so the offset never skips past a line that is still being appended -- the
    # half-line race observed as introduced-bug-fix #4 (intermittent: the poll either read
    # the finished line and matched, or consumed the half line and then saw "no new lines"
    # forever because the completed line did not increase the line count).
    # The file is opened with FileShare.ReadWrite so a child process holding the write
    # handle does not block the read.
    param([string]$Path, [ref]$Offset)
    if (-not (Test-Path -LiteralPath $Path)) { return @() }
    $text = $null
    try {
        $fs = [System.IO.File]::Open($Path, [System.IO.FileMode]::Open,
            [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
        try {
            $sr = New-Object System.IO.StreamReader($fs)
            try { $text = $sr.ReadToEnd() } finally { $sr.Dispose() }
        } finally { $fs.Dispose() }
    } catch { return @() }
    if ($null -eq $text -or $text.Length -eq 0) { return @() }
    $parts = @($text -split "`n")
    # Without a trailing newline the last element is a partial line -> drop it; with one it
    # is the empty tail of the split -> drop it too. Same slice either way.
    $completeCount = $parts.Count - 1
    if ($completeCount -le 0) { return @() }
    $lines = New-Object System.Collections.Generic.List[string]
    for ($i = 0; $i -lt $completeCount; $i++) { $lines.Add($parts[$i].TrimEnd("`r")) }
    $total = $lines.Count
    if ($total -le $Offset.Value) { return @() }
    $new = @()
    for ($i = $Offset.Value; $i -lt $total; $i++) { $new += $lines[$i] }
    $Offset.Value = $total
    return $new
}

function Get-FcLogFullText {
    # [16 B-01] Diagnostic counterpart of Get-FcLogNewLines: the COMPLETE current file
    # content as one string, opened with FileShare.ReadWrite and kept VERBATIM -- including
    # a trailing fragment that has no newline yet. The OM-4 A9 evidence block uses it after
    # the 15s window to distinguish the miss causes: pattern present in the full text but
    # never consumable as a complete line (unterminated tail) vs pattern absent entirely
    # (the server had not written it inside the window). The incremental reader alone
    # cannot show that difference.
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path)) { return $null }
    try {
        $fs = [System.IO.File]::Open($Path, [System.IO.FileMode]::Open,
            [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
        try {
            $sr = New-Object System.IO.StreamReader($fs)
            try { return $sr.ReadToEnd() } finally { $sr.Dispose() }
        } finally { $fs.Dispose() }
    } catch { return $null }
}

function Test-FcSessionAccepted {
    # [mp] conn_accept (sync_engine_server.cpp L2627) is printed only after a successful
    # handshake; probes close pre-handshake and never produce it -- the S1 "valid session"
    # signal. (Probe close is demoted to debug output, L2657-2661.) Kept for OM-4 A4.
    param([string[]]$Lines)
    foreach ($l in @($Lines)) {
        if ($null -ne $l -and $l -match '\[mp\] conn_accept') { return $true }
    }
    return $false
}

# ---------------------------------------------------------------------------
# [10 section 4.1 / 12 section 1] OM-4 frame-gate proxy: C# definitions (the ONLY parser)
# ---------------------------------------------------------------------------
# Wire format (protocol.cpp L14/L20/L52-114, no TLS/encryption in this repo -- F2/F3):
# every frame is a fixed 9-byte header `type(1) + streamId(LE32) + payloadLen(LE32)`
# followed by payloadLen bytes. FileChunk = 31 (protocol.h). The scanner stops (holds) at
# the FIRST complete `type==31 && len>0` frame after its header+payload have been fully
# written to the client and flushed (no half frame, no pre-read of the next frame, D-03 B).
#
# Abort source chain (11 section 3, C1 -- do not claim "necessarily" outside this chain):
#   hold + stop reading downlink => client TCP window closes; the server physically cannot
#   push 512MB past kWinDefaultSndBuf=16MB (win_socket.cpp L81), so no SyncDone/done/
#   conn_done clean return exists and RunSessionServer has no early-return path; the kill
#   of the client makes the gate RST the upstream socket, which the server observes as a
#   failed WSASend/recv => [mp] conn_error => sticky exit 5. The proxy never buffers the
#   fixture: its replay/pending upper bound is the 64KB copy buffer, and the fail-closed
#   scan cap is capBytes (64MB), NOT 512MB.
#
# Lifecycle (12 section 3, C8): exactly two threads (accept thread does the downlink scan
# synchronously; one uplink pump). Dispose = idempotent; set cancel -> Shutdown(Both) both
# sockets (unblocks pending Read/Write) -> Close both sockets AND the listener (BEFORE the
# joins) -> join both threads under ONE aggregate 5000ms deadline (Join(remaining), never
# 3x5000) -> on a join miss set DisposeTimedOut and log FRAME_GATE_DISPOSE_TIMEOUT
# (explicit failure; Thread.Abort is forbidden). A Start failure releases the half-built
# listener before rethrowing.

$script:FcGateCs = @'
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Threading;

namespace FcGate {
    // [36 §3.2.1] append-only lifecycle additions: Finished appended AFTER Error (existing
    // values never reordered); GateError gains NoSyncDone + ClientClosedBeforeResume at the
    // END only. UpstreamCloseMode is new (09 §1 / 32 §1 authorized): Abortive = the frozen
    // OM-4 RST chain (default everywhere), Graceful = the OM-2 half-close(FIN) completion.
    public enum GateState { Init, Listening, UpstreamConnected, Scanning, Held, ClientClosed, Aborted, Error, Finished }
    public enum GateError { None, NoListener, Upstream, BadLength, Cap, OverFixture, EofBeforeChunk, NoFileChunk, ClientWrite, DisposeTimeout, NoSyncDone, ClientClosedBeforeResume }
    public enum UpstreamCloseMode { Abortive = 0, Graceful = 1 }

    public sealed class GateEvent {
        public string Kind; public int Type; public uint StreamId;
        public long Len, Cum; public string Reason;
    }

    public sealed class Result {
        public bool Found; public int Type; public uint StreamId;
        public long Bytes; public GateError Error;
        // Supplementary counters (12 section 1 field set is the frozen minimum).
        public long Len; public long Cum; public long Frames; public byte[] OverRead;
    }

    // The ONLY frame parser. Unit tests (MemoryStream) and the real loopback FrameGate call
    // this very same static method; copying or faking this logic anywhere else is forbidden.
    public static class Scanner {
        public const long MaxSaneFramePayload = 1073741824L;   // 1 GiB (protocol.cpp L20)

        // [36 §3.1.1] The single shared 9-byte header decode (LE32 fields, protocol.cpp
        // L66-72/L83-98: type(1) + streamId(LE32) + payloadLen(LE32), plaintext, no TLS).
        // Scanner.Scan and the FrameGate uplink SyncDone observer BOTH call this method --
        // there is no second parser anywhere. Byte-equivalent with the previous inline code
        // (F11 endianness evidence unchanged).
        public static void DecodeHeader(byte[] hdr, out int type, out uint streamId, out long len) {
            if (hdr == null || hdr.Length < 9) throw new ArgumentException("DecodeHeader needs the full 9-byte header");
            type = hdr[0] & 0xFF;
            streamId = (uint)(hdr[1] | (hdr[2] << 8) | (hdr[3] << 16) | (hdr[4] << 24));
            len = (long)((uint)(hdr[5] | (hdr[6] << 8) | (hdr[7] << 16) | (hdr[8] << 24)));
        }

        public static Result Scan(Stream down, Stream up, int stopType, long capBytes, long fixtureBytes,
                                  int readTimeoutMs, byte[] copyBuf, Func<bool> clientAlive) {
            if (down == null) throw new ArgumentNullException("down");
            if (up == null) throw new ArgumentNullException("up");
            if (copyBuf == null || copyBuf.Length < 1 || copyBuf.Length > 65536)
                throw new ArgumentException("copyBuf must be a caller-owned fixed buffer of 1..65536 bytes");
            Result res = new Result();
            res.Error = GateError.None;
            res.OverRead = new byte[0];
            try { down.ReadTimeout = readTimeoutMs; } catch (Exception) { /* MemoryStream: fine */ }
            long cum = 0, frames = 0;
            List<byte> pending = new List<byte>();   // over-read bytes beyond the current frame
            byte[] hdr = new byte[9];
            bool eof = false;
            try {
                while (true) {
                    if (clientAlive != null && !clientAlive()) { res.Error = GateError.ClientWrite; break; }
                    // ---- NEED_HEADER(9): accumulate until the exact 9-byte header is complete.
                    // [16 S-03] One physical Read may cross the frame boundary by up to
                    // copyBuf.Length (<=64KB); those bytes only ever enter the internal
                    // replay buffer and are never forwarded before Resume(). "No pre-read of
                    // the next frame" (12 section 2 L39) means no CONSUMPTION/forwarding past
                    // the hold boundary, while 12 section 1 L34 explicitly allows this <=64KB
                    // over-read buffer; pending stays bounded < copyBuf.Length (a completing
                    // read can only add what the current frame still needed, minus one byte).
                    while (pending.Count < 9 && !eof) {
                        int n = down.Read(copyBuf, 0, copyBuf.Length);
                        if (n <= 0) { eof = true; break; }
                        for (int i = 0; i < n; i++) pending.Add(copyBuf[i]);
                    }
                    if (pending.Count < 9) { res.Error = GateError.EofBeforeChunk; break; }
                    for (int i = 0; i < 9; i++) hdr[i] = pending[i];
                    pending.RemoveRange(0, 9);
                    int type; uint streamId; long len;
                    DecodeHeader(hdr, out type, out streamId, out len);
                    // Fail-closed bounds BEFORE forwarding any byte of this frame.
                    if (len > MaxSaneFramePayload) { res.Error = GateError.BadLength; break; }
                    if (cum + 9 + len > fixtureBytes) { res.Error = GateError.OverFixture; break; }
                    // Forward the header, then the payload. Never allocate by len: the fixed
                    // caller-owned copyBuf carries every byte (C4: no new byte[len]).
                    try { up.Write(hdr, 0, 9); } catch (Exception) { res.Error = GateError.ClientWrite; break; }
                    cum += 9;
                    long remaining = len;
                    bool frameAborted = false;
                    while (remaining > 0) {
                        if (clientAlive != null && !clientAlive()) { res.Error = GateError.ClientWrite; frameAborted = true; break; }
                        if (pending.Count > 0) {
                            int take = (int)Math.Min(remaining, (long)pending.Count);
                            byte[] seg = new byte[take];
                            for (int i = 0; i < take; i++) seg[i] = pending[i];
                            pending.RemoveRange(0, take);
                            try { up.Write(seg, 0, take); } catch (Exception) { res.Error = GateError.ClientWrite; frameAborted = true; break; }
                            cum += take; remaining -= take;
                        } else {
                            int want = (int)Math.Min(remaining, (long)copyBuf.Length);
                            int n = down.Read(copyBuf, 0, want);
                            if (n <= 0) { eof = true; res.Error = GateError.EofBeforeChunk; frameAborted = true; break; }
                            try { up.Write(copyBuf, 0, n); } catch (Exception) { res.Error = GateError.ClientWrite; frameAborted = true; break; }
                            cum += n; remaining -= n;
                        }
                    }
                    if (frameAborted) break;
                    frames++;
                    // Frame boundary reached. [16 B-06] The fail-closed cap check runs at
                    // EVERY frame boundary BEFORE the stop-frame Found return: a first stop
                    // frame whose cum already exceeds capBytes must be Cap, never Found
                    // (12 section 2 L41; the found branch previously skipped this check).
                    if (cum > capBytes) { res.Error = GateError.Cap; break; }
                    // Stop ONLY on a complete first payload>0 stop-frame.
                    if (type == stopType && len > 0) {
                        try { up.Flush(); } catch (Exception) { res.Error = GateError.ClientWrite; break; }
                        res.Found = true; res.Type = type; res.StreamId = streamId;
                        res.Len = len; res.Cum = cum; res.Frames = frames; res.Bytes = cum;
                        res.OverRead = pending.ToArray();   // replayed by Resume(), never forwarded here
                        return res;
                    }
                    // len==0 stop-frames only count; unknown/32/35 pass through untouched.
                    if (eof) { res.Error = GateError.EofBeforeChunk; break; }
                }
            } catch (IOException) {
                // Downstream read timeout / reset before any FileChunk was found.
                if (res.Error == GateError.None) res.Error = GateError.NoFileChunk;
            } catch (ObjectDisposedException) {
                if (res.Error == GateError.None) res.Error = GateError.NoFileChunk;
            }
            res.Bytes = cum; res.Cum = cum; res.Frames = frames;
            res.OverRead = pending.ToArray();
            return res;
        }

        // Transparent continuation from the hold point: replay the over-read bytes first,
        // then pump every remaining downstream byte through the fixed copy buffer. Used by
        // the byte-equivalence unit tests and FrameGate.Resume(); the OM-4 gate NEVER calls it.
        public static long Resume(Stream down, Stream up, byte[] overRead, byte[] copyBuf) {
            if (copyBuf == null || copyBuf.Length < 1 || copyBuf.Length > 65536)
                throw new ArgumentException("copyBuf must be a caller-owned fixed buffer of 1..65536 bytes");
            long total = 0;
            if (overRead != null && overRead.Length > 0) { up.Write(overRead, 0, overRead.Length); total += overRead.Length; }
            while (true) {
                int n = down.Read(copyBuf, 0, copyBuf.Length);
                if (n <= 0) break;
                up.Write(copyBuf, 0, n);
                total += n;
            }
            up.Flush();
            return total;
        }

        // [36 §3.1.2] Outcome of ResumeDrain. Total = bytes successfully written to the
        // client-side stream; TailDiscarded = bytes dropped after the client was observed
        // gone (write-failure block + every later block read to EOF); Eof = the ONLY
        // completion signal (down.Read<=0); ClientWriteFailed = a gate->client write failed
        // while clientGone()==true (the sole swallowable failure, 36 section 3.2.4).
        public sealed class ResumeOutcome {
            public long Total;
            public long TailDiscarded;
            public bool Eof;
            public bool ClientWriteFailed;
        }

        // [36 §3.1.2] Graceful continuation: identical to the 4-arg Resume when clientGone
        // is null (any up.Write failure throws -- legacy semantics preserved verbatim);
        // with a clientGone observer, a gate->client write/flush failure is swallowed ONLY
        // when the client is really gone (FIN observed), switching to discard mode which
        // keeps reading the SAME fixed 64KB copyBuf to EOF and counts TailDiscarded. A
        // clientGone()==false write failure rethrows (real client/upstream error). Eof is
        // set ONLY by down.Read<=0; read timeouts/exceptions always propagate (never a
        // completion). Zero new byte[len] allocations anywhere; buffer bound stays 64KB.
        // [39 S-01] Layering note: this observer only answers client LIVENESS; the
        // _syncDoneForwarded leg of the "Graceful + forwarded + clientGone" swallow rule
        // (36 section 3.2.4) is enforced by the CALLER -- ClassifyGracefulDrain re-checks
        // _syncDoneForwarded and turns a missing SyncDone into NoSyncDone (fail-closed),
        // so a swallowed tail write can never fabricate a Finished verdict.
        public static ResumeOutcome ResumeDrain(Stream down, Stream up, byte[] overRead, byte[] copyBuf, Func<bool> clientGone) {
            if (copyBuf == null || copyBuf.Length < 1 || copyBuf.Length > 65536)
                throw new ArgumentException("copyBuf must be a caller-owned fixed buffer of 1..65536 bytes");
            ResumeOutcome res = new ResumeOutcome();
            long total = 0;
            bool discard = false;
            if (overRead != null && overRead.Length > 0) {
                try {
                    up.Write(overRead, 0, overRead.Length);
                    total += overRead.Length;
                } catch (Exception) {
                    if (clientGone == null || !clientGone()) throw;
                    res.ClientWriteFailed = true; discard = true;
                    res.TailDiscarded += overRead.Length;
                }
            }
            while (true) {
                int n = down.Read(copyBuf, 0, copyBuf.Length);
                if (n <= 0) { res.Eof = true; break; }
                if (!discard) {
                    try {
                        up.Write(copyBuf, 0, n);
                        total += n;
                    } catch (Exception) {
                        if (clientGone == null || !clientGone()) throw;
                        res.ClientWriteFailed = true; discard = true;
                        res.TailDiscarded += n;
                    }
                } else {
                    res.TailDiscarded += n;
                }
            }
            if (!discard) {
                try { up.Flush(); } catch (Exception) {
                    if (clientGone == null || !clientGone()) throw;
                    res.ClientWriteFailed = true;
                }
            }
            res.Total = total;
            return res;
        }
    }

    public sealed class FrameGate : IDisposable {
        private readonly string _upHost; private readonly int _upPort;
        private readonly int _stopType; private readonly long _capBytes, _fixtureBytes;
        private readonly int _readTimeoutMs; private readonly string _logPath;
        private readonly object _sync = new object();
        private TcpListener _listener;
        private StreamWriter _log;
        private Thread _acceptThread, _uplinkThread;
        private TcpClient _client, _upstream;
        private volatile GateState _state = GateState.Init;
        private int _acceptedConnections = 0;
        private bool _disposed = false;
        private volatile bool _cancel = false;
        private volatile bool _resumed = false;
        private volatile bool _clientClosedSeen = false;
        private readonly ManualResetEventSlim _exitSignal = new ManualResetEventSlim(false);
        private readonly List<GateEvent> _events = new List<GateEvent>();
        private long _downBytes = 0, _upBytes = 0, _frames = 0;
        private int _port = 0;
        private bool _disposeTimedOut = false;
        private string _lastError = "";
        // [36 §3.1.3/§3.2] Graceful lifecycle state: close mode (fixed at Start), the
        // uplink SyncDone observer (9B + counters only, zero payload copies) and the
        // downlink tail bookkeeping for CLIENT_WRITE_AFTER_CLOSE.
        private readonly UpstreamCloseMode _closeMode = UpstreamCloseMode.Abortive;
        private volatile bool _syncDoneForwarded = false;
        // [59 C2] The ONE lock covering the SyncDone flag: the completing segment's
        // Write+Flush and the flag publication happen in the SAME _syncDoneLock critical
        // section, and every cross-thread reader goes through the SyncDoneForwarded()
        // accessor (same lock). A reader that observes false therefore provably did so
        // BEFORE the SyncDone's last byte was flushed, so an EOF it classifies can never
        // be the FIN that this SyncDone caused (the old post-Write observer race produced
        // false NoSyncDone verdicts). Bulk block bytes NEVER enter this lock (<=9B only).
        private readonly object _syncDoneLock = new object();
        private byte[] _obsHdr = new byte[9];
        private int _obsNeed = 9;      // header bytes still required for the current frame
        private long _obsSkip = 0;     // payload bytes still to be passed for the current frame
        private long _tailDiscarded = 0;

        // Aggregate dispose budget (ms): ONE deadline shared by all Join(remaining) calls
        // (C8: never 3x5000). Writable so the isolated tests can shrink it.
        public int DisposeTimeoutMs { get; set; }
        // Test-only seam (default 0 = disabled): delays both worker threads on exit so the
        // bounded-dispose path can be exercised deterministically. Product paths never set it.
        public static int TestHookThreadExitDelayMs = 0;

        private FrameGate(string upHost, int upPort, int stopType, long capBytes, long fixtureBytes,
                          int readTimeoutMs, string logPath, UpstreamCloseMode closeMode) {
            _upHost = upHost; _upPort = upPort; _stopType = stopType;
            _capBytes = capBytes; _fixtureBytes = fixtureBytes;
            _readTimeoutMs = readTimeoutMs; _logPath = logPath;
            _closeMode = closeMode;
            DisposeTimeoutMs = 5000;
        }

        // Start = bind+listen on 127.0.0.1:0 (OS-assigned port), resolve the REAL local port,
        // write FRAME_GATE_READY, spawn the single accept thread, and only then return.
        // Readiness is exactly this; there is NO TCP probe / self-connect anywhere (C2).
        // Any failure stops and disposes the half-built listener before rethrowing.
        // [36 §3.2.1] The legacy 7-arg Start is kept VERBATIM in behavior: it delegates to
        // the 8-arg overload with the default Abortive mode, so every existing caller
        // (OM-4 A1, the G group, I1) compiles and behaves exactly as before.
        public static FrameGate Start(string upHost, int upPort, int stopType, long capBytes, long fixtureBytes,
                                      int readTimeoutMs, string logPath) {
            return Start(upHost, upPort, stopType, capBytes, fixtureBytes, readTimeoutMs, logPath,
                         UpstreamCloseMode.Abortive);
        }

        public static FrameGate Start(string upHost, int upPort, int stopType, long capBytes, long fixtureBytes,
                                      int readTimeoutMs, string logPath, UpstreamCloseMode closeMode) {
            if (!IPAddress.Parse(upHost).Equals(IPAddress.Loopback))
                throw new ArgumentException("FrameGate is loopback-only (127.0.0.1), got: " + upHost);
            FrameGate g = new FrameGate(upHost, upPort, stopType, capBytes, fixtureBytes, readTimeoutMs, logPath, closeMode);
            try {
                g._listener = new TcpListener(IPAddress.Loopback, 0);   // port 0 -> OS picks a free port
                g._listener.Start();
                g._port = ((IPEndPoint)g._listener.LocalEndpoint).Port;
                g._log = new StreamWriter(logPath, true);
                g._log.AutoFlush = true;
                g.LogLine("FRAME_GATE_READY port=" + g._port + " upstream=" + upHost + ":" + upPort +
                          " stopType=" + stopType + " cap=" + capBytes + " fixture=" + fixtureBytes);
                g._state = GateState.Listening;
                g._acceptThread = new Thread(g.AcceptLoop);
                g._acceptThread.IsBackground = true;
                g._acceptThread.Start();
                return g;
            } catch (Exception) {
                g.DisposeInternal();
                throw;
            }
        }

        public int Port { get { lock (_sync) { return _port; } } }
        public GateState State { get { return _state; } }
        public int AcceptedConnections { get { return _acceptedConnections; } }
        public bool DisposeTimedOut { get { return _disposeTimedOut; } }
        // [36 §3.2.1] Fixed at Start; Abortive is the default for every pre-existing caller.
        public UpstreamCloseMode CloseMode { get { return _closeMode; } }

        // [36 §3.2.3] Direct client-liveness observation (FIN or RST both count as gone):
        // the uplink thread's _clientClosedSeen flag, or a zero-timeout Poll that sees the
        // socket readable with no buffered bytes (the FIN has arrived but the uplink thread
        // has not read it yet) -- no dependence on thread timing. Any Poll/Available error
        // means the socket is dead: gone.
        private bool ClientGone() {
            if (_clientClosedSeen) return true;
            try {
                Socket s = _client.Client;
                return s.Poll(0, SelectMode.SelectRead) && s.Available == 0;
            } catch (Exception) {
                return true;
            }
        }

        // [36 §3.1.3 / 59 C2] Uplink SyncDone observer, step 1 (PRE-write): advance the
        // SAME cross-block state machine (_obsNeed/_obsHdr/_obsSkip, single uplink-thread
        // owner, the one Scanner.DecodeHeader) over the block and locate the FIRST
        // complete type==40 && len==0 (SyncDone) frame. segStart/segEnd delimit the
        // bytes of THAT frame that lie in THIS block (segEnd = one past the frame's last
        // byte); a frame split across reads reports its completing tail segment
        // (segStart=0 when the 9-byte header began in an earlier block). Memory bound
        // stays 9 bytes + counters; zero payload copies. Returns false when no SyncDone
        // completes in this block -- the caller then keeps the original single
        // Write+Flush (hot path: no new lock). The state machine advances here, but the
        // FLAG is only published by PublishSyncDone after the completing segment's
        // Write+Flush succeeded inside _syncDoneLock.
        private bool ProbeSyncDone(byte[] buf, int n, out int segStart, out int segEnd, out uint sid) {
            segStart = 0; segEnd = 0; sid = 0;
            bool found = false;
            int pos = 0;
            // -1 = no header in progress inside this block; otherwise the block index of
            // the current header's first byte within this block.
            int hdrStartInBlock = (_obsNeed < 9) ? 0 : -1;
            while (pos < n) {
                if (_obsNeed > 0) {
                    if (hdrStartInBlock < 0) hdrStartInBlock = pos;
                    int take = _obsNeed;
                    if (take > n - pos) take = n - pos;
                    Array.Copy(buf, pos, _obsHdr, 9 - _obsNeed, take);
                    pos += take; _obsNeed -= take;
                    if (_obsNeed == 0) {
                        int t; uint s; long len;
                        Scanner.DecodeHeader(_obsHdr, out t, out s, out len);
                        int hdrStart = hdrStartInBlock;
                        hdrStartInBlock = -1;
                        if (len == 0) {
                            // zero-payload frame: its boundary IS the end of its 9-byte header
                            if (t == 40 && !found) {
                                sid = s;
                                segStart = hdrStart;
                                segEnd = pos;
                                found = true;
                            }
                            _obsNeed = 9;
                        } else {
                            _obsSkip = len;
                        }
                    }
                } else {
                    int take = (int)Math.Min(_obsSkip, (long)(n - pos));
                    pos += take; _obsSkip -= take;
                    if (_obsSkip == 0) { _obsNeed = 9; hdrStartInBlock = -1; }   // payload-frame boundary (len>0: cannot be the len==0 SyncDone)
                }
            }
            return found;
        }

        // [59 C2] Observer step 2: latch + log. Called ONLY with _syncDoneLock held,
        // immediately after the completing segment's Write+Flush succeeded in the SAME
        // critical section (so a false read is ordered before that write, never
        // "EOF raced ahead of the publish").
        private void PublishSyncDone(uint sid, long upbytes) {
            _syncDoneForwarded = true;
            LogEvent("sync_done_forwarded", 40, sid, 0, 0, null);
            LogLine("FRAME_GATE_SYNCDONE_FORWARDED upbytes=" + upbytes + " sid=" + sid);
        }

        // [59 C2] The ONLY sanctioned cross-thread read of the flag (all four former
        // bare _syncDoneForwarded readers route through here): same lock as the publish,
        // so the happens-before ordering is carried by the critical section itself.
        private bool SyncDoneForwarded() { lock (_syncDoneLock) { return _syncDoneForwarded; } }

        // [36 §3.2.3] Graceful terminal classification after ResumeDrain. Finished needs
        // ALL of: upstream EOF, a complete forwarded SyncDone, no cancel, and the client
        // observed gone (its FIN, or a swallowed post-close tail write). A missing
        // SyncDone at EOF is NoSyncDone (early close / server fault / half frame -- never
        // Finished). The <=2000ms bounded poll only waits for the client FIN to arrive for
        // classification; it never carries semantics and is not additive with the host's
        // 15s Finished window.
        private void ClassifyGracefulDrain(Scanner.ResumeOutcome o) {
            if (_cancel) {
                LogLine("FRAME_GATE_FINISHED_SUPPRESSED reason=cancel");
                return;
            }
            if (o.Eof) {
                if (SyncDoneForwarded()) {
                    bool gone = o.ClientWriteFailed || ClientGone();
                    if (!gone) {
                        Stopwatch sw = Stopwatch.StartNew();
                        while (sw.ElapsedMilliseconds < 2000 && !_cancel) {
                            if (ClientGone()) { gone = true; break; }
                            Thread.Sleep(25);
                        }
                    }
                    if (_cancel) { LogLine("FRAME_GATE_FINISHED_SUPPRESSED reason=cancel"); return; }
                    if (gone) {
                        SetState(GateState.Finished);
                        LogEvent("finished", 0, 0, 0, 0, null);
                        LogLine("FRAME_GATE_FINISHED sync_done_forwarded=1 eof=1 downbytes=" + _downBytes +
                                " upbytes=" + _upBytes + " tail_discarded=" + _tailDiscarded + " frames=" + _frames);
                    } else {
                        SetError(GateError.ClientWrite, "client still connected after upstream EOF (no FIN within 2000ms)");
                    }
                } else {
                    SetError(GateError.NoSyncDone, "upstream EOF before a complete SyncDone frame was forwarded");
                }
                return;
            }
            // Unreachable by construction (ResumeDrain returns only on Read<=0; timeouts and
            // resets propagate to the caller's catch). Fail closed if it is ever reached.
            SetError(GateError.Upstream, "drain ended without EOF");
        }

        public bool WaitForState(GateState s, int ms) {
            Stopwatch sw = Stopwatch.StartNew();
            while (true) {
                if (_state == s) return true;
                if (sw.ElapsedMilliseconds >= ms) return _state == s;
                Thread.Sleep(25);
            }
        }

        public string Snapshot() {
            lock (_sync) {
                StringBuilder ev = new StringBuilder();
                foreach (GateEvent e in _events) {
                    if (ev.Length > 0) ev.Append(',');
                    ev.Append(e.Kind);
                    if (e.Kind == "first_filechunk") ev.Append("(len=").Append(e.Len).Append(",cum=").Append(e.Cum).Append(')');
                }
                return "state=" + _state + " port=" + _port + " conns=" + _acceptedConnections +
                       " frames=" + _frames + " downbytes=" + _downBytes + " upbytes=" + _upBytes +
                       " error=" + _lastError + " events=" + ev.ToString();
            }
        }

        // Release the hold and transparently drain the remaining downlink (tests only).
        public void Resume() {
            _resumed = true;
            _exitSignal.Set();
        }

        public void Dispose() { DisposeInternal(); }

        private void DisposeInternal() {
            lock (_sync) {
                if (_disposed) return;   // idempotent: a second call returns immediately
                _disposed = true;
            }
            // [16 S-04] The aggregate deadline is measured FROM ENTRY so the <=5000ms budget
            // also covers the Shutdown/Close phase (12 section 3 "total <= 5000ms").
            Stopwatch sw = Stopwatch.StartNew();
            _cancel = true;
            _exitSignal.Set();
            // (1) Unblock both worker threads: Shutdown(Both) cancels blocked Read/Write.
            // [16 S-04] Snapshot both sockets under the lock so an accept racing Dispose can
            // never leave an unpublished socket unclosed (AcceptLoop closes its own side).
            TcpClient c, u;
            lock (_sync) { c = _client; u = _upstream; }
            SafeShutdownClose(c);
            SafeShutdownClose(u);
            // (2) Close the listener BEFORE joining (stops pending AcceptTcpClient).
            SafeStopListener();
            // (3) ONE aggregate deadline; every thread joins with the REMAINING budget.
            bool ok = JoinRemaining(_acceptThread, sw);
            ok = JoinRemaining(_uplinkThread, sw) && ok;
            if (!ok) {
                _disposeTimedOut = true;
                LogLine("FRAME_GATE_DISPOSE_TIMEOUT budget_ms=" + DisposeTimeoutMs);
            } else {
                LogLine("FRAME_GATE_DISPOSED conns=" + _acceptedConnections + " downbytes=" + _downBytes +
                        " upbytes=" + _upBytes);
            }
            lock (_sync) {
                if (_log != null) { try { _log.Dispose(); } catch (Exception) { } _log = null; }
            }
        }

        private bool JoinRemaining(Thread t, Stopwatch sw) {
            if (t == null) return true;
            long remaining = DisposeTimeoutMs - sw.ElapsedMilliseconds;
            if (remaining < 0) remaining = 0;
            return t.Join((int)remaining);
        }

        private static void SafeShutdownClose(TcpClient c) {
            if (c == null) return;
            try { c.Client.Shutdown(SocketShutdown.Both); } catch (Exception) { }
            try { c.Close(); } catch (Exception) { }
        }

        private void SafeStopListener() {
            lock (_sync) {
                if (_listener != null) { try { _listener.Stop(); } catch (Exception) { } _listener = null; }
            }
        }

        private void LogLine(string line) {
            lock (_sync) {
                if (_log != null) {
                    try { _log.WriteLine(line); } catch (Exception) { }
                }
            }
        }

        private void LogEvent(string kind, int type, uint streamId, long len, long cum, string reason) {
            lock (_sync) {
                _events.Add(new GateEvent { Kind = kind, Type = type, StreamId = streamId,
                                            Len = len, Cum = cum, Reason = reason });
            }
        }

        private void SetState(GateState s) { _state = s; }

        private void SetError(GateError err, string reason) {
            _state = GateState.Error;
            _lastError = err.ToString();
            LogLine("FRAME_GATE_ERROR reason=" + err + " detail=" + reason +
                    " frames=" + _frames + " downbytes=" + _downBytes + " upbytes=" + _upBytes);
            LogEvent("error", 0, 0, 0, 0, err.ToString());
        }

        private static void ThreadDelayHook() {
            int d = TestHookThreadExitDelayMs;
            if (d > 0) Thread.Sleep(d);
        }

        // Accept thread: accepts the (single expected) client connection, connects the ONE
        // upstream socket, starts the uplink pump, then runs the downlink scan synchronously
        // on THIS thread (12 section 3: exactly accept-1 + uplink-1 threads).
        private void AcceptLoop() {
            try {
                TcpClient client = _listener.AcceptTcpClient();   // throws once Dispose stops the listener
                Interlocked.Increment(ref _acceptedConnections);   // ONLY a real accept counts (C2)
                lock (_sync) { _client = client; }                 // [16 S-04] publish under the lock
                if (_cancel) {                                     // Dispose raced the accept: the
                    try { client.Close(); } catch (Exception) { }  // shutdown snapshot missed this
                    return;                                        // socket -- close it here, no events
                }
                LogEvent("accept", 0, 0, 0, 0, null);
                LogLine("FRAME_GATE_ACCEPT n=" + _acceptedConnections);
                try {
                    TcpClient up = new TcpClient();
                    lock (_sync) { _upstream = up; }               // [16 S-04] publish before Connect
                    up.NoDelay = true;
                    up.Connect(_upHost, _upPort);                  // the ONLY server connection
                } catch (Exception ex) {
                    SetError(GateError.Upstream, ex.Message);
                    return;
                }
                SetState(GateState.UpstreamConnected);
                LogLine("FRAME_GATE_UPSTREAM_CONNECTED upstream=" + _upHost + ":" + _upPort);
                _uplinkThread = new Thread(UplinkLoop);
                _uplinkThread.IsBackground = true;
                _uplinkThread.Start();
                Downlink();
            } catch (Exception ex) {
                // [16 S-04] Unified terminal-state guard (same as Downlink): never overwrite
                // ClientClosed/Aborted with Error, and never record an error during Dispose.
                if (!_cancel && !_clientClosedSeen) SetError(GateError.Upstream, ex.Message);
            } finally {
                ThreadDelayHook();
            }
        }

        private void Downlink() {
            try {
                NetworkStream down = _upstream.GetStream();        // server -> gate
                NetworkStream cli = _client.GetStream();           // gate -> client
                byte[] copyBuf = new byte[65536];
                // [16 S-05] _client.Connected is a CACHED flag from the last socket operation
                // (advisory inside the scan loop only); the authoritative client-liveness
                // assertion is the host-side A6 $c4a.Refresh()/HasExited check. Deliberately
                // kept as-is: the scan's fail-closed bounds (len/cap/fixture/timeout) do not
                // depend on this delegate being real-time.
                Func<bool> alive = delegate() { return _client.Connected; };
                FirstByteStream wrapper = new FirstByteStream(down, this);
                Result r = Scanner.Scan(wrapper, cli, _stopType, _capBytes, _fixtureBytes,
                                        _readTimeoutMs, copyBuf, alive);
                _downBytes = r.Bytes; _frames = r.Frames;
                if (r.Found) {
                    LogEvent("first_filechunk", r.Type, r.StreamId, r.Len, r.Cum, null);
                    LogLine("FRAME_GATE_FIRST_FILECHUNK type=" + r.Type + " len=" + r.Len +
                            " cum=" + r.Cum + " frames=" + r.Frames + " bytes=" + r.Bytes);
                    SetState(GateState.Held);
                    LogEvent("held", 0, 0, 0, 0, null);
                    LogLine("FRAME_GATE_HELD frames=" + r.Frames + " bytes=" + r.Bytes);
                    // Hold: wait for Resume/Dispose/client-close while watching for extra
                    // connections (bypass guard A2). No downlink byte is read past the frame
                    // boundary until Resume() replays the over-read buffer.
                    while (!_cancel && !_resumed && !_clientClosedSeen) {
                        PollExtraConnection();
                        if (_exitSignal.Wait(50)) break;
                    }
                    if (_closeMode == UpstreamCloseMode.Graceful) {
                        // [36 §3.2.3] Graceful: exactly ONE path after the hold. A client
                        // close observed while still Held (before Resume) is an error, never
                        // Finished; a Resume goes straight into ResumeDrain with the
                        // ClientGone observer and the terminal classification -- there is no
                        // second wait loop on this branch.
                        if (_clientClosedSeen && !_resumed && !_cancel) {
                            SetError(GateError.ClientClosedBeforeResume,
                                     "client closed while the gate was Held (before Resume)");
                        } else if (_resumed && !_cancel) {
                            LogLine("FRAME_GATE_RESUMED");
                            Scanner.ResumeOutcome o = Scanner.ResumeDrain(down, cli, r.OverRead, copyBuf,
                                                                          new Func<bool>(ClientGone));
                            _downBytes += o.Total;
                            _tailDiscarded = o.TailDiscarded;
                            LogLine("FRAME_GATE_DRAINED extra=" + o.Total + " downbytes=" + _downBytes +
                                    " tail_discarded=" + _tailDiscarded);
                            if (o.ClientWriteFailed) {
                                // [36 §3.2.4] The ONLY swallowable write failure: Graceful +
                                // SyncDone already forwarded + client really gone. Metadata
                                // only; no Error, no Aborted.
                                LogLine("FRAME_GATE_CLIENT_WRITE_AFTER_CLOSE n=" + o.TailDiscarded);
                            }
                            // [39 B-01] Forward the upstream (server) EOF to the client as a
                            // downstream half-close: a byte-transparent proxy must relay the
                            // server's per-connection FIN (conn_done -> closesocket, 36
                            // section 2.1) the way a direct connection would deliver it.
                            // Without this the client's blocked recv (sync_engine_client.cpp
                            // L1421-1422) only wakes on the 120s idle watchdog (L681) and the
                            // whole run stalls ~120s per client. The half-close PRECEDES the
                            // terminal classification on purpose: in the EOF-first /
                            // clientGone-second race between the two threads, forwarding the
                            // FIN here is exactly what lets the client finish its join and
                            // close, making ClientGone() true for ClassifyGracefulDrain's
                            // bounded (<=2000ms) poll -- the poll itself stays unchanged.
                            // ONLY Shutdown(Send): the uplink pump keeps reading the client
                            // (never CloseBoth -- that would truncate a client still sending).
                            // Positive conditions ONLY (Graceful + resumed branch, upstream
                            // EOF actually observed, complete SyncDone already forwarded, no
                            // cancel): every other path (early close / upstream fault /
                            // cancel / missing SyncDone) stays fail-closed and forwards
                            // nothing (unit-verified: I9a-e, I9c-e, I9d-f, I13a-e, I13b-e).
                            if (o.Eof && SyncDoneForwarded() && !_cancel) {
                                try {
                                    _client.Client.Shutdown(SocketShutdown.Send);
                                    LogLine("FRAME_GATE_DOWNSTREAM_HALF_CLOSE_SEND downbytes=" + _downBytes +
                                            " sync_done_forwarded=1 eof=1");
                                } catch (Exception) { }
                            }
                            ClassifyGracefulDrain(o);
                        }
                    } else {
                        // Abortive: the original two-phase path, verbatim semantics.
                        if (_resumed && !_cancel && !_clientClosedSeen) {
                            LogLine("FRAME_GATE_RESUMED");
                            long extra = Scanner.Resume(down, cli, r.OverRead, copyBuf);
                            _downBytes += extra;
                            LogLine("FRAME_GATE_DRAINED extra=" + extra + " downbytes=" + _downBytes);
                        }
                        while (!_cancel && !_clientClosedSeen) {
                            PollExtraConnection();
                            if (_exitSignal.Wait(50)) break;
                        }
                    }
                } else {
                    if (!_cancel) SetError(r.Error, r.Error.ToString());
                }
            } catch (Exception ex) {
                // A client that already went away (uplink observed the close and RST'd the
                // upstream, which unblocks this thread's Read with an exception) is the
                // NORMAL completion path, not a gate error: never overwrite Aborted with
                // Error here (introduced-bug fix #2).
                if (!_cancel && !_clientClosedSeen) SetError(GateError.Upstream, ex.Message);
                else if (!_cancel && _clientClosedSeen && _closeMode == UpstreamCloseMode.Graceful
                         && _state != GateState.Error && _state != GateState.Finished) {
                    // [36 §3.2.3] Graceful drain interrupted by a read exception after the
                    // client closed: classify fail-closed (a timeout/reset is NEVER EOF, so
                    // it can never be Finished).
                    if (!SyncDoneForwarded()) SetError(GateError.NoSyncDone, "drain failed before SyncDone: " + ex.Message);
                    else SetError(GateError.Upstream, "drain failed after client close: " + ex.Message);
                }
            }
        }

        private void PollExtraConnection() {
            try {
                TcpListener l = _listener;
                if (l != null && l.Pending()) {
                    TcpClient extra = l.AcceptTcpClient();
                    int n = Interlocked.Increment(ref _acceptedConnections);
                    LogEvent("extra_conn", 0, 0, 0, 0, "bypass-guard");
                    LogLine("FRAME_GATE_EXTRA_CONN n=" + n);
                    try { extra.Close(); } catch (Exception) { }
                }
            } catch (Exception) { }
        }

        // Uplink pump: client -> server, fixed 64KB buffer, fully transparent (no framing,
        // no whole-file buffering). [16 B-04] Exception classification: ONLY a real client
        // read EOF/RST (observed before any Dispose) may record ClientClosed/Aborted and RST
        // the upstream; an UPSTREAM WRITE failure surfaces as the Upstream gate error with
        // its reason preserved, and anything unblocked by Dispose (cancel already set)
        // returns with NO event and NO state change -- a Dispose shutdown must never
        // fabricate a client_closed -> upstream_aborted -> Aborted chain (12 section 2 L42).
        private void UplinkLoop() {
            long total = 0;
            bool clientClosed = false;
            string upstreamFail = null;
            try {
                NetworkStream cli = _client.GetStream();
                NetworkStream up = _upstream.GetStream();
                byte[] buf = new byte[65536];
                while (true) {
                    if (_cancel) return;                     // Dispose in flight: no events
                    int n;
                    try {
                        n = cli.Read(buf, 0, buf.Length);
                    } catch (Exception) {
                        if (_cancel) return;                 // unblocked by Dispose, not a client close
                        clientClosed = true;                 // real client-side failure (RST after the owned kill)
                        break;
                    }
                    if (n <= 0) { clientClosed = true; break; }   // clean client FIN
                    // [59 C2] Probe BEFORE writing: if this block COMPLETES a SyncDone
                    // frame, the write is split into three segments -- prefix outside
                    // the lock, the <=9B completing segment + Flush + flag publication
                    // inside _syncDoneLock (ONE critical section), suffix outside. A
                    // block with no completing SyncDone keeps the original single
                    // Write+Flush (bulk bytes never enter the lock; zero new locking on
                    // the hot path). This closes the 58 C2 race where Write+Flush
                    // succeeded, the server's FIN arrived at the downlink, and only THEN
                    // the observer set the flag -> false NoSyncDone.
                    int segStart, segEnd; uint syncSid;
                    bool completesSyncDone = ProbeSyncDone(buf, n, out segStart, out segEnd, out syncSid);
                    try {
                        if (!completesSyncDone) {
                            up.Write(buf, 0, n);
                            up.Flush();
                            total += n;
                        } else {
                            if (segStart > 0) up.Write(buf, 0, segStart);
                            lock (_syncDoneLock) {
                                up.Write(buf, segStart, segEnd - segStart);
                                up.Flush();
                                total += segEnd;
                                PublishSyncDone(syncSid, total);
                            }
                            if (segEnd < n) up.Write(buf, segEnd, n - segEnd);
                            total += (n - segEnd);
                        }
                    } catch (Exception ex) {
                        if (_cancel) return;                 // unblocked by Dispose, not an upstream error
                        upstreamFail = ex.Message;           // upstream write failure: NOT a client close
                        break;
                    }
                }
            } catch (Exception) {
                if (_cancel) return;
                clientClosed = true;                         // the client stream itself failed
            }
            _upBytes = total;
            if (_cancel) return;
            if (upstreamFail != null) {
                // Upstream write failure: error state with the reason preserved; the event
                // queue stays free of fabricated client_closed/upstream_aborted entries.
                SetError(GateError.Upstream, upstreamFail);
                _exitSignal.Set();                           // release a held downlink, if any
                ThreadDelayHook();
                return;
            }
            if (!clientClosed) return;   // unreachable by construction: the loop above only
                                         // exits via the client-close or upstream-fail breaks
            // Real client close only from here (cli.Read EOF, or a client-socket exception
            // observed while _cancel was still false).
            _clientClosedSeen = true;
            SetState(GateState.ClientClosed);
            LogEvent("client_closed", 0, 0, 0, 0, null);
            LogLine("FRAME_GATE_CLIENT_CLOSED upbytes=" + total);
            if (_closeMode == UpstreamCloseMode.Graceful) {
                // [36 §3.2.2] Graceful branch: a forwarded complete SyncDone means the client
                // closed NORMALLY -> half-close the upstream send side (FIN, no LingerState,
                // no RST, no Close, no _exitSignal) so the server sees the lane ending
                // cleanly. No SyncDone forwarded => an early disconnect is an ERROR.
                if (SyncDoneForwarded()) {
                    try {
                        _upstream.Client.Shutdown(SocketShutdown.Send);
                        LogLine("FRAME_GATE_UPSTREAM_HALF_CLOSE_SEND upbytes=" + total + " sync_done=1");
                    } catch (Exception) { }
                } else {
                    SetError(GateError.NoSyncDone, "client closed before a complete SyncDone frame was forwarded");
                    _exitSignal.Set();
                }
                ThreadDelayHook();
                return;
            }
            // Abortive close of the single upstream socket: LingerOption(true,0) + Close()
            // WITHOUT a prior Shutdown (12 section 2 L42 intent; introduced-bug fix #3).
            // [16 S-02] Evidence discipline: the OBSERVED pair is full-v2-run1
            // (Shutdown-first variant) failing A9 inside its 15s window vs full-v2-run2/
            // run3 + the om4 repeats passing after the switch to the bare abortive close.
            // The exact server-side failure latency is NOT claimed from any probe --
            // rst_semantics_probe.ps1 was cancelled before producing output -- and the
            // asserted abort source chain remains the quoted 11 section 3 C1 chain above.
            try {
                Socket s = _upstream.Client;
                s.LingerState = new LingerOption(true, 0);   // RST on close, no FIN handshake
                s.Close();
            } catch (Exception) { }
            SetState(GateState.Aborted);
            LogEvent("upstream_aborted", 0, 0, 0, 0, null);
            LogLine("FRAME_GATE_UPSTREAM_ABORTED");
            _exitSignal.Set();
            ThreadDelayHook();
        }

        // Stream wrapper that flips the gate into Scanning exactly on the first downlink
        // byte (state machine 12 section 2), while delegating everything else 1:1.
        private sealed class FirstByteStream : Stream {
            private readonly Stream _inner;
            private readonly FrameGate _gate;
            private bool _first = true;
            public FirstByteStream(Stream inner, FrameGate gate) { _inner = inner; _gate = gate; }
            public override int Read(byte[] buffer, int offset, int count) {
                int n = _inner.Read(buffer, offset, count);
                if (_first && n > 0) {
                    _first = false;
                    _gate.SetState(GateState.Scanning);
                    _gate.LogLine("FRAME_GATE_SCANNING");
                }
                return n;
            }
            public override bool CanRead { get { return _inner.CanRead; } }
            public override bool CanSeek { get { return _inner.CanSeek; } }
            public override bool CanWrite { get { return _inner.CanWrite; } }
            public override long Length { get { return _inner.Length; } }
            public override long Position { get { return _inner.Position; } set { _inner.Position = value; } }
            public override void Flush() { _inner.Flush(); }
            public override long Seek(long offset, SeekOrigin origin) { return _inner.Seek(offset, origin); }
            public override void SetLength(long value) { _inner.SetLength(value); }
            public override void Write(byte[] buffer, int offset, int count) { _inner.Write(buffer, offset, count); }
            public override int ReadTimeout { get { return _inner.ReadTimeout; } set { _inner.ReadTimeout = value; } }
        }
    }
}
'@

function Initialize-FcFrameGateType {
    # Compiles the FcGate C# definitions ONCE per process. Explicit-only: dot-sourcing the
    # support module neither compiles nor starts anything (10 section 4.1 / 12 section 1).
    # The type check makes a second call a no-op even across dot-source re-loads.
    if ($script:FcGateTypeLoaded) { return }
    $already = $false
    try { $null = [FcGate.FrameGate]; $already = $true } catch { }
    if ($already) { $script:FcGateTypeLoaded = $true; return }
    # LIB/LIBPATH/INCLUDE can carry a stale VS toolset path (e.g. the non-existent atlmfc
    # entry) that makes csc fail with "Invalid search path"; isolate the compile environment
    # and restore it afterwards (precedent: tests/file_range_tcp_proxy.ps1 L88-110).
    # [19 compliance] This is a process-level save -> temporarily unset -> finally restore
    # of three environment VARIABLES only: no path of any kind is involved, no global/user
    # setting is written, and the pre-call value (including "was unset") is restored in the
    # finally block. The .NET process-scoped API is used deliberately; the Env: drive path
    # form is NOT used to route around any host-level safe-delete protection (fix #1).
    $saved = @{}
    foreach ($v in @('LIB', 'LIBPATH', 'INCLUDE')) {
        $saved[$v] = [Environment]::GetEnvironmentVariable($v)
        [Environment]::SetEnvironmentVariable($v, $null)
    }
    try {
        Add-Type -TypeDefinition $script:FcGateCs -ReferencedAssemblies @('System', 'System.Core')
    }
    finally {
        foreach ($k in @($saved.Keys)) {
            if ($null -ne $saved[$k]) {
                [Environment]::SetEnvironmentVariable($k, $saved[$k])
            }
        }
    }
    $script:FcGateTypeLoaded = $true
}

function Start-FcFrameGateProxy {
    # Starts the in-process OM-4 frame gate. Loopback-only for BOTH the bind and the
    # upstream (a 0.0.0.0 bind or a non-loopback upstream is refused). Returns the live
    # [FcGate.FrameGate] object (Port/State/AcceptedConnections/Snapshot/Resume/Dispose).
    # [36 §3.3] -CloseMode 'Abortive' (default -- OM-4 and every pre-existing caller) or
    # 'Graceful' (the OM-2 lifecycle gates). Pure forwarding only: no port probe.
    param([string]$UpstreamHost = '127.0.0.1', [int]$UpstreamPort,
          [int]$StopAfterType = 31, [long]$CapBytes = 67108864, [long]$FixtureBytes = 536870912,
          [int]$ReadTimeoutMs = 60000, [string]$LogPath, [string]$CloseMode = 'Abortive')
    if ($UpstreamHost -ne '127.0.0.1') {
        throw "Start-FcFrameGateProxy: loopback-only upstream expected (127.0.0.1), got '$UpstreamHost'"
    }
    if ($UpstreamPort -le 0 -or $UpstreamPort -gt 65535) {
        throw "Start-FcFrameGateProxy: invalid upstream port $UpstreamPort"
    }
    if ([string]::IsNullOrWhiteSpace($LogPath)) {
        throw "Start-FcFrameGateProxy: LogPath is required (gate evidence must be on disk)"
    }
    # Compile FIRST: the [FcGate.*] type literals below need the assembly loaded in a fresh
    # process (the isolated harness pre-loads it in group F, the integration host does not
    # -- the first Start-FcFrameGateProxy call IS the first compile point).
    Initialize-FcFrameGateType
    $mode = [FcGate.UpstreamCloseMode]::Abortive
    if ($CloseMode -eq 'Graceful') { $mode = [FcGate.UpstreamCloseMode]::Graceful }
    elseif ($CloseMode -ne 'Abortive') {
        throw "Start-FcFrameGateProxy: CloseMode must be 'Abortive' or 'Graceful', got '$CloseMode'"
    }
    return [FcGate.FrameGate]::Start($UpstreamHost, $UpstreamPort, $StopAfterType,
        $CapBytes, $FixtureBytes, $ReadTimeoutMs, $LogPath, $mode)
}

function Test-FcGateFinishedOrder {
    # [36 §3.3] Pure helper shared by OM-2 step10 and the I group: true only when the gate
    # log contains BOTH a FRAME_GATE_SYNCDONE_FORWARDED line and a real FRAME_GATE_FINISHED
    # line (the exact 'FRAME_GATE_FINISHED ' event, NOT FRAME_GATE_FINISHED_SUPPRESSED) and
    # the SYNCDONE_FORWARDED line comes strictly BEFORE it -- the forwarded-complete-SyncDone
    # event must precede the Finished verdict, on the same gate log.
    param([string]$LogText)
    if ([string]::IsNullOrWhiteSpace($LogText)) { return $false }
    $lines = @($LogText -split "`r?`n")
    $sdIdx = -1
    $finIdx = -1
    for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($sdIdx -lt 0 -and $lines[$i] -match 'FRAME_GATE_SYNCDONE_FORWARDED') { $sdIdx = $i }
        if ($finIdx -lt 0 -and $lines[$i] -match '^FRAME_GATE_FINISHED ') { $finIdx = $i }
    }
    if ($sdIdx -lt 0 -or $finIdx -lt 0) { return $false }
    return ($sdIdx -lt $finIdx)
}

function Test-FcOm2OverlapEvidence {
    # [33 §3.5 spec, kept as the AUXILIARY checker per 36 §3.3] Aggregating overlap-evidence
    # validator for the OM-2 barrier moment. $E keys: GateA_Held, GateB_Held, GateA_Cum,
    # GateB_Cum, Accepted_A, Accepted_B, ClientA_Alive, ClientB_Alive, SessionIds (array),
    # AuxEndA, AuxEndB, FixtureBytes. Reason codes aggregate without swallowing:
    #   NO-FILECHUNK            (a gate not Held, or its Cum <= 0 / >= FixtureBytes)
    #   SESSION-NOT-DISTINCT    (fewer than 2 distinct conn_accept sessionIds)
    #   CLIENT-FINISHED-EARLY   (either client already exited at the barrier)
    #   MULTI-LANE              (any Accepted != 1 or any AuxEnd != 0)
    # This is NEVER the only overlap evidence (the real barrier + joint Resume + the host
    # joint chain are); it re-checks the same observable values the host just collected.
    param([hashtable]$E)
    $reasons = New-Object System.Collections.Generic.List[string]
    if (-not $E.GateA_Held -or -not $E.GateB_Held -or
        [long]$E.GateA_Cum -le 0 -or [long]$E.GateB_Cum -le 0 -or
        [long]$E.GateA_Cum -ge [long]$E.FixtureBytes -or [long]$E.GateB_Cum -ge [long]$E.FixtureBytes) {
        $reasons.Add('NO-FILECHUNK')
    }
    $distinctIds = @(@($E.SessionIds) | Select-Object -Unique)
    if ($distinctIds.Count -lt 2) { $reasons.Add('SESSION-NOT-DISTINCT') }
    if (-not $E.ClientA_Alive -or -not $E.ClientB_Alive) { $reasons.Add('CLIENT-FINISHED-EARLY') }
    if ([int]$E.Accepted_A -ne 1 -or [int]$E.Accepted_B -ne 1 -or
        [int]$E.AuxEndA -ne 0 -or [int]$E.AuxEndB -ne 0) {
        $reasons.Add('MULTI-LANE')
    }
    return [pscustomobject]@{ Ok = ($reasons.Count -eq 0); Reasons = $reasons.ToArray() }
}

# ---------------------------------------------------------------------------
# [3.6] Safe temp-dir ownership guard (G-AC8)
# ---------------------------------------------------------------------------

function Test-FcSafeTempRoot {
    # Structural ownership validation for THIS run's unique temp dir: a direct child of
    # $env:TEMP whose leaf matches the fixed unique prefix fastclone-onemulti-it-<digits>.
    # Rejects empty/whitespace strings, '$' style literals, $env:TEMP itself, foreign
    # parents, wrong prefixes and non-numeric suffixes. [19 compliance] kept fully testable
    # (H1-H7); since the 19 rework it gates NO deletion anywhere -- this run deletes nothing;
    # the tree is retained and its path printed instead (retention by design).
    param([string]$Path)
    if ([string]::IsNullOrWhiteSpace($Path)) { return $false }
    if ($Path -eq '$') { return $false }
    if (-not (Test-Path -LiteralPath $Path)) { return $false }
    $full = [System.IO.Path]::GetFullPath($Path)
    $tempFull = [System.IO.Path]::GetFullPath($env:TEMP).TrimEnd([System.IO.Path]::DirectorySeparatorChar)
    if (-not $full.EndsWith([System.IO.Path]::DirectorySeparatorChar)) {
        $parent = Split-Path -Path $full -Parent
    } else {
        $parent = Split-Path -Path $full.TrimEnd([System.IO.Path]::DirectorySeparatorChar) -Parent
    }
    if (-not [string]::Equals($parent, $tempFull, [System.StringComparison]::OrdinalIgnoreCase)) { return $false }
    $leaf = Split-Path -Path $full -Leaf
    if ($leaf -notmatch '^fastclone-onemulti-it-\d+$') { return $false }
    return $true
}

function Test-FcOwnTempMarker {
    # [16 S-06] Per-run ownership proof: the caller shows the tree is its own via the marker
    # file this very run wrote into the (structurally guarded) root. The proof is the exact
    # GUID token match, so a same-named leftover directory or a foreign tree can never be
    # mistaken for this run's fixture. [19 compliance] kept fully testable (H8-H11); it no
    # longer authorizes any deletion -- no deletion call exists in this task's scripts; the
    # guard result is only reported alongside the retained path.
    param([string]$Path, [string]$Token)
    if ([string]::IsNullOrWhiteSpace($Path)) { return $false }
    if ([string]::IsNullOrWhiteSpace($Token)) { return $false }
    $marker = Join-Path $Path '.fc-it-owner'
    if (-not (Test-Path -LiteralPath $marker -PathType Leaf)) { return $false }
    try {
        $got = (Get-Content -LiteralPath $marker -TotalCount 1)
        return ("$got" -eq $Token)
    } catch { return $false }
}
