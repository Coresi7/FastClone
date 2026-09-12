# FastClone server memory-retention probe (Windows).
#
# Reproduces the operator report: "running as the server, when the client aborts mid-transfer the
# memory is not released (hash memcache disabled). It climbs while transferring and never comes
# back down, but it does not grow without bound across repeated completed/aborted runs."
#
# The probe runs a RESIDENT server and repeats: touch every source file (so the client cannot
# short-circuit on unchanged mtime and the server really re-reads each file), start a client, kill
# it mid-transfer, let the server tear the session down, then sample the server process. It
# separates the candidate explanations instead of guessing between them:
#   * peak vs post-teardown PrivateMemorySize64 -> how much of the climb is given back at all
#   * Private Bytes growth across rounds        -> a REAL leak (committed pages never released)
#   * WorkingSet64 vs after EmptyWorkingSet     -> memory that is merely RESIDENT (not a leak)
#   * HandleCount                               -> driver file handles no teardown path closes
#   * "[debug][server] driver_retention"        -> process-global DiskIoDriver bookkeeping, which
#                                                  is the one thing shared by ALL sessions
#
# Usage:
#   .\tests\server_memory_retention_probe.ps1 [-ExePath path\to\FastClone.exe]
#       [-Port 27931] [-Rounds 4] [-AbortAfterMs 2500]
#       [-SmallFiles 800] [-SmallSizeKb 64] [-BigFiles 2] [-BigSizeMb 300]
#
# Creates temp data + a temp target dir and always stops processes on exit.

param(
    [string]$ExePath = "",
    [int]$Port = 27931,
    [int]$Rounds = 4,
    [int]$AbortAfterMs = 2500,
    [int]$SmallFiles = 800,
    [int]$SmallSizeKb = 64,
    [int]$BigFiles = 2,
    [int]$BigSizeMb = 300
)

$ErrorActionPreference = "Stop"

