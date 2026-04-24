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

# sync runtime DLLs when present; static builds do not produce them
foreach ($dll in @(
    @{ Source = "$buildDir\modules\neixx\Release\neixx.dll"; Target = "$runtimeDir\neixx.dll" },
    @{ Source = "$buildDir\modules\nei\Release\nei.dll";     Target = "$runtimeDir\nei.dll" }
)) {
    if (Test-Path $dll.Source) {
        Copy-Item -Force $dll.Source $dll.Target
    }
}

if (-not (Test-Path $exePath)) { throw "exe not found: $exePath" }

function Add-MarkdownHighlights {
    param(
        [System.Text.StringBuilder]$Markdown,
        [object[]]$Summary
    )

    if ($Summary.Count -eq 0) { return }

    $best = $Summary | Sort-Object avg_e2e_us | Select-Object -First 1
    $worst = $Summary | Sort-Object avg_e2e_us -Descending | Select-Object -First 1

    $null = $Markdown.AppendLine('## Highlights')
    $null = $Markdown.AppendLine('')
    $null = $Markdown.AppendLine('| Type | Benchmark | Mode | Avg e2e us/log | Avg e2e logs/sec |')
    $null = $Markdown.AppendLine('|---|---|---|---:|---:|')
    $null = $Markdown.AppendLine(('| Best | {0} | {1} | {2:0.0000} | {3:0} |' -f `
        $best.benchmark, $best.mode, $best.avg_e2e_us, $best.avg_e2e_logs_sec))
    $null = $Markdown.AppendLine(('| Worst | {0} | {1} | {2:0.0000} | {3:0} |' -f `
        $worst.benchmark, $worst.mode, $worst.avg_e2e_us, $worst.avg_e2e_logs_sec))
    $null = $Markdown.AppendLine('')
}

