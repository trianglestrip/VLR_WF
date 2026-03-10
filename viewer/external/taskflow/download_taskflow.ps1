# ============================================================================
# Taskflow 下载脚本
# 
# Taskflow是header-only库，只需下载头文件即可
# ============================================================================

$ErrorActionPreference = "Stop"

$TASKFLOW_VERSION = "v3.8.0"
$TASKFLOW_URL = "https://github.com/taskflow/taskflow/archive/refs/tags/$TASKFLOW_VERSION.zip"
$DOWNLOAD_DIR = "$PSScriptRoot\temp"
$INSTALL_DIR = $PSScriptRoot

Write-Host "=== Taskflow 下载脚本 ===" -ForegroundColor Cyan
Write-Host "版本: $TASKFLOW_VERSION" -ForegroundColor Cyan
Write-Host "类型: Header-Only (无需编译)" -ForegroundColor Green
Write-Host ""

# 1. 创建临时目录
Write-Host "[1/4] 创建临时目录..." -ForegroundColor Yellow
New-Item -ItemType Directory -Path $DOWNLOAD_DIR -Force | Out-Null

# 2. 下载Taskflow
$zipFile = "$DOWNLOAD_DIR\taskflow.zip"
if (Test-Path $zipFile) {
    Write-Host "[2/4] 源码已存在，跳过下载" -ForegroundColor Green
} else {
    Write-Host "[2/4] 下载Taskflow..." -ForegroundColor Yellow
    Write-Host "URL: $TASKFLOW_URL" -ForegroundColor Gray
    try {
        Invoke-WebRequest -Uri $TASKFLOW_URL -OutFile $zipFile -UseBasicParsing
        Write-Host "下载完成!" -ForegroundColor Green
    } catch {
        Write-Host "下载失败: $_" -ForegroundColor Red
        exit 1
    }
}

# 3. 解压
Write-Host "[3/4] 解压源码..." -ForegroundColor Yellow
$extractDir = "$DOWNLOAD_DIR\taskflow-source"
if (Test-Path $extractDir) {
    Remove-Item -Path $extractDir -Recurse -Force
}
Expand-Archive -Path $zipFile -DestinationPath $DOWNLOAD_DIR -Force

# 找到解压后的目录
$extractedFolder = Get-ChildItem -Path $DOWNLOAD_DIR -Directory | Where-Object { $_.Name -like "taskflow-*" } | Select-Object -First 1
if ($extractedFolder) {
    Move-Item -Path $extractedFolder.FullName -Destination $extractDir -Force
}

# 4. 复制头文件
Write-Host "[4/4] 复制头文件..." -ForegroundColor Yellow
$sourceInclude = "$extractDir\taskflow"
$targetInclude = "$INSTALL_DIR\include\taskflow"

if (Test-Path $targetInclude) {
    Remove-Item -Path $targetInclude -Recurse -Force
}

Copy-Item -Path $sourceInclude -Destination "$INSTALL_DIR\include\" -Recurse -Force
Write-Host "头文件复制完成!" -ForegroundColor Green

# 清理
Write-Host "清理临时文件..." -ForegroundColor Yellow
Remove-Item -Path $DOWNLOAD_DIR -Recurse -Force -ErrorAction SilentlyContinue

Write-Host ""
Write-Host "=== 完成! ===" -ForegroundColor Green
Write-Host "Taskflow已安装到: $INSTALL_DIR\include\taskflow" -ForegroundColor Green
Write-Host ""
Write-Host "使用方法:" -ForegroundColor Cyan
Write-Host '  #include <taskflow/taskflow.hpp>' -ForegroundColor Gray
Write-Host ""
Write-Host "Taskflow是header-only库，无需编译!" -ForegroundColor Green
