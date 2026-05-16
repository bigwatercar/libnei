<#
.SYNOPSIS
    Run task_thread_bench with tracing ON and OFF, compare results and show table.

.PARAMETER Rounds
    Number of rounds per tracing mode. Default: 10

.PARAMETER TaskCount
    Number of tasks to post per round. Default: 100000

.PARAMETER BenchExe
    Path to task_thread_bench.exe. Auto-detected from build tree if not specified.

.PARAMETER OutputDir
    Directory for log output. Default: bench/output (relative to repo root).

.EXAMPLE
    .\run_task_thread_bench.ps1 -Rounds 10 -TaskCount 100000
#>

[CmdletBinding()]
param(
    [int]   $Rounds     = 10,
    [int]   $TaskCount  = 100000,
    [string]$BenchExe   = "",
    [string]$OutputDir  = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

# ---------------------------------------------------------------------------
# Resolve paths
# ---------------------------------------------------------------------------
$ScriptDir  = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot   = Split-Path -Parent $ScriptDir

if ($OutputDir -eq "") {
    $OutputDir = Join-Path $ScriptDir "output"
}
if (-not (Test-Path $OutputDir)) {
    New-Item -ItemType Directory -Force $OutputDir | Out-Null
}

if ($BenchExe -eq "") {
    $candidates = @(
        (Join-Path $RepoRoot "build\windows-vs2022-release-shared\bench\Release\task_thread_bench.exe"),
        (Join-Path $RepoRoot "build\bin\Release\task_thread_bench.exe"),
        (Join-Path $RepoRoot "build\bench\Release\task_thread_bench.exe")
    )
    foreach ($c in $candidates) {
        if (Test-Path $c) { $BenchExe = $c; break }
    }
}

if ($BenchExe -eq "" -or -not (Test-Path $BenchExe)) {
    Write-Error "Cannot find task_thread_bench.exe. Pass -BenchExe <path>."
    exit 1
}

$Timestamp  = Get-Date -Format "yyyyMMdd_HHmmss"
$LogFile    = Join-Path $OutputDir "bench_${Timestamp}.txt"

# ---------------------------------------------------------------------------
# Baselines (from bench/task_thread_bench_result.md)
# ---------------------------------------------------------------------------
$BaselineOn  = 6938273   # Tracing ON  baseline mean (tasks/sec)
$BaselineOff = 6800000   # Tracing OFF baseline mean (tasks/sec, estimated)

# Gate thresholds
$GateOnMean  = 6600000
$GateOffMean = 6600000

# ---------------------------------------------------------------------------
# Helper: parse a single bench output block into a hashtable
# ---------------------------------------------------------------------------
function Parse-BenchOutput([string[]]$lines) {
    $r = @{
        tracing_enabled = -1
        tasks           = 0
        posted_ok       = 0
        failed          = 0
        sentinel_failed = 0
        verify_ok       = 0
        post_elapsed_ms = 0.0
        total_elapsed_ms= 0.0
        post_throughput = 0.0
        total_throughput= 0.0
        avg_post_ns     = 0.0
        avg_drain_ns    = 0.0
        avg_total_ns    = 0.0
    }
    foreach ($line in $lines) {
        if ($line -match "tracing_enabled_for_run=(\d+)") {
            $r.tracing_enabled = [int]$Matches[1]
        }
        if ($line -match "tasks=(\d+),\s*posted_ok=(\d+),\s*failed=(\d+),\s*sentinel_failed=(\d+)") {
            $r.tasks           = [int]$Matches[1]
            $r.posted_ok       = [int]$Matches[2]
            $r.failed          = [int]$Matches[3]
            $r.sentinel_failed = [int]$Matches[4]
        }
        if ($line -match "verify_ok=(\d+)") {
            $r.verify_ok = [int]$Matches[1]
        }
        if ($line -match "post_elapsed_ms=([\d.]+),\s*total_elapsed_ms=([\d.]+)") {
            $r.post_elapsed_ms  = [double]$Matches[1]
            $r.total_elapsed_ms = [double]$Matches[2]
        }
        if ($line -match "post_throughput=([\d.]+).*total_throughput=([\d.]+)") {
            $r.post_throughput  = [double]$Matches[1]
            $r.total_throughput = [double]$Matches[2]
        }
        if ($line -match "avg_post_ns_per_task=([\d.]+),\s*avg_drain_ns_per_task=([\d.]+),\s*avg_total_ns_per_task=([\d.]+)") {
            $r.avg_post_ns  = [double]$Matches[1]
            $r.avg_drain_ns = [double]$Matches[2]
            $r.avg_total_ns = [double]$Matches[3]
        }
    }
    return $r
}

# ---------------------------------------------------------------------------
# Helper: compute stats from an array of values
# ---------------------------------------------------------------------------
function Get-Stats([double[]]$values) {
    $n    = $values.Count
    $mean = ($values | Measure-Object -Average).Average
    $min  = ($values | Measure-Object -Minimum).Minimum
    $max  = ($values | Measure-Object -Maximum).Maximum
    $variance = ($values | ForEach-Object { [math]::Pow($_ - $mean, 2) } | Measure-Object -Average).Average
    $stddev   = [math]::Sqrt($variance)
    $cv       = if ($mean -gt 0) { $stddev / $mean * 100 } else { 0 }
    return [pscustomobject]@{ N=$n; Mean=$mean; Min=$min; Max=$max; StdDev=$stddev; CV=$cv }
}

# ---------------------------------------------------------------------------
# Run one mode
# ---------------------------------------------------------------------------
function Run-Mode([string]$mode, [int]$rounds, [int]$taskCount) {
    $results   = @()
    $nsResults = @()
    $rowData   = @()

    for ($i = 1; $i -le $rounds; $i++) {
        $output = & $BenchExe $taskCount $mode 2>&1
        $parsed = Parse-BenchOutput -lines $output

        $results   += $parsed.total_throughput
        $nsResults += $parsed.avg_total_ns
        $rowData   += [pscustomobject]@{
            Round           = $i
            Mode            = $mode.ToUpper()
            TotalThroughput = [int]$parsed.total_throughput
            AvgPostNs       = [math]::Round($parsed.avg_post_ns,  1)
            AvgDrainNs      = [math]::Round($parsed.avg_drain_ns, 1)
            AvgTotalNs      = [math]::Round($parsed.avg_total_ns, 1)
            VerifyOk        = $parsed.verify_ok
            Failed          = $parsed.failed
            SentinelFailed  = $parsed.sentinel_failed
        }

        # Append raw output to log file
        Add-Content -Path $LogFile -Value "=== Round $i / Mode=$mode ==="
        Add-Content -Path $LogFile -Value $output
        Add-Content -Path $LogFile -Value ""
    }

    return @{ Stats=$results; NsStats=$nsResults; Rows=$rowData }
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
Write-Host ""
Write-Host "task_thread_bench  |  Rounds=$Rounds  TaskCount=$TaskCount" -ForegroundColor Cyan
Write-Host "Exe : $BenchExe"
Write-Host "Log : $LogFile"
Write-Host ""

# Header in log
Add-Content -Path $LogFile -Value "task_thread_bench run  $(Get-Date)"
Add-Content -Path $LogFile -Value "Rounds=$Rounds  TaskCount=$TaskCount"
Add-Content -Path $LogFile -Value "Exe=$BenchExe"
Add-Content -Path $LogFile -Value ""

Write-Host ">>> Running Tracing ON  ($Rounds rounds) ..." -ForegroundColor Yellow
$onResult  = Run-Mode -mode "on"  -rounds $Rounds -taskCount $TaskCount

Write-Host ">>> Running Tracing OFF ($Rounds rounds) ..." -ForegroundColor Yellow
$offResult = Run-Mode -mode "off" -rounds $Rounds -taskCount $TaskCount

# ---------------------------------------------------------------------------
# Per-round table
# ---------------------------------------------------------------------------
$allRows = $onResult.Rows + $offResult.Rows

$sep  = "+" + ("-" * 7) + "+" + ("-" * 6) + "+" + ("-" * 16) + "+" + ("-" * 11) + "+" + ("-" * 12) + "+" + ("-" * 12) + "+" + ("-" * 10) + "+" + ("-" * 8) + "+"
$hdr  = "| {0,-5} | {1,-4} | {2,14} | {3,9} | {4,10} | {5,10} | {6,8} | {7,6} |" -f `
    "Round","Mode","total_tp(t/s)","post_ns","drain_ns","total_ns","verify_ok","failed"

Write-Host ""
Write-Host "=== Per-Round Results ===" -ForegroundColor Cyan
Write-Host $sep
Write-Host $hdr
Write-Host $sep
foreach ($row in $allRows) {
    $line = "| {0,-5} | {1,-4} | {2,14} | {3,9} | {4,10} | {5,10} | {6,8} | {7,6} |" -f `
        $row.Round, $row.Mode, $row.TotalThroughput, $row.AvgPostNs, $row.AvgDrainNs, $row.AvgTotalNs, $row.VerifyOk, $row.Failed
    Write-Host $line
}
Write-Host $sep

# Also write table to log
Add-Content -Path $LogFile -Value ""
Add-Content -Path $LogFile -Value "=== Per-Round Results ==="
Add-Content -Path $LogFile -Value $sep
Add-Content -Path $LogFile -Value $hdr
Add-Content -Path $LogFile -Value $sep
foreach ($row in $allRows) {
    $line = "| {0,-5} | {1,-4} | {2,14} | {3,9} | {4,10} | {5,10} | {6,8} | {7,6} |" -f `
        $row.Round, $row.Mode, $row.TotalThroughput, $row.AvgPostNs, $row.AvgDrainNs, $row.AvgTotalNs, $row.VerifyOk, $row.Failed
    Add-Content -Path $LogFile -Value $line
}
Add-Content -Path $LogFile -Value $sep

# ---------------------------------------------------------------------------
# Summary stats + baseline comparison
# ---------------------------------------------------------------------------
$onStats    = Get-Stats -values ($onResult.Stats    | ForEach-Object { [double]$_ })
$offStats   = Get-Stats -values ($offResult.Stats   | ForEach-Object { [double]$_ })
$onNsStats  = Get-Stats -values ($onResult.NsStats  | ForEach-Object { [double]$_ })
$offNsStats = Get-Stats -values ($offResult.NsStats | ForEach-Object { [double]$_ })

function Pct-Delta([double]$val, [double]$base) {
    if ($base -le 0) { return "N/A" }
    $pct = ($val - $base) / $base * 100
    return $pct.ToString("+0.0;-0.0") + "%"
}
function Gate-Status([double]$mean, [double]$gate) {
    if ($mean -ge $gate) { return "PASS" } else { return "FAIL" }
}

$onDelta    = Pct-Delta  -val $onStats.Mean  -base $BaselineOn
$offDelta   = Pct-Delta  -val $offStats.Mean -base $BaselineOff
$onGate     = Gate-Status -mean $onStats.Mean  -gate $GateOnMean
$offGate    = Gate-Status -mean $offStats.Mean -gate $GateOffMean

$sumSep = "+" + ("-" * 8) + "+" + ("-" * 14) + "+" + ("-" * 14) + "+" + ("-" * 14) + "+" + ("-" * 14) + "+" + ("-" * 8) + "+" + ("-" * 12) + "+" + ("-" * 8) + "+"
$sumHdr = "| {0,-6} | {1,12} | {2,12} | {3,12} | {4,12} | {5,6} | {6,10} | {7,6} |" -f `
    "Mode","Mean(t/s)","Min(t/s)","Max(t/s)","StdDev","CV%","vs Baseline","Gate"

Write-Host ""
Write-Host "=== Summary & Baseline Comparison ===" -ForegroundColor Cyan
Write-Host $sumSep
Write-Host $sumHdr
Write-Host $sumSep

$onCv   = $onStats.CV.ToString("F2")
$offCv  = $offStats.CV.ToString("F2")
$onRow  = "| {0,-6} | {1,12} | {2,12} | {3,12} | {4,12} | {5,6} | {6,10} | {7,6} |" -f `
    "ON", [int]$onStats.Mean, [int]$onStats.Min, [int]$onStats.Max, [int]$onStats.StdDev, $onCv, $onDelta, $onGate
$offRow = "| {0,-6} | {1,12} | {2,12} | {3,12} | {4,12} | {5,6} | {6,10} | {7,6} |" -f `
    "OFF", [int]$offStats.Mean, [int]$offStats.Min, [int]$offStats.Max, [int]$offStats.StdDev, $offCv, $offDelta, $offGate

Write-Host $onRow
Write-Host $offRow
Write-Host $sumSep

# ns/task summary table
$nsSep = "+" + ("-" * 8) + "+" + ("-" * 12) + "+" + ("-" * 12) + "+" + ("-" * 12) + "+" + ("-" * 12) + "+" + ("-" * 8) + "+"
$nsHdr = "| {0,-6} | {1,10} | {2,10} | {3,10} | {4,10} | {5,6} |" -f `
    "Mode","Mean_ns","Min_ns","Max_ns","StdDev_ns","CV%"
$onNsCv  = $onNsStats.CV.ToString("F2")
$offNsCv = $offNsStats.CV.ToString("F2")
$onNsRow  = "| {0,-6} | {1,10} | {2,10} | {3,10} | {4,10} | {5,6} |" -f `
    "ON",  $onNsStats.Mean.ToString("F1"),  $onNsStats.Min.ToString("F1"),  $onNsStats.Max.ToString("F1"),  $onNsStats.StdDev.ToString("F1"),  $onNsCv
$offNsRow = "| {0,-6} | {1,10} | {2,10} | {3,10} | {4,10} | {5,6} |" -f `
    "OFF", $offNsStats.Mean.ToString("F1"), $offNsStats.Min.ToString("F1"), $offNsStats.Max.ToString("F1"), $offNsStats.StdDev.ToString("F1"), $offNsCv

Write-Host ""
Write-Host "=== ns/task Summary ===" -ForegroundColor Cyan
Write-Host $nsSep
Write-Host $nsHdr
Write-Host $nsSep
Write-Host $onNsRow
Write-Host $offNsRow
Write-Host $nsSep

Add-Content -Path $LogFile -Value ""
Add-Content -Path $LogFile -Value "=== ns/task Summary ==="
Add-Content -Path $LogFile -Value $nsSep
Add-Content -Path $LogFile -Value $nsHdr
Add-Content -Path $LogFile -Value $nsSep
Add-Content -Path $LogFile -Value $onNsRow
Add-Content -Path $LogFile -Value $offNsRow
Add-Content -Path $LogFile -Value $nsSep

# Speedup ratio OFF vs ON
$speedup = if ($onStats.Mean -gt 0) { $offStats.Mean / $onStats.Mean } else { 0 }
Write-Host ""
$speedupPct = (($speedup - 1) * 100).ToString("+0.0;-0.0") + "%"
Write-Host ("Tracing OFF speedup vs ON : {0:F2}x  ({1})" -f $speedup, $speedupPct) -ForegroundColor Green
Write-Host ""
Write-Host ("Baselines used  ->  ON: {0:N0} t/s  |  OFF: {1:N0} t/s" -f $BaselineOn, $BaselineOff) -ForegroundColor DarkGray
Write-Host ("Gate thresholds ->  ON: {0:N0} t/s  |  OFF: {1:N0} t/s" -f $GateOnMean, $GateOffMean) -ForegroundColor DarkGray

# Write summary to log
Add-Content -Path $LogFile -Value ""
Add-Content -Path $LogFile -Value "=== Summary & Baseline Comparison ==="
Add-Content -Path $LogFile -Value $sumSep
Add-Content -Path $LogFile -Value $sumHdr
Add-Content -Path $LogFile -Value $sumSep
Add-Content -Path $LogFile -Value $onRow
Add-Content -Path $LogFile -Value $offRow
Add-Content -Path $LogFile -Value $sumSep
Add-Content -Path $LogFile -Value ("Tracing OFF speedup vs ON : {0:F2}x" -f $speedup)
Add-Content -Path $LogFile -Value ("Baselines: ON={0}  OFF={1}" -f $BaselineOn, $BaselineOff)

# ---------------------------------------------------------------------------
# Exit code: FAIL if either gate fails
# ---------------------------------------------------------------------------
if ($onGate -eq "FAIL" -or $offGate -eq "FAIL") {
    Write-Host ""
    Write-Host "GATE FAILED - see table above." -ForegroundColor Red
    exit 1
}
Write-Host "All gates PASSED." -ForegroundColor Green
