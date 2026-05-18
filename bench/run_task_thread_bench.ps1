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
        scenarios       = @{
            Standard       = @{}
            Delayed        = @{}
            "Multi-threaded" = @{}
        }
    }

    $currentScenario = ""
    foreach ($line in $lines) {
        if ($line -match "Tracing enabled:\s*(yes|no)") {
            $r.tracing_enabled = if ($Matches[1] -eq "yes") { 1 } else { 0 }
            continue
        }

        if ($line -match "---\s+Standard PostTask") {
            $currentScenario = "Standard"
            continue
        }
        if ($line -match "---\s+Delayed PostTask") {
            $currentScenario = "Delayed"
            continue
        }
        if ($line -match "---\s+Multi-threaded PostTask") {
            $currentScenario = "Multi-threaded"
            continue
        }

        if ($currentScenario -eq "") {
            continue
        }

        $s = $r.scenarios[$currentScenario]
        if ($line -match "Posted:\s*(\d+),\s*Failed:\s*(\d+)") {
            $s.posted = [int]$Matches[1]
            $s.failed = [int]$Matches[2]
            continue
        }
        if ($line -match "Post elapsed:\s*([\d.]+)\s*ms") {
            $s.post_elapsed_ms = [double]$Matches[1]
            continue
        }
        if ($line -match "Total elapsed:\s*([\d.]+)\s*ms") {
            $s.total_elapsed_ms = [double]$Matches[1]
            continue
        }
        if ($line -match "Post throughput:\s*([\d.]+)\s*tasks/sec") {
            $s.post_throughput = [double]$Matches[1]
            continue
        }
        if ($line -match "Total throughput:\s*([\d.]+)\s*tasks/sec") {
            $s.total_throughput = [double]$Matches[1]
            continue
        }
        if ($line -match "Avg post ns/task:\s*([\d.]+)") {
            $s.avg_post_ns = [double]$Matches[1]
            continue
        }
        if ($line -match "Avg drain ns/task:\s*([\d.]+)") {
            $s.avg_drain_ns = [double]$Matches[1]
            continue
        }
        if ($line -match "Avg total ns/task:\s*([\d.]+)") {
            $s.avg_total_ns = [double]$Matches[1]
            continue
        }
        if ($line -match "Verification:\s*(PASS|FAIL)") {
            $s.verify = $Matches[1]
            continue
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
    $scenarios = @("Standard", "Delayed", "Multi-threaded")
    $results   = @{}
    $nsResults = @{}
    foreach ($sc in $scenarios) {
        $results[$sc] = @()
        $nsResults[$sc] = @()
    }
    $rowData   = @()

    for ($i = 1; $i -le $rounds; $i++) {
        $output = & $BenchExe $taskCount $mode 2>&1
        $parsed = Parse-BenchOutput -lines $output

        foreach ($sc in $scenarios) {
            $s = $parsed.scenarios[$sc]
            if ($null -eq $s.total_throughput) {
                continue
            }

            $results[$sc] += [double]$s.total_throughput
            $nsResults[$sc] += [double]$s.avg_total_ns
            $rowData += [pscustomobject]@{
                Round           = $i
                Mode            = $mode.ToUpper()
                Scenario        = $sc
                TotalThroughput = [int]$s.total_throughput
                AvgPostNs       = [math]::Round([double]$s.avg_post_ns,  3)
                AvgDrainNs      = [math]::Round([double]$s.avg_drain_ns, 3)
                AvgTotalNs      = [math]::Round([double]$s.avg_total_ns, 3)
                Verify          = if ($null -eq $s.verify) { "N/A" } else { $s.verify }
                Failed          = if ($null -eq $s.failed) { 0 } else { [int]$s.failed }
            }
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

$sep  = "+" + ("-" * 7) + "+" + ("-" * 6) + "+" + ("-" * 17) + "+" + ("-" * 16) + "+" + ("-" * 11) + "+" + ("-" * 12) + "+" + ("-" * 12) + "+" + ("-" * 8) + "+" + ("-" * 8) + "+"
$hdr  = "| {0,-5} | {1,-4} | {2,-15} | {3,14} | {4,9} | {5,10} | {6,10} | {7,6} | {8,6} |" -f `
    "Round","Mode","Scenario","total_tp(t/s)","post_ns","drain_ns","total_ns","verify","failed"

Write-Host ""
Write-Host "=== Per-Round Results ===" -ForegroundColor Cyan
Write-Host $sep
Write-Host $hdr
Write-Host $sep
foreach ($row in $allRows) {
    $line = "| {0,-5} | {1,-4} | {2,-15} | {3,14} | {4,9} | {5,10} | {6,10} | {7,6} | {8,6} |" -f `
        $row.Round, $row.Mode, $row.Scenario, $row.TotalThroughput, $row.AvgPostNs, $row.AvgDrainNs, $row.AvgTotalNs, $row.Verify, $row.Failed
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
    $line = "| {0,-5} | {1,-4} | {2,-15} | {3,14} | {4,9} | {5,10} | {6,10} | {7,6} | {8,6} |" -f `
        $row.Round, $row.Mode, $row.Scenario, $row.TotalThroughput, $row.AvgPostNs, $row.AvgDrainNs, $row.AvgTotalNs, $row.Verify, $row.Failed
    Add-Content -Path $LogFile -Value $line
}
Add-Content -Path $LogFile -Value $sep

# ---------------------------------------------------------------------------
# Summary stats + baseline comparison
# ---------------------------------------------------------------------------
function Pct-Delta([double]$val, [double]$base) {
    if ($base -le 0) { return "N/A" }
    $pct = ($val - $base) / $base * 100
    return $pct.ToString("+0.0;-0.0") + "%"
}
function Gate-Status([double]$mean, [double]$gate) {
    if ($mean -ge $gate) { return "PASS" } else { return "FAIL" }
}

$scenarios = @("Standard", "Delayed", "Multi-threaded")
$summaryRows = @()
foreach ($sc in $scenarios) {
    $onStats    = Get-Stats -values ($onResult.Stats[$sc]    | ForEach-Object { [double]$_ })
    $offStats   = Get-Stats -values ($offResult.Stats[$sc]   | ForEach-Object { [double]$_ })
    $onNsStats  = Get-Stats -values ($onResult.NsStats[$sc]  | ForEach-Object { [double]$_ })
    $offNsStats = Get-Stats -values ($offResult.NsStats[$sc] | ForEach-Object { [double]$_ })

    $onDelta = "N/A"
    $offDelta = "N/A"
    $onGate = "N/A"
    $offGate = "N/A"
    if ($sc -eq "Standard") {
        $onDelta  = Pct-Delta  -val $onStats.Mean  -base $BaselineOn
        $offDelta = Pct-Delta  -val $offStats.Mean -base $BaselineOff
        $onGate   = Gate-Status -mean $onStats.Mean  -gate $GateOnMean
        $offGate  = Gate-Status -mean $offStats.Mean -gate $GateOffMean
    }

    $summaryRows += [pscustomobject]@{
        Mode      = "ON"
        Scenario  = $sc
        MeanTp    = [int]$onStats.Mean
        MinTp     = [int]$onStats.Min
        MaxTp     = [int]$onStats.Max
        StdDevTp  = [int]$onStats.StdDev
        CV        = $onStats.CV.ToString("F2")
        MeanNs    = $onNsStats.Mean.ToString("F1")
        VsBase    = $onDelta
        Gate      = $onGate
    }
    $summaryRows += [pscustomobject]@{
        Mode      = "OFF"
        Scenario  = $sc
        MeanTp    = [int]$offStats.Mean
        MinTp     = [int]$offStats.Min
        MaxTp     = [int]$offStats.Max
        StdDevTp  = [int]$offStats.StdDev
        CV        = $offStats.CV.ToString("F2")
        MeanNs    = $offNsStats.Mean.ToString("F1")
        VsBase    = $offDelta
        Gate      = $offGate
    }
}

$sumSep = "+" + ("-" * 8) + "+" + ("-" * 17) + "+" + ("-" * 14) + "+" + ("-" * 14) + "+" + ("-" * 14) + "+" + ("-" * 14) + "+" + ("-" * 8) + "+" + ("-" * 10) + "+" + ("-" * 12) + "+" + ("-" * 8) + "+"
$sumHdr = "| {0,-6} | {1,-15} | {2,12} | {3,12} | {4,12} | {5,12} | {6,6} | {7,8} | {8,10} | {9,6} |" -f `
    "Mode","Scenario","Mean(t/s)","Min(t/s)","Max(t/s)","StdDev","CV%","Mean_ns","vsBase","Gate"

Write-Host ""
Write-Host "=== Summary & Baseline Comparison ===" -ForegroundColor Cyan
Write-Host $sumSep
Write-Host $sumHdr
Write-Host $sumSep

foreach ($row in $summaryRows) {
    $sumLine = "| {0,-6} | {1,-15} | {2,12} | {3,12} | {4,12} | {5,12} | {6,6} | {7,8} | {8,10} | {9,6} |" -f `
        $row.Mode, $row.Scenario, $row.MeanTp, $row.MinTp, $row.MaxTp, $row.StdDevTp, $row.CV, $row.MeanNs, $row.VsBase, $row.Gate
    Write-Host $sumLine
}
Write-Host $sumSep

# Speedup ratio OFF vs ON
$onStandardMean = ($summaryRows | Where-Object { $_.Mode -eq "ON" -and $_.Scenario -eq "Standard" } | Select-Object -First 1).MeanTp
$offStandardMean = ($summaryRows | Where-Object { $_.Mode -eq "OFF" -and $_.Scenario -eq "Standard" } | Select-Object -First 1).MeanTp
$speedup = if ($onStandardMean -gt 0) { $offStandardMean / $onStandardMean } else { 0 }
Write-Host ""
$speedupPct = (($speedup - 1) * 100).ToString("+0.0;-0.0") + "%"
Write-Host ("Standard scenario OFF speedup vs ON : {0:F2}x  ({1})" -f $speedup, $speedupPct) -ForegroundColor Green
Write-Host ""
Write-Host ("Baselines used  ->  ON: {0:N0} t/s  |  OFF: {1:N0} t/s" -f $BaselineOn, $BaselineOff) -ForegroundColor DarkGray
Write-Host ("Gate thresholds ->  ON: {0:N0} t/s  |  OFF: {1:N0} t/s" -f $GateOnMean, $GateOffMean) -ForegroundColor DarkGray

# Write summary to log
Add-Content -Path $LogFile -Value ""
Add-Content -Path $LogFile -Value "=== Summary & Baseline Comparison ==="
Add-Content -Path $LogFile -Value $sumSep
Add-Content -Path $LogFile -Value $sumHdr
Add-Content -Path $LogFile -Value $sumSep
foreach ($row in $summaryRows) {
    $sumLine = "| {0,-6} | {1,-15} | {2,12} | {3,12} | {4,12} | {5,12} | {6,6} | {7,8} | {8,10} | {9,6} |" -f `
        $row.Mode, $row.Scenario, $row.MeanTp, $row.MinTp, $row.MaxTp, $row.StdDevTp, $row.CV, $row.MeanNs, $row.VsBase, $row.Gate
    Add-Content -Path $LogFile -Value $sumLine
}
Add-Content -Path $LogFile -Value $sumSep
Add-Content -Path $LogFile -Value ("Standard scenario OFF speedup vs ON : {0:F2}x" -f $speedup)
Add-Content -Path $LogFile -Value ("Baselines: ON={0}  OFF={1}" -f $BaselineOn, $BaselineOff)

# ---------------------------------------------------------------------------
# Exit code: FAIL if either gate fails
# ---------------------------------------------------------------------------
$onGate = ($summaryRows | Where-Object { $_.Mode -eq "ON" -and $_.Scenario -eq "Standard" } | Select-Object -First 1).Gate
$offGate = ($summaryRows | Where-Object { $_.Mode -eq "OFF" -and $_.Scenario -eq "Standard" } | Select-Object -First 1).Gate
if ($onGate -eq "FAIL" -or $offGate -eq "FAIL") {
    Write-Host ""
    Write-Host "GATE FAILED - see table above." -ForegroundColor Red
    exit 1
}
Write-Host "All gates PASSED." -ForegroundColor Green
