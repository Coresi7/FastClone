# FastClone --once-multi (multi-session idle-grace server) integration tests (Windows).
# Usage: .\tests\once_multi_integration.ps1 [-ExePath path\to\FastClone.exe] [-Port 27894]
# Creates small/large temp fixtures; always kills FastClone processes on exit.
#
# Covers (design §9.2 / requirements AC-08..16):
#   OM-1 sequential   -> server stays up between two real sessions, exits 0 after idle-grace
#                        (AC-08 / AC-13 / AC-15)
#   OM-2 concurrent   -> two real sessions served at once, exit 0 after both finish (AC-09/AC-13)
#   OM-3 grace race   -> a new real session before grace expiry resets the timer (AC-10)
#   OM-4 failure agg  -> any aborted real session -> sticky exit 5, even after a later clean one
#                        (AC-14 / B5)
#   OM-5 probe/never  -> (a) probes during grace do not reset it; (b) probe-only never idle-exits
#                        (AC-11 / AC-12)

param(
    [string]$ExePath = "",
    [int]$Port = 27894,
    # Test-only: when set (ctest passes -BindLoopback on), the server binds ::1 instead of ::
    # so external LAN/WAN traffic cannot flood its single session (OM-4 flaky root cause).
    [switch]$BindLoopback
)

$ErrorActionPreference = "Stop"

# Precomputed server arg fragment: empty in production, "--bind-loopback" in test mode.
# Spliced into every Start-FastCloneProcess/Invoke-FastCloneSync server CliArgs below.
$bindLoopbackArg = if ($BindLoopback) { "--bind-loopback" } else { "" }

# ---------------------------------------------------------------------------
# Task ci-integration-test-timeout: global bounded-execution skeleton (design sections
# 3.4/3.5/3.12/3.13). Canonical implementation; the same block is copied into every
# integration script (N3: no shared dot-source module).
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
    # [I-2] v2 (R-06): Wait-ExitCode is the LARGEST single wait budget in these scripts
    # (OM-2 uses 300s for the concurrent 512MB clients). Without clamping it to the global
    # deadline, I-2 does not close on #6: 300s (OM-2) + 150s (a T0 stall) = 450s, with no
    # room left for the diagnostic dump.
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
    # Drop null/empty fragments (e.g. the optional "--bind-loopback" slot is "" when off).
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
    # Cache the native handle so .ExitCode survives a self-exiting process (PowerShell quirk).
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

function Resolve-FastCloneExe {
    param([string]$Hint)
    if ($Hint -and (Test-Path $Hint)) { return (Resolve-Path $Hint).Path }
    $candidates = @(
        "$PSScriptRoot\..\FastClone\x64\Release\FastClone.exe",
        "$PSScriptRoot\..\x64\Release\FastClone.exe",
        "$PSScriptRoot\..\build\Release\FastClone.exe",
        "$PSScriptRoot\..\build\FastClone.exe"
    )
    foreach ($c in $candidates) {
        if (Test-Path $c) { return (Resolve-Path $c).Path }
    }
    throw "FastClone.exe not found; pass -ExePath"
}

# Open a bare TCP connection and immediately close it (pre-handshake probe, session == null).
function Send-Probe {
    param([int]$ProbePort)
    $probe = New-Object System.Net.Sockets.TcpClient
    $probe.Connect("127.0.0.1", $ProbePort)
    Start-Sleep -Milliseconds 150
    $probe.Close()
}

$exe = Resolve-FastCloneExe -Hint $ExePath
$root = Join-Path $env:TEMP "fastclone-onemulti-it-$(Get-Random)"
$logDir = Join-Path $root "logs"
$src = Join-Path $root "src"
New-Item -ItemType Directory -Force -Path $logDir, $src | Out-Null

# Small fixture for quick real sessions.
1..15 | ForEach-Object {
    Set-Content -Path (Join-Path $src "file$_.txt") -Value ("payload-$_-" + ("x" * 4096))
}
New-Item -ItemType Directory -Force -Path (Join-Path $src "nested\deep") | Out-Null
Set-Content -Path (Join-Path $src "nested\deep\leaf.txt") -Value "nested"

# Large fixture for interruptible / long-running sessions (streams=1 + 1KB chunks is
# syscall-bound and stays in-flight for many seconds even over loopback).
$srcBig = Join-Path $root "srcbig"
New-Item -ItemType Directory -Force -Path $srcBig | Out-Null
$bigFile = Join-Path $srcBig "big.bin"
$fsBig = [System.IO.File]::Create($bigFile)
try { $fsBig.SetLength(512MB) } finally { $fsBig.Close() }

