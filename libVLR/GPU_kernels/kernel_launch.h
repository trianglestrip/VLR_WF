// ============================================================================
// VLR Wavefront - Kernel 启动辅助函数声明
//
// 本文件声明 CUDA/OptiX 内核的启动辅助函数，供 Context 调用。
// 实际实现位于 kernel_launch.cu 中（需 nvcc 编译以使用 <<<>>> 语法）。
//
// 作者：VLR 开发团队
// 创建日期：2026-03-07
// 环境：CUDA 13.1, OptiX 8.0.0, VS2022
// ============================================================================

#pragma once

#include "shared/path_types.h"
#include <cuda_runtime.h>
#include <optix.h>

namespace vlr {

// ============================================================================
// CUDA Kernel 启动辅助函数
// ============================================================================

/// 启动 GenerateRays kernel
/// @param d_params 设备端启动参数指针
/// @param width 图像宽度
/// @param height 图像高度
/// @param stream CUDA 流
void launchGenerateRaysKernel(
    shared::WavefrontLaunchParameters* d_params,
    uint32_t width,
    uint32_t height,
    cudaStream_t stream);

/// 启动 ProcessHits kernel
/// @param d_params 设备端启动参数指针
/// @param numActivePaths 活跃路径数量
/// @param stream CUDA 流
void launchProcessHitsKernel(
    shared::WavefrontLaunchParameters* d_params,
    uint32_t numActivePaths,
    cudaStream_t stream);

/// 启动 SampleLights kernel
/// @param d_params 设备端启动参数指针
/// @param numActivePaths 活跃路径数量
/// @param stream CUDA 流
void launchSampleLightsKernel(
    shared::WavefrontLaunchParameters* d_params,
    uint32_t numActivePaths,
    cudaStream_t stream);

/// 启动 SampleBSDF kernel
/// @param d_params 设备端启动参数指针
/// @param numActivePaths 活跃路径数量
/// @param stream CUDA 流
void launchSampleBSDFKernel(
    shared::WavefrontLaunchParameters* d_params,
    uint32_t numActivePaths,
    cudaStream_t stream);

/// 启动 AccumulateResults kernel
/// @param d_params 设备端启动参数指针
/// @param numPaths 总路径数（像素数）
/// @param stream CUDA 流
void launchAccumulateKernel(
    shared::WavefrontLaunchParameters* d_params,
    uint32_t numPaths,
    cudaStream_t stream);

/// 启动 RenderDebugMode kernel（调试渲染，单次采样无多次反弹）
/// @param d_params 设备端启动参数指针
/// @param numPixels 像素总数
/// @param debugMode 调试模式（VLRDebugMode 枚举值）
/// @param stream CUDA 流
void launchRenderDebugModeKernel(
    shared::WavefrontLaunchParameters* d_params,
    uint32_t numPixels,
    uint32_t debugMode,
    cudaStream_t stream);

/// 初始化 RNG 缓冲区（每个像素一个独立的 RNG 状态）
/// @param rngBuffer 设备端 RNG 缓冲区指针
/// @param numPixels 像素数量
/// @param baseSeed 基础种子（通常使用时间戳或帧数）
/// @param stream CUDA 流
void initializeRNGBuffer(
    shared::KernelRNG* rngBuffer,
    uint32_t numPixels,
    uint64_t baseSeed,
    cudaStream_t stream);

#ifdef VLR_DEBUG_NAN_TRACKING
extern __device__ __managed__ unsigned int g_vlrNanPrintCount;
/// 重置 NaN 调试计数器（每帧开始时调用）
void resetNanDebugCount();
#endif

// 通用调试计数器
extern "C" __device__ __managed__ unsigned int g_vlrDebugPrintCount;
extern "C" void resetDebugCount();

}  // namespace vlr
