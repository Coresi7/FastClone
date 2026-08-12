# FastClone large-file block (file-range) integration test (Windows)
# — T-largefile-block-multinic.
#
# Exercises the opt-in --large-file-block-kb mode (multi-NIC block fan-out) end to end:
#   FR-A  block mode ON, 2 loopback lanes: large files fan out as blocks across BOTH lanes,
#         H1 hash prefetch + whole-file XXH3 verify + atomic rename; SHA256 bit-exact.
#         Also covers V-10: one file is an exact block multiple, one has a short tail block.
#   FR-B  block mode OFF (default), 2 lanes: legacy single-stream large file, zero regression
#         (no file_range frames in the log).
#   FR-C  block mode ON but only 1 lane: gate falls back to the legacy path (AC-05).
#   FR-D  block mode ON, 2 lanes, lane 2 through a throttled TCP proxy; the proxy is killed
#         mid-transfer -> in-flight blocks reroute to the healthy lane and are re-fetched
#         whole (idempotent, AC-03); final bytes stay SHA256 bit-exact.
#
# Usage: .\tests\file_range_integration.ps1 [-ExePath path\to\FastClone.exe] [-Port 27910]

param(
    [string]$ExePath = "",
    [int]$Port = 27910
)

$ErrorActionPreference = "Stop"

