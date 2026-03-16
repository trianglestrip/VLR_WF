// ============================================================================
// VLR Wavefront - ProcessHits Kernel
//
// ??????Wavefront ?????????????
// ?????????BSDF/EDF ????????????????????Denoiser ???????
//
// ???VLR ?????
// ??????026-03-07
// ???CUDA 13.1, OptiX 8.0.0, VS2022
// ============================================================================

#define VLR_DEBUG_PROCESS_HITS 0

// ??????
// #define VLR_ENABLE_GPU_DEBUG 1

#ifdef VLR_ENABLE_GPU_DEBUG
    #define VLR_DEBUG_PRINTF(...) printf(__VA_ARGS__)
#else
    #define VLR_DEBUG_PRINTF(...) ((void)0)
#endif

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

/// ??????????MIS ??
/// ????? BSDF ????????????????????????MIS
CUDA_DEVICE_FUNCTION CUDA_INLINE float computeImplicitLightMISWeight(
    const WavefrontPathState& pathState,
    float hypAreaPDF,
    float cosOutLocal) {

    // ????? delta????????????????????MIS = 1
    if (pathState.prevSampledType.isDelta())
        return 1.0f;

    // ???????????? MIS
    if (pathState.pathLength <= 1)
        return 1.0f;

    // Power Heuristic: w_bsdf = pdf_bsdf^2 / (pdf_bsdf^2 + pdf_light^2)
    // ???????????? BSDF ?????? prevDirPDF ?? pdf_bsdf
    // pdf_light ???????????????????????1
    float pdfBSDF = pathState.prevDirPDF;
    if (pdfBSDF <= 0.0f) return 1.0f;

    // ????? PDF?????????????PDF ??
    // ???????lightInstDist ?? instProb ??geomInstProb
    float cosTerm = std::abs(cosOutLocal);
    if (cosTerm < 1e-6f) return 1.0f;
    float pdfLight = hypAreaPDF / cosTerm;
    if (pdfLight <= 0.0f || pdfLight >= 1e30f) return 1.0f;

    return powerHeuristicMIS(pdfBSDF, pdfLight);
}


/// ???????????????? Miss??
/// ????????????
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

    // ?????????????????
    SurfacePoint surfPt;
    surfPt.position = Point3D(pathState.direction.x, pathState.direction.y, pathState.direction.z);
    surfPt.atInfinity = true;
    surfPt.isFrontFace = true;
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

    // ??????EDF
    const SurfaceMaterialDescriptor& matDesc = wlp.materialDescriptorBuffer[geomInst.materialIndex];
    SampledSpectrum spEmittance = evaluateEmittance(matDesc);

    if (!spEmittance.hasNonZero())
        return;

    // ????????????????????????
    Vector3D dirOutLocal = surfPt.shadingFrame.toLocal(Vector3D(-pathState.direction.x, -pathState.direction.y, -pathState.direction.z));
    EDFContext edfCtx(matDesc, surfPt, pathState.wls);
    EDFEvaluateResult edfResult = evaluateEDF(edfCtx, dirOutLocal);

    if (!edfResult.hasEmission)
        return;

    SampledSpectrum Le = edfResult.Le;

    // MIS ???BSDF ?? vs ?????????
    float MISWeight = 1.0f;
    if (!pathState.prevSampledType.isDelta() && pathState.pathLength > 1) {
        float bsdfPDF = pathState.prevDirPDF;
        float cosOutSafe = (std::abs(dirOutLocal.z) > 1e-6f) ? std::abs(dirOutLocal.z) : 1e-6f;
        float lightPDF;
        if (wlp.envImportanceMap.isValid()) {
            // ???????????importance map ????????
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
            VLR_DEBUG_PRINTF("[NaN] process_hits(env): px=(%u,%u) pathLen=%u envContrib=(%.4f,%.4f,%.4f) op=throughput*Le*MIS\n",
                   pathState.pixelX, pathState.pixelY, pathState.pathLength,
                   envContrib.values[0], envContrib.values[1], envContrib.values[2]);
        }
    }
#endif
}


