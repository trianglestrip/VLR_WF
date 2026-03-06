@echo off
REM ============================================================================
REM Wavefront Kernels - PTX 编译验证脚本
REM 
REM 编译 wavefront_generate_rays.cu 和 wavefront_trace_rays.cu
REM 需要：VS2022, CUDA Toolkit (nvcc), OptiX SDK (trace_rays 需 VLR_USE_OPTIX)
REM ============================================================================

set SCRIPT_DIR=%~dp0
cd /d %SCRIPT_DIR%

REM 初始化 VS 编译环境（nvcc 依赖 cl.exe）
if exist "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" (
    call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
)

REM 检测 nvcc
where nvcc >nul 2>&1
if %ERRORLEVEL% neq 0 (
    echo [ERROR] nvcc 未找到，请确保 CUDA Toolkit 已安装并加入 PATH
    exit /b 1
)

echo [INFO] 编译 wavefront_generate_rays.cu -^> PTX...
nvcc -ptx -std=c++17 -arch=sm_75 ^
    -I"..\shared" ^
    -I"..\include" ^
    -DVLR_Device ^
    wavefront_generate_rays.cu ^
    -o wavefront_generate_rays.ptx

if %ERRORLEVEL% equ 0 (
    echo [OK] wavefront_generate_rays.ptx 生成成功
) else (
    echo [ERROR] wavefront_generate_rays.cu 编译失败
    exit /b 1
)

REM 可选：编译 TraceRays OptiX 内核（需 OptiX SDK）
REM 设置 OPTIX_PATH 环境变量指向 OptiX 安装目录，例如：C:\ProgramData\NVIDIA Corporation\OptiX SDK 7.x
if defined OPTIX_PATH (
    echo [INFO] 编译 wavefront_trace_rays.cu -^> PTX (OptiX)...
    nvcc -ptx -std=c++17 -arch=sm_75 ^
        -I"..\shared" ^
        -I"..\include" ^
        -I"%OPTIX_PATH%\include" ^
        -DVLR_Device ^
        -DVLR_USE_OPTIX ^
        wavefront_trace_rays.cu ^
        -o wavefront_trace_rays.ptx
    if %ERRORLEVEL% equ 0 (
        echo [OK] wavefront_trace_rays.ptx 生成成功
    ) else (
        echo [WARN] wavefront_trace_rays.cu 编译失败（可忽略，若未使用 OptiX）
    )
) else (
    echo [INFO] 跳过 wavefront_trace_rays.cu（需设置 OPTIX_PATH）
)

exit /b 0
