# FastClone/FastCheck consistency invariant - end-to-end test (Windows).
# Task unify-probe-extra-shared (design §8.3, FR-13): after a successful FastClone sync
# (exit 0), FastCheck must report diff=0 missing=0 extra_local=0 total=<source file
# count> in BOTH --mode fast (AC-06) and --mode strict (AC-07). Scenarios S1..S6.
# Preconditions missing (exes not built / port unavailable / temp unwritable) => FAIL
# with a non-zero exit (task vs-build-test-parity, R-2/P1): a missing dependency must
# never produce a "passed" signal. CTest always passes explicit -ExePath/-CheckExePath,
# so this branch is unreachable from CI. Strict failure triggers one automatic retry
# with [RETRY] tag.

param(
    [string]$ExePath = "",
    [string]$CheckExePath = "",
    [int]$Port = 27941,
    # Test-only: bind server to ::1 (loopback) to isolate from LAN/WAN traffic (OM-4 flaky root cause).
    [switch]$BindLoopback
)

$ErrorActionPreference = "Stop"

# Optional server arg fragment: empty in production, "--bind-loopback" in test mode.
$bindLoopbackArg = if ($BindLoopback) { "--bind-loopback" } else { "" }

# ---------------------------------------------------------------------------
# Task ci-integration-test-timeout: global bounded-execution skeleton (design sections
# 3.4/3.5/3.12/3.13). Canonical implementation, with this script's specifics: a 450s
# deadline (its CMake TIMEOUT is 900, design section 3.6) and $logDir pointing at the
# working directory (this script writes its logs to the CWD via relative tag paths).
# ---------------------------------------------------------------------------

# [I-2] Global wall-clock budget: 450s here (CMake TIMEOUT 900; abort with 30s reserve,
# leaving ample room for the diagnostic dump + cleanup well before the hard kill).
$script:FcDeadlineUtc = (Get-Date).ToUniversalTime().AddSeconds(450)
$logDir = (Get-Location).Path

function Get-FcRemainingSec {
    param([int]$ReserveSec = 30)
    $r = ($script:FcDeadlineUtc - (Get-Date).ToUniversalTime()).TotalSeconds - $ReserveSec
    if ($r -lt 0) { $r = 0 }
    return [int][Math]::Floor($r)
}

function Assert-FcDeadline {
    param([string]$Stage)
    if ((Get-FcRemainingSec) -le 0) {
        throw "GLOBAL-DEADLINE exceeded at stage='$Stage' (450s budget, 30s reserve)"
    }
}

# [FR-03] PID registry: only processes THIS script started.
$script:FcOwnedPids = @{}

function Register-OwnedProcess {
    param([System.Diagnostics.Process]$Proc, [string]$Tag)
    $script:FcOwnedPids[[int]$Proc.Id] = $Tag
    return $Proc
}

# [FR-02] Bounded port helpers (design section 3.4).
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

# [NFR-03] Timeout diagnostic block (design section 3.13).
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
    if ($logDir -and (Test-Path $logDir)) {
        Get-ChildItem -Path $logDir -File -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -match 'srv' } | ForEach-Object {
                $t = Get-Content $_.FullName -Tail 20 -ErrorAction SilentlyContinue
                if ($t) {
                    Write-Host "  ----- tail $($_.Name) -----"
                    $t | ForEach-Object { Write-Host "    $_" }
                }
            }
    }
    Write-Host "===== TIMEOUT-DIAG end ====="
}

function Resolve-Tool {
    param([string]$Hint, [string]$Name)
    if ($Hint -and (Test-Path $Hint)) { return (Resolve-Path $Hint).Path }
    $candidates = @(
        "$PSScriptRoot\..\x64\Release\$Name",
        "$PSScriptRoot\..\build\Release\$Name",
        "$PSScriptRoot\..\build\$Name"
    )
    foreach ($c in $candidates) {
        if (Test-Path $c) { return (Resolve-Path $c).Path }
    }
    return $null
}

