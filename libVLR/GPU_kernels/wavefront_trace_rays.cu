// ============================================================================
// VLR Wavefront - TraceRays OptiX Kernel
//
// 本文件实现 Wavefront 路径追踪的光线追踪 OptiX 程序。
// 包含：Ray Generation、Closest Hit、Miss、Any Hit（Alpha 测试）、阴影光线程序。
//
// 作者：VLR 开发团队
// 创建日期：2026-03-07
// 环境：CUDA 13.1, OptiX 7/8, VS2022
// ============================================================================

#include "../shared/wavefront_types_minimal.h"

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
// 启动参数：OptiX 设备端通过 __constant__ 访问
// ============================================================================

#if defined(__CUDACC__) && defined(VLR_USE_OPTIX)
extern "C" __constant__ WavefrontLaunchParameters wlp;
#endif

// ============================================================================
// Payload 辅助：WFTracePayload 打包/解包为 7 个 dword（28 字节）
// 布局：pathIndex(1 dword) + lambdas[4](4 dwords) + selectedLambda(1) + padding(1) = 7 dwords
// OptiX 使用 optixGetPayload_N / optixSetPayload_N 按 dword 访问
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

// 阴影光线载荷：1 个 dword，1.0f 表示可见，0.0f 表示被遮挡
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
// OptiX 程序入口宏
// ============================================================================

#if defined(__CUDACC__) && defined(VLR_USE_OPTIX)
#define RT_RG_NAME(name) __raygen__##name
#define RT_CH_NAME(name) __closesthit__##name
#define RT_MS_NAME(name) __miss__##name
#define RT_AH_NAME(name) __anyhit__##name
#endif

// ============================================================================
// 1. Ray Generation Program：从活跃队列读取路径，发射光线
// ============================================================================

#if defined(__CUDACC__) && defined(VLR_USE_OPTIX)
extern "C" __global__ void RT_RG_NAME(wavefrontTraceRays)() {
    using namespace vlr::shared;

    // 工作索引：每个线程处理活跃队列中的一个路径
    uint32_t workIndex = optixGetLaunchIndex().x;
    uint32_t numActive = *wlp.activePathQueue.counter;

    if (workIndex >= numActive)
        return;

    uint32_t pathIndex = wlp.activePathQueue.pathIndices[workIndex];
    const WavefrontPathState& pathState = wlp.pathStateBuffer[pathIndex];

    if (!pathState.isActive())
        return;

    // 构造 WFTracePayload（pathIndex + wls），用于在 Hit/Miss 中索引 hitInfoBuffer
    WFTracePayload payload;
    payload.pathIndex = pathIndex;
    payload.wls = pathState.wls;

    unsigned int pd[7];
    packWFTracePayload(payload, pd);
    optixSetPayload_0(pd[0]);
    optixSetPayload_1(pd[1]);
    optixSetPayload_2(pd[2]);
    optixSetPayload_3(pd[3]);
    optixSetPayload_4(pd[4]);
    optixSetPayload_5(pd[5]);
    optixSetPayload_6(pd[6]);

    // OptiX 8 optixTrace: (handle, float3 origin, float3 direction, tmin, tmax, rayTime, mask, flags, sbtOffset, sbtStride, missSbtIndex, payload&...)
    float3 rayOrigin = make_float3(pathState.origin.x, pathState.origin.y, pathState.origin.z);
    float3 rayDirection = make_float3(pathState.direction.x, pathState.direction.y, pathState.direction.z);
    optixTrace(
        wlp.topGroup,
        rayOrigin,
        rayDirection,
        1e-5f,   // tmin：避免自相交
        FLT_MAX, // tmax
        0.0f,    // rayTime
        0xFF,    // visibilityMask
        OPTIX_RAY_FLAG_NONE,
        0,       // SBT offset（默认 hit group）
        0,       // SBT stride
        0,       // miss SBT index
        pd[0], pd[1], pd[2], pd[3], pd[4], pd[5], pd[6]
    );
}
#endif

// ============================================================================
// 2. Closest Hit Program：填充 HitInfo（instIndex, geomInstIndex, primIndex, u, v, t）
// ============================================================================

#if defined(__CUDACC__) && defined(VLR_USE_OPTIX)
extern "C" __global__ void RT_CH_NAME(wavefrontClosestHit)() {
    using namespace vlr::shared;

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
    WavefrontHitInfo& hitInfo = wlp.hitInfoBuffer[pathIndex];

    // OptiX 设备 API 获取命中几何信息
    uint32_t instIndex = optixGetInstanceId();
    uint32_t primIndex = optixGetPrimitiveIndex();
    float2 barycentrics = optixGetTriangleBarycentrics();

    // geomInstIndex：通过 Instance 缓冲解析（多几何实例时）
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
// 3. Miss Program：标记命中环境光（光线逃离场景，击中无穷远）
// ============================================================================

#if defined(__CUDACC__) && defined(VLR_USE_OPTIX)
extern "C" __global__ void RT_MS_NAME(wavefrontMiss)() {
    using namespace vlr::shared;

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
    WavefrontHitInfo& hitInfo = wlp.hitInfoBuffer[pathIndex];

    hitInfo.reset();
    hitInfo.setHasHit(true);
    hitInfo.setHitInfinity(true);
}
#endif

// ============================================================================
// 4. Any Hit Program：Alpha 测试（镂空/透明几何，alpha < 阈值时 optixIgnoreIntersection）
// ============================================================================

#if defined(__CUDACC__) && defined(VLR_USE_OPTIX)
extern "C" __global__ void RT_AH_NAME(wavefrontAnyHitWithAlpha)() {
    using namespace vlr::shared;

    const float alphaThreshold = 0.5f;
    (void)alphaThreshold;
    // TODO: 从材质获取 alpha，若 < threshold 则 optixIgnoreIntersection()
}
#endif

// ============================================================================
// 5. Shadow Ray Miss Program：阴影光线未击中，光源可见（visibility = 1.0）
// ============================================================================

#if defined(__CUDACC__) && defined(VLR_USE_OPTIX)
extern "C" __global__ void RT_MS_NAME(wavefrontShadowMiss)() {
    using namespace vlr::shared;
    setShadowPayloadVisible();
}
#endif

// ============================================================================
// 6. Shadow Ray Any Hit Program：阴影光线击中，光源被遮挡（optixTerminateRay）
// ============================================================================

#if defined(__CUDACC__) && defined(VLR_USE_OPTIX)
extern "C" __global__ void RT_AH_NAME(wavefrontShadowAnyHit)() {
    using namespace vlr::shared;
    setShadowPayloadOccluded();
    optixTerminateRay();
}
#endif

// ============================================================================
// 7. Shadow Any Hit（带 Alpha 测试，镂空几何透射）
// ============================================================================

#if defined(__CUDACC__) && defined(VLR_USE_OPTIX)
extern "C" __global__ void RT_AH_NAME(wavefrontShadowAnyHitWithAlpha)() {
    using namespace vlr::shared;
    const float alphaThreshold = 0.5f;
    (void)alphaThreshold;
    // TODO: 从材质获取 alpha，< threshold 则 optixIgnoreIntersection() 否则 optixTerminateRay()
    setShadowPayloadOccluded();
    optixTerminateRay();
}
#endif
