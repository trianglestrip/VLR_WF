#pragma once

#include "lambert.h"
#include "ggx_reflection.h"
#include "specular.h"

namespace vlr {
namespace shared {

// ============================================================================
// 4.10 混合 BSDF（多层材质）
// ============================================================================

/// 混合 BSDF：f = (1-w)*f0 + w*f1，f0=Lambert, f1=GGX
CUDA_DEVICE_FUNCTION CUDA_INLINE SampledSpectrum evaluateMixedBSDF(
    const SampledSpectrum& albedo0, const SampledSpectrum& albedo1,
    float blendWeight, float roughness,
    const Vector3D& dirInLocal, const Vector3D& dirOutLocal,
    const Normal3D& geomNormalLocal) {

    SampledSpectrum f0 = evaluateLambertBSDF(albedo0, dirInLocal, dirOutLocal, geomNormalLocal);
    SampledSpectrum f1 = evaluateGGXBSDF(albedo1, roughness, dirInLocal, dirOutLocal, geomNormalLocal);
    SampledSpectrum result;
    float w = ::vlr::vlr_max(0.0f, ::vlr::vlr_min(1.0f, blendWeight));
    for (int i = 0; i < NumSpectralSamples; ++i)
        result.values[i] = (1.0f - w) * f0.values[i] + w * f1.values[i];
    return result;
}

// ============================================================================
// 4.10 多表面材质 MultiSurface（2-4 层）
// ============================================================================

/// 评估单个子材质 BSDF（支持 Lambert, GGX, Specular, UE4BRDF, FrostbiteBRDF, MicrofacetReflection）
CUDA_DEVICE_FUNCTION CUDA_INLINE SampledSpectrum evaluateSubBSDF(
    BSDFType subType,
    const SampledSpectrum& albedo,
    float roughness,
    const Vector3D& dirInLocal, const Vector3D& dirOutLocal,
    const Normal3D& geomNormalLocal) {
    switch (subType) {
    case BSDFType_Lambert:
        return evaluateLambertBSDF(albedo, dirInLocal, dirOutLocal, geomNormalLocal);
    case BSDFType_GGX:
    case BSDFType_UE4BRDF:
    case BSDFType_FrostbiteBRDF:
    case BSDFType_MicrofacetReflection:
        return evaluateGGXBSDF(albedo, roughness, dirInLocal, dirOutLocal, geomNormalLocal);
    case BSDFType_Specular: {
        SampledSpectrum eta, kappa;
        for (int i = 0; i < NumSpectralSamples; ++i) {
            eta.values[i] = 1.0f;
            kappa.values[i] = 0.0f;
        }
        return evaluateSpecularBSDF(albedo, eta, kappa, dirInLocal, dirOutLocal, geomNormalLocal);
    }
    default:
        return evaluateLambertBSDF(albedo, dirInLocal, dirOutLocal, geomNormalLocal);
    }
}

/// 获取单个子材质 BSDF 的 PDF
CUDA_DEVICE_FUNCTION CUDA_INLINE float getSubBSDFPDF(
    BSDFType subType,
    const SampledSpectrum& albedo,
    float roughness,
    const Vector3D& dirInLocal, const Vector3D& dirOutLocal,
    const Normal3D& geomNormalLocal) {
    switch (subType) {
    case BSDFType_Lambert:
        return getLambertBSDFPDF(dirOutLocal, geomNormalLocal);
    case BSDFType_GGX:
    case BSDFType_UE4BRDF:
    case BSDFType_FrostbiteBRDF:
    case BSDFType_MicrofacetReflection:
        return getGGXBSDFPDF(albedo, roughness, dirInLocal, dirOutLocal, geomNormalLocal);
    case BSDFType_Specular:
        return getSpecularBSDFPDF(dirInLocal, dirOutLocal, geomNormalLocal);
    default:
        return getLambertBSDFPDF(dirOutLocal, geomNormalLocal);
    }
}

/// 评估 MultiSurface BSDF: f_total = Σ(weight_i * f_i)
CUDA_DEVICE_FUNCTION CUDA_INLINE SampledSpectrum evaluateMultiSurfaceBSDF(
    int numLayers,
    const BSDFType subTypes[4],
    const SampledSpectrum subAlbedos[4],
    const float subRoughness[4],
    const float weights[4],
    const Vector3D& dirInLocal, const Vector3D& dirOutLocal,
    const Normal3D& geomNormalLocal) {
    SampledSpectrum result = SampledSpectrum::Zero();
    for (int i = 0; i < numLayers; ++i) {
        SampledSpectrum fi = evaluateSubBSDF(subTypes[i], subAlbedos[i], subRoughness[i],
            dirInLocal, dirOutLocal, geomNormalLocal);
        for (int c = 0; c < NumSpectralSamples; ++c)
            result.values[c] += weights[i] * fi.values[c];
    }
    return result;
}

/// 采样 MultiSurface BSDF：按权重离散选择一层，PDF = Σ(weight_i * pdf_i)
CUDA_DEVICE_FUNCTION CUDA_INLINE void sampleMultiSurfaceBSDF(
    int numLayers,
    const BSDFType subTypes[4],
    const SampledSpectrum subAlbedos[4],
    const float subRoughness[4],
    const float weights[4],
    const Vector3D& dirInLocal,
    const Normal3D& geomNormalLocal,
    float u0, float u1, float u2,
    BSDFSampleResult* result) {
    float cum = 0.0f;
    int sel = numLayers - 1;
    for (int i = 0; i < numLayers; ++i) {
        cum += weights[i];
        if (u2 < cum) { sel = i; break; }
        sel = i;
    }
    switch (subTypes[sel]) {
    case BSDFType_Lambert:
        sampleLambertBSDF(subAlbedos[sel], dirInLocal, geomNormalLocal, u0, u1, result);
        break;
    case BSDFType_GGX:
    case BSDFType_UE4BRDF:
    case BSDFType_FrostbiteBRDF:
    case BSDFType_MicrofacetReflection:
        sampleGGXBSDF(subAlbedos[sel], subRoughness[sel], dirInLocal, geomNormalLocal, u0, u1, result);
        break;
    case BSDFType_Specular: {
        SampledSpectrum eta, kappa;
        for (int i = 0; i < NumSpectralSamples; ++i) {
            eta.values[i] = 1.0f;
            kappa.values[i] = 0.0f;
        }
        sampleSpecularBSDF(subAlbedos[sel], eta, kappa, dirInLocal, geomNormalLocal, u0, u1, result);
        break;
    }
    default:
        sampleLambertBSDF(subAlbedos[sel], dirInLocal, geomNormalLocal, u0, u1, result);
        break;
    }
    result->sampledBSDFType = BSDFType_MultiSurface;
    float pdfMix = 0.0f;
    for (int i = 0; i < numLayers; ++i) {
        float pdfi = getSubBSDFPDF(subTypes[i], subAlbedos[i], subRoughness[i],
            dirInLocal, result->dirLocal, geomNormalLocal);
        pdfMix += weights[i] * pdfi;
    }
    result->pdf = pdfMix;
}

/// MultiSurface BSDF 的 PDF
CUDA_DEVICE_FUNCTION CUDA_INLINE float getMultiSurfaceBSDFPDF(
    int numLayers,
    const BSDFType subTypes[4],
    const SampledSpectrum subAlbedos[4],
    const float subRoughness[4],
    const float weights[4],
    const Vector3D& dirInLocal, const Vector3D& dirOutLocal,
    const Normal3D& geomNormalLocal) {
    float pdf = 0.0f;
    for (int i = 0; i < numLayers; ++i) {
        float pdfi = getSubBSDFPDF(subTypes[i], subAlbedos[i], subRoughness[i],
            dirInLocal, dirOutLocal, geomNormalLocal);
        pdf += weights[i] * pdfi;
    }
    return pdf;
}


} // namespace shared
} // namespace vlr
