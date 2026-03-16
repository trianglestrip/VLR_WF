# ============================================================================
# libVLR - Wavefront 路径追踪核心库
#
# 构建内容：
# 1. PTX 文件：OptiX 光线追踪程序（编译 .cu 为 PTX）
# 2. libVLR 库：C++ 源文件 + CUDA 内核（链接为静态/动态库）
#
# 作者：VLR 开发团队
# 创建：2026-03-07
# ============================================================================

# ----------------------------------------------------------------------------
# 包含目录
# ----------------------------------------------------------------------------
set(LIBVLR_INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/shared
    ${CUDAToolkit_INCLUDE_DIRS}
)

if(OPTIX_FOUND)
    list(APPEND LIBVLR_INCLUDE_DIRS ${OPTIX_INCLUDE_DIR})
endif()

# ----------------------------------------------------------------------------
# PTX 编译：OptiX 内核（trace_rays.cu 含 RG/CH/MS/AH 程序）
# 输出到 bin/GPU_kernels/ 目录
# ----------------------------------------------------------------------------
set(PTX_OUTPUT_DIR ${CMAKE_SOURCE_DIR}/bin/GPU_kernels)
file(MAKE_DIRECTORY ${PTX_OUTPUT_DIR})

if(OPTIX_FOUND)
    add_custom_command(
        OUTPUT ${PTX_OUTPUT_DIR}/trace_rays.ptx
        COMMAND ${CUDAToolkit_NVCC_EXECUTABLE}
            -ptx
            -std=c++17
            -arch=sm_${VLR_CUDA_ARCH}
            --expt-relaxed-constexpr
            -DVLR_Device
            -DVLR_USE_OPTIX
            -DVLR_PTX_TRACERAYS
            -D_WIN64
            -I${CMAKE_CURRENT_SOURCE_DIR}/shared
            -I${CMAKE_CURRENT_SOURCE_DIR}/include
            -I${OPTIX_INCLUDE_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/GPU_kernels/trace_rays.cu
            -o ${PTX_OUTPUT_DIR}/trace_rays.ptx
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/GPU_kernels/trace_rays.cu
        COMMENT "编译 trace_rays.cu -> PTX (OptiX) -> bin/GPU_kernels/"
        VERBATIM
    )
    add_custom_target(trace_rays_ptx ALL
        DEPENDS ${PTX_OUTPUT_DIR}/trace_rays.ptx
    )
    message(STATUS "[libVLR] OptiX PTX 目标已启用：trace_rays.ptx -> bin/GPU_kernels/")
endif()

# ----------------------------------------------------------------------------
# 可选：生成光线 PTX（generate_rays - 当前作为 CUDA 内核链接）
# 若需单独 PTX 可取消注释
# ----------------------------------------------------------------------------
# add_custom_command(
#     OUTPUT ${PTX_OUTPUT_DIR}/generate_rays.ptx
#     COMMAND ${CUDAToolkit_NVCC_EXECUTABLE}
#         -ptx -std=c++17 -arch=sm_${VLR_CUDA_ARCH}
#         -DVLR_Device -I${CMAKE_CURRENT_SOURCE_DIR}/shared -I${CMAKE_CURRENT_SOURCE_DIR}/include
#         ${CMAKE_CURRENT_SOURCE_DIR}/GPU_kernels/generate_rays.cu
#         -o ${PTX_OUTPUT_DIR}/generate_rays.ptx
#     DEPENDS generate_rays.cu
#     COMMENT "编译 generate_rays.cu -> PTX"
# )

# ----------------------------------------------------------------------------
# C++ 源文件（主机端逻辑）
# ----------------------------------------------------------------------------
set(LIBVLR_CPP_SOURCES
    vlr.cpp
    context.cpp
    context_optix.cpp
    context_texture.cpp
    context_wavefront.cpp
    scene.cpp
    image_loader.cpp
    env_importance.cpp
    tinyexr_impl.cpp
    denoiser.cpp
)

# ----------------------------------------------------------------------------
# CUDA 源文件（含设备端内核，编译为对象文件并链接入库）
# ----------------------------------------------------------------------------
set(LIBVLR_CUDA_SOURCES
    GPU_kernels/generate_rays.cu
    GPU_kernels/process_hits.cu
    GPU_kernels/sample_lights.cu
    GPU_kernels/sample_bsdf.cu
    GPU_kernels/light_path.cu
    GPU_kernels/compact.cu
    GPU_kernels/accumulate.cu
    GPU_kernels/debug_rendering.cu
    GPU_kernels/kernel_launch.cu
)

# 注意：trace_rays.cu 仅编译为 PTX，供 OptiX 运行时加载，不参与库链接

# ----------------------------------------------------------------------------
# 创建 libVLR 库
# ----------------------------------------------------------------------------
add_library(VLR SHARED ${LIBVLR_CPP_SOURCES} ${LIBVLR_CUDA_SOURCES})

# 确保 PTX 在复制前已构建
if(OPTIX_FOUND)
    add_dependencies(VLR trace_rays_ptx)
endif()

# Taskflow 头文件库路径（Header-Only）
set(TASKFLOW_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/external/taskflow")

# 设置目标属性
target_include_directories(VLR PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/shared
    ${CMAKE_SOURCE_DIR}/external
    ${CMAKE_SOURCE_DIR}/external/tinyexr
    ${CMAKE_SOURCE_DIR}/external/tinyexr/deps/miniz
    ${TASKFLOW_INCLUDE_DIR}
)

target_compile_options(VLR PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:/utf-8>
    $<$<COMPILE_LANGUAGE:CUDA>:-DVLR_Device --expt-relaxed-constexpr -Xcompiler=/utf-8>
)

# 解决 LIBCMT 冲突：强制使用动态运行时库
set_target_properties(VLR PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
)

# 显式忽略 LIBCMT
target_link_options(VLR PRIVATE
    $<$<CONFIG:Release>:/NODEFAULTLIB:LIBCMT>
    $<$<CONFIG:Debug>:/NODEFAULTLIB:LIBCMTD>
)

# CUDA 架构
set_target_properties(VLR PROPERTIES
    CUDA_ARCHITECTURES ${VLR_CUDA_ARCH}
    CUDA_SEPARABLE_COMPILATION ON
)

# 链接 CUDA 运行时
target_link_libraries(VLR PUBLIC
    CUDA::cudart
    CUDA::cuda_driver
)

# OptiX：仅头文件，无链接库（OptiX 8 为头文件库）
if(OPTIX_FOUND)
    target_include_directories(VLR PRIVATE ${OPTIX_INCLUDE_DIR})
    target_compile_definitions(VLR PRIVATE VLR_USE_OPTIX)
endif()

# NaN 调试追踪（cmake -DVLR_DEBUG_NAN_TRACKING=ON）
if(VLR_DEBUG_NAN_TRACKING)
    target_compile_definitions(VLR PRIVATE VLR_DEBUG_NAN_TRACKING)
    message(STATUS "[libVLR] VLR_DEBUG_NAN_TRACKING 已启用")
endif()

# 快速 RNG（cmake -DVLR_USE_FAST_RNG=ON）
if(VLR_USE_FAST_RNG)
    target_compile_definitions(VLR PRIVATE VLR_USE_FAST_RNG)
    message(STATUS "[libVLR] VLR_USE_FAST_RNG 已启用（使用 xorshift RNG）")
endif()

# 场景准备性能统计（cmake -DVLR_PROFILE_SCENE_PREPARE=ON）
if(VLR_PROFILE_SCENE_PREPARE)
    target_compile_definitions(VLR PRIVATE VLR_PROFILE_SCENE_PREPARE)
    message(STATUS "[libVLR] VLR_PROFILE_SCENE_PREPARE 已启用（输出场景准备各阶段耗时）")
endif()

# ----------------------------------------------------------------------------
# PTX 输出：已通过 PTX_OUTPUT_DIR 直接输出到 bin/GPU_kernels/，无需复制
# ----------------------------------------------------------------------------
# PTX 文件已通过 add_custom_command 直接输出到 ${CMAKE_SOURCE_DIR}/bin/GPU_kernels/
# 运行时从该目录加载即可

# ----------------------------------------------------------------------------
# 安装规则
# ----------------------------------------------------------------------------
install(TARGETS VLR
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/vlr
    DESTINATION include
    FILES_MATCHING PATTERN "*.h"
)

# 安装 PTX 到 lib/vlr 或 bin/GPU_kernels
if(OPTIX_FOUND)
    install(FILES ${PTX_OUTPUT_DIR}/trace_rays.ptx
        DESTINATION bin/GPU_kernels
        OPTIONAL
    )
endif()
