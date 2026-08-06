[CmdletBinding()]
param(
    [int]$Rounds = 10,
    [int]$TaskCount = 1000000,
    [string]$TracingMode = "off",
    [string]$BenchExe = "",
    [string]$OutputDir = "",
    [switch]$SkipBuild,
    [switch]$SkipParse
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = Split-Path -Parent $scriptDir

if ($OutputDir -eq "") {
    $OutputDir = Join-Path $scriptDir "output"
}
if (-not (Test-Path $OutputDir)) {
    New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null
}

if ($BenchExe -eq "") {
    $candidates = @(
        (Join-Path $repoRoot "build\windows-vs2022-release-shared\bench\Release\task_threadpool_bench.exe"),
        (Join-Path $repoRoot "build\bench\Release\task_threadpool_bench.exe")
    )
    foreach ($c in $candidates) {
        if (Test-Path $c) {
            $BenchExe = $c
            break
        }
    }
}

if ($BenchExe -eq "" -or -not (Test-Path $BenchExe)) {
    throw "Cannot find task_threadpool_bench.exe. Pass -BenchExe <path>."
}

if ($TracingMode -notin @("on", "off", "true", "false", "1", "0")) {
    throw "TracingMode must be one of: on/off/true/false/1/0"
}

if (-not $SkipBuild) {
    Write-Host "==> Building task_threadpool_bench (Release)..." -ForegroundColor Yellow
    cmake --build (Join-Path $repoRoot "build\windows-vs2022-release-shared") --config Release --target task_threadpool_bench
}

$timestamp = Get-Date -Format "yyyyMMdd_HHmmss"
$rawLogPath = Join-Path $OutputDir ("task_threadpool_bench_raw_{0}.log" -f $timestamp)
$reportPath = Join-Path $OutputDir ("task_threadpool_bench_report_{0}.md" -f $timestamp)

Write-Host "==> Running $Rounds rounds, task_count=$TaskCount, tracing=$TracingMode" -ForegroundColor Cyan
Write-Host "==> Raw log: $rawLogPath"

"task_threadpool_bench run $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')" | Out-File -FilePath $rawLogPath -Encoding utf8
"bench_exe=$BenchExe" | Out-File -FilePath $rawLogPath -Append -Encoding utf8
"rounds=$Rounds task_count=$TaskCount tracing_mode=$TracingMode" | Out-File -FilePath $rawLogPath -Append -Encoding utf8
"" | Out-File -FilePath $rawLogPath -Append -Encoding utf8

for ($i = 1; $i -le $Rounds; $i++) {
    Write-Host ("  Round {0}/{1}" -f $i, $Rounds)
    "=== Round $i ===" | Out-File -FilePath $rawLogPath -Append -Encoding utf8
    $cmdLine = ('"{0}" {1} {2} 2>&1' -f $BenchExe, $TaskCount, $TracingMode)
    cmd /d /c $cmdLine | Out-File -FilePath $rawLogPath -Append -Encoding utf8
    "" | Out-File -FilePath $rawLogPath -Append -Encoding utf8
}

if (-not $SkipParse) {
    $parseScript = Join-Path $scriptDir "parse_task_threadpool_bench_result.ps1"
    if (-not (Test-Path $parseScript)) {
        throw "Parse script not found: $parseScript"
    }

    & $parseScript -InputLog $rawLogPath -OutputMarkdown $reportPath
    Write-Host "==> Report: $reportPath" -ForegroundColor Green
}
