<#
.SYNOPSIS
    将指定文件夹下的 C/C++ 源代码文件按文件名排序后，合并到一个 Markdown 文件中。
.PARAMETER SourceFolder
    要扫描的源文件夹路径。
.PARAMETER MarkdownPath
    要输出或追加的 Markdown 文件路径。
#>
param (
    [Parameter(Mandatory = $true, HelpMessage = "请输入源文件夹路径")]
    [string]$SourceFolder,

    [Parameter(Mandatory = $true, HelpMessage = "请输入目标 Markdown 文件路径")]
    [string]$MarkdownPath
)

# 1. 检查源文件夹是否存在
if (-not (Test-Path -Path $SourceFolder -PathType Container)) {
    Write-Error "错误：找不到源文件夹路径 '$SourceFolder'"
    exit
}

# 获取绝对路径，确保后续计算相对路径时准确
$absoluteSourceFolder = (Get-Item $SourceFolder).FullName
$currentDir = Get-Location

Write-Host "开始扫描文件夹: $absoluteSourceFolder"

# 2. 扫描该路径下（含子目录）的 C/C++ 文件
$extensions = @('*.h', '*.c', '*.cpp')
$files = Get-ChildItem -Path $absoluteSourceFolder -Include $extensions -Recurse -File

if ($files.Count -eq 0) {
    Write-Warning "未找到任何 .h, .c 或 .cpp 文件。"
    exit
}

# 3. 根据文件名（BaseName，不含后缀）对路径集合进行排序
# 如果你想包含后缀排序，可以把 BaseName 改为 Name
$sortedFiles = $files | Sort-Object BaseName

Write-Host "找到并排序了 $($sortedFiles.Count) 个文件，正在写入 '$MarkdownPath'..."

# 4. 循环处理文件并追加到 Markdown
foreach ($file in $sortedFiles) {
    # 5.1 计算相对于当前工作路径（Current Working Directory）的相对路径
    # 如果该文件在当前工作路径之外，Resolve-Path -Relative 会自动处理为正确的相对形式
    $relativePath = Resolve-Path -Path $file.FullName -Relative

    # 准备写入的内容
    # 5.1 & 5.3 标题及空行
    $header = "### $relativePath" + "`n"

    # 5.2 & 5.3 代码块及空行
    # 读取文件内容
    $fileContent = Get-Content -Path $file.FullName -Raw

    $codeBlock = '```C++' + "`n" + $fileContent + "`n" + '```' + "`n"

    # 将内容追加到指定的 Markdown 文件中（如果不存在会自动创建）
    # 使用 -Encoding utf8 确保中文注释不会乱码
    $header | Out-File -FilePath $MarkdownPath -Append -Encoding utf8
    $codeBlock | Out-File -FilePath $MarkdownPath -Append -Encoding utf8

    # 5.4 输出当前处理的文件路径
    Write-Host "已处理: $relativePath" -ForegroundColor Cyan
}

Write-Host "操作完成！成功合并到 $MarkdownPath" -ForegroundColor Green