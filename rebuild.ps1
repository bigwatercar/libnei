Set-Location -Path "c:\Personal\Projects\LibNei\libnei-src"
if (-not (Test-Path "temp")) { New-Item -ItemType Directory -Path "temp" }
$sb = New-Object System.Text.StringBuilder
$hFiles = Get-ChildItem -Path "modules/neixx/task" -Filter "*.h" -Recurse | Sort-Object FullName
$cppFiles = Get-ChildItem -Path "modules/neixx/task" -Filter "*.cpp" -Recurse | Sort-Object FullName
foreach ($file in ($hFiles + $cppFiles)) {
    $rel = $file.FullName.Substring("c:\Personal\Projects\LibNei\libnei-src\".Length).Replace("\", "/")
    [void]$sb.AppendLine($rel)
    [void]$sb.AppendLine("```cpp")
    [void]$sb.AppendLine(([System.IO.File]::ReadAllText($file.FullName)).TrimEnd())
    [void]$sb.AppendLine("```")
    [void]$sb.AppendLine("")
}
[System.IO.File]::WriteAllText("temp/neixx.md", $sb.ToString(), [System.Text.Encoding]::UTF8)
$fi = Get-Item "temp/neixx.md"
$cnt = (Select-String -Path "temp/neixx.md" -Pattern "^neixx/").Count
Write-Host "1) Size: $($fi.Length)"
Write-Host "2) Matches: $cnt"
Write-Host "3) Head:"
Get-Content "temp/neixx.md" -Head 8
