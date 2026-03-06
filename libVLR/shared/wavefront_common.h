// ============================================================================
// VLR Wavefront Path Tracing - Common Functions
// 
// This file defines common utility functions used across Wavefront kernels.
// These functions are device-side only and should be inlined.
// 
// Author: VLR Development Team
// Created: 2026-03-07
// Environment: CUDA 13.1, OptiX 8.0.0, VS2022
// ============================================================================

#pragma once

#include "wavefront_types.h"
#include "kernel_common.h"

namespace vlr {
namespace shared {

#if defined(VLR_Device) || defined(OPTIXU_Platform_CodeCompletion)

// ============================================================================
// Material Classification
// ============================================================================

/// Classify material based on BSDF characteristics
/// This is used to sort paths by material type for better coherence
CUDA_DEVICE_FUNCTION CUDA_INLINE MaterialCategory classifyMaterial(
    const BSDF<TransportMode::Radiance>& bsdf) {
    
    // Check for perfect specular reflection
    if (bsdf.matches(DirectionType::Delta0D() | DirectionType::Reflection()))
        return MaterialCategory_Specular;
    
    // Check for transmissive materials
    if (bsdf.matches(DirectionType::Transmission()))
        return MaterialCategory_Transmissive;
    
    // Check for glossy reflection (high frequency)
    if (bsdf.matches(DirectionType::HighFreq() | DirectionType::Reflection()))
        return MaterialCategory_Glossy;
    
    // Check for mixed materials
    if (bsdf.matches(DirectionType::Reflection()) && 
        bsdf.matches(DirectionType::Transmission()))
        return MaterialCategory_Mixed;
    
    // Default to diffuse
    return MaterialCategory_Diffuse;
}


// ============================================================================
// Multiple Importance Sampling (MIS)
// ============================================================================

/// Compute MIS weight using Power Heuristic (beta=2)
/// This is the standard MIS weight computation used in path tracing
CUDA_DEVICE_FUNCTION CUDA_INLINE float computeMISWeight(
    float pdf1, float pdf2) {
    
    if (isinf(pdf1) || isinf(pdf2))
        return 1.0f;
    
    float pdf1Sq = pdf1 * pdf1;
    float pdf2Sq = pdf2 * pdf2;
    return pdf1Sq / (pdf1Sq + pdf2Sq);
}


/// Compute MIS weight with sample counts (generalized version)
CUDA_DEVICE_FUNCTION CUDA_INLINE float computeMISWeight(
    float pdf1, uint32_t n1, float pdf2, uint32_t n2, float beta = 2.0f) {
    
    if (isinf(pdf1) || isinf(pdf2))
        return 1.0f;
    
    float w1 = n1 * pdf1;
    float w2 = n2 * pdf2;
    
    if (beta == 2.0f) {
        return (w1 * w1) / (w1 * w1 + w2 * w2);
    } else {
        float w1_beta = pow(w1, beta);
        float w2_beta = pow(w2, beta);
        return w1_beta / (w1_beta + w2_beta);
    }
}


// ============================================================================
// Geometry Calculations
// ============================================================================

/// Compute geometry term G(x <-> y) = cos(theta_x) * cos(theta_y) / distance^2
/// This is used in light transport equations
CUDA_DEVICE_FUNCTION CUDA_INLINE float computeGeometryTerm(
    const SurfacePoint& sp1,
    const SurfacePoint& sp2,
    const Vector3D& direction,
    float squaredDistance) {
    
    if (sp1.atInfinity || sp2.atInfinity)
        return 1.0f;
    
    float cos1 = absDot(direction, sp1.geometricNormal);
    float cos2 = absDot(-direction, sp2.geometricNormal);
    
    return (cos1 * cos2) / squaredDistance;
}


/// Compute solid angle PDF from area PDF
CUDA_DEVICE_FUNCTION CUDA_INLINE float convertAreaPDFToSolidAnglePDF(
    float areaPDF,
    float distance,
    float cosThetaLight) {
    
    if (cosThetaLight == 0.0f)
        return 0.0f;
    
    float distanceSq = distance * distance;
    return areaPDF * distanceSq / abs(cosThetaLight);
}


// ============================================================================
// Path Termination (Russian Roulette)
// ============================================================================

/// Check if path should terminate using Russian Roulette
/// Returns true if path should be terminated
CUDA_DEVICE_FUNCTION CUDA_INLINE bool shouldTerminatePath(
    WavefrontPathState& pathState,
    float rrThreshold = 0.05f) {
    
    // Don't use RR for first few bounces
    if (pathState.pathLength < WavefrontConfig::RRStartDepth)
        return false;
    
    // Compute continuation probability
    float importance = pathState.throughput.importance(pathState.wls.selectedLambdaIndex());
    float continueProb = min(importance / pathState.initImportance, 1.0f);
    
    // Force termination if importance is too low
    if (continueProb < rrThreshold)
        return true;
    
    // Russian roulette
    if (pathState.rng.getFloat0cTo1o() >= continueProb)
        return true;
    
    // Path continues, adjust throughput
    pathState.throughput /= continueProb;
    return false;
}


/// Check if path should terminate (simple version without RR adjustment)
CUDA_DEVICE_FUNCTION CUDA_INLINE bool shouldTerminatePathSimple(
    const WavefrontPathState& pathState,
    float rrThreshold = 0.05f) {
    
    if (pathState.pathLength < WavefrontConfig::RRStartDepth)
        return false;
    
    float importance = pathState.throughput.importance(pathState.wls.selectedLambdaIndex());
    float continueProb = min(importance / pathState.initImportance, 1.0f);
    
    if (continueProb < rrThreshold)
        return true;
    
    return pathState.rng.getFloat0cTo1o() >= continueProb;
}


// ============================================================================
// Surface Point Computation
// ============================================================================

/// Compute surface point information from HitInfo
/// This includes geometry decoding, normal mapping, and tangent modification
CUDA_DEVICE_FUNCTION CUDA_INLINE void computeSurfacePoint(
    const WavefrontHitInfo& hitInfo,
    const WavelengthSamples& wls,
    SurfacePoint* surfPt,
    float* hypAreaPDF) {
    
    const GeometryInstance& geomInst = wlp.geomInstBuffer[hitInfo.geomInstIndex];
    
    // Call geometry decode program
    ProgSigDecodeHitPoint decodeHitPoint(geomInst.progDecodeHitPoint);
    decodeHitPoint(
        hitInfo.instIndex,
        hitInfo.geomInstIndex,
        hitInfo.primIndex,
        hitInfo.u, hitInfo.v,
        surfPt);
    
    // Apply normal mapping
    Normal3D localNormal = calcNode(geomInst.nodeNormal, Normal3D(0.0f, 0.0f, 1.0f), *surfPt, wls);
    applyBumpMapping(localNormal, surfPt);
    
    // Apply tangent modification
    Vector3D newTangent = calcNode(geomInst.nodeTangent, surfPt->shadingFrame.x, *surfPt, wls);
    modifyTangent(newTangent, surfPt);
    
    // Compute area PDF (for light sampling)
    if (geomInst.geomType == GeometryType_TriangleMesh) {
        const Triangle& tri = geomInst.asTriMesh.triangleBuffer[hitInfo.primIndex];
        *hypAreaPDF = 1.0f / tri.area;
    } else {
        *hypAreaPDF = 1.0f;
    }
}


// ============================================================================
// Environment Light Processing
// ============================================================================

/// Process environment light hit
/// Evaluates environment map and accumulates contribution with MIS
CUDA_DEVICE_FUNCTION CUDA_INLINE void processEnvironmentHit(
    WavefrontPathState& pathState,
    const WavefrontHitInfo& hitInfo) {
    
    const Instance& inst = wlp.instBuffer[wlp.envLightInstIndex];
    const GeometryInstance& geomInst = wlp.geomInstBuffer[inst.geomInstIndices[0]];
    
    if (geomInst.importance == 0)
        return;
    
    // Compute environment direction
    Vector3D direction = pathState.direction;
    float phi, theta;
    direction.toPolarYUp(&theta, &phi);
    
    // Construct surface point
    SurfacePoint surfPt;
    surfPt.position = Point3D(direction.x, direction.y, direction.z);
    surfPt.atInfinity = true;
    surfPt.geometricNormal = -direction;
    
    float sinPhi, cosPhi;
    sincos(phi, &sinPhi, &cosPhi);
    Vector3D texCoord0Dir = normalize(Vector3D(-cosPhi, 0.0f, -sinPhi));
    surfPt.shadingFrame = ReferenceFrame(texCoord0Dir, -direction);
    
    phi += inst.rotationPhi;
    phi = phi - floor(phi / (2 * VLR_M_PI)) * 2 * VLR_M_PI;
    surfPt.texCoord = TexCoord2D(phi / (2 * VLR_M_PI), theta / VLR_M_PI);
    
    // Evaluate environment light
    const SurfaceMaterialDescriptor& matDesc = wlp.materialDescriptorBuffer[geomInst.materialIndex];
    EDF edf(matDesc, surfPt, pathState.wls);
    
    Vector3D dirOutLocal = surfPt.shadingFrame.toLocal(-direction);
    SampledSpectrum spEmittance = edf.evaluateEmittance();
    
    if (spEmittance.hasNonZero()) {
        EDFQuery feQuery(DirectionType::All(), pathState.wls);
        SampledSpectrum Le = spEmittance * edf.evaluate(feQuery, dirOutLocal);
        
        // MIS weight calculation
        float MISWeight = 1.0f;
        if (!pathState.prevSampledType.isDelta() && pathState.pathLength > 1) {
            float uvPDF = geomInst.asInfSphere.importanceMap.evaluatePDF(
                phi / (2 * VLR_M_PI), theta / VLR_M_PI);
            float hypAreaPDF = uvPDF / (2 * VLR_M_PI * VLR_M_PI * sin(theta));
            
            float instProb = inst.lightGeomInstDistribution.integral() / wlp.lightInstDist.integral();
            float geomInstProb = geomInst.importance / inst.lightGeomInstDistribution.integral();
            
            float bsdfPDF = pathState.prevDirPDF;
            float lightPDF = instProb * geomInstProb * hypAreaPDF / abs(dirOutLocal.z);
            
            MISWeight = computeMISWeight(bsdfPDF, lightPDF);
        }
        
        // Accumulate contribution
        pathState.contribution += pathState.throughput * Le * MISWeight;
    }
}


// ============================================================================
// Emissive Surface Processing
// ============================================================================

/// Process emissive surface hit (implicit light sampling)
/// This handles direct hits on area lights
CUDA_DEVICE_FUNCTION CUDA_INLINE void processEmissiveSurface(
    WavefrontPathState& pathState,
    const SurfacePoint& surfPt,
    const GeometryInstance& geomInst,
    float hypAreaPDF) {
    
    // Evaluate EDF
    const SurfaceMaterialDescriptor& matDesc = wlp.materialDescriptorBuffer[geomInst.materialIndex];
    EDF edf(matDesc, surfPt, pathState.wls);
    
    Vector3D dirOutLocal = surfPt.shadingFrame.toLocal(-pathState.direction);
    SampledSpectrum spEmittance = edf.evaluateEmittance();
    
    if (!spEmittance.hasNonZero())
        return;
    
    EDFQuery feQuery(DirectionType::All(), pathState.wls);
    SampledSpectrum Le = spEmittance * edf.evaluate(feQuery, dirOutLocal);
    
    // MIS weight calculation
    float MISWeight = 1.0f;
    if (!pathState.prevSampledType.isDelta() && pathState.pathLength > 1) {
        // Compute light sampling PDF
        const Instance& inst = wlp.instBuffer[geomInst.instIndex];
        float instProb = inst.lightGeomInstDistribution.integral() / wlp.lightInstDist.integral();
        float geomInstProb = geomInst.importance / inst.lightGeomInstDistribution.integral();
        
        float cosTerm = abs(dirOutLocal.z);
        float lightPDF = instProb * geomInstProb * hypAreaPDF / cosTerm;
        
        float bsdfPDF = pathState.prevDirPDF;
        MISWeight = computeMISWeight(bsdfPDF, lightPDF);
    }
    
    // Accumulate contribution
    pathState.contribution += pathState.throughput * Le * MISWeight;
}


// ============================================================================
// Path State Initialization
// ============================================================================

/// Initialize a new path state for a pixel
CUDA_DEVICE_FUNCTION CUDA_INLINE void initializePathState(
    WavefrontPathState& pathState,
    uint32_t pixelX,
    uint32_t pixelY,
    const KernelRNG& rng) {
    
    pathState.origin = Point3D(0, 0, 0);
    pathState.direction = Vector3D(0, 0, 1);
    pathState.throughput = SampledSpectrum::Zero();
    pathState.contribution = SampledSpectrum::Zero();
    pathState.initImportance = 1.0f;
    pathState.selectWLPDF = 1.0f;
    pathState.rng = rng;
    pathState.prevDirPDF = 0.0f;
    pathState.prevSampledType = DirectionType();
    pathState.pathLength = 0;
    pathState.pixelX = pixelX;
    pathState.pixelY = pixelY;
    pathState.flags = 0;
    pathState.materialCategory = MaterialCategory_Diffuse;
    pathState.setActive(true);
}


// ============================================================================
// Ray Generation Helpers
// ============================================================================

/// Generate primary ray from camera
CUDA_DEVICE_FUNCTION CUDA_INLINE void generateCameraRay(
    WavefrontPathState& pathState,
    float screenX,
    float screenY,
    const CameraDescriptor& camera) {
    
    // Sample lens position
    LensPosSample lensPosSample;
    ProgSigSampleLensPosition sampleLensPos(wlp.progSampleLensPosition);
    sampleLensPos(pathState.rng.getFloat0cTo1o(), pathState.rng.getFloat0cTo1o(), &lensPosSample);
    
    // Compute image plane position
    float vh = 2.0f * std::tan(camera.fovY * 0.5f);
    float vw = camera.aspect * vh;
    
    Vector3D rayDir = normalize(
        camera.orientation.x * vw * (screenX - 0.5f) +
        camera.orientation.y * vh * (screenY - 0.5f) +
        camera.orientation.z);
    
    // Set ray origin and direction
    pathState.origin = camera.position + lensPosSample.position;
    pathState.direction = rayDir;
    
    // Evaluate IDF
    IDFSample idfSample;
    idfSample.dirLocal = camera.orientation.toLocal(rayDir);
    idfSample.positionLocal = lensPosSample.position;
    
    ProgSigEvaluateIDF evaluateIDF(camera.progEvaluateIDF);
    SampledSpectrum We = evaluateIDF(idfSample, pathState.wls);
    
    // Initialize throughput
    pathState.throughput = We / (lensPosSample.areaPDF * idfSample.dirPDF);
    pathState.initImportance = pathState.throughput.importance(pathState.wls.selectedLambdaIndex());
}


// ============================================================================
// Light Sampling Helpers
// ============================================================================

/// Sample a light source in the scene
/// Returns true if sampling succeeded
CUDA_DEVICE_FUNCTION CUDA_INLINE bool sampleLight(
    WavefrontPathState& pathState,
    const SurfacePoint& shadingSurfPt,
    WavefrontLightSample* lightSample) {
    
    // Sample light instance
    float uLight = pathState.rng.getFloat0cTo1o();
    float instProb;
    uint32_t instIndex = wlp.lightInstDist.sample(uLight, &instProb);
    
    const Instance& inst = wlp.instBuffer[instIndex];
    
    // Sample geometry instance
    float uGeomInst = pathState.rng.getFloat0cTo1o();
    float geomInstProb;
    uint32_t geomInstIndexInInst = inst.lightGeomInstDistribution.sample(uGeomInst, &geomInstProb);
    uint32_t geomInstIndex = inst.geomInstIndices[geomInstIndexInInst];
    
    const GeometryInstance& geomInst = wlp.geomInstBuffer[geomInstIndex];
    
    // Sample position on light
    float uPos0 = pathState.rng.getFloat0cTo1o();
    float uPos1 = pathState.rng.getFloat0cTo1o();
    
    LightPosSample lightPosSample;
    ProgSigSampleLightPosition sampleLightPos(geomInst.progSampleLightPosition);
    sampleLightPos(uPos0, uPos1, &lightPosSample);
    
    // Transform to world space
    lightSample->lightSurfPt = inst.transform * lightPosSample.surfPt;
    
    // Compute light PDF
    lightSample->lightPDF = instProb * geomInstProb * lightPosSample.areaPDF;
    
    // Evaluate EDF
    const SurfaceMaterialDescriptor& matDesc = wlp.materialDescriptorBuffer[geomInst.materialIndex];
    EDF edf(matDesc, lightSample->lightSurfPt, pathState.wls);
    
    Vector3D shadowRayDir = lightSample->lightSurfPt.position - shadingSurfPt.position;
    float dist = length(shadowRayDir);
    shadowRayDir /= dist;
    
    Vector3D dirOutLocal = lightSample->lightSurfPt.shadingFrame.toLocal(-shadowRayDir);
    SampledSpectrum spEmittance = edf.evaluateEmittance();
    
    if (!spEmittance.hasNonZero())
        return false;
    
    EDFQuery feQuery(DirectionType::All(), pathState.wls);
    lightSample->Le = spEmittance * edf.evaluate(feQuery, dirOutLocal);
    
    // Convert area PDF to solid angle PDF
    float cosTerm = abs(dirOutLocal.z);
    lightSample->lightPDF = convertAreaPDFToSolidAnglePDF(
        lightSample->lightPDF, dist, cosTerm);
    
    return true;
}


// ============================================================================
// BSDF Evaluation Helpers
// ============================================================================

/// Evaluate BSDF for a given direction
CUDA_DEVICE_FUNCTION CUDA_INLINE SampledSpectrum evaluateBSDF(
    const BSDF<TransportMode::Radiance>& bsdf,
    const Vector3D& dirInLocal,
    const Vector3D& dirOutLocal,
    const WavelengthSamples& wls,
    float* pdf) {
    
    BSDFQuery fsQuery(dirInLocal, dirOutLocal, wls, DirectionType::All(), TransportMode::Radiance);
    SampledSpectrum fs = bsdf.evaluate(fsQuery);
    
    if (pdf) {
        BSDFQuery bsdfPDFQuery(dirInLocal, dirOutLocal, wls, fsQuery.dirTypeFilter, TransportMode::Radiance);
        *pdf = bsdf.evaluatePDF(bsdfPDFQuery);
    }
    
    return fs;
}


/// Sample BSDF direction
CUDA_DEVICE_FUNCTION CUDA_INLINE bool sampleBSDF(
    const BSDF<TransportMode::Radiance>& bsdf,
    const Vector3D& dirInLocal,
    WavefrontPathState& pathState,
    WavefrontBSDFSample* bsdfSample) {
    
    float uDir0 = pathState.rng.getFloat0cTo1o();
    float uDir1 = pathState.rng.getFloat0cTo1o();
    
    BSDFSample sample;
    BSDFQuery fsQuery(dirInLocal, pathState.wls, DirectionType::All(), TransportMode::Radiance);
    SampledSpectrum fs = bsdf.sample(fsQuery, uDir0, uDir1, &sample);
    
    if (fs == SampledSpectrum::Zero() || sample.dirPDF == 0.0f) {
        bsdfSample->isValid = false;
        return false;
    }
    
    bsdfSample->dirLocal = sample.dirLocal;
    bsdfSample->f = fs;
    bsdfSample->pdf = sample.dirPDF;
    bsdfSample->sampledType = sample.dirType;
    bsdfSample->isValid = true;
    
    return true;
}


// ============================================================================
// Contribution Accumulation
// ============================================================================

/// Accumulate path contribution to output buffer
CUDA_DEVICE_FUNCTION CUDA_INLINE void accumulateContribution(
    const WavefrontPathState& pathState,
    SpectrumStorage* accumBuffer,
    uint32_t imageStride) {
    
    if (pathState.contribution.hasNonZero()) {
        uint32_t linearIndex = pathState.pixelY * imageStride + pathState.pixelX;
        
        DiscretizedSpectrum contrib = pathState.contribution.toDiscretizedSpectrum(pathState.wls);
        
        // Atomic add to accumulation buffer
        atomicAdd(&accumBuffer[linearIndex].r, contrib.r);
        atomicAdd(&accumBuffer[linearIndex].g, contrib.g);
        atomicAdd(&accumBuffer[linearIndex].b, contrib.b);
    }
}


// ============================================================================
// Debug and Validation
// ============================================================================

/// Validate path state (for debugging)
CUDA_DEVICE_FUNCTION CUDA_INLINE bool validatePathState(
    const WavefrontPathState& pathState) {
    
    if (!WavefrontConfig::EnableValidation)
        return true;
    
    // Check for NaN/Inf
    if (isnan(pathState.origin.x) || isinf(pathState.origin.x))
        return false;
    if (isnan(pathState.direction.x) || isinf(pathState.direction.x))
        return false;
    if (isnan(pathState.throughput.values[0]) || isinf(pathState.throughput.values[0]))
        return false;
    
    // Check direction is normalized
    float dirLenSq = dot(pathState.direction, pathState.direction);
    if (abs(dirLenSq - 1.0f) > 0.001f)
        return false;
    
    // Check throughput is non-negative
    for (int i = 0; i < NumSpectralSamples; ++i) {
        if (pathState.throughput.values[i] < 0.0f)
            return false;
    }
    
    return true;
}


/// Print path state for debugging
CUDA_DEVICE_FUNCTION CUDA_INLINE void printPathState(
    const WavefrontPathState& pathState,
    const char* label = "") {
    
    vlrprintf("=== Path State %s ===\n", label);
    vlrprintf("Pixel: (%u, %u)\n", pathState.pixelX, pathState.pixelY);
    vlrprintf("Origin: (%.3f, %.3f, %.3f)\n", 
        pathState.origin.x, pathState.origin.y, pathState.origin.z);
    vlrprintf("Direction: (%.3f, %.3f, %.3f)\n",
        pathState.direction.x, pathState.direction.y, pathState.direction.z);
    vlrprintf("Path Length: %u\n", pathState.pathLength);
    vlrprintf("Active: %d, Terminated: %d\n", 
        pathState.isActive(), pathState.isTerminated());
    vlrprintf("Throughput importance: %.6f\n",
        pathState.throughput.importance(pathState.wls.selectedLambdaIndex()));
}


#endif // VLR_Device

} // namespace shared
} // namespace vlr
