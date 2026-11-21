# FastClone --once (oneshot server) integration smoke tests (Windows).
# Usage: .\tests\once_server_integration.ps1 [-ExePath path\to\FastClone.exe] [-Port 27893]
# Creates small temp dirs; always kills FastClone processes on exit.
#
# Covers (design §5.2):
#   OS-1 success path   -> server --once auto-exits with code 0 (V-04 / AC-04/AC-08A/AC-12)
#   OS-5 terminal path  -> after once terminal, second client is refused (AC-10 / FR-10/FR-11)
#   OS-5b in-flight path-> while target session is active, second Auth gets AuthFail (AC-10)
#   OS-2 failure path   -> served session aborted -> server exit 5 (V-05/V-06 / AC-05/AC-06)
#   OS-3 usage errors   -> client --once / --once+--enable-hash-memcache -> exit 1 (V-02/V-03)
#   OS-4 probe (opt.)   -> pre-handshake close does not exit the server (V-07 / AC-07)

param(
    [string]$ExePath = "",
    [int]$Port = 27893,
    # Test-only: bind server to ::1 (loopback) to isolate from LAN/WAN traffic (OM-4 flaky root cause).
    [switch]$BindLoopback
)

$ErrorActionPreference = "Stop"

# Optional server arg fragment: empty in production, "--bind-loopback" in test mode.
$bindLoopbackArg = if ($BindLoopback) { "--bind-loopback" } else { "" }

# [59 section 3.7 / C7] OS-5b needs the loopback frame-gate proxy (Start-FcFrameGateProxy)
# for its deterministic in-flight hold. The support module is DEFINITIONS + shared-state
# init ONLY (no main body, no side effects at dot-source time; the C# compile happens on
# the first Start-FcFrameGateProxy call). Function sets are disjoint from this script's
# own helpers (verified: 17 vs 18 names, zero overlaps), so the dot-source only ADDS the
# gate/budget-wait/log-parse primitives this script now uses.
. "$PSScriptRoot\once_multi_test_support.ps1"

function Resolve-FastCloneExe {
    param([string]$Hint)
    if ($Hint -and (Test-Path $Hint)) { return (Resolve-Path $Hint).Path }
    $candidates = @(
        "$PSScriptRoot\..\x64\Release\FastClone.exe",
        "$PSScriptRoot\..\build\Release\FastClone.exe",
        "$PSScriptRoot\..\build\FastClone.exe"
    )
    foreach ($c in $candidates) {
        if (Test-Path $c) { return (Resolve-Path $c).Path }
    }
    throw "FastClone.exe not found; pass -ExePath"
}

# ---------------------------------------------------------------------------
# Task ci-integration-test-timeout: global bounded-execution skeleton (design sections
# 3.4/3.5/3.12/3.13). Canonical implementation; the same block is copied into every
# integration script (N3: no shared dot-source module).
# NOTE (gate H-1): this script's Wait-ExitCode is the 7th clamp site the design v2
# missed (E-1); its budget chain (30+20+10+180+180+30+30 = 480s) does not close I-2
# without the clamp below.
# ---------------------------------------------------------------------------

# [I-2] Global wall-clock budget. ctest hard-kills at 600s, and a hard-kill means neither the
# catch block nor any diagnostic ever runs -- which is exactly why #2/#6/#9 produced nothing
# but "(Timeout)" on build #58. Aborting at 480s leaves 120s for the diagnostic dump + cleanup.
$script:FcDeadlineUtc = (Get-Date).ToUniversalTime().AddSeconds(480)

function Get-FcRemainingSec {
    param([int]$ReserveSec = 30)
    $r = ($script:FcDeadlineUtc - (Get-Date).ToUniversalTime()).TotalSeconds - $ReserveSec
    if ($r -lt 0) { $r = 0 }
    return [int][Math]::Floor($r)
}

function Assert-FcDeadline {
    param([string]$Stage)
    if ((Get-FcRemainingSec) -le 0) {
        throw "GLOBAL-DEADLINE exceeded at stage='$Stage' (480s budget, 30s reserve)"
    }
}

# [FR-03] PID registry: only processes THIS script started.
$script:FcOwnedPids = @{}

function Register-OwnedProcess {
    param([System.Diagnostics.Process]$Proc, [string]$Tag)
    $script:FcOwnedPids[[int]$Proc.Id] = $Tag
    return $Proc
}

function Stop-OwnedFastClone {
    foreach ($pidKey in @($script:FcOwnedPids.Keys)) {
        $p = Get-Process -Id $pidKey -ErrorAction SilentlyContinue
        # [FR-03] PID-reuse guard: a recycled PID may now belong to an unrelated process.
        # Only kill it if it is still FastClone.
        if ($p -and $p.ProcessName -eq 'FastClone') {
            Stop-Process -Id $pidKey -Force -ErrorAction SilentlyContinue
        }
    }
    $script:FcOwnedPids = @{}
}

