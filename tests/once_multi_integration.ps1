# FastClone --once-multi (multi-session idle-grace server) integration tests (Windows).
# Usage: .\tests\once_multi_integration.ps1 [-ExePath path\to\FastClone.exe] [-Port 27894]
#            [-Case OM-1,OM-2,...]   # default: all six scenarios (ctest behavior unchanged)
# Creates small/large temp fixtures; always kills the FastClone processes it started.
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
#
# Task ci-once-multi-test-budget: per-case budget skeleton + frame-gate OM-4 abort
# (design 10 sections 4.1-4.6, gate 11 section 4 C1..C11, contract 12). A `-Case` subset run
# (e.g. -Case OM-4) is a LOCAL REPEAT vehicle only and must never be reported as a
# full-suite pass.

param(
    [string]$ExePath = "",
    [int]$Port = 27894,
    # Test-only: when set (ctest passes -BindLoopback on), the server binds ::1 instead of ::
    # so external LAN/WAN traffic cannot flood its single session (OM-4 flaky root cause).
    [switch]$BindLoopback,
    # [3.8] Scenario subset filter. Default = all six (ctest invocation unchanged).
    [string[]]$Case = @('OM-1', 'OM-2', 'OM-3', 'OM-4', 'OM-5a', 'OM-5b')
)

$ErrorActionPreference = "Stop"

# Precomputed server arg fragment: empty in production, "--bind-loopback" in test mode.
# Spliced into every Start-FastCloneProcess/Invoke-FastCloneSync server CliArgs below.
$bindLoopbackArg = if ($BindLoopback) { "--bind-loopback" } else { "" }

# ---------------------------------------------------------------------------
# Task ci-once-multi-test-budget: shared support module (design 03 section 3.2). Budget,
# bounded-wait, incremental log parsing and the OM-4 abort-gate decision live there so the
# isolated harness (tests/test_once_multi_test_support.ps1) drives the REAL functions under
# an injected fake clock. The other integration scripts keep their self-contained skeletons
# (their 480/600 budget structure is untouched by this task); once_multi is the only script
# re-based because it is the only one whose budget structure changed.
. "$PSScriptRoot\once_multi_test_support.ps1"

# ---------------------------------------------------------------------------
# [3.1] Budget constants. Every case gets its OWN budget counted from its OWN start, so a
# slow predecessor can never squeeze a later case (G-AC4; the old shared 480s deadline
# clamped OM-4's second client to 99s on CI run 44066798220). Values are the worst-step
# decompositions from design 03 section 3.1:
#   OM-1  90  = bind15 + c1a30 + c1b30 + srvExit15
#   OM-2 300  = bind15 + two-accept readiness 60 + client wait 210 + srvExit15
#   OM-3 300  = bind15 + c3a30 + armed10 + reset10 + grace window 7 + c3b200 + srvExit25
#   OM-4 350  = bind15 + gateReady10 + abortGate60 + settle15 + c4b210 + srvExit25 (+15 slack)
#               [10 section 4.6 / 12 section 5, C6: 330 -> 350 with the frame-gate proxy;
#                sum(case) = 1160, 1160+150 = 1310 <= 1320, 1320+180 = 1500 <= CTEST 1500]
#   OM-5a 75  = bind15 + c5 20 + probe 2 + srvExit30
#   OM-5b 45  = bind15 + probe 1 + alive window 8 + kill/portfree 15
$script:FcCaseBudgetSec = @{
    'OM-1'  = 90
    'OM-2'  = 300
    'OM-3'  = 300
    'OM-4'  = 350
    'OM-5a' = 75
    'OM-5b' = 45
}
# FIXED_SUITE_OVERHEAD 150 = 512MB sparse fixture creation + initial port sweep + final cleanup.
$script:FcFixedSuiteOverheadSec = 150
# SCRIPT_SUITE_DEADLINE 1320 = hard cap for the whole body (sum(case)=1160 + 150 = 1310 with
# 10s slack). This is the total cap: fixed at script start, never renewed, no
# extend-on-progress, no skip/retry-to-green (G-AC5, D-02).
$script:FcSuiteDeadlineSec = 1320
# DIAG_RESERVE 180 = the [1320,1500] window for the diagnostic dump + owned-process cleanup,
# so a ctest hard kill can never eat the diagnostics again (#58 lesson).
$script:FcDiagReserveSec = 180
# CTest TIMEOUT for once_multi_integration (CMakeLists, single line) = 1500.
$script:FcCtestTimeoutSec = 1500

# [3.1] Startup self-check: executable form of the two budget relations.
#   (1) sum(case) + FIXED  <= SCRIPT_SUITE_DEADLINE
#   (2) SCRIPT_SUITE_DEADLINE + DIAG_RESERVE <= CTEST_TIMEOUT
# Throws CONFIG-BUDGET-INVARIANT before anything runs if anyone breaks a constant.
$null = Test-FcBudgetInvariant -CaseBudgets $script:FcCaseBudgetSec `
    -FixedOverheadSec $script:FcFixedSuiteOverheadSec -SuiteDeadlineSec $script:FcSuiteDeadlineSec `
    -DiagReserveSec $script:FcDiagReserveSec -CtestTimeoutSec $script:FcCtestTimeoutSec

$script:FcSuiteDeadlineUtc = (Get-FcNowUtc).AddSeconds($script:FcSuiteDeadlineSec)
$script:FcCurrentCase = $null
$script:FcDiagEmitted = $false

function Enter-FcCase {
    param([string]$Name)
    # [G-AC4/G-AC5] Suite guard, then a FRESH per-case budget: CaseDeadline =
    # min(caseStart + BudgetSec, SCRIPT_SUITE_DEADLINE) -- gate 05 C1 authoritative wording
    # (NOT SuiteDeadline-DiagReserve; that would re-create the 99s clamp). The startup
    # invariant guarantees the suite cap is not binding for on-schedule cases, so each case
    # receives its full own budget no matter how much its predecessors spent.
    if ((Get-FcSuiteRemainingSec) -le 0) {
        throw "SUITE-BUDGET-EXHAUSTED entering case '$Name' (suite=$($script:FcSuiteDeadlineSec)s)"
    }
    $script:FcCurrentCase = New-FcCaseBudget -Name $Name `
        -BudgetSec $script:FcCaseBudgetSec[$Name] -SuiteDeadlineUtc $script:FcSuiteDeadlineUtc
    $script:FcDiagEmitted = $false
    Write-Host ("[BUDGET] case={0} budget={1}s case_remaining={2}s suite_remaining={3}s" -f `
        $Name, $script:FcCurrentCase.BudgetSec, (Get-FcCaseRemainingSec), (Get-FcSuiteRemainingSec))
}

# [FR-03] PID registry: only processes THIS script started. [3.6] The registry also records
# each process's StartTime so the PID-reuse guard requires BOTH the FastClone name AND an
# identical start time (a recycled PID would have a different one).
$script:FcOwnedPids = @{}

function Register-OwnedProcess {
    param([System.Diagnostics.Process]$Proc, [string]$Tag)
    $startUtc = $null
    try { $startUtc = $Proc.StartTime.ToUniversalTime() } catch { $startUtc = $null }
    $script:FcOwnedPids[[int]$Proc.Id] = [pscustomobject]@{ Tag = $Tag; StartTimeUtc = $startUtc }
    return $Proc
}

function Stop-OwnedFastClone {
    foreach ($pidKey in @($script:FcOwnedPids.Keys)) {
        $info = $script:FcOwnedPids[$pidKey]
        $p = Get-Process -Id $pidKey -ErrorAction SilentlyContinue
        # [FR-03/3.6] PID-reuse guard: stop it only if it is STILL named FastClone AND was
        # started at exactly the recorded time. Anything else is not ours -- never kill.
        $ours = $false
        if ($p -and $p.ProcessName -eq 'FastClone' -and $null -ne $info.StartTimeUtc) {
            try {
                if ($p.StartTime.ToUniversalTime() -eq $info.StartTimeUtc) { $ours = $true }
            } catch { $ours = $false }
        }
        if ($ours) {
            Stop-Process -Id $pidKey -Force -ErrorAction SilentlyContinue
        } elseif ($p) {
            Write-Host "[CLEANUP] skipping pid=$pidKey (name=$($p.ProcessName) no longer matches our registry entry)"
        }
    }
    $script:FcOwnedPids = @{}
}

