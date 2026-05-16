[CmdletBinding()]
param(
    [int]$Rounds = 5,
    [int]$TaskCount = 100000,
    [string]$TracingMode = "off",
    [string]$BuildDir = ""
)

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$runScript = Join-Path $scriptDir "run_task_threadpool_bench.ps1"

if (-not (Test-Path $runScript)) {
    throw "Missing script: $runScript"
}

$benchExe = ""
if ($BuildDir -ne "") {
    $benchExe = Join-Path $BuildDir "bench\Release\task_threadpool_bench.exe"
}

$params = @{
    Rounds = $Rounds
    TaskCount = $TaskCount
    TracingMode = $TracingMode
}

if ($benchExe -ne "") {
    $params.BenchExe = $benchExe
}

& $runScript @params