# [FR-02] Bounded port helpers. Replaces the fixed `Start-Sleep -Seconds 1` readiness
# assumption with a bounded poll on real OS state.
function Get-PortHolders {
    param([int]$PortNum)
    $rows = @()
    foreach ($l in (netstat -ano | Select-String -Pattern ":$PortNum " -SimpleMatch)) {
        $f = ($l.Line -split '\s+') | Where-Object { $_ -ne '' }
        if ($f.Count -ge 5) {
            $rows += [pscustomobject]@{
                Proto = $f[0]; Local = $f[1]; Remote = $f[2]; State = $f[3]; Pid = [int]$f[4]
            }
        }
    }
    return $rows
}

function Wait-PortFree {
    param([int]$PortNum, [int]$TimeoutSec = 15)
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSec)
    while ([DateTime]::UtcNow -lt $deadline) {
        # Only a LISTENING socket on the local port actually blocks the next bind.
        # TIME_WAIT entries (usually the CLIENT side of a previous connection, whose REMOTE
        # column matches ":port") do NOT block bind -- waiting for them to clear would cost
        # the full 15s per sub-case on the healthy path (observed +130s on file_range).
        $blocking = @(Get-PortHolders -PortNum $PortNum | Where-Object { $_.State -eq 'LISTENING' })
        if ($blocking.Count -eq 0) { return $true }
        # A non-FastClone LISTENING holder will not go away by waiting (F-2 observed
        # 3x15s of dead waiting on a foreign holder): bail out immediately with the
        # caller's warning instead of burning the budget. Only a FastClone holder
        # (e.g. just killed by the port-scoped cleanup) can be expected to release.
        $fcHeld = $false
        foreach ($b in $blocking) {
            $bp = Get-Process -Id $b.Pid -ErrorAction SilentlyContinue
            if ($bp -and $bp.ProcessName -eq 'FastClone') { $fcHeld = $true; break }
        }
        if (-not $fcHeld) { return $false }
        Start-Sleep -Milliseconds 250
    }
    return $false
}

function Wait-PortListening {
    param([int]$PortNum, [System.Diagnostics.Process]$Proc, [int]$TimeoutSec = 15)
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSec)
    while ([DateTime]::UtcNow -lt $deadline) {
        if ($Proc.HasExited) { return $false }
        $hit = Get-PortHolders -PortNum $PortNum |
               Where-Object { $_.State -eq 'LISTENING' -and $_.Pid -eq $Proc.Id }
        if ($hit) { return $true }
        Start-Sleep -Milliseconds 200
    }
    return $false
}

# [NFR-03] Timeout diagnostic block: stuck PID + port netstat + client/server log tails +
# last [phase]/[hb]/[wd] marker, so a failure is attributable instead of "(Timeout)".
# Reads $Port / $logDir via PowerShell dynamic scoping.
function Write-FcDiag {
    param([string]$StageTag, [string]$Reason, [int]$TimeoutSec,
          [System.Diagnostics.Process]$Proc, [string]$OutLog, [string]$ErrLog)
    Write-Host "===== TIMEOUT-DIAG begin stage=$StageTag reason=$Reason timeout=${TimeoutSec}s ====="
    if ($Proc) {
        $p = Get-Process -Id $Proc.Id -ErrorAction SilentlyContinue
        Write-Host "  stuck_pid=$($Proc.Id) name=$(if ($p) { $p.ProcessName } else { '<exited>' })"
    }
    Write-Host "  port=$Port netstat:"
    (netstat -ano | Select-String -Pattern ":$Port ") | ForEach-Object { Write-Host "    $($_.Line)" }
    foreach ($f in @($OutLog, $ErrLog)) {
        if ($f -and (Test-Path $f)) {
            Write-Host "  ----- tail $f -----"
            Get-Content -Path $f -Tail 40 -ErrorAction SilentlyContinue | ForEach-Object { Write-Host "    $_" }
        }
    }
    if ($ErrLog -and (Test-Path $ErrLog)) {
        $ph = Get-Content $ErrLog -ErrorAction SilentlyContinue |
              Select-String -Pattern '\[phase\]|\[hb\]|\[wd\]'
        if ($ph) { Write-Host "  client_phase_last=$($ph[-1].Line)" }
        else     { Write-Host "  client_phase_last=<none>" }
    }
    if (Test-Path $logDir) {
        Get-ChildItem -Path $logDir -File -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -match 'server' } | ForEach-Object {
                $t = Get-Content $_.FullName -Tail 20 -ErrorAction SilentlyContinue
                if ($t) {
                    Write-Host "  ----- tail $($_.Name) -----"
                    $t | ForEach-Object { Write-Host "    $_" }
                }
            }
    }
    Write-Host "===== TIMEOUT-DIAG end ====="
}

