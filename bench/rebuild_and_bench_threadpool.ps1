# ThreadPool benchmark — rebuild + run + summarize
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File .\bench\rebuild_and_bench_threadpool.ps1
# Parameters:
#   -Runs10k  N   (default 3)  — repeat count for 10 000-task suite
#   -Runs100k N   (default 2)  — repeat count for 100 000-task suite
#   -SkipBuild    — skip cmake --build (use existing binaries)
param(
    [int]$Runs10k = 3,
    [int]$Runs100k = 2,
    [switch]$SkipBuild,
    [string]$BuildDir
)

$ErrorActionPreference = 'Stop'

# ── paths ────────────────────────────────────────────────────────────────────
$repoRoot = Split-Path -Parent $PSScriptRoot
$buildDir = if ($BuildDir) { $BuildDir } else { "$repoRoot\build\windows-vs2022-release-shared" }
$exePath = "$buildDir\bench\Release\task_threadpool_bench.exe"
$runtimeDir = "$buildDir\bench\Release"
$outMd = "$repoRoot\threadpool_bench_latest.md"

# ── scenario whitelist (order determines display order) ──────────────────────
$scenarios = @(
    '1 thread', '2 threads', '4 threads',
    'default', 'default_no_comp',
    '1 thread_noop', '2 threads_noop', '4 threads_noop',
    'default_noop', 'default_no_comp_noop',
    'default_delayed_mix', 'default_no_comp_delayed_mix',
    'default_delayed_fixed', 'default_no_comp_delayed_fixed'
)

# ── build ────────────────────────────────────────────────────────────────────
Get-Process -ErrorAction SilentlyContinue |
Where-Object { $_.ProcessName -in @('task_threadpool_bench', 'nei_tests') } |
Stop-Process -Force

if (-not $SkipBuild) {
    Write-Host '==> Building...'
    & cmake --build $buildDir --config Release --target neixx task_threadpool_bench
    Write-Host ''
}

# sync DLLs so the exe can find them
Copy-Item -Force "$buildDir\modules\neixx\Release\neixx.dll" "$runtimeDir\neixx.dll"
Copy-Item -Force "$buildDir\modules\nei\Release\nei.dll"     "$runtimeDir\nei.dll"

if (-not (Test-Path $exePath)) { throw "exe not found: $exePath" }

# ── run & collect raw rows ───────────────────────────────────────────────────
$linePattern = '^(?<lbl>.+?) \| workers=(?<w>\d+) \| enqueue_only_ms=(?<enq>[0-9.]+) \| drain_wait_ms=(?<drn>[0-9.]+) \| total_ms=(?<tot>[0-9.]+) \| enqueue_only_ns_per_task=[0-9.]+ \| drain_wait_ns_per_task=[0-9.]+ \| total_ns_per_task=(?<tot_ns>[0-9.]+) \| sum=\d+ \| status=(?<st>PASS|FAIL)\r?$'

$raw = @()
foreach ($n in @(10000, 100000)) {
    $runs = if ($n -eq 10000) { $Runs10k } else { $Runs100k }
    for ($r = 1; $r -le $runs; $r++) {
        Write-Host ("  Running {0,6} tasks  (run {1}/{2})" -f $n, $r, $runs)
        & $exePath $n | ForEach-Object {
            $m = [regex]::Match($_, $linePattern)
            if ($m.Success -and $m.Groups['lbl'].Value -in $scenarios) {
                $tot = [double]$m.Groups['tot'].Value
                $raw += [pscustomobject]@{
                    n        = $n
                    run      = $r
                    scenario = $m.Groups['lbl'].Value
                    workers  = [int]$m.Groups['w'].Value
                    enq_ms   = [double]$m.Groups['enq'].Value
                    drn_ms   = [double]$m.Groups['drn'].Value
                    tot_ms   = $tot
                    tot_ns   = [double]$m.Groups['tot_ns'].Value
                    tps      = if ($tot -gt 0) { [int]($n / ($tot / 1000.0)) } else { 0 }
                    pass     = ($m.Groups['st'].Value -eq 'PASS')
                }
            }
        }
    }
}

