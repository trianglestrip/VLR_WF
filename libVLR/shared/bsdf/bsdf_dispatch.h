#pragma once

#include "lambert.h"
#include "ggx_reflection.h"
#include "specular.h"
#include "scattering.h"
#include "transmission.h"
#include "advanced_brdf.h"
#include "composite.h"

namespace vlr {
namespace shared {

// ============================================================================
// 5. 统一 BSDF 接口（分发给具体实现）
// ============================================================================

/// 前向声明：完整采样接口（三随机数），供 sampleBSDF 调用
CUDA_DEVICE_FUNCTION CUDA_INLINE void sampleBSDFWithU2(
    const BSDFContext& ctx,
    const Vector3D& dirInLocal,
    float u0, float u1, float u2,
    BSDFSampleResult* result);

/// 根据材质类型评估 BSDF
CUDA_DEVICE_FUNCTION CUDA_INLINE SampledSpectrum evaluateBSDF(
    const BSDFContext& ctx,
    const Vector3D& dirInLocal,
    const Vector3D& dirOutLocal) {

    const SurfaceMaterialDescriptor& matDesc = *ctx.matDesc;
    BSDFType type = getBSDFType(matDesc);

    switch (type) {
    case BSDFType_Lambert: {
        SampledSpectrum albedo;
        getEffectiveLambertAlbedo(matDesc, ctx.texturedParams, &albedo);
        return evaluateLambertBSDF(albedo, dirInLocal, dirOutLocal, ctx.geomNormalLocal);
    }
    case BSDFType_LambertCheckerboard: {
        SampledSpectrum albedo;
        getLambertAlbedoCheckerboard(matDesc, ctx.surfPt, &albedo);
        return evaluateLambertBSDF(albedo, dirInLocal, dirOutLocal, ctx.geomNormalLocal);
    }
    case BSDFType_LambertianScattering: {
        SampledSpectrum albedo;
        getEffectiveLambertAlbedo(matDesc, ctx.texturedParams, &albedo);
        return evaluateLambertianScatteringBSDF(albedo, dirInLocal, dirOutLocal, ctx.geomNormalLocal);
    }
    case BSDFType_GGX: {
        SampledSpectrum reflectance;
        float roughness;
        getEffectiveGGXParams(matDesc, ctx.texturedParams, &reflectance, &roughness);
        return evaluateGGXBSDF(reflectance, roughness, dirInLocal, dirOutLocal, ctx.geomNormalLocal);
    }
    case BSDFType_MicrofacetReflection: {
        SampledSpectrum coeffR, eta, kappa;
        getEffectiveLambertAlbedo(matDesc, ctx.texturedParams, &coeffR);
        float roughness;
        getMicrofacetReflectionParams(matDesc, &eta, &kappa, &roughness);
        return evaluateMicrofacetReflectionBSDF(coeffR, eta, kappa, roughness, dirInLocal, dirOutLocal, ctx.geomNormalLocal);
    }
    case BSDFType_MicrofacetScattering: {
        float ior, roughness;
        SampledSpectrum coeff;
        getMicrofacetScatteringParams(matDesc, &ior, &roughness, &coeff);
        return evaluateMicrofacetScatteringBSDF(ior, roughness, coeff, dirInLocal, dirOutLocal, ctx.geomNormalLocal);
    }
    case BSDFType_Specular: {
        SampledSpectrum coeffR, eta, kappa;
        getSpecularConductorParams(matDesc, &coeffR, &eta, &kappa);
        return evaluateSpecularBSDF(coeffR, eta, kappa, dirInLocal, dirOutLocal, ctx.geomNormalLocal);
    }
    case BSDFType_SpecularTransmission: {
        float ior, disp;
        getTransmissionParams(matDesc, &ior, &disp);
        SampledSpectrum trans;
        getEffectiveLambertAlbedo(matDesc, ctx.texturedParams, &trans);
        return evaluateSpecularTransmissionBSDF_FrontFace(ior, trans, dirInLocal, dirOutLocal, ctx.geomNormalLocal, ctx.surfPt->isFrontFace);
    }
    case BSDFType_GGXTransmission: {
        float ior, disp;
        getTransmissionParams(matDesc, &ior, &disp);
        SampledSpectrum refl;
        float roughness;
        getEffectiveGGXParams(matDesc, ctx.texturedParams, &refl, &roughness);
        SampledSpectrum trans;
        for (int i = 0; i < NumSpectralSamples; ++i) trans.values[i] = 1.0f;
        return evaluateGGXTransmissionBSDF(trans, roughness, 1.0f, ior, dirInLocal, dirOutLocal, ctx.geomNormalLocal);
    }
    case BSDFType_FresnelBlend: {
        SampledSpectrum diff, spec;
        float roughness;
        getEffectiveFresnelBlendParams(matDesc, ctx.texturedParams, &diff, &spec, &roughness);
        return evaluateFresnelBlendBSDF(diff, spec, roughness, dirInLocal, dirOutLocal, ctx.geomNormalLocal);
    }
    case BSDFType_UE4BRDF:
    case BSDFType_FrostbiteBRDF: {
        SampledSpectrum baseColor;
        float metallic, roughness;
        getEffectiveUE4Params(matDesc, ctx.texturedParams, &baseColor, &metallic, &roughness);
        return evaluateUE4BRDF(baseColor, metallic, roughness, dirInLocal, dirOutLocal, ctx.geomNormalLocal);
    }
    case BSDFType_DisneyBRDF: {
        SampledSpectrum baseColor;
        float metallic, subsurface, specular, roughness, specularTint;
        float anisotropic, sheen, sheenTint, clearcoat, clearcoatGloss;
        getDisneyParams(matDesc, &baseColor, &metallic, &subsurface, &specular, &roughness,
            &specularTint, &anisotropic, &sheen, &sheenTint, &clearcoat, &clearcoatGloss);
        return evaluateDisneyBRDF(baseColor, metallic, subsurface, specular, roughness,
            specularTint, anisotropic, sheen, sheenTint, clearcoat, clearcoatGloss,
            dirInLocal, dirOutLocal, ctx.geomNormalLocal);
    }
    case BSDFType_MixedBSDF: {
        SampledSpectrum alb0, alb1;
        float weight, roughness;
        getMixedParams(matDesc, &alb0, &alb1, &weight, &roughness);
        return evaluateMixedBSDF(alb0, alb1, weight, roughness, dirInLocal, dirOutLocal, ctx.geomNormalLocal);
    }
    case BSDFType_MultiSurface: {
        int numLayers;
        BSDFType subTypes[4];
        SampledSpectrum subAlbedos[4];
        float subRoughness[4], weights[4];
        getMultiSurfaceParams(matDesc, &numLayers, subTypes, subAlbedos, subRoughness, weights);
        return evaluateMultiSurfaceBSDF(numLayers, subTypes, subAlbedos, subRoughness, weights,
            dirInLocal, dirOutLocal, ctx.geomNormalLocal);
    }
    default:
        return SampledSpectrum::Zero();
    }
}

/// 根据材质类型采样 BSDF
/// 需要 u2 时从 result 或外部传入，此处用 u0 替代第三次随机数
CUDA_DEVICE_FUNCTION CUDA_INLINE void sampleBSDF(
    const BSDFContext& ctx,
    const Vector3D& dirInLocal,
    float u0, float u1,
    BSDFSampleResult* result) {
    sampleBSDFWithU2(ctx, dirInLocal, u0, u1, u0, result);
}

/// 完整采样接口（支持需要三随机数的 BSDF）
CUDA_DEVICE_FUNCTION CUDA_INLINE void sampleBSDFWithU2(
    const BSDFContext& ctx,
    const Vector3D& dirInLocal,
    float u0, float u1, float u2,
    BSDFSampleResult* result) {

    const SurfaceMaterialDescriptor& matDesc = *ctx.matDesc;
    BSDFType type = getBSDFType(matDesc);
    const WavelengthSamples* wls = ctx.wls;
    bool singleWl = ctx.singleWlSelected;

#if defined(__CUDA_ARCH__) && defined(VLR_DEBUG_MATERIAL)
    if (threadIdx.x == 0 && blockIdx.x == 0) {
        printf("[GPU sampleBSDFWithU2] BSDFType=%u\n", (uint32_t)type);
    }
#endif

    switch (type) {
    case BSDFType_Lambert: {
        SampledSpectrum albedo;
        getEffectiveLambertAlbedo(matDesc, ctx.texturedParams, &albedo);
        sampleLambertBSDF(albedo, dirInLocal, ctx.geomNormalLocal, u0, u1, result);
        break;
    }
    case BSDFType_LambertCheckerboard: {
        SampledSpectrum albedo;
        getLambertAlbedoCheckerboard(matDesc, ctx.surfPt, &albedo);
        sampleLambertBSDF(albedo, dirInLocal, ctx.geomNormalLocal, u0, u1, result);
        result->sampledBSDFType = BSDFType_LambertCheckerboard;
        break;
    }
    case BSDFType_LambertianScattering: {
        SampledSpectrum albedo;
        getLambertAlbedo(matDesc, &albedo);
        sampleLambertianScatteringBSDF(albedo, dirInLocal, ctx.geomNormalLocal, u0, u1, u2, result);
        break;
    }
    case BSDFType_GGX: {
        SampledSpectrum reflectance;
        float roughness;
        getEffectiveGGXParams(matDesc, ctx.texturedParams, &reflectance, &roughness);
        sampleGGXBSDF(reflectance, roughness, dirInLocal, ctx.geomNormalLocal, u0, u1, result);
        break;
    }
    case BSDFType_MicrofacetReflection: {
#if defined(__CUDA_ARCH__) && defined(VLR_DEBUG_MATERIAL)
        if (threadIdx.x == 0 && blockIdx.x == 0) {
            printf("[GPU sampleBSDFWithU2] Entering MicrofacetReflection branch\n");
        }
#endif
        SampledSpectrum coeffR, eta, kappa;
        getEffectiveLambertAlbedo(matDesc, ctx.texturedParams, &coeffR);
        float roughness, anisotropy;
        getMicrofacetReflectionParamsAniso(matDesc, &eta, &kappa, &roughness, &anisotropy);
#if defined(__CUDA_ARCH__) && defined(VLR_DEBUG_MATERIAL)
        if (threadIdx.x == 0 && blockIdx.x == 0) {
            printf("  coeffR=(%.3f,%.3f,%.3f), eta=(%.3f,%.3f,%.3f), kappa=(%.3f,%.3f,%.3f), roughness=%.3f, anisotropy=%.3f\n",
                coeffR.values[0], coeffR.values[1], coeffR.values[2],
                eta.values[0], eta.values[1], eta.values[2],
                kappa.values[0], kappa.values[1], kappa.values[2], roughness, anisotropy);
        }
#endif
        if (anisotropy > 0.001f) {
            float alphaX, alphaY;
            roughnessAnisotropyToAlpha(roughness, anisotropy, &alphaX, &alphaY);
            sampleMicrofacetReflectionBSDF_Aniso(coeffR, eta, kappa, alphaX, alphaY, dirInLocal, ctx.geomNormalLocal, u0, u1, result);
        } else {
            sampleMicrofacetReflectionBSDF(coeffR, eta, kappa, roughness, dirInLocal, ctx.geomNormalLocal, u0, u1, result);
        }
        break;
    }
    case BSDFType_MicrofacetScattering: {
        float ior, roughness;
        SampledSpectrum coeff;
        getMicrofacetScatteringParams(matDesc, &ior, &roughness, &coeff);
#ifdef __CUDA_ARCH__
        if (blockIdx.x * blockDim.x + threadIdx.x < 5) {
            printf("[sampleBSDFWithU2] Calling MicrofacetScattering: pathIdx=%u, ior=%.2f, roughness=%.4f\n",
                   blockIdx.x * blockDim.x + threadIdx.x, ior, roughness);
        }
#endif
        sampleMicrofacetScatteringBSDF(ior, roughness, coeff, dirInLocal, ctx.geomNormalLocal, u0, u1, u2, result);
        break;
    }
    case BSDFType_Specular: {
        SampledSpectrum coeffR, eta, kappa;
        getSpecularConductorParams(matDesc, &coeffR, &eta, &kappa);
        sampleSpecularBSDF(coeffR, eta, kappa, dirInLocal, ctx.geomNormalLocal, u0, u1, result);
        break;
    }
    case BSDFType_SpecularTransmission: {
        float ior, disp;
        getTransmissionParams(matDesc, &ior, &disp);
        
        SampledSpectrum transmittance;
        for (int i = 0; i < NumSpectralSamples; ++i) {
            transmittance.values[i] = 1.0f;
        }
        
        sampleDielectricBSDF_PBRT(1.0f, ior, transmittance, dirInLocal, ctx.geomNormalLocal,
            ctx.surfPt->isFrontFace, u0, result);
        break;
    }
    case BSDFType_GGXTransmission: {
        float ior, disp;
        getTransmissionParams(matDesc, &ior, &disp);
        SampledSpectrum trans;
        getEffectiveLambertAlbedo(matDesc, ctx.texturedParams, &trans);
        SampledSpectrum refl;
        float roughness;
        getEffectiveGGXParams(matDesc, ctx.texturedParams, &refl, &roughness);
        sampleGGXTransmissionBSDF(trans, roughness, 1.0f, ior,
            dirInLocal, ctx.geomNormalLocal, u0, u1, u2, result);
        break;
    }
    case BSDFType_FresnelBlend: {
        SampledSpectrum diff, spec;
        float roughness;
        getEffectiveFresnelBlendParams(matDesc, ctx.texturedParams, &diff, &spec, &roughness);
        sampleFresnelBlendBSDF(diff, spec, roughness, dirInLocal, ctx.geomNormalLocal, u0, u1, u2, result);
        break;
    }
    case BSDFType_UE4BRDF:
    case BSDFType_FrostbiteBRDF: {
        SampledSpectrum baseColor;
        float metallic, roughness;
        getEffectiveUE4Params(matDesc, ctx.texturedParams, &baseColor, &metallic, &roughness);
        sampleGGXBSDF(baseColor, roughness, dirInLocal, ctx.geomNormalLocal, u0, u1, result);
        result->sampledBSDFType = type;
        break;
    }
    case BSDFType_DisneyBRDF: {
        SampledSpectrum baseColor;
        float metallic, subsurface, specular, roughness, specularTint;
        float anisotropic, sheen, sheenTint, clearcoat, clearcoatGloss;
        getDisneyParams(matDesc, &baseColor, &metallic, &subsurface, &specular, &roughness,
            &specularTint, &anisotropic, &sheen, &sheenTint, &clearcoat, &clearcoatGloss);
        sampleDisneyBRDF(baseColor, metallic, subsurface, specular, roughness,
            specularTint, anisotropic, sheen, sheenTint, clearcoat, clearcoatGloss,
            dirInLocal, ctx.geomNormalLocal, u0, u1, u2, result);
        break;
    }
    case BSDFType_MixedBSDF: {
        SampledSpectrum alb0, alb1;
        float weight, roughness;
        getMixedParams(matDesc, &alb0, &alb1, &weight, &roughness);
        if (u2 < weight) {
            sampleGGXBSDF(alb1, roughness, dirInLocal, ctx.geomNormalLocal, u0, u1, result);
            result->pdf *= weight;
        } else {
            sampleLambertBSDF(alb0, dirInLocal, ctx.geomNormalLocal, u0, u1, result);
            result->pdf *= (1.0f - weight);
        }
        result->sampledBSDFType = BSDFType_MixedBSDF;
        break;
    }
    case BSDFType_MultiSurface: {
        int numLayers;
        BSDFType subTypes[4];
        SampledSpectrum subAlbedos[4];
        float subRoughness[4], weights[4];
        getMultiSurfaceParams(matDesc, &numLayers, subTypes, subAlbedos, subRoughness, weights);
        sampleMultiSurfaceBSDF(numLayers, subTypes, subAlbedos, subRoughness, weights,
            dirInLocal, ctx.geomNormalLocal, u0, u1, u2, result);
        break;
    }
    default: {
        result->pdf = 0.0f;
        result->f = SampledSpectrum::Zero();
        result->isDelta = false;
        break;
    }
    }
}

/// 根据材质类型计算 BSDF PDF
CUDA_DEVICE_FUNCTION CUDA_INLINE float getBSDFPDF(
    const BSDFContext& ctx,
    const Vector3D& dirInLocal,
    const Vector3D& dirOutLocal) {

    const SurfaceMaterialDescriptor& matDesc = *ctx.matDesc;
    BSDFType type = getBSDFType(matDesc);

    switch (type) {
    case BSDFType_Lambert:
        return getLambertBSDFPDF(dirOutLocal, ctx.geomNormalLocal);
    case BSDFType_LambertCheckerboard:
        return getLambertBSDFPDF(dirOutLocal, ctx.geomNormalLocal);
    case BSDFType_LambertianScattering:
        return getLambertianScatteringBSDFPDF(dirInLocal, dirOutLocal, ctx.geomNormalLocal);
    case BSDFType_GGX: {
        SampledSpectrum reflectance;
        float roughness;
        getEffectiveGGXParams(matDesc, ctx.texturedParams, &reflectance, &roughness);
        return getGGXBSDFPDF(reflectance, roughness, dirInLocal, dirOutLocal, ctx.geomNormalLocal);
    }
    case BSDFType_MicrofacetReflection: {
        float roughness, anisotropy;
        SampledSpectrum eta, kappa;
        getMicrofacetReflectionParamsAniso(matDesc, &eta, &kappa, &roughness, &anisotropy);
        if (anisotropy > 0.001f) {
            float alphaX, alphaY;
            roughnessAnisotropyToAlpha(roughness, anisotropy, &alphaX, &alphaY);
            return getMicrofacetReflectionBSDFPDF_Aniso(alphaX, alphaY, dirInLocal, dirOutLocal, ctx.geomNormalLocal);
        } else {
            return getMicrofacetReflectionBSDFPDF(roughness, dirInLocal, dirOutLocal, ctx.geomNormalLocal);
        }
    }
    case BSDFType_MicrofacetScattering: {
        float ior, roughness;
        getMicrofacetScatteringParams(matDesc, &ior, &roughness);
        return getMicrofacetScatteringBSDFPDF(ior, roughness, dirInLocal, dirOutLocal, ctx.geomNormalLocal);
    }
    case BSDFType_Specular:
        return getSpecularBSDFPDF(dirInLocal, dirOutLocal, ctx.geomNormalLocal);
    case BSDFType_SpecularTransmission: {
        float ior, disp;
        getTransmissionParams(matDesc, &ior, &disp);
        return getSpecularTransmissionBSDFPDF_FrontFace(ior, dirInLocal, dirOutLocal, ctx.geomNormalLocal, ctx.surfPt->isFrontFace);
    }
    case BSDFType_GGXTransmission: {
        SampledSpectrum refl;
        float roughness;
        getEffectiveGGXParams(matDesc, ctx.texturedParams, &refl, &roughness);
        return getGGXBSDFPDF(refl, roughness, dirInLocal, dirOutLocal, ctx.geomNormalLocal);
    }
    case BSDFType_FresnelBlend:
    case BSDFType_UE4BRDF:
    case BSDFType_FrostbiteBRDF: {
        SampledSpectrum refl;
        float roughness;
        getEffectiveGGXParams(matDesc, ctx.texturedParams, &refl, &roughness);
        return getGGXBSDFPDF(refl, roughness, dirInLocal, dirOutLocal, ctx.geomNormalLocal);
    }
    case BSDFType_DisneyBRDF: {
        SampledSpectrum baseColor;
        float metallic, subsurface, specular, roughness, specularTint;
        float anisotropic, sheen, sheenTint, clearcoat, clearcoatGloss;
        getDisneyParams(matDesc, &baseColor, &metallic, &subsurface, &specular, &roughness,
            &specularTint, &anisotropic, &sheen, &sheenTint, &clearcoat, &clearcoatGloss);
        return getDisneyBRDFPDF(baseColor, metallic, subsurface, specular, specularTint,
            roughness, anisotropic, sheen, clearcoat, clearcoatGloss, dirInLocal, dirOutLocal, ctx.geomNormalLocal);
    }
    case BSDFType_MixedBSDF: {
        float weight, roughness;
        SampledSpectrum a0, a1;
        getMixedParams(matDesc, &a0, &a1, &weight, &roughness);
        SampledSpectrum refl;
        getEffectiveGGXParams(matDesc, ctx.texturedParams, &refl, &roughness);
        float pdfGGX = getGGXBSDFPDF(refl, roughness, dirInLocal, dirOutLocal, ctx.geomNormalLocal);
        float pdfLambert = getLambertBSDFPDF(dirOutLocal, ctx.geomNormalLocal);
        return (1.0f - weight) * pdfLambert + weight * pdfGGX;
    }
    case BSDFType_MultiSurface: {
        int numLayers;
        BSDFType subTypes[4];
        SampledSpectrum subAlbedos[4];
        float subRoughness[4], weights[4];
        getMultiSurfaceParams(matDesc, &numLayers, subTypes, subAlbedos, subRoughness, weights);
        return getMultiSurfaceBSDFPDF(numLayers, subTypes, subAlbedos, subRoughness, weights,
            dirInLocal, dirOutLocal, ctx.geomNormalLocal);
    }
    default:
        return 0.0f;
    }
}

} // namespace shared
} // namespace vlr
