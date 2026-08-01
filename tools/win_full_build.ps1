# win_full_build.ps1 — 4-quadrant Windows build + test
$ErrorActionPreference = "Continue"
$SRC = "c:\Personal\Projects\LibNei\libnei-src"
$BUILD_BASE = "$SRC\build"
$RESULTS = "$BUILD_BASE\win_results.txt"

# Clean old builds
Remove-Item -Recurse -Force "$BUILD_BASE\win-*" -ErrorAction SilentlyContinue

"=== Windows 4-Quadrant Build & Test ===" | Out-File $RESULTS
"Started: $(Get-Date)" | Out-File $RESULTS -Append
"" | Out-File $RESULTS -Append

$summaryFile = "$env:TEMP\win_results_summary.txt"
"" | Out-File $summaryFile

function Build-And-Test {
    param($Name, $BuildType, $Shared)
    $Dir = "$BUILD_BASE\win-$Name"
    $SharedLabel = if ($Shared -eq "ON") { "shared" } else { "static" }
    "" | Out-File $RESULTS -Append
    "========== $Name ($BuildType / $SharedLabel) ==========" | Out-File $RESULTS -Append

    # Configure
    "  Configuring..." | Out-File $RESULTS -Append
    $cmakeArgs = @("-S", $SRC, "-B", $Dir, "-DCMAKE_BUILD_TYPE=$BuildType", "-DBUILD_SHARED_LIBS=$Shared")
    cmake @cmakeArgs >> $RESULTS 2>&1
    if ($LASTEXITCODE -ne 0) {
        "  CONFIGURE FAILED" | Out-File $RESULTS -Append
        "FAIL:$Name" | Out-File $summaryFile -Append
        return
    }

    # Build
    "  Building..." | Out-File $RESULTS -Append
    cmake --build $Dir --config $BuildType -j $env:NUMBER_OF_PROCESSORS >> $RESULTS 2>&1
    if ($LASTEXITCODE -ne 0) {
        "  BUILD FAILED" | Out-File $RESULTS -Append
        "FAIL:$Name" | Out-File $summaryFile -Append
        return
    }

    # Test
    "  Testing..." | Out-File $RESULTS -Append
    Push-Location $Dir
    $testOutput = ctest --output-on-failure -C $BuildType 2>&1
    $testOutput | Out-File $RESULTS -Append
    Pop-Location

    if ($LASTEXITCODE -eq 0) {
        "PASS:$Name" | Out-File $summaryFile -Append
    } else {
        "FAIL:$Name" | Out-File $summaryFile -Append
    }
}

# 4 quadrants
Build-AndTest "debug-shared"  "Debug"    "ON"
Build-AndTest "debug-static"  "Debug"    "OFF"
Build-AndTest "rel-shared"    "Release"  "ON"
Build-AndTest "rel-static"    "Release"  "OFF"

# Summary
"" | Out-File $RESULTS -Append
"========== SUMMARY ==========" | Out-File $RESULTS -Append
Get-Content $summaryFile | Out-File $RESULTS -Append
"" | Out-File $RESULTS -Append
"Finished: $(Get-Date)" | Out-File $RESULTS -Append
Write-Host "=== Windows Results ==="
Get-Content $summaryFile
Write-Host "Full results: $RESULTS"