function Wait-ExitCode {
    param([System.Diagnostics.Process]$Proc, [int]$TimeoutSec = 120, [string]$StageTag = "wait")
    # [I-2] v2 (R-06): clamp the wait to the global deadline so I-2 closes on this script too.
    $eff = [Math]::Min($TimeoutSec, (Get-FcRemainingSec))
    if ($eff -le 0) {
        # NOTE: WaitForExit(0) returns immediately regardless of state, so we must NOT fall
        # through with 0 -- that would silently kill a healthy process and look like a stall.
        throw "GLOBAL-DEADLINE exceeded waiting for pid=$($Proc.Id) stage=$StageTag (budget=${TimeoutSec}s)"
    }
    if ($Proc.HasExited) { $Proc.Refresh() }
    elseif (-not $Proc.WaitForExit($eff * 1000)) {
        Write-FcDiag -StageTag $StageTag -Reason "wait-exitcode-timeout" -TimeoutSec $eff `
            -Proc $Proc -OutLog $null -ErrLog $null
        Stop-Process -Id $Proc.Id -Force -ErrorAction SilentlyContinue
        throw "TIMEOUT ${StageTag}: pid=$($Proc.Id) did not exit within ${eff}s (budget ${TimeoutSec}s)"
    }
    $Proc.Refresh()
    return [int]$Proc.ExitCode
}

# [RC-6] Bounded tool invocation (design section 3.3): v1 declared $TimeoutSec=300 but NEVER
# used it -- the `cmd /c` below was an unbounded wait. This is the FastCheck path (ctest #4,
# HandshakeClientCheck). Same shape as Invoke-FastCloneSync: Start-Process + WaitForExit(ms)
# + kill on timeout + diagnostic block. Default 300s = tier T2 (design section 3.2).
function Invoke-Tool {
    param([string]$Exe, [string[]]$CliArgs, [string]$OutLog, [string]$ErrLog,
          [int]$TimeoutSec = 300, [string]$StageTag = "tool")
    foreach ($a in $CliArgs) {
        if ($null -eq $a -or $a -eq "") { throw "Invoke-Tool: null/empty argument" }
    }
    Assert-FcDeadline -Stage "$StageTag/pre"
    $eff = [Math]::Min($TimeoutSec, (Get-FcRemainingSec))
    if ($eff -le 0) { throw "GLOBAL-DEADLINE exceeded before stage='$StageTag'" }
    $proc = Start-Process -FilePath $Exe -ArgumentList ([string[]]$CliArgs) `
        -RedirectStandardOutput $OutLog -RedirectStandardError $ErrLog `
        -PassThru -NoNewWindow
    $null = $proc.Handle
    $null = Register-OwnedProcess -Proc $proc -Tag $StageTag
    if (-not $proc.WaitForExit($eff * 1000)) {
        Write-FcDiag -StageTag $StageTag -Reason "tool-timeout" -TimeoutSec $eff `
            -Proc $proc -OutLog $OutLog -ErrLog $ErrLog
        Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
        throw "TIMEOUT ${StageTag}: pid=$($proc.Id) exe=$Exe did not exit within ${eff}s"
    }
    return [int]$proc.ExitCode
}

function New-TempDir {
    param([string]$Tag)
    $d = Join-Path ([System.IO.Path]::GetTempPath()) ("fc_cc_" + $Tag + "_" + [System.IO.Path]::GetRandomFileName().Replace('.', ''))
    New-Item -ItemType Directory -Path $d | Out-Null
    return $d
}

function Write-Binary {
    param([string]$Path, [int]$Size, [datetime]$MtimeUtc)
    $dir = Split-Path $Path -Parent
    if ($dir -and -not (Test-Path $dir)) { New-Item -ItemType Directory -Path $dir | Out-Null }
    $bytes = New-Object byte[] $Size
    for ($i = 0; $i -lt $Size; $i++) { $bytes[$i] = ($i * 7 + 13) -band 0xFF }
    [System.IO.File]::WriteAllBytes($Path, $bytes)
    [System.IO.File]::SetLastWriteTimeUtc($Path, $MtimeUtc)
}

function Start-Server {
    param([string]$Exe, [string]$SrcDir, [string]$Pw, [string]$LogTag)
    $out = "$LogTag.out"; $err = "$LogTag.err"
    # $bindLoopbackArg from parent scope (empty in production, "--bind-loopback" in test mode);
    # filter empties so Start-Process never sees a stray blank argument.
    $srvArgs = @("server", "--dir", $SrcDir, "--password", $Pw, "--port", "$Port", $bindLoopbackArg, "--once") |
        Where-Object { $null -ne $_ -and $_ -ne "" }
    $proc = Start-Process -FilePath $Exe -ArgumentList ([string[]]$srvArgs) `
        -RedirectStandardOutput $out -RedirectStandardError $err -PassThru -NoNewWindow
    $null = $proc.Handle
    $null = Register-OwnedProcess -Proc $proc -Tag $LogTag
    # [FR-02 / RC-5d] Bounded readiness probe (design section 3.4) replacing the fixed
    # 1500ms sleep: fail fast (15s) with the server .err in the diagnostic block instead of
    # letting the client hang in an unbounded connect/recv.
    if (-not (Wait-PortListening -PortNum $Port -Proc $proc -TimeoutSec 15)) {
        Write-FcDiag -StageTag "$LogTag/server-not-listening" -Reason "server-bind" -TimeoutSec 15 `
            -Proc $proc -OutLog $out -ErrLog $err
        throw "$LogTag : server pid=$($proc.Id) never reached LISTENING on port $Port within 15s"
    }
    return $proc
}

