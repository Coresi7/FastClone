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
    [int]$BigSizeMb = 300,
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
        "$PSScriptRoot\..\build\Release\FastClone.exe",
        "$PSScriptRoot\..\x64\Release\FastClone.exe",
        "$PSScriptRoot\..\build\FastClone.exe"
    )
    foreach ($c in $candidates) {
        if (Test-Path $c) { return (Resolve-Path $c).Path }
    }
    throw "FastClone.exe not found; pass -ExePath"
}

# ---------------------------------------------------------------------------
# Task ci-integration-test-timeout (design section 3.6/3.7): PID-scoped cleanup + 480s
# global deadline. This script has NO Invoke-FastCloneSync/Wait-ExitCode/cmd-c path and its
# big WaitForExit(900000) stays untouched (RC-7 known boundary, annotated below).
# ---------------------------------------------------------------------------

# [I-2] Global wall-clock budget (diagnostic reserve; ctest TIMEOUT is 600 here).
$script:FcDeadlineUtc = (Get-Date).ToUniversalTime().AddSeconds(480)

function Get-FcRemainingSec {
    param([int]$ReserveSec = 30)
    $r = ($script:FcDeadlineUtc - (Get-Date).ToUniversalTime()).TotalSeconds - $ReserveSec
    if ($r -lt 0) { $r = 0 }
    return [int][Math]::Floor($r)
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

# Add-Type compiles C# in-process and inherits the caller's environment. Under MSBuild the
# LIB/LIBPATH/INCLUDE of the VS 2026 toolset contain a NON-EXISTENT entry
# ("...\MSVC\14.51.36231\atlmfc\lib\x64"); the C# compiler treats it as
# "Warning as Error: Invalid search path" and Add-Type throws -> MSB3073. Drop the three
# variables for the duration of the compile ONLY, then restore them (a variable that was
# unset stays unset). Mirrors tests/data_integrity_integration.ps1:113-133.
$fcSavedBuildEnv = @{}
foreach ($fcEnvVar in 'LIB', 'LIBPATH', 'INCLUDE') {
    $fcSavedBuildEnv[$fcEnvVar] = [Environment]::GetEnvironmentVariable($fcEnvVar)
    Remove-Item "Env:\$fcEnvVar" -ErrorAction SilentlyContinue
}
try {
    Add-Type -Namespace FcProbe -Name Psapi -MemberDefinition @'
[DllImport("psapi.dll", SetLastError = true)]
public static extern bool EmptyWorkingSet(System.IntPtr hProcess);
'@
}
finally {
    foreach ($fcEnvVar in @($fcSavedBuildEnv.Keys)) {
        if ($null -ne $fcSavedBuildEnv[$fcEnvVar]) {
            [Environment]::SetEnvironmentVariable($fcEnvVar, $fcSavedBuildEnv[$fcEnvVar])
        }
    }
}

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
    # $bindLoopbackArg from parent scope (empty in production, "--bind-loopback" in test mode);
    # filter empties so Start-Process never sees a stray blank argument.
    $srvArgs = @("server", "--dir", $src, "--password", $password, "--port", "$Port", $bindLoopbackArg) |
        Where-Object { $null -ne $_ -and $_ -ne "" }
    $srv = Start-Process -FilePath $exe `
        -ArgumentList ([string[]]$srvArgs) `
        -RedirectStandardOutput (Join-Path $logs "server.out") `
        -RedirectStandardError  (Join-Path $logs "server.err") `
        -PassThru -NoNewWindow
    $null = $srv.Handle
    $null = Register-OwnedProcess -Proc $srv -Tag "server"
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
        $null = Register-OwnedProcess -Proc $cli -Tag "client_$Stage"

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
            # RC-7 (KNOWN BOUNDARY, intentionally NOT fixed by task ci-integration-test-timeout):
            # this WaitForExit budget is 900s, which is LONGER than the ctest timeout (600s). If
            # the client ever stalls here, ctest hard-kills the whole powershell process, so
            # neither the catch block nor any diagnostic below ever runs -- the exact failure
            # mode that made #2/#6/#9 un-diagnosable. Out of scope here (requirements §7/§11
            # RC-7: "本任务不强制修，须如实标注"); tracked as a follow-up. This task only adds the
            # pre-wait marker so a future stall is at least timestamped.
            $elapsed = if ($sw) { [math]::Round($sw.Elapsed.TotalSeconds, 1) } else { -1 }
            Write-Host "[rc7] entering WaitForExit(900000) pid=$($cli.Id) elapsed_s=$elapsed"
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
