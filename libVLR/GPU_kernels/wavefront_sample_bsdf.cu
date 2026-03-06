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
#include "../shared/wavefront_types.h"
#include "../shared/bsdf_common.h"
#include "../shared/geometry_common.h"
#include "../shared/material_types.h"
#include "../include/vlr/basic_types.h"

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

extern "C" __global__ void wavefrontSampleBSDF(
    vlr::shared::WavefrontLaunchParameters* params) {
    using namespace vlr::shared;
    WavefrontLaunchParameters& wlp = *params;

#ifdef __CUDACC__
    // 工作索引：每个线程处理一条活跃路径
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

    // ========================================================================
    // 1. 获取材质和 BSDF 上下文
    // ========================================================================
    const GeometryInstance& geomInst = wlp.geomInstBuffer[hitInfo.geomInstIndex];
    const SurfaceMaterialDescriptor& matDesc = wlp.materialDescriptorBuffer[geomInst.materialIndex];

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
    float u0 = pathState.rng.getFloat0cTo1o();
    float u1 = pathState.rng.getFloat0cTo1o();
    float u2 = pathState.rng.getFloat0cTo1o();

    BSDFSampleResult result;
    sampleBSDFWithU2(bsdfCtx, dirInLocal, u0, u1, u2, &result);

    // 检查采样结果是否有效
    if (!result.isValid() || result.pdf <= 0.0f) {
        pathState.setTerminated();
        return;
    }

    if (!result.f.hasNonZero()) {
        pathState.setTerminated();
        return;
    }

    // ========================================================================
    // 3. 处理色散材质
    // ========================================================================
    // 色散材质（如玻璃透射）需在首次采样后选择单一波长，
    // 并相应调整 PDF（除以光谱分量数）
    if (isDispersiveBSDFType(result.sampledBSDFType) && !pathState.singleWlSelected()) {
        result.pdf /= NumSpectralSamples;
        pathState.setSingleWlSelected();
    }

    // ========================================================================
    // 4. 更新路径吞吐量：throughput *= fs / pdf
    // ========================================================================
    // 渲染方程中的 cos 项：|cos(theta_out)|
    float cosFactor = dot(result.dirLocal, geomNormalLocal);

    // 避免除零与无效值
    float pdfSafe = (result.pdf > 1e-8f) ? result.pdf : 1e-8f;
    float cosAbs = std::abs(cosFactor);

    pathState.throughput *= result.f * (cosAbs / pdfSafe);

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
