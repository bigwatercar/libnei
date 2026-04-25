# Log compare benchmark - rebuild + run + summarize
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File .\bench\rebuild_and_bench_log_compare.ps1
param(
    [int]$Runs = 5,
    [switch]$SkipBuild,
    [string]$BuildDir
)

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$buildDir = if ($BuildDir) { $BuildDir } else { "$repoRoot\build\windows-vs2022-release-shared" }
$runtimeDir = "$buildDir\bench\Release"
$exePath = "$runtimeDir\log_bench_compare.exe"
$outMd = "$repoRoot\log_bench_compare_latest.md"

$logFiles = @(
    'C:\var\nei_cmp_simple.log',
    'C:\var\spdlog_cmp_simple.log',
    'C:\var\nei_cmp_multi.log',
    'C:\var\spdlog_cmp_multi.log',
    'C:\var\nei_cmp_literal.log',
    'C:\var\nei_cmp_vlog_literal.log',
    'C:\var\nei_cmp_simple_sync.log',
    'C:\var\spdlog_cmp_simple_sync.log',
    'C:\var\nei_cmp_multi_sync.log',
    'C:\var\spdlog_cmp_multi_sync.log',
    'C:\var\nei_cmp_literal_sync.log',
    'C:\var\nei_cmp_vlog_literal_sync.log',
    'C:\var\nei_cmp_simple_strict.log',
    'C:\var\spdlog_cmp_simple_strict.log',
    'C:\var\nei_cmp_multi_strict.log',
    'C:\var\spdlog_cmp_multi_strict.log'
)

Get-Process -ErrorAction SilentlyContinue |
    Where-Object { $_.ProcessName -in @('log_bench_compare', 'nei_tests') } |
    Stop-Process -Force

if (-not $SkipBuild) {
    Write-Host '==> Building...'
    & cmake --build $buildDir --config Release --target nei neixx log_bench_compare
    Write-Host ''
}

foreach ($dll in @(
    @{ Source = "$buildDir\modules\neixx\Release\neixx.dll"; Target = "$runtimeDir\neixx.dll" },
    @{ Source = "$buildDir\modules\nei\Release\nei.dll"; Target = "$runtimeDir\nei.dll" }
)) {
    if (Test-Path $dll.Source) {
        Copy-Item -Force $dll.Source $dll.Target
    }
}

if (-not (Test-Path $exePath)) { throw "exe not found: $exePath" }

function Get-CompareCanonicalName {
    param([string]$Benchmark)

    $name = $Benchmark -replace '^\[[^\]]+\]\s*', ''
    $name = $name -replace '^file\s+', ''
    $name = $name -replace '\s*\((flush request each log|async logger \+ flush request each log|sync flush each log)\)$', ''
    $name = $name.Trim()

    switch -Regex ($name) {
        '^simple\s' { return 'simple' }
        '^multi printf \(fmt_plan cache miss\)' { return 'multi (cache miss)' }
        '^multi\s' { return 'multi' }
        '^llog_literal(\s|$)' { return 'literal' }
        '^literal only$' { return 'literal' }
        '^vlog_literal(\s|$)' { return 'vlog_literal' }
        default { return $name }
    }
}

function Add-MarkdownHighlights {
    param(
        [System.Text.StringBuilder]$Markdown,
        [object[]]$Summary
    )

    if ($Summary.Count -eq 0) { return }

    $best = $Summary | Sort-Object avg_us | Select-Object -First 1
    $worst = $Summary | Sort-Object avg_us -Descending | Select-Object -First 1

    $null = $Markdown.AppendLine('## Highlights')
    $null = $Markdown.AppendLine('')
    $null = $Markdown.AppendLine('| Type | Benchmark | Section | Avg us/log | Avg logs/sec |')
    $null = $Markdown.AppendLine('|---|---|---|---:|---:|')
    $null = $Markdown.AppendLine(('| Best | {0} | {1} | {2:0.0000} | {3:0} |' -f `
        $best.benchmark, $best.section, $best.avg_us, $best.avg_logs_sec))
    $null = $Markdown.AppendLine(('| Worst | {0} | {1} | {2:0.0000} | {3:0} |' -f `
        $worst.benchmark, $worst.section, $worst.avg_us, $worst.avg_logs_sec))
    $null = $Markdown.AppendLine('')
}

