<#
.SYNOPSIS
    Generate report.md for an existing bench log directory
    (Windows or WSL results produced by run_all_benches.ps1 / its WSL twin).
.EXAMPLE
    .\generate_report.ps1 -LogDir bench\results\wsl_20260818_231246
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$LogDir
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Continue"
$outDir = (Resolve-Path $LogDir).Path

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
    @{f="tls_throughput.log";t="TLS Throughput"},@{f="http_throughput.log";t="HTTP Throughput"},
    @{f="http2_throughput_seq.log";t="HTTP/2 Throughput (seq)"},@{f="http2_throughput_par8.log";t="HTTP/2 Throughput (par8)"},
    @{f="http2_throughput_par64.log";t="HTTP/2 Throughput (par64)"},
    @{f="parallel_runner.log";t="Parallel Runner"},@{f="task_priority_perf.log";t="Task Priority Perf"}
)){
    $fp=Join-Path $outDir $s.f
    if(Test-Path $fp){$null=$sb.AppendLine("### $($s.t)`n");$null=$sb.AppendLine('```');$null=$sb.AppendLine((single-block $fp).Trim());$null=$sb.AppendLine('```');$null=$sb.AppendLine("")}
}

Set-Content -Path $reportPath -Value $sb.ToString() -Encoding UTF8
Write-Host "Report : $reportPath" -ForegroundColor Green