function Assert-CheckClean {
    param([string]$CheckExe, [string]$Target, [string]$Pw, [string]$Mode, [int]$ExpectedTotal, [string]$Tag)
    $out = "$Tag.out"; $err = "$Tag.err"
    $code = Invoke-Tool -Exe $CheckExe -CliArgs @("--server", "127.0.0.1:$Port", "--target", $Target, "--password", $Pw, "--mode", $Mode) -OutLog $out -ErrLog $err
    $stdout = ""
    if (Test-Path $out) { $stdout = Get-Content $out -Raw }
    if ($code -ne 0) {
        throw "[$Tag] FastCheck ($Mode) exited $code, expected 0. stdout: $stdout; stderr: $(Get-Content $err -Raw)"
    }
    # "Check completed. same=N diff=N missing=N extra_local=N total=N mode=... duration_ms=..."
    if ($stdout -notmatch 'same=(\d+) diff=(\d+) missing=(\d+) extra_local=(\d+) total=(\d+)') {
        throw "[$Tag] FastCheck ($Mode) summary line not parseable: $stdout"
    }
    $same = [int]$Matches[1]; $diff = [int]$Matches[2]; $missing = [int]$Matches[3]
    $extra = [int]$Matches[4]; $total = [int]$Matches[5]
    if ($diff -ne 0 -or $missing -ne 0 -or $extra -ne 0 -or $total -ne $ExpectedTotal) {
        throw "[$Tag] FastCheck ($Mode) not clean: diff=$diff missing=$missing extra_local=$extra total=$total (expected total=$ExpectedTotal)"
    }
    return $stdout
}

function Assert-CheckExtra {
    param([string]$CheckExe, [string]$Target, [string]$Pw, [string]$Mode, [string]$ExpectedExtraRel, [string]$Tag)
    $out = "$Tag.out"; $err = "$Tag.err"
    $code = Invoke-Tool -Exe $CheckExe -CliArgs @("--server", "127.0.0.1:$Port", "--target", $Target, "--password", $Pw, "--mode", $Mode) -OutLog $out -ErrLog $err
    $stdout = ""
    if (Test-Path $out) { $stdout = Get-Content $out -Raw }
    if ($code -ne 1) {
        throw "[$Tag] FastCheck ($Mode) exited $code, expected 1. stdout: $stdout"
    }
    if ($stdout -notmatch 'extra_local=(\d+)') { throw "[$Tag] summary not parseable: $stdout" }
    if ([int]$Matches[1] -ne 1) { throw "[$Tag] extra_local=$($Matches[1]), expected 1. stdout: $stdout" }
    if ($stdout -notmatch [regex]::Escape($ExpectedExtraRel)) {
        throw "[$Tag] extra detail line must mention '$ExpectedExtraRel'. stdout: $stdout"
    }
    return $stdout
}

