<#
.SYNOPSIS
    循环运行可执行文件，实时捕获输出并匹配关键字。

.DESCRIPTION
    重复执行指定的 EXE 程序，实时捕获标准输出与错误流。
    每轮输出中若匹配到指定关键字，默认立即停止；可通过 -Continue 开关
    改为继续运行直到全部轮次结束。

.PARAMETER exePath
    要执行的可执行文件路径（必填，无默认值）。

.PARAMETER round
    运行轮数，默认为 10。

.PARAMETER keyword
    要匹配的关键字（大小写敏感），默认为 "FAILED"。

.PARAMETER Continue
    开关：指定后即使匹配到关键字也继续运行，不提前退出。

.PARAMETER passArgs
    传递给 exePath 的额外命令行参数字符串，按空格拆分后原样追加。
    例如 -passArgs "1000000 200 autofail=1"

.EXAMPLE
    .\run_task_threadpool_round.ps1 -exePath ".\task_thread_bench.exe"

.EXAMPLE
    .\run_task_threadpool_round.ps1 -exePath ".\bench.exe" -round 100 -keyword "ERROR"

.EXAMPLE
    .\run_task_threadpool_round.ps1 -exePath ".\bench.exe" -round 50 -keyword "FAILED" -Continue

.EXAMPLE
    .\run_task_threadpool_round.ps1 -exePath ".\bench.exe" -passArgs "1000000 200 autofail=1"
#>

param(
    [Parameter(Mandatory = $true, Position = 0, HelpMessage = "可执行文件路径（必填）")]
    [string]$exePath,

    [Parameter(Position = 1)]
    [int]$round = 10,

    [Parameter(Position = 2)]
    [string]$keyword = "FAILED",

    [Parameter()]
    [switch]$Continue,

    [Parameter()]
    [string]$passArgs = ""
)

$foundMatch = $false

# 校验 exe 文件是否存在
if (-not (Test-Path -Path $exePath -PathType Leaf)) {
    Write-Host "错误：可执行文件不存在 -> $exePath" -ForegroundColor Red
    exit 1
}

for ($run = 1; $run -le $round; $run++) {
    Write-Host "====================================="
    Write-Host "【第 $run / $round 轮】程序：$exePath" $(if ($passArgs) { " $passArgs" } else { "" })
    Write-Host "====================================="

    if ($passArgs) {
        $output = Invoke-Expression "& '$exePath' $passArgs 2>&1"
    } else {
        $output = & $exePath 2>&1
    }
    $exitCode = $LASTEXITCODE

    # 实时输出
    $output | ForEach-Object { Write-Host $_ }

    # 大小写敏感匹配
    if ($output -cmatch $keyword) {
        Write-Host "`n!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!" -ForegroundColor Red
        Write-Host "检测到关键字【$keyword】（大小写敏感），轮次：$run" -ForegroundColor Red
        Write-Host "程序退出码：$exitCode" -ForegroundColor Red
        Write-Host "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!" -ForegroundColor Red
        $foundMatch = $true

        if (-not $Continue) {
            Write-Host "已停止（-Continue 未指定）。" -ForegroundColor Yellow
            break
        }
        Write-Host "继续运行（-Continue 已指定）。" -ForegroundColor Yellow
    } else {
        Write-Host "【第 $run 轮完成，无匹配，退出码：$exitCode】`n" -ForegroundColor Green
    }
}

if (-not $foundMatch) {
    Write-Host "============================="
    Write-Host "全部 $round 轮执行完毕，未检测到关键字【$keyword】" -ForegroundColor Green
    Write-Host "============================="
} elseif ($Continue) {
    Write-Host "============================="
    Write-Host "全部 $round 轮执行完毕（已匹配关键字但 -Continue 生效）" -ForegroundColor Yellow
    Write-Host "============================="
}
