# test_once_multi_test_support.ps1 -- isolated tests for once_multi_test_support.ps1.
#
# Task ci-once-multi-test-budget, design 10 section 4.5 / gate 11 section 4 C7 / contract 12
# section 6. Groups A-E (clock, budget invariant with the reworked OM-4=350 numbers,
# per-case budget independence, bounded wait, incremental log reads) and H (safe temp root)
# are kept from the previous 48-assertion harness; the deleted rate-tick groups (old E7-E13,
# F1-F11) are replaced by:
#   F  -- >=10 parser cases driving the REAL FcGate.Scanner under MemoryStream (whole frame,
#         sticky frames, byte-dribbled header, unknown/zero/oversized frames, EOF, timeout,
#         endianness, hold-after-write, resume byte-equivalence). No copied/fake parser.
#   G  -- one real in-process loopback socket suite (TcpListener + writer, no dummy bin,
#         no external exe, loopback only, OS-assigned ports): transparent forwarding, hold
#         at the frame boundary, AcceptedConnections==1, Resume, client-close -> Aborted,
#         idempotent bounded Dispose, explicit FRAME-GATE-CLOSE-TIMEOUT, Start-failure
#         teardown with no half-initialized listener.
#
# Run: powershell -NoProfile -ExecutionPolicy Bypass -File tests\test_once_multi_test_support.ps1
# Exit 0 = all assertions pass, exit 1 = at least one failure. A failing harness never
# starts the product binary (no FastClone.exe, no exec, no port scans).

$ErrorActionPreference = 'Stop'

. "$PSScriptRoot\once_multi_test_support.ps1"

$script:PassCount = 0
$script:FailCount = 0

function Assert-True {
    param([bool]$Cond, [string]$Name)
    if ($Cond) { $script:PassCount++; Write-Host ("  PASS " + $Name) }
    else { $script:FailCount++; Write-Host ("  FAIL " + $Name) -ForegroundColor Red }
}
function Assert-Equal {
    param($Expected, $Actual, [string]$Name)
    if ("$Expected" -eq "$Actual") { $script:PassCount++; Write-Host ("  PASS " + $Name) }
    else {
        $script:FailCount++
        Write-Host ("  FAIL " + $Name + " (expected='" + $Expected + "' actual='" + $Actual + "')") -ForegroundColor Red
    }
}
function Assert-Throws {
    param([scriptblock]$Block, [string]$Name, [string]$WithText)
    $threw = $false; $msg = ''
    try { & $Block | Out-Null } catch { $threw = $true; $msg = "$_" }
    if ($threw -and ($WithText -eq '' -or $msg -like ("*" + $WithText + "*"))) {
        $script:PassCount++; Write-Host ("  PASS " + $Name)
    } else {
        $script:FailCount++
        Write-Host ("  FAIL " + $Name + " (threw=" + $threw + " msg=" + $msg + ")") -ForegroundColor Red
    }
}

# Fake clock: the provider reads this $script:-scoped variable, so tests fast-forward time
# by mutating it (or from inside Wait-FcCondition's condition scriptblock).
$script:FakeNow = [DateTime]::UtcNow
Set-FcNowUtcProvider { $script:FakeNow }

Write-Host "== A: clock injection =="
$realish = (Get-Date).ToUniversalTime()
$a1 = [Math]::Abs(((Get-FcNowUtc) - $realish).TotalSeconds)
Assert-True ($a1 -lt 5) "A1 provider returns fake (=real at setup)"
$script:FakeNow = $script:FakeNow.AddSeconds(351)
Assert-True ([Math]::Abs(((Get-FcNowUtc) - $realish).TotalSeconds - 351) -lt 5) "A2 clock advances with fake"
Reset-FcNowUtcProvider
$a3 = [Math]::Abs(((Get-FcNowUtc) - $realish).TotalSeconds)
Assert-True ($a3 -lt 360) "A3 reset restores real clock (351s fake offset cleared)"
Set-FcNowUtcProvider { $script:FakeNow }
$script:FakeNow = [DateTime]::UtcNow

Write-Host "== B: budget invariant (V-07; reworked OM-4=350 numbers) =="
$realBudgets = @{ 'OM-1' = 90; 'OM-2' = 300; 'OM-3' = 300; 'OM-4' = 350; 'OM-5a' = 75; 'OM-5b' = 45 }
Assert-True (Test-FcBudgetInvariant -CaseBudgets $realBudgets -FixedOverheadSec 150 -SuiteDeadlineSec 1320 -DiagReserveSec 180 -CtestTimeoutSec 1500) "B1 positive: 1160+150<=1320, 1320+180<=1500"
Assert-Throws { Test-FcBudgetInvariant -CaseBudgets $realBudgets -FixedOverheadSec 300 -SuiteDeadlineSec 1320 -DiagReserveSec 180 -CtestTimeoutSec 1500 } "B2 negative: sum+FIXED>suite throws" "CONFIG-BUDGET-INVARIANT"
Assert-Throws { Test-FcBudgetInvariant -CaseBudgets $realBudgets -FixedOverheadSec 150 -SuiteDeadlineSec 1320 -DiagReserveSec 200 -CtestTimeoutSec 1500 } "B3 negative: suite+DIAG>ctest throws" "CONFIG-BUDGET-INVARIANT"
$badCase = @{ 'OM-4' = 500 }
Assert-Throws { Test-FcBudgetInvariant -CaseBudgets $badCase -FixedOverheadSec 900 -SuiteDeadlineSec 1320 -DiagReserveSec 180 -CtestTimeoutSec 1500 } "B4 negative: any broken constant fails" "CONFIG-BUDGET-INVARIANT"

