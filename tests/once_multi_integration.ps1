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
    [int]$Port = 27894
)

$ErrorActionPreference = "Stop"

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

function Stop-AllFastClone {
    Get-Process FastClone -ErrorAction SilentlyContinue | ForEach-Object {
        Stop-Process -Id $_.Id -Force -ErrorAction SilentlyContinue
    }
    Start-Sleep -Milliseconds 500
}

function Wait-ExitCode {
    param([System.Diagnostics.Process]$Proc, [int]$TimeoutSec = 30)
    if ($Proc.HasExited) {
        $Proc.Refresh()
    } elseif (-not $Proc.WaitForExit($TimeoutSec * 1000)) {
        Stop-Process -Id $Proc.Id -Force
        throw "Process $($Proc.Id) did not exit within ${TimeoutSec}s"
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
    return $proc
}

function Invoke-FastCloneSync {
    param(
        [string]$Exe,
        [string[]]$CliArgs,
        [string]$OutLog,
        [string]$ErrLog
    )
    foreach ($a in $CliArgs) {
        if ($null -eq $a -or $a -eq "") {
            throw "Invoke-FastCloneSync: null/empty argument"
        }
    }
    $env:FASTCLONE_DEBUG = "1"
    $argLine = ($CliArgs | ForEach-Object {
        if ($_ -match '\s') { "`"$_`"" } else { $_ }
    }) -join " "
    cmd /c "`"$Exe`" $argLine 1>`"$OutLog`" 2>`"$ErrLog`""
    return [int]$LASTEXITCODE
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
    $srv1 = Start-FastCloneProcess -Exe $exe -CliArgs @(
        "server", "--dir", $src, "--password", $password, "--port", "$Port",
        "--once-multi", "--once-idle-grace", $grace
    ) -OutLog "$logDir\om1-server.out" -ErrLog "$logDir\om1-server.err"
    Start-Sleep -Seconds 1
    $tgt1a = Join-Path $root "tgt1a"; New-Item -ItemType Directory -Force -Path $tgt1a | Out-Null
    $c1a = Invoke-FastCloneSync -Exe $exe -CliArgs @(
        "client", "--server", $serverAddr, "--target", $tgt1a, "--password", $password
    ) -OutLog "$logDir\om1-c1a.out" -ErrLog "$logDir\om1-c1a.err"
    if ($c1a -ne 0) { throw "OM-1: first client expected exit 0, got $c1a" }
    if ($srv1.HasExited) { throw "OM-1: server exited after first session (should survive within grace)" }
    $tgt1b = Join-Path $root "tgt1b"; New-Item -ItemType Directory -Force -Path $tgt1b | Out-Null
    $c1b = Invoke-FastCloneSync -Exe $exe -CliArgs @(
        "client", "--server", $serverAddr, "--target", $tgt1b, "--password", $password
    ) -OutLog "$logDir\om1-c1b.out" -ErrLog "$logDir\om1-c1b.err"
    if ($c1b -ne 0) { throw "OM-1: second client expected exit 0, got $c1b" }
    $code1 = Wait-ExitCode -Proc $srv1 -TimeoutSec 30
    if ($code1 -ne 0) { throw "OM-1: server expected exit 0 after grace, got $code1" }
    $done1 = (Select-String -Path "$logDir\om1-server.out" -Pattern "\[mp\] conn_done").Count
    if ($done1 -lt 2) { throw "OM-1: expected >=2 completed sessions in server log, got $done1" }
    Write-Host "  OK two sequential sessions served; server exit=$code1"

    Write-Host "[OM-2] concurrent: two real sessions served at once, exit 0 after both finish"
    $srv2 = Start-FastCloneProcess -Exe $exe -CliArgs @(
        "server", "--dir", $srcBig, "--password", $password, "--port", "$Port",
        "--once-multi", "--once-idle-grace", $grace
    ) -OutLog "$logDir\om2-server.out" -ErrLog "$logDir\om2-server.err"
    Start-Sleep -Seconds 1
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
    $longGrace = "5s"
    $srv3 = Start-FastCloneProcess -Exe $exe -CliArgs @(
        "server", "--dir", $srcBig, "--password", $password, "--port", "$Port",
        "--once-multi", "--once-idle-grace", $longGrace
    ) -OutLog "$logDir\om3-server.out" -ErrLog "$logDir\om3-server.err"
    Start-Sleep -Seconds 1
    # First (quick) session completes and arms the grace timer.
    $tgt3a = Join-Path $root "tgt3a"; New-Item -ItemType Directory -Force -Path $tgt3a | Out-Null
    $c3a = Invoke-FastCloneSync -Exe $exe -CliArgs @(
        "client", "--server", $serverAddr, "--target", $tgt3a, "--password", $password
    ) -OutLog "$logDir\om3-c3a.out" -ErrLog "$logDir\om3-c3a.err"
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
    $srv4 = Start-FastCloneProcess -Exe $exe -CliArgs @(
        "server", "--dir", $srcBig, "--password", $password, "--port", "$Port",
        "--once-multi", "--once-idle-grace", $grace
    ) -OutLog "$logDir\om4-server.out" -ErrLog "$logDir\om4-server.err"
    Start-Sleep -Seconds 1
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
    ) -OutLog "$logDir\om4-c4b.out" -ErrLog "$logDir\om4-c4b.err"
    if ($c4b -ne 0) { throw "OM-4: clean second client expected exit 0, got $c4b" }
    $code4 = Wait-ExitCode -Proc $srv4 -TimeoutSec 30
    if ($code4 -ne 5) { throw "OM-4: server expected sticky exit 5, got $code4" }
    Write-Host "  OK aborted session forced sticky exit=$code4"

    Write-Host "[OM-5a] probes during grace do not reset it; server exits on schedule"
    $srv5 = Start-FastCloneProcess -Exe $exe -CliArgs @(
        "server", "--dir", $src, "--password", $password, "--port", "$Port",
        "--once-multi", "--once-idle-grace", "4s"
    ) -OutLog "$logDir\om5a-server.out" -ErrLog "$logDir\om5a-server.err"
    Start-Sleep -Seconds 1
    $tgt5 = Join-Path $root "tgt5"; New-Item -ItemType Directory -Force -Path $tgt5 | Out-Null
    $c5 = Invoke-FastCloneSync -Exe $exe -CliArgs @(
        "client", "--server", $serverAddr, "--target", $tgt5, "--password", $password
    ) -OutLog "$logDir\om5a-c5.out" -ErrLog "$logDir\om5a-c5.err"
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
    $srv6 = Start-FastCloneProcess -Exe $exe -CliArgs @(
        "server", "--dir", $src, "--password", $password, "--port", "$Port",
        "--once-multi", "--once-idle-grace", "2s"
    ) -OutLog "$logDir\om5b-server.out" -ErrLog "$logDir\om5b-server.err"
    Start-Sleep -Seconds 1
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
