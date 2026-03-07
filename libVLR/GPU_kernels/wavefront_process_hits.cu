// ============================================================================
// VLR Wavefront - ProcessHits Kernel
//
// 本文件实现 Wavefront 路径追踪的命中处理内核。
// 功能：表面点计算、BSDF/EDF 评估、隐式光源采样、路径终止、材质分类、Denoiser 辅助缓冲区。
//
// 作者：VLR 开发团队
// 创建日期：2026-03-07
// 环境：CUDA 13.1, OptiX 8.0.0, VS2022
// ============================================================================

#include "../shared/wavefront_types.h"
#include "../shared/geometry_common.h"
#include "../shared/geometry_types.h"
#include "../shared/bsdf_common.h"
#include "../shared/material_types.h"
#include "../shared/texture_common.h"
#include "../include/vlr/basic_types.h"

#include <cuda_runtime.h>
#include <cmath>

namespace {

using namespace vlr;
using namespace vlr::shared;

/// 计算隐式光源采样的 MIS 权重
/// 当路径通过 BSDF 采样到达发光表面时，与显式光源采样做平衡启发式 MIS
CUDA_DEVICE_FUNCTION CUDA_INLINE float computeImplicitLightMISWeight(
    const WavefrontPathState& pathState,
    float hypAreaPDF,
    float cosOutLocal) {

    // 若前一跳为 delta（镜面），显式光源采样不可能产生该路径，MIS = 1
    if (pathState.prevSampledType.isDelta())
        return 1.0f;

    // 第一跳（直接相机）时无需 MIS
    if (pathState.pathLength <= 1)
        return 1.0f;

    // Power Heuristic: w_bsdf = pdf_bsdf^2 / (pdf_bsdf^2 + pdf_light^2)
    // 此处为隐式命中：我们通过 BSDF 到达，故使用 prevDirPDF 作为 pdf_bsdf
    // pdf_light 需要光源选择的概率，简化实现中若无法获取则用 1
    float pdfBSDF = pathState.prevDirPDF;
    if (pdfBSDF <= 0.0f) return 1.0f;

    // 简化的光源 PDF：假设均匀选择，使用面积 PDF 近似
    // 完整实现需从 lightInstDist 获取 instProb 和 geomInstProb
    float cosTerm = std::abs(cosOutLocal);
    if (cosTerm < 1e-6f) return 1.0f;
    float pdfLight = hypAreaPDF / cosTerm;

    return powerHeuristicMIS(pdfBSDF, pdfLight);
}


/// 处理环境光命中（光线击中无穷远或 Miss）
/// 评估环境贴图并累积贡献
CUDA_DEVICE_FUNCTION CUDA_INLINE void processEnvironmentHit(
    WavefrontPathState& pathState,
    const WavefrontHitInfo& hitInfo,
    WavefrontLaunchParameters& wlp) {

    if (wlp.envLightInstIndex == 0xFFFFFFFF)
        return;

    const Instance& inst = wlp.instBuffer[wlp.envLightInstIndex];
    if (inst.numGeomInsts == 0)
        return;

    uint32_t geomInstIdx = inst.geomInstIndices[0];
    const GeometryInstance& geomInst = wlp.geomInstBuffer[geomInstIdx];

    if (geomInst.importance <= 0.0f)
        return;

    // 构造无穷远表面点（方向即击中点）
    SurfacePoint surfPt;
    surfPt.position = Point3D(pathState.direction.x, pathState.direction.y, pathState.direction.z);
    surfPt.atInfinity = true;
    surfPt.geometricNormal = Vector3D(-pathState.direction.x, -pathState.direction.y, -pathState.direction.z);

    float theta, phi;
    pathState.direction.toPolarYUp(&theta, &phi);

    float sinPhi, cosPhi;
    sinPhi = std::sin(phi);
    cosPhi = std::cos(phi);
    Vector3D texCoord0Dir = normalize(Vector3D(-cosPhi, 0.0f, -sinPhi));
    surfPt.shadingFrame = ReferenceFrame(texCoord0Dir, surfPt.geometricNormal);

    phi += inst.rotationPhi;
    phi = phi - std::floor(phi / VLR_M_2PI) * VLR_M_2PI;
    surfPt.texCoord = TexCoord2D(phi / VLR_M_2PI, theta / VLR_M_PI);

    // 评估环境光 EDF
    const SurfaceMaterialDescriptor& matDesc = wlp.materialDescriptorBuffer[geomInst.materialIndex];
    SampledSpectrum spEmittance = evaluateEmittance(matDesc);

    if (!spEmittance.hasNonZero())
        return;

    // 出射方向：从环境球面朝向相机（与入射光线相反）
    Vector3D dirOutLocal = surfPt.shadingFrame.toLocal(Vector3D(-pathState.direction.x, -pathState.direction.y, -pathState.direction.z));
    EDFContext edfCtx(matDesc, surfPt, pathState.wls);
    EDFEvaluateResult edfResult = evaluateEDF(edfCtx, dirOutLocal);

    if (!edfResult.hasEmission)
        return;

    SampledSpectrum Le = edfResult.Le;

    // MIS 权重（简化：环境光采样难以精确 PDF，用 1）
    float MISWeight = 1.0f;
    if (!pathState.prevSampledType.isDelta() && pathState.pathLength > 1) {
        float bsdfPDF = pathState.prevDirPDF;
        float sinTheta = std::sin(theta);
        float sinThetaSafe = (sinTheta > 1e-6f) ? sinTheta : 1e-6f;
        float envAreaPDF = 1.0f / (VLR_M_2PI * VLR_M_PI * sinThetaSafe);
        float lightPDF = envAreaPDF / std::abs(dirOutLocal.z);
        if (lightPDF > 0.0f)
            MISWeight = powerHeuristicMIS(bsdfPDF, lightPDF);
    }

    pathState.contribution += pathState.throughput * Le * MISWeight;
}


/// 处理发光表面命中（隐式光源采样）
/// 当光线直接击中区域光表面时累积贡献
CUDA_DEVICE_FUNCTION CUDA_INLINE void processEmissiveSurface(
    WavefrontPathState& pathState,
    const SurfacePoint& surfPt,
    const GeometryInstance& geomInst,
    float hypAreaPDF,
    WavefrontLaunchParameters& wlp) {

    const SurfaceMaterialDescriptor& matDesc = wlp.materialDescriptorBuffer[geomInst.materialIndex];

    if (!materialHasEmission(matDesc))
        return;

    SampledSpectrum spEmittance = evaluateEmittance(matDesc);
    if (!spEmittance.hasNonZero())
        return;

    Vector3D dirOutLocal = surfPt.shadingFrame.toLocal(Vector3D(-pathState.direction.x, -pathState.direction.y, -pathState.direction.z));
    EDFContext edfCtx(matDesc, surfPt, pathState.wls);
    EDFEvaluateResult edfResult = evaluateEDF(edfCtx, dirOutLocal);

    if (!edfResult.hasEmission)
        return;

    SampledSpectrum Le = edfResult.Le;
    float MISWeight = computeImplicitLightMISWeight(pathState, hypAreaPDF, dirOutLocal.z);

    pathState.contribution += pathState.throughput * Le * MISWeight;
    pathState.setHitEmissive();
}


}  // anonymous namespace


