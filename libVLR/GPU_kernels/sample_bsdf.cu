// ============================================================================
// VLR Wavefront - SampleBSDF Kernel
//
// ??????Wavefront ??????BSDF ??????
// ???BSDF ??????BSDFQuery?????????????????
//       ???????????PathState?????????????
//
// ???VLR ?????
// ??????026-03-07
// ???CUDA 13.1, OptiX 8.0.0, VS2022
// ============================================================================

#include "../shared/kernel_common.h"
#include "../shared/path_types.h"
#include "kernel_launch.h"
#include "../shared/bsdf_common.h"
#include "../shared/geometry_common.h"
#include "../shared/material_types.h"
#include "../shared/texture_types.h"
#include "../shared/performance_config.h"

// ? constexpr ?????????????
// ????????,??WavefrontLaunchParameters???????
#define VLR_USE_MATERIAL_CACHE 0
#include "../include/vlr/basic_types.h"
#include "warp_utils.cuh"
#include "shared_memory_cache.cuh"

#include <cuda_runtime.h>
#include <cmath>

#ifndef VLR_DEBUG_SPEC_TRANS_ONEPIX
#define VLR_DEBUG_SPEC_TRANS_ONEPIX 0
#endif
#ifndef VLR_DEBUG_SPEC_TRANS_PX
#define VLR_DEBUG_SPEC_TRANS_PX 320
#endif
#ifndef VLR_DEBUG_SPEC_TRANS_PY
#define VLR_DEBUG_SPEC_TRANS_PY 84
#endif

namespace {

using namespace vlr;
using namespace vlr::shared;

}  // anonymous namespace

// ============================================================================
// SampleBSDF Kernel
// ============================================================================
// ??????????????BSDF ???????????
// ???????????????????????
// ?????ProcessHits -> SampleLights -> SampleBSDF

