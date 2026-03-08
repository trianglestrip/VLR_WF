// ============================================================================
// VLR Wavefront - AccumulateResults Kernel
//
// 本文件实现 Wavefront 路径追踪的结果累积内核。
// 功能：贡献值累积、RNG 状态更新、Denoiser 缓冲区处理、帧计数器逻辑。
//
// 参考：docs/wavefront_design.md § 4.7 Kernel 7: AccumulateResults
//
// 作者：VLR 开发团队
// 创建日期：2026-03-07
// 环境：CUDA 13.1, OptiX 8.0.0, VS2022
// ============================================================================

#define VLR_DEBUG_ACCUMULATE 1

#include "../shared/path_types.h"
#include "../include/vlr/basic_types.h"
#include "../shared/kernel_common.h"
#include "kernel_launch.h"

#include <cuda_runtime.h>
#include <cmath>

namespace {

using namespace vlr;
using namespace vlr::shared;

}  // anonymous namespace

// ============================================================================
// AccumulateResults Kernel
// ============================================================================
// 将本轮渲染的所有路径贡献值累加到输出缓冲区，并更新 RNG 状态。
// 每个像素对应一条路径，pathIndex = pixelY * imageSize.x + pixelX。

extern "C" __global__ void accumulateResults(
    vlr::shared::WavefrontLaunchParameters* params) {
    using namespace vlr::shared;
    WavefrontLaunchParameters& wlp = *params;

#ifdef __CUDACC__
    // 一维启动：每个线程处理一个像素/路径
    uint32_t pathIndex = blockIdx.x * blockDim.x + threadIdx.x;
    uint32_t totalPaths = wlp.imageSize.x * wlp.imageSize.y;

    if (pathIndex >= totalPaths)
        return;

    WavefrontPathState& pathState = wlp.pathStateBuffer[pathIndex];

    // ========================================================================
    // 1. 贡献值有效性检查
    // ========================================================================
    // 若贡献值含 NaN/Inf，则跳过累积，避免污染输出
    if (!pathState.contribution.allFinite()) {
#ifdef VLR_DEBUG_NAN_TRACKING
        {
            unsigned int idx = atomicAdd(&g_vlrNanPrintCount, 1);
            if (idx < 5) {
                printf("[NaN] accumulate(pre): px=(%u,%u) pathLen=%u contribution=(%.4f,%.4f,%.4f) skipped\n",
                       pathState.pixelX, pathState.pixelY, pathState.pathLength,
                       pathState.contribution.values[0], pathState.contribution.values[1], pathState.contribution.values[2]);
            }
        }
#endif
        return;
    }

    // 像素线性索引
    uint32_t stride = (wlp.imageStrideInPixels > 0) ? wlp.imageStrideInPixels : wlp.imageSize.x;
    uint32_t pixelIdx = pathState.pixelY * stride + pathState.pixelX;

    // ========================================================================
    // 2. 贡献值累积到 accumBuffer
    // ========================================================================
    SpectrumStorage* accum = wlp.accumBuffer.data;
    if (accum == nullptr)
        return;

    // 首帧时重置累积缓冲区（与原始 VLR path_tracing.cu 一致）
    if (wlp.numAccumFrames == 1) {
        accum[pixelIdx].r = 0.0f;
        accum[pixelIdx].g = 0.0f;
        accum[pixelIdx].b = 0.0f;
    }

    // 将光谱贡献转换为 RGB 并累加（与原始 VLR 一致：始终 add，无 hasNonZero 条件）
    // Wavefront 架构下每像素对应一条路径，无并发写，使用直接加法即可
    DiscretizedSpectrum contrib = pathState.contribution.toDiscretizedSpectrum(pathState.wls);
    
#ifdef VLR_DEBUG_ACCUMULATE
    if (pathIndex == 0) {
        printf("[GPU Accumulate] pathIndex=0: contribution=(%.6f,%.6f,%.6f,%.6f), RGB=(%.6f,%.6f,%.6f)\n",
            pathState.contribution.values[0], pathState.contribution.values[1],
            pathState.contribution.values[2], pathState.contribution.values[3],
            contrib.r, contrib.g, contrib.b);
    }
#endif
    
    accum[pixelIdx].r += contrib.r;
    accum[pixelIdx].g += contrib.g;
    accum[pixelIdx].b += contrib.b;

    // 若累加结果含 NaN/Inf（理论上不应发生，因 contribution 已通过 allFinite 检查），
    // 重置为 0 避免显示洋红色/黑色异常
#ifdef __CUDACC__
    bool hadNaN = __isnanf(accum[pixelIdx].r) || __isinf(accum[pixelIdx].r) ||
                  __isnanf(accum[pixelIdx].g) || __isinf(accum[pixelIdx].g) ||
                  __isnanf(accum[pixelIdx].b) || __isinf(accum[pixelIdx].b);
    if (hadNaN) {
#ifdef VLR_DEBUG_NAN_TRACKING
        {
            unsigned int idx = atomicAdd(&g_vlrNanPrintCount, 1);
            if (idx < 5) {
                printf("[NaN] accumulate(post): px=(%u,%u) accum=(%.4f,%.4f,%.4f) after add, reset to 0\n",
                       pathState.pixelX, pathState.pixelY,
                       accum[pixelIdx].r, accum[pixelIdx].g, accum[pixelIdx].b);
            }
        }
#endif
        accum[pixelIdx].r = 0.0f;
        accum[pixelIdx].g = 0.0f;
        accum[pixelIdx].b = 0.0f;
    }
#else
    if (!std::isfinite(accum[pixelIdx].r))
        accum[pixelIdx].r = 0.0f;
    if (!std::isfinite(accum[pixelIdx].g))
        accum[pixelIdx].g = 0.0f;
    if (!std::isfinite(accum[pixelIdx].b))
        accum[pixelIdx].b = 0.0f;
#endif

    // ========================================================================
    // 3. 更新 RNG 状态到 rngBuffer
    // ========================================================================
    // 保存当前路径的 RNG 状态，供下一帧 GenerateRays 使用
    KernelRNG* rngBuf = wlp.rngBuffer.data;
    if (rngBuf != nullptr) {
        rngBuf[pixelIdx] = pathState.rng;
    }

    // ========================================================================
    // 4. Denoiser 辅助缓冲区
    // ========================================================================
    // 反照率（accumAlbedoBuffer）和法线（accumNormalBuffer）由 ProcessHits
    // 在首次命中时写入，AccumulateResults 此处无需额外处理。
    // 若需对 Denoiser 缓冲区做逐样本累积（如多帧平均），可在此扩展。

    // ========================================================================
    // 5. 帧计数器
    // ========================================================================
    // numAccumFrames 由主机在每帧结束后更新；本 kernel 仅根据其值决定是否
    // 重置 accumBuffer（numAccumFrames == 1 表示新序列首帧）。
#endif
}
