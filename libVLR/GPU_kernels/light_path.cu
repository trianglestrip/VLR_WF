// ============================================================================
// VLR Wavefront - Light Path Kernels (LVC-BPT)
//
// Implements light path tracing for Bidirectional Path Tracing.
// Multi-bounce: continues through delta surfaces (glass/mirror).
//
// ============================================================================

#include "../shared/kernel_common.h"
#include "kernel_launch.h"
#include "../shared/path_types.h"
#include "../shared/light_common.h"
#include "../shared/light_types.h"
#include "../shared/bsdf_common.h"
#include "../shared/geometry_common.h"
#include "../shared/geometry_types.h"
#include "../shared/material_types.h"
#include "../shared/texture_common.h"
#include "../shared/sampling_common.h"
#include "../include/vlr/basic_types.h"

#include <cuda_runtime.h>
#include <cmath>

using namespace vlr;
using namespace vlr::shared;

// ============================================================================
// generateLightPaths Kernel
// ============================================================================

extern "C" __global__ void generateLightPaths(
    vlr::shared::WavefrontLaunchParameters* params) {
    using namespace vlr::shared;
    WavefrontLaunchParameters& wlp = *params;

#ifdef __CUDACC__

    if (!wlp.useBDPT || wlp.lightVertexCache == nullptr || wlp.lightPathStateBuffer == nullptr)
        return;

    if (wlp.numLightVertices == nullptr || wlp.lightHitInfoBuffer == nullptr)
        return;

    uint32_t pathIndex = blockIdx.x * blockDim.x + threadIdx.x;
    if (pathIndex >= wlp.numLightPaths)
        return;

    KernelRNG rng;
    {
        uint64_t seed = (static_cast<uint64_t>(wlp.numAccumFrames) * wlp.numLightPaths + pathIndex) * 0x853c49e6748fea9bULL;
        rng.state = seed ^ 0xda3e39cb94b95bdbULL;
        rng.inc = 0xda3e39cb94b95bdbULL;
    }

    float selectWLPDF;
    WavelengthSamples wls = WavelengthSamples::createWithEqualOffsets(
        rng.getFloat0cTo1o(), rng.getFloat0cTo1o(), &selectWLPDF);

    float uLight = rng.getFloat0cTo1o();
    LightSelectResult selectResult;
    if (!selectLight(uLight, &selectResult, wlp))
        return;

    LightSampleResult sampleResult;
    float u0 = rng.getFloat0cTo1o();
    float u1 = rng.getFloat0cTo1o();
    float u2 = rng.getFloat0cTo1o();
    if (!sampleLight(selectResult.descriptor, Point3D(0, 0, 0), u0, u1, u2, &sampleResult, wlp))
        return;

    sampleResult.lightSelectProb = selectResult.selectProb;

    const SurfacePoint& lightSurfPt = sampleResult.lightSurfPt;
    const LightDescriptor& desc = selectResult.descriptor;

    if (lightSurfPt.atInfinity)
        return;

    float lightAreaPDF = sampleResult.lightSelectProb * sampleResult.areaPDF;
    if (lightAreaPDF < 1e-10f) return;
    float numLP = static_cast<float>(wlp.numLightPaths);

    SampledSpectrum Le = evaluateEmittance(wlp.materialDescriptorBuffer[
        wlp.geomInstBuffer[desc.geomInstIndex].materialIndex]);
    if (!Le.hasNonZero()) return;

    // flux for pathLength=0 vertex: Le / (numLightPaths * lightAreaPDF)
    // This represents the radiance per unit area on the light surface.
    // The connection formula will handle geometry term and EDF evaluation.
    SampledSpectrum alpha = Le / (numLP * lightAreaPDF);

    Vector3D emitDirWorld;
    if (desc.type == LightType_Point) {
        u0 = rng.getFloat0cTo1o();
        u1 = rng.getFloat0cTo1o();
        emitDirWorld = lightSurfPt.shadingFrame.toWorld(sampleUniformSphere(u0, u1));
    } else if (desc.type == LightType_Area) {
        u0 = rng.getFloat0cTo1o();
        u1 = rng.getFloat0cTo1o();
        Vector3D dirLocal = sampleCosineHemisphere(u0, u1);
        emitDirWorld = lightSurfPt.shadingFrame.toWorld(dirLocal);
        if (dirLocal.z <= 0.0f) return;
    } else {
        return;
    }

    uint32_t vertexSlot = atomicAdd(wlp.numLightVertices, 1u);
    if (vertexSlot >= wlp.maxLightVertices)
        return;

    LightPathVertex lightVertex;
    lightVertex.position = lightSurfPt.position;
    lightVertex.geometricNormal = lightSurfPt.geometricNormal;
    lightVertex.shadingFrame = lightSurfPt.shadingFrame;
    lightVertex.flux = alpha;
    lightVertex.dirInLocal = lightSurfPt.shadingFrame.toLocal(emitDirWorld);
    lightVertex.materialIndex = wlp.geomInstBuffer[desc.geomInstIndex].materialIndex;
    lightVertex.flags = (desc.type == LightType_Point) ? 0x8u : 0u;
    lightVertex.pathLength = 0;
    lightVertex._padding = 0.0f;

    wlp.lightVertexCache[vertexSlot] = lightVertex;

    LightPathState& state = wlp.lightPathStateBuffer[pathIndex];
    state.origin = lightSurfPt.position;
    state.direction = emitDirWorld;
    state.flux = alpha;
    state.wls = wls;
    state.rng = rng;
    state.pathLength = 1;
    state.flags = 1;
    state.setActive(true);

    wlp.lightHitInfoBuffer[pathIndex].reset();
#endif
}

