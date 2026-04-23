# Log benchmark — rebuild + run + summarize
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File .\bench\rebuild_and_bench_log.ps1
# Parameters:
#   -Runs  N    (default 5) — number of full benchmark runs to average over
#   -SkipBuild  — skip cmake --build (use existing binaries)
#   -BuildDir   — override build directory (default: build\windows-vs2022-release-shared)
param(
    [int]$Runs = 5,
    [switch]$SkipBuild,
    [string]$BuildDir
)

$ErrorActionPreference = 'Stop'

# ── paths ────────────────────────────────────────────────────────────────────
$repoRoot  = Split-Path -Parent $PSScriptRoot
$buildDir  = if ($BuildDir) { $BuildDir } else { "$repoRoot\build\windows-vs2022-release-shared" }
$runtimeDir = "$buildDir\bench\Release"
$exePath   = "$runtimeDir\log_bench.exe"
$outMd     = "$repoRoot\log_bench_latest.md"

# log files written by log_bench (opened in append mode — must delete before each run)
$logFiles = @(
    'C:\var\log_bench_info.log',
    'C:\var\log_bench_warn.log',
    'C:\var\log_bench_error.log',
    'C:\var\log_bench_format.log',
    'C:\var\log_bench_verbose.log',
    'C:\var\log_bench_info_literal.log',
    'C:\var\log_bench_verbose_literal.log'
)

# ── build ────────────────────────────────────────────────────────────────────
Get-Process -ErrorAction SilentlyContinue |
    Where-Object { $_.ProcessName -in @('log_bench', 'nei_tests') } |
    Stop-Process -Force

if (-not $SkipBuild) {
    Write-Host '==> Building...'
    & cmake --build $buildDir --config Release --target nei neixx log_bench
    Write-Host ''
}

# sync DLLs so the exe can find them
Copy-Item -Force "$buildDir\modules\neixx\Release\neixx.dll" "$runtimeDir\neixx.dll"
Copy-Item -Force "$buildDir\modules\nei\Release\nei.dll"     "$runtimeDir\nei.dll"

if (-not (Test-Path $exePath)) { throw "exe not found: $exePath" }