// ============================================================================
// ProcessHits Kernel
// ============================================================================

extern "C" __global__ void wavefrontProcessHits(
    vlr::shared::WavefrontLaunchParameters* params) {

    using namespace vlr::shared;

#ifdef __CUDACC__
    WavefrontLaunchParameters& wlp = *params;

    // 工作索引：每个线程处理一条活跃路径
    uint32_t workIndex = blockIdx.x * blockDim.x + threadIdx.x;
    uint32_t activeCount = wlp.activePathQueue.size();

    if (workIndex >= activeCount)
        return;

    uint32_t pathIndex = wlp.activePathQueue.pathIndices[workIndex];
    WavefrontPathState& pathState = wlp.pathStateBuffer[pathIndex];
    WavefrontHitInfo& hitInfo = wlp.hitInfoBuffer[pathIndex];

    if (!pathState.isActive())
        return;

    // 无有效命中时跳过（不应发生，因 TraceRays 只为活跃路径发射）
    if (!hitInfo.hasHit()) {
        pathState.setTerminated();
        return;
    }

    // ========================================================================
    // 1. 环境光命中：光线击中无穷远 / Miss
    // ========================================================================
    if (hitInfo.hitInfinity()) {
        processEnvironmentHit(pathState, hitInfo, wlp);
        pathState.setTerminated();
        return;
    }

    // ========================================================================
    // 2. 表面点计算（decodeHitPoint + computeSurfacePoint）
    // ========================================================================
    SurfacePoint surfPt;
    float hypAreaPDF = 1.0f;

    if (wlp.vertexPositions != nullptr) {
        HitPointDecodeInput input(hitInfo.instIndex, hitInfo.geomInstIndex,
                                 hitInfo.primIndex, hitInfo.u, hitInfo.v);

        TriangleMeshVertexData vertexData(wlp.vertexPositions,
                                         wlp.vertexNormals,
                                         wlp.vertexTexCoords);

        GeometryDecodeContext ctx(
            &wlp.geomInstBuffer[hitInfo.geomInstIndex],
            &wlp.instBuffer[hitInfo.instIndex],
            vertexData);

        computeSurfacePointBasic(input, ctx, &surfPt, &hypAreaPDF);

        // 应用法线贴图（若有纹理和材质绑定）
        if (wlp.textureDescriptorBuffer != nullptr && wlp.materialNormalMapIndices != nullptr) {
            const uint32_t matIdx = wlp.geomInstBuffer[hitInfo.geomInstIndex].materialIndex;
            const uint32_t normalMapTexIdx = wlp.materialNormalMapIndices[matIdx];
            TextureSampler normSampler = getNormalMapSampler(
                wlp.textureDescriptorBuffer, normalMapTexIdx, TextureFilter_Linear);
            const TextureSampler* normPtr = normSampler.isValid() ? &normSampler : nullptr;
            applyBumpMapping(Normal3D(0, 0, 1), &surfPt, normPtr);
        }
    } else {
        // 无顶点数据时：使用简化几何信息
        const GeometryInstance& geomInst = wlp.geomInstBuffer[hitInfo.geomInstIndex];
        const Instance& inst = wlp.instBuffer[hitInfo.instIndex];

        if (geomInst.geomType == GeometryType_TriangleMesh &&
            geomInst.asTriMesh.triangleBuffer != nullptr) {

            const Triangle& tri = geomInst.asTriMesh.triangleBuffer[hitInfo.primIndex];
            hypAreaPDF = (tri.area > 0.0f) ? (1.0f / tri.area) : 1.0f;

            // 无法插值顶点属性时，使用三角形质心（需顶点位置，此处占位）
            surfPt.position = Point3D(0, 0, 0);  // 占位，需从顶点插值
            surfPt.geometricNormal = Normal3D(0, 1, 0);
            surfPt.shadingFrame = ReferenceFrame(Vector3D(1, 0, 0), surfPt.geometricNormal);
            surfPt.texCoord = TexCoord2D(hitInfo.u, hitInfo.v);
            surfPt.atInfinity = false;
        } else {
            hypAreaPDF = 1.0f;
            surfPt.atInfinity = false;
        }
    }

    // 存储表面点供后续 SampleLights 和 SampleBSDF 使用
    wlp.surfacePointBuffer[pathIndex] = surfPt;

    // ========================================================================
    // 3. 获取材质并评估
    // ========================================================================
    const GeometryInstance& geomInst = wlp.geomInstBuffer[hitInfo.geomInstIndex];
    const SurfaceMaterialDescriptor& matDesc = wlp.materialDescriptorBuffer[geomInst.materialIndex];

    // BSDFContext 将在 SampleBSDF kernel 中从 surfacePointBuffer 重建
    // 此处 surfacePointBuffer 已填充，SampleLights/SampleBSDF 可直接使用

    // ========================================================================
    // 4. EDF 评估与隐式光源采样（命中发光表面）
    // ========================================================================
    processEmissiveSurface(pathState, surfPt, geomInst, hypAreaPDF, wlp);

    // ========================================================================
    // 5. 路径长度检查与终止
    // ========================================================================
    pathState.pathLength++;

    if (pathState.pathLength >= wlp.maxPathLength) {
        pathState.setMaxLengthReached();
        pathState.setTerminated();
        return;
    }

    // ========================================================================
    // 6. 俄罗斯轮盘赌
    // ========================================================================
    if (pathState.pathLength >= WavefrontConfig::RRStartDepth) {
        float importance = pathState.throughput.importance(pathState.wls.selectedLambdaIndex());
        float continueProb = (pathState.initImportance > 1e-8f)
            ? ::vlr::vlr_min(importance / pathState.initImportance, 1.0f)
            : 1.0f;

        if (continueProb < WavefrontConfig::RRThreshold) {
            pathState.setTerminated();
            return;
        }

        if (pathState.rng.getFloat0cTo1o() >= continueProb) {
            pathState.setTerminated();
            return;
        }

        pathState.throughput /= continueProb;
    }

    // ========================================================================
    // 7. 材质分类（调用 classifyMaterial 等价逻辑）
    // ========================================================================
    MaterialCategory category = bsdfTypeToMaterialCategory(getBSDFType(matDesc));
    pathState.materialCategory = category;

    // 若 wavefront_common 的 classifyMaterial 可用（依赖 BSDF），可替换为：
    // BSDF<TransportMode::Radiance> bsdf(&matDesc);
    // category = classifyMaterial(bsdf);
    // 当前使用 material_types 的 bsdfTypeToMaterialCategory 保持兼容

    // ========================================================================
    // 8. Denoiser 辅助缓冲区更新
    // ========================================================================
    uint32_t stride = (wlp.imageStrideInPixels > 0) ? wlp.imageStrideInPixels : wlp.imageSize.x;
    uint32_t pixelIdx = pathState.pixelY * stride + pathState.pixelX;

    if (wlp.accumAlbedoBuffer != nullptr) {
        // 反照率：从材质 AlbedoR/G/B 直接获取（用于 Denoiser）
        const float* d = getMaterialDataAsFloats(matDesc);
        float r = d[MaterialDataLayout::AlbedoR];
        float g = d[MaterialDataLayout::AlbedoG];
        float b = d[MaterialDataLayout::AlbedoB];
        wlp.accumAlbedoBuffer[pixelIdx].r = r;
        wlp.accumAlbedoBuffer[pixelIdx].g = g;
        wlp.accumAlbedoBuffer[pixelIdx].b = b;
    }

    if (wlp.accumNormalBuffer != nullptr) {
        // 法线：着色法线（世界空间）
        wlp.accumNormalBuffer[pixelIdx].x = surfPt.shadingFrame.z.x;
        wlp.accumNormalBuffer[pixelIdx].y = surfPt.shadingFrame.z.y;
        wlp.accumNormalBuffer[pixelIdx].z = surfPt.shadingFrame.z.z;
    }
#endif
}
