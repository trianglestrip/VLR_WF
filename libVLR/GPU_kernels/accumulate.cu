// ============================================================================
// VLR Wavefront - AccumulateResults Kernel
//
// ??????Wavefront ?????????????
// ?????????RNG ?????Denoiser ??????????????
//
// ???docs/wavefront_design.md ? 4.7 Kernel 7: AccumulateResults
//
// ???VLR ?????
// ??????026-03-07
// ???CUDA 13.1, OptiX 8.0.0, VS2022
// ============================================================================

#define VLR_DEBUG_ACCUMULATE 0

#ifndef VLR_DEBUG_SPEC_TRANS_ONEPIX
#define VLR_DEBUG_SPEC_TRANS_ONEPIX 0
#endif
#ifndef VLR_DEBUG_SPEC_TRANS_PX
#define VLR_DEBUG_SPEC_TRANS_PX 320
#endif
#ifndef VLR_DEBUG_SPEC_TRANS_PY
#define VLR_DEBUG_SPEC_TRANS_PY 84
#endif

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
// ??????????????????????????RNG ????
// ???????????pathIndex = pixelY * imageSize.x + pixelX??

extern "C" __global__ void accumulateResults(
    vlr::shared::WavefrontLaunchParameters* params) {
    using namespace vlr::shared;
    WavefrontLaunchParameters& wlp = *params;

#ifdef __CUDACC__
    // ??????????????????
    uint32_t pathIndex = blockIdx.x * blockDim.x + threadIdx.x;
    uint32_t totalPaths = wlp.imageSize.x * wlp.imageSize.y;

    if (pathIndex >= totalPaths)
        return;

    WavefrontPathState& pathState = wlp.pathStateBuffer[pathIndex];

    // ========================================================================
    // 1. ?????????
    // ========================================================================
    // ????? NaN/Inf??????????????
    if (!pathState.contribution.allFinite()) {
#ifdef VLR_DEBUG_NAN_TRACKING
        {
            unsigned int idx = atomicAdd(&g_vlrNanPrintCount, 1);
            if (idx < 5) {
                VLR_DEBUG_PRINTF("[NaN] accumulate(pre): px=(%u,%u) pathLen=%u contribution=(%.4f,%.4f,%.4f) skipped\n",
                       pathState.pixelX, pathState.pixelY, pathState.pathLength,
                       pathState.contribution.values[0], pathState.contribution.values[1], pathState.contribution.values[2]);
            }
        }
#endif
        return;
    }

    // ???????
    uint32_t stride = (wlp.imageStrideInPixels > 0) ? wlp.imageStrideInPixels : wlp.imageSize.x;
    uint32_t pixelIdx = pathState.pixelY * stride + pathState.pixelX;

    // ========================================================================
    // 2. ?????? accumBuffer
    // ========================================================================
    SpectrumStorage* accum = wlp.accumBuffer.data;
    if (accum == nullptr)
        return;

    // ?????????????? VLR path_tracing.cu ???
    if (wlp.numAccumFrames == 1) {
        accum[pixelIdx].r = 0.0f;
        accum[pixelIdx].g = 0.0f;
        accum[pixelIdx].b = 0.0f;
    }

    // ???????? RGB ????????VLR ????? add?? hasNonZero ????
    // Wavefront ???????????????????????????
    DiscretizedSpectrum contrib = pathState.contribution.toDiscretizedSpectrum(pathState.wls);

#if VLR_DEBUG_SPEC_TRANS_ONEPIX
    if (wlp.numAccumFrames <= 2) {
        if (pathState.pixelX == VLR_DEBUG_SPEC_TRANS_PX && pathState.pixelY == VLR_DEBUG_SPEC_TRANS_PY) {
            printf("[AccumDbg] GLASS px=(%u,%u) frame=%u pathLen=%u contrib=(%.6g,%.6g,%.6g)\n",
                pathState.pixelX, pathState.pixelY, wlp.numAccumFrames, pathState.pathLength,
                pathState.contribution.values[0], pathState.contribution.values[1], pathState.contribution.values[2]);
        }
        if (pathState.pixelX == 256 && pathState.pixelY == 84) {
            printf("[AccumDbg] WALL  px=(%u,%u) frame=%u pathLen=%u contrib=(%.6g,%.6g,%.6g)\n",
                pathState.pixelX, pathState.pixelY, wlp.numAccumFrames, pathState.pathLength,
                pathState.contribution.values[0], pathState.contribution.values[1], pathState.contribution.values[2]);
        }
    }
#endif

#ifdef VLR_DEBUG_ACCUMULATE
    if (pathIndex == 0) {
        VLR_DEBUG_PRINTF("[GPU Accumulate] pathIndex=0: contribution=(%.6f,%.6f,%.6f,%.6f), RGB=(%.6f,%.6f,%.6f)\n",
            pathState.contribution.values[0], pathState.contribution.values[1],
            pathState.contribution.values[2], pathState.contribution.values[3],
            contrib.r, contrib.g, contrib.b);
    }
#endif
    
    accum[pixelIdx].r += contrib.r;
    accum[pixelIdx].g += contrib.g;
    accum[pixelIdx].b += contrib.b;

    // ?????? NaN/Inf?????????? contribution ??? allFinite ?????
    // ????0 ????????????
#ifdef __CUDACC__
    bool hadNaN = __isnanf(accum[pixelIdx].r) || __isinf(accum[pixelIdx].r) ||
                  __isnanf(accum[pixelIdx].g) || __isinf(accum[pixelIdx].g) ||
                  __isnanf(accum[pixelIdx].b) || __isinf(accum[pixelIdx].b);
    if (hadNaN) {
#ifdef VLR_DEBUG_NAN_TRACKING
        {
            unsigned int idx = atomicAdd(&g_vlrNanPrintCount, 1);
            if (idx < 5) {
                VLR_DEBUG_PRINTF("[NaN] accumulate(post): px=(%u,%u) accum=(%.4f,%.4f,%.4f) after add, reset to 0\n",
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
    // 3. ?? RNG ??? rngBuffer
    // ========================================================================
    // ????????RNG ????????GenerateRays ??
    KernelRNG* rngBuf = wlp.rngBuffer.data;
    if (rngBuf != nullptr) {
        rngBuf[pixelIdx] = pathState.rng;
    }

    // ========================================================================
    // 4. Denoiser ??????
    // ========================================================================
    // ????accumAlbedoBuffer?????accumNormalBuffer?? ProcessHits
    // ?????????AccumulateResults ??????????
    // ????Denoiser ????????????????????????

    // ========================================================================
    // 5. ????
    // ========================================================================
    // numAccumFrames ??????????????kernel ??????????
    // ?? accumBuffer?numAccumFrames == 1 ??????????
#endif
}