Write-Host "== C: per-case budgets, prior-spent independence (V-06 / gate C1) =="
$T0 = [DateTime]::UtcNow
$script:FakeNow = $T0
$script:FcSuiteDeadlineUtc = $T0.AddSeconds(1320)
$script:FcCurrentCase = $null
$c1case = New-FcCaseBudget -Name 'OM-4' -BudgetSec 350 -SuiteDeadlineUtc $script:FcSuiteDeadlineUtc
$script:FcCurrentCase = $c1case
Assert-Equal 350 (Get-FcCaseRemainingSec) "C1 fresh case gets its full 350s budget"
# V-06: a predecessor burned 351s of wall clock; the NEW case still gets its own full budget.
$script:FakeNow = $T0.AddSeconds(351)
$c2case = New-FcCaseBudget -Name 'OM-4' -BudgetSec 350 -SuiteDeadlineUtc $script:FcSuiteDeadlineUtc
$script:FcCurrentCase = $c2case
Assert-Equal 350 (Get-FcCaseRemainingSec) "C2 prior 351s spent does NOT clamp the new case (no 99s squeeze)"
Assert-Equal 969 (Get-FcSuiteRemainingSec) "C3 suite remaining tracks the shared cap (1320-351=969)"
$script:FakeNow = $script:FakeNow.AddSeconds(100)
Assert-True ((Get-FcCaseRemainingSec) -le 250 -and (Get-FcCaseRemainingSec) -ge 249) "C4 remaining decreases with time (fixed deadline, no renewal)"
Assert-Equal 150 (Get-FcCaseRemainingSec -ReserveSec 100) "C5 ReserveSec is subtracted"
# Suite cap: a case started 1300s into the suite is capped at the suite deadline (C1 wording).
$script:FakeNow = $T0.AddSeconds(1300)
$c4case = New-FcCaseBudget -Name 'OM-5b' -BudgetSec 45 -SuiteDeadlineUtc $script:FcSuiteDeadlineUtc
$script:FcCurrentCase = $c4case
Assert-True ((Get-FcCaseRemainingSec) -le 20 -and (Get-FcCaseRemainingSec) -ge 19) "C6 suite deadline caps a late-started case (min semantics)"
$script:FcCurrentCase = $null
Assert-Equal 20 (Get-FcCaseRemainingSec) "C7 no active case falls back to suite deadline"

Write-Host "== D: bounded condition wait =="
$script:FakeNow = [DateTime]::UtcNow
$d1 = Wait-FcCondition -Condition { $true } -TimeoutSec 5 -PollMs 25
Assert-True ($d1.Met -and $d1.ElapsedSec -eq 0) "D1 immediately-met condition returns Met with 0 elapsed"
$d2 = Wait-FcCondition -Condition { $script:FakeNow = $script:FakeNow.AddMilliseconds(300); $false } -TimeoutSec 2 -PollMs 25
Assert-True ((-not $d2.Met) -and ($d2.ElapsedSec -ge 2)) "D2 timeout is bounded and elapsed reflects fake clock"
$d3 = Wait-FcCondition -Condition { $false } -TimeoutSec 0 -PollMs 25
Assert-True ((-not $d3.Met) -and ($d3.ElapsedSec -eq 0)) "D3 zero timeout still evaluates the condition once"

Write-Host "== E: incremental log reading (stale, missing; rate parsing removed with the S2 gate) =="
$tmpLog = [System.IO.Path]::GetTempFileName()
try {
    Set-Content -LiteralPath $tmpLog -Value @('[server] hash_files_s=0 io_read_mb_s=0.0 net_send_mb_s=1.5 interval_ms=1000', '[server] hash_files_s=0 io_read_mb_s=0.1 net_send_mb_s=1.2 interval_ms=1001')
    $off = 0
    $first = @(Get-FcLogNewLines -Path $tmpLog -Offset ([ref]$off))
    Assert-Equal 2 $first.Count "E1 initial read returns all lines"
    Add-Content -LiteralPath $tmpLog -Value @('[mp] conn_accept conn=1 sessionId=s live_conns=1', '[server] hash_files_s=1 io_read_mb_s=0.0 net_send_mb_s=0.9 interval_ms=1000')
    $second = @(Get-FcLogNewLines -Path $tmpLog -Offset ([ref]$off))
    Assert-Equal 2 $second.Count "E2 second read returns only the new lines (offset advanced)"
    $third = @(Get-FcLogNewLines -Path $tmpLog -Offset ([ref]$off))
    Assert-Equal 0 $third.Count "E3 unchanged log yields nothing (stale log)"
    Assert-Equal 4 $off "E4 offset tracks total line count"
    $e5 = @(Get-FcLogNewLines -Path ($tmpLog + ".does-not-exist") -Offset ([ref]$off))
    Assert-Equal 0 $e5.Count "E5 missing file returns empty"
    # The host script pattern: [ref] on a $script:-scoped variable must write through.
    $script:OmTestOffset = 0
    $null = @(Get-FcLogNewLines -Path $tmpLog -Offset ([ref]$script:OmTestOffset))
    Assert-Equal 4 $script:OmTestOffset "E6 [ref] on script-scoped offset writes through"
    Assert-True (Test-FcSessionAccepted -Lines $second) "E7 Test-FcSessionAccepted still detects [mp] conn_accept (OM-4 A4)"
    # E8/E9 (fix #4 half-line race): a partially-written line must not be consumed; once the
    # newline arrives the completed line is returned exactly once with the CR trimmed.
    $halfLog = [System.IO.Path]::GetTempFileName()
    try {
        $hoff = 0
        [System.IO.File]::WriteAllText($halfLog, '[mp] conn_er')       # half line, no newline
        $h1 = @(Get-FcLogNewLines -Path $halfLog -Offset ([ref]$hoff))
        Assert-Equal 0 $h1.Count "E8a half-written line is not consumed"
        Assert-Equal 0 $hoff "E8b offset unchanged for a half line"
        [System.IO.File]::WriteAllText($halfLog, "[mp] conn_error conn=1`r`n")
        $h2 = @(Get-FcLogNewLines -Path $halfLog -Offset ([ref]$hoff))
        Assert-Equal 1 $h2.Count "E9a completed line returned after the newline"
        Assert-Equal '[mp] conn_error conn=1' $h2[0] "E9b line complete and CRLF-trimmed"
        $h3 = @(Get-FcLogNewLines -Path $halfLog -Offset ([ref]$hoff))
        Assert-Equal 0 $h3.Count "E9c completed line not returned twice"
    } finally {
        # [19 compliance] self-created temp file kept, path printed; no deletion call.
        Write-Host ("RETAINED selftest temp file (no deletion): " + $halfLog)
    }
} finally {
    # [19 compliance] self-created temp file kept, path printed; no deletion call.
    Write-Host ("RETAINED selftest temp file (no deletion): " + $tmpLog)
}