// ============================================================================
// processLightHits Kernel
// ============================================================================

extern "C" __global__ void processLightHits(
    vlr::shared::WavefrontLaunchParameters* params) {
    using namespace vlr::shared;
    WavefrontLaunchParameters& wlp = *params;

#ifdef __CUDACC__

    if (!wlp.useBDPT || wlp.lightVertexCache == nullptr || wlp.lightPathStateBuffer == nullptr)
        return;

    uint32_t pathIndex = blockIdx.x * blockDim.x + threadIdx.x;
    if (pathIndex >= wlp.numLightPaths)
        return;

    LightPathState& state = wlp.lightPathStateBuffer[pathIndex];
    if (!state.isActive())
        return;

    const WavefrontHitInfo& hitInfo = wlp.lightHitInfoBuffer[pathIndex];
    if (!hitInfo.hasHit() || hitInfo.hitInfinity()) {
        state.setTerminated();
        return;
    }

    SurfacePoint surfPt;
    float hypAreaPDF = 1.0f;

    if (wlp.vertexPositions != nullptr) {
        HitPointDecodeInput input(hitInfo.instIndex, hitInfo.geomInstIndex,
            hitInfo.primIndex, hitInfo.u, hitInfo.v);

        TriangleMeshVertexData vertexData(wlp.vertexPositions,
            wlp.vertexNormals, wlp.vertexTexCoords);

        GeometryDecodeContext ctx(
            &wlp.geomInstBuffer[hitInfo.geomInstIndex],
            &wlp.instBuffer[hitInfo.instIndex],
            vertexData);

        computeSurfacePointBasic(input, ctx, &surfPt, &hypAreaPDF);

        Vector3D rayDir = state.direction;
        float ndotd = dot(surfPt.geometricNormal, rayDir);
        surfPt.isFrontFace = (ndotd <= 0.0f);
        if (ndotd > 0.0f) {
            surfPt.geometricNormal = -surfPt.geometricNormal;
            surfPt.shadingFrame = ReferenceFrame(surfPt.shadingFrame.x, -surfPt.shadingFrame.z);
        }
    } else {
        const GeometryInstance& geomInst = wlp.geomInstBuffer[hitInfo.geomInstIndex];
        if (geomInst.geomType == GeometryType_TriangleMesh &&
            geomInst.asTriMesh.triangleBuffer != nullptr) {
            const Triangle& tri = geomInst.asTriMesh.triangleBuffer[hitInfo.primIndex];
            hypAreaPDF = (tri.area > 0.0f) ? (1.0f / tri.area) : 1.0f;
        }
        surfPt.position = Point3D(
            state.origin.x + state.direction.x * hitInfo.t,
            state.origin.y + state.direction.y * hitInfo.t,
            state.origin.z + state.direction.z * hitInfo.t);
        surfPt.geometricNormal = Normal3D(0, 1, 0);
        surfPt.shadingFrame = ReferenceFrame(Vector3D(1, 0, 0), surfPt.geometricNormal);
        surfPt.texCoord = TexCoord2D(hitInfo.u, hitInfo.v);
        surfPt.atInfinity = false;
        surfPt.isFrontFace = (dot(surfPt.geometricNormal, state.direction) <= 0.0f);
    }

    if (wlp.lightSurfacePointBuffer != nullptr)
        wlp.lightSurfacePointBuffer[pathIndex] = surfPt;

    const GeometryInstance& geomInst = wlp.geomInstBuffer[hitInfo.geomInstIndex];
    const SurfaceMaterialDescriptor& matDesc = wlp.materialDescriptorBuffer[geomInst.materialIndex];
    bool isDelta = materialIsDelta(matDesc);

    Vector3D dirInLocal = surfPt.shadingFrame.toLocal(
        Vector3D(-state.direction.x, -state.direction.y, -state.direction.z));

    // Store vertex in cache (for non-delta surfaces only; delta vertices
    // cannot be connected to since their BSDF is a Dirac delta)
    if (!isDelta) {
        uint32_t vertexSlot = atomicAdd(wlp.numLightVertices, 1u);
        if (vertexSlot < wlp.maxLightVertices) {
            LightPathVertex hitVertex;
            hitVertex.position = surfPt.position;
            hitVertex.geometricNormal = surfPt.geometricNormal;
            hitVertex.shadingFrame = surfPt.shadingFrame;
            hitVertex.flux = state.flux;
            hitVertex.dirInLocal = dirInLocal;
            hitVertex.materialIndex = geomInst.materialIndex;
            hitVertex.flags = 0u;
            hitVertex.pathLength = state.pathLength;
            hitVertex._padding = 0.0f;
            wlp.lightVertexCache[vertexSlot] = hitVertex;
        }
    }

    // For delta surfaces (glass/mirror), sample BSDF and continue tracing
    if (isDelta && state.pathLength < 8) {
        PathTexturedMaterialParams dummyTexParams;
        dummyTexParams.flags = 0;
        BSDFContext bsdfCtx(matDesc, surfPt, state.wls);
        bsdfCtx.texturedParams = &dummyTexParams;

        float u0 = state.rng.getFloat0cTo1o();
        float u1 = state.rng.getFloat0cTo1o();
        float u2 = state.rng.getFloat0cTo1o();
        BSDFSampleResult bsdfResult;
        bsdfResult.pdf = 0.0f;
        sampleBSDFWithU2(bsdfCtx, dirInLocal, u0, u1, u2, &bsdfResult);

        if (bsdfResult.pdf > 1e-10f && bsdfResult.f.hasNonZero()) {
            Vector3D newDirWorld = surfPt.shadingFrame.toWorld(bsdfResult.dirLocal);
            float cosAbs = std::abs(dot(bsdfResult.dirLocal,
                                        surfPt.shadingFrame.toLocal(surfPt.geometricNormal)));

            SampledSpectrum weight;
            if (bsdfResult.isDelta)
                weight = bsdfResult.f * (cosAbs / bsdfResult.pdf);
            else
                weight = bsdfResult.f * (cosAbs / bsdfResult.pdf);

            state.flux = state.flux * weight;

            if (!state.flux.allFinite() || !state.flux.hasNonZero()) {
                state.setTerminated();
            } else {
                float cosFactor = dot(Vector3D(surfPt.geometricNormal), newDirWorld);
                state.origin = offsetRayOriginForNextBounce(surfPt, cosFactor);
                state.direction = newDirWorld;
                state.pathLength++;
                wlp.lightHitInfoBuffer[pathIndex].reset();
            }
        } else {
            state.setTerminated();
        }
    } else {
        state.setTerminated();
    }
#endif
}

// ============================================================================
// applyShadowRayResults Kernel
// After traceShadowRays, apply visible vertex connection contributions.
// ============================================================================

extern "C" __global__ void applyShadowRayResults(
    vlr::shared::WavefrontLaunchParameters* params) {
    using namespace vlr::shared;
    WavefrontLaunchParameters& wlp = *params;

#ifdef __CUDACC__
    if (wlp.shadowRayQueue == nullptr || wlp.shadowRayResults == nullptr ||
        wlp.numShadowRayRequests == nullptr)
        return;

    uint32_t workIndex = blockIdx.x * blockDim.x + threadIdx.x;
    uint32_t numRequests = *wlp.numShadowRayRequests;
    if (workIndex >= numRequests)
        return;

    float visibility = wlp.shadowRayResults[workIndex];
    if (visibility < 0.5f)
        return;

    const ShadowRayRequest& req = wlp.shadowRayQueue[workIndex];
    uint32_t pathIndex = req.pathIndex;
    if (pathIndex >= wlp.maxNumPaths)
        return;

    WavefrontPathState& pathState = wlp.pathStateBuffer[pathIndex];
    pathState.contribution += req.contribution;
#endif
}
