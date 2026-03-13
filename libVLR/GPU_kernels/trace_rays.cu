// ============================================================================
// VLR Wavefront - TraceRays OptiX Kernel
//
// ??????Wavefront ??????????OptiX ????
// ???Ray Generation?Closest Hit?Miss?Any Hit?Alpha ????????????
//
// ???VLR ?????
// ??????026-03-07
// ???CUDA 13.1, OptiX 7/8, VS2022
// ============================================================================

#define VLR_DEBUG_TRACE_RAYS 0

// ??????
// #define VLR_ENABLE_GPU_DEBUG 1

#ifdef VLR_ENABLE_GPU_DEBUG
    #define VLR_DEBUG_PRINTF(...) printf(__VA_ARGS__)
#else
    #define VLR_DEBUG_PRINTF(...) ((void)0)
#endif

#include "../shared/path_types_minimal.h"

#if defined(__CUDACC__) && defined(VLR_USE_OPTIX)
#ifndef _WIN64
#define _WIN64 1
#endif
#ifndef CUdeviceptr
typedef unsigned long long CUdeviceptr;
#endif
#include <optix.h>
#include <optix_device.h>
#endif

#include <cfloat>

namespace vlr {
namespace shared {

// ============================================================================
// ?????OptiX ????? SBT ????
// ============================================================================
// OptiX 7+ ???? SBT ?????launch parameters ???????__constant__ ??

// ============================================================================
// Payload ???WFTracePayload ??/????7 ??dword??8 ????
// ???pathIndex(1 dword) + lambdas[4](4 dwords) + selectedLambda(1) + padding(1) = 7 dwords
// OptiX ?? optixGetPayload_N / optixSetPayload_N ??dword ??
// ============================================================================

#if defined(__CUDACC__) && defined(VLR_USE_OPTIX)
CUDA_DEVICE_FUNCTION CUDA_INLINE void packWFTracePayload(
    const WFTracePayload& p, unsigned int* dwords) {
    dwords[0] = p.pathIndex;
    *reinterpret_cast<float*>(&dwords[1]) = p.wls.lambdas[0];
    *reinterpret_cast<float*>(&dwords[2]) = p.wls.lambdas[1];
    *reinterpret_cast<float*>(&dwords[3]) = p.wls.lambdas[2];
    *reinterpret_cast<float*>(&dwords[4]) = p.wls.lambdas[3];
    dwords[5] = p.wls.selectedLambda;
    dwords[6] = p.wls._padding;
}

CUDA_DEVICE_FUNCTION CUDA_INLINE void unpackWFTracePayload(
    const unsigned int* dwords, WFTracePayload* p) {
    p->pathIndex = dwords[0];
    p->wls.lambdas[0] = *reinterpret_cast<const float*>(&dwords[1]);
    p->wls.lambdas[1] = *reinterpret_cast<const float*>(&dwords[2]);
    p->wls.lambdas[2] = *reinterpret_cast<const float*>(&dwords[3]);
    p->wls.lambdas[3] = *reinterpret_cast<const float*>(&dwords[4]);
    p->wls.selectedLambda = dwords[5];
    p->wls._padding = dwords[6];
}

// ???????? ??dword??.0f ??????.0f ??????
CUDA_DEVICE_FUNCTION CUDA_INLINE void setShadowPayloadVisible() {
    float f = 1.0f;
    optixSetPayload_0(*reinterpret_cast<unsigned int*>(&f));
}
CUDA_DEVICE_FUNCTION CUDA_INLINE void setShadowPayloadOccluded() {
    float f = 0.0f;
    optixSetPayload_0(*reinterpret_cast<unsigned int*>(&f));
}
#endif

}  // namespace shared
}  // namespace vlr

// ============================================================================
// OptiX ??????
// ============================================================================

#if defined(__CUDACC__) && defined(VLR_USE_OPTIX)
#define RT_RG_NAME(name) __raygen__##name
#define RT_CH_NAME(name) __closesthit__##name
#define RT_MS_NAME(name) __miss__##name
#define RT_AH_NAME(name) __anyhit__##name
#endif

// ============================================================================
// 1. Ray Generation Program????????????????
// ============================================================================

