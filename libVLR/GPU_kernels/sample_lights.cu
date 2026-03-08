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

    // 注意：optixTrace 只能在 OptiX RayGen/Hit/Miss 中调用，不能从普通 CUDA kernel 调用。
    // SampleLights 是 CUDA kernel，故暂不发射阴影光线，假定光源可见。
    // 后续可通过将阴影测试移至单独的 OptiX 内核实现完整的可见性测试
    (void)shadingSurfPt;
    (void)lightSurfPt;
    (void)dirToLight;
    (void)distance;
    (void)topGroup;
    return 1.0f;
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

#if PerformanceConfig::UseLightCache
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
    
    // 优化：使用 __restrict__ 提示编译器优化内存访问
    WavefrontPathState* __restrict__ pathStatePtr = &wlp.pathStateBuffer[pathIndex];
    WavefrontPathState& pathState = *pathStatePtr;

    // 优化：warp-level 早期退出
    bool isActive = pathState.isActive();
    if (warpAllInactive(isActive))
        return;
    
    // 跳过非活跃路径
    if (!isActive)
        return;

    const WavefrontHitInfo* __restrict__ hitInfoPtr = &wlp.hitInfoBuffer[pathIndex];
    const WavefrontHitInfo& hitInfo = *hitInfoPtr;
    if (!hitInfo.hasHit() || hitInfo.hitInfinity())
        return;

    const SurfacePoint* __restrict__ surfPtPtr = &wlp.surfacePointBuffer[pathIndex];
    const SurfacePoint& surfPt = *surfPtPtr;

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
    float u2 = pathState.rng.getFloat0cTo1o();
    LightSampleResult sampleResult;
    if (!sampleLight(selectResult.descriptor, surfPt.position, u0, u1, u2, &sampleResult, wlp))
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

#ifdef VLR_DEBUG_BSDF_VERBOSE
    if (pathIndex < 5) {
        printf("[GPU SampleLights] pathIndex=%u: Direction check\n", pathIndex);
        printf("  pathState.direction (world)=(%.3f,%.3f,%.3f)\n",
            pathState.direction.x, pathState.direction.y, pathState.direction.z);
        printf("  dirToLight (world)=(%.3f,%.3f,%.3f)\n",
            dirToLight.x, dirToLight.y, dirToLight.z);
        printf("  surfPt.geometricNormal (world)=(%.3f,%.3f,%.3f)\n",
            surfPt.geometricNormal.x, surfPt.geometricNormal.y, surfPt.geometricNormal.z);
        printf("  dirInLocal=(%.3f,%.3f,%.3f)\n",
            dirInLocal.x, dirInLocal.y, dirInLocal.z);
        printf("  dirOutLocal=(%.3f,%.3f,%.3f)\n",
            dirOutLocal.x, dirOutLocal.y, dirOutLocal.z);
        printf("  geomNormalLocal=(%.3f,%.3f,%.3f)\n",
            geomNormalLocal.x, geomNormalLocal.y, geomNormalLocal.z);
        float NdotIn = dot(dirInLocal, geomNormalLocal);
        float NdotOut = dot(dirOutLocal, geomNormalLocal);
        printf("  NdotIn=%.3f, NdotOut=%.3f\n", NdotIn, NdotOut);
    }
#endif

    BSDFContext bsdfCtx(matDesc, surfPt, pathState.wls);
    SampledSpectrum fs = evaluateBSDF(bsdfCtx, dirInLocal, dirOutLocal);

#ifdef VLR_DEBUG_MATERIAL
    if (pathIndex < 5) {
        BSDFType bsdfType = getBSDFType(matDesc);
        printf("[GPU SampleLights] pathIndex=%u: evaluateBSDF result\n", pathIndex);
        printf("  BSDFType=%u\n", (uint32_t)bsdfType);
        printf("  fs=(%.6f, %.6f, %.6f, %.6f)\n", 
            fs.values[0], fs.values[1], fs.values[2], fs.values[3]);
    }