function Stop-AllFastClone {
    # [FR-03] Only processes THIS script started. The old implementation
    # (`Get-Process FastClone | Stop-Process -Force`) was a name-global kill that can
    # terminate unrelated FastClone processes on a shared build agent (RC-3).
    Stop-OwnedFastClone
    # Defensive, PORT-SCOPED fallback: a process we do not own but that holds OUR port would
    # break the next bind. Kill only that PID and log it (never a name-global sweep).
    foreach ($h in (Get-PortHolders -PortNum $Port)) {
        $p = Get-Process -Id $h.Pid -ErrorAction SilentlyContinue
        if ($p -and $p.ProcessName -eq 'FastClone') {
            Write-Host "[CLEANUP] killing non-owned FastClone pid=$($h.Pid) state=$($h.State) holding port $Port"
            Stop-Process -Id $h.Pid -Force -ErrorAction SilentlyContinue
        }
    }
    if (-not (Wait-PortFree -PortNum $Port -TimeoutSec 15)) {
        Write-Host "[CLEANUP][warn] port $Port still not free after 15s:"
        (netstat -ano | Select-String -Pattern ":$Port ") | ForEach-Object { Write-Host "    $($_.Line)" }
    }
}

function Wait-ExitCode {
    param([System.Diagnostics.Process]$Proc, [int]$TimeoutSec = 30, [string]$StageTag = "wait")
    # [I-2] v2 (R-06 / gate H-1, clamp site 7/7): the design v2 "6 clamp sites" enumeration
    # missed this script (E-1); its worst budget chain sums to exactly 480s, so without the
    # clamp I-2 does not close on ctest #7.
    $eff = [Math]::Min($TimeoutSec, (Get-FcRemainingSec))
    if ($eff -le 0) {
        # NOTE: WaitForExit(0) returns immediately regardless of state, so we must NOT fall
        # through with 0 -- that would silently kill a healthy process and look like a stall.
        throw "GLOBAL-DEADLINE exceeded waiting for pid=$($Proc.Id) stage=$StageTag (budget=${TimeoutSec}s)"
    }
    if ($Proc.HasExited) {
        $Proc.Refresh()
    } elseif (-not $Proc.WaitForExit($eff * 1000)) {
        Write-FcDiag -StageTag $StageTag -Reason "wait-exitcode-timeout" -TimeoutSec $eff `
            -Proc $Proc -OutLog $null -ErrLog $null
        Stop-Process -Id $Proc.Id -Force -ErrorAction SilentlyContinue
        throw "TIMEOUT ${StageTag}: pid=$($Proc.Id) did not exit within ${eff}s (budget ${TimeoutSec}s)"
    }
    $Proc.Refresh()
    if ($null -eq $Proc.ExitCode) {
        throw "Process $($Proc.Id) exited but ExitCode is unavailable"
    }
    return [int]$Proc.ExitCode
}

function Start-FastCloneProcess {
    param(
        [string]$Exe,
        [string[]]$CliArgs,
        [string]$OutLog,
        [string]$ErrLog
    )
    # Drop null/empty fragments (the optional "--bind-loopback" slot is "" when off).
    $CliArgs = @($CliArgs | Where-Object { $null -ne $_ -and $_ -ne "" })
    foreach ($a in $CliArgs) {
        if ($null -eq $a -or $a -eq "") {
            throw "Start-FastCloneProcess: null/empty argument in: $($CliArgs -join ' | ')"
        }
    }
    $env:FASTCLONE_DEBUG = "1"
    $proc = Start-Process -FilePath $Exe -ArgumentList ([string[]]$CliArgs) `
        -RedirectStandardOutput $OutLog -RedirectStandardError $ErrLog `
        -PassThru -NoNewWindow
    # Touch .Handle to cache the native handle now; otherwise .ExitCode is "unavailable"
    # after a self-exiting process (e.g. server --once) terminates (classic PowerShell quirk).
    $null = $proc.Handle
    $null = Register-OwnedProcess -Proc $proc -Tag "server"
    return $proc
}

# [FR-01] Bounded synchronous client invocation.
# Replaces the unbounded `cmd /c` (RC-1): Start-Process gives us the child PID directly, so a
# timeout can kill exactly that process (cmd /c would leave FastClone.exe as an orphan).
function Invoke-FastCloneSync {
    param(
        [string]$Exe,
        [string[]]$CliArgs,
        [string]$OutLog,
        [string]$ErrLog,
        [int]$TimeoutSec = 150,          # tier T0, see design section 3.2
        [string]$StageTag = "client"     # NFR-03: goes into the diagnostic block
    )
    foreach ($a in $CliArgs) {
        if ($null -eq $a -or $a -eq "") {
            throw "Invoke-FastCloneSync: null/empty argument"
        }
    }
    Assert-FcDeadline -Stage "$StageTag/pre"

    # Local fault-injection hooks (AC-02, design section 3.14). Never set on CI; inert by
    # default: the branch is only entered when the environment variable exists. NOTE: the
    # timeout override must run BEFORE the $eff clamp below, otherwise it never takes effect.
    if ($env:FASTCLONE_TEST_HANG_CLIENT -eq '1') {
        $hangSec = 900
        if ($env:FASTCLONE_TEST_HANG_SEC) { $hangSec = [int]$env:FASTCLONE_TEST_HANG_SEC }
        $Exe = "$env:windir\System32\WindowsPowerShell\v1.0\powershell.exe"
        $CliArgs = @("-NoProfile", "-Command", "Start-Sleep -Seconds $hangSec")
        Write-Host "[INJECT] hanging child substituted for stage=$StageTag"
    }
    if ($env:FASTCLONE_TEST_CLIENT_TIMEOUT_SEC) { $TimeoutSec = [int]$env:FASTCLONE_TEST_CLIENT_TIMEOUT_SEC }

    $eff = [Math]::Min($TimeoutSec, (Get-FcRemainingSec))
    if ($eff -le 0) { throw "GLOBAL-DEADLINE exceeded before stage='$StageTag'" }

    $env:FASTCLONE_DEBUG = "1"
    $proc = Start-Process -FilePath $Exe -ArgumentList ([string[]]$CliArgs) `
        -RedirectStandardOutput $OutLog -RedirectStandardError $ErrLog `
        -PassThru -NoNewWindow
    $null = $proc.Handle            # cache the handle so .ExitCode is readable
    $null = Register-OwnedProcess -Proc $proc -Tag $StageTag

    if (-not $proc.WaitForExit($eff * 1000)) {
        Write-FcDiag -StageTag $StageTag -Reason "client-timeout" -TimeoutSec $eff `
            -Proc $proc -OutLog $OutLog -ErrLog $ErrLog
        Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
        throw "TIMEOUT ${StageTag}: FastClone client pid=$($proc.Id) did not exit within ${eff}s"
    }
    return [int]$proc.ExitCode
}

