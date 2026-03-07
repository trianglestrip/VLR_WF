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
# PTX 编译：OptiX 内核（wavefront_trace_rays.cu 含 RG/CH/MS/AH 程序）
# 输出到 bin/GPU_kernels/ 目录
# ----------------------------------------------------------------------------
set(PTX_OUTPUT_DIR ${CMAKE_SOURCE_DIR}/bin/GPU_kernels)
file(MAKE_DIRECTORY ${PTX_OUTPUT_DIR})

if(OPTIX_FOUND)
    add_custom_command(
        OUTPUT ${PTX_OUTPUT_DIR}/wavefront_trace_rays.ptx
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
            ${CMAKE_CURRENT_SOURCE_DIR}/GPU_kernels/wavefront_trace_rays.cu
            -o ${PTX_OUTPUT_DIR}/wavefront_trace_rays.ptx
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/GPU_kernels/wavefront_trace_rays.cu
        COMMENT "编译 wavefront_trace_rays.cu -> PTX (OptiX) -> bin/GPU_kernels/"
        VERBATIM
    )
    add_custom_target(wavefront_trace_rays_ptx ALL
        DEPENDS ${PTX_OUTPUT_DIR}/wavefront_trace_rays.ptx
    )
    message(STATUS "[libVLR] OptiX PTX 目标已启用：wavefront_trace_rays.ptx -> bin/GPU_kernels/")
endif()

# ----------------------------------------------------------------------------
# 可选：生成光线 PTX（wavefront_generate_rays - 当前作为 CUDA 内核链接）
# 若需单独 PTX 可取消注释
# ----------------------------------------------------------------------------
# add_custom_command(
#     OUTPUT ${PTX_OUTPUT_DIR}/wavefront_generate_rays.ptx
#     COMMAND ${CUDAToolkit_NVCC_EXECUTABLE}
#         -ptx -std=c++17 -arch=sm_${VLR_CUDA_ARCH}
#         -DVLR_Device -I${CMAKE_CURRENT_SOURCE_DIR}/shared -I${CMAKE_CURRENT_SOURCE_DIR}/include
#         ${CMAKE_CURRENT_SOURCE_DIR}/GPU_kernels/wavefront_generate_rays.cu
#         -o ${PTX_OUTPUT_DIR}/wavefront_generate_rays.ptx
#     DEPENDS wavefront_generate_rays.cu
#     COMMENT "编译 wavefront_generate_rays.cu -> PTX"
# )

# ----------------------------------------------------------------------------
# C++ 源文件（主机端逻辑）
# ----------------------------------------------------------------------------
set(LIBVLR_CPP_SOURCES
    vlr.cpp
    context.cpp
    scene.cpp
)

# ----------------------------------------------------------------------------
# CUDA 源文件（含设备端内核，编译为对象文件并链接入库）
# ----------------------------------------------------------------------------
set(LIBVLR_CUDA_SOURCES
    GPU_kernels/wavefront_generate_rays.cu
    GPU_kernels/wavefront_process_hits.cu
    GPU_kernels/wavefront_sample_lights.cu
    GPU_kernels/wavefront_sample_bsdf.cu
    GPU_kernels/wavefront_compact.cu
    GPU_kernels/wavefront_accumulate.cu
    GPU_kernels/wavefront_launch.cu
)

# 注意：wavefront_trace_rays.cu 仅编译为 PTX，供 OptiX 运行时加载，不参与库链接

# ----------------------------------------------------------------------------
# 创建 libVLR 库
# ----------------------------------------------------------------------------
add_library(VLR SHARED ${LIBVLR_CPP_SOURCES} ${LIBVLR_CUDA_SOURCES})

# 确保 PTX 在复制前已构建
if(OPTIX_FOUND)
    add_dependencies(VLR wavefront_trace_rays_ptx)
endif()

# 设置目标属性
target_include_directories(VLR PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/shared
)

target_compile_options(VLR PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:/utf-8>
    $<$<COMPILE_LANGUAGE:CUDA>:-DVLR_Device --expt-relaxed-constexpr>
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

# ----------------------------------------------------------------------------
# PTX 输出：构建后将 PTX 复制到库输出目录，便于运行时加载
# ----------------------------------------------------------------------------
if(OPTIX_FOUND)
    add_custom_command(TARGET VLR POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:VLR>/GPU_kernels
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${PTX_OUTPUT_DIR}/wavefront_trace_rays.ptx $<TARGET_FILE_DIR:VLR>/GPU_kernels/
        COMMENT "复制 PTX 到库输出目录"
    )
endif()

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
    install(FILES ${PTX_OUTPUT_DIR}/wavefront_trace_rays.ptx
        DESTINATION bin/GPU_kernels
        OPTIONAL
    )
endif()
