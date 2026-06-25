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
    [int]$Port = 27895
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
    $srv1 = Start-FastCloneProcess -Exe $exe -CliArgs @(
        "server", "--dir", $src, "--password", $password, "--port", "$Port",
        "--once", "--wait-connect-timeout", "3s"
    ) -OutLog "$logDir\wct1-server.out" -ErrLog "$logDir\wct1-server.err"
    $code1 = Wait-ExitCode -Proc $srv1 -TimeoutSec 30
    if ($code1 -ne 6) { throw "WCT-1: expected exit 6, got $code1" }
    $thr = (Select-String -Path "$logDir\wct1-server.out" -Pattern "wait_connect_timeout threshold_ms=3000").Count
    $nov = (Select-String -Path "$logDir\wct1-server.out" -Pattern "no_valid_connection=1").Count
    if ($thr -lt 1) { throw "WCT-1: expected 'wait_connect_timeout threshold_ms=3000' in log" }
    if ($nov -lt 1) { throw "WCT-1: expected 'no_valid_connection=1' in log (AC-13)" }
    Write-Host "  OK no-connection timeout exit=$code1 with both log elements"

    Write-Host "[WCT-2] --once, only TCP probes -> exit 6 (AC-08)"
    $srv2 = Start-FastCloneProcess -Exe $exe -CliArgs @(
        "server", "--dir", $src, "--password", $password, "--port", "$Port",
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
    $srv3 = Start-FastCloneProcess -Exe $exe -CliArgs @(
        "server", "--dir", $src, "--password", $password, "--port", "$Port",
        "--once", "--wait-connect-timeout", "30s"
    ) -OutLog "$logDir\wct3-server.out" -ErrLog "$logDir\wct3-server.err"
    Start-Sleep -Seconds 1
    $tgt3 = Join-Path $root "tgt3"; New-Item -ItemType Directory -Force -Path $tgt3 | Out-Null
    $c3 = Invoke-FastCloneSync -Exe $exe -CliArgs @(
        "client", "--server", $serverAddr, "--target", $tgt3, "--password", $password
    ) -OutLog "$logDir\wct3-c3.out" -ErrLog "$logDir\wct3-c3.err"
    if ($c3 -ne 0) { throw "WCT-3: real client expected exit 0, got $c3" }
    $code3 = Wait-ExitCode -Proc $srv3 -TimeoutSec 30
    if ($code3 -ne 0) { throw "WCT-3: server expected exit 0 after clean session, got $code3" }
    $fvc3 = (Select-String -Path "$logDir\wct3-server.out" -Pattern "first_valid_connection").Count
    $to3 = (Select-String -Path "$logDir\wct3-server.out" -Pattern "wait_connect_timeout").Count
    if ($fvc3 -lt 1) { throw "WCT-3: expected 'first_valid_connection' log after real connection" }
    if ($to3 -ne 0) { throw "WCT-3: must NOT log wait_connect_timeout after a valid connection" }
    Write-Host "  OK first connection disabled wait-connect; server exit=$code3"

    Write-Host "[WCT-3c] --once, real session aborted mid-transfer -> exit 5 (AC-10b)"
    $srv3c = Start-FastCloneProcess -Exe $exe -CliArgs @(
        "server", "--dir", $srcBig, "--password", $password, "--port", "$Port",
        "--once", "--wait-connect-timeout", "30s"
    ) -OutLog "$logDir\wct3c-server.out" -ErrLog "$logDir\wct3c-server.err"
    Start-Sleep -Seconds 1
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
    $srv4 = Start-FastCloneProcess -Exe $exe -CliArgs @(
        "server", "--dir", $src, "--password", $password, "--port", "$Port",
        "--once-multi", "--wait-connect-timeout", "30s", "--once-idle-grace", "5s"
    ) -OutLog "$logDir\wct4-server.out" -ErrLog "$logDir\wct4-server.err"
    Start-Sleep -Seconds 1
    $tgt4 = Join-Path $root "tgt4"; New-Item -ItemType Directory -Force -Path $tgt4 | Out-Null
    $c4 = Invoke-FastCloneSync -Exe $exe -CliArgs @(
        "client", "--server", $serverAddr, "--target", $tgt4, "--password", $password
    ) -OutLog "$logDir\wct4-c4.out" -ErrLog "$logDir\wct4-c4.err"
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
    $srv5 = Start-FastCloneProcess -Exe $exe -CliArgs @(
        "server", "--dir", $src, "--password", $password, "--port", "$Port",
        "--once-multi", "--wait-connect-timeout", "3s", "--once-idle-grace", "5s"
    ) -OutLog "$logDir\wct5-server.out" -ErrLog "$logDir\wct5-server.err"
    Start-Sleep -Milliseconds 500
    1..4 | ForEach-Object { Send-Probe -ProbePort $Port; Start-Sleep -Milliseconds 400 }
    $code5 = Wait-ExitCode -Proc $srv5 -TimeoutSec 30
    if ($code5 -ne 6) { throw "WCT-5: once-multi probe-only expected exit 6, got $code5" }
    Write-Host "  OK once-multi probe-only timeout exit=$code5"

    Write-Host "[WCT-6] --once, accepted TCP connection held open w/o handshake -> still exit 6 (B-01)"
    $srv6 = Start-FastCloneProcess -Exe $exe -CliArgs @(
        "server", "--dir", $src, "--password", $password, "--port", "$Port",
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
    $srv7 = Start-FastCloneProcess -Exe $exe -CliArgs @(
        "server", "--dir", $src, "--password", $password, "--port", "$Port",
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
