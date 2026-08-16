<#
.SYNOPSIS
    Unified benchmark runner for all libnei benches.
    Saves raw results to timestamped log files under the output directory.

.PARAMETER BenchBinDir
    Path to directory containing bench .exe files
    (e.g. build\windows-vs2022-shared\bench\Release).

.PARAMETER LogOutputDir
    Root directory for log output. A `bench_<timestamp>` subdirectory
    will be created automatically.

.EXAMPLE
    .\run_all_benches.ps1 -BenchBinDir build\windows-vs2022-shared\bench\Release -LogOutputDir bench\results
#>

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$BenchBinDir,

    [Parameter(Mandatory = $true)]
    [string]$LogOutputDir
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Continue"

# ---------------------------------------------------------------------------
# Resolve paths & set up environment
# ---------------------------------------------------------------------------
$timestamp = Get-Date -Format "yyyyMMdd_HHmmss"
$outDir = Join-Path $LogOutputDir "bench_$timestamp"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

# Add DLL search paths: bench dir itself + sibling Release/ dir
$binParent = Split-Path -Parent $BenchBinDir
$releaseDir = Join-Path $binParent "..\Release"
$env:PATH = "$BenchBinDir;$releaseDir;$env:PATH"

Write-Host "=== libnei Unified Bench Runner ===" -ForegroundColor Cyan
Write-Host "Bin dir : $BenchBinDir"
Write-Host "Log dir : $outDir"
Write-Host "Started : $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"
Write-Host ""

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
function Write-RunHeader($Name, $Round, $TotalRounds) {
    if ($TotalRounds -gt 1) {
        "`n=== $Name === Round $Round / $TotalRounds ===`n"
    } else {
        "`n=== $Name ===`n"
    }
}

function Invoke-BenchRound {
    param(
        [string]$Name,
        [string]$Exe,
        [string[]]$ExeArgs,
        [int]$Rounds = 1,
        [string]$LogFileName = $null
    )
    $logFile = if ($LogFileName) { Join-Path $outDir $LogFileName } else { Join-Path $outDir "$Name.log" }

    for ($r = 1; $r -le $Rounds; $r++) {
        Write-Host ("  [{0,-38}] round {1}/{2}" -f $Name, $r, $Rounds)
        Write-RunHeader $Name $r $Rounds | Out-File -FilePath $logFile -Append -Encoding utf8
        $output = & $Exe @ExeArgs 2>&1
        $output | Out-File -FilePath $logFile -Append -Encoding utf8
    }
}

# ---------------------------------------------------------------------------
# Benchmarks
# ---------------------------------------------------------------------------

# ── Task benches (multi-round, documented methodology) ─────────────────────
$taskThreadExe     = Join-Path $BenchBinDir "task_thread_bench.exe"
$taskThreadpoolExe = Join-Path $BenchBinDir "task_threadpool_bench.exe"
$taskTpParallelExe = Join-Path $BenchBinDir "task_threadpool_parallel_bench.exe"

if (Test-Path $taskThreadExe) {
    Invoke-BenchRound -Name "task_thread_tracing_on"  -Exe $taskThreadExe -ExeArgs @("1000000", "on")  -Rounds 10 -LogFileName "task_thread_tracing_on.log"
    Invoke-BenchRound -Name "task_thread_tracing_off" -Exe $taskThreadExe -ExeArgs @("1000000", "off") -Rounds 10 -LogFileName "task_thread_tracing_off.log"
} else { Write-Host "  [SKIP] task_thread_bench.exe not found" -ForegroundColor Yellow }

if (Test-Path $taskThreadpoolExe) {
    Invoke-BenchRound -Name "task_threadpool" -Exe $taskThreadpoolExe -ExeArgs @("1000000", "off") -Rounds 5
} else { Write-Host "  [SKIP] task_threadpool_bench.exe not found" -ForegroundColor Yellow }

if (Test-Path $taskTpParallelExe) {
    Invoke-BenchRound -Name "task_threadpool_parallel" -Exe $taskTpParallelExe -ExeArgs @() -Rounds 5
} else { Write-Host "  [SKIP] task_threadpool_parallel_bench.exe not found" -ForegroundColor Yellow }

