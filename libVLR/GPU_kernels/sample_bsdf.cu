// ============================================================================
// VLR Wavefront - SampleBSDF Kernel
//
// 本文件实现 Wavefront 路径追踪的 BSDF 采样内核。
// 功能：BSDF 采样、构造 BSDFQuery、处理色散材质、更新路径吞吐量、
//       生成下一跳光线、更新 PathState、将路径加入下一轮队列。
//
// 作者：VLR 开发团队
// 创建日期：2026-03-07
// 环境：CUDA 13.1, OptiX 8.0.0, VS2022
// ============================================================================

#include "../shared/kernel_common.h"
#include "../shared/path_types.h"
#include "kernel_launch.h"
#include "../shared/bsdf_common.h"
#include "../shared/geometry_common.h"
#include "../shared/material_types.h"
#include "../shared/performance_config.h"
#include "../include/vlr/basic_types.h"
#include "warp_utils.cuh"
#include "shared_memory_cache.cuh"

#include <cuda_runtime.h>
#include <cmath>

namespace {

using namespace vlr;
using namespace vlr::shared;

}  // anonymous namespace

// ============================================================================
// SampleBSDF Kernel
// ============================================================================
// 对活跃队列中的每条路径执行 BSDF 采样，生成下一跳方向，
// 更新路径吞吐量，并将路径加入下一轮活跃队列。
// 调用顺序：ProcessHits -> SampleLights -> SampleBSDF

