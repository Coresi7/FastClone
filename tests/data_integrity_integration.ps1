# FastClone data integrity integration test (Windows).
#
# Verifies that bytes transferred by the server are byte-for-byte identical to
# the source (SHA256) after a full sync. This is the automated regression guard
# for the C2 "transferred bytes are byte-for-byte unchanged" guarantee that A1 (a1-sendloop-buffer-reuse)
# relies on but previously had only a one-shot manual SHA256 check.
#
# Covers the gaps identified in the post-A1 static review:
#   DI-1  post-transfer SHA256 byte-for-byte comparison (the core C2 guarantee)
#   DI-2  empty file (0 bytes) — skips read path entirely
#   DI-3  EOF boundary (last chunk < effectiveChunkSize) — single-file stream path
#   DI-4  batch multi-file switch — readBuffer reuse across files in one batch
#   DI-5  single-file stream vs batch stream paths both exercised
#         (threshold: fileSize <= 1920 KiB -> batch, > 1920 KiB -> single)
#   DI-6  small file (< effectiveChunkSize) — single chunk + EOF
#   DI-7  multiple large files concurrent — multiple activeStreams round-robin,
#         each with its own readBuffer; verifies no cross-stream buffer leakage
#   DI-8  batch mixed file sizes — readBuffer grows then does not shrink; the
#         smaller trailing file's copy must exclude residual bytes
#
# Two variants are run to cover different effectiveChunkSize values:
#   variant A: default chunk size (auto-tuned to 4 MiB at streamLimit<=8)
#   variant B: --chunk-kb 256 (256 KiB) — forces many chunks on the large file,
#              exercising the EOF tail boundary repeatedly
#
# Usage: .\tests\data_integrity_integration.ps1 [-ExePath path\to\FastClone.exe] [-Port 27894]

param(
    [string]$ExePath = "",
    [int]$Port = 27894,
    # Test-only: bind server to ::1 (loopback) to isolate from LAN/WAN traffic (OM-4 flaky root cause).
    [switch]$BindLoopback
)

$ErrorActionPreference = "Stop"

# Optional server arg fragment: empty in production, "--bind-loopback" in test mode.
# Read inside Invoke-DataIntegrityVariant via PowerShell dynamic scoping (parent scope).
$bindLoopbackArg = if ($BindLoopback) { "--bind-loopback" } else { "" }

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
    param([System.Diagnostics.Process]$Proc, [int]$TimeoutSec = 60, [string]$StageTag = "wait")
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
        [int]$TimeoutSec = 150,          # tier T0/T1, see design section 3.2; callers override
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

