// ============================================================================
// VLR Wavefront - Kernel 启动辅助函数实现
//
// 本文件实现 CUDA 内核的启动逻辑，供 Context 调用。
// 须使用 nvcc 编译以支持 <<<>>> kernel 启动语法。
//
// 作者：VLR 开发团队
// 创建日期：2026-03-07
// 环境：CUDA 13.1, OptiX 8.0.0, VS2022
// ============================================================================

#include "wavefront_launch.h"
#include "../shared/wavefront_types.h"
#include "../utils/cuda_util.h"

#include <cuda_runtime.h>

namespace vlr {

// ============================================================================
// CUDA Kernel 外部声明（实现在各 kernel .cu 文件中）
// ============================================================================

extern "C" __global__ void wavefrontGenerateRays(
    shared::WavefrontLaunchParameters* params);

extern "C" __global__ void wavefrontProcessHits(
    shared::WavefrontLaunchParameters* params);

extern "C" __global__ void wavefrontSampleLights(
    shared::WavefrontLaunchParameters* params);

extern "C" __global__ void wavefrontSampleBSDF(
    shared::WavefrontLaunchParameters* params);

extern "C" __global__ void wavefrontAccumulateResults(
    shared::WavefrontLaunchParameters* params);

// ============================================================================
// 内核启动辅助函数实现
// ============================================================================

void launchGenerateRaysKernel(
    shared::WavefrontLaunchParameters* d_params,
    uint32_t width,
    uint32_t height,
    cudaStream_t stream)
{
    // 计算 2D grid/block 尺寸（16x16 块覆盖图像）
    constexpr uint32_t blockWidth = 16;
    constexpr uint32_t blockHeight = 16;
    dim3 blockDim(blockWidth, blockHeight);
    dim3 gridDim(
        (width + blockWidth - 1) / blockWidth,
        (height + blockHeight - 1) / blockHeight);

    wavefrontGenerateRays<<<gridDim, blockDim, 0, stream>>>(d_params);
    CUDA_CHECK(cudaGetLastError());
}

void launchProcessHitsKernel(
    shared::WavefrontLaunchParameters* d_params,
    uint32_t numActivePaths,
    cudaStream_t stream)
{
    constexpr uint32_t blockSize = 256;
    uint32_t gridSize = (numActivePaths + blockSize - 1) / blockSize;
    if (gridSize == 0) return;

    wavefrontProcessHits<<<gridSize, blockSize, 0, stream>>>(d_params);
    CUDA_CHECK(cudaGetLastError());
}

void launchSampleLightsKernel(
    shared::WavefrontLaunchParameters* d_params,
    uint32_t numActivePaths,
    cudaStream_t stream)
{
    constexpr uint32_t blockSize = 256;
    uint32_t gridSize = (numActivePaths + blockSize - 1) / blockSize;
    if (gridSize == 0) return;

    wavefrontSampleLights<<<gridSize, blockSize, 0, stream>>>(d_params);
    CUDA_CHECK(cudaGetLastError());
}

void launchSampleBSDFKernel(
    shared::WavefrontLaunchParameters* d_params,
    uint32_t numActivePaths,
    cudaStream_t stream)
{
    constexpr uint32_t blockSize = 256;
    uint32_t gridSize = (numActivePaths + blockSize - 1) / blockSize;
    if (gridSize == 0) return;

    wavefrontSampleBSDF<<<gridSize, blockSize, 0, stream>>>(d_params);
    CUDA_CHECK(cudaGetLastError());
}

void launchAccumulateKernel(
    shared::WavefrontLaunchParameters* d_params,
    uint32_t numPaths,
    cudaStream_t stream)
{
    constexpr uint32_t blockSize = 256;
    uint32_t gridSize = (numPaths + blockSize - 1) / blockSize;
    if (gridSize == 0) return;

    wavefrontAccumulateResults<<<gridSize, blockSize, 0, stream>>>(d_params);
    CUDA_CHECK(cudaGetLastError());
}

}  // namespace vlr
