// ============================================================================
// VLR Wavefront - SampleLights Kernel (Next Event Estimation)
//
// ??????Wavefront ??????????????NEE???
// ????????????????????????BSDF ???MIS ????
//       ??????????????????delta ????
//
// ???VLR ?????
// ??????026-03-07
// ???CUDA 13.1, OptiX 8.0.0, VS2022
// ============================================================================

#define VLR_DEBUG_LIGHT_SAMPLING 0

#ifndef VLR_DEBUG_SPEC_TRANS_ONEPIX
#define VLR_DEBUG_SPEC_TRANS_ONEPIX 0
#endif
#ifndef VLR_DEBUG_SPEC_TRANS_PX
#define VLR_DEBUG_SPEC_TRANS_PX 256
#endif
#ifndef VLR_DEBUG_SPEC_TRANS_PY
#define VLR_DEBUG_SPEC_TRANS_PY 166
#endif

#include "../shared/kernel_common.h"
#include "kernel_launch.h"
#include "../shared/path_types.h"
#include "../shared/render_common.h"
#include "../shared/light_common.h"
#include "../shared/light_types.h"
#include "../shared/bsdf_common.h"
#include "../shared/geometry_common.h"
#include "../shared/material_types.h"
#include "../shared/performance_config.h"
#include "../include/vlr/basic_types.h"
#include "warp_utils.cuh"
#include "shared_memory_cache.cuh"

#include <cuda_runtime.h>
#include <cfloat>
#include <cmath>

// ? constexpr ?????????????
// ????????,??WavefrontLaunchParameters???????
#define VLR_USE_LIGHT_CACHE 0

// OptiX ????????????
#if defined(__CUDACC__) && defined(VLR_USE_OPTIX)
    #include <optix_device.h>
#endif

// ============================================================================
// SampleLights Kernel
// ============================================================================
// ??????????????Next Event Estimation
// ?????ProcessHits -> SampleLights -> SampleBSDF

