[CmdletBinding()]
param(
    [string]$QtRoot = "D:\qt\6.11.2\mingw_64",
    [string]$MinGwRoot = "D:\qt\Tools\mingw1310_64",
    [string]$CMakeRoot = "D:\qt\Tools\CMake_64",
    [string]$BuildDirectory = "build\windows-release",
    [string]$OutputDirectory = "dist\WhqMusic"
)

$ErrorActionPreference = "Stop"

$projectRoot = Split-Path -Parent $PSScriptRoot
$buildPath = Join-Path $projectRoot $BuildDirectory
$outputPath = Join-Path $projectRoot $OutputDirectory
$cmakePath = Join-Path $QtRoot "bin\qt-cmake.bat"
$cmakeExecutablePath = Join-Path $CMakeRoot "bin\cmake.exe"
$deployToolPath = Join-Path $QtRoot "bin\windeployqt.exe"
$makePath = Join-Path $MinGwRoot "bin\mingw32-make.exe"
$cCompilerPath = Join-Path $MinGwRoot "bin\gcc.exe"
$cxxCompilerPath = Join-Path $MinGwRoot "bin\g++.exe"
$executablePath = Join-Path $buildPath "WhqMusic.exe"

$requiredTools = @(
    $cmakePath,
    $cmakeExecutablePath,
    $deployToolPath,
    $makePath,
    $cCompilerPath,
    $cxxCompilerPath
)

foreach ($requiredTool in $requiredTools) {
    if (-not (Test-Path -LiteralPath $requiredTool -PathType Leaf)) {
        throw "未找到打包所需工具：$requiredTool"
    }
}

$env:PATH = "$(Split-Path -Parent $cmakeExecutablePath);$(Split-Path -Parent $makePath);$env:PATH"

New-Item -ItemType Directory -Force -Path $buildPath | Out-Null

& $cmakePath `
    -S $projectRoot `
    -B $buildPath `
    -G "MinGW Makefiles" `
    -DCMAKE_BUILD_TYPE=Release `
    "-DCMAKE_MAKE_PROGRAM=$makePath" `
    "-DCMAKE_C_COMPILER=$cCompilerPath" `
    "-DCMAKE_CXX_COMPILER=$cxxCompilerPath"

if ($LASTEXITCODE -ne 0) {
    throw "CMake 配置失败，退出码：$LASTEXITCODE"
}

& $cmakePath --build $buildPath --config Release --parallel

if ($LASTEXITCODE -ne 0) {
    throw "Release 编译失败，退出码：$LASTEXITCODE"
}

if (-not (Test-Path -LiteralPath $executablePath -PathType Leaf)) {
    throw "编译完成后未找到程序：$executablePath"
}

New-Item -ItemType Directory -Force -Path $outputPath | Out-Null
Get-ChildItem -LiteralPath $outputPath -Force | Remove-Item -Recurse -Force
Copy-Item -LiteralPath $executablePath -Destination $outputPath

& $deployToolPath `
    --release `
    --compiler-runtime `
    --no-translations `
    --dir $outputPath `
    (Join-Path $outputPath "WhqMusic.exe")

if ($LASTEXITCODE -ne 0) {
    throw "Qt 运行依赖部署失败，退出码：$LASTEXITCODE"
}

# The streaming decoder loads FFmpeg directly, so deploy its runtime explicitly.
foreach ($library in @("avutil-59.dll", "swresample-5.dll", "avcodec-61.dll", "avformat-61.dll")) {
    $libraryPath = Join-Path $buildPath $library
    if (-not (Test-Path -LiteralPath $libraryPath -PathType Leaf)) {
        throw "未找到实时解码依赖：$libraryPath"
    }
    Copy-Item -LiteralPath $libraryPath -Destination $outputPath -Force
}
Copy-Item -LiteralPath (Join-Path $buildPath "licenses") -Destination $outputPath -Recurse -Force

Write-Host "打包完成：$outputPath" -ForegroundColor Green
Write-Host "双击 WhqMusic.exe 即可运行。发布时请复制整个 WhqMusic 文件夹。"
