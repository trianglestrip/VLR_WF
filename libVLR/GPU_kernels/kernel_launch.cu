// ============================================================================
// VLR Wavefront - Kernel 启动辅助函数实现
//
// 本文件实�?CUDA 内核的启动逻辑，供 Context 调用�?
// 须使�?nvcc 编译以支�?<<<>>> kernel 启动语法�?
//
// 作者：VLR 开发团�?
// 创建日期�?026-03-07
// 环境：CUDA 13.1, OptiX 8.0.0, VS2022
// ============================================================================

#include "kernel_launch.h"
#include "../shared/performance_config.h"

#ifdef VLR_DEBUG_NAN_TRACKING
__device__ __managed__ unsigned int g_vlrNanPrintCount = 0;

void resetNanDebugCount() {
    g_vlrNanPrintCount = 0;
}
#endif

// 通用调试计数器（不需�?VLR_DEBUG_NAN_TRACKING 宏）
extern "C" __device__ __managed__ unsigned int g_vlrDebugPrintCount = 0;

extern "C" void resetDebugCount() {
    g_vlrDebugPrintCount = 0;
}
#include "../utils/cuda_util.h"

#include <cuda_runtime.h>

namespace vlr {

// ============================================================================
// CUDA Kernel 外部声明（实现在�?kernel .cu 文件中）
// ============================================================================

extern "C" __global__ void generateRays(
    shared::WavefrontLaunchParameters* params);

extern "C" __global__ void processHits(
    shared::WavefrontLaunchParameters* params);

extern "C" __global__ void sampleLights(
    shared::WavefrontLaunchParameters* params);

extern "C" __global__ void sampleBSDF(
    shared::WavefrontLaunchParameters* params);

extern "C" __global__ void accumulateResults(
    shared::WavefrontLaunchParameters* params);

extern "C" __global__ void renderDebugMode(
    shared::WavefrontLaunchParameters* params,
    uint32_t debugMode);

// ============================================================================
// 内核启动辅助函数实现
// ============================================================================

void launchGenerateRaysKernel(
    shared::WavefrontLaunchParameters* d_params,
    uint32_t width,
    uint32_t height,
    cudaStream_t stream)
{
    // 使用性能配置�?block 尺寸
    constexpr uint32_t blockWidth = shared::PerformanceConfig::GenerateRaysBlockWidth;
    constexpr uint32_t blockHeight = shared::PerformanceConfig::GenerateRaysBlockHeight;
    dim3 blockDim(blockWidth, blockHeight);
    dim3 gridDim(
        (width + blockWidth - 1) / blockWidth,
        (height + blockHeight - 1) / blockHeight);

    generateRays<<<gridDim, blockDim, 0, stream>>>(d_params);
    CUDA_CHECK(cudaGetLastError());
}

void launchProcessHitsKernel(
    shared::WavefrontLaunchParameters* d_params,
    uint32_t numActivePaths,
    cudaStream_t stream)
{
    // 使用性能配置�?block size
    constexpr uint32_t blockSize = shared::PerformanceConfig::ProcessHitsBlockSize;
    uint32_t gridSize = (numActivePaths + blockSize - 1) / blockSize;
    if (gridSize == 0) return;

    processHits<<<gridSize, blockSize, 0, stream>>>(d_params);
    CUDA_CHECK(cudaGetLastError());
}

void launchSampleLightsKernel(
    shared::WavefrontLaunchParameters* d_params,
    uint32_t numActivePaths,
    cudaStream_t stream)
{
    // 使用性能配置�?block size
    constexpr uint32_t blockSize = shared::PerformanceConfig::SampleLightsBlockSize;
    uint32_t gridSize = (numActivePaths + blockSize - 1) / blockSize;
    if (gridSize == 0) return;

    sampleLights<<<gridSize, blockSize, 0, stream>>>(d_params);
    CUDA_CHECK(cudaGetLastError());
}

void launchSampleBSDFKernel(
    shared::WavefrontLaunchParameters* d_params,
    uint32_t numActivePaths,
    cudaStream_t stream)
{
    // 使用性能配置�?block size
    constexpr uint32_t blockSize = shared::PerformanceConfig::SampleBSDFBlockSize;
    uint32_t gridSize = (numActivePaths + blockSize - 1) / blockSize;
    if (gridSize == 0) return;

    sampleBSDF<<<gridSize, blockSize, 0, stream>>>(d_params);
    CUDA_CHECK(cudaGetLastError());
}

void launchAccumulateKernel(
    shared::WavefrontLaunchParameters* d_params,
    uint32_t numPaths,
    cudaStream_t stream)
{
    // 使用性能配置�?block size
    constexpr uint32_t blockSize = shared::PerformanceConfig::AccumulateBlockSize;
    uint32_t gridSize = (numPaths + blockSize - 1) / blockSize;
    if (gridSize == 0) return;

    accumulateResults<<<gridSize, blockSize, 0, stream>>>(d_params);
    CUDA_CHECK(cudaGetLastError());
}

void launchRenderDebugModeKernel(
    shared::WavefrontLaunchParameters* d_params,
    uint32_t numPixels,
    uint32_t debugMode,
    cudaStream_t stream)
{
    constexpr uint32_t blockSize = 256;
    uint32_t gridSize = (numPixels + blockSize - 1) / blockSize;
    if (gridSize == 0) return;

    renderDebugMode<<<gridSize, blockSize, 0, stream>>>(d_params, debugMode);
    CUDA_CHECK(cudaGetLastError());
}

// ============================================================================
// RNG 初始�?Kernel
// ============================================================================

__global__ void initializeRNGKernel(
    shared::KernelRNG* rngBuffer,
    uint32_t numPixels,
    uint64_t baseSeed)
{
    uint32_t idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= numPixels) return;

    // 为每个像素生成唯一�?RNG 种子
    // 使用 PCG32 的初始化方式
    uint64_t seed = baseSeed + idx;
    rngBuffer[idx].state = seed ^ 0xda3e39cb94b95bdbULL;
    rngBuffer[idx].inc = (idx << 1u) | 1u;  // 每个像素�?inc 必须是奇数且不同
}

void initializeRNGBuffer(
    shared::KernelRNG* rngBuffer,
    uint32_t numPixels,
    uint64_t baseSeed,
    cudaStream_t stream)
{
    constexpr uint32_t blockSize = 256;
    uint32_t gridSize = (numPixels + blockSize - 1) / blockSize;
    if (gridSize == 0) return;

    initializeRNGKernel<<<gridSize, blockSize, 0, stream>>>(rngBuffer, numPixels, baseSeed);
    CUDA_CHECK(cudaGetLastError());
}

}  // namespace vlr
