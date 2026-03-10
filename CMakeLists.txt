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
# C++ 标准：C++20
# ----------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ----------------------------------------------------------------------------
# 配置选项
# ----------------------------------------------------------------------------

# CUDA 计算能力（sm_75=RTX 20系, sm_80=Ampere, sm_86=RTX 30系移动版, sm_89=Ada）
set(VLR_CUDA_ARCH "75" CACHE STRING "CUDA 计算能力架构（如 75, 80, 86, 89）")
set_property(CACHE VLR_CUDA_ARCH PROPERTY STRINGS "75" "80" "86" "89" "90")

# 调试选项：NaN 追踪（启用时在 GPU 内核中打印 NaN 检测信息）
set(VLR_DEBUG_NAN_TRACKING OFF CACHE BOOL "启用 NaN 调试追踪（每帧打印前 5 次 NaN 检测）")

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

# Viewer：3D场景查看器和渲染器前端（使用Assimp加载模型）
add_subdirectory(viewer)

# ----------------------------------------------------------------------------
# 测试可执行文件（仅包含存在的测试文件）
# ----------------------------------------------------------------------------
# 注释掉不存在的测试文件，避免 CMake 配置失败
# add_executable(simple_render_test test/simple_render_test.cpp)
# add_executable(optix_minimal_test test/optix_minimal_test.cpp)
# add_executable(cornell_box_test test/cornell_box_test.cpp)
# add_executable(glass_spheres_test test/glass_spheres_test.cpp)
# add_executable(multi_material_test test/multi_material_test.cpp)

# Cornell Box Improved 测试场景（完整材质）
add_executable(cornell_box_improved_test test/cornell_box_improved_test.cpp)
target_include_directories(cornell_box_improved_test PRIVATE
    ${CMAKE_SOURCE_DIR}/libVLR/include
    ${CMAKE_SOURCE_DIR}/test
)
target_link_libraries(cornell_box_improved_test PRIVATE VLR CUDA::cudart)

# Material System 测试场景（验证各种 BSDF）
add_executable(material_test test/material_test.cpp)
target_include_directories(material_test PRIVATE
    ${CMAKE_SOURCE_DIR}/libVLR/include
    ${CMAKE_SOURCE_DIR}/test
)
target_link_libraries(material_test PRIVATE VLR CUDA::cudart)

# Disney Principled BRDF 测试（多球体展示不同参数组合）
add_executable(disney_brdf_test test/disney_brdf_test.cpp)
target_include_directories(disney_brdf_test PRIVATE
    ${CMAKE_SOURCE_DIR}/libVLR/include
    ${CMAKE_SOURCE_DIR}/test
)
target_link_libraries(disney_brdf_test PRIVATE VLR CUDA::cudart)

# LambertianScattering 次表面散射 BSDF 测试
add_executable(lambertian_scattering_test test/lambertian_scattering_test.cpp)
target_include_directories(lambertian_scattering_test PRIVATE
    ${CMAKE_SOURCE_DIR}/libVLR/include
    ${CMAKE_SOURCE_DIR}/test
)
target_link_libraries(lambertian_scattering_test PRIVATE VLR CUDA::cudart)

# Anisotropic MicrofacetReflection 测试（各向异性金属材质）
add_executable(anisotropic_test test/anisotropic_test.cpp)
target_include_directories(anisotropic_test PRIVATE
    ${CMAKE_SOURCE_DIR}/libVLR/include
    ${CMAKE_SOURCE_DIR}/test
)
target_link_libraries(anisotropic_test PRIVATE VLR CUDA::cudart)

# MultiSurface Material 测试（2-4 层混合材质）
add_executable(multi_surface_test test/multi_surface_test.cpp)
target_include_directories(multi_surface_test PRIVATE
    ${CMAKE_SOURCE_DIR}/libVLR/include
    ${CMAKE_SOURCE_DIR}/test
)
target_link_libraries(multi_surface_test PRIVATE VLR CUDA::cudart)

# Texture System 测试（纹理加载、程序化纹理、Cornell Box）
add_executable(texture_test test/texture_test.cpp)
target_include_directories(texture_test PRIVATE
    ${CMAKE_SOURCE_DIR}/libVLR/include
    ${CMAKE_SOURCE_DIR}/test
)
target_link_libraries(texture_test PRIVATE VLR CUDA::cudart)

