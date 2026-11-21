# FastClone reconnect integration smoke tests (Windows).
# Usage: .\tests\reconnect_integration.ps1 [-ExePath path\to\FastClone.exe]
# Creates small temp dirs; always kills FastClone processes on exit.

param(
    [string]$ExePath = "",
    [int]$Port = 27892,
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
    # [59 C8] Record the start time so the kill guard below can require BOTH the pid and
    # an identical StartTime (a recycled pid would have a different one).
    $startUtc = $null
    try { $startUtc = $Proc.StartTime.ToUniversalTime() } catch { $startUtc = $null }
    $script:FcOwnedPids[[int]$Proc.Id] = [pscustomobject]@{ Tag = $Tag; StartTimeUtc = $startUtc }
    return $Proc
}

# [59 C8 / original-source triple guard, cf. once_multi Stop-FcOwnedProcess] Stop a
# process THIS script registered: the pid must be in this run's registry, STILL be named
# FastClone, and carry the exact recorded StartTime (a recycled pid fails the name or the
# time check; a MISSING StartTimeUtc is a hard, fail-closed refusal). No whole-machine
# kill, no by-port/by-name fallback. Returns $true if the stop was issued, $false if the
# process was already gone (the CALLER judges that case: no kill happened).
function Stop-FcOwnedProcess {
    param([System.Diagnostics.Process]$Proc, [string]$StageTag = "stop-owned")
    if ($null -eq $Proc) { throw "$StageTag`: Stop-FcOwnedProcess called with no process" }
    if (-not $script:FcOwnedPids.ContainsKey([int]$Proc.Id)) {
        throw "$StageTag`: pid=$($Proc.Id) is not in this run's owned-process registry (refusing to touch it)"
    }
    $info = $script:FcOwnedPids[[int]$Proc.Id]
    $p = Get-Process -Id $Proc.Id -ErrorAction SilentlyContinue
    if (-not $p) { return $false }    # already exited on its own
    if ($p.ProcessName -ne 'FastClone') {
        throw "$StageTag`: pid=$($Proc.Id) name='$($p.ProcessName)' is not FastClone (pid reuse? refusing)"
    }
    if ($null -eq $info.StartTimeUtc) {
        throw "$StageTag`: pid=$($Proc.Id) registry entry has no recorded StartTimeUtc (fail-closed: refusing to stop)"
    }
    $same = $false
    try { $same = ($p.StartTime.ToUniversalTime() -eq $info.StartTimeUtc) } catch { $same = $false }
    if (-not $same) {
        throw "$StageTag`: pid=$($Proc.Id) StartTime differs from the registry entry (pid reuse? refusing)"
    }
    Stop-Process -Id $Proc.Id -Force -ErrorAction SilentlyContinue
    return $true
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

function Assert-ReconnectLog {
    param([string[]]$Paths, [string]$Scenario)
    $joined = ($Paths | ForEach-Object {
        Get-Content $_ -ErrorAction SilentlyContinue
    }) -join "`n"
    if ($joined -notmatch '\[reconnect\] attempt=') {
        throw "$Scenario : missing [reconnect] log in: $($Paths -join ', ')"
    }
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
    # Cache the handle so .ExitCode stays readable on a self-exiting process (PowerShell quirk).
    $null = $proc.Handle
    $null = Register-OwnedProcess -Proc $proc -Tag "server"
    return $proc
}

function Assert-NoReconnectAttempts {
    param([string]$ErrLog, [string]$Scenario)
    if (Select-String -Path $ErrLog -Pattern "\[reconnect\] attempt=" -Quiet) {
        throw "$Scenario : unexpected reconnect budget use (see $ErrLog)"
    }
}

function Assert-Contains {
    param([string]$Path, [string]$Pattern, [string]$Scenario)
    if (-not (Select-String -Path $Path -Pattern $Pattern -Quiet)) {
        throw "$Scenario : expected pattern '$Pattern' in $Path"
    }
}

$exe = Resolve-FastCloneExe -Hint $ExePath
$root = Join-Path $env:TEMP "fastclone-reconnect-it-$(Get-Random)"
$logDir = Join-Path $root "logs"
$src = Join-Path $root "src"
$tgt = Join-Path $root "tgt"
New-Item -ItemType Directory -Force -Path $logDir, $src, $tgt | Out-Null

# Small fixture: slow enough (streams=1) to interrupt before completion.
1..200 | ForEach-Object {
    Set-Content -Path (Join-Path $src "file$_.txt") -Value ("payload-$_-" + ("x" * 262144))
}
New-Item -ItemType Directory -Force -Path (Join-Path $src "nested\deep") | Out-Null
Set-Content -Path (Join-Path $src "nested\deep\leaf.txt") -Value "nested"

$password = "fc-it-pw"
$serverAddr = "127.0.0.1:$Port"
$failures = 0

try {
    Stop-AllFastClone

    Write-Host "[IT-1] wrong password -> exit 1, no reconnect budget"
    Assert-FcDeadline -Stage "IT-1"
    if (-not (Wait-PortFree -PortNum $Port -TimeoutSec 15)) {
        Write-Host "[PORT-BUSY] port $Port still held before IT-1 server start:"
        (netstat -ano | Select-String -Pattern ":$Port ") | ForEach-Object { Write-Host "    $($_.Line)" }
    }
    $srv1 = Start-FastCloneProcess -Exe $exe -CliArgs @(
        "server", "--dir", $src, "--password", $password, "--port", "$Port", $bindLoopbackArg
    ) -OutLog "$logDir\it1-server.out" -ErrLog "$logDir\it1-server.err"
    if (-not (Wait-PortListening -PortNum $Port -Proc $srv1 -TimeoutSec 15)) {
        Write-FcDiag -StageTag "IT-1/server-not-listening" -Reason "server-bind" -TimeoutSec 15 `
            -Proc $srv1 -OutLog "$logDir\it1-server.out" -ErrLog "$logDir\it1-server.err"
        throw "IT-1: server pid=$($srv1.Id) never reached LISTENING on port $Port within 15s"
    }
    $code1 = Invoke-FastCloneSync -Exe $exe -CliArgs @(
        "client", "--server", $serverAddr, "--target", $tgt,
        "--password", "wrong-password", "--reconnect-retries", "10"
    ) -OutLog "$logDir\it1-client.out" -ErrLog "$logDir\it1-client.err" -TimeoutSec 150 -StageTag "IT-1"
    Stop-Process -Id $srv1.Id -Force -ErrorAction SilentlyContinue
    if ($code1 -ne 1) { throw "IT-1: expected exit 1, got $code1" }
    Assert-NoReconnectAttempts -ErrLog "$logDir\it1-client.err" -Scenario "IT-1"
    Write-Host "  OK exit=$code1"

    Write-Host "[IT-2] server late start -> connect tolerance + reconnect log"
    Assert-FcDeadline -Stage "IT-2"
    # [r61 / G-AC8] Fresh target for this case: the old between-case reset was a recursive
    # force-delete of $tgt; the zero-delete rule replaces it with a NEW unique target dir
    # per case (same fresh-empty-target semantics; nothing is ever deleted).
    $tgtIt2 = Join-Path $root "tgt-it2"
    New-Item -ItemType Directory -Force -Path $tgtIt2 | Out-Null
    Write-Host "RETAINED fresh target dir (no deletion by design): $tgtIt2"
    $cli2 = Start-FastCloneProcess -Exe $exe -CliArgs @(
        "client", "--server", $serverAddr, "--target", $tgtIt2,
        "--password", $password, "--reconnect-retries", "10",
        "--streams", "1", "--chunk-kb", "1"
    ) -OutLog "$logDir\it2-client.out" -ErrLog "$logDir\it2-client.err"
    Start-Sleep -Seconds 5
    $srv2 = Start-FastCloneProcess -Exe $exe -CliArgs @(
        "server", "--dir", $src, "--password", $password, "--port", "$Port", $bindLoopbackArg
    ) -OutLog "$logDir\it2-server.out" -ErrLog "$logDir\it2-server.err"
    # [59 C8] The fixed 12s sleep becomes a BOUNDED EVENT wait (local PS5.1 helper:
    # Select-String + 200ms polls): the server is provably serving when [mp] conn_accept
    # appears in it2-server.out, OR the client already finished on its own. No event
    # within 12s => loud failure with diagnostics -- never a silent kill that would mask
    # a dead server (the old code force-killed on timeout and then misjudged the -1).
    $it2Deadline = [DateTime]::UtcNow.AddSeconds(12)
    $it2Accept = $false
    while ([DateTime]::UtcNow -lt $it2Deadline) {
        if ($cli2.HasExited) { break }
        if (Select-String -Path "$logDir\it2-server.out" -Pattern '\[mp\] conn_accept' -Quiet -ErrorAction SilentlyContinue) {
            $it2Accept = $true
            break
        }
        Start-Sleep -Milliseconds 200
    }
    if (-not $it2Accept -and -not $cli2.HasExited) {
        Write-FcDiag -StageTag "IT-2/no-server-event" -Reason "no-conn-accept" -TimeoutSec 12 `
            -Proc $cli2 -OutLog "$logDir\it2-client.out" -ErrLog "$logDir\it2-client.err"
        throw "IT-2: no [mp] conn_accept in it2-server.out and client still running after 12s (server not serving; failing loudly instead of masking)"
    }
    # Reconnect evidence BEFORE any kill: only an active kill that FOLLOWS an observed
    # reconnect attempt may later justify the forced -1 (both exit branches must pass).
    Assert-ReconnectLog -Paths @("$logDir\it2-client.err", "$logDir\it2-client.out") -Scenario "IT-2"
    if (Select-String -Path "$logDir\it2-client.err" -Pattern "FastClone error: connect failed" -Quiet) {
        throw "IT-2: connect failed was fatal (regression)"
    }
    # [59 C8] Unforced-exit record: did the client exit on its OWN before any stop?
    $cli2.Refresh()
    $preExit = $cli2.HasExited
    $ownKilled = $false
    if (-not $preExit) {
        # Active abort of OUR registered client, behind the triple ownership guard
        # (registry pid + still-named FastClone + identical recorded StartTime).
        $null = Stop-FcOwnedProcess -Proc $cli2 -StageTag "IT-2-own-stop"
        $ownKilled = $true
        $null = $cli2.WaitForExit(3000)
        $cli2.Refresh()
    }
    if ($cli2.HasExited) {
        $ec = $cli2.ExitCode
        if ($ownKilled) {
            # Our own force-kill's expected product: PowerShell reports -1. Accepted ONLY
            # because the reconnect attempt was observed BEFORE the kill.
            if ($null -eq $ec -or $ec -ne -1) { throw "IT-2: own-killed client expected exit -1, got $ec" }
        } elseif ($null -ne $ec -and $ec -ne 0 -and $ec -ne 2) {
            # Autonomous exit keeps the ORIGINAL allowed set {0,2}; 6 is server-only
            # (kExitWaitConnectTimeout, sync_engine_server) and stays excluded.
            throw "IT-2: unexpected client exit $ec"
        }
    }
    Write-Host "  OK connect tolerance (client exit=$($cli2.ExitCode) ownkilled=$ownKilled preExit=$preExit)"
    Stop-Process -Id $srv2.Id -Force -ErrorAction SilentlyContinue

    Write-Host "[IT-3] reconnect disabled -> exit 3 on disconnect"
    Assert-FcDeadline -Stage "IT-3"
    # [r61 / G-AC8] Fresh unique target (replaces the old recursive force-delete reset).
    $tgtIt3 = Join-Path $root "tgt-it3"
    New-Item -ItemType Directory -Force -Path $tgtIt3 | Out-Null
    Write-Host "RETAINED fresh target dir (no deletion by design): $tgtIt3"
    if (-not (Wait-PortFree -PortNum $Port -TimeoutSec 15)) {
        Write-Host "[PORT-BUSY] port $Port still held before IT-3 server start:"
        (netstat -ano | Select-String -Pattern ":$Port ") | ForEach-Object { Write-Host "    $($_.Line)" }
    }
    $srv4 = Start-FastCloneProcess -Exe $exe -CliArgs @(
        "server", "--dir", $src, "--password", $password, "--port", "$Port", $bindLoopbackArg
    ) -OutLog "$logDir\it3-server.out" -ErrLog "$logDir\it3-server.err"
    if (-not (Wait-PortListening -PortNum $Port -Proc $srv4 -TimeoutSec 15)) {
        Write-FcDiag -StageTag "IT-3/server-not-listening" -Reason "server-bind" -TimeoutSec 15 `
            -Proc $srv4 -OutLog "$logDir\it3-server.out" -ErrLog "$logDir\it3-server.err"
        throw "IT-3: server pid=$($srv4.Id) never reached LISTENING on port $Port within 15s"
    }
    $cli3 = Start-FastCloneProcess -Exe $exe -CliArgs @(
        "client", "--server", $serverAddr, "--target", $tgtIt3,
        "--password", $password, "--reconnect-retries", "0",
        "--streams", "1", "--chunk-kb", "1"
    ) -OutLog "$logDir\it3-client.out" -ErrLog "$logDir\it3-client.err"
    Start-Sleep -Milliseconds 300
    Stop-Process -Id $srv4.Id -Force
    Start-Sleep -Seconds 2
    if (-not $cli3.WaitForExit(20000)) {
        Stop-Process -Id $cli3.Id -Force
    }
    Start-Sleep -Milliseconds 300
    $cli3.Refresh()
    $code3 = $cli3.ExitCode
    if ($null -eq $code3) {
        if (Select-String -Path "$logDir\it3-client.out" -Pattern "Sync aborted \(incomplete manifest\)" -Quiet) {
            $code3 = 3
        } else {
            throw "IT-3: exit code unavailable and no incomplete-manifest message"
        }
    }
    if ($code3 -ne 3) { throw "IT-3: expected exit 3, got $code3" }
    Assert-NoReconnectAttempts -ErrLog "$logDir\it3-client.err" -Scenario "IT-3"
    Write-Host "  OK exit=$code3"

    Write-Host "[IT-4] reconnect budget exhausted -> exit 4"
    Assert-FcDeadline -Stage "IT-4"
    # [r61 / G-AC8] Fresh unique target (replaces the old recursive force-delete reset).
    $tgtIt4 = Join-Path $root "tgt-it4"
    New-Item -ItemType Directory -Force -Path $tgtIt4 | Out-Null
    Write-Host "RETAINED fresh target dir (no deletion by design): $tgtIt4"
    $code4 = Invoke-FastCloneSync -Exe $exe -CliArgs @(
        "client", "--server", $serverAddr, "--target", $tgtIt4,
        "--password", $password, "--reconnect-retries", "1"
    ) -OutLog "$logDir\it4-client.out" -ErrLog "$logDir\it4-client.err" -TimeoutSec 150 -StageTag "IT-4"
    if ($code4 -ne 4) { throw "IT-4: expected exit 4, got $code4" }
    Assert-Contains -Path "$logDir\it4-client.err" -Pattern "\[reconnect\] budget exhausted" -Scenario "IT-4"
    if (-not (Test-Path "$logDir\it4-client.err") -or (Get-Item "$logDir\it4-client.err").Length -eq 0) {
        Assert-Contains -Path "$logDir\it4-client.out" -Pattern "\[reconnect\] budget exhausted" -Scenario "IT-4-out"
    }
    Write-Host "  OK exit=$code4"

    Write-Host "All reconnect integration tests passed. Logs: $logDir"
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
    # now executes no deletion of any kind (the three old between-case recursive
    # force-deletes of $tgt were replaced by fresh unique per-case target dirs). The
    # run tree (fastclone-reconnect-it-<random> directly under $env:TEMP, this run's
    # own) is retained and its path printed so the CI failure-artifact upload can
    # collect the per-case logs.
    Write-Host "RETAINED run temp root (no deletion by design; logs: $logDir): $root"
    if ($failures -ne 0) { exit 1 }
}