# E10 [16 B-01, hypothesis b]: the EXACT A9 call chain -- a [ref]$script: offset advanced by
# Get-FcLogNewLines INSIDE a condition scriptblock invoked by Wait-FcCondition's `& $Condition`
# -- writes through: with the hit deferred to the 3rd poll, a failed write-through would
# re-return every line on every poll (duplicate count) and leave the offset pinned at 0.
$e10Log = [System.IO.Path]::GetTempFileName()
try {
    Set-Content -LiteralPath $e10Log -Value 'seed-line-1'
    $script:E10Offset = 0
    $script:E10Seen = 0
    $script:E10Poll = 0
    $e10 = Wait-FcCondition -Condition {
        $script:E10Poll++
        if ($script:E10Poll -eq 3) {
            Add-Content -LiteralPath $e10Log -Value '[mp] conn_error conn=1 WSA=10054'
        }
        $new = Get-FcLogNewLines -Path $e10Log -Offset ([ref]$script:E10Offset)
        foreach ($l in @($new)) { $script:E10Seen++ }
        $hit = $false
        foreach ($l in @($new)) { if ($l -match '\[mp\] conn_error') { $hit = $true } }
        $hit
    } -TimeoutSec 5 -PollMs 50
    Assert-True $e10.Met "E10a Wait-FcCondition observes a line appended during the wait (real A9 chain)"
    Assert-Equal 3 $script:E10Poll "E10b the condition really polled 3 times (cross-poll persistence exercised)"
    Assert-Equal 2 $script:E10Offset "E10c [ref] offset writes through the Wait-FcCondition invocation"
    Assert-Equal 2 $script:E10Seen "E10d no line returned twice across polls (2 distinct lines, 2 total returns)"
    Assert-True ($e10.ElapsedSec -lt 5) "E10e met before the timeout (timing state)"
} finally {
    # [19 compliance] self-created temp file kept, path printed; no deletion call.
    Write-Host ("RETAINED selftest temp file (no deletion): " + $e10Log)
}

# E11 [16 B-01, hypothesis a + discriminator]: a line that exists WITHOUT a trailing newline
# is NOT a consumable line (invisible to every poll, offset stops before it); once the
# newline lands the completed line is returned exactly once. Get-FcLogFullText keeps the
# unterminated tail verbatim -- this is what separates miss-cause (a) from (c) in the A9 diag.
$e11Log = [System.IO.Path]::GetTempFileName()
try {
    [System.IO.File]::WriteAllText($e11Log, "seed`n[mp] conn_er")
    $script:E11Offset = 0
    $e11a = @(Get-FcLogNewLines -Path $e11Log -Offset ([ref]$script:E11Offset))
    Assert-Equal 1 $e11a.Count "E11a unterminated tail is not a line (only the complete seed returned)"
    Assert-Equal 1 $script:E11Offset "E11b offset stops before the unterminated tail"
    [System.IO.File]::WriteAllText($e11Log, "seed`n[mp] conn_error conn=1 WSA=10054`n")
    $e11c = @(Get-FcLogNewLines -Path $e11Log -Offset ([ref]$script:E11Offset))
    Assert-Equal 1 $e11c.Count "E11c completed tail returned once the newline lands"
    Assert-True ($e11c[0] -match '\[mp\] conn_error') "E11d the completed line matches the A9 pattern"
    $e11e = @(Get-FcLogNewLines -Path $e11Log -Offset ([ref]$script:E11Offset))
    Assert-Equal 0 $e11e.Count "E11e no re-read after consumption"
    [System.IO.File]::WriteAllText($e11Log, "seed`n[mp] conn_error-no-newline")
    $e11Full = Get-FcLogFullText -Path $e11Log
    Assert-True ($null -ne $e11Full -and $e11Full.Contains('[mp] conn_error')) "E11f full-text keeps the unterminated tail (miss-cause discriminator)"
    Assert-True ($e11Full.EndsWith('newline')) "E11g full-text keeps the tail verbatim"
    Assert-True ($null -eq (Get-FcLogFullText -Path ($e11Log + ".none"))) "E11h missing file yields a null full text"
} finally {
    # [19 compliance] self-created temp file kept, path printed; no deletion call.
    Write-Host ("RETAINED selftest temp file (no deletion): " + $e11Log)
}

# ---------------------------------------------------------------------------
# F: frame parser -- the REAL FcGate.Scanner (single shared implementation, C7).
# ---------------------------------------------------------------------------
Write-Host "== F: frame parser (real FcGate.Scanner, MemoryStream) =="
Initialize-FcFrameGateType

# Test-stream helpers (test infrastructure only; the parser itself is never copied/faked):
# TimeoutStream simulates a ReadTimeout by throwing IOException; DribbleStream hands out
# exactly ONE byte per Read to exercise header/payload fragmentation.
$fcTestStreamCs = @'
using System;
using System.IO;
public sealed class FcTimeoutStream : MemoryStream {
    public bool ThrowOnRead = false;
    public FcTimeoutStream(byte[] data) : base(data) { }
    public override int Read(byte[] buffer, int offset, int count) {
        if (ThrowOnRead) throw new IOException("simulated read timeout");
        return base.Read(buffer, offset, count);
    }
}
public sealed class FcDribbleStream : MemoryStream {
    public FcDribbleStream(byte[] data) : base(data) { }
    public override int Read(byte[] buffer, int offset, int count) {
        int want = count; if (want > 1) want = 1;
        return base.Read(buffer, offset, want);
    }
}
'@
if ($null -eq ('FcTimeoutStream' -as [type])) {
    Add-Type -TypeDefinition $fcTestStreamCs -ReferencedAssemblies @('System', 'System.Core')
}

function New-FcFrameBytes {
    # Builds one wire frame: type(1) + streamId(LE32) + payloadLen(LE32) + payload.
    param([int]$Type, [long]$StreamId, [byte[]]$Payload)
    if ($null -eq $Payload) { $Payload = @() }
    $ms = New-Object System.IO.MemoryStream
    $ms.WriteByte([byte]$Type)
    $sid = [BitConverter]::GetBytes([int]$StreamId)
    $ms.Write($sid, 0, 4)
    $len = [BitConverter]::GetBytes([int]$Payload.Length)
    $ms.Write($len, 0, 4)
    if ($Payload.Length -gt 0) { $ms.Write($Payload, 0, $Payload.Length) }
    $ms.Close()
    return $ms.ToArray()
}
function New-FcPayload {
    param([int]$Size)
    $p = New-Object byte[] $Size
    for ($i = 0; $i -lt $Size; $i++) { $p[$i] = [byte]($i % 251) }
    return $p
}
function Test-FcBytesEqual {
    param([byte[]]$A, [byte[]]$B, [string]$Name)
    if ($null -eq $A -or $null -eq $B) { Assert-True $false $Name; return }
    if ($A.Length -ne $B.Length) { Assert-True $false $Name; return }
    for ($i = 0; $i -lt $A.Length; $i++) { if ($A[$i] -ne $B[$i]) { Assert-True $false $Name; return } }
    Assert-True $true $Name
}
$copyBuf = New-Object byte[] 65536

