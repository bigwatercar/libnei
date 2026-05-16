[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$InputLog,
    [string]$OutputMarkdown = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

if (-not (Test-Path $InputLog)) {
    throw "Input log not found: $InputLog"
}

if ($OutputMarkdown -eq "") {
    $dir = Split-Path -Parent $InputLog
    $base = [System.IO.Path]::GetFileNameWithoutExtension($InputLog)
    $OutputMarkdown = Join-Path $dir ("{0}_report.md" -f $base)
}

$scenarioOrder = @(
    "Standard PostTask (fast-path)",
    "Delayed PostTask (non-fast-path)",
    "Multi-threaded PostTask (4 threads)"
)

$rows = New-Object System.Collections.Generic.List[object]
$currentRound = 0
$currentScenario = ""
$current = $null

function Flush-Current {
    param([object]$obj, [System.Collections.Generic.List[object]]$list)
    if ($null -ne $obj) {
        $list.Add($obj)
    }
}

$lines = Get-Content -Path $InputLog
foreach ($line in $lines) {
    if ($line -match '^=== Round\s+(\d+)\s+===') {
        Flush-Current -obj $current -list $rows
        $current = $null
        $currentRound = [int]$Matches[1]
        $currentScenario = ""
        continue
    }

    if ($line -match '^---\s+(.+)\s+---$') {
        Flush-Current -obj $current -list $rows
        $currentScenario = $Matches[1]
        $current = [ordered]@{
            Round = $currentRound
            Scenario = $currentScenario
            Posted = 0
            Failed = 0
            SentinelFailed = $false
            PostElapsedMs = 0.0
            TotalElapsedMs = 0.0
            PostThroughput = 0.0
            TotalThroughput = 0.0
            AvgPostNs = 0.0
            AvgDrainNs = 0.0
            AvgTotalNs = 0.0
            Verification = "UNKNOWN"
        }
        continue
    }

    if ($null -eq $current) {
        continue
    }

    if ($line -match '^Posted:\s+(\d+),\s+Failed:\s+(\d+)(,\s+Sentinel FAILED)?$') {
        $current.Posted = [int]$Matches[1]
        $current.Failed = [int]$Matches[2]
        $current.SentinelFailed = -not [string]::IsNullOrEmpty($Matches[3])
        continue
    }
    if ($line -match '^Post elapsed:\s+([0-9.]+)\s+ms$') {
        $current.PostElapsedMs = [double]$Matches[1]
        continue
    }
    if ($line -match '^Total elapsed:\s+([0-9.]+)\s+ms$') {
        $current.TotalElapsedMs = [double]$Matches[1]
        continue
    }
    if ($line -match '^Post throughput:\s+([0-9.]+)\s+tasks/sec$') {
        $current.PostThroughput = [double]$Matches[1]
        continue
    }
    if ($line -match '^Total throughput:\s+([0-9.]+)\s+tasks/sec$') {
        $current.TotalThroughput = [double]$Matches[1]
        continue
    }
    if ($line -match '^Avg post ns/task:\s+([0-9.]+)$') {
        $current.AvgPostNs = [double]$Matches[1]
        continue
    }
    if ($line -match '^Avg drain ns/task:\s+([0-9.]+)$') {
        $current.AvgDrainNs = [double]$Matches[1]
        continue
    }
    if ($line -match '^Avg total ns/task:\s+([0-9.]+)$') {
        $current.AvgTotalNs = [double]$Matches[1]
        continue
    }
    if ($line -match '^Verification:\s+(PASS|FAIL)') {
        $current.Verification = $Matches[1]
        continue
    }
}
Flush-Current -obj $current -list $rows

if ($rows.Count -eq 0) {
    throw "No benchmark rows parsed from log."
}

$grouped = $rows | Group-Object Scenario

$summary = foreach ($g in $grouped) {
    $items = @($g.Group)
    [pscustomobject]@{
        Scenario = $g.Name
        Rounds = $items.Count
        PostThroughputAvg = [math]::Round(($items.PostThroughput | Measure-Object -Average).Average, 3)
        TotalThroughputAvg = [math]::Round(($items.TotalThroughput | Measure-Object -Average).Average, 3)
        AvgPostNs = [math]::Round(($items.AvgPostNs | Measure-Object -Average).Average, 3)
        AvgDrainNs = [math]::Round(($items.AvgDrainNs | Measure-Object -Average).Average, 3)
        AvgTotalNs = [math]::Round(($items.AvgTotalNs | Measure-Object -Average).Average, 3)
        PassCount = @($items | Where-Object { $_.Verification -eq 'PASS' }).Count
    }
}

$summary = $summary | Sort-Object { [array]::IndexOf($scenarioOrder, $_.Scenario) }

Write-Host ""
Write-Host "=== task_threadpool_bench Summary ===" -ForegroundColor Cyan
$summary | Format-Table -AutoSize Scenario, Rounds, PostThroughputAvg, TotalThroughputAvg, AvgPostNs, AvgDrainNs, AvgTotalNs, PassCount

$md = New-Object System.Text.StringBuilder
$null = $md.AppendLine("# task_threadpool_bench Report")
$null = $md.AppendLine("")
$null = $md.AppendLine(("Source log: {0}" -f $InputLog))
$null = $md.AppendLine(("Generated: {0}" -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss')))
$null = $md.AppendLine("")
$null = $md.AppendLine("## Summary")
$null = $md.AppendLine("")
$null = $md.AppendLine("| Scenario | Rounds | Avg Post Throughput (tasks/s) | Avg Total Throughput (tasks/s) | Avg Post ns/task | Avg Drain ns/task | Avg Total ns/task | PASS Rounds |")
$null = $md.AppendLine("|---|---:|---:|---:|---:|---:|---:|---:|")
foreach ($s in $summary) {
    $null = $md.AppendLine(("| {0} | {1} | {2} | {3} | {4} | {5} | {6} | {7}/{1} |" -f `
        $s.Scenario, $s.Rounds, $s.PostThroughputAvg, $s.TotalThroughputAvg, $s.AvgPostNs, $s.AvgDrainNs, $s.AvgTotalNs, $s.PassCount))
}

$null = $md.AppendLine("")
$null = $md.AppendLine("## Per Round")
$null = $md.AppendLine("")
$null = $md.AppendLine("| Round | Scenario | Posted | Failed | Sentinel Failed | Post elapsed (ms) | Total elapsed (ms) | Post Throughput | Total Throughput | Avg Post ns/task | Avg Drain ns/task | Avg Total ns/task | Verification |")
$null = $md.AppendLine("|---:|---|---:|---:|---|---:|---:|---:|---:|---:|---:|---:|---|")
foreach ($r in ($rows | Sort-Object Round, @{Expression={ [array]::IndexOf($scenarioOrder, $_.Scenario) }})) {
    $null = $md.AppendLine(("| {0} | {1} | {2} | {3} | {4} | {5} | {6} | {7} | {8} | {9} | {10} | {11} | {12} |" -f `
        $r.Round, $r.Scenario, $r.Posted, $r.Failed, $r.SentinelFailed, $r.PostElapsedMs, $r.TotalElapsedMs, $r.PostThroughput, $r.TotalThroughput, $r.AvgPostNs, $r.AvgDrainNs, $r.AvgTotalNs, $r.Verification))
}

Set-Content -Path $OutputMarkdown -Encoding utf8 -Value $md.ToString()
Write-Host ("Report saved: {0}" -f $OutputMarkdown) -ForegroundColor Green