extern "C" __global__ void sampleBSDF(
    vlr::shared::WavefrontLaunchParameters* params) {
    using namespace vlr::shared;
    WavefrontLaunchParameters& wlp = *params;

#ifdef __CUDACC__

#if PerformanceConfig::UseMaterialCache
    __shared__ MaterialCache<PerformanceConfig::MaterialCacheSize> materialCache;
    if (threadIdx.x == 0) {
        uint32_t numMaterials = wlp.numMaterials;
        if (numMaterials > PerformanceConfig::MaterialCacheSize)
            numMaterials = PerformanceConfig::MaterialCacheSize;
        for (uint32_t i = 0; i < numMaterials; ++i) {
            materialCache.materials[i] = wlp.materialDescriptorBuffer[i];
        }
        materialCache.numMaterials = numMaterials;
    }
    __syncthreads();
#endif

    // 工作索引：每个线程处理一条活跃路径
    uint32_t workIndex = blockIdx.x * blockDim.x + threadIdx.x;
    if (workIndex >= wlp.activePathQueue.size())
        return;

    uint32_t pathIndex = wlp.activePathQueue.pathIndices[workIndex];
    
    // 优化：使用 __restrict__ 提示编译器优化内存访问
    WavefrontPathState* __restrict__ pathStatePtr = &wlp.pathStateBuffer[pathIndex];
    WavefrontPathState& pathState = *pathStatePtr;

    // 优化：warp-level 早期退出
    // 如果整个 warp 都不活跃，直接返回
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

    // ========================================================================
    // 1. 获取材质和 BSDF 上下文
    // ========================================================================
    const GeometryInstance& geomInst = wlp.geomInstBuffer[hitInfo.geomInstIndex];

#if PerformanceConfig::UseMaterialCache
    const SurfaceMaterialDescriptor& matDesc = *materialCache.get(
        geomInst.materialIndex, wlp.materialDescriptorBuffer);
#else
    const SurfaceMaterialDescriptor& matDesc = wlp.materialDescriptorBuffer[geomInst.materialIndex];
#endif

#ifdef VLR_DEBUG_MATERIAL
    if (pathIndex < 10) {
        BSDFType bsdfType = getBSDFType(matDesc);
        const float* dataAsFloat = getMaterialDataAsFloats(matDesc);
        printf("[GPU SampleBSDF] pathIndex=%u, geomInstIndex=%u, materialIndex=%u\n",
            pathIndex, hitInfo.geomInstIndex, geomInst.materialIndex);
        printf("  bsdfProcedureSetIndex=%u, getBSDFType()=%u\n",
            matDesc.bsdfProcedureSetIndex, (uint32_t)bsdfType);
        printf("  Albedo: (%.3f, %.3f, %.3f)\n",
            dataAsFloat[MaterialDataLayout::AlbedoR],
            dataAsFloat[MaterialDataLayout::AlbedoG],
            dataAsFloat[MaterialDataLayout::AlbedoB]);
        printf("  Roughness: %.3f, IOR: %.3f\n",
            dataAsFloat[MaterialDataLayout::Roughness],
            dataAsFloat[MaterialDataLayout::IOR]);
        printf("  Eta: (%.3f, %.3f, %.3f)\n",
            dataAsFloat[MaterialDataLayout::EtaR],
            dataAsFloat[MaterialDataLayout::EtaG],
            dataAsFloat[MaterialDataLayout::EtaB]);
        printf("  Kappa: (%.3f, %.3f, %.3f)\n",
            dataAsFloat[MaterialDataLayout::KappaR],
            dataAsFloat[MaterialDataLayout::KappaG],
            dataAsFloat[MaterialDataLayout::KappaB]);
    }
#endif

    // 构造 BSDF 采样所需参数：入射方向（局部）、几何法线
    // 入射方向 = 光线到达表面的方向 = -pathState.direction
    Vector3D dirInLocal = surfPt.shadingFrame.toLocal(
        Vector3D(-pathState.direction.x, -pathState.direction.y, -pathState.direction.z));
    Normal3D geomNormalLocal = surfPt.shadingFrame.toLocal(surfPt.geometricNormal);

    BSDFContext bsdfCtx(matDesc, surfPt, pathState.wls, pathState.singleWlSelected());
    bsdfCtx.geomNormalLocal = geomNormalLocal;

    // ========================================================================
    // 2. BSDF 采样（使用 sampleBSDFWithU2，部分 BSDF 需三随机数）
    // ========================================================================
    // 优化：预生成随机数，减少 RNG 调用开销
    float u0 = pathState.rng.getFloat0cTo1o();
    float u1 = pathState.rng.getFloat0cTo1o();
    float u2 = pathState.rng.getFloat0cTo1o();

    BSDFSampleResult result;
    sampleBSDFWithU2(bsdfCtx, dirInLocal, u0, u1, u2, &result);

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

    // 与原始 VLR path_tracing.cu:243 完全一致：
    // if (fs == SampledSpectrum::Zero() || fsResult.dirPDF == 0.0f) return;
    // 仅检查 dirPDF == 0.0f，不使用 1e-10 等过严阈值，否则会错误终止有效路径
#ifdef __CUDACC__
    bool pdfInvalid = (result.pdf <= 0.0f || __isnanf(result.pdf) || __isinf(result.pdf));
#else
    bool pdfInvalid = (result.pdf <= 0.0f || !std::isfinite(result.pdf));
#endif
    if (result.f == SampledSpectrum::Zero() || pdfInvalid) {
        pathState.setTerminated();
        return;
    }

    // BSDF 值必须有限，避免 NaN/Inf 污染 throughput（原始 VLR 无此检查，作为额外防护）
    if (!result.f.allFinite()) {
        pathState.setTerminated();
        return;
    }

    // ========================================================================
    // 3. 处理色散材质（仅当 dispersionStrength > 0 时）
    // ========================================================================
    // 与 libWR 对比：libWR 无光谱色散，玻璃使用 RGB 直接计算。
    // isDispersiveBSDFType 内部已检查 dispersionStrength，仅实际启用色散时返回 true。
    bool isDispersive = isDispersiveBSDFType(result.sampledBSDFType, matDesc);
    
    if (isDispersive && !pathState.singleWlSelected()) {
        result.pdf /= NumSpectralSamples;
        pathState.setSingleWlSelected();
    }

    // ========================================================================
    // 4. 更新路径吞吐量：throughput *= fs * |cos| / pdf
    // ========================================================================
    // 与原始 VLR path_tracing.cu 完全一致：alpha *= fs * (|cosFactor| / dirPDF)
    // 注意：禁止对 pdf 做 clamp，否则小 pdf 时 throughput 会爆炸产生洋红色
    float cosFactor = dot(result.dirLocal, geomNormalLocal);
    float cosAbs = std::abs(cosFactor);

    pathState.throughput *= result.f * (cosAbs / result.pdf);

    // 与 libWR 一致：SpecularTransmission 折射时应用 adjoint BSDF 校正
    // （Veach 5.3：当 shading normal != geometric normal 时，cosGeometric/cosShading）
    if (result.sampledBSDFType == BSDFType_SpecularTransmission) {
        float cosShading = std::abs(result.dirLocal.z);  // shading normal = (0,0,1) in local frame
        float cosGeometric = std::abs(dot(result.dirLocal, geomNormalLocal));
        if (cosShading >= 1e-6f && cosGeometric >= 1e-6f) {
            float correction = cosGeometric / cosShading;
            pathState.throughput *= SampledSpectrum(correction);
        }
    }

#ifdef VLR_DEBUG_SPECULAR_TRANSMISSION
    // 调试：在指定像素处打印 SpecularTransmission 关键值（cmake -DVLR_DEBUG_SPECULAR_TRANSMISSION=ON）
    if (getBSDFType(matDesc) == BSDFType_SpecularTransmission &&
        pathState.pixelX == 256 && pathState.pixelY == 256 && pathState.pathLength == 1) {
        printf("[SpecTrans] px=(256,256) len=1 pdf=%.6f f=(%.4f,%.4f,%.4f) cosAbs=%.4f throughput=(%.4f,%.4f,%.4f)\n",
               result.pdf, result.f.values[0], result.f.values[1], result.f.values[2],
               cosAbs, pathState.throughput.values[0], pathState.throughput.values[1], pathState.throughput.values[2]);
    }
#endif

    // 检查吞吐量有效性
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
                printf("[NaN] sample_bsdf: px=(%u,%u) pathLen=%u throughput=(%.4f,%.4f,%.4f) op=throughput*=f*cos/pdf\n",
                       pathState.pixelX, pathState.pixelY, pathState.pathLength,
                       pathState.throughput.values[0], pathState.throughput.values[1], pathState.throughput.values[2]);
            }
        }
#endif
        pathState.setTerminated();
        return;
    }

    // ========================================================================
    // 5. 生成下一跳光线（使用 offsetRayOriginForNextBounce）
    // ========================================================================
    Vector3D dirIn = surfPt.shadingFrame.toWorld(result.dirLocal);

    pathState.origin = offsetRayOriginForNextBounce(surfPt, cosFactor);
    pathState.direction = dirIn;

    // ========================================================================
    // 6. 更新 PathState
    // ========================================================================
    pathState.prevDirPDF = result.pdf;
    pathState.prevSampledType = bsdfTypeToDirectionType(result.sampledBSDFType);
    // pathLength 已在 ProcessHits 中递增，此处不重复

    // ========================================================================
    // 7. 将路径加入下一轮队列（nextActivePathQueue）
    // ========================================================================
    wlp.nextActivePathQueue.enqueue(pathIndex);
#endif
}
