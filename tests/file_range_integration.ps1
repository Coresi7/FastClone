# FastClone large-file block (file-range) integration test (Windows)
# — T-largefile-block-multinic.
#
# Exercises the --large-file-block mode (multi-NIC block fan-out) end to end. Since
# T-largefile-block-auto-default the mode is three-state and defaults to AUTO (flag absent /
# no value / the literal value auto): it activates whenever the server advertises the
# file-range capability AND >=2 healthy links are up AND --large-file-lane is not given.
#   FR-A  block mode ON, 2 loopback lanes: large files fan out as blocks across BOTH lanes,
#         H1 hash prefetch + whole-file XXH3 verify + atomic rename; SHA256 bit-exact.
#         Also covers V-10: one file is an exact block multiple, one has a short tail block.
#   FR-B  block mode OFF via the explicit `--large-file-block off` form, 2 lanes: legacy
#         single-stream large file, zero regression (no file_range frames in the log).
#   FR-C  block mode ON but only 1 lane: gate falls back to the legacy path (AC-05).
#   FR-D  block mode ON, 2 lanes, lane 2 through a throttled TCP proxy; the proxy is killed
#         mid-transfer -> in-flight blocks reroute to the healthy lane and are re-fetched
#         whole (idempotent, AC-03); final bytes stay SHA256 bit-exact.
#   FR-E  block mode via the NO-VALUE form (`--large-file-block` alone = AUTO, default 32 MiB
#         reference block), 2 lanes: auto activates block mode end to end (C-4).
#   FR-F  block mode ON + delta ON, 2 lanes: a large file whose local old version differs >65%
#         from the server is admitted into delta then benefit-rejected (too different); the
#         fallback is routed through block mode (kind=file_range) instead of single-stream.
#   FR-G  block mode ON, 2 lanes, batch pressure (300 small files): reserved lane slots keep
#         file_range blocks allocatable on >=2 lanes (no batch starvation), block admission
#         sends NO H1 HashRequest prefetch (client hash_req_sent == 0), and structural
#         finalize validation still delivers SHA256-bit-exact bytes
#         (T-block-lane-quota-and-h1-hash).
#   FR-H  block mode ABSENT (default auto), 2 lanes: auto-activation with the 32 MiB
#         reference block (plan shows block=33554432), blocks fan out on >=2 lanes,
#         SHA256 bit-exact (T-largefile-block-auto-default AC-01/AC-03).
#   FR-I  default auto + explicit --large-file-lane -> treated as off (C-3 case 1): no
#         file_range frames, the lane-pinned legacy route still applies, exit 0 (AC-19).
#   FR-J  --large-file-block 8M + --large-file-lane -> parse-time rejection (C-3 case 2):
#         non-zero exit and the error message names BOTH flags (AC-20).
#
# Usage: .\tests\file_range_integration.ps1 [-ExePath path\to\FastClone.exe] [-Port 27910]

param(
    [string]$ExePath = "",
    [int]$Port = 27910,
    # Test-only: bind server to ::1 (loopback) to isolate from LAN/WAN traffic (OM-4 flaky root cause).
    [switch]$BindLoopback
)

$ErrorActionPreference = "Stop"

# Optional server arg fragment: empty in production, "--bind-loopback" in test mode.
$bindLoopbackArg = if ($BindLoopback) { "--bind-loopback" } else { "" }