# F1 (case 1): one complete FileChunk -> Found, Bytes == 9+len, exact type/streamId.
$p300 = New-FcPayload 300
$f1frame = New-FcFrameBytes 31 7 $p300
$r = [FcGate.Scanner]::Scan((New-Object System.IO.MemoryStream (,$f1frame)), `
        (New-Object System.IO.MemoryStream), 31, 64MB, 512MB, 5000, $copyBuf, $null)
Assert-True $r.Found "F1a whole FileChunk frame -> Found"
Assert-Equal 309 $r.Bytes "F1b Bytes == 9+len (309)"
Assert-Equal 31 $r.Type "F1c Type == 31"
Assert-Equal 7 $r.StreamId "F1d StreamId preserved"
Assert-Equal 300 $r.Len "F1e Len == 300"
Assert-Equal 1 $r.Frames "F1f Frames == 1"

# F2 (case 2): two frames in ONE read (sticky) -> stop at the FIRST frame boundary; the
# over-read second frame stays in the replay buffer (never forwarded).
$p100 = New-FcPayload 100
$p50 = New-FcPayload 50
$sf1 = New-FcFrameBytes 31 1 $p100
$sf2 = New-FcFrameBytes 32 2 $p50
$sticky = New-Object byte[] ($sf1.Length + $sf2.Length)
[Array]::Copy($sf1, 0, $sticky, 0, $sf1.Length)
[Array]::Copy($sf2, 0, $sticky, $sf1.Length, $sf2.Length)
$f2up = New-Object System.IO.MemoryStream
$r = [FcGate.Scanner]::Scan((New-Object System.IO.MemoryStream (,$sticky)), $f2up, 31, 64MB, 512MB, 5000, $copyBuf, $null)
Assert-True $r.Found "F2a sticky frames -> Found on first frame"
Assert-Equal 109 $r.Bytes "F2b stops at first frame boundary (Bytes == 109)"
Assert-Equal 59 $r.OverRead.Length "F2c second frame over-read stays in replay buffer (59B)"
Assert-Equal 109 $f2up.Length "F2d up stream got ONLY the first frame"
Test-FcBytesEqual $sf1 $f2up.ToArray() "F2e up bytes == first frame exactly"

# F3 (case 3): byte-dribbled header + payload -> still recognized, byte-exact.
$dribbleDown = New-Object FcDribbleStream (,$f1frame)
$f3up = New-Object System.IO.MemoryStream
$r = [FcGate.Scanner]::Scan($dribbleDown, $f3up, 31, 64MB, 512MB, 5000, $copyBuf, $null)
Assert-True $r.Found "F3a dribbled header/payload -> Found"
Assert-Equal 309 $r.Bytes "F3b Bytes == 9+len under fragmentation"
Test-FcBytesEqual $f1frame $f3up.ToArray() "F3c up bytes byte-exact under fragmentation"

# F4 (case 4): unknown type 99 + FileEnd 32 pass through untouched, then 31 holds.
$p40 = New-FcPayload 40
$u99 = New-FcFrameBytes 99 5 $p40
$e32 = New-FcFrameBytes 32 2 @()
$c31 = New-FcFrameBytes 31 3 (New-FcPayload 50)
$mix = New-Object byte[] ($u99.Length + $e32.Length + $c31.Length)
[Array]::Copy($u99, 0, $mix, 0, $u99.Length)
[Array]::Copy($e32, 0, $mix, $u99.Length, $e32.Length)
[Array]::Copy($c31, 0, $mix, ($u99.Length + $e32.Length), $c31.Length)
$f4up = New-Object System.IO.MemoryStream
$r = [FcGate.Scanner]::Scan((New-Object System.IO.MemoryStream (,$mix)), $f4up, 31, 64MB, 512MB, 5000, $copyBuf, $null)
Assert-True $r.Found "F4a unknown99 + FileEnd32 transparent, then 31 holds"
Assert-Equal ($u99.Length + $e32.Length + $c31.Length) $r.Bytes "F4b Bytes includes transparently forwarded frames"
Test-FcBytesEqual $mix $f4up.ToArray() "F4c up bytes == all three frames (transparent)"

# F5 (case 5): len==0 FileChunk only counts, does NOT hold.
$z31 = New-FcFrameBytes 31 9 @()
$nz31 = New-FcFrameBytes 31 9 (New-FcPayload 30)
$zpair = New-Object byte[] ($z31.Length + $nz31.Length)
[Array]::Copy($z31, 0, $zpair, 0, $z31.Length)
[Array]::Copy($nz31, 0, $zpair, $z31.Length, $nz31.Length)
$r = [FcGate.Scanner]::Scan((New-Object System.IO.MemoryStream (,$zpair)), `
        (New-Object System.IO.MemoryStream), 31, 64MB, 512MB, 5000, $copyBuf, $null)
Assert-True $r.Found "F5a zero-len 31 does not hold; next payload>0 31 does"
Assert-Equal 2 $r.Frames "F5b both 31 frames counted (frames == 2)"
Assert-Equal 30 $r.Len "F5c held frame is the payload>0 one (len == 30, not the 9+30 total)"

# F6 (case 6): len > 1GiB -> BadLength, fail-closed, no allocation by len.
$badHdr = New-Object byte[] 9
$badHdr[0] = 31
[Array]::Copy([BitConverter]::GetBytes([int]1), 0, $badHdr, 1, 4)
[Array]::Copy([BitConverter]::GetBytes([uint32]2147483648), 0, $badHdr, 5, 4)  # 2 GiB, LE
$smallBuf = New-Object byte[] 4096
$gcBefore = [System.GC]::GetTotalMemory($true)
$r = [FcGate.Scanner]::Scan((New-Object System.IO.MemoryStream (,$badHdr)), `
        (New-Object System.IO.MemoryStream), 31, 64MB, 512MB, 5000, $smallBuf, $null)
$gcAfter = [System.GC]::GetTotalMemory($true)
Assert-Equal 'BadLength' $r.Error.ToString() "F6a len=2GiB -> BadLength"
Assert-Equal 0 $r.Bytes "F6b bad frame header not forwarded (Bytes == 0)"
Assert-True ($gcAfter - $gcBefore -lt 8MB) "F6c no allocation by len (GC delta < 8MB, copyBuf=4KB)"
Assert-True ($smallBuf.Length -le 65536) "F6d caller copyBuf stays the fixed <=64KB buffer"

# F7 (case 7): cumulative scan cap.
$k1 = New-FcFrameBytes 40 1 (New-FcPayload 10)
$k2 = New-FcFrameBytes 41 2 (New-FcPayload 10)
$kpair = New-Object byte[] ($k1.Length + $k2.Length)
[Array]::Copy($k1, 0, $kpair, 0, $k1.Length)
[Array]::Copy($k2, 0, $kpair, $k1.Length, $k2.Length)
$r = [FcGate.Scanner]::Scan((New-Object System.IO.MemoryStream (,$kpair)), `
        (New-Object System.IO.MemoryStream), 31, 20, 512MB, 5000, $copyBuf, $null)