function Resolve-FastCloneExe {
    param([string]$Hint)
    if ($Hint -and (Test-Path $Hint)) { return (Resolve-Path $Hint).Path }
    # Prefer the CMake build output (what build_and_test.ps1 builds/tests); the legacy VS
    # x64\Release artifact may be stale and lack the --large-file-block-kb flag.
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

function Stop-AllFastClone {
    Get-Process FastClone -ErrorAction SilentlyContinue | ForEach-Object {
        Stop-Process -Id $_.Id -Force -ErrorAction SilentlyContinue
    }
    Start-Sleep -Milliseconds 500
}

function Wait-ExitCode {
    param([System.Diagnostics.Process]$Proc, [int]$TimeoutSec = 120)
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
    $env:FASTCLONE_DEBUG = "1"
    $proc = Start-Process -FilePath $Exe -ArgumentList ([string[]]$CliArgs) `
        -RedirectStandardOutput $OutLog -RedirectStandardError $ErrLog `
        -PassThru -NoNewWindow
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
    $env:FASTCLONE_DEBUG = "1"
    $argLine = ($CliArgs | ForEach-Object {
        if ($_ -match '\s') { "`"$_`"" } else { $_ }
    }) -join " "
    cmd /c "`"$Exe`" $argLine 1>`"$OutLog`" 2>`"$ErrLog`""
    return [int]$LASTEXITCODE
}

$longPathSig = @'
[DllImport("kernel32.dll", CharSet=CharSet.Auto, SetLastError=true)]
public static extern int GetLongPathName(string lpszShortPath, System.Text.StringBuilder lpszLongPath, int cchBuffer);
'@
$longPathUtil = Add-Type -MemberDefinition $longPathSig -Name 'LongPathUtilFR' -Namespace 'FcDiag' -PassThru
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
$blockArgs = @("--large-file-threshold", "8M", "--large-file-block-kb", "8192")
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
    $tgt = Join-Path $env:TEMP "fc-fr-tgt-a-$(Get-Random)"
    New-Item -ItemType Directory -Force -Path $tgt | Out-Null
    $srv = Start-FastCloneProcess -Exe $exe -CliArgs @(
        "server", "--dir", $src, "--password", $password, "--port", "$Port", "--once"
    ) -OutLog "$logDir\A-server.out" -ErrLog "$logDir\A-server.err"
    Start-Sleep -Seconds 1
    # NOTE: build the client arg array FIRST — a compound "@(...) + $x" expression as a
    # cmdlet argument is silently truncated by PowerShell's argument-mode parser.
    $clientArgsA = @("client", "--server", $serverAddr, "--target", $tgt,
                     "--password", $password) + $twoLinks + $blockArgs
    $code = Invoke-FastCloneSync -Exe $exe -CliArgs $clientArgsA -OutLog "$logDir\A-client.out" -ErrLog "$logDir\A-client.err"
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

    # ============================ FR-B: block OFF, 2 lanes ===========================
    Write-Host ""
    Write-Host "[FR-B] block mode OFF (default), 2 loopback lanes"
    $tgt = Join-Path $env:TEMP "fc-fr-tgt-b-$(Get-Random)"
    New-Item -ItemType Directory -Force -Path $tgt | Out-Null
    $srv = Start-FastCloneProcess -Exe $exe -CliArgs @(
        "server", "--dir", $src, "--password", $password, "--port", "$Port", "--once"
    ) -OutLog "$logDir\B-server.out" -ErrLog "$logDir\B-server.err"
    Start-Sleep -Seconds 1
    $clientArgsB = @("client", "--server", $serverAddr, "--target", $tgt,
                     "--password", $password) + $twoLinks + @("--large-file-threshold", "8M")
    $code = Invoke-FastCloneSync -Exe $exe -CliArgs $clientArgsB -OutLog "$logDir\B-client.out" -ErrLog "$logDir\B-client.err"
    if ($code -ne 0) { throw "FR-B: client expected exit 0, got $code" }
    $srvCode = Wait-ExitCode -Proc $srv -TimeoutSec 120
    if ($srvCode -ne 0) { throw "FR-B: server --once expected exit 0, got $srvCode" }
    $diff = Compare-FileHashTrees -Src $srcHashes -Tgt (Get-FileHashTree -Root $tgt)
    if ($null -ne $diff) { throw "FR-B: DATA INTEGRITY FAILURE: $diff" }
    if (Select-String -Path "$logDir\B-client.out" -Pattern 'kind=file_range' -Quiet) {
        throw "FR-B: unexpected kind=file_range with the switch OFF (AC-07 regression)"
    }
    Write-Host "  OK FR-B: default path unchanged (no file_range frames), SHA256 match"
    Remove-Item -Recurse -Force $tgt -ErrorAction SilentlyContinue

    # ============================ FR-C: block ON, 1 lane =============================
    Write-Host ""
    Write-Host "[FR-C] block mode ON, single lane (AC-05 fallback)"
    $tgt = Join-Path $env:TEMP "fc-fr-tgt-c-$(Get-Random)"
    New-Item -ItemType Directory -Force -Path $tgt | Out-Null
    $srv = Start-FastCloneProcess -Exe $exe -CliArgs @(
        "server", "--dir", $src, "--password", $password, "--port", "$Port", "--once"
    ) -OutLog "$logDir\C-server.out" -ErrLog "$logDir\C-server.err"
    Start-Sleep -Seconds 1
    $clientArgsC = @("client", "--server", $serverAddr, "--target", $tgt,
                     "--password", $password) + $oneLink + $blockArgs
    $code = Invoke-FastCloneSync -Exe $exe -CliArgs $clientArgsC -OutLog "$logDir\C-client.out" -ErrLog "$logDir\C-client.err"
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

    # ================= FR-D: block ON, 2 lanes, kill lane mid-transfer ================
    Write-Host ""
    Write-Host "[FR-D] block mode ON, 2 lanes, kill lane 2 mid-transfer (AC-03 reroute)"
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
    $srv = Start-FastCloneProcess -Exe $exe -CliArgs @(
        "server", "--dir", $src, "--password", $password, "--port", "$Port"
    ) -OutLog "$logDir\D-server.out" -ErrLog "$logDir\D-server.err"
    Start-Sleep -Seconds 1
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
    # Block-level re-send proof: 2 + 24 blocks planned; more allocs than planned blocks means
    # killed in-flight blocks were re-issued on the surviving lane (not a whole-file restart).
    $allocCount = (Select-String -Path "$logDir\D-client.out" -Pattern 'kind=file_range').Count
    if ($allocCount -le 26) {
        throw "FR-D: expected >26 file_range allocs (26 planned + re-sent blocks), got $allocCount"
    }
    if (-not (Select-String -Path "$logDir\D-client.out" -Pattern '\[file_range\] done' -Quiet)) {
        throw "FR-D: missing '[file_range] done' after reroute"
    }
    if (Select-String -Path "$logDir\D-client.out" -Pattern 'file_range_finalize_failed' -Quiet) {
        throw "FR-D: unexpected finalize failure after reroute"
    }
    Write-Host "  OK FR-D: lane kill -> block reroute, all SHA256 match (allocs=$allocCount)"
    Remove-Item -Recurse -Force $tgt -ErrorAction SilentlyContinue

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