#if defined(__CUDACC__) && defined(VLR_USE_OPTIX)
extern "C" __global__ void RT_RG_NAME(traceRays)() {
    using namespace vlr::shared;

#ifdef VLR_DEBUG_TRACE_RAYS
    uint32_t workIndex_debug = optixGetLaunchIndex().x;
    if (workIndex_debug == 0) {
        VLR_DEBUG_PRINTF("[GPU TraceRays RayGen] ENTRY: workIndex=%u\n", workIndex_debug);
    }
#endif

    // ??SBT ???? launch parameters ??
    const WavefrontSBTData* sbtData = reinterpret_cast<const WavefrontSBTData*>(
        optixGetSbtDataPointer()
    );
    
#ifdef VLR_DEBUG_TRACE_RAYS
    if (workIndex_debug == 0) {
        VLR_DEBUG_PRINTF("[GPU TraceRays RayGen] sbtData=%p\n", sbtData);
        if (sbtData) {
            VLR_DEBUG_PRINTF("[GPU TraceRays RayGen] sbtData->params=%p\n", sbtData->params);
        }
    }
#endif
    
    if (!sbtData || !sbtData->params) {
#ifdef VLR_DEBUG_TRACE_RAYS
        if (workIndex_debug == 0) {
            VLR_DEBUG_PRINTF("[GPU TraceRays RayGen] ERROR: sbtData or params is null!\n");
        }
#endif
        return;
    }
    
    const WavefrontLaunchParameters& wlp = *sbtData->params;

#ifdef VLR_DEBUG_TRACE_RAYS
    if (workIndex_debug == 0) {
        VLR_DEBUG_PRINTF("[GPU TraceRays RayGen] wlp loaded successfully\n");
    }
#endif

    // ??????????????????????
    uint32_t workIndex = optixGetLaunchIndex().x;
    
#ifdef VLR_DEBUG_TRACE_RAYS
    if (workIndex == 0) {
        VLR_DEBUG_PRINTF("[GPU TraceRays RayGen] workIndex=%u, counter ptr=%p\n", workIndex, wlp.activePathQueue.counter);
        VLR_DEBUG_PRINTF("[GPU TraceRays RayGen] wlp.imageSize=(%u,%u), wlp.maxPathLength=%u\n",
               wlp.imageSize.x, wlp.imageSize.y, wlp.maxPathLength);
        VLR_DEBUG_PRINTF("[GPU TraceRays RayGen] wlp.pathStateBuffer=%p, wlp.topGroup=%llu\n",
               wlp.pathStateBuffer, (unsigned long long)wlp.topGroup);
    }
#endif
    
    if (!wlp.activePathQueue.counter) {
#ifdef VLR_DEBUG_TRACE_RAYS
        if (workIndex == 0) {
            VLR_DEBUG_PRINTF("[GPU TraceRays RayGen] ERROR: counter is null!\n");
        }
#endif
        return;
    }
    
    uint32_t numActive = *wlp.activePathQueue.counter;
    
#ifdef VLR_DEBUG_TRACE_RAYS
    if (workIndex == 0) {
        VLR_DEBUG_PRINTF("[GPU TraceRays RayGen] numActive=%u\n", numActive);
    }
#endif

    if (workIndex >= numActive)
        return;

    uint32_t pathIndex = wlp.activePathQueue.pathIndices[workIndex];
    const WavefrontPathState& pathState = wlp.pathStateBuffer[pathIndex];

    if (!pathState.isActive())
        return;

    // ???WFTracePayload?pathIndex + wls??????Hit/Miss ????hitInfoBuffer
    WFTracePayload payload;
    payload.pathIndex = pathIndex;
    payload.wls = pathState.wls;

    unsigned int pd[7];
    packWFTracePayload(payload, pd);

    // OptiX 8 optixTrace: (handle, float3 origin, float3 direction, tmin, tmax, rayTime, mask, flags, sbtOffset, sbtStride, missSbtIndex, payload&...)
    // ???? RayGen ??payload ?? optixTrace ??????????optixSetPayload
    float3 rayOrigin = make_float3(pathState.origin.x, pathState.origin.y, pathState.origin.z);
    float3 rayDirection = make_float3(pathState.direction.x, pathState.direction.y, pathState.direction.z);
    
#ifdef VLR_DEBUG_TRACE_RAYS
    if (pathIndex == 0) {
        VLR_DEBUG_PRINTF("[GPU TraceRays RayGen] pathIndex=%u, workIndex=%u, numActive=%u\n", pathIndex, workIndex, numActive);
        VLR_DEBUG_PRINTF("[GPU TraceRays RayGen] origin=(%.3f,%.3f,%.3f), dir=(%.3f,%.3f,%.3f)\n",
               rayOrigin.x, rayOrigin.y, rayOrigin.z,
               rayDirection.x, rayDirection.y, rayDirection.z);
        VLR_DEBUG_PRINTF("[GPU TraceRays RayGen] topGroup=%llu\n", (unsigned long long)wlp.topGroup);
    }
#endif
    
    optixTrace(
        wlp.topGroup,
        rayOrigin,
        rayDirection,
        0.0f,    // tmin???? VLR ????? offsetRayOrigin ??????
        FLT_MAX, // tmax
        0.0f,    // rayTime
        0xFF,    // visibilityMask
        OPTIX_RAY_FLAG_NONE,
        0,       // SBT offset????hit group??
        2,       // SBT stride?Ray Types ???Closest Hit + Shadow??
        0,       // miss SBT index?Ray Type 0?Closest Hit??
        pd[0], pd[1], pd[2], pd[3], pd[4], pd[5], pd[6]
    );
    
#ifdef VLR_DEBUG_TRACE_RAYS
    if (pathIndex == 0) {
        VLR_DEBUG_PRINTF("[GPU TraceRays RayGen] optixTrace returned\n");
    }
#endif
}
#endif

