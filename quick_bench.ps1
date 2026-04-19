#!/usr/bin/env powershell
# Quick benchmark comparison: focus on key scenarios
$bench = "c:\Personal\Projects\LibNei\libnei-src\build\windows-vs2022-debug\tests\Debug\task_threadpool_bench_demo.exe"

function ExtractTPS {
    param([string]$output, [string]$label)
    $pattern = "^" + [regex]::Escape($label) + "\s*\|\s*workers=(\d+)\s*\|\s*enqueue_only_ms=([0-9.]+)\s*\|\s*drain_wait_ms=([0-9.]+)\s*\|\s*total_ms=([0-9.]+)"
    $m = [regex]::Match($output, $pattern, [System.Text.RegularExpressions.RegexOptions]::Multiline)
    if($m.Success) {
        $total_ms = [double]$m.Groups[4].Value
        return 100000000 / $total_ms
    }
    return 0
}

function CalcStats {
    param([array]$values)
    if($values.Count -eq 0) { return @(0,0) }
    $mean = ($values | Measure-Object -Average).Average
    $var = 0
    foreach($v in $values) { $var += ($v-$mean)*($v-$mean) }
    $std = if($values.Count -gt 1) { [math]::Sqrt($var/($values.Count-1)) } else { 0 }
    return @($mean,$std)
}

Write-Host "============================================"
Write-Host "Quick Benchmark Comparison (key scenarios)"
Write-Host "============================================"
Write-Host ""

# Collect current (MPSC)
Write-Host "[1/2] Collecting MPSC Immediate (dev)..."
$mpsc_default = @()
$mpsc_delayed_mix = @()
$mpsc_delayed_fixed = @()

for($r=1; $r -le 3; $r++) {
    $out = & $bench 100000 | Out-String
    $mpsc_default += ExtractTPS $out "default"
    $mpsc_delayed_mix += ExtractTPS $out "default_delayed_mix"
    $mpsc_delayed_fixed += ExtractTPS $out "default_delayed_fixed"
    Write-Host "  Round $r done"
}

# Switch to baseline
Write-Host "[2/2] Collecting baseline (origin/dev)..."
cd c:\Personal\Projects\LibNei\libnei-src
git checkout origin/dev 2>&1 | Out-Null

$baseline_default = @()
$baseline_delayed_mix = @()
$baseline_delayed_fixed = @()

for($r=1; $r -le 3; $r++) {
    $out = & $bench 100000 | Out-String
    $baseline_default += ExtractTPS $out "default"
    $baseline_delayed_mix += ExtractTPS $out "default_delayed_mix"
    $baseline_delayed_fixed += ExtractTPS $out "default_delayed_fixed"
    Write-Host "  Round $r done"
}

# Generate report
Write-Host ""
Write-Host "============================================"
Write-Host "RESULTS (100k benchmark)"
Write-Host "============================================"
Write-Host ""

$m_def_stats = CalcStats $mpsc_default
$b_def_stats = CalcStats $baseline_default
$m_mix_stats = CalcStats $mpsc_delayed_mix
$b_mix_stats = CalcStats $baseline_delayed_mix
$m_fix_stats = CalcStats $mpsc_delayed_fixed
$b_fix_stats = CalcStats $baseline_delayed_fixed

$report = @()
$report += "Scenario,MPSC_TPS_Mean,MPSC_TPS_Std,Baseline_TPS_Mean,Baseline_TPS_Std,Delta_Pct"
$report += ("default,{0:N0},{1:N0},{2:N0},{3:N0},{4:+0.0;-0.0}%" -f $m_def_stats[0], $m_def_stats[1], $b_def_stats[0], $b_def_stats[1], (($m_def_stats[0]-$b_def_stats[0])/$b_def_stats[0])*100)
$report += ("default_delayed_mix,{0:N0},{1:N0},{2:N0},{3:N0},{4:+0.0;-0.0}%" -f $m_mix_stats[0], $m_mix_stats[1], $b_mix_stats[0], $b_mix_stats[1], (($m_mix_stats[0]-$b_mix_stats[0])/$b_mix_stats[0])*100)
$report += ("default_delayed_fixed,{0:N0},{1:N0},{2:N0},{3:N0},{4:+0.0;-0.0}%" -f $m_fix_stats[0], $m_fix_stats[1], $b_fix_stats[0], $b_fix_stats[1], (($m_fix_stats[0]-$b_fix_stats[0])/$b_fix_stats[0])*100)

$report | ForEach-Object { Write-Host $_ }
$report | Set-Content -Path "c:\Personal\Projects\LibNei\libnei-src\bench_quick_comparison.csv" -Encoding UTF8

Write-Host ""
Write-Host "Report saved: bench_quick_comparison.csv"
Write-Host ""

git checkout dev 2>&1 | Out-Null
Write-Host "Done! Back on dev branch."
