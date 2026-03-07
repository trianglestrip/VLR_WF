# ============================================================================
# VLR Wavefront 路径追踪 - 主 CMake 构建文件
#
# 项目：VLR Wavefront Path Tracing
# 环境：CUDA 13.1, OptiX 8.0.0, VS2022
# 作者：VLR 开发团队
# 创建：2026-03-07
# ============================================================================

cmake_minimum_required(VERSION 3.18)
project(VLR_Wavefront
    VERSION 1.0.0
    LANGUAGES CXX CUDA
    DESCRIPTION "VLR Wavefront Path Tracing Renderer"
)

# ----------------------------------------------------------------------------
# 构建类型（Debug/Release）
# ----------------------------------------------------------------------------
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "构建类型 [Debug|Release|RelWithDebInfo|MinSizeRel]" FORCE)
endif()

# ----------------------------------------------------------------------------
# C++ 标准：C++17
# ----------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ----------------------------------------------------------------------------
# 配置选项
# ----------------------------------------------------------------------------

# CUDA 计算能力（sm_75=RTX 20系, sm_80=Ampere, sm_86=RTX 30系移动版, sm_89=Ada）
set(VLR_CUDA_ARCH "75" CACHE STRING "CUDA 计算能力架构（如 75, 80, 86, 89）")
set_property(CACHE VLR_CUDA_ARCH PROPERTY STRINGS "75" "80" "86" "89" "90")

# OptiX SDK 路径（默认：ProgramData 安装位置）
set(OPTIX_PATH "C:/ProgramData/NVIDIA Corporation/OptiX SDK 8.0.0" CACHE PATH "OptiX SDK 安装路径")
if(DEFINED ENV{OPTIX_PATH})
    set(OPTIX_PATH "$ENV{OPTIX_PATH}" CACHE PATH "OptiX SDK 路径（来自环境变量）" FORCE)
endif()

# ----------------------------------------------------------------------------
# 查找 CUDA
# ----------------------------------------------------------------------------
find_package(CUDAToolkit REQUIRED)

if(CUDAToolkit_FOUND)
    message(STATUS "[VLR] CUDA 版本: ${CUDAToolkit_VERSION}")
    message(STATUS "[VLR] CUDA 架构: sm_${VLR_CUDA_ARCH}")
endif()

# ----------------------------------------------------------------------------
# 查找 OptiX
# ----------------------------------------------------------------------------
if(EXISTS "${OPTIX_PATH}/include/optix.h")
    set(OPTIX_FOUND TRUE)
    set(OPTIX_INCLUDE_DIR "${OPTIX_PATH}/include")
    message(STATUS "[VLR] OptiX 路径: ${OPTIX_PATH}")
else()
    set(OPTIX_FOUND FALSE)
    message(WARNING "[VLR] 未找到 OptiX SDK，请设置 OPTIX_PATH 或安装 OptiX 8.0.0")
endif()

# ----------------------------------------------------------------------------
# 设置输出目录：所有可执行文件和库输出到 bin/
# ----------------------------------------------------------------------------
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)

# 为多配置生成器（如 Visual Studio）设置
foreach(OUTPUTCONFIG ${CMAKE_CONFIGURATION_TYPES})
    string(TOUPPER ${OUTPUTCONFIG} OUTPUTCONFIG)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${OUTPUTCONFIG} ${CMAKE_SOURCE_DIR}/bin)
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${OUTPUTCONFIG} ${CMAKE_SOURCE_DIR}/bin)
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${OUTPUTCONFIG} ${CMAKE_SOURCE_DIR}/bin)
endforeach()

# ----------------------------------------------------------------------------
# 添加子目录（OPTIX_FOUND、OPTIX_PATH、VLR_CUDA_ARCH 将传递给 libVLR）
# ----------------------------------------------------------------------------
add_subdirectory(libVLR)

# ----------------------------------------------------------------------------
# 简单渲染测试可执行文件
# ----------------------------------------------------------------------------
add_executable(simple_render_test test/simple_render_test.cpp)
target_include_directories(simple_render_test PRIVATE ${CMAKE_SOURCE_DIR}/libVLR/include)
target_link_libraries(simple_render_test PRIVATE VLR CUDA::cudart)

# OptiX 最小测试
add_executable(optix_minimal_test test/optix_minimal_test.cpp)
target_include_directories(optix_minimal_test PRIVATE 
    ${OPTIX_PATH}/include
    ${CMAKE_CUDA_TOOLKIT_INCLUDE_DIRECTORIES})
target_link_libraries(optix_minimal_test PRIVATE CUDA::cudart CUDA::cuda_driver)

# PTX 文件已通过 libVLR 的 add_custom_command 直接输出到 bin/GPU_kernels/
# 无需额外复制
