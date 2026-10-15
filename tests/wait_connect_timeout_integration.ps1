# FastClone --wait-connect-timeout (server first-connect wait) integration tests (Windows).
# Usage: .\tests\wait_connect_timeout_integration.ps1 [-ExePath path\to\FastClone.exe] [-Port 27895]
# Creates small/large temp fixtures; always kills FastClone processes on exit.
#
# Covers (design §5 V-07..V-17 / requirements AC-07..14):
#   WCT-1 timeout/no-conn  -> --once, no client at all -> exit 6; log has threshold + no_valid_connection
#                             (AC-07 / AC-13)
#   WCT-2 timeout/probes    -> --once, only TCP probes (no handshake) -> exit 6 (AC-08)
#   WCT-3 first-conn clean   -> --once, real client connects in time -> wait-connect disabled, exit 0;
#                              log has first_valid_connection and NO wait_connect_timeout (AC-09 / AC-10a)
#   WCT-3c first-conn fail   -> --once, real session aborted mid-transfer -> exit 5 (AC-10b)
#   WCT-4 once-multi disabled -> --once-multi: after first conn, exit is decided by idle-grace, never 6
#                              and never logs wait_connect_timeout (AC-11)
#   WCT-5 once-multi probes   -> --once-multi, only probes -> exit 6 (AC-08 under once-multi / V-17)
#   WCT-6 held-open (once)     -> --once, a TCP connection accepted but never sends any handshake
#                              bytes and held open past the deadline -> still exit 6 near threshold
#                              (B-01 regression: pre-handshake connection must not suppress timeout)
#   WCT-7 held-open (once-multi)-> --once-multi, same held-open pre-handshake connection -> exit 6

param(
    [string]$ExePath = "",
    [int]$Port = 27895,
    # Test-only: bind server to ::1 (loopback) to isolate from LAN/WAN traffic (OM-4 flaky root cause).
    [switch]$BindLoopback
)

$ErrorActionPreference = "Stop"

# Optional server arg fragment: empty in production, "--bind-loopback" in test mode.
$bindLoopbackArg = if ($BindLoopback) { "--bind-loopback" } else { "" }

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
    # [I-2] v2 (R-06): Wait-ExitCode is one of the LARGEST single wait budgets in these
    # scripts. Without clamping it to the global deadline, I-2 does not close: a full-budget
    # wait plus a later stage stall can reach ctest's 600s hard kill before any diagnostic.
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

# Open a bare TCP connection and immediately close it (pre-handshake probe, session == null).
function Send-Probe {
    param([int]$ProbePort)
    $probe = New-Object System.Net.Sockets.TcpClient
    $probe.Connect("127.0.0.1", $ProbePort)
    Start-Sleep -Milliseconds 150
    $probe.Close()
}

# Open a bare TCP connection and KEEP it open without ever sending a byte. The server accepts it
# (so its handshake thread is dispatched and parks in recv) but it never forms a valid connection.
# Returns the live TcpClient; the caller must Close() it after asserting the server's exit (B-01).
function Open-HeldConnection {
    param([int]$HeldPort)
    $held = New-Object System.Net.Sockets.TcpClient
    $held.Connect("127.0.0.1", $HeldPort)
    return $held
}

$exe = Resolve-FastCloneExe -Hint $ExePath
$root = Join-Path $env:TEMP "fastclone-wct-it-$(Get-Random)"
$logDir = Join-Path $root "logs"
$src = Join-Path $root "src"
New-Item -ItemType Directory -Force -Path $logDir, $src | Out-Null

# Small fixture for quick real sessions.
1..15 | ForEach-Object {
    Set-Content -Path (Join-Path $src "file$_.txt") -Value ("payload-$_-" + ("x" * 4096))
}

# Large fixture for an interruptible session (streams=1 + 1KB chunks stays in-flight for seconds).
$srcBig = Join-Path $root "srcbig"
New-Item -ItemType Directory -Force -Path $srcBig | Out-Null
$bigFile = Join-Path $srcBig "big.bin"
$fsBig = [System.IO.File]::Create($bigFile)
try { $fsBig.SetLength(512MB) } finally { $fsBig.Close() }