# Light System 测试（区域光、点光源、方向光、环境光、混合光源）
add_executable(light_system_test test/light_system_test.cpp)
target_include_directories(light_system_test PRIVATE
    ${CMAKE_SOURCE_DIR}/libVLR/include
    ${CMAKE_SOURCE_DIR}/test
)
target_link_libraries(light_system_test PRIVATE VLR CUDA::cudart)

# VLR_WF vs VLR 对比测试
add_executable(vlr_wf_vs_vlr_test test/vlr_wf_vs_vlr_test.cpp)
target_include_directories(vlr_wf_vs_vlr_test PRIVATE
    ${CMAKE_SOURCE_DIR}/libVLR/include
    ${CMAKE_SOURCE_DIR}/test
)
target_link_libraries(vlr_wf_vs_vlr_test PRIVATE VLR CUDA::cudart)

# 简单区域光测试(修复版)
add_executable(simple_area_light_test_fixed test/simple_area_light_test_fixed.cpp)
target_include_directories(simple_area_light_test_fixed PRIVATE
    ${CMAKE_SOURCE_DIR}/libVLR/include
    ${CMAKE_SOURCE_DIR}/test
)
target_link_libraries(simple_area_light_test_fixed PRIVATE VLR CUDA::cudart)

# 玻璃和金属材质测试
add_executable(glass_metal_test test/glass_metal_test.cpp)
target_include_directories(glass_metal_test PRIVATE
    ${CMAKE_SOURCE_DIR}/libVLR/include
    ${CMAKE_SOURCE_DIR}/test
)
target_link_libraries(glass_metal_test PRIVATE VLR CUDA::cudart)

# ----------------------------------------------------------------------------
# Viewer 示例程序（如果Viewer库可用）
# ----------------------------------------------------------------------------
if(TARGET Viewer)
    # 简单的Viewer测试程序
    add_executable(simple_viewer_test viewer/examples/simple_viewer_test.cpp)
    target_include_directories(simple_viewer_test PRIVATE
        ${CMAKE_SOURCE_DIR}/viewer/include
        ${CMAKE_SOURCE_DIR}/libVLR/include
    )
    target_link_libraries(simple_viewer_test PRIVATE Viewer VLR CUDA::cudart)
    
    # 零拷贝性能基准测试（需要直接链接Assimp）
    set(ASSIMP_LIB_RELEASE "${CMAKE_SOURCE_DIR}/viewer/external/assimp/lib/assimp-vc143-mt.lib")
    if(EXISTS "${ASSIMP_LIB_RELEASE}")
        add_executable(zero_copy_benchmark viewer/examples/zero_copy_benchmark.cpp)
        target_include_directories(zero_copy_benchmark PRIVATE
            ${CMAKE_SOURCE_DIR}/viewer/include
            ${CMAKE_SOURCE_DIR}/viewer/external/assimp/include
        )
        target_link_libraries(zero_copy_benchmark PRIVATE ${ASSIMP_LIB_RELEASE})
        message(STATUS "[VLR] 零拷贝基准测试已启用: zero_copy_benchmark")
    endif()
    
    # Taskflow并行加载性能基准测试
    add_executable(parallel_benchmark viewer/examples/parallel_benchmark.cpp)
    target_include_directories(parallel_benchmark PRIVATE
        ${CMAKE_SOURCE_DIR}/viewer/include
        ${CMAKE_SOURCE_DIR}/libVLR/include
    )
    target_link_libraries(parallel_benchmark PRIVATE Viewer VLR CUDA::cudart)
    
    # Kitchen场景渲染测试（295个PLY网格）
    add_executable(kitchen_render_test viewer/examples/kitchen_render_test.cpp)
    target_include_directories(kitchen_render_test PRIVATE
        ${CMAKE_SOURCE_DIR}/viewer/include
        ${CMAKE_SOURCE_DIR}/libVLR/include
    )
    target_link_libraries(kitchen_render_test PRIVATE Viewer VLR CUDA::cudart)
    
    message(STATUS "[VLR] Viewer测试程序已启用: simple_viewer_test, zero_copy_benchmark, parallel_benchmark, kitchen_render_test")
endif()

# PTX 文件已通过 libVLR 的 add_custom_command 直接输出到 bin/GPU_kernels/
# 无需额外复制
