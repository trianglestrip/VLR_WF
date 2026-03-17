// ============================================================================
// Tonemap Kernel
//
// 将 SpectrumStorage (float r,g,b 累积值) 转换为 RGBA8 用于 OpenGL 显示。
// 支持 ACES Filmic / Reinhard tonemap + gamma 校正。
// ============================================================================

#include <cuda_runtime.h>
#include <stdint.h>
#include <cstdio>

// ACES Filmic tonemap (Narkowicz 2015 fit)
// Better color preservation and highlight rolloff than Reinhard
__device__ __forceinline__ float ACESFilmic(float x) {
    const float a = 2.51f;
    const float b = 0.03f;
    const float c = 2.43f;
    const float d = 0.59f;
    const float e = 0.14f;
    return fminf(fmaxf((x * (a * x + b)) / (x * (c * x + d) + e), 0.0f), 1.0f);
}

__global__ void tonemapKernel(
    const float* __restrict__ accumBuf,
    uint8_t* __restrict__ outRGBA8,
    uint32_t numPixels,
    float invFrames,
    float exposure,
    float invGamma)
{
    uint32_t idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= numPixels) return;

    float r = accumBuf[idx * 3 + 0] * invFrames * exposure;
    float g = accumBuf[idx * 3 + 1] * invFrames * exposure;
    float b = accumBuf[idx * 3 + 2] * invFrames * exposure;

    // ACES Filmic tonemap
    r = ACESFilmic(r);
    g = ACESFilmic(g);
    b = ACESFilmic(b);

    // Gamma correction
    r = powf(fmaxf(r, 0.0f), invGamma);
    g = powf(fmaxf(g, 0.0f), invGamma);
    b = powf(fmaxf(b, 0.0f), invGamma);

    outRGBA8[idx * 4 + 0] = static_cast<uint8_t>(fminf(r * 255.0f + 0.5f, 255.0f));
    outRGBA8[idx * 4 + 1] = static_cast<uint8_t>(fminf(g * 255.0f + 0.5f, 255.0f));
    outRGBA8[idx * 4 + 2] = static_cast<uint8_t>(fminf(b * 255.0f + 0.5f, 255.0f));
    outRGBA8[idx * 4 + 3] = 255;
}

extern "C"
void launchTonemapKernel(
    const float* accumBuf,
    uint8_t* outRGBA8,
    uint32_t numPixels,
    float invFrames,
    float exposure,
    float invGamma,
    cudaStream_t stream)
{
    constexpr uint32_t blockSize = 256;
    uint32_t numBlocks = (numPixels + blockSize - 1) / blockSize;
    tonemapKernel<<<numBlocks, blockSize, 0, stream>>>(
        accumBuf, outRGBA8, numPixels, invFrames, exposure, invGamma);
}

__global__ void scaleBufferKernel(
    const float* __restrict__ src,
    float* __restrict__ dst,
    uint32_t numFloats,
    float scale)
{
    uint32_t idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= numFloats) return;
    dst[idx] = src[idx] * scale;
}

extern "C"
void launchScaleBufferKernel(
    const float* src, float* dst,
    uint32_t numPixels, float scale,
    cudaStream_t stream)
{
    uint32_t numFloats = numPixels * 3;
    constexpr uint32_t blockSize = 256;
    uint32_t numBlocks = (numFloats + blockSize - 1) / blockSize;
    scaleBufferKernel<<<numBlocks, blockSize, 0, stream>>>(
        src, dst, numFloats, scale);
}