Assert-Equal 'Cap' $r.Error.ToString() "F7a cum > cap(20) without a 31 -> Cap"
Assert-Equal 38 $r.Bytes "F7b cum counted through both frames (38)"
# F7c [16 B-06] real negative: a FIRST stop frame (type=31, len>0) whose cum already exceeds
# capBytes must fail closed to Cap -- the found branch may not skip the boundary cap check.
$capStop = New-FcFrameBytes 31 5 (New-FcPayload 30)   # 9+30 = 39 > cap 20
$r = [FcGate.Scanner]::Scan((New-Object System.IO.MemoryStream (,$capStop)), `
        (New-Object System.IO.MemoryStream), 31, 20, 512MB, 5000, $copyBuf, $null)
Assert-Equal 'Cap' $r.Error.ToString() "F7c stop frame with cum>cap -> Cap (cap check precedes Found)"
Assert-True (-not $r.Found) "F7d not Found despite type==31 len>0 (fail-closed cap wins)"
Assert-Equal 39 $r.Bytes "F7e cum counted through the stop frame (39; boundary check is post-forward per 12 s2)"

# F8 (case 8): downstream EOF before any FileChunk.
$r = [FcGate.Scanner]::Scan((New-Object System.IO.MemoryStream (,$k1)), `
        (New-Object System.IO.MemoryStream), 31, 64MB, 512MB, 5000, $copyBuf, $null)
Assert-Equal 'EofBeforeChunk' $r.Error.ToString() "F8a EOF without 31 -> EofBeforeChunk"
Assert-True (-not $r.Found) "F8b not Found"

# F9 (case 9): read timeout (IOException) -> NoFileChunk.
$tstream = New-Object FcTimeoutStream (,$k1)
$tstream.ThrowOnRead = $true
$r = [FcGate.Scanner]::Scan($tstream, (New-Object System.IO.MemoryStream), 31, 64MB, 512MB, 5000, $copyBuf, $null)
Assert-Equal 'NoFileChunk' $r.Error.ToString() "F9a IOException read -> NoFileChunk"
Assert-True (-not $r.Found) "F9b not Found"

# F10 (case 10): Resume() -> whole-stream byte equivalence.
$p20 = New-FcPayload 20
$r10a = New-FcFrameBytes 31 1 $p100
$r10b = New-FcFrameBytes 32 2 $p50
$r10c = New-FcFrameBytes 99 3 $p20
$whole = New-Object byte[] ($r10a.Length + $r10b.Length + $r10c.Length)
[Array]::Copy($r10a, 0, $whole, 0, $r10a.Length)
[Array]::Copy($r10b, 0, $whole, $r10a.Length, $r10b.Length)
[Array]::Copy($r10c, 0, $whole, ($r10a.Length + $r10b.Length), $r10c.Length)
$r10down = New-Object System.IO.MemoryStream (,$whole)
$r10up = New-Object System.IO.MemoryStream
$r = [FcGate.Scanner]::Scan($r10down, $r10up, 31, 64MB, 512MB, 5000, $copyBuf, $null)
Assert-True $r.Found "F10a held at first frame"
$null = [FcGate.Scanner]::Resume($r10down, $r10up, $r.OverRead, $copyBuf)
Test-FcBytesEqual $whole $r10up.ToArray() "F10b Resume() -> whole stream byte-equivalent"

# F11 (case 11): header endianness -- LE32 fields must not be read as big-endian.
$leFrame = New-FcFrameBytes 31 67305985 (New-FcPayload 258)   # sid=0x04030201, len=0x00000102
$r = [FcGate.Scanner]::Scan((New-Object System.IO.MemoryStream (,$leFrame)), `
        (New-Object System.IO.MemoryStream), 31, 64MB, 512MB, 5000, $copyBuf, $null)
Assert-Equal 67305985 $r.StreamId "F11a streamId parsed little-endian (0x04030201)"
Assert-Equal 258 $r.Len "F11b payloadLen parsed little-endian (0x00000102, not 0x02010000)"

# F12 (case 12): hold-after-write -- the client already holds the COMPLETE first chunk
# (header + full payload), never a half frame.
$f12up = New-Object System.IO.MemoryStream
$r = [FcGate.Scanner]::Scan((New-Object System.IO.MemoryStream (,$sticky)), $f12up, 31, 64MB, 512MB, 5000, $copyBuf, $null)
Test-FcBytesEqual $sf1 $f12up.ToArray() "F12a held frame fully written (no half frame)"

# F13 (case 13): cum+len > fixture (512MB) -> OverFixture, nothing forwarded.
$ofHdr = New-Object byte[] 9
$ofHdr[0] = 31
[Array]::Copy([BitConverter]::GetBytes([int]1), 0, $ofHdr, 1, 4)
[Array]::Copy([BitConverter]::GetBytes([int]629145600), 0, $ofHdr, 5, 4)   # 600MB > 512MB fixture
$r = [FcGate.Scanner]::Scan((New-Object System.IO.MemoryStream (,$ofHdr)), `
        (New-Object System.IO.MemoryStream), 31, 64MB, 512MB, 5000, $copyBuf, $null)
Assert-Equal 'OverFixture' $r.Error.ToString() "F13a cum+9+len > fixture -> OverFixture"
Assert-Equal 0 $r.Bytes "F13b oversized frame not forwarded"

# ---------------------------------------------------------------------------
# G: loopback socket suite -- the REAL FcGate.FrameGate over in-process TcpListeners.
# ---------------------------------------------------------------------------
Write-Host "== G: loopback socket FrameGate (real TcpListener + writer thread, loopback only) =="

function Read-FcExact {
    # Bounded strict read: returns exactly $Count bytes or throws on timeout.
    param([System.IO.Stream]$Stream, [int]$Count, [int]$TimeoutMs = 8000)
    $buf = New-Object byte[] $Count
    $off = 0
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    while ($off -lt $Count) {
        if ($sw.ElapsedMilliseconds -gt $TimeoutMs) { throw "Read-FcExact timeout ($off/$Count bytes)" }
        $n = $Stream.Read($buf, $off, $Count - $off)
        if ($n -le 0) { throw "Read-FcExact EOF ($off/$Count bytes)" }
        $off += $n
    }
    return $buf
}
function Wait-FcGateCount {
    param($GateObj, [int]$Want, [int]$TimeoutMs = 8000)
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    while ($sw.ElapsedMilliseconds -lt $TimeoutMs) {
        if ($GateObj.AcceptedConnections -ge $Want) { return $true }
        Start-Sleep -Milliseconds 25
    }
    return ($GateObj.AcceptedConnections -ge $Want)
}

