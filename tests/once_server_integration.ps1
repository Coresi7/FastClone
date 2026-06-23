# FastClone --once (oneshot server) integration smoke tests (Windows).
# Usage: .\tests\once_server_integration.ps1 [-ExePath path\to\FastClone.exe] [-Port 27893]
# Creates small temp dirs; always kills FastClone processes on exit.
#
# Covers (design §5.2):
#   OS-1 success path   -> server --once auto-exits with code 0 (V-04 / AC-04/AC-08A/AC-12)
#   OS-2 failure path   -> served session aborted -> server exit 5 (V-05/V-06 / AC-05/AC-06)
#   OS-3 usage errors   -> client --once / --once+--enable-hash-memcache -> exit 1 (V-02/V-03)
#   OS-4 probe (opt.)   -> pre-handshake close does not exit the server (V-07 / AC-07)

param(
    [string]$ExePath = "",
    [int]$Port = 27893
)

$ErrorActionPreference = "Stop"

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
    # Touch .Handle to cache the native handle now; otherwise .ExitCode is "unavailable"
    # after a self-exiting process (e.g. server --once) terminates (classic PowerShell quirk).
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
    $srv1 = Start-FastCloneProcess -Exe $exe -CliArgs @(
        "server", "--dir", $src, "--password", $password, "--port", "$Port", "--once"
    ) -OutLog "$logDir\os1-server.out" -ErrLog "$logDir\os1-server.err"
    Start-Sleep -Seconds 1
    $code1c = Invoke-FastCloneSync -Exe $exe -CliArgs @(
        "client", "--server", $serverAddr, "--target", $tgt, "--password", $password
    ) -OutLog "$logDir\os1-client.out" -ErrLog "$logDir\os1-client.err"
    if ($code1c -ne 0) { throw "OS-1: client sync expected exit 0, got $code1c" }
    $code1 = Wait-ExitCode -Proc $srv1 -TimeoutSec 30
    if ($code1 -ne 0) { throw "OS-1: server --once expected exit 0, got $code1" }
    Write-Host "  OK server exit=$code1 (auto-exited, no kill)"

    Write-Host "[OS-5] AC-10: after first session terminal, a second client is refused"
    # The OS-1 server already auto-exited after its single real session (FR-10/FR-11): it is
    # not kept alive for a second session. A new client must therefore fail to connect rather
    # than be served (exit != 0). Guards against the server lingering for a second session.
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

    Write-Host "[OS-2] failure path -> client aborted mid-transfer -> server exit 5"
    $srv2 = Start-FastCloneProcess -Exe $exe -CliArgs @(
        "server", "--dir", $srcBig, "--password", $password, "--port", "$Port", "--once"
    ) -OutLog "$logDir\os2-server.out" -ErrLog "$logDir\os2-server.err"
    Start-Sleep -Seconds 1
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
    $code3a = Invoke-FastCloneSync -Exe $exe -CliArgs @(
        "client", "--server", $serverAddr, "--target", $tgt, "--password", $password, "--once"
    ) -OutLog "$logDir\os3a.out" -ErrLog "$logDir\os3a.err"
    if ($code3a -ne 1) { throw "OS-3: client --once expected exit 1, got $code3a" }
    $code3b = Invoke-FastCloneSync -Exe $exe -CliArgs @(
        "server", "--dir", $src, "--password", $password, "--once", "--enable-hash-memcache"
    ) -OutLog "$logDir\os3b.out" -ErrLog "$logDir\os3b.err"
    if ($code3b -ne 1) { throw "OS-3: --once + --enable-hash-memcache expected exit 1, got $code3b" }
    Write-Host "  OK client--once=$code3a  mutually-exclusive=$code3b"

    Write-Host "[OS-4] pre-handshake probe does not exit the server, real client still completes 0"
    $tgt4 = Join-Path $root "tgt4"
    New-Item -ItemType Directory -Force -Path $tgt4 | Out-Null
    $srv4 = Start-FastCloneProcess -Exe $exe -CliArgs @(
        "server", "--dir", $src, "--password", $password, "--port", "$Port", "--once"
    ) -OutLog "$logDir\os4-server.out" -ErrLog "$logDir\os4-server.err"
    Start-Sleep -Seconds 1
    # Probe: connect then immediately close without sending any bytes (session == null).
    $probe = New-Object System.Net.Sockets.TcpClient
    $probe.Connect("127.0.0.1", $Port)
    Start-Sleep -Milliseconds 200
    $probe.Close()
    Start-Sleep -Seconds 1
    if ($srv4.HasExited) { throw "OS-4: server exited on a pre-handshake probe (should stay up)" }
    $code4c = Invoke-FastCloneSync -Exe $exe -CliArgs @(
        "client", "--server", $serverAddr, "--target", $tgt4, "--password", $password
    ) -OutLog "$logDir\os4-client.out" -ErrLog "$logDir\os4-client.err"
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
    if ($failures -ne 0) { exit 1 }
}