function Sync-And-Verify {
    param([string]$Exe, [string]$CheckExe, [string]$SrcDir, [string]$Target, [string]$Pw,
           [int]$ExpectedTotal, [string]$Tag, [switch]$SkipStrict)
    # 1) client sync (fresh server, --once)
    $srv = Start-Server -Exe $Exe -SrcDir $SrcDir -Pw $Pw -LogTag "$Tag-srv1"
    $syncCode = Invoke-Tool -Exe $Exe -CliArgs @("--server", "127.0.0.1:$Port", "--target", $Target, "--password", $Pw) -OutLog "$Tag-sync.out" -ErrLog "$Tag-sync.err"
    $srvCode = Wait-ExitCode -Proc $srv
    if ($syncCode -ne 0 -or $srvCode -ne 0) {
        throw "[$Tag] sync failed: client=$syncCode server=$srvCode"
    }
    # 2) fast mode
    $srv = Start-Server -Exe $Exe -SrcDir $SrcDir -Pw $Pw -LogTag "$Tag-srv2"
    $null = Assert-CheckClean -CheckExe $CheckExe -Target $Target -Pw $Pw -Mode "fast" -ExpectedTotal $ExpectedTotal -Tag "$Tag-fast"
    $null = Wait-ExitCode -Proc $srv
    # 3) strict mode (AC-07, one automatic retry on failure)
    if (-not $SkipStrict) {
        $srv = Start-Server -Exe $Exe -SrcDir $SrcDir -Pw $Pw -LogTag "$Tag-srv3"
        try {
            $null = Assert-CheckClean -CheckExe $CheckExe -Target $Target -Pw $Pw -Mode "strict" -ExpectedTotal $ExpectedTotal -Tag "$Tag-strict"
        } catch {
            Write-Output "[RETRY] [$Tag] strict failed once, retrying: $($_.Exception.Message)"
            $srv = Start-Server -Exe $Exe -SrcDir $SrcDir -Pw $Pw -LogTag "$Tag-srv4"
            $null = Assert-CheckClean -CheckExe $CheckExe -Target $Target -Pw $Pw -Mode "strict" -ExpectedTotal $ExpectedTotal -Tag "$Tag-strict2"
        }
        $null = Wait-ExitCode -Proc $srv
    }
}

function Count-Files {
    param([string]$Root)
    return (Get-ChildItem -Path $Root -Recurse -File | Measure-Object).Count
}

# ---- preconditions: missing => FAIL (exit 2), never a silent pass -----------------------
$exe = Resolve-Tool -Hint $ExePath -Name "FastClone.exe"
$check = Resolve-Tool -Hint $CheckExePath -Name "FastCheck.exe"
if (-not $exe -or -not $check) {
    Write-Output "ERROR: clone_check_consistency preconditions not met (this is a FAILURE, not a skip)."
    Write-Output "  -ExePath='$ExePath'      -> resolved FastClone.exe='$exe'"
    Write-Output "  -CheckExePath='$CheckExePath' -> resolved FastCheck.exe='$check'"
    exit 2
}
$pw = "cc-invariant-pw"

# ---- S1: baseline tree (empty / 50B / 100KiB / 5MiB / nested / non-ASCII) --------------
{
    Assert-FcDeadline -Stage "S1"
    $base = New-TempDir "s1"
    $src = Join-Path $base "src"; $tgt = Join-Path $base "tgt"
    New-Item -ItemType Directory -Path $src | Out-Null
    $t = [datetime]::UtcNow
    Write-Binary (Join-Path $src "empty.bin") 0 $t
    Write-Binary (Join-Path $src "a_50b.txt") 50 $t
    Write-Binary (Join-Path $src "b_100k.bin") (100 * 1024) $t
    Write-Binary (Join-Path $src "c_5m.bin") (5 * 1024 * 1024) $t
    Write-Binary (Join-Path $src "nested/deep/d.dat") 2048 $t
    Write-Binary (Join-Path $src "nested/中文名.txt") 7 $t
    $total = Count-Files $src
    Sync-And-Verify -Exe $exe -CheckExe $check -SrcDir $src -Target $tgt -Pw $pw -ExpectedTotal $total -Tag "s1"
    Write-Output "S1 baseline: PASS"
    Remove-Item -Recurse -Force $base -ErrorAction SilentlyContinue
}

# ---- S2: historical / boundary mtimes (0 sentinel, 1985 area around the 5e17 ns
# threshold, future, 2001-01-01) - divergence-point A + TryNormalize boundary coverage ----
{
    Assert-FcDeadline -Stage "S2"
    $base = New-TempDir "s2"
    $src = Join-Path $base "src"; $tgt = Join-Path $base "tgt"
    New-Item -ItemType Directory -Path $src | Out-Null
    $cases = @(
        @{ f = "m_zero.bin";  t = [datetime]::new(1970, 1, 1, 0, 0, 1, [DateTimeKind]::Utc) },
        @{ f = "m_1985a.bin"; t = [datetime]::new(1985, 11, 4, 12, 0, 0, [DateTimeKind]::Utc) },  # < 5e17 ns
        @{ f = "m_1985b.bin"; t = [datetime]::new(1985, 11, 6, 12, 0, 0, [DateTimeKind]::Utc) },  # > 5e17 ns
        @{ f = "m_2001.bin";  t = [datetime]::new(2001, 1, 1, 0, 0, 0, [DateTimeKind]::Utc) },
        @{ f = "m_future.bin"; t = [datetime]::new(2099, 6, 1, 0, 0, 0, [DateTimeKind]::Utc) }
    )
    foreach ($c in $cases) { Write-Binary (Join-Path $src $c.f) 1234 $c.t }
    $total = Count-Files $src
    Sync-And-Verify -Exe $exe -CheckExe $check -SrcDir $src -Target $tgt -Pw $pw -ExpectedTotal $total -Tag "s2"
    Write-Output "S2 historical mtimes: PASS"
    Remove-Item -Recurse -Force $base -ErrorAction SilentlyContinue
}

