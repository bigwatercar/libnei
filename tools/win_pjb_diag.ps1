# Reproduce post_job_bench hang on Windows with the TimedWait diagnostic.
$ErrorActionPreference = "Continue"
$exe = "C:\Personal\Projects\LibNei\libnei-src\build\windows-vs2022-shared\bench\Release\post_job_bench.exe"
$env:PATH = "C:\Personal\Projects\LibNei\libnei-src\build\windows-vs2022-shared\Release;$env:PATH"
$rounds = if ($args.Count -gt 0) { [int]$args[0] } else { 25 }
$hung = 0
for ($i = 1; $i -le $rounds; $i++) {
    $p = Start-Process -FilePath $exe -PassThru `
        -RedirectStandardOutput "$env:TEMP\pjb_diag_$i.txt" -RedirectStandardError "$env:TEMP\pjb_diag_e_$i.txt"
    if (-not $p.WaitForExit(30000)) {
        "run $i : HUNG (pid=$($p.Id))"
        $dl = Get-Content "$env:TEMP\pjb_diag_e_$i.txt" | Select-String "DEADLOCK"
        if ($dl) { $dl } else { "  (no DEADLOCK line in stderr)" }
        $p.Kill()
        $hung++
    } else {
        "run $i : ok"
    }
}
"TOTAL hung: $hung/$rounds"
