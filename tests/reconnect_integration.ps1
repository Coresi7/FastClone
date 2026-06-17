# FastClone reconnect integration smoke tests (Windows).
# Usage: .\tests\reconnect_integration.ps1 [-ExePath path\to\FastClone.exe]
# Creates small temp dirs; always kills FastClone processes on exit.

param(
    [string]$ExePath = "",
    [int]$Port = 27892
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
    foreach ($a in $CliArgs) {
        if ($null -eq $a -or $a -eq "") {
            throw "Start-FastCloneProcess: null/empty argument in: $($CliArgs -join ' | ')"
        }
    }
    $env:FASTCLONE_DEBUG = "1"
    return Start-Process -FilePath $Exe -ArgumentList ([string[]]$CliArgs) `
        -RedirectStandardOutput $OutLog -RedirectStandardError $ErrLog `
        -PassThru -NoNewWindow
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
    $srv1 = Start-FastCloneProcess -Exe $exe -CliArgs @(
        "server", "--dir", $src, "--password", $password, "--port", "$Port"
    ) -OutLog "$logDir\it1-server.out" -ErrLog "$logDir\it1-server.err"
    Start-Sleep -Seconds 1
    $code1 = Invoke-FastCloneSync -Exe $exe -CliArgs @(
        "client", "--server", $serverAddr, "--target", $tgt,
        "--password", "wrong-password", "--reconnect-retries", "10", "--reconnect-window", "5m"
    ) -OutLog "$logDir\it1-client.out" -ErrLog "$logDir\it1-client.err"
    Stop-Process -Id $srv1.Id -Force -ErrorAction SilentlyContinue
    if ($code1 -ne 1) { throw "IT-1: expected exit 1, got $code1" }
    Assert-NoReconnectAttempts -ErrLog "$logDir\it1-client.err" -Scenario "IT-1"
    Write-Host "  OK exit=$code1"

    Write-Host "[IT-2] server late start -> connect tolerance + reconnect log"
    Remove-Item -Recurse -Force $tgt -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force -Path $tgt | Out-Null
    $cli2 = Start-FastCloneProcess -Exe $exe -CliArgs @(
        "client", "--server", $serverAddr, "--target", $tgt,
        "--password", $password, "--reconnect-retries", "10", "--reconnect-window", "5m",
        "--streams", "1", "--chunk-kb", "1"
    ) -OutLog "$logDir\it2-client.out" -ErrLog "$logDir\it2-client.err"
    Start-Sleep -Seconds 5
    $srv2 = Start-FastCloneProcess -Exe $exe -CliArgs @(
        "server", "--dir", $src, "--password", $password, "--port", "$Port"
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
    Remove-Item -Recurse -Force $tgt -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force -Path $tgt | Out-Null
    $srv4 = Start-FastCloneProcess -Exe $exe -CliArgs @(
        "server", "--dir", $src, "--password", $password, "--port", "$Port"
    ) -OutLog "$logDir\it3-server.out" -ErrLog "$logDir\it3-server.err"
    Start-Sleep -Seconds 1
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
    Remove-Item -Recurse -Force $tgt -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force -Path $tgt | Out-Null
    $code4 = Invoke-FastCloneSync -Exe $exe -CliArgs @(
        "client", "--server", $serverAddr, "--target", $tgt,
        "--password", $password, "--reconnect-retries", "1", "--reconnect-window", "1m"
    ) -OutLog "$logDir\it4-client.out" -ErrLog "$logDir\it4-client.err"
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
