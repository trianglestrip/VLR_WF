// ============================================================================
// VLR Wavefront - Light Path Kernels (LVC-BPT)
//
// Implements light path tracing for Bidirectional Path Tracing.
// Single-bounce version: pathLength 0 (on light) and 1 (first hit).
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
#include "../include/vlr/basic_types.h"

#include <cuda_runtime.h>
#include <cmath>

namespace {

using namespace vlr;
using namespace vlr::shared;

CUDA_DEVICE_FUNCTION CUDA_INLINE Vector3D sampleCosineHemisphere(float u0, float u1) {
    float r = std::sqrt(u0);
    float phi = u1 * VLR_M_2PI;
    float x = r * std::cos(phi);
    float y = r * std::sin(phi);
    float z = std::sqrt(1.0f - u0);
    return normalize(Vector3D(x, y, z));
}

CUDA_DEVICE_FUNCTION CUDA_INLINE Vector3D sampleUniformSphere(float u0, float u1) {
    float z = 1.0f - 2.0f * u0;
    float r = std::sqrt(1.0f - z * z);
    float phi = u1 * VLR_M_2PI;
    return normalize(Vector3D(r * std::cos(phi), r * std::sin(phi), z));
}

}  // anonymous namespace

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

    uint32_t vertexSlot = atomicAdd(wlp.numLightVertices, 1u);
    if (vertexSlot < wlp.maxLightVertices) {
        Vector3D dirInLocal = surfPt.shadingFrame.toLocal(Vector3D(-state.direction.x, -state.direction.y, -state.direction.z));

        LightPathVertex hitVertex;
        hitVertex.position = surfPt.position;
        hitVertex.geometricNormal = surfPt.geometricNormal;
        hitVertex.shadingFrame = surfPt.shadingFrame;
        hitVertex.flux = state.flux;
        hitVertex.dirInLocal = dirInLocal;
        hitVertex.materialIndex = geomInst.materialIndex;
        hitVertex.flags = materialIsDelta(matDesc) ? 0x1u : 0u;
        hitVertex.pathLength = 1;
        hitVertex._padding = 0.0f;

        wlp.lightVertexCache[vertexSlot] = hitVertex;
    }

    state.setTerminated();
#endif
}