function Add-MarkdownMemoryFileRatios {
    param(
        [System.Text.StringBuilder]$Markdown,
        [object[]]$Summary
    )

    $pairs = @()
    foreach ($row in ($Summary | Where-Object { $_.section -eq 'Memory (async, minimal sink)' })) {
        $fileRow = $Summary | Where-Object {
            $_.section -eq 'File (async file sink)' -and
            $_.library -eq $row.library -and
            $_.canonical -eq $row.canonical
        } | Select-Object -First 1

        if ($null -eq $fileRow -or $row.avg_us -le 0) { continue }

        $pairs += [pscustomobject]@{
            library    = $row.library
            scenario   = $row.canonical
            memory_us  = $row.avg_us
            file_us    = $fileRow.avg_us
            ratio      = $fileRow.avg_us / $row.avg_us
        }
    }

    if ($pairs.Count -eq 0) { return }

    $null = $Markdown.AppendLine('## Memory vs File Ratios')
    $null = $Markdown.AppendLine('')
    $null = $Markdown.AppendLine('| Library | Scenario | Memory avg us/log | File avg us/log | File/Memory x |')
    $null = $Markdown.AppendLine('|---|---|---:|---:|---:|')
    foreach ($pair in $pairs) {
        $null = $Markdown.AppendLine(('| {0} | {1} | {2:0.0000} | {3:0.0000} | {4:0.00} |' -f `
            $pair.library, $pair.scenario, $pair.memory_us, $pair.file_us, $pair.ratio))
    }
    $null = $Markdown.AppendLine('')
}

function Invoke-LogCompareBenchmark {
    param(
        [string]$ExecutablePath,
        [string]$MarkdownPath,
        [string[]]$LogFilePaths,
        [int]$TotalRuns
    )

    $raw = @()
    $section = $null
    $namePattern = '^(?<name>\[[^\]]+\].+)$'
    $sectionPattern = '^---\s+(?<name>.+?)\s+---$'
    $runtimePattern = '^\s+Runtime stats:\s+producer_spins=(?<producer>\d+),\s+flush_wait_loops=(?<flush>\d+),\s+consumer_wakeups=(?<wakeups>\d+),\s+ring_hwm=(?<hwm>\d+)$'

    for ($r = 1; $r -le $TotalRuns; $r++) {
        foreach ($f in $LogFilePaths) {
            if (Test-Path $f) {
                Remove-Item -Force $f
                Write-Host ("  Deleted: $f")
            }
        }

        Write-Host ("  Running log_bench_compare (run {0}/{1})" -f $r, $TotalRuns)

        $current = $null
        $section = $null
        foreach ($line in (& $ExecutablePath)) {
            if ($line -match $sectionPattern) {
                $section = $Matches['name']
                continue
            }

            if ($line -match $namePattern) {
                $benchmark = $Matches['name']
                $library = if ($benchmark -match '^\[(?<lib>[^\]]+)\]') { $Matches['lib'] } else { 'unknown' }
                $current = [ordered]@{
                    run              = $r
                    section          = $section
                    benchmark        = $benchmark
                    library          = $library
                    canonical        = Get-CompareCanonicalName -Benchmark $benchmark
                    iterations       = $null
                    total_us         = $null
                    avg_us           = $null
                    logs_sec         = $null
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
            if ($line -match '^\s+Total time:\s+(?<v>[0-9.]+) microseconds$') {
                $current.total_us = [double]$Matches['v']; continue
            }
            if ($line -match '^\s+Average time per log:\s+(?<v>[0-9.]+) microseconds$') {
                $current.avg_us = [double]$Matches['v']; continue
            }
            if ($line -match '^\s+Logs per second:\s+(?<v>[0-9.eE+\-]+)$') {
                $current.logs_sec = [double]$Matches['v']; continue
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
            if ([string]::IsNullOrWhiteSpace($line) -and
                $null -ne $current -and
                $null -ne $current.logs_sec -and
                $current.section -eq 'Memory (async, minimal sink)') {
                $raw += [pscustomobject]$current
                $current = $null
            }
        }

        if ($null -ne $current -and $null -ne $current.logs_sec) {
            $raw += [pscustomobject]$current
        }
    }

    $sectionOrder = @(
        'Memory (async, minimal sink)',
        'File (async file sink)',
        'File (per-call flush request over async pipeline)',
        'File (strict sync flush semantics)'
    )

    $benchmarkOrder = @(
        '[NEI]  simple %s',
        '[spdlog] simple {}',
        '[NEI]  multi printf',
        '[spdlog] multi fmt',
        '[NEI]  multi printf (fmt_plan cache miss)',
        '[NEI]  llog_literal (opaque body)',
        '[spdlog] literal only',
        '[NEI]  vlog_literal (opaque body)',
        '[NEI] file simple %s',
        '[spdlog] file simple {}',
        '[NEI] file multi',
        '[spdlog] file multi',
        '[NEI] file llog_literal',
        '[NEI] file vlog_literal',
        '[NEI] file sync simple (flush request each log)',
        '[spdlog] file sync simple (async logger + flush request each log)',
        '[NEI] file sync multi (flush request each log)',
        '[spdlog] file sync multi (async logger + flush request each log)',
        '[NEI] file sync llog_literal (flush request each log)',
        '[NEI] file sync vlog_literal (flush request each log)',
        '[NEI] file strict simple (sync flush each log)',
        '[spdlog] file strict simple (sync flush each log)',
        '[NEI] file strict multi (sync flush each log)',
        '[spdlog] file strict multi (sync flush each log)'
    )

    $summary = @()
    foreach ($sectionName in $sectionOrder) {
        foreach ($benchmarkName in $benchmarkOrder) {
            $items = @($raw | Where-Object { $_.section -eq $sectionName -and $_.benchmark -eq $benchmarkName })
            if ($items.Count -eq 0) { continue }

            $avgMean = ($items.avg_us | Measure-Object -Average).Average
            $lpsMean = ($items.logs_sec | Measure-Object -Average).Average
            $totalMean = ($items.total_us | Measure-Object -Average).Average
            $stddev = if ($items.Count -gt 1) {
                [math]::Round([math]::Sqrt((($items.avg_us | ForEach-Object { ($_ - $avgMean) * ($_ - $avgMean) }) | Measure-Object -Average).Average), 4)
            } else { 0.0 }

            $avgFileSize = $null
            if ($null -ne $items[0].file_size) {
                $avgFileSize = [math]::Round((($items.file_size | Measure-Object -Average).Average), 0)
            }

            $producerSpins = if ($items | Where-Object { $null -ne $_.producer_spins }) {
                [math]::Round((($items | Where-Object { $null -ne $_.producer_spins } | Select-Object -ExpandProperty producer_spins | Measure-Object -Average).Average), 0)
            } else { $null }
            $flushWaits = if ($items | Where-Object { $null -ne $_.flush_wait_loops }) {
                [math]::Round((($items | Where-Object { $null -ne $_.flush_wait_loops } | Select-Object -ExpandProperty flush_wait_loops | Measure-Object -Average).Average), 2)
            } else { $null }
            $wakeups = if ($items | Where-Object { $null -ne $_.consumer_wakeups }) {
                [math]::Round((($items | Where-Object { $null -ne $_.consumer_wakeups } | Select-Object -ExpandProperty consumer_wakeups | Measure-Object -Average).Average), 2)
            } else { $null }
            $ringHwm = if ($items | Where-Object { $null -ne $_.ring_hwm }) {
                [math]::Round((($items | Where-Object { $null -ne $_.ring_hwm } | Select-Object -ExpandProperty ring_hwm | Measure-Object -Average).Average), 2)
            } else { $null }

            $summary += [pscustomobject]@{
                section          = $sectionName
                benchmark        = $benchmarkName
                library          = $items[0].library
                canonical        = $items[0].canonical
                runs             = $items.Count
                iterations       = $items[0].iterations
                avg_total_us     = [math]::Round($totalMean, 2)
                avg_us           = [math]::Round($avgMean, 4)
                stddev_us        = $stddev
                avg_logs_sec     = [math]::Round($lpsMean, 0)
                avg_file_size    = $avgFileSize
                avg_producer_spins = $producerSpins
                avg_flush_wait_loops = $flushWaits
                avg_consumer_wakeups = $wakeups
                avg_ring_hwm = $ringHwm
            }
        }
    }

    Write-Host ''
    Write-Host ('Log Compare Benchmark  -  {0}  (Runs: {1})' -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'), $TotalRuns)
    Write-Host ''
    foreach ($sectionName in $sectionOrder) {
        $rows = @($summary | Where-Object { $_.section -eq $sectionName })
        if ($rows.Count -eq 0) { continue }

        Write-Host $sectionName
        Write-Host ''
        Write-Host ('{0,-52}  {1,4}  {2,12}  {3,10}  {4,12}  {5,12}' -f `
            'Benchmark', 'Runs', 'Avg us/log', 'Stddev', 'Logs/sec', 'File bytes')
        Write-Host ('{0,-52}  {1,4}  {2,12}  {3,10}  {4,12}  {5,12}' -f `
            ('-' * 52), ('-' * 4), ('-' * 12), ('-' * 10), ('-' * 12), ('-' * 12))

        foreach ($row in $rows) {
            $fs = if ($null -ne $row.avg_file_size) { '{0:N0}' -f $row.avg_file_size } else { '-' }
            Write-Host ('{0,-52}  {1,4}  {2,12:N4}  {3,10:N4}  {4,12:N0}  {5,12}' -f `
                $row.benchmark, $row.runs, $row.avg_us, $row.stddev_us, $row.avg_logs_sec, $fs)
        }
        Write-Host ''
    }

    Write-Host 'Diagnostics (NEI rows only)'
    Write-Host ''
    Write-Host ('{0,-52}  {1,12}  {2,12}  {3,12}  {4,10}' -f `
        'Benchmark', 'Prod spins', 'Flush waits', 'Wakeups', 'Ring HWM')
    Write-Host ('{0,-52}  {1,12}  {2,12}  {3,12}  {4,10}' -f `
        ('-' * 52), ('-' * 12), ('-' * 12), ('-' * 12), ('-' * 10))
    foreach ($row in ($summary | Where-Object { $_.library -eq 'NEI' })) {
        $producer = if ($null -ne $row.avg_producer_spins) { '{0:N0}' -f $row.avg_producer_spins } else { '-' }
        $flush = if ($null -ne $row.avg_flush_wait_loops) { '{0:N2}' -f $row.avg_flush_wait_loops } else { '-' }
        $wakeups = if ($null -ne $row.avg_consumer_wakeups) { '{0:N2}' -f $row.avg_consumer_wakeups } else { '-' }
        $hwm = if ($null -ne $row.avg_ring_hwm) { '{0:N2}' -f $row.avg_ring_hwm } else { '-' }
        Write-Host ('{0,-52}  {1,12}  {2,12}  {3,12}  {4,10}' -f `
            $row.benchmark, $producer, $flush, $wakeups, $hwm)
    }
    Write-Host ''

    $md = [System.Text.StringBuilder]::new()
    $null = $md.AppendLine('# Log Compare Benchmark')
    $null = $md.AppendLine('')
    $null = $md.AppendLine('Generated: ' + (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'))
    $null = $md.AppendLine('Runs: ' + $TotalRuns)
    $null = $md.AppendLine('Build: Release')
    $null = $md.AppendLine('')

    Add-MarkdownHighlights -Markdown $md -Summary $summary
    Add-MarkdownMemoryFileRatios -Markdown $md -Summary $summary

    foreach ($sectionName in $sectionOrder) {
        $rows = @($summary | Where-Object { $_.section -eq $sectionName })
        if ($rows.Count -eq 0) { continue }

        $null = $md.AppendLine('## ' + $sectionName)
        $null = $md.AppendLine('')
        $null = $md.AppendLine('| Benchmark | Runs | Iterations | Avg total us | Avg us/log | Stddev us/log | Avg logs/sec | Avg file bytes |')
        $null = $md.AppendLine('|---|---:|---:|---:|---:|---:|---:|---:|')
        foreach ($row in $rows) {
            $fs = if ($null -ne $row.avg_file_size) { '{0:0}' -f $row.avg_file_size } else { '-' }
            $null = $md.AppendLine(('| {0} | {1} | {2} | {3:0.00} | {4:0.0000} | {5:0.0000} | {6:0} | {7} |' -f `
                $row.benchmark, $row.runs, $row.iterations, $row.avg_total_us, $row.avg_us, $row.stddev_us, $row.avg_logs_sec, $fs))
        }
        $null = $md.AppendLine('')

        $null = $md.AppendLine('### Diagnostics')
        $null = $md.AppendLine('')
        $null = $md.AppendLine('| Benchmark | Avg producer spins | Avg flush wait loops | Avg consumer wakeups | Avg ring HWM |')
        $null = $md.AppendLine('|---|---:|---:|---:|---:|')
        foreach ($row in $rows) {
            $producer = if ($null -ne $row.avg_producer_spins) { '{0:0}' -f $row.avg_producer_spins } else { '-' }
            $flush = if ($null -ne $row.avg_flush_wait_loops) { '{0:0.00}' -f $row.avg_flush_wait_loops } else { '-' }
            $wakeups = if ($null -ne $row.avg_consumer_wakeups) { '{0:0.00}' -f $row.avg_consumer_wakeups } else { '-' }
            $hwm = if ($null -ne $row.avg_ring_hwm) { '{0:0.00}' -f $row.avg_ring_hwm } else { '-' }
            $null = $md.AppendLine(('| {0} | {1} | {2} | {3} | {4} |' -f `
                $row.benchmark, $producer, $flush, $wakeups, $hwm))
        }
        $null = $md.AppendLine('')
    }

    Set-Content -Path $MarkdownPath -Value $md.ToString() -Encoding UTF8
    Write-Host "Saved: $MarkdownPath"
}

Invoke-LogCompareBenchmark -ExecutablePath $exePath -MarkdownPath $outMd -LogFilePaths $logFiles -TotalRuns $Runs