// ============================================================================
// VLR Wavefront - Kernel ????????
//
// ??????CUDA ????????? Context ????
// ????nvcc ??????<<<>>> kernel ??????
//
// ???VLR ?????
// ??????026-03-07
// ???CUDA 13.1, OptiX 8.0.0, VS2022
// ============================================================================

#include "kernel_launch.h"
#include "../shared/performance_config.h"

#ifdef VLR_DEBUG_NAN_TRACKING
__device__ __managed__ unsigned int g_vlrNanPrintCount = 0;

void resetNanDebugCount() {
    g_vlrNanPrintCount = 0;
}
#endif

// ????????????VLR_DEBUG_NAN_TRACKING ??
extern "C" __device__ __managed__ unsigned int g_vlrDebugPrintCount = 0;

extern "C" void resetDebugCount() {
    g_vlrDebugPrintCount = 0;
}
#include "../utils/cuda_util.h"

#include <cuda_runtime.h>

namespace vlr {

// ============================================================================
// CUDA Kernel ??????????kernel .cu ????
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

extern "C" __global__ void generateLightPaths(
    shared::WavefrontLaunchParameters* params);

extern "C" __global__ void processLightHits(
    shared::WavefrontLaunchParameters* params);

extern "C" __global__ void applyShadowRayResults(
    shared::WavefrontLaunchParameters* params);

// ============================================================================
// ??????????
// ============================================================================

void launchGenerateRaysKernel(
    shared::WavefrontLaunchParameters* d_params,
    uint32_t width,
    uint32_t height,
    cudaStream_t stream)
{
    // ????????block ??
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
    // ????????block size
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
    // ????????block size
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
    // ????????block size
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
    // ????????block size
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

void launchGenerateLightPathsKernel(
    shared::WavefrontLaunchParameters* d_params,
    uint32_t numLightPaths,
    cudaStream_t stream)
{
    constexpr uint32_t blockSize = 256;
    uint32_t gridSize = (numLightPaths + blockSize - 1) / blockSize;
    if (gridSize == 0) return;
    generateLightPaths<<<gridSize, blockSize, 0, stream>>>(d_params);
    CUDA_CHECK(cudaGetLastError());
}

void launchProcessLightHitsKernel(
    shared::WavefrontLaunchParameters* d_params,
    uint32_t numLightPaths,
    cudaStream_t stream)
{
    constexpr uint32_t blockSize = 256;
    uint32_t gridSize = (numLightPaths + blockSize - 1) / blockSize;
    if (gridSize == 0) return;
    processLightHits<<<gridSize, blockSize, 0, stream>>>(d_params);
    CUDA_CHECK(cudaGetLastError());
}

void launchApplyShadowRayResultsKernel(
    shared::WavefrontLaunchParameters* d_params,
    uint32_t maxRequests,
    cudaStream_t stream)
{
    constexpr uint32_t blockSize = 256;
    uint32_t gridSize = (maxRequests + blockSize - 1) / blockSize;
    if (gridSize == 0) return;
    applyShadowRayResults<<<gridSize, blockSize, 0, stream>>>(d_params);
    CUDA_CHECK(cudaGetLastError());
}

// ============================================================================
// RNG ????Kernel
// ============================================================================

__global__ void initializeRNGKernel(
    shared::KernelRNG* rngBuffer,
    uint32_t numPixels,
    uint64_t baseSeed)
{
    uint32_t idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= numPixels) return;

    // ???????????RNG ??
    // ?? PCG32 ??????
    uint64_t seed = baseSeed + idx;
    rngBuffer[idx].state = seed ^ 0xda3e39cb94b95bdbULL;
    rngBuffer[idx].inc = (idx << 1u) | 1u;  // ??????inc ????????
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
