# win_rebuild_all.ps1 — Rebuild all 4 Windows quadrants using existing cmake dirs
$ErrorActionPreference = "Continue"
$SRC = "c:\Personal\Projects\LibNei\libnei-src"
$OUT = "$SRC\build\win_rebuild_result.txt"
"" | Out-File $OUT

$quadrants = @(
    @{Dir="win-debug-shared"; Config="Debug"},
    @{Dir="win-debug-static"; Config="Debug"},
    @{Dir="win-rel-shared";   Config="Release"},
    @{Dir="win-rel-static";   Config="Release"}
)

$allPass = $true
foreach ($q in $quadrants) {
    $d = $q.Dir
    $c = $q.Config
    Write-Host "=== $d ($c) ==="
    "=== $d ($c) ===" | Out-File $OUT -Append

    $buildDir = "$SRC\build\$d"
    if (-not (Test-Path "$buildDir\CMakeCache.txt")) {
        $msg = "SKIP: $d — no CMakeCache.txt"
        Write-Host "  $msg"
        $msg | Out-File $OUT -Append
        continue
    }

    cmake --build $buildDir --config $c -j $env:NUMBER_OF_PROCESSORS *>> $OUT
    if ($LASTEXITCODE -eq 0) {
        $msg = "PASS: $d build OK"
        Write-Host "  $msg"
        $msg | Out-File $OUT -Append
    } else {
        $msg = "FAIL: $d build error (exit=$LASTEXITCODE)"
        Write-Host "  $msg"
        $msg | Out-File $OUT -Append
        $allPass = $false
    }
}

"" | Out-File $OUT -Append
if ($allPass) {
    "ALL PASS" | Out-File $OUT -Append
    Write-Host "`nALL PASS"
} else {
    "SOME FAILED — see $OUT" | Out-File $OUT -Append
    Write-Host "`nSOME FAILED"
}
"Done: $(Get-Date)" | Out-File $OUT -Append
