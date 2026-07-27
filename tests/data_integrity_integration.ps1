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

# Build a SHA256 hashtable {relativePath -> hash} for all files under $Root.
function Get-FileHashTree {
    param([string]$Root)
    $hashes = @{}
    Get-ChildItem -Path $Root -Recurse -File | ForEach-Object {
        $rel = $_.FullName.Substring($Root.Length).TrimStart('\', '/')
        $hashes[$rel] = (Get-FileHash -Path $_.FullName -Algorithm SHA256).Hash
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

    $srvCode = Wait-ExitCode -Proc $srv -TimeoutSec 60
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
    Get-ChildItem -Path $src -Recurse -File | ForEach-Object {
        $rel = $_.FullName.Substring($src.Length).TrimStart('\', '/')
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
    Stop-AllFastClone
    exit 1
}
