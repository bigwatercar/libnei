<#
.SYNOPSIS
    将指定文件夹或文件下的 C/C++ 源代码按文件名排序后，合并到一个 Markdown 文件中。
.PARAMETER SourcePath
    要扫描的源文件夹路径，或单个源文件路径。
    如果是文件夹，会递归扫描其中的 .h/.c/.cpp 文件并按名称排序输出。
    如果是文件，则直接处理该单个文件。
.PARAMETER MarkdownPath
    要输出或追加的 Markdown 文件路径。
.PARAMETER Clear
    如果指定此开关，会在写入前先清空目标 Markdown 文件（默认行为是追加）。
#>
param (
    [Parameter(Mandatory = $true, HelpMessage = "请输入源文件夹或源文件路径")]
    [string]$SourcePath,

    [Parameter(Mandatory = $true, HelpMessage = "请输入目标 Markdown 文件路径")]
    [string]$MarkdownPath,

    [Parameter(Mandatory = $false)]
    [switch]$Clear
)

# 0. 如果指定了 -Clear，先清空目标文件
if ($Clear -and (Test-Path -Path $MarkdownPath)) {
    Remove-Item -Path $MarkdownPath -Force
    Write-Host "已清空目标文件: $MarkdownPath"
}

# 1. 检查源路径是否存在
if (-not (Test-Path -Path $SourcePath)) {
    Write-Error "错误：找不到源路径 '$SourcePath'"
    exit
}

# 获取绝对路径，确保后续计算相对路径时准确
$absoluteSourcePath = (Get-Item $SourcePath).FullName

# 2. 判断是文件还是文件夹，收集待处理文件列表
if (Test-Path -Path $absoluteSourcePath -PathType Leaf) {
    # --- 单文件模式 ---
    $isSingleFile = $true
    $extension = [System.IO.Path]::GetExtension($absoluteSourcePath)
    $validExtensions = @('.h', '.c', '.cpp')
    if ($extension -notin $validExtensions) {
        Write-Warning "文件 '$absoluteSourcePath' 不是 .h/.c/.cpp 文件，仍然继续处理。"
    }
    $filesToProcess = @(Get-Item $absoluteSourcePath)
    Write-Host "处理单个文件: $absoluteSourcePath"
}
else {
    # --- 文件夹模式（保持原有流程）---
    $isSingleFile = $false
    Write-Host "开始扫描文件夹: $absoluteSourcePath"

    $extensions = @('*.h', '*.c', '*.cpp')
    $files = Get-ChildItem -Path $absoluteSourcePath -Include $extensions -Recurse -File

    if ($files.Count -eq 0) {
        Write-Warning "未找到任何 .h, .c 或 .cpp 文件。"
        exit
    }

    # 根据文件名（BaseName，不含后缀）对路径集合进行排序
    # 如果你想包含后缀排序，可以把 BaseName 改为 Name
    $filesToProcess = $files | Sort-Object BaseName

    Write-Host "找到并排序了 $($filesToProcess.Count) 个文件，正在写入 '$MarkdownPath'..."
}

# 3. 循环处理文件并追加到 Markdown
foreach ($file in $filesToProcess) {
    # 计算相对于当前工作路径（Current Working Directory）的相对路径
    # 如果该文件在当前工作路径之外，Resolve-Path -Relative 会自动处理为正确的相对形式
    $relativePath = Resolve-Path -Path $file.FullName -Relative

    # 准备写入的内容
    # 标题及空行
    $header = "### $relativePath" + "`n"

    # 代码块及空行
    # 读取文件内容
    $fileContent = Get-Content -Path $file.FullName -Raw

    $codeBlock = '```C++' + "`n" + $fileContent + "`n" + '```' + "`n"

    # 将内容追加到指定的 Markdown 文件中（如果不存在会自动创建）
    # 使用 -Encoding utf8 确保中文注释不会乱码
    $header | Out-File -FilePath $MarkdownPath -Append -Encoding utf8
    $codeBlock | Out-File -FilePath $MarkdownPath -Append -Encoding utf8

    # 输出当前处理的文件路径
    Write-Host "已处理: $relativePath" -ForegroundColor Cyan
}

Write-Host "操作完成！成功合并到 $MarkdownPath" -ForegroundColor Green