/// ????????????????
/// ??????????????????
CUDA_DEVICE_FUNCTION CUDA_INLINE void processEmissiveSurface(
    WavefrontPathState& pathState,
    const SurfacePoint& surfPt,
    const GeometryInstance& geomInst,
    float hypAreaPDF,
    WavefrontLaunchParameters& wlp) {

#ifdef VLR_DEBUG_PROCESS_HITS
    if (pathState.pixelX == 256 && pathState.pixelY == 256 && pathState.pathLength == 0) {
        VLR_DEBUG_PRINTF("[GPU ProcessEmissive ENTRY] px=(256,256): geomInstIndex=%u, materialIndex=%u\n",
            geomInst.instIndex, geomInst.materialIndex);
    }
#endif

    const SurfaceMaterialDescriptor& matDesc = wlp.materialDescriptorBuffer[geomInst.materialIndex];

#ifdef VLR_DEBUG_PROCESS_HITS
    if (pathState.pixelX == 256 && pathState.pixelY == 256 && pathState.pathLength == 0) {
        VLR_DEBUG_PRINTF("[GPU ProcessEmissive] px=(256,256): materialIndex=%u, hasEmission=%d\n",
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
        VLR_DEBUG_PRINTF("[GPU ProcessEmissive] px=(256,256): Le=(%.3f,%.3f,%.3f,%.3f), throughput=(%.3f,%.3f,%.3f,%.3f), MIS=%.3f, contrib=(%.3f,%.3f,%.3f,%.3f)\n",
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
            VLR_DEBUG_PRINTF("[NaN] process_hits(emissive): px=(%u,%u) pathLen=%u emissiveContrib=(%.4f,%.4f,%.4f) op=throughput*Le*MIS\n",
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

    // ??????????????????
    uint32_t workIndex = blockIdx.x * blockDim.x + threadIdx.x;
    uint32_t activeCount = wlp.activePathQueue.size();

    if (workIndex >= activeCount)
        return;

    uint32_t pathIndex = wlp.activePathQueue.pathIndices[workIndex];
    WavefrontPathState& pathState = wlp.pathStateBuffer[pathIndex];
    WavefrontHitInfo& hitInfo = wlp.hitInfoBuffer[pathIndex];

#ifdef VLR_DEBUG_PROCESS_HITS
    if (pathIndex == 0) {
        VLR_DEBUG_PRINTF("[GPU ProcessHits] pathIndex=0: isActive=%d, hasHit=%d, hitInfinity=%d, geomInstIndex=%u\n",
            pathState.isActive() ? 1 : 0, hitInfo.hasHit() ? 1 : 0, hitInfo.hitInfinity() ? 1 : 0, hitInfo.geomInstIndex);
    }
#endif

    if (!pathState.isActive())
        return;

    // ????????????????TraceRays ??????????
    if (!hitInfo.hasHit()) {
        pathState.setTerminated();
        return;
    }

    // ========================================================================
    // 1. ??????????????/ Miss
    // ========================================================================
    if (hitInfo.hitInfinity()) {
        processEnvironmentHit(pathState, hitInfo, wlp);
        pathState.setTerminated();
        return;
    }

    // ========================================================================
    // 2. ??????decodeHitPoint + computeSurfacePoint??
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

        // Always faceforward the geometric normal for a consistent local frame,
        // but preserve whether this hit was on the front face (outside -> inside).
        {
            Vector3D rayDir = pathState.direction;
            float ndotd = dot(surfPt.geometricNormal, rayDir);
            surfPt.isFrontFace = (ndotd <= 0.0f);
            if (ndotd > 0.0f) {
                surfPt.geometricNormal = -surfPt.geometricNormal;
                // Do NOT flip the shading frame. The BSDF code uses isFrontFace
                // to determine IOR direction. Keeping the original frame avoids
                // handedness issues that corrupt the refracted world direction.
            }
        }

        // ????????????????????????????????
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
        // ????????????????
        const GeometryInstance& geomInst = wlp.geomInstBuffer[hitInfo.geomInstIndex];

        if (geomInst.geomType == GeometryType_TriangleMesh &&
            geomInst.asTriMesh.triangleBuffer != nullptr) {

            const Triangle& tri = geomInst.asTriMesh.triangleBuffer[hitInfo.primIndex];
            hypAreaPDF = (tri.area > 0.0f) ? (1.0f / tri.area) : 1.0f;

            // ?????????????????????origin + direction * t
            // ????VLR ??????????????
            surfPt.position = Point3D(
                pathState.origin.x + pathState.direction.x * hitInfo.t,
                pathState.origin.y + pathState.direction.y * hitInfo.t,
                pathState.origin.z + pathState.direction.z * hitInfo.t);
            surfPt.geometricNormal = Normal3D(0, 1, 0);
            surfPt.shadingFrame = ReferenceFrame(Vector3D(1, 0, 0), surfPt.geometricNormal);
            surfPt.texCoord = TexCoord2D(hitInfo.u, hitInfo.v);
            surfPt.atInfinity = false;
            surfPt.isFrontFace = (dot(surfPt.geometricNormal, pathState.direction) <= 0.0f);
        } else {
            hypAreaPDF = 1.0f;
            // ??????????????????
            surfPt.position = Point3D(
                pathState.origin.x + pathState.direction.x * hitInfo.t,
                pathState.origin.y + pathState.direction.y * hitInfo.t,
                pathState.origin.z + pathState.direction.z * hitInfo.t);
            surfPt.geometricNormal = Normal3D(0, 1, 0);
            surfPt.shadingFrame = ReferenceFrame(Vector3D(1, 0, 0), surfPt.geometricNormal);
            surfPt.texCoord = TexCoord2D(hitInfo.u, hitInfo.v);
            surfPt.atInfinity = false;
            surfPt.isFrontFace = (dot(surfPt.geometricNormal, pathState.direction) <= 0.0f);
        }
    }

    // ???????? SampleLights ??SampleBSDF ??
    wlp.surfacePointBuffer[pathIndex] = surfPt;

    // ========================================================================
    // 3. ????????
    // ========================================================================
    const GeometryInstance& geomInst = wlp.geomInstBuffer[hitInfo.geomInstIndex];
    const SurfaceMaterialDescriptor& matDesc = wlp.materialDescriptorBuffer[geomInst.materialIndex];
    const uint32_t matIdx = geomInst.materialIndex;

    // ========================================================================
    // 3.1 ?????BaseColor?Roughness?Metallic
    // ========================================================================
    if (wlp.pathTexturedParamsBuffer != nullptr) {
        PathTexturedMaterialParams& tp = wlp.pathTexturedParamsBuffer[pathIndex];
        tp.flags = 0;

        // ???????????? scale/offset??????????
        float tu, tv;
        if (wlp.materialTextureParamsBuffer != nullptr) {
            const MaterialTextureParams& mtp = wlp.materialTextureParamsBuffer[matIdx];
            transformTexCoord(surfPt.texCoord.x, surfPt.texCoord.y,
                mtp.scaleU, mtp.scaleV, mtp.offsetU, mtp.offsetV, &tu, &tv);
        } else {
            transformTexCoordIdentity(surfPt.texCoord.x, surfPt.texCoord.y, &tu, &tv);
        }

        const float* d = getMaterialDataAsFloats(matDesc);

        // ?? BaseColor ??
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

        // ?? Roughness ??
        if (wlp.textureDescriptorBuffer != nullptr && wlp.materialRoughnessTextureIndices != nullptr) {
            const uint32_t roughTexIdx = wlp.materialRoughnessTextureIndices[matIdx];
            TextureSampler roughSampler = getTextureSampler(
                wlp.textureDescriptorBuffer, roughTexIdx, TextureFilter_Linear);
            if (roughSampler.isValid()) {
                TextureSampleRGBA sample = sampleTexture2D(roughSampler, tu, tv);
                tp.roughness = sample.r;  // ???????R ??
                tp.flags |= PathTexturedFlags::HasRoughnessTex;
            }
        }
        if (!(tp.flags & PathTexturedFlags::HasRoughnessTex)) {
            tp.roughness = d[MaterialDataLayout::Roughness];
        }
        tp.roughness = ::vlr::vlr_max(0.001f, tp.roughness);

        // ?? Metallic ??
        if (wlp.textureDescriptorBuffer != nullptr && wlp.materialMetallicTextureIndices != nullptr) {
            const uint32_t metalTexIdx = wlp.materialMetallicTextureIndices[matIdx];
            TextureSampler metalSampler = getTextureSampler(
                wlp.textureDescriptorBuffer, metalTexIdx, TextureFilter_Linear);
            if (metalSampler.isValid()) {
                TextureSampleRGBA sample = sampleTexture2D(metalSampler, tu, tv);
                tp.metallic = sample.r;  // ???????R ??
                tp.flags |= PathTexturedFlags::HasMetallicTex;
            }
        }
        if (!(tp.flags & PathTexturedFlags::HasMetallicTex)) {
            tp.metallic = d[MaterialDataLayout::Metallic];
        }
        tp.metallic = ::vlr::vlr_max(0.0f, ::vlr::vlr_min(1.0f, tp.metallic));
    }

    // BSDFContext ?? SampleBSDF kernel ?? surfacePointBuffer ??
    // ?? surfacePointBuffer ????SampleLights/SampleBSDF ??????

    // ========================================================================
    // 4. EDF ??????????????????
    // ========================================================================
    processEmissiveSurface(pathState, surfPt, geomInst, hypAreaPDF, wlp);

    // ========================================================================
    // 4.5 LVC-BPT: Vertex Connection with Light Vertex Cache
    //     Enqueue shadow ray for visibility test instead of direct accumulation
    // ========================================================================
    if (wlp.useBDPT && wlp.lightVertexCache != nullptr && wlp.numLightVertices != nullptr) {
        uint32_t numLV = *wlp.numLightVertices;
        if (numLV > 0 && !materialIsDelta(matDesc)) {
            uint32_t lvIndex = vlr_min(
                static_cast<uint32_t>(pathState.rng.getFloat0cTo1o() * numLV),
                numLV - 1);
            const LightPathVertex& lv = wlp.lightVertexCache[lvIndex];

            Vector3D conDir = lv.position - surfPt.position;
            float dist2 = dot(conDir, conDir);
            if (dist2 > 1e-8f) {
                float dist = sqrtf(dist2);
                conDir = conDir / dist;

                float cosE = dot(conDir, surfPt.geometricNormal);
                float cosL = -dot(conDir, lv.geometricNormal);

                if (cosE > 1e-5f && cosL > 1e-5f) {
                    float G = cosE * cosL / dist2;

                    Vector3D dirInLocal = surfPt.shadingFrame.toLocal(-pathState.direction);
                    Vector3D conDirLocal = surfPt.shadingFrame.toLocal(conDir);
                    BSDFContext bsdfCtx(matDesc, surfPt, pathState.wls);
                    SampledSpectrum fsE = evaluateBSDF(bsdfCtx, dirInLocal, conDirLocal);

                    if (fsE.hasNonZero()) {
                        float vertexProb = 1.0f / static_cast<float>(numLV);
                        float edfFactor = (lv.pathLength == 0) ? VLR_M_INV_PI : 1.0f;
                        SampledSpectrum contrib = pathState.throughput * fsE * G * lv.flux * edfFactor / vertexProb;

                        if (contrib.allFinite() && contrib.hasNonZero() &&
                            wlp.shadowRayQueue != nullptr && wlp.numShadowRayRequests != nullptr) {
                            uint32_t slot = atomicAdd(wlp.numShadowRayRequests, 1u);
                            if (slot < wlp.maxShadowRayRequests) {
                                ShadowRayRequest& req = wlp.shadowRayQueue[slot];
                                req.origin = surfPt.position + conDir * 1e-4f;
                                req.direction = conDir;
                                req.tMax = dist;
                                req.pathIndex = pathIndex;
                                req.contribution = contrib;
                            }
                        }
                    }
                }
            }
        }
    }

    // ========================================================================
    // 5. ?????????
    // ========================================================================
    pathState.pathLength++;

    if (pathState.pathLength >= wlp.maxPathLength) {
        pathState.setMaxLengthReached();
        pathState.setTerminated();
        return;
    }

    // ========================================================================
    // 6. ??????
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
    // 7. ????????classifyMaterial ??????
    // ========================================================================
    MaterialCategory category = bsdfTypeToMaterialCategory(getBSDFType(matDesc));
    pathState.materialCategory = category;

    // ??render_common ??classifyMaterial ??????BSDF????????
    // BSDF<TransportMode::Radiance> bsdf(&matDesc);
    // category = classifyMaterial(bsdf);
    // ???? material_types ??bsdfTypeToMaterialCategory ????

    // ========================================================================
    // 8. Denoiser ????????
    // ========================================================================
    uint32_t stride = (wlp.imageStrideInPixels > 0) ? wlp.imageStrideInPixels : wlp.imageSize.x;
    uint32_t pixelIdx = pathState.pixelY * stride + pathState.pixelX;

    if (wlp.accumAlbedoBuffer != nullptr) {
        // ???????????????????????? Denoiser??
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
        // ??????????????
        wlp.accumNormalBuffer[pixelIdx].x = surfPt.shadingFrame.z.x;
        wlp.accumNormalBuffer[pixelIdx].y = surfPt.shadingFrame.z.y;
        wlp.accumNormalBuffer[pixelIdx].z = surfPt.shadingFrame.z.z;
    }
#endif
}