$exe = Resolve-FastCloneExe -Hint $ExePath
$root = Join-Path $env:TEMP "fastclone-once-it-$(Get-Random)"
$logDir = Join-Path $root "logs"
$src = Join-Path $root "src"
$tgt = Join-Path $root "tgt"
New-Item -ItemType Directory -Force -Path $logDir, $src, $tgt | Out-Null

# Small fixture for the quick success path.
1..20 | ForEach-Object {
    Set-Content -Path (Join-Path $src "file$_.txt") -Value ("payload-$_-" + ("x" * 4096))
}
New-Item -ItemType Directory -Force -Path (Join-Path $src "nested\deep") | Out-Null
Set-Content -Path (Join-Path $src "nested\deep\leaf.txt") -Value "nested"

# Larger fixture for the interruptible failure path. A single 512MB file transferred with
# streams=1 + 1KB chunks is syscall-bound and stays in-flight for many seconds, so the
# client can be killed mid-transfer deterministically (even on fast loopback).
$srcBig = Join-Path $root "srcbig"
New-Item -ItemType Directory -Force -Path $srcBig | Out-Null
$bigFile = Join-Path $srcBig "big.bin"
$fsBig = [System.IO.File]::Create($bigFile)
try { $fsBig.SetLength(512MB) } finally { $fsBig.Close() }

$password = "fc-once-pw"
$serverAddr = "127.0.0.1:$Port"
$failures = 0