// ============================================================================
// 2. Closest Hit Program????HitInfo?instIndex, geomInstIndex, primIndex, u, v, t??
// ============================================================================

#if defined(__CUDACC__) && defined(VLR_USE_OPTIX)
extern "C" __global__ void RT_CH_NAME(closestHit)() {
    using namespace vlr::shared;

    // ??SBT ???? launch parameters ??
    const WavefrontSBTData* sbtData = reinterpret_cast<const WavefrontSBTData*>(
        optixGetSbtDataPointer()
    );
    const WavefrontLaunchParameters& wlp = *sbtData->params;

    unsigned int pd[7];
    pd[0] = optixGetPayload_0();
    pd[1] = optixGetPayload_1();
    pd[2] = optixGetPayload_2();
    pd[3] = optixGetPayload_3();
    pd[4] = optixGetPayload_4();
    pd[5] = optixGetPayload_5();
    pd[6] = optixGetPayload_6();

    WFTracePayload payload;
    unpackWFTracePayload(pd, &payload);

    uint32_t pathIndex = payload.pathIndex;
    
#ifdef VLR_DEBUG_TRACE_RAYS
    if (pathIndex == 0) {
        VLR_DEBUG_PRINTF("[GPU TraceRays ClosestHit] pathIndex=%u ENTERED\n", pathIndex);
    }
#endif
    
    WavefrontHitInfo& hitInfo = wlp.hitInfoBuffer[pathIndex];

    // OptiX ?? API ????????
    uint32_t instIndex = optixGetInstanceId();
    uint32_t primIndex = optixGetPrimitiveIndex();
    float2 barycentrics = optixGetTriangleBarycentrics();

    // geomInstIndex??? Instance ????????????
    uint32_t geomInstIndex = instIndex;
    if (wlp.instBuffer != nullptr && wlp.instBuffer[instIndex].numGeomInsts > 0) {
        geomInstIndex = wlp.instBuffer[instIndex].geomInstIndices[0];
    }

    float t = optixGetRayTmax();

    hitInfo.instIndex = instIndex;
    hitInfo.geomInstIndex = geomInstIndex;
    hitInfo.primIndex = primIndex;
    hitInfo.u = barycentrics.x;
    hitInfo.v = barycentrics.y;
    hitInfo.t = t;
    hitInfo.setHasHit(true);
    hitInfo.setHitInfinity(false);
}
#endif

// ============================================================================
// 3. Miss Program???????????????????????
// ============================================================================

#if defined(__CUDACC__) && defined(VLR_USE_OPTIX)
extern "C" __global__ void RT_MS_NAME(miss)() {
    using namespace vlr::shared;

    // ??SBT ???? launch parameters ??
    const WavefrontSBTData* sbtData = reinterpret_cast<const WavefrontSBTData*>(
        optixGetSbtDataPointer()
    );
    const WavefrontLaunchParameters& wlp = *sbtData->params;

    unsigned int pd[7];
    pd[0] = optixGetPayload_0();
    pd[1] = optixGetPayload_1();
    pd[2] = optixGetPayload_2();
    pd[3] = optixGetPayload_3();
    pd[4] = optixGetPayload_4();
    pd[5] = optixGetPayload_5();
    pd[6] = optixGetPayload_6();

    WFTracePayload payload;
    unpackWFTracePayload(pd, &payload);

    uint32_t pathIndex = payload.pathIndex;
    
#ifdef VLR_DEBUG_TRACE_RAYS
    if (pathIndex == 0) {
        VLR_DEBUG_PRINTF("[GPU TraceRays Miss] pathIndex=%u ENTERED\n", pathIndex);
    }
#endif
    
    WavefrontHitInfo& hitInfo = wlp.hitInfoBuffer[pathIndex];

    hitInfo.reset();
    hitInfo.setHasHit(true);
    hitInfo.setHitInfinity(true);
}
#endif

