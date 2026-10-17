<#
.SYNOPSIS
    Runs the libnei benchmark suite on Windows and in WSL back to back, from one command.

.DESCRIPTION
    Both platforms read the same bench list (benches.tsv), so the two runs are directly
    comparable. The Windows run uses the local Release binaries; the WSL run uses the
    Release build inside the Linux filesystem (a build on /mnt/c would measure the 9p
    filesystem, not the ext4 root, and the numbers would not be comparable).

    Sequence matters: the two runs are NOT parallel, because concurrent CPU load makes both
    sides unusable. Reports are printed at the end for both log directories.

.PARAMETER WindowsBinDir
    Bench binary directory for the Windows build (default: build\windows-vs2022-shared\bench\Release).

.PARAMETER WslBuildRoot
    Release build root inside WSL (default: ~/libnei_build/wsl-release-shared).

.PARAMETER LogOutputDir
    Results root; each platform gets a bench_<timestamp> directory under it.

.PARAMETER SkipWindows
.PARAMETER SkipWsl
    Run only one side.

.EXAMPLE
    .\run_both_platforms.ps1

.EXAMPLE
    .\run_both_platforms.ps1 -SkipWsl -Only tcp_loopback,tls_throughput
#>

[CmdletBinding()]
param(
    [string]$WindowsBinDir = (Join-Path $PSScriptRoot "..\build\windows-vs2022-shared\bench\Release"),

    [string]$WslBuildRoot = "~/libnei_build/wsl-release-shared",

    [string]$LogOutputDir = (Join-Path $PSScriptRoot "results"),

    [string[]]$Only = @(),

    [switch]$SkipWindows,

    [switch]$SkipWsl
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$logRoot = if ([System.IO.Path]::IsPathRooted($LogOutputDir)) { $LogOutputDir } else { Join-Path $repoRoot $LogOutputDir }
$onlyArg = if ($Only.Count -gt 0) { $Only -join "," } else { "" }

Write-Host "=== libnei benchmarks: Windows + WSL ===" -ForegroundColor Cyan
Write-Host "Logs: $logRoot"
Write-Host ""

$windowsLogs = $null
$wslLogs = $null

if (-not $SkipWindows) {
    Write-Host "--- Windows run ---" -ForegroundColor Yellow
    $windowsBinDir = if ([System.IO.Path]::IsPathRooted($WindowsBinDir)) {
        $WindowsBinDir
    } else {
        Join-Path $repoRoot $WindowsBinDir
    }
    if (-not (Test-Path $windowsBinDir)) {
        throw "Windows bench dir not found: $windowsBinDir (build the Release config first)"
    }

    $before = Get-ChildItem $logRoot -Directory -Filter "bench_*" -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Name
    $params = @{ BenchBinDir = $windowsBinDir; LogOutputDir = $logRoot }
    if ($Only.Count -gt 0) { $params["Only"] = $Only }
    & (Join-Path $PSScriptRoot "run_all_benches.ps1") @params | Out-Null
    $windowsLogs = Get-ChildItem $logRoot -Directory -Filter "bench_*" |
        Where-Object { $_.Name -notin $before } |
        Sort-Object Name |
        Select-Object -Last 1 -ExpandProperty FullName
    Write-Host "Windows logs: $windowsLogs" -ForegroundColor Green
    Write-Host ""
}

if (-not $SkipWsl) {
    Write-Host "--- WSL run ---" -ForegroundColor Yellow
    # Paths handed to WSL must be POSIX; the results root lives on /mnt/c so both runs share it.
    $wslLogRoot = (& wsl -e wslpath -a "$logRoot").Trim()
    $wslBinDir = "$WslBuildRoot/bench"
    $wslScriptPath = Join-Path $PSScriptRoot "run_all_benches.sh"
    $wslScript = (& wsl -e wslpath -a "$wslScriptPath").Trim()

    $onlyClause = if ($onlyArg) { " --only '$onlyArg'" } else { "" }
    $cmd = "bash '$wslScript' --bin-dir $wslBinDir --log-dir '$wslLogRoot'$onlyClause"

    $before = & wsl -e bash -lc "ls -d $wslLogRoot/bench_* 2>/dev/null || true"
    & wsl -e bash -lc $cmd
    if ($LASTEXITCODE -ne 0) { throw "WSL benchmark run failed (exit $LASTEXITCODE)" }

    $after = (& wsl -e bash -lc "ls -d $wslLogRoot/bench_* 2>/dev/null || true") -split "`n" |
        Where-Object { $_ -and ($before -notcontains $_) }
    $wslLogs = ($after | Sort-Object | Select-Object -Last 1)
    Write-Host "WSL logs: $wslLogs" -ForegroundColor Green
    Write-Host ""
}

Write-Host "=== Done ===" -ForegroundColor Cyan
if ($windowsLogs) { Write-Host "  Windows: $windowsLogs" }
if ($wslLogs) { Write-Host "  WSL    : $wslLogs" }
Write-Host ""
Write-Host "Compare with:"
if ($windowsLogs) { Write-Host "  Get-Content '$windowsLogs\report.md'" }
if ($wslLogs) { Write-Host "  (WSL) cat '$wslLogs/report.md'" }
