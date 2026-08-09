$ErrorActionPreference = "Stop"
$win = Get-Content "$env:TEMP\win_tests.txt" -Encoding UTF8
$wsl = Get-Content "C:\Personal\Projects\LibNei\libnei-src\build\wsl_tests.txt"

function Parse-Tests($lines) {
  $result = @()
  $curSuite = ""
  foreach ($line in $lines) {
    $t = $line.Trim()
    if ($t -eq "") { continue }
    if ($line -match '^\S') {
      $curSuite = $t -replace '\.$', ''
    } else {
      $result += "$curSuite.$t"
    }
  }
  return $result
}

$winTests = Parse-Tests $win
$wslTests = Parse-Tests $wsl
"Win tests: $($winTests.Count)"
"WSL tests: $($wslTests.Count)"

$onlyWin = $winTests | Where-Object { $_ -notin $wslTests }
$onlyWsl = $wslTests | Where-Object { $_ -notin $winTests }

"=== Only on Windows ($($onlyWin.Count)) ==="
$onlyWin | ForEach-Object { "  $_" }
"=== Only on WSL ($($onlyWsl.Count)) ==="
$onlyWsl | ForEach-Object { "  $_" }