// ============================================================================
// 4. Any Hit Program?Alpha ???????????alpha < ??? optixIgnoreIntersection??
// ============================================================================

#if defined(__CUDACC__) && defined(VLR_USE_OPTIX)
extern "C" __global__ void RT_AH_NAME(anyHitWithAlpha)() {
    using namespace vlr::shared;

    const float alphaThreshold = 0.5f;
    (void)alphaThreshold;
    // TODO: ??????alpha?? < threshold ??optixIgnoreIntersection()
}
#endif

// ============================================================================
// 5. Shadow Ray Miss Program??????????????visibility = 1.0??
// ============================================================================

#if defined(__CUDACC__) && defined(VLR_USE_OPTIX)
extern "C" __global__ void RT_MS_NAME(shadowMiss)() {
    using namespace vlr::shared;
    setShadowPayloadVisible();
}
#endif

// ============================================================================
// 6. Shadow Ray Any Hit Program??????????????optixTerminateRay??
// ============================================================================

#if defined(__CUDACC__) && defined(VLR_USE_OPTIX)
extern "C" __global__ void RT_AH_NAME(shadowAnyHit)() {
    using namespace vlr::shared;
    setShadowPayloadOccluded();
    optixTerminateRay();
}
#endif

// ============================================================================
// 7. Shadow Any Hit?? Alpha ???????????
// ============================================================================

#if defined(__CUDACC__) && defined(VLR_USE_OPTIX)
extern "C" __global__ void RT_AH_NAME(shadowAnyHitWithAlpha)() {
    using namespace vlr::shared;
    
    const float alphaThreshold = 0.5f;
    (void)alphaThreshold;
    // TODO: ??????alpha?? threshold ??optixIgnoreIntersection() ?? optixTerminateRay()
    setShadowPayloadOccluded();
    optixTerminateRay();
}
#endif

// ============================================================================
// 8. Light Path Ray Generation Program (LVC-BPT)
// ============================================================================

#if defined(__CUDACC__) && defined(VLR_USE_OPTIX)
extern "C" __global__ void RT_RG_NAME(traceLightRays)() {
    using namespace vlr::shared;

    const WavefrontSBTData* sbtData = reinterpret_cast<const WavefrontSBTData*>(
        optixGetSbtDataPointer()
    );
    if (!sbtData || !sbtData->params) return;
    const WavefrontLaunchParameters& wlp = *sbtData->params;

    uint32_t pathIndex = optixGetLaunchIndex().x;
    if (pathIndex >= wlp.numLightPaths) return;
    if (!wlp.lightPathStateBuffer) return;

    const LightPathState& state = wlp.lightPathStateBuffer[pathIndex];
    if (!state.isActive()) return;

    WFTracePayload payload;
    payload.pathIndex = pathIndex;
    payload.wls = state.wls;

    unsigned int pd[7];
    packWFTracePayload(payload, pd);

    float3 rayOrigin = make_float3(state.origin.x, state.origin.y, state.origin.z);
    float3 rayDirection = make_float3(state.direction.x, state.direction.y, state.direction.z);

    optixTrace(
        wlp.topGroup,
        rayOrigin,
        rayDirection,
        0.001f,
        FLT_MAX,
        0.0f,
        0xFF,
        OPTIX_RAY_FLAG_NONE,
        0,
        2,
        0,
        pd[0], pd[1], pd[2], pd[3], pd[4], pd[5], pd[6]
    );
}
#endif

// ============================================================================
// 9. Light Path Closest Hit - writes to lightHitInfoBuffer
// ============================================================================