function Stop-FcOwnedProcess {
    # [12 section 4 A7] Single-process first-abort stop with the triple ownership guard:
    # the pid must be in THIS run's registry, STILL be named FastClone, and carry the exact
    # StartTime recorded at registration (a recycled PID fails the start-time check; a
    # MISSING StartTimeUtc is a hard refusal -- fail-closed, 16 B-05). No whole-machine
    # kill, no by-port/by-name fallback for the abort path.
    # Returns $true if the stop was issued, $false if the process was already gone (the
    # CALLER must judge that case: no kill happened, so the A7 abort chain was not exercised).
    param([System.Diagnostics.Process]$Proc, [string]$StageTag = "stop-owned")
    if ($null -eq $Proc) { throw "$StageTag`: Stop-FcOwnedProcess called with no process" }
    if (-not $script:FcOwnedPids.ContainsKey([int]$Proc.Id)) {
        throw "$StageTag`: pid=$($Proc.Id) is not in this run's owned-process registry (refusing to touch it)"
    }
    $info = $script:FcOwnedPids[[int]$Proc.Id]
    $p = Get-Process -Id $Proc.Id -ErrorAction SilentlyContinue
    if (-not $p) { return $false }    # already exited on its own
    if ($p.ProcessName -ne 'FastClone') {
        throw "$StageTag`: pid=$($Proc.Id) name='$($p.ProcessName)' is not FastClone (pid reuse? refusing)"
    }
    # [16 B-05] Fail-closed on a MISSING StartTime: a registry entry without a recorded
    # creation time cannot satisfy the triple guard, so the stop is REFUSED (consistent
    # with Stop-OwnedFastClone's null->skip-kill behavior). The previous code skipped only
    # the comparison and still killed -- fail-open.
    if ($null -eq $info.StartTimeUtc) {
        throw "$StageTag`: pid=$($Proc.Id) registry entry has no recorded StartTimeUtc (fail-closed: refusing to stop)"
    }
    $same = $false
    try { $same = ($p.StartTime.ToUniversalTime() -eq $info.StartTimeUtc) } catch { $same = $false }
    if (-not $same) {
        throw "$StageTag`: pid=$($Proc.Id) StartTime differs from the registry entry (pid reuse? refusing)"
    }
    Stop-Process -Id $Proc.Id -Force -ErrorAction SilentlyContinue
    return $true
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

# [NFR-03/3.5] Timeout diagnostic block: stuck PID (+pid_start) + port netstat + client/server
# log tails + budget fields + last [phase]/[hb]/[wd] and [server]/[mp]/[om] markers, so any
# failure is attributable to a specific scenario and client instead of "(Timeout)".
# Reads $Port / $logDir via PowerShell dynamic scoping. [3.5] Sets $script:FcDiagEmitted so
# the catch block can tell whether this case already produced its diagnostics.
function Write-FcDiag {
    param([string]$StageTag, [string]$Reason, [int]$TimeoutSec,
          [System.Diagnostics.Process]$Proc, [string]$OutLog, [string]$ErrLog)
    $script:FcDiagEmitted = $true
    Write-Host "===== TIMEOUT-DIAG begin stage=$StageTag reason=$Reason timeout=${TimeoutSec}s ====="
    # [3.5] Fixed budget fields (G-AC7): case + budget + elapsed + remaining + suite_remaining.
    $caseName = '<none>'
    $budget = 0; $elapsed = 0; $remaining = 0
    if ($null -ne $script:FcCurrentCase) {
        $caseName = $script:FcCurrentCase.Name
        $budget = $script:FcCurrentCase.BudgetSec
        $elapsed = [int][Math]::Floor(((Get-FcNowUtc) - $script:FcCurrentCase.StartUtc).TotalSeconds)
        $remaining = Get-FcCaseRemainingSec
    }
    Write-Host ("  case={0} budget={1}s elapsed={2}s remaining={3}s suite_remaining={4}s" -f `
        $caseName, $budget, $elapsed, $remaining, (Get-FcSuiteRemainingSec))
    if ($Proc) {
        $p = Get-Process -Id $Proc.Id -ErrorAction SilentlyContinue
        Write-Host "  stuck_pid=$($Proc.Id) name=$(if ($p) { $p.ProcessName } else { '<exited>' })"
        if ($script:FcOwnedPids.ContainsKey([int]$Proc.Id)) {
            $st = $script:FcOwnedPids[[int]$Proc.Id].StartTimeUtc
            $stTxt = '<unknown>'
            if ($st) { $stTxt = $st.ToString('yyyy-MM-dd HH:mm:ss') + 'Z' }
            Write-Host "  pid_start=$stTxt"
        }
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
        $serverLast = $null
        Get-ChildItem -Path $logDir -File -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -match 'server' } | ForEach-Object {
                $t = Get-Content $_.FullName -Tail 20 -ErrorAction SilentlyContinue
                if ($t) {
                    Write-Host "  ----- tail $($_.Name) -----"
                    $t | ForEach-Object { Write-Host "    $_" }
                    $sl = $t | Select-String -Pattern '\[server\]|\[mp\]|\[om\]'
                    if ($sl) { $serverLast = $sl[-1].Line }
                }
            }
        if ($serverLast) { Write-Host "  server_state_last=$serverLast" }
        else             { Write-Host "  server_state_last=<none>" }
    }
    Write-Host "===== TIMEOUT-DIAG end ====="
}

# [10 section 4.4 A11 / 12 section 3] OM-4 frame-gate diagnostic helper: dumps the gate
# state/events/bytes/port (metadata ONLY -- the gate never logs payload or password) plus
# the tail of its own log. Called from the OM-4 failure paths BEFORE any cleanup runs.
# [36 §3.4 step12] Optional -CaseTag (default 'OM-4' -- every pre-existing call site and
# output line is unchanged); the OM-2 rework passes 'OM-2' so the same A11-shaped helper
# can dump both lifecycle gates without a misleading [OM-4] prefix.
function Write-FcGateDiag {
    param($GateObj, [string]$GateLog, [string]$CaseTag = 'OM-4')
    if ($null -eq $GateObj) {
        Write-Host "  [$CaseTag] frame gate: <not started>"
        return
    }
    try { Write-Host "  [$CaseTag] frame gate snapshot: $($GateObj.Snapshot())" } catch {
        Write-Host "  [$CaseTag] frame gate snapshot: <unavailable: $_>"
    }
    if ($GateLog -and (Test-Path -LiteralPath $GateLog)) {
        Write-Host "  ----- tail $(Split-Path -Leaf $GateLog) -----"
        Get-Content -LiteralPath $GateLog -Tail 25 -ErrorAction SilentlyContinue | ForEach-Object { Write-Host "    $_" }
    }
}

function Stop-AllFastClone {
    # [FR-03] Only processes THIS script started. The old implementation
    # (`Get-Process FastClone | Stop-Process -Force`) was a name-global kill that can
    # terminate unrelated FastClone processes on a shared build agent (RC-3).
    Stop-OwnedFastClone
    # Defensive, PORT-SCOPED fallback: a process we do not own but that holds OUR port would
    # break the next bind. Kill only that PID (still gated on the FastClone name -- a user
    # process on our port is NEVER killed) and log it.
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
    # [3.1] The wait is clamped to the CURRENT CASE's remaining budget (falls back to the
    # suite deadline outside cases), so a slow predecessor can no longer starve later cases
    # -- the old global clamp was exactly the 99s failure on CI run 44066798220.
    $eff = [Math]::Min($TimeoutSec, (Get-FcCaseRemainingSec))
    if ($eff -le 0) {
        # NOTE: WaitForExit(0) returns immediately regardless of state, so we must NOT fall
        # through with 0 -- that would silently kill a healthy process and look like a stall.
        $cn = '<suite>'
        if ($null -ne $script:FcCurrentCase) { $cn = $script:FcCurrentCase.Name }
        throw "CASE-BUDGET-EXHAUSTED waiting for pid=$($Proc.Id) stage=$StageTag (requested ${TimeoutSec}s, case=$cn)"
    }
    if ($Proc.HasExited) {
        $Proc.Refresh()
    } elseif (-not $Proc.WaitForExit($eff * 1000)) {
        Write-FcDiag -StageTag $StageTag -Reason "wait-exitcode-timeout" -TimeoutSec $eff `
            -Proc $Proc -OutLog $null -ErrLog $null
        Stop-Process -Id $Proc.Id -Force -ErrorAction SilentlyContinue
        throw "TIMEOUT ${StageTag}: pid=$($Proc.Id) did not exit within ${eff}s (CASE-BUDGET-EXHAUSTED, requested ${TimeoutSec}s)"
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
    # Drop null/empty fragments (e.g. the optional "--bind-loopback" slot is "" when off).
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
    # Cache the native handle so .ExitCode survives a self-exiting process (PowerShell quirk).
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
        [int]$TimeoutSec = 150,          # tier T0, see design section 3.2
        [string]$StageTag = "client"     # NFR-03: goes into the diagnostic block
    )
    foreach ($a in $CliArgs) {
        if ($null -eq $a -or $a -eq "") {
            throw "Invoke-FastCloneSync: null/empty argument"
        }
    }
    # [3.1] Case-budget pre-check (was: global Assert-FcDeadline).
    if ((Get-FcCaseRemainingSec) -le 0) {
        $cn = '<suite>'
        if ($null -ne $script:FcCurrentCase) { $cn = $script:FcCurrentCase.Name }
        throw "CASE-BUDGET-EXHAUSTED before stage='$StageTag' (case=$cn)"
    }

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

    # [3.1] Clamp to the CURRENT CASE's remaining budget (was: shared global deadline).
    $eff = [Math]::Min($TimeoutSec, (Get-FcCaseRemainingSec))
    if ($eff -le 0) { throw "CASE-BUDGET-EXHAUSTED before stage='$StageTag'" }

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

# Open a bare TCP connection and immediately close it (pre-handshake probe, session == null).
function Send-Probe {
    param([int]$ProbePort)
    $probe = New-Object System.Net.Sockets.TcpClient
    $probe.Connect("127.0.0.1", $ProbePort)
    Start-Sleep -Milliseconds 150
    $probe.Close()
}

# [3.8] Validate the -Case filter up front: no silent empty runs, no typo'd scenario names.
$validCases = @('OM-1', 'OM-2', 'OM-3', 'OM-4', 'OM-5a', 'OM-5b')
if ($null -eq $Case -or @($Case).Count -eq 0) {
    throw "Invalid -Case: at least one of $($validCases -join ',') is required"
}
foreach ($c in $Case) {
    if ($validCases -notcontains $c) {
        throw "Invalid -Case value '$c' (valid: $($validCases -join ','))"
    }
}

$exe = Resolve-FastCloneExe -Hint $ExePath
# [16 B-07] Exe provenance echo: every run log records WHICH local product binary executed
# the scenarios (path + mtime/size + --version self-report with the real exit code), so the
# A2/A3/A9 evidence is attributable to a concrete local build. The v2 runs' logs lacked
# this line -- the run exe had to be reconstructed from the probe scripts + post-v2 notes.
try {
    $exeItem = Get-Item -LiteralPath $exe
    $exeVerLines = @(& cmd /c "`"$exe`" --version 2>&1" | ForEach-Object { "$_" })
    Write-Host ("[EXE] path={0}" -f $exe)
    Write-Host ("[EXE] mtime={0} size={1}" -f $exeItem.LastWriteTime.ToString('yyyy-MM-dd HH:mm:ss'), $exeItem.Length)
    Write-Host ("[EXE] --version='{0}' exit={1}" -f ($exeVerLines -join ' '), $LASTEXITCODE)
} catch {
    Write-Host "[EXE] provenance echo failed: $_"
}
# [16 S-06] Unique temp root (regenerated on collision -- never overwrites an existing
# tree) + a per-run ownership marker: cleanup deletes ONLY a structurally-guarded root
# whose marker file carries THIS run's token (Test-FcSafeTempRoot + Test-FcOwnTempMarker).
$script:FcRunOwnerToken = [System.Guid]::NewGuid().ToString('N')
do {
    $root = Join-Path $env:TEMP "fastclone-onemulti-it-$(Get-Random)"
} while (Test-Path -LiteralPath $root)
$logDir = Join-Path $root "logs"
$src = Join-Path $root "src"
New-Item -ItemType Directory -Path $logDir, $src | Out-Null
Set-Content -LiteralPath (Join-Path $root '.fc-it-owner') -Value $script:FcRunOwnerToken

# Small fixture for quick real sessions.
1..15 | ForEach-Object {
    Set-Content -Path (Join-Path $src "file$_.txt") -Value ("payload-$_-" + ("x" * 4096))
}
New-Item -ItemType Directory -Force -Path (Join-Path $src "nested\deep") | Out-Null
Set-Content -Path (Join-Path $src "nested\deep\leaf.txt") -Value "nested"

# Large fixture for interruptible / long-running sessions (streams=1 + 1KB chunks is
# syscall-bound and stays in-flight for many seconds even over loopback).
# [36 §3.5] THREE physically isolated fixture variables, each written ONCE with its own
# SetLength in its own directory -- no variable is shared between cases, so no single
# SetLength can ever shrink or grow another case's source:
#   $src              small (15 files + nested)   OM-1 / OM-5a / OM-5b (verbatim, untouched)
#   $srcLifecycle128  128 MiB (134217728 bytes)   OM-2 srv2 + OM-3 srv3 + the two OM-2 gates
#   $srcBig512        512 MB (536870912 bytes)   OM-4 srv4 + its gate + the L776 cum guard ONLY
# 128 is NOT 512: no threshold of one is ever compared against the other (36 §3.5 note);
# the 512-level big-IO oracle stays with data_integrity/file_range (not touched, not run).
$script:FcLifecycleBytes = 134217728
$srcLifecycle128 = Join-Path $root "srclife128"
New-Item -ItemType Directory -Force -Path $srcLifecycle128 | Out-Null
$lifeFile = Join-Path $srcLifecycle128 "big.bin"
$fsLife = [System.IO.File]::Create($lifeFile)
try { $fsLife.SetLength($script:FcLifecycleBytes) } finally { $fsLife.Close() }
$script:FcBigBytes = 536870912
$srcBig512 = Join-Path $root "srcbig512"
New-Item -ItemType Directory -Force -Path $srcBig512 | Out-Null
$bigFile = Join-Path $srcBig512 "big.bin"
$fsBig = [System.IO.File]::Create($bigFile)
try { $fsBig.SetLength($script:FcBigBytes) } finally { $fsBig.Close() }

$password = "fc-om-pw"
$serverAddr = "127.0.0.1:$Port"
$grace = "3s"
$failures = 0

try {
    Stop-AllFastClone

    if ($Case -contains 'OM-1') {
    Write-Host "[OM-1] sequential: server survives between two real sessions, exits 0 after grace"
    Enter-FcCase -Name 'OM-1'
    if (-not (Wait-PortFree -PortNum $Port -TimeoutSec 15)) {
        Write-Host "[PORT-BUSY] port $Port still held before OM-1 server start:"
        (netstat -ano | Select-String -Pattern ":$Port ") | ForEach-Object { Write-Host "    $($_.Line)" }
    }
    $srv1 = Start-FastCloneProcess -Exe $exe -CliArgs @(
        "server", "--dir", $src, "--password", $password, "--port", "$Port", $bindLoopbackArg,
        "--once-multi", "--once-idle-grace", $grace
    ) -OutLog "$logDir\om1-server.out" -ErrLog "$logDir\om1-server.err"
    if (-not (Wait-PortListening -PortNum $Port -Proc $srv1 -TimeoutSec 15)) {
        Write-FcDiag -StageTag "OM-1/server-not-listening" -Reason "server-bind" -TimeoutSec 15 `
            -Proc $srv1 -OutLog "$logDir\om1-server.out" -ErrLog "$logDir\om1-server.err"
        throw "OM-1: server pid=$($srv1.Id) never reached LISTENING on port $Port within 15s"
    }
    $tgt1a = Join-Path $root "tgt1a"; New-Item -ItemType Directory -Force -Path $tgt1a | Out-Null
    $c1a = Invoke-FastCloneSync -Exe $exe -CliArgs @(
        "client", "--server", $serverAddr, "--target", $tgt1a, "--password", $password
    ) -OutLog "$logDir\om1-c1a.out" -ErrLog "$logDir\om1-c1a.err" -TimeoutSec 150 -StageTag "OM-1"
    if ($c1a -ne 0) { throw "OM-1: first client expected exit 0, got $c1a" }
    if ($srv1.HasExited) { throw "OM-1: server exited after first session (should survive within grace)" }
    $tgt1b = Join-Path $root "tgt1b"; New-Item -ItemType Directory -Force -Path $tgt1b | Out-Null
    $c1b = Invoke-FastCloneSync -Exe $exe -CliArgs @(
        "client", "--server", $serverAddr, "--target", $tgt1b, "--password", $password
    ) -OutLog "$logDir\om1-c1b.out" -ErrLog "$logDir\om1-c1b.err" -TimeoutSec 150 -StageTag "OM-1"
    if ($c1b -ne 0) { throw "OM-1: second client expected exit 0, got $c1b" }
    $code1 = Wait-ExitCode -Proc $srv1 -TimeoutSec 30
    if ($code1 -ne 0) { throw "OM-1: server expected exit 0 after grace, got $code1" }
    $done1 = (Select-String -Path "$logDir\om1-server.out" -Pattern "\[mp\] conn_done").Count
    if ($done1 -lt 2) { throw "OM-1: expected >=2 completed sessions in server log, got $done1" }
    Write-Host "  OK two sequential sessions served; server exit=$code1"
    }

    if ($Case -contains 'OM-2') {
    Write-Host "[OM-2] concurrent: two real sessions held at two frame gates, released together, exit 0 after both finish"
    Enter-FcCase -Name 'OM-2'
    if (-not (Wait-PortFree -PortNum $Port -TimeoutSec 15)) {
        Write-Host "[PORT-BUSY] port $Port still held before OM-2 server start:"
        (netstat -ano | Select-String -Pattern ":$Port ") | ForEach-Object { Write-Host "    $($_.Line)" }
    }
    # [36 section 3.4 step1] srv2 serves the 128MiB lifecycle source; the two gates below
    # and the SHA oracle use the SAME $script:FcLifecycleBytes constant -- no 512-level
    # threshold is ever compared against the 128MiB fixture (128 != 512, 36 section 3.5).
    $srv2 = Start-FastCloneProcess -Exe $exe -CliArgs @(
        "server", "--dir", $srcLifecycle128, "--password", $password, "--port", "$Port", $bindLoopbackArg,
        "--once-multi", "--once-idle-grace", $grace
    ) -OutLog "$logDir\om2-server.out" -ErrLog "$logDir\om2-server.err"
    if (-not (Wait-PortListening -PortNum $Port -Proc $srv2 -TimeoutSec 15)) {
        Write-FcDiag -StageTag "OM-2/server-not-listening" -Reason "server-bind" -TimeoutSec 15 `
            -Proc $srv2 -OutLog "$logDir\om2-server.out" -ErrLog "$logDir\om2-server.err"
        throw "OM-2: server pid=$($srv2.Id) never reached LISTENING on port $Port within 15s"
    }
    $g2a = $null
    $g2b = $null
    $gateLogA = Join-Path $logDir "om2-gate-a.log"
    $gateLogB = Join-Path $logDir "om2-gate-b.log"
    $om2Failed = $false
    try {
        # [36 section 3.4 step2] Two INDEPENDENT loopback frame gates, both upstreamed to
        # srv2, CloseMode=Graceful (SyncDone-observing half-close lifecycle), FixtureBytes =
        # the 128MiB lifecycle constant. Readiness = in-process Listening + the
        # FRAME_GATE_READY line on disk (A1-shaped; no TCP probe, C2).
        $g2a = Start-FcFrameGateProxy -UpstreamHost '127.0.0.1' -UpstreamPort $Port `
            -StopAfterType 31 -CapBytes 64MB -FixtureBytes $script:FcLifecycleBytes `
            -ReadTimeoutMs 60000 -CloseMode Graceful -LogPath $gateLogA
        $g2b = Start-FcFrameGateProxy -UpstreamHost '127.0.0.1' -UpstreamPort $Port `
            -StopAfterType 31 -CapBytes 64MB -FixtureBytes $script:FcLifecycleBytes `
            -ReadTimeoutMs 60000 -CloseMode Graceful -LogPath $gateLogB
        if ($g2a.State -ne [FcGate.GateState]::Listening -or $g2b.State -ne [FcGate.GateState]::Listening) {
            Write-FcDiag -StageTag "OM-2/gate-start" -Reason "FRAME-GATE-NOT-READY" -TimeoutSec 10 `
                -Proc $srv2 -OutLog "$logDir\om2-server.out" -ErrLog "$logDir\om2-server.err"
            Write-FcGateDiag -GateObj $g2a -GateLog $gateLogA -CaseTag 'OM-2'
            Write-FcGateDiag -GateObj $g2b -GateLog $gateLogB -CaseTag 'OM-2'
            throw "OM-2: FRAME-GATE-NOT-READY gate states a=$($g2a.State) b=$($g2b.State) (expected Listening)"
        }
        $readyA = (Select-String -Path $gateLogA -Pattern 'FRAME_GATE_READY' | Select-Object -First 1).Line
        $readyB = (Select-String -Path $gateLogB -Pattern 'FRAME_GATE_READY' | Select-Object -First 1).Line
        if (-not $readyA -or -not ($readyA.Contains("port=" + $g2a.Port)) -or
            -not $readyB -or -not ($readyB.Contains("port=" + $g2b.Port))) {
            throw "OM-2: FRAME-GATE-NOT-READY FRAME_GATE_READY line missing/wrong on disk (a='$readyA' b='$readyB')"
        }
        Write-Host ("  [OM-2] gates ready a={0} b={1} -> upstream 127.0.0.1:{2} (Graceful, fixture={3}B)" -f `
            $g2a.Port, $g2b.Port, $Port, $script:FcLifecycleBytes)

        # [36 section 3.4 step3] c2a/c2b each go THROUGH their own gate with BOTH --server
        # and the single --link pin aimed at it (pins[1..] empty => aux lanes = 0, the same
        # pinning as OM-4 c4a); the slow flags --streams 1 --chunk-kb 1 stay exactly as the
        # previous direct-connect OM-2 had them (no easy-green removal).
        $tgt2a = Join-Path $root "tgt2a"; New-Item -ItemType Directory -Force -Path $tgt2a | Out-Null
        $tgt2b = Join-Path $root "tgt2b"; New-Item -ItemType Directory -Force -Path $tgt2b | Out-Null
        $gateAddrA = "127.0.0.1:$($g2a.Port)"
        $gateAddrB = "127.0.0.1:$($g2b.Port)"
        $c2a = Start-FastCloneProcess -Exe $exe -CliArgs @(
            "client", "--server", $gateAddrA, "--link", "127.0.0.1=$gateAddrA",
            "--target", $tgt2a, "--password", $password,
            "--reconnect-retries", "0", "--streams", "1", "--chunk-kb", "1"
        ) -OutLog "$logDir\om2-c2a.out" -ErrLog "$logDir\om2-c2a.err"
        $c2b = Start-FastCloneProcess -Exe $exe -CliArgs @(
            "client", "--server", $gateAddrB, "--link", "127.0.0.1=$gateAddrB",
            "--target", $tgt2b, "--password", $password,
            "--reconnect-retries", "0", "--streams", "1", "--chunk-kb", "1"
        ) -OutLog "$logDir\om2-c2b.out" -ErrLog "$logDir\om2-c2b.err"
    # [3.4] Wait until BOTH concurrent sessions are really accepted (two [mp] conn_accept
    # lines in om2-server.out) instead of a fixed 3s sleep; bounded by min(60, case budget).
    # [36 section 3.4 step4] Kept as the NECESSARY-but-not-sufficient counter gate; the
    # real overlap proof is the step5 barrier + the step6 evidence snapshot.
    $script:Om2Offset = 0
    $script:Om2Accepts = 0
    $g2 = Wait-FcCondition -Condition {
        $new = Get-FcLogNewLines -Path "$logDir\om2-server.out" -Offset ([ref]$script:Om2Offset)
        foreach ($l in @($new)) { if ($l -match '\[mp\] conn_accept') { $script:Om2Accepts++ } }
        ($script:Om2Accepts -ge 2) -or $srv2.HasExited
    } -TimeoutSec ([Math]::Min(60, (Get-FcCaseRemainingSec))) -PollMs 250
    if ($srv2.HasExited) { throw "OM-2: server exited while two sessions were active" }
    if ($script:Om2Accepts -lt 2) {
        Write-FcDiag -StageTag "OM-2/concurrent-accept" -Reason "abort-signal-missing" -TimeoutSec 60 `
            -Proc $srv2 -OutLog "$logDir\om2-c2a.out" -ErrLog "$logDir\om2-c2a.err"
        throw "OM-2: expected 2 [mp] conn_accept within window, saw $($script:Om2Accepts) (ABORT-SIGNAL-MISSING)"
    }

    # [36 section 3.4 step5] BARRIER: BOTH gates Held at their first complete FileChunk
    # while srv2 is still up -- the mechanism-level overlap proof (each client has
    # provably received its first complete chunk and provably not finished; 32-R1: not a
    # conn_accept/live_conns accumulation, not an active_real sample).
    $barrierOk = Wait-FcCondition -Condition {
        ($g2a.State -eq [FcGate.GateState]::Held -and $g2b.State -eq [FcGate.GateState]::Held) -or
        $srv2.HasExited
    } -TimeoutSec ([Math]::Min(60, (Get-FcCaseRemainingSec))) -PollMs 100
    if (-not ($g2a.State -eq [FcGate.GateState]::Held -and $g2b.State -eq [FcGate.GateState]::Held)) {
        Write-FcDiag -StageTag "OM-2/gate-barrier" -Reason "FRAME-GATE-NO-FILECHUNK" -TimeoutSec 60 `
            -Proc $srv2 -OutLog "$logDir\om2-server.out" -ErrLog "$logDir\om2-server.err"
        Write-FcGateDiag -GateObj $g2a -GateLog $gateLogA -CaseTag 'OM-2'
        Write-FcGateDiag -GateObj $g2b -GateLog $gateLogB -CaseTag 'OM-2'
        throw "OM-2: FRAME-GATE-NO-FILECHUNK both gates not Held within window (a=$($g2a.State) b=$($g2b.State))"
    }

    # [36 section 3.4 step6] Evidence snapshot, persisted ONCE, BEFORE the joint Resume --
    # this is what the overlap claim stands on.
    # (a) both gate logs: FIRST_FILECHUNK type=31 len>0 cum < FcLifecycleBytes.
    $chunkLineA = (Select-String -Path $gateLogA -Pattern 'FRAME_GATE_FIRST_FILECHUNK' | Select-Object -First 1).Line
    $chunkLineB = (Select-String -Path $gateLogB -Pattern 'FRAME_GATE_FIRST_FILECHUNK' | Select-Object -First 1).Line
    $cLenA = -1; $cCumA = -1; $cLenB = -1; $cCumB = -1
    if ($chunkLineA) {
        if ($chunkLineA -match 'len=(\d+)') { $cLenA = [long]$Matches[1] }
        if ($chunkLineA -match 'cum=(\d+)') { $cCumA = [long]$Matches[1] }
    }
    if ($chunkLineB) {
        if ($chunkLineB -match 'len=(\d+)') { $cLenB = [long]$Matches[1] }
        if ($chunkLineB -match 'cum=(\d+)') { $cCumB = [long]$Matches[1] }
    }
    if ($cLenA -le 0 -or $cCumA -le 0 -or $cCumA -ge $script:FcLifecycleBytes -or
        $cLenB -le 0 -or $cCumB -le 0 -or $cCumB -ge $script:FcLifecycleBytes) {
        Write-FcGateDiag -GateObj $g2a -GateLog $gateLogA -CaseTag 'OM-2'
        Write-FcGateDiag -GateObj $g2b -GateLog $gateLogB -CaseTag 'OM-2'
        throw "OM-2: FRAME-GATE-NO-FILECHUNK bad first-chunk evidence (a len=$cLenA cum=$cCumA; b len=$cLenB cum=$cCumB; fixture=$($script:FcLifecycleBytes))"
    }
    # (b) exactly one accepted connection per gate (multi-lane bypass guard).
    if ($g2a.AcceptedConnections -ne 1 -or $g2b.AcceptedConnections -ne 1) {
        Write-FcGateDiag -GateObj $g2a -GateLog $gateLogA -CaseTag 'OM-2'
        Write-FcGateDiag -GateObj $g2b -GateLog $gateLogB -CaseTag 'OM-2'
        throw "OM-2: MULTI-LANE gate accepted a=$($g2a.AcceptedConnections) b=$($g2b.AcceptedConnections) (expected 1 each)"
    }
    # (c) the two [mp] conn_accept sessionIds are DISTINCT (source-level session identity).
    $acceptIds = @()
    foreach ($al in @(Select-String -Path "$logDir\om2-server.out" -Pattern '\[mp\] conn_accept' |
            ForEach-Object { $_.Line })) {
        if ($al -match 'sessionId=(\S+)') { $acceptIds += $Matches[1] }
    }
    $distinctAcceptIds = @($acceptIds | Select-Object -Unique)
    if ($distinctAcceptIds.Count -ne 2) {
        throw "OM-2: SESSION-NOT-DISTINCT conn_accept sessionIds not 2 distinct values (got: $($acceptIds -join ','))"
    }
    # (d) both clients .err show [phase] tag=aux_end n=0 (zero aux lanes). The line is
    # emitted unconditionally (client L803-804) shortly after the lanes settle; the
    # bounded wait (A3-shaped) keeps a not-yet-written line from being read as a bypass.
    $script:Om2AuxOffA = 0
    $script:Om2AuxOffB = 0
    $script:Om2AuxA = $false
    $script:Om2AuxB = $false
    $null = Wait-FcCondition -Condition {
        $na = Get-FcLogNewLines -Path "$logDir\om2-c2a.err" -Offset ([ref]$script:Om2AuxOffA)
        foreach ($l in @($na)) { if ($l -match '\[phase\] tag=aux_end n=') { $script:Om2AuxA = $true } }
        $nb = Get-FcLogNewLines -Path "$logDir\om2-c2b.err" -Offset ([ref]$script:Om2AuxOffB)
        foreach ($l in @($nb)) { if ($l -match '\[phase\] tag=aux_end n=') { $script:Om2AuxB = $true } }
        ($script:Om2AuxA -and $script:Om2AuxB) -or $c2a.HasExited -or $c2b.HasExited
    } -TimeoutSec ([Math]::Min(10, (Get-FcCaseRemainingSec))) -PollMs 250
    foreach ($ce in @(@("$logDir\om2-c2a.err", 'c2a'), @("$logDir\om2-c2b.err", 'c2b'))) {
        $auxLine = (Select-String -Path $ce[0] -Pattern '\[phase\] tag=aux_end n=(\d+)' | Select-Object -First 1).Line
        $auxN = -1
        if ($auxLine -and $auxLine -match 'n=(\d+)') { $auxN = [int]$Matches[1] }
        if ($auxN -ne 0) {
            throw "OM-2: PROXY-BYPASS-AUX-LANE $($ce[1]) aux_end n=$auxN (expected 0; missing line is also a failure)"
        }
    }
    # (e) both clients still ALIVE at the barrier moment (Refresh before asking).
    $c2a.Refresh()
    $c2b.Refresh()
    if ($c2a.HasExited -or $c2b.HasExited) {
        throw "OM-2: CLIENT-FINISHED-EARLY c2a exited=$($c2a.HasExited) c2b exited=$($c2b.HasExited) at the barrier"
    }
    # (f) auxiliary pure-function re-check over the SAME just-collected observable values
    # (Test-FcOm2OverlapEvidence is a helper, never the only overlap evidence).
    $overlap = Test-FcOm2OverlapEvidence -E @{
        GateA_Held    = ($g2a.State -eq [FcGate.GateState]::Held)
        GateB_Held    = ($g2b.State -eq [FcGate.GateState]::Held)
        GateA_Cum     = $cCumA
        GateB_Cum     = $cCumB
        Accepted_A    = $g2a.AcceptedConnections
        Accepted_B    = $g2b.AcceptedConnections
        ClientA_Alive = (-not $c2a.HasExited)
        ClientB_Alive = (-not $c2b.HasExited)
        SessionIds    = $acceptIds
        AuxEndA       = 0
        AuxEndB       = 0
        FixtureBytes  = $script:FcLifecycleBytes
    }
    if (-not $overlap.Ok) {
        throw "OM-2: overlap evidence recheck failed: $($overlap.Reasons -join ',')"
    }
    Write-Host ("  [OM-2] barrier evidence: both gates Held (a len={0}B cum={1}B; b len={2}B cum={3}B); sessions {4} / {5}; both clients alive; aux=0" -f `
        $cLenA, $cCumA, $cLenB, $cCumB, $distinctAcceptIds[0], $distinctAcceptIds[1])

    # [36 section 3.4 step7] JOINT release: two consecutive statements -- never "finish A
    # then release B" (that would prove nothing about overlap).
    $g2a.Resume(); $g2b.Resume()

    # [36 section 3.4 step8] Both clients exit 0 (each wait clamped to the case budget).
    # [r44 41-B2] restored the pre-r40 240s window -- 39 S-03's 240->30 tightening was an
    # unauthorized cap change; the no-timeout-as-completion oracle stays the product-evidence
    # block right below (no join_recv heartbeat + join_recv->exit <= 5000ms).
    $code2a = Wait-ExitCode -Proc $c2a -TimeoutSec 240
    $code2b = Wait-ExitCode -Proc $c2b -TimeoutSec 240
    if ($code2a -ne 0) { throw "OM-2: client A expected exit 0, got $code2a" }
    if ($code2b -ne 0) { throw "OM-2: client B expected exit 0, got $code2b" }

    # [39 B-01 product evidence] The forwarded downstream EOF must wake each client's blocked
    # recv thread immediately (sync_engine_client.cpp L1421-1422; pre-fix the join hung on the
    # 120s idle watchdog, L5115-5124/L681, logging 5s-interval [hb] stage=join_recv heartbeats
    # -- the retained r38 run showed 24 of them before exit at t_ms=120591). Hard per-client
    # evidence: (1) NO join_recv heartbeat line may exist, and (2) the exit phase must land
    # within 5000ms of the join_recv phase (the direct-connection reference is ~5ms; 5000ms
    # = one heartbeat interval, i.e. strictly before any watchdog rescue could fire).
    foreach ($ce in @(@("$logDir\om2-c2a.err", 'c2a'), @("$logDir\om2-c2b.err", 'c2b'))) {
        $cliErrText = Get-FcLogFullText -Path $ce[0]
        if ($null -eq $cliErrText) { $cliErrText = '' }
        if ($cliErrText -match '\[hb\][^\r\n]*stage=join_recv') {
            throw "OM-2: $($ce[1]) still blocks in join_recv ([hb] stage=join_recv heartbeat present -- the downstream EOF was not forwarded; the pre-fix run relied on the 120s idle-watchdog rescue)"
        }
        $jrMs = -1; $exitMs = -1
        foreach ($pl in @($cliErrText -split "`r?`n")) {
            if ($pl -match '\[phase\] tag=join_recv t_ms=(\d+)') { $jrMs = [int]$Matches[1] }
            elseif ($pl -match '\[phase\] tag=exit code=\d+ t_ms=(\d+)') { $exitMs = [int]$Matches[1] }
        }
        if ($jrMs -lt 0 -or $exitMs -lt 0) {
            throw "OM-2: $($ce[1]) .err lacks the join_recv/exit phase lines (join_recv=$jrMs exit=$exitMs)"
        }
        if (($exitMs - $jrMs) -gt 5000) {
            throw "OM-2: $($ce[1]) join_recv->exit gap $($exitMs - $jrMs)ms exceeds 5000ms (the client was NOT woken by the forwarded downstream EOF; pre-fix gap was ~120000ms)"
        }
    }

    # [36 section 3.4 step9] Server-side protocol completion: srv2 exit 0 within 30s,
    # EXACTLY two [mp] conn_done whose sessionId SET equals the two conn_accept sessionIds
    # (source-level reconciliation, sync_engine_server.cpp L2627/L2641-2642), and NO
    # [mp] conn_error anywhere on this clean path (the gate's own SyncDone/Finished
    # self-evidence is only the transport layer; the PROTOCOL verdict is this joint chain).
    $code2 = Wait-ExitCode -Proc $srv2 -TimeoutSec 30
    if ($code2 -ne 0) { throw "OM-2: server expected exit 0 after both finished, got $code2" }
    $doneLines = @(Select-String -Path "$logDir\om2-server.out" -Pattern '\[mp\] conn_done' |
        ForEach-Object { $_.Line })
    if ($doneLines.Count -ne 2) {
        throw "OM-2: expected exactly 2 [mp] conn_done, got $($doneLines.Count) ($($doneLines -join ' | '))"
    }
    $doneIds = @()
    foreach ($dl in $doneLines) { if ($dl -match 'sessionId=(\S+)') { $doneIds += $Matches[1] } }
    $distinctDoneIds = @($doneIds | Select-Object -Unique)
    if ($distinctDoneIds.Count -ne 2) {
        throw "OM-2: conn_done sessionIds not 2 distinct values (got: $($doneIds -join ','))"
    }
    $missingIds = @($distinctAcceptIds | Where-Object { $distinctDoneIds -notcontains $_ })
    $extraIds = @($distinctDoneIds | Where-Object { $distinctAcceptIds -notcontains $_ })
    if ($missingIds.Count -gt 0 -or $extraIds.Count -gt 0) {
        throw "OM-2: conn_done/conn_accept sessionId reconciliation failed (missing=$($missingIds -join ',') extra=$($extraIds -join ','))"
    }
    $om2ErrText = Get-FcLogFullText -Path "$logDir\om2-server.err"
    if ($null -ne $om2ErrText -and $om2ErrText -match '\[mp\] conn_error') {
        throw "OM-2: clean path produced [mp] conn_error (see om2-server.err)"
    }

    # [36 section 3.4 step10] Both gates reach Finished inside the bounded 15s window
    # (the 60s read timeout stays a stall backstop only, never the normal terminator);
    # SYNCDONE_FORWARDED strictly before FINISHED per gate; no ABORTED, no ERROR.
    foreach ($gp in @(@($g2a, $gateLogA, 'a'), @($g2b, $gateLogB, 'b'))) {
        $gObj = $gp[0]; $gLog = $gp[1]; $gTag = $gp[2]
        if (-not $gObj.WaitForState([FcGate.GateState]::Finished, 15000)) {
            Write-FcGateDiag -GateObj $gObj -GateLog $gLog -CaseTag 'OM-2'
            throw "OM-2: gate $gTag did not reach Finished within 15s (state=$($gObj.State))"
        }
        $gLogText = Get-Content -LiteralPath $gLog -Raw
        if (-not (Test-FcGateFinishedOrder -LogText $gLogText)) {
            throw "OM-2: gate $gTag log lacks SYNCDONE_FORWARDED-before-FINISHED order evidence"
        }
        if ($gLogText -notmatch '(?m)^FRAME_GATE_DOWNSTREAM_HALF_CLOSE_SEND ') {
            throw "OM-2: gate $gTag log lacks the downstream half-close evidence (the server EOF was not forwarded to the client; 39 B-01)"
        }
        if ($gLogText -match 'FRAME_GATE_UPSTREAM_ABORTED' -or $gLogText -match 'FRAME_GATE_ERROR') {
            throw "OM-2: gate $gTag log shows ABORTED/ERROR on the clean path"
        }
    }

    # [36 section 3.4 step11] Full byte equivalence: source vs BOTH targets (SHA256 + exact
    # length; the target file carries the same relative name as the source leaf).
    $tgt2aFile = Join-Path $tgt2a "big.bin"
    $tgt2bFile = Join-Path $tgt2b "big.bin"
    foreach ($tf in @($tgt2aFile, $tgt2bFile)) {
        if (-not (Test-Path -LiteralPath $tf)) {
            throw "OM-2: target file missing after sync: $tf"
        }
        $tl = (Get-Item -LiteralPath $tf).Length
        if ($tl -ne $script:FcLifecycleBytes) {
            throw "OM-2: target length $tl != $($script:FcLifecycleBytes) ($tf)"
        }
    }
    $srcHash2 = (Get-FileHash -LiteralPath $lifeFile -Algorithm SHA256).Hash
    $hashA2 = (Get-FileHash -LiteralPath $tgt2aFile -Algorithm SHA256).Hash
    $hashB2 = (Get-FileHash -LiteralPath $tgt2bFile -Algorithm SHA256).Hash
    if ($hashA2 -ne $srcHash2 -or $hashB2 -ne $srcHash2) {
        throw "OM-2: SHA256 mismatch src=$srcHash2 a=$hashA2 b=$hashB2"
    }
    Write-Host "  OK two concurrent sessions through both gates; server exit=$code2; SHA256 three-way equal"
    }
    catch {
        # [36 section 3.4 step12] A11-shaped: both gate snapshots + log tails BEFORE any
        # cleanup, then the ORIGINAL exception propagates unchanged (never masked).
        $om2Failed = $true
        if (-not $script:FcDiagEmitted) {
            Write-FcDiag -StageTag "OM-2/unhandled" -Reason "unhandled-failure" -TimeoutSec 0 `
                -Proc $null -OutLog $null -ErrLog $null
        }
        Write-FcGateDiag -GateObj $g2a -GateLog $gateLogA -CaseTag 'OM-2'
        Write-FcGateDiag -GateObj $g2b -GateLog $gateLogB -CaseTag 'OM-2'
        throw
    }
    finally {
        # [36 section 3.4 step13 / section 2.3] Per-gate Dispose (aggregate 5000ms each)
        # BEFORE the global Stop-AllFastClone -- killing the server first would make the
        # gates' pending reads fail and pollute the verdict. A dispose timeout is APPENDED
        # (FRAME-GATE-CLOSE-TIMEOUT) and never masks the original failure.
        foreach ($gObj in @($g2a, $g2b)) {
            if ($null -ne $gObj) {
                try { $gObj.Dispose() } catch {
                    Write-Host "  [OM-2] FRAME-GATE-CLOSE-TIMEOUT: gate dispose threw: $_" -ForegroundColor Red
                }
                if ($gObj.DisposeTimedOut) {
                    $closeFail = "OM-2: FRAME-GATE-CLOSE-TIMEOUT gate worker threads did not join within $($gObj.DisposeTimeoutMs)ms"
                    Write-Host "FAILED: $closeFail" -ForegroundColor Red
                    if (-not $om2Failed) { throw $closeFail }
                }
            }
        }
    }
    }

    if ($Case -contains 'OM-3') {
    Write-Host "[OM-3] grace race: a new real session before grace expiry resets the timer"
    Enter-FcCase -Name 'OM-3'
    $longGrace = "5s"
    if (-not (Wait-PortFree -PortNum $Port -TimeoutSec 15)) {
        Write-Host "[PORT-BUSY] port $Port still held before OM-3 server start:"
        (netstat -ano | Select-String -Pattern ":$Port ") | ForEach-Object { Write-Host "    $($_.Line)" }
    }
    $srv3 = Start-FastCloneProcess -Exe $exe -CliArgs @(
        "server", "--dir", $srcLifecycle128, "--password", $password, "--port", "$Port", $bindLoopbackArg,
        "--once-multi", "--once-idle-grace", $longGrace
    ) -OutLog "$logDir\om3-server.out" -ErrLog "$logDir\om3-server.err"
    if (-not (Wait-PortListening -PortNum $Port -Proc $srv3 -TimeoutSec 15)) {
        Write-FcDiag -StageTag "OM-3/server-not-listening" -Reason "server-bind" -TimeoutSec 15 `
            -Proc $srv3 -OutLog "$logDir\om3-server.out" -ErrLog "$logDir\om3-server.err"
        throw "OM-3: server pid=$($srv3.Id) never reached LISTENING on port $Port within 15s"
    }
    # First (quick) session completes and arms the grace timer.
    $tgt3a = Join-Path $root "tgt3a"; New-Item -ItemType Directory -Force -Path $tgt3a | Out-Null
    $c3a = Invoke-FastCloneSync -Exe $exe -CliArgs @(
        "client", "--server", $serverAddr, "--target", $tgt3a, "--password", $password
    ) -OutLog "$logDir\om3-c3a.out" -ErrLog "$logDir\om3-c3a.err" -TimeoutSec 150 -StageTag "OM-3"
    if ($c3a -ne 0) { throw "OM-3: first client expected exit 0, got $c3a" }
    $armedAt = Get-Date
    # [3.4] Wait for the grace to REALLY arm ([om] idle_grace_armed in om3-server.out)
    # instead of a fixed 1500ms sleep, so the second session below provably RESETS an armed
    # timer; bounded by min(10, case budget).
    $script:Om3Offset = 0
    $script:Om3Armed = 0
    $g3a = Wait-FcCondition -Condition {
        $new = Get-FcLogNewLines -Path "$logDir\om3-server.out" -Offset ([ref]$script:Om3Offset)
        foreach ($l in @($new)) { if ($l -match 'idle_grace_armed') { $script:Om3Armed++ } }
        ($script:Om3Armed -ge 1) -or $srv3.HasExited
    } -TimeoutSec ([Math]::Min(10, (Get-FcCaseRemainingSec))) -PollMs 200
    if ($srv3.HasExited) { throw "OM-3: server exited during initial grace window (grace too short?)" }
    if ($script:Om3Armed -lt 1) {
        Write-FcDiag -StageTag "OM-3/grace-armed" -Reason "abort-signal-missing" -TimeoutSec 10 `
            -Proc $srv3 -OutLog "$logDir\om3-c3a.out" -ErrLog "$logDir\om3-c3a.err"
        throw "OM-3: idle_grace_armed not observed within window (ABORT-SIGNAL-MISSING)"
    }
    # Still well before the 5s grace expires, start a long second session so the timer must reset.
    $tgt3b = Join-Path $root "tgt3b"; New-Item -ItemType Directory -Force -Path $tgt3b | Out-Null
    $c3b = Start-FastCloneProcess -Exe $exe -CliArgs @(
        "client", "--server", $serverAddr, "--target", $tgt3b, "--password", $password,
        "--streams", "1", "--chunk-kb", "1"
    ) -OutLog "$logDir\om3-c3b.out" -ErrLog "$logDir\om3-c3b.err"
    # [3.4] Wait for the REAL reset ([om] idle_grace_reset) instead of a fixed 2s sleep;
    # bounded by min(10, case budget).
    $script:Om3Reset = 0
    $g3r = Wait-FcCondition -Condition {
        $new = Get-FcLogNewLines -Path "$logDir\om3-server.out" -Offset ([ref]$script:Om3Offset)
        foreach ($l in @($new)) { if ($l -match 'idle_grace_reset') { $script:Om3Reset++ } }
        ($script:Om3Reset -ge 1) -or $srv3.HasExited
    } -TimeoutSec ([Math]::Min(10, (Get-FcCaseRemainingSec))) -PollMs 200
    if ($srv3.HasExited) { throw "OM-3: server exited despite a new session arriving during grace" }
    if ($script:Om3Reset -lt 1) {
        Write-FcDiag -StageTag "OM-3/grace-reset" -Reason "abort-signal-missing" -TimeoutSec 10 `
            -Proc $srv3 -OutLog "$logDir\om3-c3b.out" -ErrLog "$logDir\om3-c3b.err"
        throw "OM-3: idle_grace_reset not observed within window (ABORT-SIGNAL-MISSING)"
    }
    # Wait until safely past "first-session-completed + grace": server must STILL be alive,
    # proving the grace timer was reset by the second session (AC-10). [C6] This finite 7s
    # window carries the real grace timing (not an unbounded sleep, not a completeness wait)
    # and is implicitly clamped by the case budget.
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
    }

    if ($Case -contains 'OM-4') {
    Write-Host "[OM-4] failure aggregation: aborted session -> sticky exit 5 (clean later session does not mask it)"
    Enter-FcCase -Name 'OM-4'
    if (-not (Wait-PortFree -PortNum $Port -TimeoutSec 15)) {
        Write-Host "[PORT-BUSY] port $Port still held before OM-4 server start:"
        (netstat -ano | Select-String -Pattern ":$Port ") | ForEach-Object { Write-Host "    $($_.Line)" }
    }
    $srv4 = Start-FastCloneProcess -Exe $exe -CliArgs @(
        "server", "--dir", $srcBig512, "--password", $password, "--port", "$Port", $bindLoopbackArg,
        "--once-multi", "--once-idle-grace", $grace
    ) -OutLog "$logDir\om4-server.out" -ErrLog "$logDir\om4-server.err"
    if (-not (Wait-PortListening -PortNum $Port -Proc $srv4 -TimeoutSec 15)) {
        Write-FcDiag -StageTag "OM-4/server-not-listening" -Reason "server-bind" -TimeoutSec 15 `
            -Proc $srv4 -OutLog "$logDir\om4-server.out" -ErrLog "$logDir\om4-server.err"
        throw "OM-4: server pid=$($srv4.Id) never reached LISTENING on port $Port within 15s"
    }

    # [10 section 4.4 / 12 section 4] Frame-gate abort chain A1..A11. The old rate-tick
    # gate (03 section 3.3 S2, structurally unreachable on fast machines per 07 B-01) is
    # REPLACED by the in-process loopback proxy: c4a connects through the gate, the gate
    # holds after the FIRST complete FileChunk frame (machine-speed independent), the TEST
    # kills the owned c4a, and the gate RSTs the single upstream socket.
    $gate = $null
    $gateLog = Join-Path $logDir "om4-gate.log"
    $om4Failed = $false
    try {
        # A1: start the gate AFTER the server is LISTENING and BEFORE c4a. Readiness is the
        # in-process Bind+Listen + FRAME_GATE_READY log line ONLY -- no TCP probe is ever
        # aimed at the gate port (C2; Send-Probe is only used by OM-5 against $Port).
        $gate = Start-FcFrameGateProxy -UpstreamHost '127.0.0.1' -UpstreamPort $Port `
            -StopAfterType 31 -CapBytes 64MB -FixtureBytes 512MB -ReadTimeoutMs 60000 -LogPath $gateLog
        if ($gate.State -ne [FcGate.GateState]::Listening) {
            Write-FcDiag -StageTag "OM-4/gate-start" -Reason "FRAME-GATE-NOT-READY" -TimeoutSec 10 `
                -Proc $srv4 -OutLog "$logDir\om4-server.out" -ErrLog "$logDir\om4-server.err"
            throw "OM-4: FRAME-GATE-NOT-READY gate state=$($gate.State) (expected Listening)"
        }
        Write-Host "  [OM-4] frame gate ready port=$($gate.Port) -> upstream 127.0.0.1:$Port"

        # [10 section 4.3, C3/C5] c4a goes THROUGH the gate with BOTH --server and the
        # single --link pin aimed at it: pins[0] overrides the primary lane onto the gate,
        # pins[1..] is empty => aux lanes = 0 (sync_engine_client.cpp L364-369/L734-739),
        # and advertised endpoints are only consumed in the automatic branch (L391), so the
        # 12-endpoint bypass of 07 S-04 is pinned shut. 512MB fixture + streams 1 + chunk-kb
        # 1 stay exactly as before (G-AC1).
        $gateAddr = "127.0.0.1:$($gate.Port)"
        $tgt4a = Join-Path $root "tgt4a"; New-Item -ItemType Directory -Force -Path $tgt4a | Out-Null
        $c4a = Start-FastCloneProcess -Exe $exe -CliArgs @(
            "client", "--server", $gateAddr, "--link", "127.0.0.1=$gateAddr",
            "--target", $tgt4a, "--password", $password,
            "--reconnect-retries", "0", "--streams", "1", "--chunk-kb", "1"
        ) -OutLog "$logDir\om4-c4a.out" -ErrLog "$logDir\om4-c4a.err"

        # A2 (first half): exactly one connection reaches the gate within 10s.
        $null = Wait-FcCondition -Condition {
            ($gate.AcceptedConnections -ge 1) -or $c4a.HasExited
        } -TimeoutSec ([Math]::Min(10, (Get-FcCaseRemainingSec))) -PollMs 200
        if ($gate.AcceptedConnections -ne 1) {
            Write-FcDiag -StageTag "OM-4/gate-accept" -Reason "PROXY-BYPASS-MULTI-CONN" -TimeoutSec 10 `
                -Proc $c4a -OutLog "$logDir\om4-c4a.out" -ErrLog "$logDir\om4-c4a.err"
            throw "OM-4: PROXY-BYPASS-MULTI-CONN gate accepted $($gate.AcceptedConnections) connections (expected exactly 1)"
        }

        # A4: the own-case server really accepted the proxied session (S1, [mp] conn_accept
        # only prints after a successful handshake; Test-FcSessionAccepted kept).
        $script:Om4SrvOutOffset = 0
        $null = Wait-FcCondition -Condition {
            $new = Get-FcLogNewLines -Path "$logDir\om4-server.out" -Offset ([ref]$script:Om4SrvOutOffset)
            (Test-FcSessionAccepted -Lines $new) -or $srv4.HasExited
        } -TimeoutSec ([Math]::Min(30, (Get-FcCaseRemainingSec))) -PollMs 250
        if (-not (Test-FcSessionAccepted -Lines @(Get-Content "$logDir\om4-server.out" -ErrorAction SilentlyContinue))) {
            Write-FcDiag -StageTag "OM-4/session-accepted" -Reason "ABORT-SIGNAL-MISSING" -TimeoutSec 30 `
                -Proc $srv4 -OutLog "$logDir\om4-server.out" -ErrLog "$logDir\om4-server.err"
            throw "OM-4: ABORT-SIGNAL-MISSING no [mp] conn_accept in om4-server.out"
        }

        # A5: the gate holds at the FIRST complete FileChunk (type=31, len>0). Bounded by
        # min(60, case remaining); a missing chunk fails closed (D-06 B).
        $abortWindow = [Math]::Min(60, (Get-FcCaseRemainingSec))
        if (-not $gate.WaitForState([FcGate.GateState]::Held, ($abortWindow * 1000))) {
            Write-FcDiag -StageTag "OM-4/frame-gate-hold" -Reason "FRAME-GATE-NO-FILECHUNK" -TimeoutSec $abortWindow `
                -Proc $c4a -OutLog "$logDir\om4-c4a.out" -ErrLog "$logDir\om4-c4a.err"
            throw "OM-4: FRAME-GATE-NO-FILECHUNK gate did not reach Held within ${abortWindow}s (state=$($gate.State))"
        }
        $firstChunkLine = (Select-String -Path $gateLog -Pattern 'FRAME_GATE_FIRST_FILECHUNK' |
            Select-Object -First 1).Line
        $fcLen = -1; $fcCum = -1
        if ($firstChunkLine) {
            if ($firstChunkLine -match 'len=(\d+)') { $fcLen = [long]$Matches[1] }
            if ($firstChunkLine -match 'cum=(\d+)') { $fcCum = [long]$Matches[1] }
        }
        if ($fcLen -le 0 -or $fcCum -lt 0 -or $fcCum -ge 524288000) {
            Write-FcDiag -StageTag "OM-4/frame-gate-hold" -Reason "FRAME-GATE-NO-FILECHUNK" -TimeoutSec $abortWindow `
                -Proc $c4a -OutLog "$logDir\om4-c4a.out" -ErrLog "$logDir\om4-c4a.err"
            throw "OM-4: FRAME-GATE-NO-FILECHUNK first chunk evidence bad: len=$fcLen cum=$fcCum (need len>0, cum<512MB)"
        }
        Write-Host ("  [OM-4] frame gate HELD at first FileChunk len={0}B cum={1}B -- c4a provably mid-transfer" -f $fcLen, $fcCum)

        # A2 (second half): re-check == 1 AFTER the hold. The gate polls listener.Pending()
        # every 50ms during the hold, so a second (bypassing) lane would be counted by now.
        Start-Sleep -Milliseconds 300
        if ($gate.AcceptedConnections -ne 1) {
            Write-FcDiag -StageTag "OM-4/gate-accept-recheck" -Reason "PROXY-BYPASS-MULTI-CONN" -TimeoutSec 10 `
                -Proc $c4a -OutLog "$logDir\om4-c4a.out" -ErrLog "$logDir\om4-c4a.err"
            throw "OM-4: PROXY-BYPASS-MULTI-CONN gate accepted $($gate.AcceptedConnections) connections after hold (aux lane bypass?)"
        }

        # A6: at the hold point the client must STILL be alive (the gate stopped forwarding,
        # so a fast machine physically cannot have finished -- the 07 B-01 gap is closed).
        $c4a.Refresh()
        if ($c4a.HasExited) {
            Write-FcDiag -StageTag "OM-4/hold-alive-check" -Reason "ABORT-CLIENT-FINISHED-EARLY" -TimeoutSec 0 `
                -Proc $c4a -OutLog "$logDir\om4-c4a.out" -ErrLog "$logDir\om4-c4a.err"
            throw "OM-4: ABORT-CLIENT-FINISHED-EARLY c4a pid=$($c4a.Id) already exited at the hold (exit=$($c4a.ExitCode))"
        }

        # A3: the client itself confirms zero aux lanes: [phase] tag=aux_end n=0 (EmitPhase
        # writes stderr unconditionally, sync_engine_client.cpp L66-79/L803-804). A missing
        # line (old binary) or n>0 (bypass) is an explicit failure, never a silent pass.
        $script:Om4CliErrOffset = 0
        $null = Wait-FcCondition -Condition {
            $new = Get-FcLogNewLines -Path "$logDir\om4-c4a.err" -Offset ([ref]$script:Om4CliErrOffset)
            $hit = $false
            foreach ($l in @($new)) { if ($l -match '\[phase\] tag=aux_end n=') { $hit = $true } }
            $hit -or $c4a.HasExited
        } -TimeoutSec ([Math]::Min(10, (Get-FcCaseRemainingSec))) -PollMs 250
        $auxN = -1
        $auxLine = (Select-String -Path "$logDir\om4-c4a.err" -Pattern '\[phase\] tag=aux_end n=(\d+)' |
            Select-Object -First 1).Line
        if ($auxLine -and $auxLine -match 'n=(\d+)') { $auxN = [int]$Matches[1] }
        if ($auxN -ne 0) {
            Write-FcDiag -StageTag "OM-4/aux-lane-guard" -Reason "PROXY-BYPASS-AUX-LANE" -TimeoutSec 10 `
                -Proc $c4a -OutLog "$logDir\om4-c4a.out" -ErrLog "$logDir\om4-c4a.err"
            throw "OM-4: PROXY-BYPASS-AUX-LANE aux_end n=$auxN (expected 0; missing line is also a failure)"
        }

        # A7: first-abort the owned c4a with the triple-guard stop (Id in registry +
        # ProcessName FastClone + identical StartTime), then require a NON-ZERO exit within
        # 15s. [D-05 A] the TEST performs the kill; exit 0 means the kill was ineffective.
        Write-Host "  [OM-4] stopping owned c4a pid=$($c4a.Id) (Id+StartTime+FastClone triple guard)"
        $a7StopIssued = Stop-FcOwnedProcess -Proc $c4a -StageTag "OM-4/abort"
        if (-not $a7StopIssued) {
            # [16 B-05] The guard reports the process is ALREADY GONE: no kill was issued,
            # so the TEST-abort chain (A7) was never exercised -- judge that reality instead
            # of silently continuing into A8/A9 with a session that was not aborted by us.
            Write-FcDiag -StageTag "OM-4/abort" -Reason "ABORT-CLIENT-FINISHED-EARLY" -TimeoutSec 15 `
                -Proc $c4a -OutLog "$logDir\om4-c4a.out" -ErrLog "$logDir\om4-c4a.err"
            throw "OM-4: ABORT-CLIENT-FINISHED-EARLY c4a pid=$($c4a.Id) exited on its own before the guarded stop was issued (no kill happened)"
        }
        if (-not $c4a.WaitForExit(15000)) {
            Write-FcDiag -StageTag "OM-4/abort-exit" -Reason "ABORT-NOT-OBSERVED" -TimeoutSec 15 `
                -Proc $c4a -OutLog "$logDir\om4-c4a.out" -ErrLog "$logDir\om4-c4a.err"
            throw "OM-4: ABORT-NOT-OBSERVED c4a pid=$($c4a.Id) did not exit within 15s after stop"
        }
        $c4a.Refresh()
        if ($null -eq $c4a.ExitCode) { throw "OM-4: EXITCODE-UNAVAILABLE for c4a pid=$($c4a.Id)" }
        if ([int]$c4a.ExitCode -eq 0) {
            Write-FcDiag -StageTag "OM-4/abort-exit" -Reason "ABORT-NOT-OBSERVED" -TimeoutSec 15 `
                -Proc $c4a -OutLog "$logDir\om4-c4a.out" -ErrLog "$logDir\om4-c4a.err"
            throw "OM-4: ABORT-NOT-OBSERVED c4a exited 0 (transfer completed before the kill; abort ineffective)"
        }
        Write-Host "  [OM-4] c4a aborted exit=$([int]$c4a.ExitCode)"

        # A8: the gate observed the client close and RST'd the single upstream socket within
        # 15s (CLIENT_CLOSED + UPSTREAM_ABORTED). The kill alone proves nothing: the chain
        # A5+A6+A8+A9+A10 must hold together.
        if (-not $gate.WaitForState([FcGate.GateState]::Aborted, 15000)) {
            Write-FcDiag -StageTag "OM-4/gate-aborted" -Reason "ABORT-NOT-OBSERVED" -TimeoutSec 15 `
                -Proc $c4a -OutLog "$logDir\om4-c4a.out" -ErrLog "$logDir\om4-c4a.err"
            throw "OM-4: ABORT-NOT-OBSERVED gate did not reach Aborted within 15s (state=$($gate.State))"
        }
        $om4Snap = $gate.Snapshot()
        if (-not ($om4Snap.Contains('client_closed') -and $om4Snap.Contains('upstream_aborted'))) {
            Write-FcDiag -StageTag "OM-4/gate-aborted" -Reason "ABORT-NOT-OBSERVED" -TimeoutSec 15 `
                -Proc $c4a -OutLog "$logDir\om4-c4a.out" -ErrLog "$logDir\om4-c4a.err"
            throw "OM-4: ABORT-NOT-OBSERVED gate events missing client_closed/upstream_aborted ($om4Snap)"
        }

        # A9: the server observed the interruption within 15s. Source chain (11 section 3,
        # C1 -- quoted, not assumed): hold + stop reading the downlink => client TCP window
        # closes; the server cannot push 512MB past kWinDefaultSndBuf=16MB, so SyncDone/
        # done/conn_done cannot happen and RunSessionServer has no early return; the gate's
        # RST makes WSASend/recv fail => [mp] conn_error => sticky exit 5.
        # [16 B-01] Per-poll evidence: every 250ms poll records elapsed / file len / mtime /
        # offset / new-complete-lines / hit, so a miss is ATTRIBUTABLE instead of guessed:
        #   (a) the pattern exists but the tail line has no newline yet (never consumable),
        #   (b) the [ref] offset fails to write through the Wait-FcCondition invocation
        #       (covered in isolation by E10/E11), or
        #   (c) the server only wrote the line after the deadline (real delay).
        # The 15s window itself is UNCHANGED (12 section 4 A9) -- no extension, no early
        # c4b start, no retry; if real evidence ever demands a different window that is a
        # contract change for @designer, not a number to tune here.
        $script:Om4SrvErrOffset = 0
        $script:Om4A9Trace = New-Object System.Collections.Generic.List[string]
        $script:Om4A9Start = [DateTime]::UtcNow
        $script:Om4A9HitLine = $null
        $script:Om4A9HitMs = -1
        $a9 = Wait-FcCondition -Condition {
            $a9Len = -1; $a9Mtime = '<none>'
            try {
                $a9Fi = Get-Item -LiteralPath "$logDir\om4-server.err" -ErrorAction SilentlyContinue
                if ($a9Fi) { $a9Len = $a9Fi.Length; $a9Mtime = $a9Fi.LastWriteTime.ToString('HH:mm:ss.fff') }
            } catch { }
            $new = Get-FcLogNewLines -Path "$logDir\om4-server.err" -Offset ([ref]$script:Om4SrvErrOffset)
            $hit = $false
            foreach ($l in @($new)) {
                if ($l -match '\[mp\] conn_error') { $hit = $true; $script:Om4A9HitLine = $l }
            }
            $a9Ms = [int][Math]::Floor(([DateTime]::UtcNow - $script:Om4A9Start).TotalMilliseconds)
            if ($hit -and $script:Om4A9HitMs -lt 0) { $script:Om4A9HitMs = $a9Ms }
            $script:Om4A9Trace.Add(("poll t=+{0}ms len={1} mtime={2} off={3} new={4} hit={5}" -f `
                $a9Ms, $a9Len, $a9Mtime, $script:Om4SrvErrOffset, @($new).Count, $hit))
            $hit
        } -TimeoutSec 15 -PollMs 250
        # Final full-text record (tail kept verbatim, even without a trailing newline):
        # distinguishes miss-cause (a) [pattern present, tail unterminated] from (c)
        # [pattern absent during the window / landed late] -- Get-FcLogFullText (16 B-01).
        $a9Full = Get-FcLogFullText -Path "$logDir\om4-server.err"
        $a9FullLen = -1; $a9FullHas = $false; $a9TailTerm = $false
        if ($null -ne $a9Full) {
            $a9FullLen = $a9Full.Length
            $a9FullHas = $a9Full.Contains('[mp] conn_error')
            $a9TailTerm = $a9Full.EndsWith("`n")
        }
        if (-not $a9.Met) {
            Write-FcDiag -StageTag "OM-4/server-conn-error" -Reason "ABORT-NOT-OBSERVED" -TimeoutSec 15 `
                -Proc $srv4 -OutLog "$logDir\om4-server.out" -ErrLog "$logDir\om4-server.err"
            Write-Host "  [OM-4] A9 per-poll trace (window=15s, polls=$($script:Om4A9Trace.Count)):"
            foreach ($a9t in $script:Om4A9Trace) { Write-Host "    $a9t" }
            Write-Host ("  [OM-4] A9 final full-text: len={0} contains_conn_error={1} tail_newline_terminated={2}" -f `
                $a9FullLen, $a9FullHas, $a9TailTerm)
            throw "OM-4: ABORT-NOT-OBSERVED server never logged [mp] conn_error within 15s of the abort"
        }
        Write-Host ("  [OM-4] A9 [mp] conn_error observed within the 15s window (hit=+{0}ms, elapsed={1}s, polls={2})" -f `
            $script:Om4A9HitMs, $a9.ElapsedSec, $script:Om4A9Trace.Count)
        Write-Host ("  [OM-4] A9 hit line: {0}" -f $script:Om4A9HitLine)

        # A10 (original assertions, unchanged semantics): a later CLEAN DIRECT session must
        # NOT clear the sticky failure (B5). c4b bypasses the gate entirely (direct
        # $serverAddr), exits 0, and the server ends sticky exit 5 with [om] terminal exit=5.
        # [16 S-01] c4b must still reach the server INSIDE its 3s idle grace (armed after the
        # aborted session ended): it starts immediately after the A9 hit, and the measured
        # gap below (hit -> c4b start) is the evidence; A10's exit-0 + sticky terminal-exit=5
        # assertions prove the direct connection really happened inside that window.
        if ($script:Om4A9HitMs -ge 0) {
            $a9ToC4bMs = [int][Math]::Floor(([DateTime]::UtcNow - $script:Om4A9Start).TotalMilliseconds) - $script:Om4A9HitMs
            Write-Host ("  [OM-4] c4b direct session starting {0}ms after the A9 hit (server idle grace=3s)" -f $a9ToC4bMs)
        }
        $tgt4b = Join-Path $root "tgt4b"; New-Item -ItemType Directory -Force -Path $tgt4b | Out-Null
        $c4b = Invoke-FastCloneSync -Exe $exe -CliArgs @(
            "client", "--server", $serverAddr, "--target", $tgt4b, "--password", $password
        ) -OutLog "$logDir\om4-c4b.out" -ErrLog "$logDir\om4-c4b.err" -TimeoutSec 210 -StageTag "OM-4"
        if ($c4b -ne 0) { throw "OM-4: clean second client expected exit 0, got $c4b" }
        $code4 = Wait-ExitCode -Proc $srv4 -TimeoutSec 30
        if ($code4 -ne 5) { throw "OM-4: server expected sticky exit 5, got $code4" }
        $termLine = (Select-String -Path "$logDir\om4-server.out" -Pattern '\[om\] terminal exit=5' |
            Select-Object -First 1).Line
        if (-not $termLine) {
            throw "OM-4: server log missing '[om] terminal exit=5' (process exit code was $code4)"
        }
        Write-Host "  OK aborted session forced sticky exit=$code4 (gate: $($gate.Snapshot()))"
    }
    catch {
        # A11: the ORIGINAL failure gets its diagnostics BEFORE any cleanup (never masked);
        # the gate snapshot/log tail is appended, then the exception propagates unchanged.
        $om4Failed = $true
        if (-not $script:FcDiagEmitted) {
            Write-FcDiag -StageTag "OM-4/unhandled" -Reason "unhandled-failure" -TimeoutSec 0 `
                -Proc $null -OutLog $null -ErrLog $null
        }
        Write-FcGateDiag -GateObj $gate -GateLog $gateLog
        throw
    }
    finally {
        # [12 section 3, C8] Dispose the gate: cancel -> Shutdown(Both) both sockets ->
        # Close sockets+listener (before the joins) -> aggregate 5000ms Join(remaining).
        # A dispose timeout is APPENDED (FRAME-GATE-CLOSE-TIMEOUT), never masking the
        # original failure; on the success path it IS the failure.
        if ($null -ne $gate) {
            try { $gate.Dispose() } catch {
                Write-Host "  [OM-4] FRAME-GATE-CLOSE-TIMEOUT: gate dispose threw: $_" -ForegroundColor Red
            }
            if ($gate.DisposeTimedOut) {
                $closeFail = "OM-4: FRAME-GATE-CLOSE-TIMEOUT gate worker threads did not join within $($gate.DisposeTimeoutMs)ms (see $gateLog)"
                Write-Host "FAILED: $closeFail" -ForegroundColor Red
                if (-not $om4Failed) { throw $closeFail }
            }
        }
    }
    }

    if ($Case -contains 'OM-5a') {
    Write-Host "[OM-5a] probes during grace do not reset it; server exits on schedule"
    Enter-FcCase -Name 'OM-5a'
    if (-not (Wait-PortFree -PortNum $Port -TimeoutSec 15)) {
        Write-Host "[PORT-BUSY] port $Port still held before OM-5a server start:"
        (netstat -ano | Select-String -Pattern ":$Port ") | ForEach-Object { Write-Host "    $($_.Line)" }
    }
    $srv5 = Start-FastCloneProcess -Exe $exe -CliArgs @(
        "server", "--dir", $src, "--password", $password, "--port", "$Port", $bindLoopbackArg,
        "--once-multi", "--once-idle-grace", "4s"
    ) -OutLog "$logDir\om5a-server.out" -ErrLog "$logDir\om5a-server.err"
    if (-not (Wait-PortListening -PortNum $Port -Proc $srv5 -TimeoutSec 15)) {
        Write-FcDiag -StageTag "OM-5a/server-not-listening" -Reason "server-bind" -TimeoutSec 15 `
            -Proc $srv5 -OutLog "$logDir\om5a-server.out" -ErrLog "$logDir\om5a-server.err"
        throw "OM-5a: server pid=$($srv5.Id) never reached LISTENING on port $Port within 15s"
    }
    $tgt5 = Join-Path $root "tgt5"; New-Item -ItemType Directory -Force -Path $tgt5 | Out-Null
    $c5 = Invoke-FastCloneSync -Exe $exe -CliArgs @(
        "client", "--server", $serverAddr, "--target", $tgt5, "--password", $password
    ) -OutLog "$logDir\om5a-c5.out" -ErrLog "$logDir\om5a-c5.err" `
        -TimeoutSec 150 -StageTag "OM-5a"
    if ($c5 -ne 0) { throw "OM-5a: real client expected exit 0, got $c5" }
    # Inject probes during the grace window; they must not re-arm/extend it. [C6] The finite
    # 800ms spacing carries the real probe timing and is clamped by the case budget.
    Send-Probe -ProbePort $Port
    Start-Sleep -Milliseconds 800
    Send-Probe -ProbePort $Port
    $code5 = Wait-ExitCode -Proc $srv5 -TimeoutSec 30
    if ($code5 -ne 0) { throw "OM-5a: server expected exit 0 (probes ignored), got $code5" }
    $resetA = (Select-String -Path "$logDir\om5a-server.out" -Pattern "idle_grace_reset").Count
    if ($resetA -ne 0) { throw "OM-5a: probe wrongly reset the grace ($resetA reset events)" }
    Write-Host "  OK probes did not reset grace; server exit=$code5"
    }

    if ($Case -contains 'OM-5b') {
    Write-Host "[OM-5b] probe-only (no real session): server never idle-exits"
    Enter-FcCase -Name 'OM-5b'
    if (-not (Wait-PortFree -PortNum $Port -TimeoutSec 15)) {
        Write-Host "[PORT-BUSY] port $Port still held before OM-5b server start:"
        (netstat -ano | Select-String -Pattern ":$Port ") | ForEach-Object { Write-Host "    $($_.Line)" }
    }
    $srv6 = Start-FastCloneProcess -Exe $exe -CliArgs @(
        "server", "--dir", $src, "--password", $password, "--port", "$Port", $bindLoopbackArg,
        "--once-multi", "--once-idle-grace", "2s"
    ) -OutLog "$logDir\om5b-server.out" -ErrLog "$logDir\om5b-server.err"
    if (-not (Wait-PortListening -PortNum $Port -Proc $srv6 -TimeoutSec 15)) {
        Write-FcDiag -StageTag "OM-5b/server-not-listening" -Reason "server-bind" -TimeoutSec 15 `
            -Proc $srv6 -OutLog "$logDir\om5b-server.out" -ErrLog "$logDir\om5b-server.err"
        throw "OM-5b: server pid=$($srv6.Id) never reached LISTENING on port $Port within 15s"
    }
    Send-Probe -ProbePort $Port
    Start-Sleep -Milliseconds 500
    Send-Probe -ProbePort $Port
    # Wait well beyond the grace; with no real session ever served the server must stay up.
    # [C6] Finite alive window (6s) carrying the real grace timing, clamped by case budget.
    Start-Sleep -Seconds 6
    if ($srv6.HasExited) { throw "OM-5b: server idle-exited without ever serving a real session" }
    Stop-Process -Id $srv6.Id -Force -ErrorAction SilentlyContinue
    Write-Host "  OK probe-only server stayed alive (killed for cleanup)"
    }

    # [3.8/C5] Report exactly what ran: a subset run (e.g. -Case OM-4) is a local repeat and
    # is NEVER a full-suite pass claim.
    $ranCases = @($Case)
    Write-Host ("Selected case(s) [{0}] passed ({1}/6 scenarios). Logs: {2}" -f `
        ($ranCases -join ', '), $ranCases.Count, $logDir)
    Stop-AllFastClone
    exit 0
}
catch {
    Write-Host "FAILED: $_" -ForegroundColor Red
    Write-Host "Logs: $logDir"
    # [3.5] If this case has not produced a diag block yet, emit one BEFORE cleanup so every
    # failure is attributable (case/budget/elapsed/laststate/PID) even for throws that did
    # not come from a wait helper.
    if (-not $script:FcDiagEmitted) {
        Write-FcDiag -StageTag "catch/unhandled" -Reason "unhandled-failure" -TimeoutSec 0 `
            -Proc $null -OutLog $null -ErrLog $null
    }
    $failures = 1
}
finally {
    Stop-AllFastClone
    # [19 compliance / 18 S-01] RETENTION ONLY -- the user's standing rules forbid recursive
    # force-delete execution statements, and an ownership guard is NOT a literal exemption
    # from that rule. This run's unique temp tree ($env:TEMP\fastclone-onemulti-it-<digits>,
    # created fresh with the per-run .fc-it-owner token [16 S-06]) is therefore KEPT on BOTH
    # the success and the failure path. The original guard validation (Test-FcSafeTempRoot
    # structure check + Test-FcOwnTempMarker token check) is still evaluated and reported,
    # but it gates nothing destructive anymore -- no deletion call of any kind (no recursive
    # force-delete cmdlet, no substitute API) exists in this script. Inspect the tree at the
    # retained path below; per-case logs live inside it ("Logs: $logDir" above).
    $retOwnerOk = (Test-FcSafeTempRoot -Path $root) -and `
        (Test-FcOwnTempMarker -Path $root -Token $script:FcRunOwnerToken)
    Write-Host ("RETAINED run temp root (no deletion by design; owner structure+token guard=" + $retOwnerOk + "): " + $root)
    if ($failures -ne 0) { exit 1 }
}