# ── aggregate per (n, scenario) ──────────────────────────────────────────────
$summary = @()
foreach ($n in @(10000, 100000)) {
    foreach ($sc in $scenarios) {
        $g = @($raw | Where-Object { $_.n -eq $n -and $_.scenario -eq $sc })
        if ($g.Count -eq 0) { continue }

        $tots = $g.tot_ms
        $tpss = $g.tps
        $mean_tot = ($tots | Measure-Object -Average).Average
        $stddev_tot = if ($g.Count -gt 1) {
            [math]::Round([math]::Sqrt(($tots | ForEach-Object { ($_ - $mean_tot) * ($_ - $mean_tot) } | Measure-Object -Average).Average), 2)
        }
        else { 0.0 }
        $summary += [pscustomobject]@{
            n          = $n
            scenario   = $sc
            runs       = $g.Count
            workers    = $g[0].workers
            avg_enq    = [math]::Round(($g.enq_ms | Measure-Object -Average).Average, 2)
            avg_drn    = [math]::Round(($g.drn_ms | Measure-Object -Average).Average, 2)
            avg_tot    = [math]::Round($mean_tot, 2)
            stddev_tot = $stddev_tot
            avg_tot_ns = [math]::Round(($g.tot_ns | Measure-Object -Average).Average, 1)
            avg_tps    = [int][math]::Round(($tpss | Measure-Object -Average).Average, 0)
            pass_count = [int]($g | Measure-Object -Property pass -Sum).Sum
        }
    }
}

# ── print to console ──────────────────────────────────────────────────────────
# layout: scenario(32) W(4) Enq(7) Drn(7) Tot(7) Stddev(7) ns/t(9) TPS(10) PASS(5)
$hdr = '{0,-32}  {1,4}  {2,7}  {3,7}  {4,7}  {5,7}  {6,9}  {7,10}  {8}'
$data = '{0,-32}  {1,4}  {2,7:0.00}  {3,7:0.00}  {4,7:0.00}  {5,7:0.00}  {6,9:0.0}  {7,10:N0}  {8}'

Write-Host ''
Write-Host ('ThreadPool Benchmark  -  {0}' -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'))
Write-Host ''

foreach ($n in @(10000, 100000)) {
    $label = if ($n -eq 10000) { '10 000' } else { '100 000' }
    Write-Host ("-- {0} tasks {1}" -f $label, ('-' * 70))
    Write-Host ($hdr -f 'Scenario', 'W', 'Enq ms', 'Drn ms', 'Tot ms', 'Stddev', 'ns/task', 'Avg TPS', 'PASS')
    Write-Host ($hdr -f ('-' * 32), ('-' * 4), ('-' * 7), ('-' * 7), ('-' * 7), ('-' * 7), ('-' * 9), ('-' * 10), ('-' * 5))

    foreach ($row in ($summary | Where-Object { $_.n -eq $n })) {
        $passStr = ('{0}/{1}' -f $row.pass_count, $row.runs)
        Write-Host ($data -f $row.scenario, $row.workers, $row.avg_enq, $row.avg_drn, $row.avg_tot, $row.stddev_tot, $row.avg_tot_ns, $row.avg_tps, $passStr)
    }
    Write-Host ''
}

# ── write markdown ────────────────────────────────────────────────────────────
$md = [System.Text.StringBuilder]::new()
$null = $md.AppendLine('# ThreadPool Benchmark')
$null = $md.AppendLine('')
$null = $md.AppendLine('Generated: ' + (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'))
$null = $md.AppendLine('Runs: 10k x ' + $Runs10k + ', 100k x ' + $Runs100k)
$null = $md.AppendLine('')

foreach ($n in @(10000, 100000)) {
    $null = $md.AppendLine('## ' + $n + ' tasks')
    $null = $md.AppendLine('')
    $null = $md.AppendLine('| Scenario | W | Enq ms | Drn ms | Tot ms | Stddev | ns/task | Avg TPS | PASS |')
    $null = $md.AppendLine('|---|---:|---:|---:|---:|---:|---:|---:|---:|')
    foreach ($row in ($summary | Where-Object { $_.n -eq $n })) {
        $passStr = '{0}/{1}' -f $row.pass_count, $row.runs
        $null = $md.AppendLine(('| {0} | {1} | {2:0.00} | {3:0.00} | {4:0.00} | {5:0.00} | {6:0.0} | {7:N0} | {8} |' -f `
                    $row.scenario, $row.workers, $row.avg_enq, $row.avg_drn, $row.avg_tot, $row.stddev_tot, $row.avg_tot_ns, $row.avg_tps, $passStr))
    }
    $null = $md.AppendLine('')
}

Set-Content -Path $outMd -Value $md.ToString() -Encoding UTF8
Write-Host "Saved: $outMd"