extern "C" __global__ void sampleLights(
    vlr::shared::WavefrontLaunchParameters* params) {
    using namespace vlr::shared;
    WavefrontLaunchParameters& wlp = *params;

#ifdef __CUDACC__

#if VLR_USE_LIGHT_CACHE
    __shared__ LightCache<PerformanceConfig::LightCacheSize> lightCache;
    if (threadIdx.x == 0) {
        uint32_t numLights = wlp.numLights;
        if (numLights > PerformanceConfig::LightCacheSize)
            numLights = PerformanceConfig::LightCacheSize;
        for (uint32_t i = 0; i < numLights; ++i) {
            lightCache.lights[i] = wlp.geomInstBuffer[wlp.lightIndices[i]];
        }
        lightCache.numLights = numLights;
    }
    __syncthreads();
#endif

    uint32_t workIndex = blockIdx.x * blockDim.x + threadIdx.x;
    
    if (workIndex >= wlp.activePathQueue.size())
        return;

    uint32_t pathIndex = wlp.activePathQueue.pathIndices[workIndex];
    
    // ??????__restrict__ ????????????
    WavefrontPathState* __restrict__ pathStatePtr = &wlp.pathStateBuffer[pathIndex];
    WavefrontPathState& pathState = *pathStatePtr;

    // ???warp-level ?????
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

    // ??????BSDF ????
    const GeometryInstance& geomInst = wlp.geomInstBuffer[hitInfo.geomInstIndex];
    const SurfaceMaterialDescriptor& matDesc = wlp.materialDescriptorBuffer[geomInst.materialIndex];

    // ??????delta ??????????NEE ??delta ????
#if VLR_DEBUG_SPEC_TRANS_ONEPIX
    if (pathState.pixelX == VLR_DEBUG_SPEC_TRANS_PX && pathState.pixelY == VLR_DEBUG_SPEC_TRANS_PY &&
        pathState.pathLength <= 4) {
        BSDFType bt = getBSDFType(matDesc);
        printf("[NEE_Dbg] px=(%u,%u) len=%u bsdfType=%u isDelta=%d surfPos=(%.3f,%.3f,%.3f) throughput=(%.4g,%.4g,%.4g)\n",
            pathState.pixelX, pathState.pixelY, pathState.pathLength,
            (uint32_t)bt, materialIsDelta(matDesc) ? 1 : 0,
            surfPt.position.x, surfPt.position.y, surfPt.position.z,
            pathState.throughput.values[0], pathState.throughput.values[1], pathState.throughput.values[2]);
    }
#endif
    if (materialIsDelta(matDesc))
        return;

    // ========================================================================
    // 1. ????
    // ========================================================================
    float uLight = pathState.rng.getFloat0cTo1o();
    LightSelectResult selectResult;
    if (!selectLight(uLight, &selectResult, wlp)) {
#ifdef VLR_DEBUG_LIGHT_SAMPLING
        if (pathIndex == 0) {
            VLR_DEBUG_PRINTF("[GPU] selectLight failed: numLights=%u\n", wlp.lightInstDist.numValues);
        }
#endif
        return;
    }
    
#ifdef VLR_DEBUG_LIGHT_SAMPLING
    if (pathIndex == 0) {
        VLR_DEBUG_PRINTF("[GPU] selectLight success: lightType=%u, instIndex=%u, geomInstIndex=%u\n",
            selectResult.descriptor.type, selectResult.descriptor.instIndex, selectResult.descriptor.geomInstIndex);
    }
#endif

    // ========================================================================
    // 2. ??????
    // ========================================================================
    float u0 = pathState.rng.getFloat0cTo1o();
    float u1 = pathState.rng.getFloat0cTo1o();
    float u2 = pathState.rng.getFloat0cTo1o();
    LightSampleResult sampleResult;
    if (!sampleLight(selectResult.descriptor, surfPt.position, u0, u1, u2, &sampleResult, wlp))
        return;

    sampleResult.lightSelectProb = selectResult.selectProb;

    // ????????????????
    Vector3D dirToLight = sampleResult.lightSurfPt.position - surfPt.position;
    float distance = length(dirToLight);
    if (distance < 1e-8f)
        return;
    dirToLight = dirToLight / distance;

    // ========================================================================
    // 3. ??????
    // ========================================================================
    Vector3D dirToShading = -dirToLight;  // ????????
    LightEmissionResult emissionResult;
    if (!evaluateLightEmission(selectResult.descriptor, sampleResult.lightSurfPt,
                              dirToShading, pathState.wls, &emissionResult, wlp))
        return;

    if (!emissionResult.Le.hasNonZero())
        return;

    // Visibility will be tested later via shadow ray pass; skip stub here.

    // ========================================================================
    // 5. BSDF ????????????f ???? MIS??
    // ========================================================================
    Vector3D dirInLocal = surfPt.shadingFrame.toLocal(-pathState.direction);
    Vector3D dirOutLocal = surfPt.shadingFrame.toLocal(dirToLight);
    Normal3D geomNormalLocal = surfPt.shadingFrame.toLocal(surfPt.geometricNormal);

#ifdef VLR_DEBUG_BSDF_VERBOSE
    if (pathIndex < 5) {
        VLR_DEBUG_PRINTF("[GPU SampleLights] pathIndex=%u: Direction check\n", pathIndex);
        VLR_DEBUG_PRINTF("  pathState.direction (world)=(%.3f,%.3f,%.3f)\n",
            pathState.direction.x, pathState.direction.y, pathState.direction.z);
        VLR_DEBUG_PRINTF("  dirToLight (world)=(%.3f,%.3f,%.3f)\n",
            dirToLight.x, dirToLight.y, dirToLight.z);
        VLR_DEBUG_PRINTF("  surfPt.geometricNormal (world)=(%.3f,%.3f,%.3f)\n",
            surfPt.geometricNormal.x, surfPt.geometricNormal.y, surfPt.geometricNormal.z);
        VLR_DEBUG_PRINTF("  dirInLocal=(%.3f,%.3f,%.3f)\n",
            dirInLocal.x, dirInLocal.y, dirInLocal.z);
        VLR_DEBUG_PRINTF("  dirOutLocal=(%.3f,%.3f,%.3f)\n",
            dirOutLocal.x, dirOutLocal.y, dirOutLocal.z);
        VLR_DEBUG_PRINTF("  geomNormalLocal=(%.3f,%.3f,%.3f)\n",
            geomNormalLocal.x, geomNormalLocal.y, geomNormalLocal.z);
        float NdotIn = dot(dirInLocal, geomNormalLocal);
        float NdotOut = dot(dirOutLocal, geomNormalLocal);
        VLR_DEBUG_PRINTF("  NdotIn=%.3f, NdotOut=%.3f\n", NdotIn, NdotOut);
    }
#endif

    BSDFContext bsdfCtx(matDesc, surfPt, pathState.wls);
    SampledSpectrum fs = evaluateBSDF(bsdfCtx, dirInLocal, dirOutLocal);

#ifdef VLR_DEBUG_MATERIAL
    if (pathIndex < 5) {
        BSDFType bsdfType = getBSDFType(matDesc);
        VLR_DEBUG_PRINTF("[GPU SampleLights] pathIndex=%u: evaluateBSDF result\n", pathIndex);
        VLR_DEBUG_PRINTF("  BSDFType=%u\n", (uint32_t)bsdfType);
        VLR_DEBUG_PRINTF("  fs=(%.6f, %.6f, %.6f, %.6f)\n", 
            fs.values[0], fs.values[1], fs.values[2], fs.values[3]);
    }
#endif

    if (!fs.hasNonZero())
        return;

    // ========================================================================
    // 6. ?? PDF ??BSDF PDF????MIS??
    // ????VLR path_tracing.cu ??????? PDF
    // ========================================================================
    // ???? PDF = ???? * ???? PDF????VLR: lightPDF = lightProb * lpResult.areaPDF??
    float lightAreaPDF = sampleResult.lightSelectProb * sampleResult.areaPDF;

    // BSDF ?? PDF ???????PDF ???? lightAreaPDF ??MIS
    // ?? VLR: bsdfPDF = bsdf.evaluatePDF() * cosLight * recSquaredDistance
    float cosLight = absDot(-dirToLight, sampleResult.lightSurfPt.geometricNormal);
    float squaredDistance = distance * distance;
    float recSquaredDistance = (squaredDistance > 1e-12f) ? (1.0f / squaredDistance) : 0.0f;
    float bsdfDirPDF = getBSDFPDF(bsdfCtx, dirInLocal, dirOutLocal);
    float bsdfAreaPDF = bsdfDirPDF * cosLight * recSquaredDistance;

    // ========================================================================
    // 7. MIS ???Power Heuristic??
    // ?? VLR: MISWeight = (lightPDF^2) / (lightPDF^2 + bsdfPDF^2)
    // ???? delta ??lightPDF ?????MISWeight = 1
    // ========================================================================
    float MISWeight = 1.0f;
    bool lightPDFInf = (lightAreaPDF != lightAreaPDF) || (lightAreaPDF >= 1e30f);
    if (!selectResult.descriptor.isDelta() && !lightPDFInf && lightAreaPDF > 1e-10f)
        MISWeight = computeMISWeight(lightAreaPDF, bsdfAreaPDF);

    // ========================================================================
    // 8. ????G = cos(theta_shading) * cos(theta_light) / distance^2
    // ?? VLR: G = fractionalVisibility * absDot(...) * cosLight * recSquaredDistance
    // ========================================================================
    float G = computeGeometryTerm(surfPt, sampleResult.lightSurfPt, dirToLight, squaredDistance);

    if (G <= 0.0f)
        return;

    // ========================================================================
    // 9. ????????
    // ?? VLR: scalarCoeff = G * MISWeight / lightPDF
    //          contribution += alpha * Le * fs * scalarCoeff
    // ?? contribution = throughput * Le * fs * G * MISWeight / lightAreaPDF
    // ========================================================================
    float invLightPDF = 1.0f;
    if (!lightPDFInf && lightAreaPDF > 1e-10f)
        invLightPDF = 1.0f / lightAreaPDF;

    SampledSpectrum contrib = pathState.throughput * emissionResult.Le * fs * G * MISWeight * invLightPDF;


#if VLR_DEBUG_SPEC_TRANS_ONEPIX
    if (pathState.pixelX == VLR_DEBUG_SPEC_TRANS_PX && pathState.pixelY == VLR_DEBUG_SPEC_TRANS_PY &&
        pathState.pathLength <= 4) {
        printf("[NEE_Dbg] CONTRIB len=%u Le=(%.4g,%.4g,%.4g) fs=(%.4g,%.4g,%.4g) G=%.6g MIS=%.4f invPDF=%.4g contrib=(%.6g,%.6g,%.6g)\n",
            pathState.pathLength,
            emissionResult.Le.values[0], emissionResult.Le.values[1], emissionResult.Le.values[2],
            fs.values[0], fs.values[1], fs.values[2],
            G, MISWeight, invLightPDF,
            contrib.values[0], contrib.values[1], contrib.values[2]);
    }
#endif

    if (!contrib.allFinite() || !contrib.hasNonZero())
        return;

    // Enqueue NEE shadow ray for deferred visibility test (no direct accumulation)
    if (wlp.shadowRayQueue != nullptr && wlp.numShadowRayRequests != nullptr &&
        wlp.maxShadowRayRequests > 0) {
        uint32_t slot = atomicAdd(wlp.numShadowRayRequests, 1u);
        if (slot < wlp.maxShadowRayRequests) {
            ShadowRayRequest& req = wlp.shadowRayQueue[slot];
            // Offset along normal facing the light to avoid self-intersection
            Normal3D offsetN = selectOffsetNormal(dot(dirToLight, surfPt.geometricNormal),
                                                  surfPt.geometricNormal);
            req.origin = offsetRayOrigin(surfPt.position, offsetN);
            req.direction = dirToLight;
            req.tMax = distance * 0.999f;
            req.pathIndex = pathIndex;
            req.contribution = contrib;
        }
    }
#endif
}