# ── benchmark function ────────────────────────────────────────────────────────
function Invoke-LogBenchmark {
    param(
        [string]$ExecutablePath,
        [string]$MarkdownPath,
        [string[]]$LogFilePaths,
        [int]$TotalRuns
    )

    $raw = @()
    $namePattern = '^(?<name>.+?)(?: \(File: .+\))?:$'

    for ($r = 1; $r -le $TotalRuns; $r++) {
        # ── clean up log files before every run so file-size measurements are exact ──
        foreach ($f in $LogFilePaths) {
            if (Test-Path $f) {
                Remove-Item -Force $f
                Write-Host ("  Deleted: $f")
            }
        }

        Write-Host ("  Running log_bench (run {0}/{1})" -f $r, $TotalRuns)

        $current = $null
        foreach ($line in (& $ExecutablePath)) {
            if ($line -match $namePattern) {
                $current = [ordered]@{
                    run          = $r
                    benchmark    = $Matches['name']
                    iterations   = $null
                    total_us     = $null
                    avg_us       = $null
                    logs_per_sec = $null
                    file_size    = $null
                }
                continue
            }

            if ($null -eq $current) { continue }

            if ($line -match '^\s+Iterations:\s+(?<v>\d+)$') {
                $current.iterations = [int]$Matches['v']; continue
            }
            if ($line -match '^\s+Total time:\s+(?<v>[0-9.]+) microseconds$') {
                $current.total_us = [double]$Matches['v']; continue
            }
            if ($line -match '^\s+Average time per log:\s+(?<v>[0-9.]+) microseconds$') {
                $current.avg_us = [double]$Matches['v']; continue
            }
            if ($line -match '^\s+Logs per second:\s+(?<v>[0-9.eE+\-]+)$') {
                $current.logs_per_sec = [double]$Matches['v']; continue
            }
            if ($line -match '^\s+File size:\s+(?<v>-?[0-9]+) bytes$') {
                $current.file_size = [long]$Matches['v']
                $raw += [pscustomobject]$current
                $current = $null
                continue
            }
            # in-memory benchmark ends with a blank line (no File size line)
            if ([string]::IsNullOrWhiteSpace($line) -and $null -ne $current -and $null -ne $current.logs_per_sec) {
                $raw += [pscustomobject]$current
                $current = $null
            }
        }
        # flush any last in-memory entry not terminated by blank line
        if ($null -ne $current -and $null -ne $current.logs_per_sec) {
            $raw += [pscustomobject]$current
        }
    }

    # ── aggregate ─────────────────────────────────────────────────────────────
    $ordered = @(
        'Log Info', 'Log Warn', 'Log Error', 'Log with Formatting',
        'Log Info (literal)', 'Log Verbose', 'Log Verbose (literal)',
        'File Log Info', 'File Log Warn', 'File Log Error', 'File Log with Formatting',
        'File Log Verbose', 'File Log Info (literal)', 'File Log Verbose (literal)'
    )

    $summary = @()
    foreach ($name in $ordered) {
        $items = @($raw | Where-Object { $_.benchmark -eq $name })
        if ($items.Count -eq 0) { continue }

        $avgMean   = ($items.avg_us       | Measure-Object -Average).Average
        $lpsMean   = ($items.logs_per_sec | Measure-Object -Average).Average
        $totalMean = ($items.total_us     | Measure-Object -Average).Average
        $stddev    = if ($items.Count -gt 1) {
            [math]::Round([math]::Sqrt((($items.avg_us | ForEach-Object { ($_ - $avgMean) * ($_ - $avgMean) }) | Measure-Object -Average).Average), 4)
        } else { 0.0 }

        $avgFileSize = $null
        if ($null -ne $items[0].file_size) {
            $avgFileSize = [math]::Round((($items.file_size | Measure-Object -Average).Average), 0)
        }

        $summary += [pscustomobject]@{
            benchmark        = $name
            runs             = $items.Count
            iterations       = $items[0].iterations
            avg_total_us     = [math]::Round($totalMean, 2)
            avg_us           = [math]::Round($avgMean,   4)
            stddev_us        = $stddev
            avg_logs_per_sec = [math]::Round($lpsMean,   0)
            avg_file_size    = $avgFileSize
            mode             = if ($name -like 'File *') { 'file' } else { 'memory' }
        }
    }

    # ── console output ────────────────────────────────────────────────────────
    Write-Host ''
    Write-Host ('Log Benchmark  —  {0}  (Runs: {1})' -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'), $TotalRuns)
    Write-Host ''
    Write-Host ('{0,-28}  {1,4}  {2,12}  {3,10}  {4,13}  {5,12}' -f `
        'Benchmark', 'Runs', 'Avg us/log', 'Stddev us', 'Logs/sec', 'File bytes')
    Write-Host ('{0,-28}  {1,4}  {2,12}  {3,10}  {4,13}  {5,12}' -f `
        ('-' * 28), ('-' * 4), ('-' * 12), ('-' * 10), ('-' * 13), ('-' * 12))

    foreach ($row in $summary) {
        $fs = if ($null -ne $row.avg_file_size) { '{0:N0}' -f $row.avg_file_size } else { '-' }
        Write-Host ('{0,-28}  {1,4}  {2,12:N4}  {3,10:N4}  {4,13:N0}  {5,12}' -f `
            $row.benchmark, $row.runs, $row.avg_us, $row.stddev_us, $row.avg_logs_per_sec, $fs)
    }
    Write-Host ''

    # ── markdown output ───────────────────────────────────────────────────────
    $md = [System.Text.StringBuilder]::new()
    $null = $md.AppendLine('# Log Benchmark')
    $null = $md.AppendLine('')
    $null = $md.AppendLine('Generated: ' + (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'))
    $null = $md.AppendLine('Runs: ' + $TotalRuns)
    $null = $md.AppendLine('Build: Release')
    $null = $md.AppendLine('')

    foreach ($mode in @('memory', 'file')) {
        $title = if ($mode -eq 'memory') { '## In-memory sink' } else { '## File sink' }
        $null = $md.AppendLine($title)
        $null = $md.AppendLine('')
        $null = $md.AppendLine('| Benchmark | Runs | Iterations | Avg total us | Avg us/log | Stddev us/log | Avg logs/sec | Avg file bytes |')
        $null = $md.AppendLine('|---|---:|---:|---:|---:|---:|---:|---:|')

        foreach ($row in ($summary | Where-Object { $_.mode -eq $mode })) {
            $fs = if ($null -ne $row.avg_file_size) { '{0:0}' -f $row.avg_file_size } else { '-' }
            $null = $md.AppendLine(('| {0} | {1} | {2} | {3:0.00} | {4:0.0000} | {5:0.0000} | {6:0} | {7} |' -f `
                $row.benchmark, $row.runs, $row.iterations, $row.avg_total_us, `
                $row.avg_us, $row.stddev_us, $row.avg_logs_per_sec, $fs))
        }
        $null = $md.AppendLine('')
    }

    Set-Content -Path $MarkdownPath -Value $md.ToString() -Encoding UTF8
    Write-Host "Saved: $MarkdownPath"
}

# ── entry point ───────────────────────────────────────────────────────────────
Invoke-LogBenchmark -ExecutablePath $exePath -MarkdownPath $outMd -LogFilePaths $logFiles -TotalRuns $Runs