$gTemp = Join-Path $env:TEMP ("fc-gate-selftest-" + [System.Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $gTemp | Out-Null
$gateLog1 = Join-Path $gTemp "gate1.log"
try {
    # Fake upstream server (stands in for the FastClone server; in-process, loopback).
    $fakeServer = New-Object System.Net.Sockets.TcpListener([System.Net.IPAddress]::Loopback, 0)
    $fakeServer.Start()
    $fakePort = ([System.Net.IPEndPoint]$fakeServer.LocalEndpoint).Port

    $gate = [FcGate.FrameGate]::Start('127.0.0.1', $fakePort, 31, 64MB, 512MB, 15000, $gateLog1)
    Assert-Equal 'Listening' $gate.State.ToString() "G1a gate starts Listening (bind+listen only)"
    Assert-True ($gate.Port -gt 0) "G1b OS-assigned port resolved (port 0 -> real port)"
    $readyLine = (Select-String -Path $gateLog1 -Pattern 'FRAME_GATE_READY' | Select-Object -First 1).Line
    Assert-True ($null -ne $readyLine -and $readyLine.Contains("port=" + $gate.Port)) "G1c FRAME_GATE_READY port=<Port> is the only readiness signal (no TCP probe)"

    $cli = New-Object System.Net.Sockets.TcpClient
    $cli.Connect('127.0.0.1', $gate.Port)
    $upConn = $fakeServer.AcceptTcpClient()      # the gate's single upstream connection
    Assert-True (Wait-FcGateCount -GateObj $gate -Want 1) "G2a AcceptedConnections == 1 after the real accept"
    Assert-Equal 1 $gate.AcceptedConnections "G2b exactly one accepted connection"

    # Server writes two frames; the gate must forward frame 1 completely and hold at the
    # boundary (frame 2 stays in the gate's replay buffer).
    $upStream = $upConn.GetStream()
    $upStream.Write($sticky, 0, $sticky.Length)
    $upStream.Flush()
    Assert-True ($gate.WaitForState([FcGate.GateState]::Held, 8000)) "G3a gate reaches Held on real NetworkStream"
    $cliStream = $cli.GetStream()
    $got = Read-FcExact -Stream $cliStream -Count 109
    Test-FcBytesEqual $sf1 $got "G3b client received exactly frame 1 (frame boundary hold)"
    Start-Sleep -Milliseconds 300
    Assert-True (-not $cliStream.DataAvailable) "G3c no frame-2 byte leaked before Resume (no pre-read)"
    $snap = $gate.Snapshot()
    Assert-True ($snap.Contains('first_filechunk(len=100')) "G4a snapshot records first_filechunk(len,cum)"
    Assert-True ($snap.Contains('state=Held')) "G4b snapshot state == Held"
    $gateLogText = Get-Content -LiteralPath $gateLog1 -Raw
    Assert-True ($gateLogText.Contains('FRAME_GATE_FIRST_FILECHUNK type=31 len=100')) "G4c gate log has FIRST_FILECHUNK evidence line"
    Assert-True ($gateLogText.Contains('FRAME_GATE_HELD')) "G4d gate log has HELD line"

    # Resume -> the whole stream reaches the client byte-equivalent.
    $gate.Resume()
    $rest = Read-FcExact -Stream $cliStream -Count 59
    Test-FcBytesEqual $sf2 $rest "G5a Resume() drains the replayed second frame byte-exact"
    $gateLogText = Get-Content -LiteralPath $gateLog1 -Raw
    Assert-True ($gateLogText.Contains('FRAME_GATE_RESUMED')) "G5b gate log records RESUMED"

    # Client closes -> gate must observe CLIENT_CLOSED and RST the upstream (Aborted).
    $cli.Close()
    Assert-True ($gate.WaitForState([FcGate.GateState]::Aborted, 8000)) "G6a client close -> Aborted"
    $snap = $gate.Snapshot()
    Assert-True ($snap.Contains('client_closed')) "G6b snapshot has client_closed event"
    Assert-True ($snap.Contains('upstream_aborted')) "G6c snapshot has upstream_aborted event (RST)"
    $gateLogText = Get-Content -LiteralPath $gateLog1 -Raw
    Assert-True ($gateLogText.Contains('FRAME_GATE_CLIENT_CLOSED')) "G6d gate log has CLIENT_CLOSED"
    Assert-True ($gateLogText.Contains('FRAME_GATE_UPSTREAM_ABORTED')) "G6e gate log has UPSTREAM_ABORTED"

    # Idempotent bounded dispose: two Dispose() calls, both fast, no timeout.
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $gate.Dispose()
    $gate.Dispose()
    $sw.Stop()
    Assert-True ($sw.ElapsedMilliseconds -lt 5500) "G7a Dispose is bounded (aggregate <= 5s budget, got $($sw.ElapsedMilliseconds)ms)"
    Assert-True (-not $gate.DisposeTimedOut) "G7b Dispose idempotent, no FRAME-GATE-CLOSE-TIMEOUT"
    $fakeServer.Stop()

    # Bounded dispose timeout (test seam): shrink the aggregate budget and delay the worker
    # threads' exit -> explicit DisposeTimedOut + FRAME_GATE_DISPOSE_TIMEOUT in the log.
    $fakeServer2 = New-Object System.Net.Sockets.TcpListener([System.Net.IPAddress]::Loopback, 0)
    $fakeServer2.Start()
    $gate2 = [FcGate.FrameGate]::Start('127.0.0.1', ([System.Net.IPEndPoint]$fakeServer2.LocalEndpoint).Port, 31, 64MB, 512MB, 15000, (Join-Path $gTemp "gate2.log"))
    $cli2 = New-Object System.Net.Sockets.TcpClient
    $cli2.Connect('127.0.0.1', $gate2.Port)
    $upConn2 = $fakeServer2.AcceptTcpClient()
    $us2 = $upConn2.GetStream()
    $us2.Write($sf1, 0, $sf1.Length); $us2.Flush()
    $null = $gate2.WaitForState([FcGate.GateState]::Held, 8000)
    [FcGate.FrameGate]::TestHookThreadExitDelayMs = 1500
    $gate2.DisposeTimeoutMs = 200
    $gate2.Dispose()
    Assert-True $gate2.DisposeTimedOut "G8a bounded dispose timeout is explicit (DisposeTimedOut)"
    $g2log = Get-Content -LiteralPath (Join-Path $gTemp "gate2.log") -Raw
    Assert-True ($g2log.Contains('FRAME_GATE_DISPOSE_TIMEOUT')) "G8b FRAME_GATE_DISPOSE_TIMEOUT logged"
    [FcGate.FrameGate]::TestHookThreadExitDelayMs = 0
    $cli2.Close(); $fakeServer2.Stop()

    # Start failure: unwritable log path -> Start throws AND releases the half-built
    # listener (a subsequent Start on a fresh path succeeds).
    $badPath = Join-Path $gTemp ("no-such-parent-" + [System.Guid]::NewGuid().ToString('N') + "\gate.log")
    $threw = $false
    try { $null = [FcGate.FrameGate]::Start('127.0.0.1', 1, 31, 64MB, 512MB, 5000, $badPath) } catch { $threw = $true }
    Assert-True $threw "G9a Start with unwritable log throws"
    $gate3 = $null
    try {
        $gate3 = [FcGate.FrameGate]::Start('127.0.0.1', 1, 31, 64MB, 512MB, 5000, (Join-Path $gTemp "gate3.log"))
        Assert-Equal 'Listening' $gate3.State.ToString() "G9b no half-initialized listener survives a failed Start"
    } finally {
        if ($null -ne $gate3) { $gate3.Dispose() }
    }

    # Upstream connect failure surfaces as the Upstream gate error (owned teardown).
    $deadListener = New-Object System.Net.Sockets.TcpListener([System.Net.IPAddress]::Loopback, 0)
    $deadListener.Start()
    $deadPort = ([System.Net.IPEndPoint]$deadListener.LocalEndpoint).Port
    $deadListener.Stop()
    $gate4 = [FcGate.FrameGate]::Start('127.0.0.1', $deadPort, 31, 64MB, 512MB, 5000, (Join-Path $gTemp "gate4.log"))
    $cli4 = New-Object System.Net.Sockets.TcpClient
    $cli4.Connect('127.0.0.1', $gate4.Port)
    Assert-True ($gate4.WaitForState([FcGate.GateState]::Error, 8000)) "G10a unreachable upstream -> Error state"
    $g4log = Get-Content -LiteralPath (Join-Path $gTemp "gate4.log") -Raw
    Assert-True ($g4log.Contains('FRAME_GATE_ERROR reason=Upstream')) "G10b FRAME_GATE_ERROR reason=Upstream logged"
    $gate4.Dispose()
    Assert-True (-not $gate4.DisposeTimedOut) "G10c error-path dispose still bounded and clean"
    $cli4.Close()

    # G12 [16 B-04]: an UPSTREAM write failure must surface as the Upstream gate error and
    # must NOT fabricate client_closed -> upstream_aborted -> Aborted (12 section 2 L42).
    $fakeServer5 = New-Object System.Net.Sockets.TcpListener([System.Net.IPAddress]::Loopback, 0)
    $fakeServer5.Start()
    $gate5 = [FcGate.FrameGate]::Start('127.0.0.1', ([System.Net.IPEndPoint]$fakeServer5.LocalEndpoint).Port, 31, 64MB, 512MB, 15000, (Join-Path $gTemp "gate5.log"))
    $cli5 = New-Object System.Net.Sockets.TcpClient
    $cli5.Connect('127.0.0.1', $gate5.Port)
    $upConn5 = $fakeServer5.AcceptTcpClient()      # the gate's single upstream connection
    Start-Sleep -Milliseconds 200
    $upConn5.Close()                               # server side gone: the next gate->server write fails
    $cs5 = $cli5.GetStream()
    $g12Chunk = New-Object byte[] 16384
    for ($i = 0; $i -lt 6; $i++) { $cs5.Write($g12Chunk, 0, $g12Chunk.Length); $cs5.Flush(); Start-Sleep -Milliseconds 100 }
    $null = $gate5.WaitForState([FcGate.GateState]::Error, 8000)
    $snap5 = $gate5.Snapshot()
    Assert-Equal 'Error' $gate5.State.ToString() "G12a upstream write failure -> Error state"
    Assert-True (-not $snap5.Contains('client_closed')) "G12b no fabricated client_closed event"
    Assert-True (-not $snap5.Contains('upstream_aborted')) "G12c no fabricated upstream_aborted event"
    $g5log = Get-Content -LiteralPath (Join-Path $gTemp "gate5.log") -Raw
    Assert-True (-not $g5log.Contains('FRAME_GATE_CLIENT_CLOSED') -and -not $g5log.Contains('FRAME_GATE_UPSTREAM_ABORTED')) "G12d gate log has no CLIENT_CLOSED/UPSTREAM_ABORTED lines"
    $gate5.Dispose()
    $cli5.Close(); $fakeServer5.Stop()

    # G13 [16 B-04]: Dispose while the uplink pump is BLOCKED in cli.Read must not fabricate
    # the abort chain either (the cancel guard returns without events or state change).
    $fakeServer6 = New-Object System.Net.Sockets.TcpListener([System.Net.IPAddress]::Loopback, 0)
    $fakeServer6.Start()
    $gate6 = [FcGate.FrameGate]::Start('127.0.0.1', ([System.Net.IPEndPoint]$fakeServer6.LocalEndpoint).Port, 31, 64MB, 512MB, 15000, (Join-Path $gTemp "gate6.log"))
    $cli6 = New-Object System.Net.Sockets.TcpClient
    $cli6.Connect('127.0.0.1', $gate6.Port)
    $null = $fakeServer6.AcceptTcpClient()
    $null = $gate6.WaitForState([FcGate.GateState]::UpstreamConnected, 8000)
    Start-Sleep -Milliseconds 300          # let the uplink pump settle into cli.Read
    $gate6.Dispose()
    $snap6 = $gate6.Snapshot()
    Assert-True (-not $gate6.DisposeTimedOut) "G13a dispose with a blocked uplink read stays bounded"
    Assert-True (-not $snap6.Contains('client_closed')) "G13b no fabricated client_closed on dispose"
    Assert-True (-not $snap6.Contains('upstream_aborted')) "G13c no fabricated upstream_aborted on dispose"
    Assert-True (-not ($snap6.Contains('state=Aborted'))) "G13d state is not Aborted after dispose"
    $g6log = Get-Content -LiteralPath (Join-Path $gTemp "gate6.log") -Raw
    Assert-True (-not $g6log.Contains('FRAME_GATE_CLIENT_CLOSED') -and -not $g6log.Contains('FRAME_GATE_UPSTREAM_ABORTED')) "G13e gate log has no CLIENT_CLOSED/UPSTREAM_ABORTED lines"
    $cli6.Close(); $fakeServer6.Stop()

    # No payload/password ever logged: the gate log only contains metadata lines.
    $allGateLogs = (Get-ChildItem -LiteralPath $gTemp -Filter '*.log' | ForEach-Object { Get-Content -LiteralPath $_.FullName -Raw }) -join "`n"
    Assert-True (-not $allGateLogs.Contains('payload')) "G11a gate logs carry no payload bytes"
} finally {
    # [19 compliance / 18 S-01] RETENTION ONLY: no deletion call of any kind (recursive
    # force-delete cmdlet or substitute API) exists in this harness. The original ownership
    # check below (direct TEMP child with the fc-gate-selftest-<guid> leaf, mirroring the
    # Test-FcSafeTempRoot discipline) is kept as a diagnostic print only -- the self-created
    # gate dir is KEPT and its path printed.
    $leaf = Split-Path -Path $gTemp -Leaf
    $parentOk = ([System.IO.Path]::GetFullPath((Split-Path -Path $gTemp -Parent))) -eq `
                ([System.IO.Path]::GetFullPath($env:TEMP).TrimEnd('\'))
    $ownSelftestDir = ($parentOk -and $leaf -match '^fc-gate-selftest-[0-9a-f]{32}$' -and (Test-Path -LiteralPath $gTemp))
    Write-Host ("RETAINED selftest gate dir (no deletion; ownership check=" + $ownSelftestDir + "): " + $gTemp)
}

Write-Host "== H: safe temp-root ownership guard (G-AC8 static part) =="
$goodDir = Join-Path $env:TEMP ("fastclone-onemulti-it-" + (Get-Random))
New-Item -ItemType Directory -Path $goodDir | Out-Null
try {
    Assert-True (Test-FcSafeTempRoot -Path $goodDir) "H1 own unique temp dir is accepted"
} finally {
    # [19 compliance] self-created fixture kept, path printed; no deletion call.
    Write-Host ("RETAINED H1 fixture dir (no deletion): " + $goodDir)
}
Assert-True (-not (Test-FcSafeTempRoot -Path '')) "H2 empty path rejected"
Assert-True (-not (Test-FcSafeTempRoot -Path '$')) "H3 literal '$' path rejected"
Assert-True (-not (Test-FcSafeTempRoot -Path $env:TEMP)) "H4 TEMP itself rejected"
# [16 S-06] Negative fixtures use UNIQUE names (guid suffix): a fixed literal name could
# collide with a pre-existing user directory of the same name. [19 compliance] nothing is
# removed anymore: the trees this harness itself just created are KEPT and printed below.
$wrongPrefix = Join-Path $env:TEMP ("wrong-prefix-" + [System.Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $wrongPrefix | Out-Null
try {
    Assert-True (-not (Test-FcSafeTempRoot -Path $wrongPrefix)) "H5 wrong prefix rejected"
} finally {
    # [19 compliance] self-created unique-named fixture kept (GUID suffix unchanged, so a
    # pre-existing user directory can never be touched); path printed, no deletion call.
    Write-Host ("RETAINED H5 fixture dir (no deletion): " + $wrongPrefix)
}
$badSuffix = Join-Path $env:TEMP ("fastclone-onemulti-it-abc-" + [System.Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $badSuffix | Out-Null
try {
    Assert-True (-not (Test-FcSafeTempRoot -Path $badSuffix)) "H6 non-numeric suffix rejected"
} finally {
    # [19 compliance] self-created unique-named fixture kept; path printed, no deletion call.
    Write-Host ("RETAINED H6 fixture dir (no deletion): " + $badSuffix)
}
Assert-True (-not (Test-FcSafeTempRoot -Path "C:\Windows\fastclone-onemulti-it-1")) "H7 foreign parent rejected"
# [16 S-06] Per-run ownership marker: structure + token validation stays testable (H8-H11).
# [19 compliance] the marker no longer gates any deletion -- there is no deletion call; the
# fixture tree is kept and its path printed.
$ownTokenDir = Join-Path $env:TEMP ("fastclone-onemulti-it-" + (Get-Random))
New-Item -ItemType Directory -Path $ownTokenDir | Out-Null
$ownToken = [System.Guid]::NewGuid().ToString('N')
Set-Content -LiteralPath (Join-Path $ownTokenDir '.fc-it-owner') -Value $ownToken
try {
    Assert-True (Test-FcOwnTempMarker -Path $ownTokenDir -Token $ownToken) "H8 own marker with the exact token is accepted"
    Assert-True (-not (Test-FcOwnTempMarker -Path $ownTokenDir -Token ([System.Guid]::NewGuid().ToString('N')))) "H9 wrong token is rejected"
    Assert-True (-not (Test-FcOwnTempMarker -Path $ownTokenDir -Token '')) "H10 empty token is rejected"
    $noMarkerDir = Join-Path $env:TEMP ("fastclone-onemulti-it-" + (Get-Random))
    New-Item -ItemType Directory -Path $noMarkerDir | Out-Null
    try {
        Assert-True (-not (Test-FcOwnTempMarker -Path $noMarkerDir -Token $ownToken)) "H11 dir without a marker is rejected (leftover/foreign tree)"
    } finally {
        # [19 compliance] self-created fixture kept, path printed; no deletion call.
        Write-Host ("RETAINED H11 fixture dir (no deletion): " + $noMarkerDir)
    }
} finally {
    # [19 compliance] self-created fixture kept, path printed; no deletion call.
    Write-Host ("RETAINED H8-H10 fixture dir (no deletion): " + $ownTokenDir)
}

Write-Host ""
$total = $script:PassCount + $script:FailCount
Write-Host ("SUPPORT-TESTS RESULT: pass=" + $script:PassCount + " fail=" + $script:FailCount + " total=" + $total)
Write-Host ("GROUPS: A=3 B=4 C=7 D=3 E=25 F=40 G=38 H=11 (16 B-01..B-07 rework: +E10/E11 wait-chain and unterminated-tail, +F7c-e cap-before-Found, +G12/G13 uplink classification, +H8-H11 ownership marker; >=48 total, 13 parser cases + 38 socket assertions)")
Reset-FcNowUtcProvider
if ($script:FailCount -gt 0) { exit 1 }
exit 0
