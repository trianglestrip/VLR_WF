// ============================================================================
// VLR Wavefront - SampleLights Kernel (Next Event Estimation)
//
// 本文件实现 Wavefront 路径追踪的显式光源采样内核（NEE）。
// 功能：光源选择、位置采样、辐射评估、可见性测试、BSDF 评估、MIS 权重、
//       几何项计算、累积直接光照贡献。跳过 delta 材质。
//
// 作者：VLR 开发团队
// 创建日期：2026-03-07
// 环境：CUDA 13.1, OptiX 8.0.0, VS2022
// ============================================================================

#include "../shared/kernel_common.h"
#include "../shared/path_types.h"
#include "../shared/render_common.h"
#include "../shared/light_common.h"
#include "../shared/light_types.h"
#include "../shared/bsdf_common.h"
#include "../shared/geometry_common.h"
#include "../shared/material_types.h"
#include "../include/vlr/basic_types.h"

#include <cuda_runtime.h>
#include <cfloat>
#include <cmath>

// OptiX 阴影光线追踪（当可用时）
#if defined(__CUDACC__) && defined(VLR_USE_OPTIX)
    #include <optix_device.h>
#endif

namespace {

using namespace vlr;
using namespace vlr::shared;

// ============================================================================
// 可见性测试：发射阴影光线，检查着色点与光源之间是否遮挡
// ============================================================================

/// 测试着色点到光源的可见性
/// 从着色点沿 dirToLight 方向发射阴影光线，若未命中任何几何则可见
///
/// @param shadingSurfPt    着色点（表面点）
/// @param lightSurfPt      光源表面点
/// @param dirToLight       从着色点指向光源的方向（已归一化）
/// @param distance        着色点到光源的距离
/// @param topGroup        OptiX 可遍历句柄
/// @return fractionalVisibility: 1.0=完全可见, 0.0=完全遮挡
CUDA_DEVICE_FUNCTION CUDA_INLINE float testVisibility(
    const SurfacePoint& shadingSurfPt,
    const SurfacePoint& lightSurfPt,
    const Vector3D& dirToLight,
    float distance,
    uint64_t topGroup) {

#if defined(__CUDACC__) && defined(VLR_USE_OPTIX)
    if (topGroup == 0)
        return 1.0f;  // 无可遍历场景时视为可见

    // 沿法线偏移光线起点，避免自相交
    Normal3D offsetNormal = shadingSurfPt.geometricNormal;
    float cosToLight = dot(dirToLight, offsetNormal);
    if (cosToLight < 0.0f)
        offsetNormal = -offsetNormal;

    Point3D rayOrigin = offsetRayOrigin(shadingSurfPt.position, offsetNormal);

    // 阴影光线：tmin 避免自相交，tmax 为到光源距离减去小偏移
    float tmin = 1e-5f;
    float tmax = distance - 1e-4f;
    if (tmax <= tmin)
        return 1.0f;  // 光源太近，视为可见

    // OptiX  trace：阴影光线使用 terminate-on-first-hit 语义
    // 若命中任何几何则被遮挡；若 miss 则可见
    // 注意：SBT 布局需与 Pipeline 一致，此处使用占位参数
    // 完整集成时需从 WavefrontLaunchParameters 传入 shadowRaySbtOffset 等
    constexpr unsigned int RAY_FLAG_TERMINATE_ON_FIRST_HIT = 1u;

    // OptiX 8 optixTrace: (handle, float3 origin, float3 direction, tmin, tmax, rayTime, mask, flags, sbtOffset, sbtStride, missSbtIndex, payload&...)
    // 阴影 payload：初值 0，Miss 设为 1(可见)，AnyHit 设为 0(遮挡)，payload 通过引用修改
    float3 ro = make_float3(rayOrigin.x, rayOrigin.y, rayOrigin.z);
    float3 rd = make_float3(dirToLight.x, dirToLight.y, dirToLight.z);
    unsigned int shadowPayload = 0u;
    optixTrace(topGroup, ro, rd, tmin, tmax, 0.0f,
               0xFF, RAY_FLAG_TERMINATE_ON_FIRST_HIT,
               0, 1, 1,  // sbtOffset, sbtStride, missSbtIndex（阴影 SBT）
               shadowPayload);

    return (shadowPayload != 0) ? 1.0f : 0.0f;  // 非零=Miss 设置的可见

#else
    // 无 OptiX 时：占位实现，假定可见（用于编译/测试）
    (void)shadingSurfPt;
    (void)lightSurfPt;
    (void)dirToLight;
    (void)distance;
    (void)topGroup;
    return 1.0f;
#endif
}

}  // anonymous namespace

// ============================================================================
// SampleLights Kernel
// ============================================================================
// 对活跃队列中的每条路径执行 Next Event Estimation
// 调用顺序：ProcessHits -> SampleLights -> SampleBSDF