# ── Log benches (multi-run, documented methodology) ────────────────────────
$logBenchExe   = Join-Path $BenchBinDir "log_bench.exe"
$logCompareExe = Join-Path $BenchBinDir "log_bench_compare.exe"
$logTempDir    = Join-Path $outDir "log_temp"
New-Item -ItemType Directory -Force -Path $logTempDir | Out-Null

if (Test-Path $logBenchExe) {
    Invoke-BenchRound -Name "log_bench" -Exe $logBenchExe -ExeArgs @($logTempDir) -Rounds 5
} else { Write-Host "  [SKIP] log_bench.exe not found" -ForegroundColor Yellow }

if (Test-Path $logCompareExe) {
    Invoke-BenchRound -Name "log_bench_compare" -Exe $logCompareExe -ExeArgs @($logTempDir) -Rounds 5
} else { Write-Host "  [SKIP] log_bench_compare.exe not found" -ForegroundColor Yellow }

# ── Single-run benches ─────────────────────────────────────────────────────
$singleBenches = @(
    @{Name="post_job";                   Exe="post_job_bench.exe";                   Args=@()},
    @{Name="flake_id";                   Exe="flake_id_bench.exe";                   Args=@()},
    @{Name="string_append";              Exe="string_append_bench.exe";              Args=@()},
    @{Name="callback";                   Exe="callback_bench.exe";                   Args=@()},
    @{Name="async_file";                 Exe="async_file_bench.exe";                 Args=@()},
    @{Name="pipe_stream";                Exe="pipe_stream_bench.exe";                Args=@()},
    @{Name="pipe_stream_cross_process";  Exe="pipe_stream_cross_process_bench.exe";  Args=@()},
    @{Name="tcp_loopback";               Exe="tcp_loopback_bench.exe";               Args=@()},
    @{Name="tcp_rtt";                    Exe="tcp_rtt_bench.exe";                    Args=@()},
    @{Name="tcp_throughput";             Exe="tcp_throughput_bench.exe";             Args=@()},
    @{Name="tcp_conn_stress";            Exe="tcp_conn_stress_bench.exe";            Args=@()},
    @{Name="tls_throughput";             Exe="tls_throughput_bench.exe";             Args=@()},
    @{Name="http_throughput";            Exe="http_throughput_bench.exe";            Args=@("20000")},
    @{Name="http2_throughput_seq";       Exe="http2_throughput_bench.exe";           Args=@("10000","1")},
    @{Name="http2_throughput_par8";      Exe="http2_throughput_bench.exe";           Args=@("10000","8")},
    @{Name="http2_throughput_par64";     Exe="http2_throughput_bench.exe";           Args=@("10000","64")},
    @{Name="parallel_runner";            Exe="parallel_runner_bench.exe";            Args=@()},
    @{Name="task_priority_perf";         Exe="task_priority_perf_demo.exe";          Args=@()}
)

foreach ($b in $singleBenches) {
    $exePath = Join-Path $BenchBinDir $b.Exe
    if (Test-Path $exePath) {
        Invoke-BenchRound -Name $b.Name -Exe $exePath -ExeArgs $b.Args -Rounds 1
    } else {
        Write-Host "  [SKIP] $($b.Exe) not found" -ForegroundColor Yellow
    }
}

# ── Cleanup log temp files ─────────────────────────────────────────────────
Remove-Item -Recurse -Force $logTempDir -ErrorAction SilentlyContinue

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
Write-Host ""
Write-Host "=== Benchmarks complete ===" -ForegroundColor Green
Write-Host "Finished: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"
Write-Host "Logs   : $outDir"
Get-ChildItem $outDir -Filter "*.log" | ForEach-Object {
    $size = "{0,8:N0}" -f $_.Length
    Write-Host ("  {0}  {1}" -f $size, $_.Name)
}

# ---------------------------------------------------------------------------
# Report generation
# ---------------------------------------------------------------------------
Write-Host ""
Write-Host "=== Generating report ===" -ForegroundColor Cyan

function stats($vals) {
    $a=@($vals|Where-Object{$_})
    if($a.Count -eq 0){return @{mean=0;stddev=0;min=0;max=0;n=0}}
    $m=($a|Measure-Object -Average).Average
    $s=0;if($a.Count -gt 1){$s=[math]::Sqrt((($a|%{($_-$m)*($_-$m)})|Measure-Object -Average).Average)}
    @{mean=$m;stddev=$s;min=($a|Measure-Object -Min).Minimum;max=($a|Measure-Object -Max).Maximum;n=$a.Count}
}