#if defined(__CUDACC__) && defined(VLR_USE_OPTIX)
extern "C" __global__ void RT_CH_NAME(lightClosestHit)() {
    using namespace vlr::shared;

    const WavefrontSBTData* sbtData = reinterpret_cast<const WavefrontSBTData*>(
        optixGetSbtDataPointer()
    );
    const WavefrontLaunchParameters& wlp = *sbtData->params;

    unsigned int pd[7];
    pd[0] = optixGetPayload_0();
    pd[1] = optixGetPayload_1();
    pd[2] = optixGetPayload_2();
    pd[3] = optixGetPayload_3();
    pd[4] = optixGetPayload_4();
    pd[5] = optixGetPayload_5();
    pd[6] = optixGetPayload_6();

    WFTracePayload payload;
    unpackWFTracePayload(pd, &payload);
    uint32_t pathIndex = payload.pathIndex;

    if (!wlp.lightHitInfoBuffer) return;
    WavefrontHitInfo& hitInfo = wlp.lightHitInfoBuffer[pathIndex];

    uint32_t instIndex = optixGetInstanceId();
    uint32_t primIndex = optixGetPrimitiveIndex();
    float2 barycentrics = optixGetTriangleBarycentrics();

    uint32_t geomInstIndex = instIndex;
    if (wlp.instBuffer != nullptr && wlp.instBuffer[instIndex].numGeomInsts > 0) {
        geomInstIndex = wlp.instBuffer[instIndex].geomInstIndices[0];
    }

    float t = optixGetRayTmax();

    hitInfo.instIndex = instIndex;
    hitInfo.geomInstIndex = geomInstIndex;
    hitInfo.primIndex = primIndex;
    hitInfo.u = barycentrics.x;
    hitInfo.v = barycentrics.y;
    hitInfo.t = t;
    hitInfo.setHasHit(true);
    hitInfo.setHitInfinity(false);
}
#endif

// ============================================================================
// 10. Light Path Miss - writes to lightHitInfoBuffer
// ============================================================================

#if defined(__CUDACC__) && defined(VLR_USE_OPTIX)
extern "C" __global__ void RT_MS_NAME(lightMiss)() {
    using namespace vlr::shared;

    const WavefrontSBTData* sbtData = reinterpret_cast<const WavefrontSBTData*>(
        optixGetSbtDataPointer()
    );
    const WavefrontLaunchParameters& wlp = *sbtData->params;

    unsigned int pd[7];
    pd[0] = optixGetPayload_0();
    pd[1] = optixGetPayload_1();
    pd[2] = optixGetPayload_2();
    pd[3] = optixGetPayload_3();
    pd[4] = optixGetPayload_4();
    pd[5] = optixGetPayload_5();
    pd[6] = optixGetPayload_6();

    WFTracePayload payload;
    unpackWFTracePayload(pd, &payload);
    uint32_t pathIndex = payload.pathIndex;

    if (!wlp.lightHitInfoBuffer) return;
    WavefrontHitInfo& hitInfo = wlp.lightHitInfoBuffer[pathIndex];
    hitInfo.reset();
    hitInfo.setHasHit(true);
    hitInfo.setHitInfinity(true);
}
#endif

// ============================================================================
// 11. Shadow Ray Batch RayGen (for vertex connection visibility test)
// ============================================================================

#if defined(__CUDACC__) && defined(VLR_USE_OPTIX)
extern "C" __global__ void RT_RG_NAME(traceShadowRays)() {
    using namespace vlr::shared;

    const WavefrontSBTData* sbtData = reinterpret_cast<const WavefrontSBTData*>(
        optixGetSbtDataPointer()
    );
    if (!sbtData || !sbtData->params) return;
    const WavefrontLaunchParameters& wlp = *sbtData->params;

    uint32_t workIndex = optixGetLaunchIndex().x;
    if (!wlp.shadowRayQueue || workIndex >= wlp.numShadowRayRequests) return;

    const ShadowRayRequest& req = wlp.shadowRayQueue[workIndex];

    float3 origin = make_float3(req.origin.x, req.origin.y, req.origin.z);
    float3 direction = make_float3(req.direction.x, req.direction.y, req.direction.z);

    float visibility = 0.0f;
    unsigned int vis_u = *reinterpret_cast<unsigned int*>(&visibility);

    optixTrace(
        wlp.topGroup,
        origin,
        direction,
        0.001f,
        req.tMax - 0.001f,
        0.0f,
        0xFF,
        OPTIX_RAY_FLAG_TERMINATE_ON_FIRST_HIT | OPTIX_RAY_FLAG_DISABLE_CLOSESTHIT,
        1,
        2,
        1,
        vis_u
    );

    visibility = *reinterpret_cast<float*>(&vis_u);
    wlp.shadowRayResults[workIndex] = visibility;
}
#endif