# Force the LONG (non-8.3) form of a path. Resolve-Path is NOT reliable for this
# on the CI machine: it returns the 8.3 short form (C:\Users\ADMINI~1\...) for a
# directory that was created via a short $env:TEMP, while Get-ChildItem returns the
# LONG form (C:\Users\Administrator\...) for the files inside it. That asymmetry
# makes a naive Substring-based rel-path computation produce wrong keys
# (e.g. "6\src\large_5m.bin", where "6" is the trailing digit of the short random
# dir name) and the comparison reports every file missing. GetLongPathName always
# expands 8.3 components, so both the root and each file become long and consistent.
$longPathSig = @'
[DllImport("kernel32.dll", CharSet=CharSet.Auto, SetLastError=true)]
public static extern int GetLongPathName(string lpszShortPath, System.Text.StringBuilder lpszLongPath, int cchBuffer);
'@
# Add-Type compiles C# in-process and inherits the caller's environment. When this script runs
# as an MSBuild post-build step, LIB/LIBPATH/INCLUDE point into the VS toolset, and VS 2026
# lists a NON-EXISTENT entry "...\MSVC\14.51.36231\atlmfc\lib\x64"; the C# compiler reports it
# as "Warning as Error: Invalid search path" and Add-Type throws, which silently killed this
# integration test (MSB3073). Drop those variables for the duration of the compile only, so the
# script works identically under MSBuild, ctest and a plain shell.
$fcSavedBuildEnv = @{}
foreach ($fcEnvVar in 'LIB', 'LIBPATH', 'INCLUDE') {
    $fcSavedBuildEnv[$fcEnvVar] = [Environment]::GetEnvironmentVariable($fcEnvVar)
    Remove-Item "Env:\$fcEnvVar" -ErrorAction SilentlyContinue
}
try {
    $longPathUtil = Add-Type -MemberDefinition $longPathSig -Name 'LongPathUtil' -Namespace 'FcDiag' -PassThru
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

# Build a SHA256 hashtable {relativePath -> hash} for all files under $Root.
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

# Compare two hash trees; returns $null on success or an error string.
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

# Build the fixture directory. Returns the source path.
# Files are sized to exercise both batch stream (<= 1920 KiB) and single-file
# stream (> 1920 KiB) paths, plus edge cases (empty, tiny, multi-file batch).
function New-TestFixture {
    param([string]$Src)

    # DI-2: empty file (0 bytes) — skips read path entirely
    Set-Content -Path (Join-Path $Src "empty.txt") -Value "" -NoNewline

    # DI-6: tiny file (50 bytes) — single chunk + EOF, batch path
    $rng = [System.IO.File]::Create((Join-Path $Src "tiny.bin"))
    try { $rng.Write((New-Object byte[] 50), 0, 50) } finally { $rng.Close() }

    # small file (100 KiB) — batch path, single chunk under default 4 MiB
    $s100 = [System.IO.File]::Create((Join-Path $Src "small_100k.bin"))
    try { $s100.Write((New-Object byte[] 102400), 0, 102400) } finally { $s100.Close() }

    # DI-4 / DI-8: batch multi-file switch — 4 files of DIFFERENT sizes in a
    # subdirectory. Sizes 1 KiB / 64 KiB / 200 KiB / 4 KiB (deliberately out of
    # size order). Exercises readBuffer growth (1K->64K->200K) then non-shrinking
    # reuse: the 4 KiB file is served from the 200 KiB-capacity buffer; copy
    # [data, data+4K) must not include residual bytes from the preceding 200K
    # file. Each file gets a distinct byte pattern so any cross-file leakage
    # changes the SHA256.
    New-Item -ItemType Directory -Force -Path (Join-Path $Src "batchdir") | Out-Null
    $batchSizes = @(1024, 65536, 204800, 4096)
    $batchIdx = 0
    foreach ($sz in $batchSizes) {
        $batchIdx++
        $f = [System.IO.File]::Create((Join-Path $Src "batchdir\file$batchIdx.bin"))
        try {
            $buf = New-Object byte[] $sz
            for ($i = 0; $i -lt $sz; $i++) { $buf[$i] = (($batchIdx * 37 + $i) % 251) }
            $f.Write($buf, 0, $sz)
        } finally { $f.Close() }
    }

    # nested directory with a small file — verifies path traversal + batch path
    New-Item -ItemType Directory -Force -Path (Join-Path $Src "nested\deep") | Out-Null
    Set-Content -Path (Join-Path $Src "nested\deep\leaf.txt") -Value "leaf-payload"

    # DI-3 / DI-5: large file (5 MiB) — > 1920 KiB threshold -> single-file stream
    # path. Under default 4 MiB chunk: 2 chunks (4 MiB + 1 MiB tail, EOF boundary).
    # Under --chunk-kb 256: 20 chunks, repeated EOF-tail exercising.
    $big = [System.IO.File]::Create((Join-Path $Src "large_5m.bin"))
    try {
        $buf = New-Object byte[] (5 * 1024 * 1024)
        for ($i = 0; $i -lt $buf.Length; $i++) { $buf[$i] = ($i % 251) }  # non-trivial pattern
        $big.Write($buf, 0, $buf.Length)
    } finally { $big.Close() }

    # DI-7: multiple large files concurrent — multiple activeStreams round-robin.
    # Each large file opens its own activeStream with its own readBuffer; concurrent
    # round-robin must not cross-contaminate buffers. Two extra files (plus
    # large_5m.bin above) give 3 concurrent single-file streams. Each file gets a
    # distinct byte pattern so any cross-stream leakage changes the SHA256.
    foreach ($tag in @("b", "c")) {
        $big = [System.IO.File]::Create((Join-Path $Src "large_5m_${tag}.bin"))
        try {
            $sz = 5 * 1024 * 1024
            $buf = New-Object byte[] $sz
            $seed = [byte][char]$tag  # 'b'=98, 'c'=99 — distinct seed per file
            for ($i = 0; $i -lt $sz; $i++) { $buf[$i] = (($seed + $i) % 251) }
            $big.Write($buf, 0, $sz)
        } finally { $big.Close() }
    }
}

# Run one sync variant, then compare source/target SHA256 trees.
function Invoke-DataIntegrityVariant {
    param(
        [string]$Exe,
        [string]$Src,
        [string]$Password,
        [string]$ServerAddr,
        [int]$PortNum,
        [string]$Label,
        [string[]]$ExtraClientArgs,
        [string]$LogDir
    )

    Write-Host "[$Label] syncing with extra args: $($ExtraClientArgs -join ' ')"
    # [I-2] Per-subcase deadline check (design section 3.12): fail fast BEFORE spending the
    # next stage's budget when the global 480s is already consumed.
    Assert-FcDeadline -Stage $Label

    # Snapshot source hashes before transfer.
    $srcHashes = Get-FileHashTree -Root $Src

    # Fresh target directory.
    $tgt = Join-Path $env:TEMP "fc-di-tgt-$(Get-Random)"
    New-Item -ItemType Directory -Force -Path $tgt | Out-Null

    # Start server (--once so it auto-exits after the single session).
    # $bindLoopbackArg comes from the parent script scope (empty in production, "--bind-loopback"
    # when ctest passes -BindLoopback on); Start-FastCloneProcess drops it when empty.
    # [FR-02] Bounded port probes replace the fixed 1s sleep: Warn when the port is still
    # held (TIME_WAIT does not necessarily block bind), FAIL when this server never reaches
    # LISTENING (RC-5d early failure with the server .err right there).
    if (-not (Wait-PortFree -PortNum $PortNum -TimeoutSec 15)) {
        Write-Host "[PORT-BUSY] port $PortNum still held before $Label server start:"
        (netstat -ano | Select-String -Pattern ":$PortNum ") | ForEach-Object { Write-Host "    $($_.Line)" }
    }
    $srv = Start-FastCloneProcess -Exe $Exe -CliArgs @(
        "server", "--dir", $Src, "--password", $Password, "--port", "$PortNum", $bindLoopbackArg, "--once"
    ) -OutLog "$LogDir\$Label-server.out" -ErrLog "$LogDir\$Label-server.err"
    if (-not (Wait-PortListening -PortNum $PortNum -Proc $srv -TimeoutSec 15)) {
        Write-FcDiag -StageTag "$Label/server-not-listening" -Reason "server-bind" -TimeoutSec 15 `
            -Proc $srv -OutLog "$LogDir\$Label-server.out" -ErrLog "$LogDir\$Label-server.err"
        throw "${Label}: server pid=$($srv.Id) never reached LISTENING on port $PortNum within 15s"
    }

    # Run client sync. T0 tier = 150s (design section 3.2).
    $clientArgs = @(
        "client", "--server", $ServerAddr, "--target", $tgt, "--password", $Password
    ) + $ExtraClientArgs
    $code = Invoke-FastCloneSync -Exe $Exe -CliArgs $clientArgs `
        -OutLog "$LogDir\$Label-client.out" -ErrLog "$LogDir\$Label-client.err" `
        -TimeoutSec 150 -StageTag $Label
    if ($code -ne 0) { throw "${Label}: client sync expected exit 0, got $code" }

    $srvCode = Wait-ExitCode -Proc $srv -TimeoutSec 60 -StageTag "$Label/server"
    if ($srvCode -ne 0) { throw "${Label}: server --once expected exit 0, got $srvCode" }

    # DI-1: compare source/target SHA256 trees byte-for-byte.
    $tgtHashes = Get-FileHashTree -Root $tgt
    $diff = Compare-FileHashTrees -Src $srcHashes -Tgt $tgtHashes
    if ($null -ne $diff) {
        throw "${Label}: DATA INTEGRITY FAILURE: $diff"
    }

    Write-Host "  OK ${Label}: $($srcHashes.Count) files, all SHA256 match"
    Remove-Item -Recurse -Force $tgt -ErrorAction SilentlyContinue
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

$exe = Resolve-FastCloneExe -Hint $ExePath
$root = Join-Path $env:TEMP "fastclone-data-integrity-$(Get-Random)"
$logDir = Join-Path $root "logs"
$src = Join-Path $root "src"
New-Item -ItemType Directory -Force -Path $logDir, $src | Out-Null

$password = "fc-di-pw"
$serverAddr = "127.0.0.1:$Port"

try {
    Stop-AllFastClone

    Write-Host "Building test fixture..."
    New-TestFixture -Src $src

    Write-Host "Source files:"
    $normSrc = Get-LongPath -Path $src
    Get-ChildItem -Path $src -Recurse -File | ForEach-Object {
        $full = Get-LongPath -Path $_.FullName
        $rel = $full.Substring($normSrc.Length).TrimStart('\', '/')
        Write-Host ("  {0,-30} {1,10:N0} bytes" -f $rel, $_.Length)
    }

    # Variant A: default chunk size (auto-tuned to 4 MiB).
    # large_5m.bin -> 2 chunks (4 MiB + 1 MiB tail), single-file stream path.
    Invoke-DataIntegrityVariant -Exe $exe -Src $src -Password $password `
        -ServerAddr $serverAddr -PortNum $Port -LogDir $logDir `
        -Label "DI-A-default" -ExtraClientArgs @()

    # Variant B: --chunk-kb 256 (256 KiB).
    # large_5m.bin -> 20 chunks, repeated EOF-tail boundary exercising.
    # small_100k.bin -> 1 chunk (100K < 256K), batch path.
    Invoke-DataIntegrityVariant -Exe $exe -Src $src -Password $password `
        -ServerAddr $serverAddr -PortNum $Port -LogDir $logDir `
        -Label "DI-B-chunk256k" -ExtraClientArgs @("--chunk-kb", "256")

    Write-Host ""
    Write-Host "All data-integrity integration tests passed. Logs: $logDir"
    Stop-AllFastClone
    exit 0
}
catch {
    Write-Host "FAILED: $_" -ForegroundColor Red
    Write-Host "Logs: $logDir"
    # On failure, surface the server/client [DIAG-BATCH]/[DIAG-STREAM] probe lines
    # from the .err logs into stdout so ctest --output-on-failure captures them.
    if (Test-Path $logDir) {
        Get-ChildItem -Path $logDir -File | ForEach-Object {
            $lines = Get-Content -Path $_.FullName -ErrorAction SilentlyContinue | Select-String -Pattern 'DIAG-BATCH|DIAG-STREAM'
            if ($lines) {
                Write-Host "----- $($_.Name) (DIAG) -----"
                $lines | ForEach-Object { Write-Host $_.Line }
            }
        }
    }
    Stop-AllFastClone
    exit 1
}