$password = "fc-om-pw"
$serverAddr = "127.0.0.1:$Port"
$grace = "3s"
$failures = 0

try {
    Stop-AllFastClone

    Write-Host "[OM-1] sequential: server survives between two real sessions, exits 0 after grace"
    Assert-FcDeadline -Stage "OM-1"
    if (-not (Wait-PortFree -PortNum $Port -TimeoutSec 15)) {
        Write-Host "[PORT-BUSY] port $Port still held before OM-1 server start:"
        (netstat -ano | Select-String -Pattern ":$Port ") | ForEach-Object { Write-Host "    $($_.Line)" }
    }
    $srv1 = Start-FastCloneProcess -Exe $exe -CliArgs @(
        "server", "--dir", $src, "--password", $password, "--port", "$Port", $bindLoopbackArg,
        "--once-multi", "--once-idle-grace", $grace
    ) -OutLog "$logDir\om1-server.out" -ErrLog "$logDir\om1-server.err"
    if (-not (Wait-PortListening -PortNum $Port -Proc $srv1 -TimeoutSec 15)) {
        Write-FcDiag -StageTag "OM-1/server-not-listening" -Reason "server-bind" -TimeoutSec 15 `
            -Proc $srv1 -OutLog "$logDir\om1-server.out" -ErrLog "$logDir\om1-server.err"
        throw "OM-1: server pid=$($srv1.Id) never reached LISTENING on port $Port within 15s"
    }
    $tgt1a = Join-Path $root "tgt1a"; New-Item -ItemType Directory -Force -Path $tgt1a | Out-Null
    $c1a = Invoke-FastCloneSync -Exe $exe -CliArgs @(
        "client", "--server", $serverAddr, "--target", $tgt1a, "--password", $password
    ) -OutLog "$logDir\om1-c1a.out" -ErrLog "$logDir\om1-c1a.err" -TimeoutSec 150 -StageTag "OM-1"
    if ($c1a -ne 0) { throw "OM-1: first client expected exit 0, got $c1a" }
    if ($srv1.HasExited) { throw "OM-1: server exited after first session (should survive within grace)" }
    $tgt1b = Join-Path $root "tgt1b"; New-Item -ItemType Directory -Force -Path $tgt1b | Out-Null
    $c1b = Invoke-FastCloneSync -Exe $exe -CliArgs @(
        "client", "--server", $serverAddr, "--target", $tgt1b, "--password", $password
    ) -OutLog "$logDir\om1-c1b.out" -ErrLog "$logDir\om1-c1b.err" -TimeoutSec 150 -StageTag "OM-1"
    if ($c1b -ne 0) { throw "OM-1: second client expected exit 0, got $c1b" }
    $code1 = Wait-ExitCode -Proc $srv1 -TimeoutSec 30
    if ($code1 -ne 0) { throw "OM-1: server expected exit 0 after grace, got $code1" }
    $done1 = (Select-String -Path "$logDir\om1-server.out" -Pattern "\[mp\] conn_done").Count
    if ($done1 -lt 2) { throw "OM-1: expected >=2 completed sessions in server log, got $done1" }
    Write-Host "  OK two sequential sessions served; server exit=$code1"

    Write-Host "[OM-2] concurrent: two real sessions served at once, exit 0 after both finish"
    Assert-FcDeadline -Stage "OM-2"
    if (-not (Wait-PortFree -PortNum $Port -TimeoutSec 15)) {
        Write-Host "[PORT-BUSY] port $Port still held before OM-2 server start:"
        (netstat -ano | Select-String -Pattern ":$Port ") | ForEach-Object { Write-Host "    $($_.Line)" }
    }
    $srv2 = Start-FastCloneProcess -Exe $exe -CliArgs @(
        "server", "--dir", $srcBig, "--password", $password, "--port", "$Port", $bindLoopbackArg,
        "--once-multi", "--once-idle-grace", $grace
    ) -OutLog "$logDir\om2-server.out" -ErrLog "$logDir\om2-server.err"
    if (-not (Wait-PortListening -PortNum $Port -Proc $srv2 -TimeoutSec 15)) {
        Write-FcDiag -StageTag "OM-2/server-not-listening" -Reason "server-bind" -TimeoutSec 15 `
            -Proc $srv2 -OutLog "$logDir\om2-server.out" -ErrLog "$logDir\om2-server.err"
        throw "OM-2: server pid=$($srv2.Id) never reached LISTENING on port $Port within 15s"
    }
    $tgt2a = Join-Path $root "tgt2a"; New-Item -ItemType Directory -Force -Path $tgt2a | Out-Null
    $tgt2b = Join-Path $root "tgt2b"; New-Item -ItemType Directory -Force -Path $tgt2b | Out-Null
    $c2a = Start-FastCloneProcess -Exe $exe -CliArgs @(
        "client", "--server", $serverAddr, "--target", $tgt2a, "--password", $password,
        "--streams", "1", "--chunk-kb", "1"
    ) -OutLog "$logDir\om2-c2a.out" -ErrLog "$logDir\om2-c2a.err"
    $c2b = Start-FastCloneProcess -Exe $exe -CliArgs @(
        "client", "--server", $serverAddr, "--target", $tgt2b, "--password", $password,
        "--streams", "1", "--chunk-kb", "1"
    ) -OutLog "$logDir\om2-c2b.out" -ErrLog "$logDir\om2-c2b.err"
    # Give both time to connect concurrently, then verify the server is still up (both active).
    Start-Sleep -Seconds 3
    if ($srv2.HasExited) { throw "OM-2: server exited while two sessions were active" }
    $code2a = Wait-ExitCode -Proc $c2a -TimeoutSec 300
    $code2b = Wait-ExitCode -Proc $c2b -TimeoutSec 300
    if ($code2a -ne 0) { throw "OM-2: client A expected exit 0, got $code2a" }
    if ($code2b -ne 0) { throw "OM-2: client B expected exit 0, got $code2b" }
    $code2 = Wait-ExitCode -Proc $srv2 -TimeoutSec 30
    if ($code2 -ne 0) { throw "OM-2: server expected exit 0 after both finished, got $code2" }
    Write-Host "  OK two concurrent sessions served; server exit=$code2"

    Write-Host "[OM-3] grace race: a new real session before grace expiry resets the timer"
    Assert-FcDeadline -Stage "OM-3"
    $longGrace = "5s"
    if (-not (Wait-PortFree -PortNum $Port -TimeoutSec 15)) {
        Write-Host "[PORT-BUSY] port $Port still held before OM-3 server start:"
        (netstat -ano | Select-String -Pattern ":$Port ") | ForEach-Object { Write-Host "    $($_.Line)" }
    }
    $srv3 = Start-FastCloneProcess -Exe $exe -CliArgs @(
        "server", "--dir", $srcBig, "--password", $password, "--port", "$Port", $bindLoopbackArg,
        "--once-multi", "--once-idle-grace", $longGrace
    ) -OutLog "$logDir\om3-server.out" -ErrLog "$logDir\om3-server.err"
    if (-not (Wait-PortListening -PortNum $Port -Proc $srv3 -TimeoutSec 15)) {
        Write-FcDiag -StageTag "OM-3/server-not-listening" -Reason "server-bind" -TimeoutSec 15 `
            -Proc $srv3 -OutLog "$logDir\om3-server.out" -ErrLog "$logDir\om3-server.err"
        throw "OM-3: server pid=$($srv3.Id) never reached LISTENING on port $Port within 15s"
    }
    # First (quick) session completes and arms the grace timer.
    $tgt3a = Join-Path $root "tgt3a"; New-Item -ItemType Directory -Force -Path $tgt3a | Out-Null
    $c3a = Invoke-FastCloneSync -Exe $exe -CliArgs @(
        "client", "--server", $serverAddr, "--target", $tgt3a, "--password", $password
    ) -OutLog "$logDir\om3-c3a.out" -ErrLog "$logDir\om3-c3a.err" -TimeoutSec 150 -StageTag "OM-3"
    if ($c3a -ne 0) { throw "OM-3: first client expected exit 0, got $c3a" }
    $armedAt = Get-Date
    # Let the grace timer actually arm (a couple of 200ms ticks with zero active sessions) so the
    # second session below provably *resets* an armed timer rather than racing in before it arms.
    Start-Sleep -Milliseconds 1500
    if ($srv3.HasExited) { throw "OM-3: server exited during initial grace window (grace too short?)" }
    # Still well before the 5s grace expires, start a long second session so the timer must reset.
    $tgt3b = Join-Path $root "tgt3b"; New-Item -ItemType Directory -Force -Path $tgt3b | Out-Null
    $c3b = Start-FastCloneProcess -Exe $exe -CliArgs @(
        "client", "--server", $serverAddr, "--target", $tgt3b, "--password", $password,
        "--streams", "1", "--chunk-kb", "1"
    ) -OutLog "$logDir\om3-c3b.out" -ErrLog "$logDir\om3-c3b.err"
    Start-Sleep -Seconds 2  # second session now active (well inside the first grace window)
    if ($srv3.HasExited) { throw "OM-3: server exited despite a new session arriving during grace" }
    # Wait until safely past "first-session-completed + grace": server must STILL be alive,
    # proving the grace timer was reset by the second session (AC-10).
    while (((Get-Date) - $armedAt).TotalSeconds -lt 7) { Start-Sleep -Milliseconds 200 }
    if ($srv3.HasExited) { throw "OM-3: server exited at first-grace deadline; timer was not reset" }
    $code3b = Wait-ExitCode -Proc $c3b -TimeoutSec 300
    if ($code3b -ne 0) { throw "OM-3: second client expected exit 0, got $code3b" }
    $code3 = Wait-ExitCode -Proc $srv3 -TimeoutSec 30
    if ($code3 -ne 0) { throw "OM-3: server expected exit 0 after last session + grace, got $code3" }
    $armed3 = (Select-String -Path "$logDir\om3-server.out" -Pattern "idle_grace_armed").Count
    $reset3 = (Select-String -Path "$logDir\om3-server.out" -Pattern "idle_grace_reset").Count
    if ($armed3 -lt 1) { throw "OM-3: expected the grace to arm after the first session, found none" }
    if ($reset3 -lt 1) { throw "OM-3: expected an idle_grace_reset log entry, found none" }
    Write-Host "  OK grace armed then reset by second session; server exit=$code3"

    Write-Host "[OM-4] failure aggregation: aborted session -> sticky exit 5 (clean later session does not mask it)"
    Assert-FcDeadline -Stage "OM-4"
    if (-not (Wait-PortFree -PortNum $Port -TimeoutSec 15)) {
        Write-Host "[PORT-BUSY] port $Port still held before OM-4 server start:"
        (netstat -ano | Select-String -Pattern ":$Port ") | ForEach-Object { Write-Host "    $($_.Line)" }
    }
    $srv4 = Start-FastCloneProcess -Exe $exe -CliArgs @(
        "server", "--dir", $srcBig, "--password", $password, "--port", "$Port", $bindLoopbackArg,
        "--once-multi", "--once-idle-grace", $grace
    ) -OutLog "$logDir\om4-server.out" -ErrLog "$logDir\om4-server.err"
    if (-not (Wait-PortListening -PortNum $Port -Proc $srv4 -TimeoutSec 15)) {
        Write-FcDiag -StageTag "OM-4/server-not-listening" -Reason "server-bind" -TimeoutSec 15 `
            -Proc $srv4 -OutLog "$logDir\om4-server.out" -ErrLog "$logDir\om4-server.err"
        throw "OM-4: server pid=$($srv4.Id) never reached LISTENING on port $Port within 15s"
    }
    $tgt4a = Join-Path $root "tgt4a"; New-Item -ItemType Directory -Force -Path $tgt4a | Out-Null
    $c4a = Start-FastCloneProcess -Exe $exe -CliArgs @(
        "client", "--server", $serverAddr, "--target", $tgt4a, "--password", $password,
        "--reconnect-retries", "0", "--streams", "1", "--chunk-kb", "1"
    ) -OutLog "$logDir\om4-c4a.out" -ErrLog "$logDir\om4-c4a.err"
    Start-Sleep -Seconds 2
    if ($c4a.HasExited) { throw "OM-4: failing client finished before it could be interrupted" }
    Stop-Process -Id $c4a.Id -Force -ErrorAction SilentlyContinue  # abort mid-transfer -> hadError
    # A later CLEAN session must NOT clear the sticky failure (B5).
    $tgt4b = Join-Path $root "tgt4b"; New-Item -ItemType Directory -Force -Path $tgt4b | Out-Null
    $c4b = Invoke-FastCloneSync -Exe $exe -CliArgs @(
        "client", "--server", $serverAddr, "--target", $tgt4b, "--password", $password
    ) -OutLog "$logDir\om4-c4b.out" -ErrLog "$logDir\om4-c4b.err" -TimeoutSec 150 -StageTag "OM-4"
    if ($c4b -ne 0) { throw "OM-4: clean second client expected exit 0, got $c4b" }
    $code4 = Wait-ExitCode -Proc $srv4 -TimeoutSec 30
    if ($code4 -ne 5) { throw "OM-4: server expected sticky exit 5, got $code4" }
    Write-Host "  OK aborted session forced sticky exit=$code4"

    Write-Host "[OM-5a] probes during grace do not reset it; server exits on schedule"
    Assert-FcDeadline -Stage "OM-5a"
    if (-not (Wait-PortFree -PortNum $Port -TimeoutSec 15)) {
        Write-Host "[PORT-BUSY] port $Port still held before OM-5a server start:"
        (netstat -ano | Select-String -Pattern ":$Port ") | ForEach-Object { Write-Host "    $($_.Line)" }
    }
    $srv5 = Start-FastCloneProcess -Exe $exe -CliArgs @(
        "server", "--dir", $src, "--password", $password, "--port", "$Port", $bindLoopbackArg,
        "--once-multi", "--once-idle-grace", "4s"
    ) -OutLog "$logDir\om5a-server.out" -ErrLog "$logDir\om5a-server.err"
    if (-not (Wait-PortListening -PortNum $Port -Proc $srv5 -TimeoutSec 15)) {
        Write-FcDiag -StageTag "OM-5a/server-not-listening" -Reason "server-bind" -TimeoutSec 15 `
            -Proc $srv5 -OutLog "$logDir\om5a-server.out" -ErrLog "$logDir\om5a-server.err"
        throw "OM-5a: server pid=$($srv5.Id) never reached LISTENING on port $Port within 15s"
    }
    $tgt5 = Join-Path $root "tgt5"; New-Item -ItemType Directory -Force -Path $tgt5 | Out-Null
    $c5 = Invoke-FastCloneSync -Exe $exe -CliArgs @(
        "client", "--server", $serverAddr, "--target", $tgt5, "--password", $password
    ) -OutLog "$logDir\om5a-c5.out" -ErrLog "$logDir\om5a-c5.err" `
        -TimeoutSec 150 -StageTag "OM-5a"
    if ($c5 -ne 0) { throw "OM-5a: real client expected exit 0, got $c5" }
    # Inject probes during the grace window; they must not re-arm/extend it.
    Send-Probe -ProbePort $Port
    Start-Sleep -Milliseconds 800
    Send-Probe -ProbePort $Port
    $code5 = Wait-ExitCode -Proc $srv5 -TimeoutSec 30
    if ($code5 -ne 0) { throw "OM-5a: server expected exit 0 (probes ignored), got $code5" }
    $resetA = (Select-String -Path "$logDir\om5a-server.out" -Pattern "idle_grace_reset").Count
    if ($resetA -ne 0) { throw "OM-5a: probe wrongly reset the grace ($resetA reset events)" }
    Write-Host "  OK probes did not reset grace; server exit=$code5"

    Write-Host "[OM-5b] probe-only (no real session): server never idle-exits"
    Assert-FcDeadline -Stage "OM-5b"
    if (-not (Wait-PortFree -PortNum $Port -TimeoutSec 15)) {
        Write-Host "[PORT-BUSY] port $Port still held before OM-5b server start:"
        (netstat -ano | Select-String -Pattern ":$Port ") | ForEach-Object { Write-Host "    $($_.Line)" }
    }
    $srv6 = Start-FastCloneProcess -Exe $exe -CliArgs @(
        "server", "--dir", $src, "--password", $password, "--port", "$Port", $bindLoopbackArg,
        "--once-multi", "--once-idle-grace", "2s"
    ) -OutLog "$logDir\om5b-server.out" -ErrLog "$logDir\om5b-server.err"
    if (-not (Wait-PortListening -PortNum $Port -Proc $srv6 -TimeoutSec 15)) {
        Write-FcDiag -StageTag "OM-5b/server-not-listening" -Reason "server-bind" -TimeoutSec 15 `
            -Proc $srv6 -OutLog "$logDir\om5b-server.out" -ErrLog "$logDir\om5b-server.err"
        throw "OM-5b: server pid=$($srv6.Id) never reached LISTENING on port $Port within 15s"
    }
    Send-Probe -ProbePort $Port
    Start-Sleep -Milliseconds 500
    Send-Probe -ProbePort $Port
    # Wait well beyond the grace; with no real session ever served the server must stay up.
    Start-Sleep -Seconds 6
    if ($srv6.HasExited) { throw "OM-5b: server idle-exited without ever serving a real session" }
    Stop-Process -Id $srv6.Id -Force -ErrorAction SilentlyContinue
    Write-Host "  OK probe-only server stayed alive (killed for cleanup)"

    Write-Host "All once-multi integration tests passed. Logs: $logDir"
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
    if ($failures -ne 0) { exit 1 }
}