try {
    Stop-AllFastClone

    Write-Host "[OS-1] success path -> server --once auto-exits 0"
    Assert-FcDeadline -Stage "OS-1"
    if (-not (Wait-PortFree -PortNum $Port -TimeoutSec 15)) {
        Write-Host "[PORT-BUSY] port $Port still held before OS-1 server start:"
        (netstat -ano | Select-String -Pattern ":$Port ") | ForEach-Object { Write-Host "    $($_.Line)" }
    }
    $srv1 = Start-FastCloneProcess -Exe $exe -CliArgs @(
        "server", "--dir", $src, "--password", $password, "--port", "$Port", $bindLoopbackArg, "--once"
    ) -OutLog "$logDir\os1-server.out" -ErrLog "$logDir\os1-server.err"
    if (-not (Wait-PortListening -PortNum $Port -Proc $srv1 -TimeoutSec 15)) {
        Write-FcDiag -StageTag "OS-1/server-not-listening" -Reason "server-bind" -TimeoutSec 15 `
            -Proc $srv1 -OutLog "$logDir\os1-server.out" -ErrLog "$logDir\os1-server.err"
        throw "OS-1: server pid=$($srv1.Id) never reached LISTENING on port $Port within 15s"
    }
    $code1c = Invoke-FastCloneSync -Exe $exe -CliArgs @(
        "client", "--server", $serverAddr, "--target", $tgt, "--password", $password
    ) -OutLog "$logDir\os1-client.out" -ErrLog "$logDir\os1-client.err" -TimeoutSec 150 -StageTag "OS-1"
    if ($code1c -ne 0) { throw "OS-1: client sync expected exit 0, got $code1c" }
    $code1 = Wait-ExitCode -Proc $srv1 -TimeoutSec 30
    if ($code1 -ne 0) { throw "OS-1: server --once expected exit 0, got $code1" }
    Write-Host "  OK server exit=$code1 (auto-exited, no kill)"

    Write-Host "[OS-5] AC-10: after first session terminal, a second client is refused"
    Assert-FcDeadline -Stage "OS-5"
    # The OS-1 server already auto-exited after its single real session (FR-10/FR-11): it is
    # not kept alive for a second session. A new client must therefore fail to connect rather
    # than be served (exit != 0). OS-5b below covers the in-flight rejection path.
    if (-not $srv1.HasExited) { throw "OS-5: server still alive after first session terminal" }
    $tgt5 = Join-Path $root "tgt5"
    New-Item -ItemType Directory -Force -Path $tgt5 | Out-Null
    $cli5 = Start-FastCloneProcess -Exe $exe -CliArgs @(
        "client", "--server", $serverAddr, "--target", $tgt5, "--password", $password,
        "--reconnect-retries", "0"
    ) -OutLog "$logDir\os5-client.out" -ErrLog "$logDir\os5-client.err"
    $code5 = Wait-ExitCode -Proc $cli5 -TimeoutSec 20
    if ($code5 -eq 0) { throw "OS-5: second client unexpectedly succeeded (exit 0); server served a second session" }
    Write-Host "  OK second client refused (no server listening); client exit=$code5"

    Write-Host "[OS-5b] AC-10: while target session is active, second Auth is rejected"
    Assert-FcDeadline -Stage "OS-5b"
    if (-not (Wait-PortFree -PortNum $Port -TimeoutSec 15)) {
        Write-Host "[PORT-BUSY] port $Port still held before OS-5b server start:"
        (netstat -ano | Select-String -Pattern ":$Port ") | ForEach-Object { Write-Host "    $($_.Line)" }
    }
    $srv5b = Start-FastCloneProcess -Exe $exe -CliArgs @(
        "server", "--dir", $srcBig, "--password", $password, "--port", "$Port", $bindLoopbackArg, "--once"
    ) -OutLog "$logDir\os5b-server.out" -ErrLog "$logDir\os5b-server.err"
    if (-not (Wait-PortListening -PortNum $Port -Proc $srv5b -TimeoutSec 15)) {
        Write-FcDiag -StageTag "OS-5b/server-not-listening" -Reason "server-bind" -TimeoutSec 15 `
            -Proc $srv5b -OutLog "$logDir\os5b-server.out" -ErrLog "$logDir\os5b-server.err"
        throw "OS-5b: server pid=$($srv5b.Id) never reached LISTENING on port $Port within 15s"
    }
    $g5b = $null
    $gate5bLog = Join-Path $logDir "os5b-gate.log"
    try {
    # [59 section 3.7 / C7] Deterministic in-flight: a Graceful frame gate holds the
    # FIRST complete FileChunk (type 31) of the 512MB session, so cli5b1 is provably
    # mid-transfer (state-machine Held, not an elapsed-time guess). The single --link
    # pin keeps the client to ONE lane (blockModeActive() needs >=2 healthy lanes,
    # sync_engine_client L2188-2192), so the payload path stays FileChunk frames and
    # StopAfterType 31 is reliable. The deliberate slow flags (--streams 1 --chunk-kb 1)
    # are GONE: the gate, not syscall slowness, now guarantees in-flight, so the 512MB
    # payload moves at full speed inside the SAME 180s completion windows (the CI
    # failure 44714408885 was 512MB / ~2.7MB/s ~= 190s > 180s -- structural, not a
    # hang; the fixture stays 512MB, the business path stays the real big-file path).
    $g5b = Start-FcFrameGateProxy -UpstreamHost '127.0.0.1' -UpstreamPort $Port `
        -StopAfterType 31 -CapBytes 64MB -FixtureBytes 512MB `
        -ReadTimeoutMs 60000 -CloseMode Graceful -LogPath $gate5bLog
    $tgt5b1 = Join-Path $root "tgt5b1"
    New-Item -ItemType Directory -Force -Path $tgt5b1 | Out-Null
    $gate5bAddr = "127.0.0.1:$($g5b.Port)"
    $cli5b1 = Start-FastCloneProcess -Exe $exe -CliArgs @(
        "client", "--server", $gate5bAddr, "--link", "127.0.0.1=$gate5bAddr",
        "--target", $tgt5b1, "--password", $password,
        "--reconnect-retries", "0"
    ) -OutLog "$logDir\os5b-cli1.out" -ErrLog "$logDir\os5b-cli1.err"
    # Readiness = the REAL Held state (bounded by min(60s, global remaining); NOTE
    # WaitForState takes MILLISECONDS -- 60000 = 60s, matching the 15000ms Finished wait
    # below) plus the client provably still alive -- no Start-Sleep guess. Held itself
    # is the in-flight proof: the server has sent (and the gate has verified) a complete
    # FileChunk of the 512MB payload while the transfer is frozen.
    $held5b = $g5b.WaitForState([FcGate.GateState]::Held, [Math]::Min(60000, ((Get-FcRemainingSec) * 1000)))
    $cli5b1.Refresh()
    if (-not $held5b -or $cli5b1.HasExited) {
        Write-FcDiag -StageTag "OS-5b/gate-not-held" -Reason "FRAME-GATE-NO-FILECHUNK" -TimeoutSec 60 `
            -Proc $cli5b1 -OutLog "$logDir\os5b-cli1.out" -ErrLog "$logDir\os5b-cli1.err"
        Write-Host ("  [OS-5b] gate snapshot: " + $g5b.Snapshot())
        if (Test-Path $gate5bLog) {
            Write-Host "  [OS-5b] gate log tail:"
            Get-Content $gate5bLog -Tail 12 | ForEach-Object { Write-Host ("    " + $_) }
        }
        throw "OS-5b: gate did not reach Held within window (state=$($g5b.State)) cli1 exited=$($cli5b1.HasExited)"
    }
    # Second client: WRONG password, DIRECT to the server (not through the gate). The
    # once server is already serving cli5b1's held session, so this handshake must be
    # rejected with AuthFail (client_handshake "Server authentication rejected") and a
    # fatal exit 1 -- deterministic, no dependence on "single slot" wording. The server
    # must NOT finish on this rejected connection: the single-session evidence is the
    # exactly-one [mp] conn_accept check below (a failed handshake never conn_accepts).
    $tgt5b2 = Join-Path $root "tgt5b2"
    New-Item -ItemType Directory -Force -Path $tgt5b2 | Out-Null
    $cli5b2 = Start-FastCloneProcess -Exe $exe -CliArgs @(
        "client", "--server", $serverAddr, "--target", $tgt5b2, "--password", "wrong-password",
        "--reconnect-retries", "0"
    ) -OutLog "$logDir\os5b-cli2.out" -ErrLog "$logDir\os5b-cli2.err"
    $code5b2 = Wait-ExitCode -Proc $cli5b2 -TimeoutSec 10 -StageTag "OS-5b-cli2"
    if ($code5b2 -ne 1) {
        throw "OS-5b: second client expected fatal exit 1 (AuthFail), got $code5b2"
    }
    $err5b2 = Get-Content "$logDir\os5b-cli2.err" -Raw -ErrorAction SilentlyContinue
    if ($err5b2 -notmatch "authentication rejected") {
        throw "OS-5b: second client stderr missing AuthFail text: $err5b2"
    }
    $accept5bCount = @(Select-String -Path "$logDir\os5b-server.out" -Pattern '\[mp\] conn_accept' `
        -ErrorAction SilentlyContinue).Count
    if ($accept5bCount -ne 1) {
        throw "OS-5b: expected exactly 1 [mp] conn_accept (the gate upstream only), got $accept5bCount"
    }
    # Release the hold: cli5b1 must complete (exit 0) and the once-server must exit 0,
    # both inside the ORIGINAL 180s windows (512MB at full loopback speed; no budget
    # change, no extended waits).
    $g5b.Resume()
    $code5b1 = Wait-ExitCode -Proc $cli5b1 -TimeoutSec 180 -StageTag "OS-5b-cli1"
    if ($code5b1 -ne 0) { throw "OS-5b: first client expected exit 0, got $code5b1" }
    $code5b = Wait-ExitCode -Proc $srv5b -TimeoutSec 180 -StageTag "OS-5b-server"
    if ($code5b -ne 0) { throw "OS-5b: server --once expected exit 0, got $code5b" }
    # Gate lifecycle evidence: a complete SyncDone was forwarded, the drain reached EOF,
    # and the gate reached Finished (bounded log-content wait; the FINISHED line is
    # written by the gate's downlink thread right after the state flips).
    $null = Wait-FcCondition -Condition {
        $t = Get-FcLogFullText -Path $gate5bLog
        ($null -ne $t) -and (Test-FcGateFinishedOrder -LogText $t) -and
        ($t -match '(?m)^FRAME_GATE_FINISHED sync_done_forwarded=1 eof=1')
    } -TimeoutSec 15 -PollMs 200
    $gate5bText = Get-FcLogFullText -Path $gate5bLog
    if (-not (Test-FcGateFinishedOrder -LogText $gate5bText)) {
        throw "OS-5b: gate log lacks SYNCDONE_FORWARDED-before-FINISHED order evidence"
    }
    if ($gate5bText -notmatch '(?m)^FRAME_GATE_FINISHED sync_done_forwarded=1 eof=1') {
        throw "OS-5b: gate log lacks FRAME_GATE_FINISHED sync_done_forwarded=1 eof=1"
    }
    if (-not $g5b.WaitForState([FcGate.GateState]::Finished, 15000)) {
        throw "OS-5b: gate did not reach Finished within 15s (state=$($g5b.State))"
    }
    # Server-side protocol completion for the held session.
    if (-not (Select-String -Path "$logDir\os5b-server.out" -Pattern '\[mp\] conn_done' -Quiet)) {
        throw "OS-5b: server log lacks [mp] conn_done for the held session"
    }
    # Byte-for-byte: the 512MB target equals the source (length + SHA256, three-way with
    # the fixture itself).
    $tgt5b1File = Join-Path $tgt5b1 "big.bin"
    if (-not (Test-Path -LiteralPath $tgt5b1File)) {
        throw "OS-5b: target file missing after sync: $tgt5b1File"
    }
    $len5b1 = (Get-Item -LiteralPath $tgt5b1File).Length
    if ($len5b1 -ne 512MB) {
        throw "OS-5b: target length $len5b1 != 536870912"
    }
    $src5bHash = (Get-FileHash -LiteralPath $bigFile -Algorithm SHA256).Hash
    $tgt5bHash = (Get-FileHash -LiteralPath $tgt5b1File -Algorithm SHA256).Hash
    if ($src5bHash -ne $tgt5bHash) {
        throw "OS-5b: SHA256 mismatch src=$src5bHash tgt=$tgt5bHash"
    }
    Write-Host "  OK second Auth rejected (exit=$code5b2); held session completed; server exit=$code5b; SHA256 equal"
    }
    finally {
        if ($null -ne $g5b) { $g5b.Dispose() }
    }

    Write-Host "[OS-2] failure path -> client aborted mid-transfer -> server exit 5"
    Assert-FcDeadline -Stage "OS-2"
    if (-not (Wait-PortFree -PortNum $Port -TimeoutSec 15)) {
        Write-Host "[PORT-BUSY] port $Port still held before OS-2 server start:"
        (netstat -ano | Select-String -Pattern ":$Port ") | ForEach-Object { Write-Host "    $($_.Line)" }
    }
    $srv2 = Start-FastCloneProcess -Exe $exe -CliArgs @(
        "server", "--dir", $srcBig, "--password", $password, "--port", "$Port", $bindLoopbackArg, "--once"
    ) -OutLog "$logDir\os2-server.out" -ErrLog "$logDir\os2-server.err"
    if (-not (Wait-PortListening -PortNum $Port -Proc $srv2 -TimeoutSec 15)) {
        Write-FcDiag -StageTag "OS-2/server-not-listening" -Reason "server-bind" -TimeoutSec 15 `
            -Proc $srv2 -OutLog "$logDir\os2-server.out" -ErrLog "$logDir\os2-server.err"
        throw "OS-2: server pid=$($srv2.Id) never reached LISTENING on port $Port within 15s"
    }
    $tgt2 = Join-Path $root "tgt2"
    New-Item -ItemType Directory -Force -Path $tgt2 | Out-Null
    $cli2 = Start-FastCloneProcess -Exe $exe -CliArgs @(
        "client", "--server", $serverAddr, "--target", $tgt2, "--password", $password,
        "--reconnect-retries", "0", "--streams", "1", "--chunk-kb", "1"
    ) -OutLog "$logDir\os2-client.out" -ErrLog "$logDir\os2-client.err"
    Start-Sleep -Seconds 2
    if ($cli2.HasExited) {
        throw "OS-2: client finished before it could be interrupted; increase fixture size"
    }
    Stop-Process -Id $cli2.Id -Force -ErrorAction SilentlyContinue
    $code2 = Wait-ExitCode -Proc $srv2 -TimeoutSec 30
    if ($code2 -ne 5) { throw "OS-2: server --once expected exit 5, got $code2" }
    Write-Host "  OK server exit=$code2"

    Write-Host "[OS-3] usage errors -> exit 1"
    Assert-FcDeadline -Stage "OS-3"
    $code3a = Invoke-FastCloneSync -Exe $exe -CliArgs @(
        "client", "--server", $serverAddr, "--target", $tgt, "--password", $password, "--once"
    ) -OutLog "$logDir\os3a.out" -ErrLog "$logDir\os3a.err" -TimeoutSec 150 -StageTag "OS-3a"
    if ($code3a -ne 1) { throw "OS-3: client --once expected exit 1, got $code3a" }
    $code3b = Invoke-FastCloneSync -Exe $exe -CliArgs @(
        "server", "--dir", $src, "--password", $password, "--once", "--enable-hash-memcache"
    ) -OutLog "$logDir\os3b.out" -ErrLog "$logDir\os3b.err" -TimeoutSec 150 -StageTag "OS-3b"
    if ($code3b -ne 1) { throw "OS-3: --once + --enable-hash-memcache expected exit 1, got $code3b" }
    Write-Host "  OK client--once=$code3a  mutually-exclusive=$code3b"

    Write-Host "[OS-4] pre-handshake probe does not exit the server, real client still completes 0"
    Assert-FcDeadline -Stage "OS-4"
    $tgt4 = Join-Path $root "tgt4"
    New-Item -ItemType Directory -Force -Path $tgt4 | Out-Null
    if (-not (Wait-PortFree -PortNum $Port -TimeoutSec 15)) {
        Write-Host "[PORT-BUSY] port $Port still held before OS-4 server start:"
        (netstat -ano | Select-String -Pattern ":$Port ") | ForEach-Object { Write-Host "    $($_.Line)" }
    }
    $srv4 = Start-FastCloneProcess -Exe $exe -CliArgs @(
        "server", "--dir", $src, "--password", $password, "--port", "$Port", $bindLoopbackArg, "--once"
    ) -OutLog "$logDir\os4-server.out" -ErrLog "$logDir\os4-server.err"
    if (-not (Wait-PortListening -PortNum $Port -Proc $srv4 -TimeoutSec 15)) {
        Write-FcDiag -StageTag "OS-4/server-not-listening" -Reason "server-bind" -TimeoutSec 15 `
            -Proc $srv4 -OutLog "$logDir\os4-server.out" -ErrLog "$logDir\os4-server.err"
        throw "OS-4: server pid=$($srv4.Id) never reached LISTENING on port $Port within 15s"
    }
    # Probe: connect then immediately close without sending any bytes (session == null).
    $probe = New-Object System.Net.Sockets.TcpClient
    $probe.Connect("127.0.0.1", $Port)
    Start-Sleep -Milliseconds 200
    $probe.Close()
    # Bounded wait (was `Start-Sleep -Seconds 1`): give the server up to 1s to wrongly react
    # to the probe close; polling makes the failure surface earlier without weakening the
    # "server must survive the full window" semantics.
    $probeGrace = [DateTime]::UtcNow.AddSeconds(1)
    while ([DateTime]::UtcNow -lt $probeGrace) {
        if ($srv4.HasExited) { break }
        Start-Sleep -Milliseconds 100
    }
    if ($srv4.HasExited) { throw "OS-4: server exited on a pre-handshake probe (should stay up)" }
    $code4c = Invoke-FastCloneSync -Exe $exe -CliArgs @(
        "client", "--server", $serverAddr, "--target", $tgt4, "--password", $password
    ) -OutLog "$logDir\os4-client.out" -ErrLog "$logDir\os4-client.err" -TimeoutSec 150 -StageTag "OS-4"
    if ($code4c -ne 0) { throw "OS-4: client sync expected exit 0, got $code4c" }
    $code4 = Wait-ExitCode -Proc $srv4 -TimeoutSec 30
    if ($code4 -ne 0) { throw "OS-4: server --once expected exit 0 after real session, got $code4" }
    Write-Host "  OK probe ignored; server exit=$code4 after real session"

    Write-Host "All oneshot-server integration tests passed. Logs: $logDir"
    Stop-AllFastClone
    exit 0
}
catch {
    Write-Host "FAILED: $_" -ForegroundColor Red
    Write-Host "Logs: $logDir"
    $failures = 1
}
finally {
    Stop-AllFastClone
    # [r61 / G-AC8] Zero-delete retention (mirrors once_multi's r19 rule): this script
    # executes no deletion of any kind -- the unique run tree (fastclone-once-it-<random>
    # directly under $env:TEMP, this run's own) is retained and its path printed so the
    # CI failure-artifact upload can collect the per-case logs.
    Write-Host "RETAINED run temp root (no deletion by design; logs: $logDir): $root"
    if ($failures -ne 0) { exit 1 }
}