# ---- S3: pre-existing extra file in the target -----------------------------------------
{
    Assert-FcDeadline -Stage "S3"
    $base = New-TempDir "s3"
    $src = Join-Path $base "src"; $tgt = Join-Path $base "tgt"
    New-Item -ItemType Directory -Path $src | Out-Null
    Write-Binary (Join-Path $src "keep.txt") 100 ([datetime]::UtcNow)
    # stale extra present BEFORE the sync: FastClone must delete it, FastCheck must see none
    New-Item -ItemType Directory -Path $tgt | Out-Null
    Write-Binary (Join-Path $tgt "stale_extra.bin") 55 ([datetime]::UtcNow)
    Sync-And-Verify -Exe $exe -CheckExe $check -SrcDir $src -Target $tgt -Pw $pw -ExpectedTotal 1 -Tag "s3"
    if (Test-Path (Join-Path $tgt "stale_extra.bin")) {
        throw "[s3] FastClone must delete the pre-existing extra (stale_extra.bin still present)"
    }
    Write-Output "S3 extra pre-seeded: PASS"
    Remove-Item -Recurse -Force $base -ErrorAction SilentlyContinue
}

# ---- S4: idempotent re-run (second sync transfers nothing) ------------------------------
{
    Assert-FcDeadline -Stage "S4"
    $base = New-TempDir "s4"
    $src = Join-Path $base "src"; $tgt = Join-Path $base "tgt"
    New-Item -ItemType Directory -Path $src | Out-Null
    Write-Binary (Join-Path $src "f1.bin") 4096 ([datetime]::UtcNow)
    Write-Binary (Join-Path $src "f2.bin") 8192 ([datetime]::UtcNow)
    Sync-And-Verify -Exe $exe -CheckExe $check -SrcDir $src -Target $tgt -Pw $pw -ExpectedTotal 2 -Tag "s4a"
    # second sync: transferred must be 0
    $srv = Start-Server -Exe $exe -SrcDir $src -Pw $pw -LogTag "s4b-srv"
    $syncCode = Invoke-Tool -Exe $exe -CliArgs @("--server", "127.0.0.1:$Port", "--target", $tgt, "--password", $pw) -OutLog "s4b-sync.out" -ErrLog "s4b-sync.err"
    $null = Wait-ExitCode -Proc $srv
    if ($syncCode -ne 0) { throw "[s4] second sync exited $syncCode" }
    $errText = Get-Content "s4b-sync.err" -Raw -ErrorAction SilentlyContinue
    $outText = Get-Content "s4b-sync.out" -Raw -ErrorAction SilentlyContinue
    if (($errText + $outText) -notmatch 'transferred=0(\D|$)') {
        throw "[s4] second sync must report transferred=0 (stats: $errText)"
    }
    $srv = Start-Server -Exe $exe -SrcDir $src -Pw $pw -LogTag "s4c-srv"
    $null = Assert-CheckClean -CheckExe $check -Target $tgt -Pw $pw -Mode "fast" -ExpectedTotal 2 -Tag "s4c"
    $null = Wait-ExitCode -Proc $srv
    Write-Output "S4 idempotent re-run: PASS"
    Remove-Item -Recurse -Force $base -ErrorAction SilentlyContinue
}

