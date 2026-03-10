# ============================================================================
# Assimp 下载和构建脚本
# 
# 用途：自动下载并编译Assimp库
# 使用：在PowerShell中运行此脚本
# ============================================================================

$ErrorActionPreference = "Stop"

$ASSIMP_VERSION = "v5.4.3"
$ASSIMP_URL = "https://github.com/assimp/assimp/archive/refs/tags/$ASSIMP_VERSION.zip"
$DOWNLOAD_DIR = "$PSScriptRoot\temp"
$BUILD_DIR = "$PSScriptRoot\build"
$INSTALL_DIR = $PSScriptRoot

Write-Host "=== Assimp 自动构建脚本 ===" -ForegroundColor Cyan
Write-Host "版本: $ASSIMP_VERSION" -ForegroundColor Cyan
Write-Host ""

# 1. 创建临时目录
Write-Host "[1/6] 创建临时目录..." -ForegroundColor Yellow
New-Item -ItemType Directory -Path $DOWNLOAD_DIR -Force | Out-Null
New-Item -ItemType Directory -Path $BUILD_DIR -Force | Out-Null

# 2. 下载Assimp源码
$zipFile = "$DOWNLOAD_DIR\assimp.zip"
if (Test-Path $zipFile) {
    Write-Host "[2/6] 源码已存在，跳过下载" -ForegroundColor Green
} else {
    Write-Host "[2/6] 下载Assimp源码..." -ForegroundColor Yellow
    Write-Host "URL: $ASSIMP_URL" -ForegroundColor Gray
    try {
        Invoke-WebRequest -Uri $ASSIMP_URL -OutFile $zipFile -UseBasicParsing
        Write-Host "下载完成!" -ForegroundColor Green
    } catch {
        Write-Host "下载失败: $_" -ForegroundColor Red
        exit 1
    }
}

# 3. 解压
Write-Host "[3/6] 解压源码..." -ForegroundColor Yellow
$extractDir = "$DOWNLOAD_DIR\assimp-source"
if (Test-Path $extractDir) {
    Remove-Item -Path $extractDir -Recurse -Force
}
Expand-Archive -Path $zipFile -DestinationPath $DOWNLOAD_DIR -Force

# 重命名解压后的目录
$extractedFolder = Get-ChildItem -Path $DOWNLOAD_DIR -Directory | Where-Object { $_.Name -like "assimp-*" } | Select-Object -First 1
if ($extractedFolder) {
    Move-Item -Path $extractedFolder.FullName -Destination $extractDir -Force
}

# 4. 配置CMake
Write-Host "[4/6] 配置CMake..." -ForegroundColor Yellow
Push-Location $BUILD_DIR
try {
    cmake "$extractDir" `
        -DCMAKE_INSTALL_PREFIX="$INSTALL_DIR" `
        -DBUILD_SHARED_LIBS=ON `
        -DASSIMP_BUILD_TESTS=OFF `
        -DASSIMP_BUILD_ASSIMP_TOOLS=OFF `
        -DASSIMP_BUILD_SAMPLES=OFF `
        -DASSIMP_INSTALL_PDB=OFF `
        -DASSIMP_NO_EXPORT=ON `
        -DASSIMP_BUILD_ALL_IMPORTERS_BY_DEFAULT=OFF `
        -DASSIMP_BUILD_OBJ_IMPORTER=ON `
        -DASSIMP_BUILD_FBX_IMPORTER=ON `
        -DASSIMP_BUILD_GLTF_IMPORTER=ON `
        -DASSIMP_BUILD_COLLADA_IMPORTER=ON `
        -DASSIMP_BUILD_3DS_IMPORTER=ON `
        -DASSIMP_BUILD_BLEND_IMPORTER=ON `
        -DASSIMP_BUILD_PLY_IMPORTER=ON `
        -DASSIMP_BUILD_STL_IMPORTER=ON `
        -G "Visual Studio 17 2022" `
        -A x64
    
    if ($LASTEXITCODE -ne 0) {
        throw "CMake配置失败"
    }
    Write-Host "CMake配置成功!" -ForegroundColor Green
} catch {
    Write-Host "CMake配置失败: $_" -ForegroundColor Red
    Pop-Location
    exit 1
}
Pop-Location

# 5. 编译
Write-Host "[5/6] 编译Assimp (Release)..." -ForegroundColor Yellow
Write-Host "这可能需要几分钟..." -ForegroundColor Gray
cmake --build $BUILD_DIR --config Release --target install

if ($LASTEXITCODE -ne 0) {
    Write-Host "编译失败!" -ForegroundColor Red
    exit 1
}

Write-Host "编译完成!" -ForegroundColor Green

# 6. 清理临时文件
Write-Host "[6/6] 清理临时文件..." -ForegroundColor Yellow
Remove-Item -Path $DOWNLOAD_DIR -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item -Path $BUILD_DIR -Recurse -Force -ErrorAction SilentlyContinue

Write-Host ""
Write-Host "=== 完成! ===" -ForegroundColor Green
Write-Host "Assimp已安装到: $INSTALL_DIR" -ForegroundColor Green
Write-Host ""
Write-Host "目录结构:" -ForegroundColor Cyan
Write-Host "  - include/assimp/  (头文件)" -ForegroundColor Gray
Write-Host "  - lib/             (导入库)" -ForegroundColor Gray
Write-Host "  - bin/             (DLL文件)" -ForegroundColor Gray
Write-Host ""
Write-Host "现在可以运行 cmake 配置项目了!" -ForegroundColor Green