function Resolve-FastCloneExe {
    param([string]$Hint)
    if ($Hint -and (Test-Path $Hint)) { return (Resolve-Path $Hint).Path }
    # Prefer the CMake build output (what build_and_test.ps1 builds/tests); the legacy VS
    # x64\Release artifact may be stale and lack the --large-file-block flag.
    $candidates = @(
        "$PSScriptRoot\..\build\Release\FastClone.exe",
        "$PSScriptRoot\..\build\FastClone.exe",
        "$PSScriptRoot\..\x64\Release\FastClone.exe"
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
    param([System.Diagnostics.Process]$Proc, [int]$TimeoutSec = 120, [string]$StageTag = "wait")
    # [I-2] v2 (R-06): Wait-ExitCode is one of the LARGEST single wait budgets in these
    # scripts (FR-D uses 300s for the client). Without clamping it to the global deadline,
    # I-2 does not close: a full-budget wait plus a later stage stall can reach ctest's 600s
    # hard kill before any diagnostic.
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
    $env:FASTCLONE_DEBUG = "1"
    $proc = Start-Process -FilePath $Exe -ArgumentList ([string[]]$CliArgs) `
        -RedirectStandardOutput $OutLog -RedirectStandardError $ErrLog `
        -PassThru -NoNewWindow
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
        [int]$TimeoutSec = 180,          # tier T1 (file_range), see design section 3.2
        [string]$StageTag = "client"     # NFR-03: goes into the diagnostic block
    )
    foreach ($a in $CliArgs) {
        if ($null -eq $a -or $a -eq "") { throw "Invoke-FastCloneSync: null/empty argument" }
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

$longPathSig = @'
[DllImport("kernel32.dll", CharSet=CharSet.Auto, SetLastError=true)]
public static extern int GetLongPathName(string lpszShortPath, System.Text.StringBuilder lpszLongPath, int cchBuffer);
'@
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
    $longPathUtil = Add-Type -MemberDefinition $longPathSig -Name 'LongPathUtilFR' -Namespace 'FcDiag' -PassThru
}
finally {
    foreach ($fcEnvVar in @($fcSavedBuildEnv.Keys)) {
        if ($null -ne $fcSavedBuildEnv[$fcEnvVar]) {
            [Environment]::SetEnvironmentVariable($fcEnvVar, $fcSavedBuildEnv[$fcEnvVar])
        }
    }
}
function Get-LongPath {
    param([string]$Path)
    $sb = New-Object System.Text.StringBuilder 4096
    $r = $longPathUtil::GetLongPathName($Path, $sb, $sb.Capacity)
    if ($r -gt 0 -and $r -le $sb.Capacity) { return $sb.ToString() }
    return $Path
}

function Get-FileHashTree {
    param([string]$Root)
    $hashes = @{}
    $normRoot = Get-LongPath -Path $Root
    Get-ChildItem -Path $Root -Recurse -File -ErrorAction SilentlyContinue | ForEach-Object {
        $full = Get-LongPath -Path $_.FullName
        $rel = $full.Substring($normRoot.Length).TrimStart('\', '/')
        $hashes[$rel] = (Get-FileHash -Path $full -Algorithm SHA256).Hash
    }
    return $hashes
}

function Compare-FileHashTrees {
    param([hashtable]$Src, [hashtable]$Tgt)
    if ($Src.Count -ne $Tgt.Count) {
        return "file count mismatch: src=$($Src.Count) tgt=$($Tgt.Count)"
    }
    foreach ($rel in $Src.Keys) {
        if (-not $Tgt.ContainsKey($rel)) {
            return "missing in target: $rel"
        }
        if ($Src[$rel] -ne $Tgt[$rel]) {
            return "hash mismatch: $rel`n  src=$($Src[$rel])`n  tgt=$($Tgt[$rel])"
        }
    }
    return $null
}

# Deterministic content generator (byte pattern seeded per file so any cross-block or
# cross-stream leakage flips the SHA256).
function New-PatternFile {
    param([string]$Path, [long]$Size, [int]$Seed)
    $f = [System.IO.File]::Create($Path)
    try {
        $bufSize = 4 * 1024 * 1024
        $buf = New-Object byte[] $bufSize
        $written = 0L
        while ($written -lt $Size) {
            $take = [Math]::Min([long]$bufSize, $Size - $written)
            for ($i = 0; $i -lt $take; $i++) { $buf[$i] = [byte](($Seed + $written + $i) % 251) }
            $f.Write($buf, 0, [int]$take)
            $written += $take
        }
    } finally { $f.Close() }
}

# Fixture: small files (batch path) + two large files above the 8 MiB test threshold.
# Block size is 8 MiB in every block-mode variant:
#   large_exact.bin  = 16 MiB exactly -> 2 full blocks, no tail (V-10 exact multiple)
#   large_tail.bin   = 20 MiB + 123 bytes -> 2 full blocks + 123-byte tail block (V-10 tail)
function New-TestFixture {
    param([string]$Src, [int]$TailFileMiB = 20)
    Set-Content -Path (Join-Path $Src "notes.txt") -Value "small batch file" -NoNewline
    New-PatternFile -Path (Join-Path $Src "small_64k.bin") -Size 65536 -Seed 11
    New-PatternFile -Path (Join-Path $Src "large_exact.bin") -Size (16 * 1024 * 1024) -Seed 57
    New-PatternFile -Path (Join-Path $Src "large_tail.bin") -Size ([long]$TailFileMiB * 1024 * 1024 + 123) -Seed 93
}

# Count distinct connIds seen on "[mp] alloc kind=file_range" lines.
function Get-FileRangeConnIds {
    param([string]$LogPath)
    $ids = @{}
    if (Test-Path $LogPath) {
        Select-String -Path $LogPath -Pattern 'kind=file_range connId=(\d+)' |
            ForEach-Object { $_.Matches[0].Groups[1].Value } |
            ForEach-Object { $ids[$_] = $true }
    }
    return $ids.Keys
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

$exe = Resolve-FastCloneExe -Hint $ExePath
$root = Join-Path $env:TEMP "fastclone-file-range-$(Get-Random)"
$logDir = Join-Path $root "logs"
$src = Join-Path $root "src"
New-Item -ItemType Directory -Force -Path $logDir, $src | Out-Null

$password = "fc-fr-pw"
$serverAddr = "127.0.0.1:$Port"
$blockArgs = @("--large-file-threshold", "8M", "--large-file-block", "8M")
$twoLinks = @("--link", "127.0.0.1=127.0.0.1:$Port", "--link", "127.0.0.2=127.0.0.1:$Port")
$oneLink = @("--link", "127.0.0.1=127.0.0.1:$Port")

try {
    Stop-AllFastClone

    Write-Host "Building test fixture..."
    New-TestFixture -Src $src
    $srcHashes = Get-FileHashTree -Root $src

    # ============================ FR-A: block ON, 2 lanes ============================
    Write-Host ""
    Write-Host "[FR-A] block mode ON, 2 loopback lanes"
    Assert-FcDeadline -Stage "FR-A"
    $tgt = Join-Path $env:TEMP "fc-fr-tgt-a-$(Get-Random)"
    New-Item -ItemType Directory -Force -Path $tgt | Out-Null
    if (-not (Wait-PortFree -PortNum $Port -TimeoutSec 15)) {
        Write-Host "[PORT-BUSY] port $Port still held before FR-A server start:"
        (netstat -ano | Select-String -Pattern ":$Port ") | ForEach-Object { Write-Host "    $($_.Line)" }
    }
    $srv = Start-FastCloneProcess -Exe $exe -CliArgs @(
        "server", "--dir", $src, "--password", $password, "--port", "$Port", $bindLoopbackArg, "--once"
    ) -OutLog "$logDir\A-server.out" -ErrLog "$logDir\A-server.err"
    if (-not (Wait-PortListening -PortNum $Port -Proc $srv -TimeoutSec 15)) {
        Write-FcDiag -StageTag "FR-A/server-not-listening" -Reason "server-bind" -TimeoutSec 15 `
            -Proc $srv -OutLog "$logDir\A-server.out" -ErrLog "$logDir\A-server.err"
        throw "FR-A: server pid=$($srv.Id) never reached LISTENING on port $Port within 15s"
    }
    # NOTE: build the client arg array FIRST — a compound "@(...) + $x" expression as a
    # cmdlet argument is silently truncated by PowerShell's argument-mode parser.
    $clientArgsA = @("client", "--server", $serverAddr, "--target", $tgt,
                     "--password", $password) + $twoLinks + $blockArgs
    $code = Invoke-FastCloneSync -Exe $exe -CliArgs $clientArgsA -OutLog "$logDir\A-client.out" -ErrLog "$logDir\A-client.err" -TimeoutSec 180 -StageTag "FR-A"
    if ($code -ne 0) { throw "FR-A: client expected exit 0, got $code" }
    $srvCode = Wait-ExitCode -Proc $srv -TimeoutSec 120
    if ($srvCode -ne 0) { throw "FR-A: server --once expected exit 0, got $srvCode" }
    $diff = Compare-FileHashTrees -Src $srcHashes -Tgt (Get-FileHashTree -Root $tgt)
    if ($null -ne $diff) { throw "FR-A: DATA INTEGRITY FAILURE: $diff" }
    $frConns = Get-FileRangeConnIds -LogPath "$logDir\A-client.out"
    if ($frConns.Count -lt 2) {
        throw "FR-A: expected file_range blocks on >=2 lanes, got connIds: $($frConns -join ',')"
    }
    if (-not (Select-String -Path "$logDir\A-client.out" -Pattern '\[file_range\] done' -Quiet)) {
        throw "FR-A: missing '[file_range] done' (block finalize log)"
    }
    if (Select-String -Path "$logDir\A-client.out" -Pattern 'file_range_finalize_failed' -Quiet) {
        throw "FR-A: unexpected finalize failure"
    }
    Write-Host "  OK FR-A: $($srcHashes.Count) files SHA256-match; file_range lanes: $($frConns -join ',')"
    Remove-Item -Recurse -Force $tgt -ErrorAction SilentlyContinue

    # ================= FR-B: block OFF (explicit off form), 2 lanes ==================
    # Since T-largefile-block-auto-default the DEFAULT is auto (which would activate on 2
    # lanes), so the legacy regression path is pinned via the explicit off form instead
    # (AC-07/AC-22): --large-file-block off keeps block mode unconditionally inactive.
    Write-Host ""
    Write-Host "[FR-B] block mode OFF (explicit --large-file-block off), 2 loopback lanes"
    Assert-FcDeadline -Stage "FR-B"
    $tgt = Join-Path $env:TEMP "fc-fr-tgt-b-$(Get-Random)"
    New-Item -ItemType Directory -Force -Path $tgt | Out-Null
    if (-not (Wait-PortFree -PortNum $Port -TimeoutSec 15)) {
        Write-Host "[PORT-BUSY] port $Port still held before FR-B server start:"
        (netstat -ano | Select-String -Pattern ":$Port ") | ForEach-Object { Write-Host "    $($_.Line)" }
    }
    $srv = Start-FastCloneProcess -Exe $exe -CliArgs @(
        "server", "--dir", $src, "--password", $password, "--port", "$Port", $bindLoopbackArg, "--once"
    ) -OutLog "$logDir\B-server.out" -ErrLog "$logDir\B-server.err"
    if (-not (Wait-PortListening -PortNum $Port -Proc $srv -TimeoutSec 15)) {
        Write-FcDiag -StageTag "FR-B/server-not-listening" -Reason "server-bind" -TimeoutSec 15 `
            -Proc $srv -OutLog "$logDir\B-server.out" -ErrLog "$logDir\B-server.err"
        throw "FR-B: server pid=$($srv.Id) never reached LISTENING on port $Port within 15s"
    }
    $clientArgsB = @("client", "--server", $serverAddr, "--target", $tgt,
                     "--password", $password) + $twoLinks + @("--large-file-threshold", "8M",
                     "--large-file-block", "off")
    $code = Invoke-FastCloneSync -Exe $exe -CliArgs $clientArgsB -OutLog "$logDir\B-client.out" -ErrLog "$logDir\B-client.err" -TimeoutSec 180 -StageTag "FR-B"
    if ($code -ne 0) { throw "FR-B: client expected exit 0, got $code" }
    $srvCode = Wait-ExitCode -Proc $srv -TimeoutSec 120
    if ($srvCode -ne 0) { throw "FR-B: server --once expected exit 0, got $srvCode" }
    $diff = Compare-FileHashTrees -Src $srcHashes -Tgt (Get-FileHashTree -Root $tgt)
    if ($null -ne $diff) { throw "FR-B: DATA INTEGRITY FAILURE: $diff" }
    if (Select-String -Path "$logDir\B-client.out" -Pattern 'kind=file_range' -Quiet) {
        throw "FR-B: unexpected kind=file_range with the switch OFF (AC-07 regression)"
    }
    if (Select-String -Path "$logDir\B-client.out" -Pattern '\[file_range\]' -Quiet) {
        throw "FR-B: unexpected [file_range] lines with the switch OFF (AC-07 regression)"
    }
    Write-Host "  OK FR-B: explicit off keeps the legacy path (no file_range frames), SHA256 match"
    Remove-Item -Recurse -Force $tgt -ErrorAction SilentlyContinue

    # ============================ FR-C: block ON, 1 lane =============================
    Write-Host ""
    Write-Host "[FR-C] block mode ON, single lane (AC-05 fallback)"
    Assert-FcDeadline -Stage "FR-C"
    $tgt = Join-Path $env:TEMP "fc-fr-tgt-c-$(Get-Random)"
    New-Item -ItemType Directory -Force -Path $tgt | Out-Null
    if (-not (Wait-PortFree -PortNum $Port -TimeoutSec 15)) {
        Write-Host "[PORT-BUSY] port $Port still held before FR-C server start:"
        (netstat -ano | Select-String -Pattern ":$Port ") | ForEach-Object { Write-Host "    $($_.Line)" }
    }
    $srv = Start-FastCloneProcess -Exe $exe -CliArgs @(
        "server", "--dir", $src, "--password", $password, "--port", "$Port", $bindLoopbackArg, "--once"
    ) -OutLog "$logDir\C-server.out" -ErrLog "$logDir\C-server.err"
    if (-not (Wait-PortListening -PortNum $Port -Proc $srv -TimeoutSec 15)) {
        Write-FcDiag -StageTag "FR-C/server-not-listening" -Reason "server-bind" -TimeoutSec 15 `
            -Proc $srv -OutLog "$logDir\C-server.out" -ErrLog "$logDir\C-server.err"
        throw "FR-C: server pid=$($srv.Id) never reached LISTENING on port $Port within 15s"
    }
    $clientArgsC = @("client", "--server", $serverAddr, "--target", $tgt,
                     "--password", $password) + $oneLink + $blockArgs
    $code = Invoke-FastCloneSync -Exe $exe -CliArgs $clientArgsC -OutLog "$logDir\C-client.out" -ErrLog "$logDir\C-client.err" -TimeoutSec 180 -StageTag "FR-C"
    if ($code -ne 0) { throw "FR-C: client expected exit 0, got $code" }
    $srvCode = Wait-ExitCode -Proc $srv -TimeoutSec 120
    if ($srvCode -ne 0) { throw "FR-C: server --once expected exit 0, got $srvCode" }
    $diff = Compare-FileHashTrees -Src $srcHashes -Tgt (Get-FileHashTree -Root $tgt)
    if ($null -ne $diff) { throw "FR-C: DATA INTEGRITY FAILURE: $diff" }
    if (Select-String -Path "$logDir\C-client.out" -Pattern 'kind=file_range' -Quiet) {
        throw "FR-C: unexpected kind=file_range with a single lane (AC-05 regression)"
    }
    Write-Host "  OK FR-C: single-lane fallback to legacy path, SHA256 match"
    Remove-Item -Recurse -Force $tgt -ErrorAction SilentlyContinue

    # ============ FR-E: NO-VALUE form = AUTO, 2 lanes (auto-activates) ==============
    # Since T-largefile-block-auto-default (C-4) the no-value form `--large-file-block`
    # classifies as AUTO (not force-on): with 2 healthy lanes + this server build the auto
    # gate activates block mode with the default 32 MiB reference block, and completes end
    # to end with a clean verify + rename. Assertion shapes (multi connId + done + SHA256)
    # are unchanged from the pre-auto semantics (AC-22/AC-06).
    Write-Host ""
    Write-Host "[FR-E] no-value form = auto (default 32M), auto-activates on 2 loopback lanes"
    Assert-FcDeadline -Stage "FR-E"
    $tgt = Join-Path $env:TEMP "fc-fr-tgt-e-$(Get-Random)"
    New-Item -ItemType Directory -Force -Path $tgt | Out-Null
    if (-not (Wait-PortFree -PortNum $Port -TimeoutSec 15)) {
        Write-Host "[PORT-BUSY] port $Port still held before FR-E server start:"
        (netstat -ano | Select-String -Pattern ":$Port ") | ForEach-Object { Write-Host "    $($_.Line)" }
    }
    $srv = Start-FastCloneProcess -Exe $exe -CliArgs @(
        "server", "--dir", $src, "--password", $password, "--port", "$Port", $bindLoopbackArg, "--once"
    ) -OutLog "$logDir\E-server.out" -ErrLog "$logDir\E-server.err"
    if (-not (Wait-PortListening -PortNum $Port -Proc $srv -TimeoutSec 15)) {
        Write-FcDiag -StageTag "FR-E/server-not-listening" -Reason "server-bind" -TimeoutSec 15 `
            -Proc $srv -OutLog "$logDir\E-server.out" -ErrLog "$logDir\E-server.err"
        throw "FR-E: server pid=$($srv.Id) never reached LISTENING on port $Port within 15s"
    }
    # No-value form: `--large-file-block` with no size -> AUTO with the default 32 MiB
    # reference block (C-4); the auto gate activates it here (2 lanes + capable server).
    $clientArgsE = @("client", "--server", $serverAddr, "--target", $tgt,
                     "--password", $password) + $twoLinks + @("--large-file-threshold", "8M", "--large-file-block")
    $code = Invoke-FastCloneSync -Exe $exe -CliArgs $clientArgsE -OutLog "$logDir\E-client.out" -ErrLog "$logDir\E-client.err" -TimeoutSec 180 -StageTag "FR-E"
    if ($code -ne 0) { throw "FR-E: client expected exit 0, got $code" }
    $srvCode = Wait-ExitCode -Proc $srv -TimeoutSec 120
    if ($srvCode -ne 0) { throw "FR-E: server --once expected exit 0, got $srvCode" }
    $diff = Compare-FileHashTrees -Src $srcHashes -Tgt (Get-FileHashTree -Root $tgt)
    if ($null -ne $diff) { throw "FR-E: DATA INTEGRITY FAILURE: $diff" }
    $frConnsE = Get-FileRangeConnIds -LogPath "$logDir\E-client.out"
    if ($frConnsE.Count -lt 1) {
        throw "FR-E: expected file_range blocks with the no-value switch, got connIds: $($frConnsE -join ',')"
    }
    if (-not (Select-String -Path "$logDir\E-client.out" -Pattern '\[file_range\] done' -Quiet)) {
        throw "FR-E: missing '[file_range] done' (block finalize log)"
    }
    if (Select-String -Path "$logDir\E-client.out" -Pattern 'file_range_finalize_failed' -Quiet) {
        throw "FR-E: unexpected finalize failure"
    }
    Write-Host "  OK FR-E: no-value = auto, block mode auto-activated (32M default), SHA256 match; lanes: $($frConnsE -join ',')"
    Remove-Item -Recurse -Force $tgt -ErrorAction SilentlyContinue

    # ================= FR-D: block ON, 2 lanes, kill lane mid-transfer ================
    Write-Host ""
    Write-Host "[FR-D] block mode ON, 2 lanes, kill lane 2 mid-transfer (AC-03 reroute)"
    Assert-FcDeadline -Stage "FR-D"
    # Bigger fixture for a deterministic transfer window: rebuild the tail file at 192 MiB.
    New-PatternFile -Path (Join-Path $src "large_tail.bin") -Size (192L * 1024 * 1024 + 123) -Seed 93
    $srcHashes = Get-FileHashTree -Root $src
    $proxyPort = $Port + 1
    $proxy = Start-Process -FilePath "powershell" -ArgumentList @(
        "-ExecutionPolicy", "Bypass", "-File",
        (Join-Path $PSScriptRoot "file_range_tcp_proxy.ps1"),
        "-ListenPort", "$proxyPort", "-TargetPort", "$Port",
        "-DownlinkBytesPerSec", "12582912"   # 12 MiB/s, keeps lane 2 busy for seconds
    ) -RedirectStandardOutput "$logDir\D-proxy.out" -RedirectStandardError "$logDir\D-proxy.err" `
      -PassThru -NoNewWindow
    $null = $proxy.Handle
    # Wait for the proxy to report ready.
    $proxyReady = $false
    for ($i = 0; $i -lt 50; $i++) {
        if ((Test-Path "$logDir\D-proxy.out") -and
            (Select-String -Path "$logDir\D-proxy.out" -Pattern 'PROXY_READY' -Quiet)) {
            $proxyReady = $true
            break
        }
        Start-Sleep -Milliseconds 200
    }
    if (-not $proxyReady) { throw "FR-D: proxy did not become ready" }

    $tgt = Join-Path $env:TEMP "fc-fr-tgt-d-$(Get-Random)"
    New-Item -ItemType Directory -Force -Path $tgt | Out-Null
    # Resident server (no --once): a killed lane marks the session hadError by design
    # (FR-07), so the --once verdict is intentionally not asserted here.
    if (-not (Wait-PortFree -PortNum $Port -TimeoutSec 15)) {
        Write-Host "[PORT-BUSY] port $Port still held before FR-D server start:"
        (netstat -ano | Select-String -Pattern ":$Port ") | ForEach-Object { Write-Host "    $($_.Line)" }
    }
    $srv = Start-FastCloneProcess -Exe $exe -CliArgs @(
        "server", "--dir", $src, "--password", $password, "--port", "$Port", $bindLoopbackArg
    ) -OutLog "$logDir\D-server.out" -ErrLog "$logDir\D-server.err"
    if (-not (Wait-PortListening -PortNum $Port -Proc $srv -TimeoutSec 15)) {
        Write-FcDiag -StageTag "FR-D/server-not-listening" -Reason "server-bind" -TimeoutSec 15 `
            -Proc $srv -OutLog "$logDir\D-server.out" -ErrLog "$logDir\D-server.err"
        throw "FR-D: server pid=$($srv.Id) never reached LISTENING on port $Port within 15s"
    }
    $killLinks = @("--link", "127.0.0.1=127.0.0.1:$Port",
                   "--link", "127.0.0.2=127.0.0.1:$proxyPort")
    $clientArgsD = @("client", "--server", $serverAddr, "--target", $tgt,
                     "--password", $password) + $killLinks + $blockArgs
    $clientProc = Start-FastCloneProcess -Exe $exe -CliArgs $clientArgsD -OutLog "$logDir\D-client.out" -ErrLog "$logDir\D-client.err"

    # Kill the proxy once the block fan-out is actually running (plan logged + blocks alloc'd).
    $fanoutSeen = $false
    for ($i = 0; $i -lt 150; $i++) {
        if ((Test-Path "$logDir\D-client.out") -and
            (Select-String -Path "$logDir\D-client.out" -Pattern 'kind=file_range' -Quiet)) {
            $fanoutSeen = $true
            break
        }
        if ($clientProc.HasExited) { break }
        Start-Sleep -Milliseconds 200
    }
    if (-not $fanoutSeen) { throw "FR-D: block fan-out never started (no kind=file_range)" }
    Start-Sleep -Seconds 2   # let lane 2 get mid-block
    Stop-Process -Id $proxy.Id -Force
    Write-Host "  proxy killed (lane 2 down); waiting for reroute + completion..."

    $clientCode = Wait-ExitCode -Proc $clientProc -TimeoutSec 300
    if ($clientCode -ne 0) { throw "FR-D: client expected exit 0 after lane kill, got $clientCode" }
    Stop-Process -Id $srv.Id -Force -ErrorAction SilentlyContinue

    $diff = Compare-FileHashTrees -Src $srcHashes -Tgt (Get-FileHashTree -Root $tgt)
    if ($null -ne $diff) { throw "FR-D: DATA INTEGRITY FAILURE after lane kill: $diff" }
    if (-not (Select-String -Path "$logDir\D-client.out" -Pattern '\[mp\] conn_down' -Quiet)) {
        throw "FR-D: expected '[mp] conn_down' after the lane kill"
    }
    $downLine = Select-String -Path "$logDir\D-client.out" -Pattern 'requeued_files=(\d+)' |
        Select-Object -First 1
    if (-not $downLine -or [int]$downLine.Matches[0].Groups[1].Value -lt 1) {
        throw "FR-D: expected requeued_files>=1 (in-flight blocks rerouted)"
    }
    # Block-level re-send proof: 2 + 25 blocks planned (large_tail is 24 full blocks + a
    # 123-byte tail block); more allocs than planned means killed in-flight blocks were
    # re-issued on the surviving lane (not a whole-file restart).
    $allocCount = (Select-String -Path "$logDir\D-client.out" -Pattern 'kind=file_range').Count
    if ($allocCount -le 27) {
        throw "FR-D: expected >27 file_range allocs (27 planned + re-sent blocks), got $allocCount"
    }
    if (-not (Select-String -Path "$logDir\D-client.out" -Pattern '\[file_range\] done' -Quiet)) {
        throw "FR-D: missing '[file_range] done' after reroute"
    }
    if (Select-String -Path "$logDir\D-client.out" -Pattern 'file_range_finalize_failed' -Quiet) {
        throw "FR-D: unexpected finalize failure after reroute"
    }
    Write-Host "  OK FR-D: lane kill -> block reroute, all SHA256 match (allocs=$allocCount)"
    Remove-Item -Recurse -Force $tgt -ErrorAction SilentlyContinue

    # ================ FR-F: delta too-different -> routed to block mode ================
    # T-largefile-block-multinic follow-up: a large file whose LOCAL old version differs >65%
    # from the server's new version is admitted into delta, then benefit-rejected (too different).
    # The fix must route that fallback through block mode so the full retransfer fans out across
    # BOTH NICs (kind=file_range) instead of falling back to a single-stream full transfer.
    Write-Host ""
    Write-Host "[FR-F] delta too-different fallback -> block mode, 2 loopback lanes"
    Assert-FcDeadline -Stage "FR-F"
    # (a) First sync (plain, no delta/block) to materialize the local old copy.
    $tgtF = Join-Path $env:TEMP "fc-fr-tgt-f-$(Get-Random)"
    New-Item -ItemType Directory -Force -Path $tgtF | Out-Null
    if (-not (Wait-PortFree -PortNum $Port -TimeoutSec 15)) {
        Write-Host "[PORT-BUSY] port $Port still held before FR-F server start:"
        (netstat -ano | Select-String -Pattern ":$Port ") | ForEach-Object { Write-Host "    $($_.Line)" }
    }
    $srvF1 = Start-FastCloneProcess -Exe $exe -CliArgs @(
        "server", "--dir", $src, "--password", $password, "--port", "$Port", $bindLoopbackArg, "--once"
    ) -OutLog "$logDir\F-server1.out" -ErrLog "$logDir\F-server1.err"
    if (-not (Wait-PortListening -PortNum $Port -Proc $srvF1 -TimeoutSec 15)) {
        Write-FcDiag -StageTag "FR-F/server1-not-listening" -Reason "server-bind" -TimeoutSec 15 `
            -Proc $srvF1 -OutLog "$logDir\F-server1.out" -ErrLog "$logDir\F-server1.err"
        throw "FR-F: server pid=$($srvF1.Id) never reached LISTENING on port $Port within 15s"
    }
    $clientArgsF1 = @("client", "--server", $serverAddr, "--target", $tgtF,
                      "--password", $password) + $twoLinks
    $codeF1 = Invoke-FastCloneSync -Exe $exe -CliArgs $clientArgsF1 -OutLog "$logDir\F-client1.out" -ErrLog "$logDir\F-client1.err" -TimeoutSec 180 -StageTag "FR-F"
    if ($codeF1 -ne 0) { throw "FR-F: first sync expected exit 0, got $codeF1" }
    $srvF1Code = Wait-ExitCode -Proc $srvF1 -TimeoutSec 120
    if ($srvF1Code -ne 0) { throw "FR-F: first server --once expected exit 0, got $srvF1Code" }
    # Sanity: local copy of large_exact.bin must be bit-exact to the (original) server content.
    $hashF1 = (Get-FileHash -Path (Join-Path $tgtF "large_exact.bin") -Algorithm SHA256).Hash
    $hashSrcOrig = (Get-FileHash -Path (Join-Path $src "large_exact.bin") -Algorithm SHA256).Hash
    if ($hashF1 -ne $hashSrcOrig) { throw "FR-F: first sync did not materialize local copy bit-exact" }

    # (b) Mutate the SERVER file: change BOTH size and content (unstructured/random) so it is a
    #     size-different changed file (goes through the TransferNow cascade, not the hash-defer
    #     path) and is >65% different at the block level (delta benefit-reject). The fixture uses a
    #     periodic pattern; a periodic->periodic mutation would coincidentally match under delta's
    #     rolling hash, so we must use random bytes to force "too different".
    $randF = [System.Security.Cryptography.RandomNumberGenerator]::Create()
    $bufF = New-Object byte[] (1 * 1024 * 1024)
    $streamF = [System.IO.File]::Create((Join-Path $src "large_exact.bin"))
    try {
        $remainingF = 20 * 1024 * 1024
        while ($remainingF -gt 0) {
            $takeF = [Math]::Min($bufF.Length, $remainingF)
            $randF.GetBytes($bufF)
            $streamF.Write($bufF, 0, $takeF)
            $remainingF -= $takeF
        }
    } finally { $streamF.Close() }
    $srcHashesF = Get-FileHashTree -Root $src   # NEW expected tree (post-mutation)

    # (c) Second sync with delta + block, same target (local old present, differs).
    if (-not (Wait-PortFree -PortNum $Port -TimeoutSec 15)) {
        Write-Host "[PORT-BUSY] port $Port still held before FR-F2 server start:"
        (netstat -ano | Select-String -Pattern ":$Port ") | ForEach-Object { Write-Host "    $($_.Line)" }
    }
    $srvF2 = Start-FastCloneProcess -Exe $exe -CliArgs @(
        "server", "--dir", $src, "--password", $password, "--port", "$Port", $bindLoopbackArg, "--once"
    ) -OutLog "$logDir\F-server2.out" -ErrLog "$logDir\F-server2.err"
    if (-not (Wait-PortListening -PortNum $Port -Proc $srvF2 -TimeoutSec 15)) {
        Write-FcDiag -StageTag "FR-F2/server-not-listening" -Reason "server-bind" -TimeoutSec 15 `
            -Proc $srvF2 -OutLog "$logDir\F-server2.out" -ErrLog "$logDir\F-server2.err"
        throw "FR-F2: server pid=$($srvF2.Id) never reached LISTENING on port $Port within 15s"
    }
    $clientArgsF2 = @("client", "--server", $serverAddr, "--target", $tgtF,
                      "--password", $password) + $twoLinks + @(
                        "--delta-min-size", "4M",
                        "--large-file-threshold", "8M", "--large-file-block", "8M")
    $codeF2 = Invoke-FastCloneSync -Exe $exe -CliArgs $clientArgsF2 -OutLog "$logDir\F-client2.out" -ErrLog "$logDir\F-client2.err" -TimeoutSec 180 -StageTag "FR-F2"
    if ($codeF2 -ne 0) { throw "FR-F: second sync expected exit 0, got $codeF2" }
    $srvF2Code = Wait-ExitCode -Proc $srvF2 -TimeoutSec 120
    if ($srvF2Code -ne 0) { throw "FR-F: second server --once expected exit 0, got $srvF2Code" }

    # (d) Assertions: delta attempted + benefit-rejected (logged to stderr), then routed to
    #     block (kind=file_range, logged to stdout) instead of single-stream full transfer.
    if (-not (Select-String -Path "$logDir\F-client2.err" -Pattern '\[delta\] fallback ' -Quiet)) {
        throw "FR-F: expected a '[delta] fallback' line (delta admitted then benefit-rejected)"
    }
    if (-not (Select-String -Path "$logDir\F-client2.out" -Pattern 'kind=file_range' -Quiet)) {
        throw "FR-F: expected kind=file_range (delta fallback must route to block, not single-stream)"
    }
    if (-not (Select-String -Path "$logDir\F-client2.out" -Pattern '\[file_range\] done' -Quiet)) {
        throw "FR-F: missing '[file_range] done' (block finalize)"
    }
    $frConnsF = Get-FileRangeConnIds -LogPath "$logDir\F-client2.out"
    if ($frConnsF.Count -lt 1) {
        throw "FR-F: expected file_range blocks on >=1 lane, got connIds: $($frConnsF -join ',')"
    }
    # Data integrity: target must now match the NEW server tree (delta fallback re-fetched via block).
    $diffF = Compare-FileHashTrees -Src $srcHashesF -Tgt (Get-FileHashTree -Root $tgtF)
    if ($null -ne $diffF) { throw "FR-F: DATA INTEGRITY FAILURE: $diffF" }
    Write-Host "  OK FR-F: delta too-different fallback routed to block (lanes: $($frConnsF -join ',')), SHA256 match"
    Remove-Item -Recurse -Force $tgtF -ErrorAction SilentlyContinue

    # ================ FR-G: batch 占满 lane + 块流存活 + 无 H1 预读 ================
    # T-block-lane-quota-and-h1-hash. Verifies on a small loopback fixture:
    #   FR-1  reserved lane slots keep file_range allocable under batch pressure
    #         (alloc kind=file_range > 0 on >= 2 lanes), and
    #   FR-2  block admission no longer issues H1 HashRequest prefetches
    #         (client hash_req_sent == 0; server hash_req_recv == 0 when observable),
    #         while structural finalize validation still yields SHA256-bit-exact bytes.
    # Uses its OWN fixture directory (gate note 6): FR-D above rebuilds large_tail.bin at
    # 192 MiB, so reusing $src would couple FR-G to FR-D's mutation.
    Write-Host ""
    Write-Host "[FR-G] block ON, 2 lanes, batch-saturated + no-H1-prefetch"
    Assert-FcDeadline -Stage "FR-G"
    $srcG = Join-Path $root "src-g"
    New-Item -ItemType Directory -Force -Path $srcG | Out-Null
    New-TestFixture -Src $srcG
    # Plenty of small files to keep the batch queues pressuring the normal stream slots.
    for ($i = 0; $i -lt 300; $i++) {
        New-PatternFile -Path (Join-Path $srcG ("small_{0}.bin" -f $i)) -Size 8192 -Seed (1000 + $i)
    }
    $srcHashesG = Get-FileHashTree -Root $srcG
    $tgtG = Join-Path $env:TEMP "fc-fr-tgt-g-$(Get-Random)"
    New-Item -ItemType Directory -Force -Path $tgtG | Out-Null
    if (-not (Wait-PortFree -PortNum $Port -TimeoutSec 15)) {
        Write-Host "[PORT-BUSY] port $Port still held before FR-G server start:"
        (netstat -ano | Select-String -Pattern ":$Port ") | ForEach-Object { Write-Host "    $($_.Line)" }
    }
    $srvG = Start-FastCloneProcess -Exe $exe -CliArgs @(
        "server", "--dir", $srcG, "--password", $password, "--port", "$Port", $bindLoopbackArg, "--once"
    ) -OutLog "$logDir\G-server.out" -ErrLog "$logDir\G-server.err"
    if (-not (Wait-PortListening -PortNum $Port -Proc $srvG -TimeoutSec 15)) {
        Write-FcDiag -StageTag "FR-G/server-not-listening" -Reason "server-bind" -TimeoutSec 15 `
            -Proc $srvG -OutLog "$logDir\G-server.out" -ErrLog "$logDir\G-server.err"
        throw "FR-G: server pid=$($srvG.Id) never reached LISTENING on port $Port within 15s"
    }
    # --streams 16 pins the LAN default so the reserved band is deterministic (2/lane);
    # --diag guarantees the end-of-session [diag] line with hash_req_sent on stdout.
    $clientArgsG = @("client", "--server", $serverAddr, "--target", $tgtG,
                     "--password", $password) + $twoLinks + $blockArgs + @("--streams", "16", "--diag")
    $codeG = Invoke-FastCloneSync -Exe $exe -CliArgs $clientArgsG -OutLog "$logDir\G-client.out" -ErrLog "$logDir\G-client.err" -TimeoutSec 180 -StageTag "FR-G"
    if ($codeG -ne 0) { throw "FR-G: client expected exit 0, got $codeG" }
    $srvGCode = Wait-ExitCode -Proc $srvG -TimeoutSec 180
    if ($srvGCode -ne 0) { throw "FR-G: server --once expected exit 0, got $srvGCode" }

    # (a) Block streams survive batch pressure: allocs on >= 2 lanes (FR-1 / AC-01 logic level).
    $frConnsG = Get-FileRangeConnIds -LogPath "$logDir\G-client.out"
    if ($frConnsG.Count -lt 2) {
        throw "FR-G: expected file_range blocks on >=2 lanes, got connIds: $($frConnsG -join ',')"
    }
    $allocG = (Select-String -Path "$logDir\G-client.out" -Pattern 'kind=file_range').Count
    if ($allocG -lt 1) { throw "FR-G: expected >=1 file_range alloc" }

    # (b) No H1 prefetch (FR-2 / AC-02/AC-10 logic level). First sync of a fresh target:
    #     pre-fix the two large files each triggered one H1 HashRequest (hash_req_sent=2);
    #     post-fix no HashRequest may be sent at all. The client [diag] line is printed at
    #     session end (guaranteed), so this assertion is strict.
    $diagLine = Select-String -Path "$logDir\G-client.out" -Pattern '\[diag\] .*hash_req_sent=(\d+)' |
        Select-Object -Last 1
    if (-not $diagLine) { throw "FR-G: missing client '[diag]' line with hash_req_sent" }
    if ([int]$diagLine.Matches[0].Groups[1].Value -ne 0) {
        throw "FR-G: client sent H1 HashRequests for block files (hash_req_sent != 0)"
    }
    #     Server-side cross-check: the 1s-periodic debug line goes to STDERR; when the run was
    #     long enough to print one, its last sample must show hash_req_recv == 0.
    $hashReqLine = Select-String -Path "$logDir\G-server.err" -Pattern 'hash_req_recv=(\d+)' |
        Select-Object -Last 1
    if ($hashReqLine -and [int]$hashReqLine.Matches[0].Groups[1].Value -gt 0) {
        throw "FR-G: unexpected server hash_req_recv>0 (H1 prefetch not eliminated)"
    }

    # (c) End-to-end correctness: structural validation + atomic rename keep bytes bit-exact.
    $diffG = Compare-FileHashTrees -Src $srcHashesG -Tgt (Get-FileHashTree -Root $tgtG)
    if ($null -ne $diffG) { throw "FR-G: DATA INTEGRITY FAILURE: $diffG" }
    if (-not (Select-String -Path "$logDir\G-client.out" -Pattern '\[file_range\] done' -Quiet)) {
        throw "FR-G: missing '[file_range] done'"
    }
    if (Select-String -Path "$logDir\G-client.out" -Pattern 'file_range_finalize_failed' -Quiet) {
        throw "FR-G: unexpected finalize failure"
    }
    Write-Host "  OK FR-G: file_range alloc=$allocG lanes=$($frConnsG -join ','), hash_req_sent=0, SHA256 match"
    Remove-Item -Recurse -Force $tgtG -ErrorAction SilentlyContinue

    # ============ FR-H: default ABSENT (auto) + 2 lanes -> auto-activation ============
    # T-largefile-block-auto-default AC-01/AC-03: no --large-file-block on the command line at
    # all -> Auto; with 2 healthy lanes + this server build block mode activates by DEFAULT
    # with the 32 MiB reference block (plan shows block=33554432; the >=32 MiB file slices
    # into blocks=ceil(size/32MiB)=3), fans out on >=2 lanes, SHA256 bit-exact.
    # Uses its OWN fixture directory: FR-D/FR-F above mutate $src, and the plan assertion
    # needs a file >= the 32 MiB reference block (New-TestFixture files are smaller).
    Write-Host ""
    Write-Host "[FR-H] default auto (no block flag), 2 loopback lanes -> auto-activation"
    Assert-FcDeadline -Stage "FR-H"
    $srcH = Join-Path $root "src-h"
    New-Item -ItemType Directory -Force -Path $srcH | Out-Null
    Set-Content -Path (Join-Path $srcH "notes.txt") -Value "small batch file" -NoNewline
    New-PatternFile -Path (Join-Path $srcH "small_64k.bin") -Size 65536 -Seed 11
    # 80 MiB + 123 bytes -> 2 full 32 MiB reference blocks + a tail block (blocks=3).
    New-PatternFile -Path (Join-Path $srcH "large_auto.bin") -Size (80L * 1024 * 1024 + 123) -Seed 71
    $srcHashesH = Get-FileHashTree -Root $srcH
    $tgtH = Join-Path $env:TEMP "fc-fr-tgt-h-$(Get-Random)"
    New-Item -ItemType Directory -Force -Path $tgtH | Out-Null
    if (-not (Wait-PortFree -PortNum $Port -TimeoutSec 15)) {
        Write-Host "[PORT-BUSY] port $Port still held before FR-H server start:"
        (netstat -ano | Select-String -Pattern ":$Port ") | ForEach-Object { Write-Host "    $($_.Line)" }
    }
    $srvH = Start-FastCloneProcess -Exe $exe -CliArgs @(
        "server", "--dir", $srcH, "--password", $password, "--port", "$Port", $bindLoopbackArg, "--once"
    ) -OutLog "$logDir\H-server.out" -ErrLog "$logDir\H-server.err"
    if (-not (Wait-PortListening -PortNum $Port -Proc $srvH -TimeoutSec 15)) {
        Write-FcDiag -StageTag "FR-H/server-not-listening" -Reason "server-bind" -TimeoutSec 15 `
            -Proc $srvH -OutLog "$logDir\H-server.out" -ErrLog "$logDir\H-server.err"
        throw "FR-H: server pid=$($srvH.Id) never reached LISTENING on port $Port within 15s"
    }
    # No --large-file-block at all: the default auto gate must activate block mode.
    $clientArgsH = @("client", "--server", $serverAddr, "--target", $tgtH,
                     "--password", $password) + $twoLinks + @("--large-file-threshold", "8M")
    $codeH = Invoke-FastCloneSync -Exe $exe -CliArgs $clientArgsH -OutLog "$logDir\H-client.out" -ErrLog "$logDir\H-client.err" -TimeoutSec 180 -StageTag "FR-H"
    if ($codeH -ne 0) { throw "FR-H: client expected exit 0, got $codeH" }
    $srvHCode = Wait-ExitCode -Proc $srvH -TimeoutSec 120
    if ($srvHCode -ne 0) { throw "FR-H: server --once expected exit 0, got $srvHCode" }
    $diffH = Compare-FileHashTrees -Src $srcHashesH -Tgt (Get-FileHashTree -Root $tgtH)
    if ($null -ne $diffH) { throw "FR-H: DATA INTEGRITY FAILURE: $diffH" }
    if (-not (Select-String -Path "$logDir\H-client.out" -Pattern 'kind=file_range' -Quiet)) {
        throw "FR-H: expected kind=file_range (default auto must activate on 2 lanes)"
    }
    $frConnsH = Get-FileRangeConnIds -LogPath "$logDir\H-client.out"
    if ($frConnsH.Count -lt 2) {
        throw "FR-H: expected file_range blocks on >=2 lanes, got connIds: $($frConnsH -join ',')"
    }
    # AC-03: the default auto block is the 32 MiB reference; the 80 MiB+123 file plans 3 blocks.
    if (-not (Select-String -Path "$logDir\H-client.out" -Pattern '\[file_range\] plan .*block=33554432 blocks=3' -Quiet)) {
        throw "FR-H: expected '[file_range] plan ... block=33554432 blocks=3' (default 32 MiB reference block)"
    }
    if (-not (Select-String -Path "$logDir\H-client.out" -Pattern '\[file_range\] done' -Quiet)) {
        throw "FR-H: missing '[file_range] done' (block finalize log)"
    }
    if (Select-String -Path "$logDir\H-client.out" -Pattern 'file_range_finalize_failed' -Quiet) {
        throw "FR-H: unexpected finalize failure"
    }
    Write-Host "  OK FR-H: default auto activated (block=33554432, lanes: $($frConnsH -join ',')), SHA256 match"
    Remove-Item -Recurse -Force $tgtH -ErrorAction SilentlyContinue

    # ============ FR-I: default auto + explicit --large-file-lane -> treated as off ======
    # T-largefile-block-auto-default AC-19 (C-3 case 1): the PRESENCE of --large-file-lane is
    # the mutual-exclusion signal; auto folds to off (lane wins) -> NO file_range frames, and
    # the lane pinning still applies to legacy large files (kind=file ... large=1 primary=1).
    # (The lane value domain is primary|aux|auto; the requirements' `... lane 1` shorthand
    # maps to `primary`.)
    # Uses its OWN fixture directory (FR-D/FR-F mutate $src).
    Write-Host ""
    Write-Host "[FR-I] default auto + --large-file-lane primary -> treated as off, 2 loopback lanes"
    Assert-FcDeadline -Stage "FR-I"
    $srcI = Join-Path $root "src-i"
    New-Item -ItemType Directory -Force -Path $srcI | Out-Null
    New-TestFixture -Src $srcI
    $srcHashesI = Get-FileHashTree -Root $srcI
    $tgtI = Join-Path $env:TEMP "fc-fr-tgt-i-$(Get-Random)"
    New-Item -ItemType Directory -Force -Path $tgtI | Out-Null
    if (-not (Wait-PortFree -PortNum $Port -TimeoutSec 15)) {
        Write-Host "[PORT-BUSY] port $Port still held before FR-I server start:"
        (netstat -ano | Select-String -Pattern ":$Port ") | ForEach-Object { Write-Host "    $($_.Line)" }
    }
    $srvI = Start-FastCloneProcess -Exe $exe -CliArgs @(
        "server", "--dir", $srcI, "--password", $password, "--port", "$Port", $bindLoopbackArg, "--once"
    ) -OutLog "$logDir\I-server.out" -ErrLog "$logDir\I-server.err"
    if (-not (Wait-PortListening -PortNum $Port -Proc $srvI -TimeoutSec 15)) {
        Write-FcDiag -StageTag "FR-I/server-not-listening" -Reason "server-bind" -TimeoutSec 15 `
            -Proc $srvI -OutLog "$logDir\I-server.out" -ErrLog "$logDir\I-server.err"
        throw "FR-I: server pid=$($srvI.Id) never reached LISTENING on port $Port within 15s"
    }
    $clientArgsI = @("client", "--server", $serverAddr, "--target", $tgtI,
                     "--password", $password) + $twoLinks + @("--large-file-threshold", "8M",
                     "--large-file-lane", "primary")
    $codeI = Invoke-FastCloneSync -Exe $exe -CliArgs $clientArgsI -OutLog "$logDir\I-client.out" -ErrLog "$logDir\I-client.err" -TimeoutSec 180 -StageTag "FR-I"
    if ($codeI -ne 0) { throw "FR-I: client expected exit 0, got $codeI" }
    $srvICode = Wait-ExitCode -Proc $srvI -TimeoutSec 120
    if ($srvICode -ne 0) { throw "FR-I: server --once expected exit 0, got $srvICode" }
    $diffI = Compare-FileHashTrees -Src $srcHashesI -Tgt (Get-FileHashTree -Root $tgtI)
    if ($null -ne $diffI) { throw "FR-I: DATA INTEGRITY FAILURE: $diffI" }
    if (Select-String -Path "$logDir\I-client.out" -Pattern 'kind=file_range' -Quiet) {
        throw "FR-I: unexpected kind=file_range (auto+lane must fold to off)"
    }
    if (Select-String -Path "$logDir\I-client.out" -Pattern '\[file_range\]' -Quiet) {
        throw "FR-I: unexpected [file_range] lines (auto+lane must fold to off)"
    }
    # Lane routing still observable: large files take the legacy single-stream path pinned to
    # the primary lane (alloc kind=file ... large=1 primary=1).
    if (-not (Select-String -Path "$logDir\I-client.out" -Pattern 'kind=file connId=\d+ .*large=1 primary=1' -Quiet)) {
        throw "FR-I: expected a lane-pinned legacy large-file alloc (kind=file large=1 primary=1)"
    }
    Write-Host "  OK FR-I: auto+lane folded to off (no file_range), lane pinning applied, SHA256 match"
    Remove-Item -Recurse -Force $tgtI -ErrorAction SilentlyContinue

    # ============ FR-J: forced ON (size) + --large-file-lane -> parse-time rejection =====
    # T-largefile-block-auto-default AC-20 (C-3 case 2): an explicit contradiction must be
    # rejected at parse time -- non-zero exit, the error message names BOTH flags, and no
    # side is silently adopted. No server is needed: parsing fails before any connection.
    Write-Host ""
    Write-Host "[FR-J] --large-file-block 8M + --large-file-lane primary -> non-zero exit"
    Assert-FcDeadline -Stage "FR-J"
    $tgtJ = Join-Path $env:TEMP "fc-fr-tgt-j-$(Get-Random)"
    New-Item -ItemType Directory -Force -Path $tgtJ | Out-Null
    $clientArgsJ = @("client", "--server", $serverAddr, "--target", $tgtJ,
                     "--password", $password) + $twoLinks + @("--large-file-threshold", "8M",
                     "--large-file-block", "8M", "--large-file-lane", "primary")
    $codeJ = Invoke-FastCloneSync -Exe $exe -CliArgs $clientArgsJ -OutLog "$logDir\J-client.out" -ErrLog "$logDir\J-client.err" -TimeoutSec 180 -StageTag "FR-J"
    if ($codeJ -eq 0) { throw "FR-J: client expected a NON-zero exit (on+lane must be rejected), got 0" }
    if (-not (Select-String -Path "$logDir\J-client.err" -Pattern '--large-file-block' -Quiet)) {
        throw "FR-J: error message must name --large-file-block"
    }
    if (-not (Select-String -Path "$logDir\J-client.err" -Pattern '--large-file-lane' -Quiet)) {
        throw "FR-J: error message must name --large-file-lane"
    }
    Write-Host "  OK FR-J: on+lane rejected at parse time (exit=$codeJ), error names both flags"
    Remove-Item -Recurse -Force $tgtJ -ErrorAction SilentlyContinue

    Write-Host ""
    Write-Host "All file-range integration tests passed. Logs: $logDir"
    Stop-AllFastClone
    exit 0
}
catch {
    Write-Host "FAILED: $_" -ForegroundColor Red
    Write-Host "Logs: $logDir"
    Stop-AllFastClone
    Get-Process powershell -ErrorAction SilentlyContinue |
        Where-Object { $_.CommandLine -match 'file_range_tcp_proxy' } |
        ForEach-Object { Stop-Process -Id $_.Id -Force -ErrorAction SilentlyContinue }
    exit 1
}
