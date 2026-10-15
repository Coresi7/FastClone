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
    Remove-Item -Recurse -Force $tgt -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force -Path $tgt | Out-Null
    $cli2 = Start-FastCloneProcess -Exe $exe -CliArgs @(
        "client", "--server", $serverAddr, "--target", $tgt,
        "--password", $password, "--reconnect-retries", "10",
        "--streams", "1", "--chunk-kb", "1"
    ) -OutLog "$logDir\it2-client.out" -ErrLog "$logDir\it2-client.err"
    Start-Sleep -Seconds 5
    $srv2 = Start-FastCloneProcess -Exe $exe -CliArgs @(
        "server", "--dir", $src, "--password", $password, "--port", "$Port", $bindLoopbackArg
    ) -OutLog "$logDir\it2-server.out" -ErrLog "$logDir\it2-server.err"
    Start-Sleep -Seconds 12
    if (-not $cli2.HasExited) {
        Stop-Process -Id $cli2.Id -Force -ErrorAction SilentlyContinue
        Start-Sleep -Milliseconds 300
    }
    Assert-ReconnectLog -Paths @("$logDir\it2-client.err", "$logDir\it2-client.out") -Scenario "IT-2"
    if (Select-String -Path "$logDir\it2-client.err" -Pattern "FastClone error: connect failed" -Quiet) {
        throw "IT-2: connect failed was fatal (regression)"
    }
    if ($cli2.HasExited) {
        $cli2.Refresh()
        $ec = $cli2.ExitCode
        if ($null -ne $ec -and $ec -ne 0 -and $ec -ne 2) {
            throw "IT-2: unexpected client exit $ec"
        }
    }
    Write-Host "  OK connect tolerance (client exit=$($cli2.ExitCode))"
    Stop-Process -Id $srv2.Id -Force -ErrorAction SilentlyContinue

    Write-Host "[IT-3] reconnect disabled -> exit 3 on disconnect"
    Assert-FcDeadline -Stage "IT-3"
    Remove-Item -Recurse -Force $tgt -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force -Path $tgt | Out-Null
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
        "client", "--server", $serverAddr, "--target", $tgt,
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
    Remove-Item -Recurse -Force $tgt -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force -Path $tgt | Out-Null
    $code4 = Invoke-FastCloneSync -Exe $exe -CliArgs @(
        "client", "--server", $serverAddr, "--target", $tgt,
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
    if ($failures -ne 0) { exit 1 }
}