function Add-MarkdownMemoryFileRatios {
    param(
        [System.Text.StringBuilder]$Markdown,
        [object[]]$Summary
    )

    $pairs = @()
    foreach ($row in ($Summary | Where-Object { $_.mode -eq 'memory' })) {
        if ($row.benchmark -notmatch '^Log (?<name>.+)$') { continue }
        $scenario = $Matches['name']
        $fileName = 'File Log ' + $scenario
        $fileRow = $Summary | Where-Object { $_.benchmark -eq $fileName } | Select-Object -First 1
        if ($null -eq $fileRow -or $row.avg_e2e_us -le 0) { continue }

        $pairs += [pscustomobject]@{
            scenario       = $scenario
            memory_e2e_us  = $row.avg_e2e_us
            file_e2e_us    = $fileRow.avg_e2e_us
            ratio          = $fileRow.avg_e2e_us / $row.avg_e2e_us
        }
    }

    if ($pairs.Count -eq 0) { return }

    $null = $Markdown.AppendLine('## Memory vs File Ratios')
    $null = $Markdown.AppendLine('')
    $null = $Markdown.AppendLine('| Scenario | Memory avg e2e us/log | File avg e2e us/log | File/Memory x |')
    $null = $Markdown.AppendLine('|---|---:|---:|---:|')
    foreach ($pair in $pairs) {
        $null = $Markdown.AppendLine(('| {0} | {1:0.0000} | {2:0.0000} | {3:0.00} |' -f `
            $pair.scenario, $pair.memory_e2e_us, $pair.file_e2e_us, $pair.ratio))
    }
    $null = $Markdown.AppendLine('')
}

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
    $runtimePattern = '^\s+Runtime stats:\s+producer_spins=(?<producer>\d+),\s+flush_wait_loops=(?<flush>\d+),\s+consumer_wakeups=(?<wakeups>\d+),\s+ring_hwm=(?<hwm>\d+)$'

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
                    run              = $r
                    benchmark        = $Matches['name']
                    iterations       = $null
                    enqueue_total_us = $null
                    enqueue_avg_us   = $null
                    flush_total_us   = $null
                    e2e_total_us     = $null
                    e2e_avg_us       = $null
                    enqueue_logs_sec = $null
                    e2e_logs_sec     = $null
                    producer_spins   = $null
                    flush_wait_loops = $null
                    consumer_wakeups = $null
                    ring_hwm         = $null
                    file_size        = $null
                }
                continue
            }

            if ($null -eq $current) { continue }

            if ($line -match '^\s+Iterations:\s+(?<v>\d+)$') {
                $current.iterations = [int]$Matches['v']; continue
            }
            if ($line -match '^\s+Enqueue total:\s+(?<v>[0-9.]+) microseconds$') {
                $current.enqueue_total_us = [double]$Matches['v']; continue
            }
            if ($line -match '^\s+Enqueue avg per log:\s+(?<v>[0-9.]+) microseconds$') {
                $current.enqueue_avg_us = [double]$Matches['v']; continue
            }
            if ($line -match '^\s+Flush total:\s+(?<v>[0-9.]+) microseconds$') {
                $current.flush_total_us = [double]$Matches['v']; continue
            }
            if ($line -match '^\s+E2E total:\s+(?<v>[0-9.]+) microseconds$') {
                $current.e2e_total_us = [double]$Matches['v']; continue
            }
            if ($line -match '^\s+E2E avg per log:\s+(?<v>[0-9.]+) microseconds$') {
                $current.e2e_avg_us = [double]$Matches['v']; continue
            }
            if ($line -match '^\s+Enqueue logs/sec:\s+(?<v>[0-9.eE+\-]+)$') {
                $current.enqueue_logs_sec = [double]$Matches['v']; continue
            }
            if ($line -match '^\s+E2E logs/sec:\s+(?<v>[0-9.eE+\-]+)$') {
                $current.e2e_logs_sec = [double]$Matches['v']; continue
            }
            if ($line -match '^\s+Total time:\s+(?<v>[0-9.]+) microseconds$') {
                $current.e2e_total_us = [double]$Matches['v']; continue
            }
            if ($line -match '^\s+Average time per log:\s+(?<v>[0-9.]+) microseconds$') {
                $current.e2e_avg_us = [double]$Matches['v']; continue
            }
            if ($line -match '^\s+Logs per second:\s+(?<v>[0-9.eE+\-]+)$') {
                $current.e2e_logs_sec = [double]$Matches['v']; continue
            }
            if ($line -match $runtimePattern) {
                $current.producer_spins = [uint64]$Matches['producer']
                $current.flush_wait_loops = [uint64]$Matches['flush']
                $current.consumer_wakeups = [uint64]$Matches['wakeups']
                $current.ring_hwm = [uint64]$Matches['hwm']
                continue
            }
            if ($line -match '^\s+File size:\s+(?<v>-?[0-9]+) bytes$') {
                $current.file_size = [long]$Matches['v']
                $raw += [pscustomobject]$current
                $current = $null
                continue
            }
            # in-memory benchmark ends with a blank line (no File size line)
            if ([string]::IsNullOrWhiteSpace($line) -and $null -ne $current -and $null -ne $current.e2e_logs_sec) {
                $raw += [pscustomobject]$current
                $current = $null
            }
        }
        # flush any last in-memory entry not terminated by blank line
        if ($null -ne $current -and $null -ne $current.e2e_logs_sec) {
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

        $enqueueAvgMean = ($items.enqueue_avg_us   | Measure-Object -Average).Average
        $e2eAvgMean     = ($items.e2e_avg_us       | Measure-Object -Average).Average
        $enqueueLpsMean = ($items.enqueue_logs_sec | Measure-Object -Average).Average
        $e2eLpsMean     = ($items.e2e_logs_sec     | Measure-Object -Average).Average
        $enqueueTotalMean = ($items.enqueue_total_us | Measure-Object -Average).Average
        $flushTotalMean   = ($items.flush_total_us   | Measure-Object -Average).Average
        $e2eTotalMean     = ($items.e2e_total_us     | Measure-Object -Average).Average
        $stddev    = if ($items.Count -gt 1) {
            [math]::Round([math]::Sqrt((($items.e2e_avg_us | ForEach-Object { ($_ - $e2eAvgMean) * ($_ - $e2eAvgMean) }) | Measure-Object -Average).Average), 4)
        } else { 0.0 }

        $avgFileSize = $null
        if ($null -ne $items[0].file_size) {
            $avgFileSize = [math]::Round((($items.file_size | Measure-Object -Average).Average), 0)
        }

        $producerSpinsMean = [math]::Round((($items.producer_spins | Measure-Object -Average).Average), 0)
        $flushWaitsMean    = [math]::Round((($items.flush_wait_loops | Measure-Object -Average).Average), 2)
        $wakeupsMean       = [math]::Round((($items.consumer_wakeups | Measure-Object -Average).Average), 2)
        $ringHwmMean       = [math]::Round((($items.ring_hwm | Measure-Object -Average).Average), 2)

        $summary += [pscustomobject]@{
            benchmark            = $name
            runs                 = $items.Count
            iterations           = $items[0].iterations
            avg_enqueue_total_us = [math]::Round($enqueueTotalMean, 2)
            avg_enqueue_us       = [math]::Round($enqueueAvgMean, 4)
            avg_flush_total_us   = if ($null -ne $flushTotalMean) { [math]::Round($flushTotalMean, 2) } else { $null }
            avg_e2e_total_us     = [math]::Round($e2eTotalMean, 2)
            avg_e2e_us           = [math]::Round($e2eAvgMean, 4)
            stddev_e2e_us        = $stddev
            avg_enqueue_logs_sec = [math]::Round($enqueueLpsMean, 0)
            avg_e2e_logs_sec     = [math]::Round($e2eLpsMean, 0)
            avg_file_size        = $avgFileSize
            avg_producer_spins   = $producerSpinsMean
            avg_flush_wait_loops = $flushWaitsMean
            avg_consumer_wakeups = $wakeupsMean
            avg_ring_hwm         = $ringHwmMean
            mode                 = if ($name -like 'File *') { 'file' } else { 'memory' }
        }
    }

    # ── console output ────────────────────────────────────────────────────────
    Write-Host ''
    Write-Host ('Log Benchmark  -  {0}  (Runs: {1})' -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'), $TotalRuns)
    Write-Host ''
    Write-Host ('{0,-28}  {1,4}  {2,12}  {3,12}  {4,10}  {5,12}  {6,12}' -f `
        'Benchmark', 'Runs', 'Enq us/log', 'E2E us/log', 'Stddev', 'E2E logs/s', 'File bytes')
    Write-Host ('{0,-28}  {1,4}  {2,12}  {3,12}  {4,10}  {5,12}  {6,12}' -f `
        ('-' * 28), ('-' * 4), ('-' * 12), ('-' * 12), ('-' * 10), ('-' * 12), ('-' * 12))

    foreach ($row in $summary) {
        $fs = if ($null -ne $row.avg_file_size) { '{0:N0}' -f $row.avg_file_size } else { '-' }
        Write-Host ('{0,-28}  {1,4}  {2,12:N4}  {3,12:N4}  {4,10:N4}  {5,12:N0}  {6,12}' -f `
            $row.benchmark, $row.runs, $row.avg_enqueue_us, $row.avg_e2e_us, $row.stddev_e2e_us, $row.avg_e2e_logs_sec, $fs)
    }
    Write-Host ''
    Write-Host 'Diagnostics (avg per benchmark)'
    Write-Host ''
    Write-Host ('{0,-28}  {1,12}  {2,12}  {3,12}  {4,10}' -f `
        'Benchmark', 'Prod spins', 'Flush waits', 'Wakeups', 'Ring HWM')
    Write-Host ('{0,-28}  {1,12}  {2,12}  {3,12}  {4,10}' -f `
        ('-' * 28), ('-' * 12), ('-' * 12), ('-' * 12), ('-' * 10))

    foreach ($row in $summary) {
        Write-Host ('{0,-28}  {1,12:N0}  {2,12:N2}  {3,12:N2}  {4,10:N2}' -f `
            $row.benchmark, $row.avg_producer_spins, $row.avg_flush_wait_loops, $row.avg_consumer_wakeups, $row.avg_ring_hwm)
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

    Add-MarkdownHighlights -Markdown $md -Summary $summary
    Add-MarkdownMemoryFileRatios -Markdown $md -Summary $summary

    foreach ($mode in @('memory', 'file')) {
        $title = if ($mode -eq 'memory') { '## In-memory sink' } else { '## File sink' }
        $null = $md.AppendLine($title)
        $null = $md.AppendLine('')
        $null = $md.AppendLine('| Benchmark | Runs | Iterations | Avg enqueue total us | Avg enqueue us/log | Avg flush total us | Avg e2e total us | Avg e2e us/log | Stddev e2e us/log | Avg e2e logs/sec | Avg file bytes |')
        $null = $md.AppendLine('|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|')

        foreach ($row in ($summary | Where-Object { $_.mode -eq $mode })) {
            $fs = if ($null -ne $row.avg_file_size) { '{0:0}' -f $row.avg_file_size } else { '-' }
            $flushTotal = if ($null -ne $row.avg_flush_total_us) { '{0:0.00}' -f $row.avg_flush_total_us } else { '-' }
            $null = $md.AppendLine(('| {0} | {1} | {2} | {3:0.00} | {4:0.0000} | {5} | {6:0.00} | {7:0.0000} | {8:0.0000} | {9:0} | {10} |' -f `
                $row.benchmark, $row.runs, $row.iterations, $row.avg_enqueue_total_us, `
                $row.avg_enqueue_us, $flushTotal, $row.avg_e2e_total_us, $row.avg_e2e_us, `
                $row.stddev_e2e_us, $row.avg_e2e_logs_sec, $fs))
        }
        $null = $md.AppendLine('')

        $null = $md.AppendLine('### Diagnostics')
        $null = $md.AppendLine('')
        $null = $md.AppendLine('| Benchmark | Avg producer spins | Avg flush wait loops | Avg consumer wakeups | Avg ring HWM |')
        $null = $md.AppendLine('|---|---:|---:|---:|---:|')

        foreach ($row in ($summary | Where-Object { $_.mode -eq $mode })) {
            $null = $md.AppendLine(('| {0} | {1:0} | {2:0.00} | {3:0.00} | {4:0.00} |' -f `
                $row.benchmark, $row.avg_producer_spins, $row.avg_flush_wait_loops, `
                $row.avg_consumer_wakeups, $row.avg_ring_hwm))
        }
        $null = $md.AppendLine('')
    }

    Set-Content -Path $MarkdownPath -Value $md.ToString() -Encoding UTF8
    Write-Host "Saved: $MarkdownPath"
}

# ── entry point ───────────────────────────────────────────────────────────────
Invoke-LogBenchmark -ExecutablePath $exePath -MarkdownPath $outMd -LogFilePaths $logFiles -TotalRuns $Runs
