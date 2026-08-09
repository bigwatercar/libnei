# Capture a post_job_bench hang dump via procdump on Windows.
$ErrorActionPreference = "Continue"
$procdump = "$env:TEMP\procdump\procdump64.exe"
$exe = "C:\Personal\Projects\LibNei\libnei-src\build\windows-vs2022-shared\bench\Release\post_job_bench.exe"
$env:PATH = "C:\Personal\Projects\LibNei\libnei-src\build\windows-vs2022-shared\Release;$env:PATH"
& $procdump -accepteula | Out-Null  # accept EULA once

for ($i = 1; $i -le 30; $i++) {
    $p = Start-Process -FilePath $exe -PassThru `
        -RedirectStandardOutput "$env:TEMP\pd_$i.txt" -RedirectStandardError "$env:TEMP\pd_e_$i.txt"
    if (-not $p.WaitForExit(30000)) {
        "run $i : HUNG (pid=$($p.Id)) - dumping"
        & $procdump -accepteula -ma $p.Id "$env:TEMP\dump_$i" 2>&1 | Select-Object -Last 3
        $p.Kill()
        "dumped: $env:TEMP\dump_$i.dmp"
        break
    } else {
        "run $i : ok"
    }
}
