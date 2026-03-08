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

#define VLR_DEBUG_PROCESS_HITS 1

#include "../shared/path_types.h"
#include "../shared/geometry_common.h"
#include "../shared/env_importance.h"
#include "kernel_launch.h"
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
    if (pdfLight <= 0.0f || pdfLight >= 1e30f) return 1.0f;

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
    const float phiOrig = phi;

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

    // MIS 权重：BSDF 采样 vs 环境光重要性采样
    float MISWeight = 1.0f;
    if (!pathState.prevSampledType.isDelta() && pathState.pathLength > 1) {
        float bsdfPDF = pathState.prevDirPDF;
        float cosOutSafe = (std::abs(dirOutLocal.z) > 1e-6f) ? std::abs(dirOutLocal.z) : 1e-6f;
        float lightPDF;
        if (wlp.envImportanceMap.isValid()) {
            // 使用未旋转的纹理坐标（importance map 基于原始贴图）
            float u = phiOrig / VLR_M_2PI;
            float v = theta / VLR_M_PI;
            lightPDF = wlp.envImportanceMap.evaluatePDF(u, v) / cosOutSafe;
        } else {
            float sinTheta = std::sin(theta);
            float sinThetaSafe = (sinTheta > 1e-6f) ? sinTheta : 1e-6f;
            float envAreaPDF = 1.0f / (VLR_M_2PI * VLR_M_PI * sinThetaSafe);
            lightPDF = envAreaPDF / cosOutSafe;
        }
        if (lightPDF > 0.0f && lightPDF < 1e30f)
            MISWeight = powerHeuristicMIS(bsdfPDF, lightPDF);
    }

    SampledSpectrum envContrib = pathState.throughput * Le * MISWeight;
    if (envContrib.allFinite())
        pathState.contribution += envContrib;
#ifdef VLR_DEBUG_NAN_TRACKING
    else {
        unsigned int idx = atomicAdd(&g_vlrNanPrintCount, 1);
        if (idx < 5) {
            printf("[NaN] process_hits(env): px=(%u,%u) pathLen=%u envContrib=(%.4f,%.4f,%.4f) op=throughput*Le*MIS\n",
                   pathState.pixelX, pathState.pixelY, pathState.pathLength,
                   envContrib.values[0], envContrib.values[1], envContrib.values[2]);
        }
    }
#endif
}


/// 处理发光表面命中（隐式光源采样）
/// 当光线直接击中区域光表面时累积贡献
CUDA_DEVICE_FUNCTION CUDA_INLINE void processEmissiveSurface(
    WavefrontPathState& pathState,
    const SurfacePoint& surfPt,
    const GeometryInstance& geomInst,
    float hypAreaPDF,
    WavefrontLaunchParameters& wlp) {

#ifdef VLR_DEBUG_PROCESS_HITS
    if (pathState.pixelX == 256 && pathState.pixelY == 256 && pathState.pathLength == 0) {
        printf("[GPU ProcessEmissive ENTRY] px=(256,256): geomInstIndex=%u, materialIndex=%u\n",
            geomInst.instIndex, geomInst.materialIndex);
    }
#endif

    const SurfaceMaterialDescriptor& matDesc = wlp.materialDescriptorBuffer[geomInst.materialIndex];

#ifdef VLR_DEBUG_PROCESS_HITS
    if (pathState.pixelX == 256 && pathState.pixelY == 256 && pathState.pathLength == 0) {
        printf("[GPU ProcessEmissive] px=(256,256): materialIndex=%u, hasEmission=%d\n",
            geomInst.materialIndex, materialHasEmission(matDesc) ? 1 : 0);
    }
#endif

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

    SampledSpectrum emissiveContrib = pathState.throughput * Le * MISWeight;
    
#ifdef VLR_DEBUG_PROCESS_HITS
    if (pathState.pixelX == 256 && pathState.pixelY == 256 && pathState.pathLength == 0) {
        printf("[GPU ProcessEmissive] px=(256,256): Le=(%.3f,%.3f,%.3f,%.3f), throughput=(%.3f,%.3f,%.3f,%.3f), MIS=%.3f, contrib=(%.3f,%.3f,%.3f,%.3f)\n",
            Le.values[0], Le.values[1], Le.values[2], Le.values[3],
            pathState.throughput.values[0], pathState.throughput.values[1], pathState.throughput.values[2], pathState.throughput.values[3],
            MISWeight,
            emissiveContrib.values[0], emissiveContrib.values[1], emissiveContrib.values[2], emissiveContrib.values[3]);
    }
#endif
    
    if (emissiveContrib.allFinite())
        pathState.contribution += emissiveContrib;
#ifdef VLR_DEBUG_NAN_TRACKING
    else {
        unsigned int idx = atomicAdd(&g_vlrNanPrintCount, 1);
        if (idx < 5) {
            printf("[NaN] process_hits(emissive): px=(%u,%u) pathLen=%u emissiveContrib=(%.4f,%.4f,%.4f) op=throughput*Le*MIS\n",
                   pathState.pixelX, pathState.pixelY, pathState.pathLength,
                   emissiveContrib.values[0], emissiveContrib.values[1], emissiveContrib.values[2]);
        }
    }
#endif
    pathState.setHitEmissive();
}


}  // anonymous namespace