$password = "fc-wct-pw"
$serverAddr = "127.0.0.1:$Port"
$failures = 0

try {
    Stop-AllFastClone

    Write-Host "[WCT-1] --once, no client at all -> exit 6 with timeout log (AC-07/AC-13)"
    Assert-FcDeadline -Stage "WCT-1"
    if (-not (Wait-PortFree -PortNum $Port -TimeoutSec 15)) {
        Write-Host "[PORT-BUSY] port $Port still held before WCT-1 server start:"
        (netstat -ano | Select-String -Pattern ":$Port ") | ForEach-Object { Write-Host "    $($_.Line)" }
    }
    $srv1 = Start-FastCloneProcess -Exe $exe -CliArgs @(
        "server", "--dir", $src, "--password", $password, "--port", "$Port", $bindLoopbackArg,
        "--once", "--wait-connect-timeout", "3s"
    ) -OutLog "$logDir\wct1-server.out" -ErrLog "$logDir\wct1-server.err"
    # [FR-02] WCT-1 has no sleep of its own; the design (section 3.4 coverage list) requires a
    # LISTENING probe here so a bind/half-start failure surfaces in 15s instead of as a
    # wait-connect timeout that masks the real cause (RC-5d).
    if (-not (Wait-PortListening -PortNum $Port -Proc $srv1 -TimeoutSec 15)) {
        Write-FcDiag -StageTag "WCT-1/server-not-listening" -Reason "server-bind" -TimeoutSec 15 `
            -Proc $srv1 -OutLog "$logDir\wct1-server.out" -ErrLog "$logDir\wct1-server.err"
        throw "WCT-1: server pid=$($srv1.Id) never reached LISTENING on port $Port within 15s"
    }
    $code1 = Wait-ExitCode -Proc $srv1 -TimeoutSec 30
    if ($code1 -ne 6) { throw "WCT-1: expected exit 6, got $code1" }
    $thr = (Select-String -Path "$logDir\wct1-server.out" -Pattern "wait_connect_timeout threshold_ms=3000").Count
    $nov = (Select-String -Path "$logDir\wct1-server.out" -Pattern "no_valid_connection=1").Count
    if ($thr -lt 1) { throw "WCT-1: expected 'wait_connect_timeout threshold_ms=3000' in log" }
    if ($nov -lt 1) { throw "WCT-1: expected 'no_valid_connection=1' in log (AC-13)" }
    Write-Host "  OK no-connection timeout exit=$code1 with both log elements"

    Write-Host "[WCT-2] --once, only TCP probes -> exit 6 (AC-08)"
    Assert-FcDeadline -Stage "WCT-2"
    $srv2 = Start-FastCloneProcess -Exe $exe -CliArgs @(
        "server", "--dir", $src, "--password", $password, "--port", "$Port", $bindLoopbackArg,
        "--once", "--wait-connect-timeout", "3s"
    ) -OutLog "$logDir\wct2-server.out" -ErrLog "$logDir\wct2-server.err"
    Start-Sleep -Milliseconds 500
    1..4 | ForEach-Object { Send-Probe -ProbePort $Port; Start-Sleep -Milliseconds 400 }
    $code2 = Wait-ExitCode -Proc $srv2 -TimeoutSec 30
    if ($code2 -ne 6) { throw "WCT-2: expected exit 6 with probes only, got $code2" }
    $fvc2 = (Select-String -Path "$logDir\wct2-server.out" -Pattern "first_valid_connection").Count
    if ($fvc2 -ne 0) { throw "WCT-2: a probe wrongly counted as a valid connection" }
    Write-Host "  OK probe-only timeout exit=$code2 (probes excluded)"

    Write-Host "[WCT-3] --once, real client connects in time -> wait-connect disabled, exit 0 (AC-09/AC-10a)"
    Assert-FcDeadline -Stage "WCT-3"
    if (-not (Wait-PortFree -PortNum $Port -TimeoutSec 15)) {
        Write-Host "[PORT-BUSY] port $Port still held before WCT-3 server start:"
        (netstat -ano | Select-String -Pattern ":$Port ") | ForEach-Object { Write-Host "    $($_.Line)" }
    }
    $srv3 = Start-FastCloneProcess -Exe $exe -CliArgs @(
        "server", "--dir", $src, "--password", $password, "--port", "$Port", $bindLoopbackArg,
        "--once", "--wait-connect-timeout", "30s"
    ) -OutLog "$logDir\wct3-server.out" -ErrLog "$logDir\wct3-server.err"
    if (-not (Wait-PortListening -PortNum $Port -Proc $srv3 -TimeoutSec 15)) {
        Write-FcDiag -StageTag "WCT-3/server-not-listening" -Reason "server-bind" -TimeoutSec 15 `
            -Proc $srv3 -OutLog "$logDir\wct3-server.out" -ErrLog "$logDir\wct3-server.err"
        throw "WCT-3: server pid=$($srv3.Id) never reached LISTENING on port $Port within 15s"
    }
    $tgt3 = Join-Path $root "tgt3"; New-Item -ItemType Directory -Force -Path $tgt3 | Out-Null
    $c3 = Invoke-FastCloneSync -Exe $exe -CliArgs @(
        "client", "--server", $serverAddr, "--target", $tgt3, "--password", $password
    ) -OutLog "$logDir\wct3-c3.out" -ErrLog "$logDir\wct3-c3.err" -TimeoutSec 150 -StageTag "WCT-3"
    if ($c3 -ne 0) { throw "WCT-3: real client expected exit 0, got $c3" }
    $code3 = Wait-ExitCode -Proc $srv3 -TimeoutSec 30
    if ($code3 -ne 0) { throw "WCT-3: server expected exit 0 after clean session, got $code3" }
    $fvc3 = (Select-String -Path "$logDir\wct3-server.out" -Pattern "first_valid_connection").Count
    $to3 = (Select-String -Path "$logDir\wct3-server.out" -Pattern "wait_connect_timeout").Count
    if ($fvc3 -lt 1) { throw "WCT-3: expected 'first_valid_connection' log after real connection" }
    if ($to3 -ne 0) { throw "WCT-3: must NOT log wait_connect_timeout after a valid connection" }
    Write-Host "  OK first connection disabled wait-connect; server exit=$code3"

    Write-Host "[WCT-3c] --once, real session aborted mid-transfer -> exit 5 (AC-10b)"
    Assert-FcDeadline -Stage "WCT-3c"
    if (-not (Wait-PortFree -PortNum $Port -TimeoutSec 15)) {
        Write-Host "[PORT-BUSY] port $Port still held before WCT-3c server start:"
        (netstat -ano | Select-String -Pattern ":$Port ") | ForEach-Object { Write-Host "    $($_.Line)" }
    }
    $srv3c = Start-FastCloneProcess -Exe $exe -CliArgs @(
        "server", "--dir", $srcBig, "--password", $password, "--port", "$Port", $bindLoopbackArg,
        "--once", "--wait-connect-timeout", "30s"
    ) -OutLog "$logDir\wct3c-server.out" -ErrLog "$logDir\wct3c-server.err"
    if (-not (Wait-PortListening -PortNum $Port -Proc $srv3c -TimeoutSec 15)) {
        Write-FcDiag -StageTag "WCT-3c/server-not-listening" -Reason "server-bind" -TimeoutSec 15 `
            -Proc $srv3c -OutLog "$logDir\wct3c-server.out" -ErrLog "$logDir\wct3c-server.err"
        throw "WCT-3c: server pid=$($srv3c.Id) never reached LISTENING on port $Port within 15s"
    }
    $tgt3c = Join-Path $root "tgt3c"; New-Item -ItemType Directory -Force -Path $tgt3c | Out-Null
    $c3c = Start-FastCloneProcess -Exe $exe -CliArgs @(
        "client", "--server", $serverAddr, "--target", $tgt3c, "--password", $password,
        "--reconnect-retries", "0", "--streams", "1", "--chunk-kb", "1"
    ) -OutLog "$logDir\wct3c-c3c.out" -ErrLog "$logDir\wct3c-c3c.err"
    Start-Sleep -Seconds 2
    if ($c3c.HasExited) { throw "WCT-3c: failing client finished before it could be interrupted" }
    Stop-Process -Id $c3c.Id -Force -ErrorAction SilentlyContinue  # abort mid-transfer -> hadError
    $code3c = Wait-ExitCode -Proc $srv3c -TimeoutSec 30
    if ($code3c -ne 5) { throw "WCT-3c: server expected exit 5 after aborted session, got $code3c" }
    $to3c = (Select-String -Path "$logDir\wct3c-server.out" -Pattern "wait_connect_timeout").Count
    if ($to3c -ne 0) { throw "WCT-3c: must NOT log wait_connect_timeout (a valid connection occurred)" }
    Write-Host "  OK aborted session -> exit=$code3c (wait-connect did not fire)"

    Write-Host "[WCT-4] --once-multi: after first conn, exit decided by idle-grace, never 6 (AC-11)"
    Assert-FcDeadline -Stage "WCT-4"
    if (-not (Wait-PortFree -PortNum $Port -TimeoutSec 15)) {
        Write-Host "[PORT-BUSY] port $Port still held before WCT-4 server start:"
        (netstat -ano | Select-String -Pattern ":$Port ") | ForEach-Object { Write-Host "    $($_.Line)" }
    }
    $srv4 = Start-FastCloneProcess -Exe $exe -CliArgs @(
        "server", "--dir", $src, "--password", $password, "--port", "$Port", $bindLoopbackArg,
        "--once-multi", "--wait-connect-timeout", "30s", "--once-idle-grace", "5s"
    ) -OutLog "$logDir\wct4-server.out" -ErrLog "$logDir\wct4-server.err"
    if (-not (Wait-PortListening -PortNum $Port -Proc $srv4 -TimeoutSec 15)) {
        Write-FcDiag -StageTag "WCT-4/server-not-listening" -Reason "server-bind" -TimeoutSec 15 `
            -Proc $srv4 -OutLog "$logDir\wct4-server.out" -ErrLog "$logDir\wct4-server.err"
        throw "WCT-4: server pid=$($srv4.Id) never reached LISTENING on port $Port within 15s"
    }
    $tgt4 = Join-Path $root "tgt4"; New-Item -ItemType Directory -Force -Path $tgt4 | Out-Null
    $c4 = Invoke-FastCloneSync -Exe $exe -CliArgs @(
        "client", "--server", $serverAddr, "--target", $tgt4, "--password", $password
    ) -OutLog "$logDir\wct4-c4.out" -ErrLog "$logDir\wct4-c4.err" `
        -TimeoutSec 150 -StageTag "WCT-4"
    if ($c4 -ne 0) { throw "WCT-4: real client expected exit 0, got $c4" }
    # Stay idle well past the (short) idle-grace; the server must exit via idle-grace (0), not 6.
    $code4 = Wait-ExitCode -Proc $srv4 -TimeoutSec 30
    if ($code4 -ne 0) { throw "WCT-4: expected idle-grace exit 0, got $code4" }
    $to4 = (Select-String -Path "$logDir\wct4-server.out" -Pattern "wait_connect_timeout").Count
    $fvc4 = (Select-String -Path "$logDir\wct4-server.out" -Pattern "first_valid_connection").Count
    if ($to4 -ne 0) { throw "WCT-4: must NOT log wait_connect_timeout after a valid connection" }
    if ($fvc4 -lt 1) { throw "WCT-4: expected 'first_valid_connection' after the real session" }
    Write-Host "  OK once-multi first conn disabled wait-connect; idle-grace exit=$code4"

    Write-Host "[WCT-5] --once-multi, only probes -> exit 6 (V-17)"
    Assert-FcDeadline -Stage "WCT-5"
    $srv5 = Start-FastCloneProcess -Exe $exe -CliArgs @(
        "server", "--dir", $src, "--password", $password, "--port", "$Port", $bindLoopbackArg,
        "--once-multi", "--wait-connect-timeout", "3s", "--once-idle-grace", "5s"
    ) -OutLog "$logDir\wct5-server.out" -ErrLog "$logDir\wct5-server.err"
    Start-Sleep -Milliseconds 500
    1..4 | ForEach-Object { Send-Probe -ProbePort $Port; Start-Sleep -Milliseconds 400 }
    $code5 = Wait-ExitCode -Proc $srv5 -TimeoutSec 30
    if ($code5 -ne 6) { throw "WCT-5: once-multi probe-only expected exit 6, got $code5" }
    Write-Host "  OK once-multi probe-only timeout exit=$code5"

    Write-Host "[WCT-6] --once, accepted TCP connection held open w/o handshake -> still exit 6 (B-01)"
    Assert-FcDeadline -Stage "WCT-6"
    $srv6 = Start-FastCloneProcess -Exe $exe -CliArgs @(
        "server", "--dir", $src, "--password", $password, "--port", "$Port", $bindLoopbackArg,
        "--once", "--wait-connect-timeout", "3s"
    ) -OutLog "$logDir\wct6-server.out" -ErrLog "$logDir\wct6-server.err"
    Start-Sleep -Milliseconds 700
    $held6 = Open-HeldConnection -HeldPort $Port
    try {
        $sw6 = [System.Diagnostics.Stopwatch]::StartNew()
        $code6 = Wait-ExitCode -Proc $srv6 -TimeoutSec 20
        $sw6.Stop()
    }
    finally {
        $held6.Close()
    }
    if ($code6 -ne 6) { throw "WCT-6: held-open pre-handshake connection -> expected exit 6, got $code6" }
    # threshold is 3s; allow generous slack but well under the 20s "still running" failure the bug caused.
    if ($sw6.Elapsed.TotalSeconds -gt 10) {
        throw "WCT-6: server took $([math]::Round($sw6.Elapsed.TotalSeconds,1))s to time out while a TCP connection was held open (B-01: in-flight handshake must not suppress timeout)"
    }
    $thr6 = (Select-String -Path "$logDir\wct6-server.out" -Pattern "wait_connect_timeout threshold_ms=3000").Count
    $nov6 = (Select-String -Path "$logDir\wct6-server.out" -Pattern "no_valid_connection=1").Count
    $fvc6 = (Select-String -Path "$logDir\wct6-server.out" -Pattern "first_valid_connection").Count
    if ($thr6 -lt 1) { throw "WCT-6: expected 'wait_connect_timeout threshold_ms=3000' in log" }
    if ($nov6 -lt 1) { throw "WCT-6: expected 'no_valid_connection=1' in log (AC-13)" }
    if ($fvc6 -ne 0) { throw "WCT-6: held pre-handshake connection wrongly counted as a valid connection" }
    Write-Host "  OK held-open connection still timed out exit=$code6 in $([math]::Round($sw6.Elapsed.TotalSeconds,1))s"

    Write-Host "[WCT-7] --once-multi, accepted TCP connection held open w/o handshake -> still exit 6 (B-01)"
    Assert-FcDeadline -Stage "WCT-7"
    $srv7 = Start-FastCloneProcess -Exe $exe -CliArgs @(
        "server", "--dir", $src, "--password", $password, "--port", "$Port", $bindLoopbackArg,
        "--once-multi", "--wait-connect-timeout", "3s", "--once-idle-grace", "5s"
    ) -OutLog "$logDir\wct7-server.out" -ErrLog "$logDir\wct7-server.err"
    Start-Sleep -Milliseconds 700
    $held7 = Open-HeldConnection -HeldPort $Port
    try {
        $sw7 = [System.Diagnostics.Stopwatch]::StartNew()
        $code7 = Wait-ExitCode -Proc $srv7 -TimeoutSec 20
        $sw7.Stop()
    }
    finally {
        $held7.Close()
    }
    if ($code7 -ne 6) { throw "WCT-7: once-multi held-open pre-handshake connection -> expected exit 6, got $code7" }
    if ($sw7.Elapsed.TotalSeconds -gt 10) {
        throw "WCT-7: once-multi server took $([math]::Round($sw7.Elapsed.TotalSeconds,1))s to time out while a TCP connection was held open (B-01)"
    }
    $fvc7 = (Select-String -Path "$logDir\wct7-server.out" -Pattern "first_valid_connection").Count
    if ($fvc7 -ne 0) { throw "WCT-7: held pre-handshake connection wrongly counted as a valid connection" }
    Write-Host "  OK once-multi held-open connection still timed out exit=$code7 in $([math]::Round($sw7.Elapsed.TotalSeconds,1))s"

    Write-Host "All wait-connect-timeout integration tests passed. Logs: $logDir"
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