function task-parse($f){
    $sc=@{};$cur=$null
    foreach($l in (Get-Content $f)){
        if($l -match '^---\s+(.+)\s+---$'){$cur=$Matches[1];if(-not $sc[$cur]){$sc[$cur]=@()}}
        if($l -match '^Post throughput:\s+([0-9.]+)' -and $cur){$sc[$cur]+=[double]$Matches[1]}
    }
    ($sc.Keys|Sort-Object|%{$s=$sc[$_];$st=stats $s;"| $_ | $($st.n) | $([math]::Round($st.mean,0)) /s | $([math]::Round($st.stddev,0)) /s | $([math]::Round($st.min,0)) /s | $([math]::Round($st.max,0)) /s |"}) -join "`n"
}

function log-parse($f){
    $bm=@{};$cur=$null
    foreach($l in (Get-Content $f)){
        if($l -match '^(?<name>.+?):$' -and $l -notmatch 'Iterations|Total|Average|Logs|File|Runtime|Phase' -and $l -notmatch '^\s'){
            $cur=$Matches['name'] -replace ' \(File:.*\)','';if(-not $bm[$cur]){$bm[$cur]=@{avg=@();lps=@()}}
        }
        if($l -match '^\s+E2E avg per log:\s+([0-9.]+)' -and $cur){$bm[$cur].avg+=[double]$Matches[1]}
        if($l -match '^\s+E2E logs/sec:\s+([0-9.eE+\-]+)' -and $cur){$bm[$cur].lps+=[double]$Matches[1]}
    }
    $order=@('Log Info','Log Warn','Log Error','Log with Formatting','Log Info (literal)','Log Verbose','Log Verbose (literal)',
             'File Log Info','File Log Warn','File Log Error','File Log with Formatting','File Log Verbose','File Log Info (literal)','File Log Verbose (literal)')
    ($order|%{if($bm[$_]){$d=$bm[$_];$sa=stats $d.avg;$sl=stats $d.lps;"| $_ | $($sa.n) | $([math]::Round($sa.mean,3)) μs | ±$([math]::Round($sa.stddev,3)) | $([math]::Round($sl.mean,0)) /s |"}}) -join "`n"
}

function log-cmp($f){
    $rows=@{};$section=$null;$curKey=$null
    foreach($l in (Get-Content $f)){
        if($l -match '^---\s+(.+)\s+---$'){$section=$Matches[1]}
        if($l -match '^\[(NEI|spdlog)\]\s+(.+)$'){$curKey="$section|$($Matches[1])|$($Matches[2])";if(-not $rows[$curKey]){$rows[$curKey]=@{avg=@();lps=@()}}}
        if($l -match '^\s+Average time per log:\s+([0-9.]+)' -and $curKey){$rows[$curKey].avg+=[double]$Matches[1]}
        if($l -match '^\s+Logs per second:\s+([0-9.eE+\-]+)' -and $curKey){$rows[$curKey].lps+=[double]$Matches[1]}
    }
    $secOrder=@('Memory (async, minimal sink)','File (async file sink)','File (per-call flush request over async pipeline)','File (strict sync flush semantics)')
    ($secOrder|%{$sec=$_;"`n**$sec**`n`n| Library | Benchmark | Runs | Avg μs/log | Stddev | Avg logs/s |`n|---|---|---:|---:|---:|---:|`n"+
        (($rows.Keys|Sort-Object|%{if($_ -like "$sec*"){$d=$rows[$_];$sa=stats $d.avg;$sl=stats $d.lps;$p=$_ -split '\|';$lib=$p[1];$bm=$p[2];"| $lib | $bm | $($sa.n) | $([math]::Round($sa.mean,3)) | ±$([math]::Round($sa.stddev,3)) | $([math]::Round($sl.mean,0)) |"}}) -join "`n")}) -join "`n"
}

function single-block($f){ (Get-Content $f -Raw) -replace '(?s)\n?===.*?===\n?','' }

$reportPath = Join-Path $outDir "report.md"
$sb=[System.Text.StringBuilder]::new()
$null=$sb.AppendLine("# libnei Benchmark Report")
$null=$sb.AppendLine("")
$null=$sb.AppendLine("**Date**: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')  ")
$null=$sb.AppendLine("**Machine**: $env:COMPUTERNAME  ")
$null=$sb.AppendLine("**Source**: $outDir  ")
$null=$sb.AppendLine("")

