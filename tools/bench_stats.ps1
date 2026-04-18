param(
    [int]$Runs      = 20,
    [int]$TaskCount = 0
)

$ErrorActionPreference = 'Stop'

$candidates = @(
    '.\build\windows-vs2022-debug\tests\Release\task_threadpool_bench_demo.exe',
    '.\build\tests\Release\task_threadpool_bench_demo.exe'
)

$exe = $null
foreach ($candidate in $candidates) {
    if (Test-Path $candidate) {
        $exe = $candidate
        break
    }
}

if ($null -eq $exe) {
    throw "benchmark executable not found in known paths"
}

$rows = @()
for ($run = 1; $run -le $Runs; $run++) {
    $exeArgs = @()
    if ($TaskCount -gt 0) { $exeArgs += [string]$TaskCount }
    & $exe @exeArgs | ForEach-Object {
        if ($_ -match '^(1 thread|2 threads|4 threads|default|default_no_comp|1 thread_noop|2 threads_noop|4 threads_noop|default_noop|default_no_comp_noop|default_delayed_mix|default_no_comp_delayed_mix) \| workers=(\d+) \| enqueue_only_ms=([0-9.]+) \| drain_wait_ms=([0-9.]+) \| total_ms=([0-9.]+) \| enqueue_only_ns_per_task=([0-9.]+) \| drain_wait_ns_per_task=([0-9.]+) \| total_ns_per_task=([0-9.]+) \| sum=(\d+) \| status=(PASS|FAIL)$') {
            $rows += [pscustomobject]@{
                run = $run
                label = $matches[1]
                workers = [int]$matches[2]
                enqueue_ns = [double]$matches[6]
                drain_ns = [double]$matches[7]
                total_ns = [double]$matches[8]
                task_count_val = if ($TaskCount -gt 0) { $TaskCount } else { 10000 }
                status = $matches[10]
            }
        }
    }
}

function Get-Median([double[]]$values) {
    $sorted = $values | Sort-Object
    $n = $sorted.Count
    if ($n -eq 0) {
        return [double]::NaN
    }
    if ($n % 2 -eq 1) {
        return $sorted[[int]($n / 2)]
    }
    return ($sorted[$n / 2 - 1] + $sorted[$n / 2]) / 2.0
}

$rank = @{
    '1 thread' = 1
    '2 threads' = 2
    '4 threads' = 3
    'default' = 4
    'default_no_comp' = 5
    '1 thread_noop' = 6
    '2 threads_noop' = 7
    '4 threads_noop' = 8
    'default_noop' = 9
    'default_no_comp_noop' = 10
    'default_delayed_mix' = 11
    'default_no_comp_delayed_mix' = 12
}

$summary = $rows |
    Group-Object label |
    ForEach-Object {
        $g = $_.Group
        $med_enq = [math]::Round((Get-Median $g.enqueue_ns), 2)
        $posts_per_sec = if ($med_enq -gt 0) { [math]::Round(1e9 / $med_enq) } else { 0 }
        [pscustomobject]@{
            label = $_.Name
            runs = $g.Count
            workers = $g[0].workers
            median_enqueue_ns = $med_enq
            median_drain_ns = [math]::Round((Get-Median $g.drain_ns), 2)
            median_total_ns = [math]::Round((Get-Median $g.total_ns), 2)
            min_enqueue_ns = [math]::Round(($g.enqueue_ns | Measure-Object -Minimum).Minimum, 2)
            max_enqueue_ns = [math]::Round(($g.enqueue_ns | Measure-Object -Maximum).Maximum, 2)
            enqueue_posts_per_sec = $posts_per_sec
            pass_count = ($g | Where-Object status -eq 'PASS').Count
        }
    } |
    Sort-Object { $rank[$_.label] }

$summary | Format-Table -AutoSize