extern "C" __global__ void sampleBSDF(
    vlr::shared::WavefrontLaunchParameters* params) {
    using namespace vlr::shared;
    WavefrontLaunchParameters& wlp = *params;

#ifdef __CUDACC__

#if VLR_USE_MATERIAL_CACHE
    __shared__ MaterialCache<PerformanceConfig::MaterialCacheSize> materialCache;
    if (threadIdx.x == 0) {
        // ??:?????N???
        uint32_t numToCache = PerformanceConfig::MaterialCacheSize;
        for (uint32_t i = 0; i < numToCache; ++i) {
            materialCache.materials[i] = wlp.materialDescriptorBuffer[i];
        }
        materialCache.numMaterials = numToCache;
    }
    __syncthreads();
#endif

    // ??????????????????
    uint32_t workIndex = blockIdx.x * blockDim.x + threadIdx.x;
    if (workIndex >= wlp.activePathQueue.size())
        return;

    uint32_t pathIndex = wlp.activePathQueue.pathIndices[workIndex];
    
    // ??????__restrict__ ????????????
    WavefrontPathState* __restrict__ pathStatePtr = &wlp.pathStateBuffer[pathIndex];
    WavefrontPathState& pathState = *pathStatePtr;

    // ???warp-level ?????
    // ???? warp ??????????
    bool isActive = pathState.isActive();
    if (warpAllInactive(isActive))
        return;
    
    // ????????
    if (!isActive)
        return;

    const WavefrontHitInfo* __restrict__ hitInfoPtr = &wlp.hitInfoBuffer[pathIndex];
    const WavefrontHitInfo& hitInfo = *hitInfoPtr;
    if (!hitInfo.hasHit() || hitInfo.hitInfinity())
        return;

    const SurfacePoint* __restrict__ surfPtPtr = &wlp.surfacePointBuffer[pathIndex];
    const SurfacePoint& surfPt = *surfPtPtr;

    // ========================================================================
    // 1. ??????BSDF ????
    // ========================================================================
    const GeometryInstance& geomInst = wlp.geomInstBuffer[hitInfo.geomInstIndex];

#if VLR_USE_MATERIAL_CACHE
    const SurfaceMaterialDescriptor& matDesc = *materialCache.get(
        geomInst.materialIndex, wlp.materialDescriptorBuffer);
#else
    const SurfaceMaterialDescriptor& matDesc = wlp.materialDescriptorBuffer[geomInst.materialIndex];
#endif

#ifdef VLR_DEBUG_MATERIAL
    if (pathIndex < 10) {
        BSDFType bsdfType = getBSDFType(matDesc);
        const float* dataAsFloat = getMaterialDataAsFloats(matDesc);
        VLR_DEBUG_PRINTF("[GPU SampleBSDF] pathIndex=%u, geomInstIndex=%u, materialIndex=%u\n",
            pathIndex, hitInfo.geomInstIndex, geomInst.materialIndex);
        VLR_DEBUG_PRINTF("  bsdfProcedureSetIndex=%u, getBSDFType()=%u\n",
            matDesc.bsdfProcedureSetIndex, (uint32_t)bsdfType);
        VLR_DEBUG_PRINTF("  Albedo: (%.3f, %.3f, %.3f)\n",
            dataAsFloat[MaterialDataLayout::AlbedoR],
            dataAsFloat[MaterialDataLayout::AlbedoG],
            dataAsFloat[MaterialDataLayout::AlbedoB]);
        VLR_DEBUG_PRINTF("  Roughness: %.3f, IOR: %.3f\n",
            dataAsFloat[MaterialDataLayout::Roughness],
            dataAsFloat[MaterialDataLayout::IOR]);
        VLR_DEBUG_PRINTF("  Eta: (%.3f, %.3f, %.3f)\n",
            dataAsFloat[MaterialDataLayout::EtaR],
            dataAsFloat[MaterialDataLayout::EtaG],
            dataAsFloat[MaterialDataLayout::EtaB]);
        VLR_DEBUG_PRINTF("  Kappa: (%.3f, %.3f, %.3f)\n",
            dataAsFloat[MaterialDataLayout::KappaR],
            dataAsFloat[MaterialDataLayout::KappaG],
            dataAsFloat[MaterialDataLayout::KappaB]);
    }
#endif

    // ???BSDF ?????????????????????
    // ???? = ??????????= -pathState.direction
    Vector3D dirInLocal = surfPt.shadingFrame.toLocal(
        Vector3D(-pathState.direction.x, -pathState.direction.y, -pathState.direction.z));
    Normal3D geomNormalLocal = surfPt.shadingFrame.toLocal(surfPt.geometricNormal);

    // ????????ProcessHits ?????? pathTexturedParamsBuffer
    const PathTexturedMaterialParams* texturedParams = nullptr;
    if (wlp.pathTexturedParamsBuffer != nullptr && pathIndex < wlp.maxNumPaths) {
        texturedParams = &wlp.pathTexturedParamsBuffer[pathIndex];
    }

    BSDFContext bsdfCtx(matDesc, surfPt, pathState.wls, pathState.singleWlSelected(), texturedParams);
    bsdfCtx.geomNormalLocal = geomNormalLocal;

    // ========================================================================
    // 2. BSDF ??????sampleBSDFWithU2????BSDF ???????
    // ========================================================================
    // ???????????? RNG ????
    float u0 = pathState.rng.getFloat0cTo1o();
    float u1 = pathState.rng.getFloat0cTo1o();
    float u2 = pathState.rng.getFloat0cTo1o();

    BSDFSampleResult result;
    sampleBSDFWithU2(bsdfCtx, dirInLocal, u0, u1, u2, &result);

#if VLR_DEBUG_SPEC_TRANS_ONEPIX
    {
        BSDFType bsdfType = getBSDFType(matDesc);
        if (pathState.pixelX == VLR_DEBUG_SPEC_TRANS_PX &&
            pathState.pixelY == VLR_DEBUG_SPEC_TRANS_PY &&
            pathState.pathLength <= 4 &&
            (bsdfType == BSDFType_SpecularTransmission || bsdfType == BSDFType_Specular)) {
            unsigned int idx = atomicAdd(&g_vlrDebugPrintCount, 1u);
            if (idx < 64) {
                printf("[SpecTransDbg] px=(%u,%u) len=%u bsdf=%u frontFace=%d wo=(%.3f,%.3f,%.3f) geomNLocal=(%.3f,%.3f,%.3f)\n",
                    pathState.pixelX, pathState.pixelY,
                    pathState.pathLength,
                    (uint32_t)bsdfType,
                    surfPt.isFrontFace ? 1 : 0,
                    dirInLocal.x, dirInLocal.y, dirInLocal.z,
                    geomNormalLocal.x, geomNormalLocal.y, geomNormalLocal.z);
                printf("[SpecTransDbg] sampled=%u pdf=%.6g f=(%.6g,%.6g,%.6g) wi=(%.3f,%.3f,%.3f)\n",
                    (uint32_t)result.sampledBSDFType, result.pdf,
                    result.f.values[0], result.f.values[1], result.f.values[2],
                    result.dirLocal.x, result.dirLocal.y, result.dirLocal.z);
            }
        }
    }
#endif

#ifdef VLR_DEBUG_MATERIAL
    if (pathIndex < 10) {
        printf("[GPU SampleBSDF] pathIndex=%u: BSDF sampling result\n", pathIndex);
        printf("  sampledBSDFType=%u, isDelta=%d\n", (uint32_t)result.sampledBSDFType, result.isDelta);
        printf("  pdf=%.6f\n", result.pdf);
        printf("  f=(%.6f, %.6f, %.6f, %.6f)\n", 
            result.f.values[0], result.f.values[1], result.f.values[2], result.f.values[3]);
        printf("  dirLocal=(%.3f, %.3f, %.3f)\n",
            result.dirLocal.x, result.dirLocal.y, result.dirLocal.z);
    }
#endif

    // ????VLR path_tracing.cu:243 ?????
    // if (fs == SampledSpectrum::Zero() || fsResult.dirPDF == 0.0f) return;
    // ????dirPDF == 0.0f???? 1e-10 ??????????????????
#ifdef __CUDACC__
    bool pdfInvalid = (result.pdf <= 0.0f || __isnanf(result.pdf) || __isinf(result.pdf));
#else
    bool pdfInvalid = (result.pdf <= 0.0f || !std::isfinite(result.pdf));
#endif
    if (result.f == SampledSpectrum::Zero() || pdfInvalid) {
        pathState.setTerminated();
        return;
    }

    // BSDF ???????? NaN/Inf ?? throughput????VLR ?????????????
    if (!result.f.allFinite()) {
        pathState.setTerminated();
        return;
    }

    // ========================================================================
    // 3. ??????????dispersionStrength > 0 ??
    // ========================================================================
    // ??libWR ???libWR ?????????? RGB ??????
    // isDispersiveBSDFType ??????dispersionStrength????????????true??
    bool isDispersive = isDispersiveBSDFType(result.sampledBSDFType, matDesc);
    
    if (isDispersive && !pathState.singleWlSelected()) {
        result.pdf /= NumSpectralSamples;
        pathState.setSingleWlSelected();
    }

    // ========================================================================
    // 4. ????????throughput *= fs * |cos| / pdf
    // ========================================================================
    // ????VLR path_tracing.cu ?????alpha *= fs * (|cosFactor| / dirPDF)
    // ?????? pdf ??clamp???? pdf ??throughput ????????
    float cosFactor = dot(result.dirLocal, geomNormalLocal);
    float cosAbs = std::abs(cosFactor);

    // Safety check: If cosAbs is too small (perpendicular hit), terminate to avoid division issues
    if (cosAbs < 1e-7f) {
        pathState.setTerminated();
        return;
    }

    pathState.throughput *= result.f * (cosAbs / result.pdf);

#ifdef VLR_DEBUG_SPECULAR_TRANSMISSION
    // ????????????SpecularTransmission ????cmake -DVLR_DEBUG_SPECULAR_TRANSMISSION=ON??
    if (getBSDFType(matDesc) == BSDFType_SpecularTransmission &&
        pathState.pixelX == 256 && pathState.pixelY == 256 && pathState.pathLength == 1) {
        VLR_DEBUG_PRINTF("[SpecTrans] px=(256,256) len=1 pdf=%.6f f=(%.4f,%.4f,%.4f) cosAbs=%.4f throughput=(%.4f,%.4f,%.4f)\n",
               result.pdf, result.f.values[0], result.f.values[1], result.f.values[2],
               cosAbs, pathState.throughput.values[0], pathState.throughput.values[1], pathState.throughput.values[2]);
    }
#endif

    // ?????????
    bool throughputValid = true;
    for (int i = 0; i < NumSpectralSamples && throughputValid; ++i) {
#ifdef __CUDACC__
        float v = pathState.throughput.values[i];
        throughputValid = throughputValid && !__isnanf(v) && !__isinf(v);
#else
        throughputValid = throughputValid && std::isfinite(pathState.throughput.values[i]);
#endif
    }
    if (!throughputValid) {
#ifdef VLR_DEBUG_NAN_TRACKING
        {
            unsigned int idx = atomicAdd(&g_vlrNanPrintCount, 1);
            if (idx < 5) {
                VLR_DEBUG_PRINTF("[NaN] sample_bsdf: px=(%u,%u) pathLen=%u throughput=(%.4f,%.4f,%.4f) op=throughput*=f*cos/pdf\n",
                       pathState.pixelX, pathState.pixelY, pathState.pathLength,
                       pathState.throughput.values[0], pathState.throughput.values[1], pathState.throughput.values[2]);
            }
        }
#endif
        pathState.setTerminated();
        return;
    }

    // ========================================================================
    // 5. ?????????? offsetRayOriginForNextBounce??
    // ========================================================================
    Vector3D dirIn = surfPt.shadingFrame.toWorld(result.dirLocal);

    pathState.origin = offsetRayOriginForNextBounce(surfPt, cosFactor);
    pathState.direction = dirIn;

    // ========================================================================
    // 6. ?? PathState
    // ========================================================================
    pathState.prevDirPDF = result.pdf;
    pathState.prevSampledType = bsdfTypeToDirectionType(result.sampledBSDFType);
    // pathLength ?? ProcessHits ?????????

    // ========================================================================
    // 7. ???????????nextActivePathQueue??
    // ========================================================================
    wlp.nextActivePathQueue.enqueue(pathIndex);
#endif
}