$f=Join-Path $outDir "task_thread_tracing_on.log"
if(Test-Path $f){$null=$sb.AppendLine("## 1. Task Thread (tracing ON, 10×1M)`n");$null=$sb.AppendLine("| Scenario | Runs | Mean Post | Stddev | Min | Max |");$null=$sb.AppendLine("|---|---:|---:|---:|---:|---:|");$null=$sb.AppendLine((task-parse $f));$null=$sb.AppendLine("")}
$f=Join-Path $outDir "task_thread_tracing_off.log"
if(Test-Path $f){$null=$sb.AppendLine("## 2. Task Thread (tracing OFF, 10×1M)`n");$null=$sb.AppendLine("| Scenario | Runs | Mean Post | Stddev | Min | Max |");$null=$sb.AppendLine("|---|---:|---:|---:|---:|---:|");$null=$sb.AppendLine((task-parse $f));$null=$sb.AppendLine("")}

$f=Join-Path $outDir "task_threadpool.log"
if(Test-Path $f){$null=$sb.AppendLine("## 3. Task ThreadPool (tracing OFF, 5×1M)`n");$null=$sb.AppendLine("| Scenario | Runs | Mean Post | Stddev | Min | Max |");$null=$sb.AppendLine("|---|---:|---:|---:|---:|---:|");$null=$sb.AppendLine((task-parse $f));$null=$sb.AppendLine("")}
$f=Join-Path $outDir "task_threadpool_parallel.log"
if(Test-Path $f){$null=$sb.AppendLine("## 4. Task ThreadPool Parallel (5×1M)`n");$null=$sb.AppendLine("| Scenario | Runs | Mean Post | Stddev | Min | Max |");$null=$sb.AppendLine("|---|---:|---:|---:|---:|---:|");$null=$sb.AppendLine((task-parse $f));$null=$sb.AppendLine("")}

$f=Join-Path $outDir "log_bench.log"
if(Test-Path $f){$null=$sb.AppendLine("## 5. Log Bench (NEI, 5 runs)`n");$null=$sb.AppendLine("| Benchmark | Runs | Avg E2E μs/log | Stddev | Avg E2E logs/s |");$null=$sb.AppendLine("|---|---:|---:|---:|---:|");$null=$sb.AppendLine((log-parse $f));$null=$sb.AppendLine("")}
$f=Join-Path $outDir "log_bench_compare.log"
if(Test-Path $f){$null=$sb.AppendLine("## 6. Log Compare (NEI vs spdlog, 5 runs)`n");$null=$sb.AppendLine((log-cmp $f));$null=$sb.AppendLine("")}

$null=$sb.AppendLine("## 7. Single-run Benches`n")
foreach($s in @(
    @{f="post_job.log";t="PostJob"},@{f="flake_id.log";t="Flake ID"},@{f="string_append.log";t="String Append"},
    @{f="callback.log";t="Callback"},@{f="async_file.log";t="AsyncFile"},@{f="pipe_stream.log";t="PipeStream"},
    @{f="pipe_stream_cross_process.log";t="PipeStream Cross-Process"},@{f="tcp_loopback.log";t="TCP Loopback"},
    @{f="tcp_rtt.log";t="TCP RTT"},@{f="tcp_throughput.log";t="TCP Throughput"},@{f="tcp_conn_stress.log";t="TCP Conn Stress"},
    @{f="tls_throughput.log";t="TLS Throughput"},@{f="parallel_runner.log";t="Parallel Runner"},@{f="task_priority_perf.log";t="Task Priority Perf"}
)){
    $fp=Join-Path $outDir $s.f
    if(Test-Path $fp){$null=$sb.AppendLine("### $($s.t)`n");$null=$sb.AppendLine('```');$null=$sb.AppendLine((single-block $fp).Trim());$null=$sb.AppendLine('```');$null=$sb.AppendLine("")}
}

Set-Content -Path $reportPath -Value $sb.ToString() -Encoding UTF8
Write-Host "Report : $reportPath" -ForegroundColor Green
Write-Host ""
Write-Host "=== All done ===" -ForegroundColor Green