# ---- S5 (AC-08, hardcoded expectation): FastClone client binary inside the target ------
# FastClone itself is excluded from its own delete walk, so sync exits 0; FastCheck is a
# DIFFERENT process whose self-exclude does not cover FastClone.exe => it must report
# extra_local=1 with exactly that file (the residual divergence pinned by AC-08).
{
    Assert-FcDeadline -Stage "S5"
    $base = New-TempDir "s5"
    $src = Join-Path $base "src"; $tgt = Join-Path $base "tgt"
    New-Item -ItemType Directory -Path $src | Out-Null
    Write-Binary (Join-Path $src "one.txt") 100 ([datetime]::UtcNow)
    New-Item -ItemType Directory -Path $tgt -Force | Out-Null
    Copy-Item $exe (Join-Path $tgt "FastClone.exe") -Force
    # sync: FastClone deletes its other extras but must NOT delete itself -> exit 0
    $srv = Start-Server -Exe $exe -SrcDir $src -Pw $pw -LogTag "s5-srv1"
    $syncCode = Invoke-Tool -Exe $exe -CliArgs @("--server", "127.0.0.1:$Port", "--target", $tgt, "--password", $pw) -OutLog "s5-sync.out" -ErrLog "s5-sync.err"
    $null = Wait-ExitCode -Proc $srv
    if ($syncCode -ne 0) { throw "[s5] FastClone sync with its own exe in target must exit 0, got $syncCode" }
    if (-not (Test-Path (Join-Path $tgt "FastClone.exe"))) { throw "[s5] FastClone must not delete its own exe" }
    # check (fast): exactly one extra - the copied client binary
    $srv = Start-Server -Exe $exe -SrcDir $src -Pw $pw -LogTag "s5-srv2"
    $null = Assert-CheckExtra -CheckExe $check -Target $tgt -Pw $pw -Mode "fast" -ExpectedExtraRel "FastClone.exe" -Tag "s5-fast"
    $null = Wait-ExitCode -Proc $srv
    # check (strict): same expectation
    $srv = Start-Server -Exe $exe -SrcDir $src -Pw $pw -LogTag "s5-srv3"
    $null = Assert-CheckExtra -CheckExe $check -Target $tgt -Pw $pw -Mode "strict" -ExpectedExtraRel "FastClone.exe" -Tag "s5-strict"
    $null = Wait-ExitCode -Proc $srv
    Write-Output "S5 client-binary-in-target: PASS (FastClone exit 0; FastCheck exit 1 extra_local=1)"
    Remove-Item -Recurse -Force $base -ErrorAction SilentlyContinue
}

# ---- S6 (small change B anchor): FastCheck binary inside the target ---------------------
# BEFORE the fix: FastCheck without an exclude reported its own exe as extra (exit 1).
# AFTER the fix (SelfExcludeUnderRoot injected): exit 0, extra_local=0.
{
    Assert-FcDeadline -Stage "S6"
    $base = New-TempDir "s6"
    $src = Join-Path $base "src"; $tgt = Join-Path $base "tgt"
    New-Item -ItemType Directory -Path $src | Out-Null
    Write-Binary (Join-Path $src "data.bin") 200 ([datetime]::UtcNow)
    # sync a clean tree first
    $srv = Start-Server -Exe $exe -SrcDir $src -Pw $pw -LogTag "s6-srv1"
    $syncCode = Invoke-Tool -Exe $exe -CliArgs @("--server", "127.0.0.1:$Port", "--target", $tgt, "--password", $pw) -OutLog "s6-sync.out" -ErrLog "s6-sync.err"
    $null = Wait-ExitCode -Proc $srv
    if ($syncCode -ne 0) { throw "[s6] sync exited $syncCode" }
    # park FastCheck.exe inside the target and run it FROM there: its self-exclude must
    # kick in (rule-level parity with FastClone) -> clean verdict.
    Copy-Item $check (Join-Path $tgt "FastCheck.exe") -Force
    $inTargetCheck = Join-Path $tgt "FastCheck.exe"
    $srv = Start-Server -Exe $exe -SrcDir $src -Pw $pw -LogTag "s6-srv2"
    $null = Assert-CheckClean -CheckExe $inTargetCheck -Target $tgt -Pw $pw -Mode "fast" -ExpectedTotal 1 -Tag "s6-fast"
    $null = Wait-ExitCode -Proc $srv
    $srv = Start-Server -Exe $exe -SrcDir $src -Pw $pw -LogTag "s6-srv3"
    $null = Assert-CheckClean -CheckExe $inTargetCheck -Target $tgt -Pw $pw -Mode "strict" -ExpectedTotal 1 -Tag "s6-strict"
    $null = Wait-ExitCode -Proc $srv
    Write-Output "S6 self-exclude parity: PASS (FastCheck excludes its own exe under the target)"
    Remove-Item -Recurse -Force $base -ErrorAction SilentlyContinue
}

Write-Output "clone_check_consistency: ALL SCENARIOS PASSED"
exit 0
