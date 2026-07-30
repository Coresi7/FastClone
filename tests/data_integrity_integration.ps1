# FastClone data integrity integration test (Windows).
#
# Verifies that bytes transferred by the server are byte-for-byte identical to
# the source (SHA256) after a full sync. This is the automated regression guard
# for the C2 "传输字节逐字节不变" guarantee that A1 (a1-sendloop-buffer-reuse)
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
    [int]$Port = 27894
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
    param([System.Diagnostics.Process]$Proc, [int]$TimeoutSec = 60)
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
$longPathUtil = Add-Type -MemberDefinition $longPathSig -Name 'LongPathUtil' -Namespace 'FcDiag' -PassThru
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

    # Snapshot source hashes before transfer.
    $srcHashes = Get-FileHashTree -Root $Src

    # Fresh target directory.
    $tgt = Join-Path $env:TEMP "fc-di-tgt-$(Get-Random)"
    New-Item -ItemType Directory -Force -Path $tgt | Out-Null

    # Start server (--once so it auto-exits after the single session).
    $srv = Start-FastCloneProcess -Exe $Exe -CliArgs @(
        "server", "--dir", $Src, "--password", $Password, "--port", "$PortNum", "--once"
    ) -OutLog "$LogDir\$Label-server.out" -ErrLog "$LogDir\$Label-server.err"
    Start-Sleep -Seconds 1

    # Run client sync.
    $clientArgs = @(
        "client", "--server", $ServerAddr, "--target", $tgt, "--password", $Password
    ) + $ExtraClientArgs
    $code = Invoke-FastCloneSync -Exe $Exe -CliArgs $clientArgs `
        -OutLog "$LogDir\$Label-client.out" -ErrLog "$LogDir\$Label-client.err"
    if ($code -ne 0) { throw "${Label}: client sync expected exit 0, got $code" }

    # TEMP DIAG: snapshot target large-file presence IMMEDIATELY after the client
    # process exits (before the server wait / compare scan). The client's
    # postclose already reported exists=1 size=5242880 for every large file, so all
    # three should be present here. If a large file is present NOW but gone at
    # COMPARE TIME (a few seconds later), it was removed by an EXTERNAL process
    # (real-time AV quarantine) in the window between client close and the scan --
    # NOT by FastClone (the client writes directly to the final path and never
    # renames/deletes). This isolates the genuine issue from the harness rel-key bug.
    $postClientLarge = Get-ChildItem -Path $tgt -Recurse -File -ErrorAction SilentlyContinue `
        | Where-Object { $_.Name -like 'large_5m_*' } `
        | ForEach-Object { $_.FullName }
    Write-Host "[$Label] large_5m_* present POST-CLIENT: $($postClientLarge -join ', ')"

    $srvCode = Wait-ExitCode -Proc $srv -TimeoutSec 60
    if ($srvCode -ne 0) { throw "${Label}: server --once expected exit 0, got $srvCode" }

    # DI-1: compare source/target SHA256 trees byte-for-byte.
    $tgtHashes = Get-FileHashTree -Root $tgt
    # TEMP DIAG: enumerate what the target actually contained AT COMPARE TIME, and
    # explicitly probe each missing source key. This confirms whether any file is
    # genuinely absent from the target (a real product issue) versus the previous
    # harness-only rel-key mismatch (caused by a DOS 8.3 short $env:TEMP path on
    # the CI machine producing wrong source relative keys).
    $largeFiles = Get-ChildItem -Path $tgt -Recurse -File -ErrorAction SilentlyContinue `
        | Where-Object { $_.Name -like 'large_5m_*' } `
        | ForEach-Object { $_.FullName }
    Write-Host "[$Label] target file count at compare: $($tgtHashes.Count) (expected $($srcHashes.Count))"
    Write-Host "[$Label] large_5m_* present in target at compare: $($largeFiles -join ', ')"
    foreach ($rel in $srcHashes.Keys) {
        if (-not $tgtHashes.ContainsKey($rel)) {
            $guess = Join-Path $tgt $rel
            $tp = Test-Path $guess
            Write-Host "[$Label] MISSING-PROBE rel=$rel Test-Path=$(if ($tp) { 1 } else { 0 }) abs=$guess"
        }
    }
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

# TEMP DIAG: dump the real path layout so we can see where the "N\src" prefix
# in the source rel keys comes from on the CI machine.
Write-Host "[DIAG-PATH] env:TEMP=$env:TEMP"
Write-Host "[DIAG-PATH] root=$root"
Write-Host "[DIAG-PATH] rootResolved=$(try { (Resolve-Path $root).Path } catch { '?' })"
Write-Host "[DIAG-PATH] src=$src"
Write-Host "[DIAG-PATH] srcResolved=$(try { (Resolve-Path $src).Path } catch { '?' })"

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
    # TEMP DIAG: dump [DIAG-BATCH]/[DIAG-STREAM] probe lines from the server/client .err logs
    # into stdout so ctest --output-on-failure (and the transcript) captures them.
    if (Test-Path $logDir) {
        Get-ChildItem -Path $logDir -File | ForEach-Object {
            $lines = Get-Content -Path $_.FullName -ErrorAction SilentlyContinue | Select-String -Pattern 'DIAG-BATCH|DIAG-STREAM'
            if ($lines) {
                Write-Host "----- $($_.Name) (DIAG) -----"
                $lines | ForEach-Object { Write-Host $_.Line }
            }
        }
    }
    # TEMP DIAG (AV hypothesis): if a large file is present POST-CLIENT but missing
    # at compare time, an external process deleted it. Best-effort: ask Windows
    # Defender for any current threat detections whose resources reference our temp
    # target dir -- a direct confirmation of quarantine.
    try {
        $dt = Get-MpThreatDetection -ErrorAction SilentlyContinue
        if ($dt) {
            $dt | ForEach-Object {
                Write-Host "[DIAG-AV] ThreatName=$($_.ThreatName) Resources=$($_.Resources -join ';')"
            }
        }
    } catch { }
    try {
        $thr = Get-MpThreat -ErrorAction SilentlyContinue
        if ($thr) {
            $thr | ForEach-Object {
                Write-Host "[DIAG-AV] Threat=$($_.ThreatName) State=$($_.ThreatStatus)"
            }
        }
    } catch { }
    Stop-AllFastClone
    exit 1
}