#endif

    if (!fs.hasNonZero())
        return;

    // ========================================================================
    // 6. 光源 PDF 与 BSDF PDF（用于 MIS）
    // 与原始 VLR path_tracing.cu 一致：使用面积 PDF
    // ========================================================================
    // 光源面积 PDF = 选择概率 * 位置面积 PDF（原始 VLR: lightPDF = lightProb * lpResult.areaPDF）
    float lightAreaPDF = sampleResult.lightSelectProb * sampleResult.areaPDF;

    // BSDF 方向 PDF 需转换为面积 PDF 空间以与 lightAreaPDF 做 MIS
    // 原始 VLR: bsdfPDF = bsdf.evaluatePDF() * cosLight * recSquaredDistance
    float cosLight = absDot(-dirToLight, sampleResult.lightSurfPt.geometricNormal);
    float squaredDistance = distance * distance;
    float recSquaredDistance = (squaredDistance > 1e-12f) ? (1.0f / squaredDistance) : 0.0f;
    float bsdfDirPDF = getBSDFPDF(bsdfCtx, dirInLocal, dirOutLocal);
    float bsdfAreaPDF = bsdfDirPDF * cosLight * recSquaredDistance;

    // ========================================================================
    // 7. MIS 权重（Power Heuristic）
    // 原始 VLR: MISWeight = (lightPDF^2) / (lightPDF^2 + bsdfPDF^2)
    // 当光源为 delta 或 lightPDF 无穷大时，MISWeight = 1
    // ========================================================================
    float MISWeight = 1.0f;
    bool lightPDFInf = (lightAreaPDF != lightAreaPDF) || (lightAreaPDF >= 1e30f);
    if (!selectResult.descriptor.isDelta() && !lightPDFInf && lightAreaPDF > 1e-10f)
        MISWeight = computeMISWeight(lightAreaPDF, bsdfAreaPDF);

    // ========================================================================
    // 8. 几何项 G = cos(theta_shading) * cos(theta_light) / distance^2
    // 原始 VLR: G = fractionalVisibility * absDot(...) * cosLight * recSquaredDistance
    // ========================================================================
    float G = computeGeometryTerm(surfPt, sampleResult.lightSurfPt, dirToLight, squaredDistance);
    G *= fractionalVisibility;

    if (G <= 0.0f)
        return;

    // ========================================================================
    // 9. 累积直接光照贡献
    // 原始 VLR: scalarCoeff = G * MISWeight / lightPDF
    //          contribution += alpha * Le * fs * scalarCoeff
    // 即: contribution = throughput * Le * fs * G * MISWeight / lightAreaPDF
    // ========================================================================
    float invLightPDF = 1.0f;
    if (!lightPDFInf && lightAreaPDF > 1e-10f)
        invLightPDF = 1.0f / lightAreaPDF;

    SampledSpectrum contrib = pathState.throughput * emissionResult.Le * fs * G * MISWeight * invLightPDF;

    // 与原始 VLR 一致：仅当贡献有限时才累加，避免 NaN/Inf 污染输出
    if (contrib.allFinite() && contrib.hasNonZero()) {
        pathState.contribution += contrib;
    }
#ifdef VLR_DEBUG_NAN_TRACKING
    else if (!contrib.allFinite()) {
        unsigned int idx = atomicAdd(&g_vlrNanPrintCount, 1);
        if (idx < 5) {
            printf("[NaN] sample_lights: px=(%u,%u) pathLen=%u contrib=(%.4f,%.4f,%.4f) op=throughput*Le*fs*G*MIS/invPDF\n",
                   pathState.pixelX, pathState.pixelY, pathState.pathLength,
                   contrib.values[0], contrib.values[1], contrib.values[2]);
        }
    }
#endif

    // 可选：统计阴影光线数
    if (wlp.numShadowRays != nullptr) {
        atomicAdd(wlp.numShadowRays, 1u);
    }
#endif
}