extern "C" __global__ void sampleLights(
    vlr::shared::WavefrontLaunchParameters* params) {
    using namespace vlr::shared;
    WavefrontLaunchParameters& wlp = *params;

#ifdef __CUDACC__
    uint32_t workIndex = blockIdx.x * blockDim.x + threadIdx.x;
    if (workIndex >= wlp.activePathQueue.size())
        return;

    uint32_t pathIndex = wlp.activePathQueue.pathIndices[workIndex];
    WavefrontPathState& pathState = wlp.pathStateBuffer[pathIndex];

    // 跳过非活跃路径
    if (!pathState.isActive())
        return;

    const WavefrontHitInfo& hitInfo = wlp.hitInfoBuffer[pathIndex];
    if (!hitInfo.hasHit() || hitInfo.hitInfinity())
        return;

    const SurfacePoint& surfPt = wlp.surfacePointBuffer[pathIndex];

    // 获取材质和 BSDF 上下文
    const GeometryInstance& geomInst = wlp.geomInstBuffer[hitInfo.geomInstIndex];
    const SurfaceMaterialDescriptor& matDesc = wlp.materialDescriptorBuffer[geomInst.materialIndex];

    // 优化：跳过 delta 材质（完美镜面等），NEE 对 delta 无贡献
    if (materialIsDelta(matDesc))
        return;

    // ========================================================================
    // 1. 选择光源
    // ========================================================================
    float uLight = pathState.rng.getFloat0cTo1o();
    LightSelectResult selectResult;
    if (!selectLight(uLight, &selectResult, wlp))
        return;

    // ========================================================================
    // 2. 采样光源位置
    // ========================================================================
    float u0 = pathState.rng.getFloat0cTo1o();
    float u1 = pathState.rng.getFloat0cTo1o();
    LightSampleResult sampleResult;
    if (!sampleLight(selectResult.descriptor, surfPt.position, u0, u1, &sampleResult, wlp))
        return;

    sampleResult.lightSelectProb = selectResult.selectProb;

    // 计算从着色点到光源的方向和距离
    Vector3D dirToLight = sampleResult.lightSurfPt.position - surfPt.position;
    float distance = length(dirToLight);
    if (distance < 1e-8f)
        return;
    dirToLight = dirToLight / distance;

    // ========================================================================
    // 3. 评估光源辐射
    // ========================================================================
    Vector3D dirToShading = -dirToLight;  // 从光源指向着色点
    LightEmissionResult emissionResult;
    if (!evaluateLightEmission(selectResult.descriptor, sampleResult.lightSurfPt,
                              dirToShading, pathState.wls, &emissionResult, wlp))
        return;

    if (!emissionResult.Le.hasNonZero())
        return;

    // ========================================================================
    // 4. 可见性测试（发射阴影光线）
    // ========================================================================
    float fractionalVisibility = testVisibility(
        surfPt, sampleResult.lightSurfPt, dirToLight, distance, wlp.topGroup);

    if (fractionalVisibility <= 0.0f)
        return;

    // ========================================================================
    // 5. BSDF 评估（用于渲染方程中的 f 项，以及 MIS）
    // ========================================================================
    Vector3D dirInLocal = surfPt.shadingFrame.toLocal(-pathState.direction);
    Vector3D dirOutLocal = surfPt.shadingFrame.toLocal(dirToLight);
    Normal3D geomNormalLocal = surfPt.shadingFrame.toLocal(surfPt.geometricNormal);

    BSDFContext bsdfCtx(matDesc, surfPt, pathState.wls);
    SampledSpectrum fs = evaluateBSDF(bsdfCtx, dirInLocal, dirOutLocal);

    if (!fs.hasNonZero())
        return;

    // ========================================================================
    // 6. 光源 PDF 与 BSDF PDF（用于 MIS）
    // ========================================================================
    float lightSolidAnglePDF = computeLightPDF(
        selectResult.descriptor,
        sampleResult.lightSurfPt,
        surfPt.position,
        dirToLight,
        sampleResult.lightSelectProb,
        sampleResult.areaPDF,
        wlp);

    float bsdfPDF = getBSDFPDF(bsdfCtx, dirInLocal, dirOutLocal);

    // ========================================================================
    // 7. MIS 权重（Power Heuristic）
    // ========================================================================
    float MISWeight = computeMISWeight(lightSolidAnglePDF, bsdfPDF);

    // ========================================================================
    // 8. 几何项 G = cos(theta_shading) * cos(theta_light) / distance^2
    // ========================================================================
    float squaredDistance = distance * distance;
    float G = computeGeometryTerm(surfPt, sampleResult.lightSurfPt, dirToLight, squaredDistance);
    G *= fractionalVisibility;

    if (G <= 0.0f)
        return;

    // ========================================================================
    // 9. 累积直接光照贡献
    // ========================================================================
    // 渲染方程 NEE 项: L = throughput * Le * fs * G * MISWeight / lightPDF
    // lightPDF 为立体角 PDF；点光源（delta）时 PDF 无穷大，取 invLightPDF=1
    float invLightPDF = 1.0f;
    // 使用设备端兼容的有限性检查（避免 std::isfinite）
    bool lightPDFFinite = (lightSolidAnglePDF == lightSolidAnglePDF) &&
        (lightSolidAnglePDF > -1e30f) && (lightSolidAnglePDF < 1e30f);
    if (lightPDFFinite && lightSolidAnglePDF > 1e-10f)
        invLightPDF = 1.0f / lightSolidAnglePDF;

    SampledSpectrum contrib = pathState.throughput * emissionResult.Le * fs * G * MISWeight * invLightPDF;

    // 检查贡献有效性（非零且有限）（使用设备端兼容的有限性检查）
    bool isFinite = true;
    for (int i = 0; i < NumSpectralSamples && isFinite; ++i) {
        float v = contrib.values[i];
        isFinite = isFinite && (v == v) && (v > -1e30f) && (v < 1e30f);
    }
    if (contrib.hasNonZero() && isFinite) {
        pathState.contribution += contrib;
    }

    // 可选：统计阴影光线数
    if (wlp.numShadowRays != nullptr) {
        atomicAdd(wlp.numShadowRays, 1u);
    }
#endif
}