// ============================================================================
// ProcessHits Kernel
// ============================================================================

extern "C" __global__ void processHits(
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

#ifdef VLR_DEBUG_PROCESS_HITS
    if (pathIndex == 0) {
        printf("[GPU ProcessHits] pathIndex=0: isActive=%d, hasHit=%d, hitInfinity=%d, geomInstIndex=%u\n",
            pathState.isActive() ? 1 : 0, hitInfo.hasHit() ? 1 : 0, hitInfo.hitInfinity() ? 1 : 0, hitInfo.geomInstIndex);
    }
#endif

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

        // Faceforward：确保法线朝向入射光线，使 NEE/BSDF 在正确半球内
        {
            Vector3D rayDir = pathState.direction;
            if (dot(surfPt.geometricNormal, rayDir) > 0.0f) {
                surfPt.geometricNormal = -surfPt.geometricNormal;
                surfPt.shadingFrame = ReferenceFrame(surfPt.shadingFrame.x, surfPt.geometricNormal);
            }
        }

        // 应用法线贴图（若有纹理和材质绑定，支持纹理坐标变换与法线强度）
        if (wlp.textureDescriptorBuffer != nullptr && wlp.materialNormalMapIndices != nullptr) {
            const uint32_t matIdxForNorm = wlp.geomInstBuffer[hitInfo.geomInstIndex].materialIndex;
            const uint32_t normalMapTexIdx = wlp.materialNormalMapIndices[matIdxForNorm];
            TextureSampler normSampler = getNormalMapSampler(
                wlp.textureDescriptorBuffer, normalMapTexIdx, TextureFilter_Linear);
            if (normSampler.isValid()) {
                float normU, normV;
                if (wlp.materialTextureParamsBuffer != nullptr) {
                    const MaterialTextureParams& mtp = wlp.materialTextureParamsBuffer[matIdxForNorm];
                    transformTexCoord(surfPt.texCoord.x, surfPt.texCoord.y,
                        mtp.scaleU, mtp.scaleV, mtp.offsetU, mtp.offsetV, &normU, &normV);
                    applyBumpMappingWithUV(Normal3D(0, 0, 1), &surfPt, &normSampler, normU, normV, mtp.normalScale);
                } else {
                    applyBumpMapping(Normal3D(0, 0, 1), &surfPt, &normSampler);
                }
            }
        }
    } else {
        // 无顶点数据时：使用简化几何信息
        const GeometryInstance& geomInst = wlp.geomInstBuffer[hitInfo.geomInstIndex];

        if (geomInst.geomType == GeometryType_TriangleMesh &&
            geomInst.asTriMesh.triangleBuffer != nullptr) {

            const Triangle& tri = geomInst.asTriMesh.triangleBuffer[hitInfo.primIndex];
            hypAreaPDF = (tri.area > 0.0f) ? (1.0f / tri.area) : 1.0f;

            // 无法插值顶点属性时，使用光线参数计算交点：origin + direction * t
            // 与原始 VLR 的射线-三角形求交逻辑一致
            surfPt.position = Point3D(
                pathState.origin.x + pathState.direction.x * hitInfo.t,
                pathState.origin.y + pathState.direction.y * hitInfo.t,
                pathState.origin.z + pathState.direction.z * hitInfo.t);
            surfPt.geometricNormal = Normal3D(0, 1, 0);
            surfPt.shadingFrame = ReferenceFrame(Vector3D(1, 0, 0), surfPt.geometricNormal);
            surfPt.texCoord = TexCoord2D(hitInfo.u, hitInfo.v);
            surfPt.atInfinity = false;
        } else {
            hypAreaPDF = 1.0f;
            // 非三角形网格：仍用光线参数计算交点
            surfPt.position = Point3D(
                pathState.origin.x + pathState.direction.x * hitInfo.t,
                pathState.origin.y + pathState.direction.y * hitInfo.t,
                pathState.origin.z + pathState.direction.z * hitInfo.t);
            surfPt.geometricNormal = Normal3D(0, 1, 0);
            surfPt.shadingFrame = ReferenceFrame(Vector3D(1, 0, 0), surfPt.geometricNormal);
            surfPt.texCoord = TexCoord2D(hitInfo.u, hitInfo.v);
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
    const uint32_t matIdx = geomInst.materialIndex;

    // ========================================================================
    // 3.1 纹理采样：BaseColor、Roughness、Metallic
    // ========================================================================
    if (wlp.pathTexturedParamsBuffer != nullptr) {
        PathTexturedMaterialParams& tp = wlp.pathTexturedParamsBuffer[pathIndex];
        tp.flags = 0;

        // 纹理坐标变换（使用材质级 scale/offset，若无则恒等变换）
        float tu, tv;
        if (wlp.materialTextureParamsBuffer != nullptr) {
            const MaterialTextureParams& mtp = wlp.materialTextureParamsBuffer[matIdx];
            transformTexCoord(surfPt.texCoord.x, surfPt.texCoord.y,
                mtp.scaleU, mtp.scaleV, mtp.offsetU, mtp.offsetV, &tu, &tv);
        } else {
            transformTexCoordIdentity(surfPt.texCoord.x, surfPt.texCoord.y, &tu, &tv);
        }

        const float* d = getMaterialDataAsFloats(matDesc);

        // 采样 BaseColor 纹理
        if (wlp.textureDescriptorBuffer != nullptr && wlp.materialAlbedoTextureIndices != nullptr) {
            const uint32_t albedoTexIdx = wlp.materialAlbedoTextureIndices[matIdx];
            TextureSampler albedoSampler = getTextureSampler(
                wlp.textureDescriptorBuffer, albedoTexIdx, TextureFilter_Linear);
            if (albedoSampler.isValid()) {
                TextureSampleRGBA sample = sampleTexture2D(albedoSampler, tu, tv);
                tp.baseColorR = sample.r;
                tp.baseColorG = sample.g;
                tp.baseColorB = sample.b;
                tp.flags |= PathTexturedFlags::HasBaseColorTex;
            }
        }
        if (!(tp.flags & PathTexturedFlags::HasBaseColorTex)) {
            tp.baseColorR = d[MaterialDataLayout::AlbedoR];
            tp.baseColorG = d[MaterialDataLayout::AlbedoG];
            tp.baseColorB = d[MaterialDataLayout::AlbedoB];
        }

        // 采样 Roughness 纹理
        if (wlp.textureDescriptorBuffer != nullptr && wlp.materialRoughnessTextureIndices != nullptr) {
            const uint32_t roughTexIdx = wlp.materialRoughnessTextureIndices[matIdx];
            TextureSampler roughSampler = getTextureSampler(
                wlp.textureDescriptorBuffer, roughTexIdx, TextureFilter_Linear);
            if (roughSampler.isValid()) {
                TextureSampleRGBA sample = sampleTexture2D(roughSampler, tu, tv);
                tp.roughness = sample.r;  // 粗糙度通常在 R 通道
                tp.flags |= PathTexturedFlags::HasRoughnessTex;
            }
        }
        if (!(tp.flags & PathTexturedFlags::HasRoughnessTex)) {
            tp.roughness = d[MaterialDataLayout::Roughness];
        }
        tp.roughness = ::vlr::vlr_max(0.001f, tp.roughness);

        // 采样 Metallic 纹理
        if (wlp.textureDescriptorBuffer != nullptr && wlp.materialMetallicTextureIndices != nullptr) {
            const uint32_t metalTexIdx = wlp.materialMetallicTextureIndices[matIdx];
            TextureSampler metalSampler = getTextureSampler(
                wlp.textureDescriptorBuffer, metalTexIdx, TextureFilter_Linear);
            if (metalSampler.isValid()) {
                TextureSampleRGBA sample = sampleTexture2D(metalSampler, tu, tv);
                tp.metallic = sample.r;  // 金属度通常在 R 通道
                tp.flags |= PathTexturedFlags::HasMetallicTex;
            }
        }
        if (!(tp.flags & PathTexturedFlags::HasMetallicTex)) {
            tp.metallic = d[MaterialDataLayout::Metallic];
        }
        tp.metallic = ::vlr::vlr_max(0.0f, ::vlr::vlr_min(1.0f, tp.metallic));
    }

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

    // 若 render_common 的 classifyMaterial 可用（依赖 BSDF），可替换为：
    // BSDF<TransportMode::Radiance> bsdf(&matDesc);
    // category = classifyMaterial(bsdf);
    // 当前使用 material_types 的 bsdfTypeToMaterialCategory 保持兼容

    // ========================================================================
    // 8. Denoiser 辅助缓冲区更新
    // ========================================================================
    uint32_t stride = (wlp.imageStrideInPixels > 0) ? wlp.imageStrideInPixels : wlp.imageSize.x;
    uint32_t pixelIdx = pathState.pixelY * stride + pathState.pixelX;

    if (wlp.accumAlbedoBuffer != nullptr) {
        // 反照率：优先使用纹理化参数，否则从材质获取（用于 Denoiser）
        BSDFType type = getBSDFType(matDesc);
        if (type == BSDFType_LambertCheckerboard) {
            SampledSpectrum albedo;
            getLambertAlbedoCheckerboard(matDesc, &surfPt, &albedo);
            wlp.accumAlbedoBuffer[pixelIdx].r = albedo.values[0];
            wlp.accumAlbedoBuffer[pixelIdx].g = albedo.values[1];
            wlp.accumAlbedoBuffer[pixelIdx].b = albedo.values[2];
        } else if (wlp.pathTexturedParamsBuffer != nullptr &&
                   (wlp.pathTexturedParamsBuffer[pathIndex].flags & PathTexturedFlags::HasBaseColorTex)) {
            const PathTexturedMaterialParams& tp = wlp.pathTexturedParamsBuffer[pathIndex];
            wlp.accumAlbedoBuffer[pixelIdx].r = tp.baseColorR;
            wlp.accumAlbedoBuffer[pixelIdx].g = tp.baseColorG;
            wlp.accumAlbedoBuffer[pixelIdx].b = tp.baseColorB;
        } else {
            const float* d = getMaterialDataAsFloats(matDesc);
            wlp.accumAlbedoBuffer[pixelIdx].r = d[MaterialDataLayout::AlbedoR];
            wlp.accumAlbedoBuffer[pixelIdx].g = d[MaterialDataLayout::AlbedoG];
            wlp.accumAlbedoBuffer[pixelIdx].b = d[MaterialDataLayout::AlbedoB];
        }
    }

    if (wlp.accumNormalBuffer != nullptr) {
        // 法线：着色法线（世界空间）
        wlp.accumNormalBuffer[pixelIdx].x = surfPt.shadingFrame.z.x;
        wlp.accumNormalBuffer[pixelIdx].y = surfPt.shadingFrame.z.y;
        wlp.accumNormalBuffer[pixelIdx].z = surfPt.shadingFrame.z.z;
    }
#endif
}