function Resolve-FastCloneExe {
    param([string]$Hint)
    if ($Hint -and (Test-Path $Hint)) { return (Resolve-Path $Hint).Path }
    $candidates = @(
        "$PSScriptRoot\..\build\Release\FastClone.exe",
        "$PSScriptRoot\..\x64\Release\FastClone.exe",
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
    Start-Sleep -Milliseconds 700
}

function New-SparseFile {
    param([string]$Path, [long]$Size)
    $fs = [System.IO.File]::Create($Path)
    try { $fs.SetLength($Size) } finally { $fs.Close() }
}

function Get-Sample {
    param([System.Diagnostics.Process]$Proc)
    $Proc.Refresh()
    [pscustomobject]@{
        PrivateMB = [math]::Round($Proc.PrivateMemorySize64 / 1MB, 1)
        WorkingMB = [math]::Round($Proc.WorkingSet64 / 1MB, 1)
        Handles   = $Proc.HandleCount
        Threads   = $Proc.Threads.Count
    }
}

Add-Type -Namespace FcProbe -Name Psapi -MemberDefinition @'
[DllImport("psapi.dll", SetLastError = true)]
public static extern bool EmptyWorkingSet(System.IntPtr hProcess);
'@

# --- setup ---------------------------------------------------------------------------------------
$exe = Resolve-FastCloneExe -Hint $ExePath
Write-Host "FastClone: $exe"

$root = Join-Path $env:TEMP ("fc_memprobe_" + [DateTime]::Now.ToString("yyyyMMdd_HHmmss"))
$src  = Join-Path $root "src"
$tgt  = Join-Path $root "tgt"
$logs = Join-Path $root "logs"
New-Item -ItemType Directory -Force -Path $src, $tgt, $logs | Out-Null

$password = "probepw"
$serverAddr = "127.0.0.1:$Port"

try {
    Stop-AllFastClone

    Write-Host "Generating source data: $SmallFiles x ${SmallSizeKb}KB + $BigFiles x ${BigSizeMb}MB (sparse)"
    $small = New-Object byte[] ($SmallSizeKb * 1024)
    (New-Object System.Random 1234).NextBytes($small)
    for ($i = 0; $i -lt $SmallFiles; $i++) {
        [System.IO.File]::WriteAllBytes((Join-Path $src ("small_" + $i + ".bin")), $small)
    }
    for ($i = 0; $i -lt $BigFiles; $i++) {
        New-SparseFile -Path (Join-Path $src ("big_" + $i + ".bin")) -Size ([long]$BigSizeMb * 1MB)
    }

    # Force the client to treat every file as changed, so the server really re-reads (and
    # re-registers) each file on every round. Without this, round 2+ is a no-op and the
    # per-session bookkeeping cannot accumulate.
    function Touch-Source {
        $now = [DateTime]::Now
        Get-ChildItem -Path $src -File | ForEach-Object { $_.LastWriteTime = $now }
    }

    $env:FASTCLONE_DEBUG = "1"
    $srv = Start-Process -FilePath $exe `
        -ArgumentList @("server", "--dir", $src, "--password", $password, "--port", "$Port") `
        -RedirectStandardOutput (Join-Path $logs "server.out") `
        -RedirectStandardError  (Join-Path $logs "server.err") `
        -PassThru -NoNewWindow
    $null = $srv.Handle
    Start-Sleep -Seconds 2
    if ($srv.HasExited) { throw "server exited early; see $logs\server.err" }

    $rows = New-Object System.Collections.ArrayList

    function Add-Row {
        param([string]$Stage, [int]$Round, [double]$PeakPrivateMB, [double]$PeakWorkingMB)
        $s = Get-Sample -Proc $srv
        [void]$rows.Add([pscustomobject]@{
            Stage          = $Stage
            Round          = $Round
            PeakPrivateMB  = $PeakPrivateMB
            PeakWorkingMB  = $PeakWorkingMB
            RestPrivateMB  = $s.PrivateMB
            RestWorkingMB  = $s.WorkingMB
            Handles        = $s.Handles
            Threads        = $s.Threads
        })
    }

    # Run a client for $AbortAfterMs while sampling the server, then kill it (abort).
    function Invoke-Round {
        param([string]$Stage, [int]$Round, [int]$RunMs, [switch]$LetFinish)

        if (Test-Path $tgt) { Remove-Item -Recurse -Force $tgt -ErrorAction SilentlyContinue }
        New-Item -ItemType Directory -Force -Path $tgt | Out-Null
        Touch-Source

        $cli = Start-Process -FilePath $exe `
            -ArgumentList @("client", "--server", $serverAddr, "--target", $tgt, "--password", $password) `
            -RedirectStandardOutput (Join-Path $logs ("client_$Stage.out")) `
            -RedirectStandardError  (Join-Path $logs ("client_$Stage.err")) `
            -PassThru -NoNewWindow
        $null = $cli.Handle

        $peakPriv = 0.0
        $peakWork = 0.0
        $sw = [System.Diagnostics.Stopwatch]::StartNew()
        while ($sw.ElapsedMilliseconds -lt $RunMs) {
            $s = Get-Sample -Proc $srv
            if ($s.PrivateMB -gt $peakPriv) { $peakPriv = $s.PrivateMB }
            if ($s.WorkingMB -gt $peakWork) { $peakWork = $s.WorkingMB }
            if ($cli.HasExited) { break }
            Start-Sleep -Milliseconds 200
        }

        if ($LetFinish) {
            $null = $cli.WaitForExit(900000)
            Write-Host ("  {0}: client exit={1}" -f $Stage, $cli.ExitCode)
        } elseif (-not $cli.HasExited) {
            Stop-Process -Id $cli.Id -Force -ErrorAction SilentlyContinue
            Write-Host ("  {0}: client killed at {1}ms" -f $Stage, $sw.ElapsedMilliseconds)
        } else {
            Write-Host ("  {0}: client exited early ({1})" -f $Stage, $cli.ExitCode)
        }

        # Let the server finish tearing the session down (hash drain + closeFile sweep).
        Start-Sleep -Seconds 4
        Add-Row -Stage $Stage -Round $Round -PeakPrivateMB $peakPriv -PeakWorkingMB $peakWork
    }

    $base = Get-Sample -Proc $srv
    Write-Host ("  baseline: private={0}MB working={1}MB handles={2}" -f $base.PrivateMB, $base.WorkingMB, $base.Handles)

    for ($r = 1; $r -le $Rounds; $r++) {
        Invoke-Round -Stage "abort" -Round $r -RunMs $AbortAfterMs
    }
    Invoke-Round -Stage "complete" -Round ($Rounds + 1) -RunMs $AbortAfterMs -LetFinish
    Invoke-Round -Stage "complete2" -Round ($Rounds + 2) -RunMs $AbortAfterMs -LetFinish

    # Distinguish "pages merely resident" from "memory really still allocated":
    # EmptyWorkingSet trims the working set; commit (Private Bytes) is untouched by it.
    [void][FcProbe.Psapi]::EmptyWorkingSet($srv.Handle)
    Start-Sleep -Milliseconds 700
    $trimmed = Get-Sample -Proc $srv

    Write-Host ""
    Write-Host "=== server process samples (Peak = during the run, Rest = after teardown) ==="
    $rows | Format-Table -AutoSize | Out-String -Width 200 | Write-Host

    $aborts = @($rows | Where-Object { $_.Stage -eq "abort" })
    $full1 = $rows | Where-Object { $_.Stage -eq "complete" } | Select-Object -First 1
    $full2 = $rows | Where-Object { $_.Stage -eq "complete2" } | Select-Object -First 1
    $lastAbortRest = if ($aborts.Count -gt 0) { $aborts[-1].RestPrivateMB } else { $base.PrivateMB }

    Write-Host "=== verdict ==="
    Write-Host ("resting private_bytes  baseline={0}MB  after_{1}_aborts={2}MB  after_complete={3}MB  after_complete2={4}MB" -f `
        $base.PrivateMB, $aborts.Count, $lastAbortRest, $full1.RestPrivateMB, $full2.RestPrivateMB)
    Write-Host ("peak vs rest (complete2) peak_private={0}MB -> rest={1}MB (given back {2}MB)" -f `
        $full2.PeakPrivateMB, $full2.RestPrivateMB, ($full2.PeakPrivateMB - $full2.RestPrivateMB))
    Write-Host ("working_set after_complete2={0}MB  after_EmptyWorkingSet={1}MB  trimmable={2}MB" -f `
        $full2.RestWorkingMB, $trimmed.WorkingMB, ($full2.RestWorkingMB - $trimmed.WorkingMB))
    Write-Host ("handles baseline={0} after_complete2={1} delta={2}" -f `
        $base.Handles, $full2.Handles, ($full2.Handles - $base.Handles))

    Write-Host ""
    Write-Host "=== per-session driver retention (server stderr) ==="
    if (Test-Path (Join-Path $logs "server.err")) {
        Get-Content (Join-Path $logs "server.err") |
            Where-Object { $_ -match "driver_retention" } |
            ForEach-Object { Write-Host "  $_" }
    }

    Write-Host ""
    Write-Host "logs: $logs"
} finally {
    Stop-AllFastClone
    Write-Host "cleanup: processes stopped; data kept at $root"
}
