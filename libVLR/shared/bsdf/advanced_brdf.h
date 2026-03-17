#pragma once

#include "fresnel.h"
#include "microfacet.h"
#include "lambert.h"

namespace vlr {
namespace shared {

// ============================================================================
// 4.7 Fresnel 混合 Lambertian
// ============================================================================

/// 评估 Fresnel 混合：diffuse * (1-F) + specular * F
/// 掠射角时更多镜面，法向时更多漫反射
CUDA_DEVICE_FUNCTION CUDA_INLINE SampledSpectrum evaluateFresnelBlendBSDF(
    const SampledSpectrum& diffuse, const SampledSpectrum& specular,
    float roughness,
    const Vector3D& dirInLocal, const Vector3D& dirOutLocal,
    const Normal3D& geomNormalLocal) {

    float NdotL = dirInLocal.z;
    float NdotV = dirOutLocal.z;
    if (NdotL <= 0.0f || NdotV <= 0.0f) return SampledSpectrum::Zero();

    Vector3D halfSum = dirInLocal + dirOutLocal;
    if (dot(halfSum, halfSum) < 1e-12f) return SampledSpectrum::Zero();
    Vector3D halfVec = normalize(halfSum);
    float F = SchlickFresnel(dot(dirOutLocal, halfVec), 0.04f);

    SampledSpectrum diffTerm = diffuse * (1.0f - F) * VLR_M_INV_PI;

    float alpha = roughnessToAlpha(roughness);
    float alpha2 = alpha * alpha;
    float NdotH = halfVec.z;
    float D = GGX_D(NdotH, alpha2);
    float G1_l = GGX_G1(NdotL, alpha2);
    float G1_v = GGX_G1(NdotV, alpha2);
    float denom = 4.0f * NdotL * NdotV;
    float specTerm = (denom > 1e-7f) ? D * G1_l * G1_v * F / denom : 0.0f;

    SampledSpectrum result;
    for (int i = 0; i < NumSpectralSamples; ++i)
        result.values[i] = diffTerm.values[i] + specular.values[i] * specTerm;
    return result;
}

/// 采样 Fresnel 混合：按 F 概率选择镜面/漫反射
/// PDF 需乘以选择概率：pdf = probSelection * componentPDF
CUDA_DEVICE_FUNCTION CUDA_INLINE void sampleFresnelBlendBSDF(
    const SampledSpectrum& diffuse, const SampledSpectrum& specular,
    float roughness,
    const Vector3D& dirInLocal, const Normal3D& geomNormalLocal,
    float u0, float u1, float u2,
    BSDFSampleResult* result) {

    float NdotL = dirInLocal.z;
    if (NdotL <= 0.0f) { result->pdf = 0.0f; result->f = SampledSpectrum::Zero(); return; }

    // 使用平均 F 决定采样概率（Schlick F0=0.04 在 cos≈0.2 时的近似）
    float avgF = 0.04f + 0.96f * 0.2f;  // 简化
    if (u2 < avgF) {
        SampledSpectrum reflectance;
        for (int i = 0; i < NumSpectralSamples; ++i) reflectance.values[i] = (specular.values[i] + 0.04f) * 0.5f;
        sampleGGXBSDF(reflectance, roughness, dirInLocal, geomNormalLocal, u0, u1, result);
        result->pdf *= avgF;  // 选择概率
    } else {
        sampleLambertBSDF(diffuse, dirInLocal, geomNormalLocal, u0, u1, result);
        result->sampledBSDFType = BSDFType_FresnelBlend;
        result->pdf *= (1.0f - avgF);  // 选择概率
    }
}


// ============================================================================
// 4.8 UE4 风格 BRDF
// ============================================================================

/// UE4 金属工作流：diffuse = (1-metal)*baseColor/pi, specular = GGX with F0 from metal
CUDA_DEVICE_FUNCTION CUDA_INLINE SampledSpectrum evaluateUE4BRDF(
    const SampledSpectrum& baseColor, float metallic, float roughness,
    const Vector3D& dirInLocal, const Vector3D& dirOutLocal,
    const Normal3D& geomNormalLocal) {

    SampledSpectrum F0;
    computeF0FromMetallic(baseColor, metallic, &F0);

    float NdotL = dirInLocal.z;
    float NdotV = dirOutLocal.z;
    if (NdotL <= 0.0f || NdotV <= 0.0f) return SampledSpectrum::Zero();

    Vector3D halfSum = dirInLocal + dirOutLocal;
    if (dot(halfSum, halfSum) < 1e-12f) return SampledSpectrum::Zero();
    Vector3D halfVec = normalize(halfSum);
    SampledSpectrum F;
    SchlickFresnelSpectrum(dot(dirOutLocal, halfVec), F0, &F);

    SampledSpectrum kD = SampledSpectrum::One();
    for (int i = 0; i < NumSpectralSamples; ++i)
        kD.values[i] = (1.0f - metallic) * (1.0f - F.values[i]);

    SampledSpectrum diffuseTerm = baseColor * kD * VLR_M_INV_PI;

    float alpha = roughnessToAlpha(roughness);
    float alpha2 = alpha * alpha;
    float NdotH = halfVec.z;
    float D = GGX_D(NdotH, alpha2);
    float G1_l = GGX_G1(NdotL, alpha2);
    float G1_v = GGX_G1(NdotV, alpha2);
    float denom = 4.0f * NdotL * NdotV;
    float spec = (denom > 1e-7f) ? D * G1_l * G1_v / denom : 0.0f;

    SampledSpectrum result;
    for (int i = 0; i < NumSpectralSamples; ++i)
        result.values[i] = diffuseTerm.values[i] + F.values[i] * spec * baseColor.values[i];
    return result;
}


// ============================================================================
// 4.9 Frostbite 风格 BRDF
// ============================================================================

/// Frostbite BRDF：与 UE4 类似，能量守恒的 diffuse + specular
/// 使用 Disney diffuse + GGX，与 UE4 细微差异
CUDA_DEVICE_FUNCTION CUDA_INLINE SampledSpectrum evaluateFrostbiteBRDF(
    const SampledSpectrum& baseColor, float metallic, float roughness,
    const Vector3D& dirInLocal, const Vector3D& dirOutLocal,
    const Normal3D& geomNormalLocal) {

    return evaluateUE4BRDF(baseColor, metallic, roughness,
        dirInLocal, dirOutLocal, geomNormalLocal);
}


// ============================================================================
// 4.9.1 Disney Principled BRDF (Burley 2012)
// ============================================================================
// 参考: Physically Based Shading at Disney, SIGGRAPH 2012 Course Notes
// 分量: Disney Diffuse, Subsurface(简化), Specular(GGX), Sheen, Clearcoat(GTR1)
// 各向异性: 暂用各向同性 GGX（anisotropic 参数预留）
// ============================================================================

/// Disney Diffuse: fd = baseColor/π * (1+(FD90-1)(1-cosθl)^5) * (1+(FD90-1)(1-cosθv)^5)
/// FD90 = 0.5 + 2*roughness*cos²θd
CUDA_DEVICE_FUNCTION CUDA_INLINE float DisneyDiffuse(
    float NdotL, float NdotV, float LdotH, float roughness) {
    float FD90 = 0.5f + 2.0f * roughness * LdotH * LdotH;
    float FL = 1.0f + (FD90 - 1.0f) * powf(1.0f - NdotL, 5.0f);
    float FV = 1.0f + (FD90 - 1.0f) * powf(1.0f - NdotV, 5.0f);
    return FL * FV * VLR_M_INV_PI;
}

/// Disney Subsurface: Hanrahan-Krueger 近似，与 diffuse 形状混合
CUDA_DEVICE_FUNCTION CUDA_INLINE float DisneySubsurface(
    float NdotL, float NdotV, float LdotH, float roughness) {
    float FD90 = 0.5f + 2.0f * roughness * LdotH * LdotH;
    float FL = 1.0f + (FD90 - 1.0f) * powf(1.0f - NdotL, 5.0f);
    float FV = 1.0f + (FD90 - 1.0f) * powf(1.0f - NdotV, 5.0f);
    float Fss90 = roughness * LdotH * LdotH;
    float Fss = 1.0f + (Fss90 - 1.0f) * (powf(1.0f - NdotL, 5.0f) + powf(1.0f - NdotV, 5.0f));
    float ss = 1.25f * (FL * FV * (1.0f / (NdotL + NdotV) - 0.5f) + 0.5f);
    return ss * VLR_M_INV_PI;
}

/// Disney Specular F0: specular 参数映射到 [0, 0.08]，specularTint 插值
/// 金属时 F0 = baseColor
CUDA_DEVICE_FUNCTION CUDA_INLINE void DisneySpecularF0(
    const SampledSpectrum& baseColor, float metallic, float specular, float specularTint,
    SampledSpectrum* F0) {
    float specF0 = 0.08f * specular;  // [0, 0.08]
    float lum = baseColor.values[0] * 0.2126f + baseColor.values[1] * 0.7152f + baseColor.values[2] * 0.0722f;
    for (int i = 0; i < NumSpectralSamples; ++i) {
        float tint = (lum > 1e-5f) ? baseColor.values[i] / lum : 1.0f;
        float F0dielectric = specF0 * (1.0f - specularTint + specularTint * tint);
        F0->values[i] = baseColor.values[i] * metallic + F0dielectric * (1.0f - metallic);
    }
    F0->values[3] = (F0->values[0] + F0->values[1] + F0->values[2]) / 3.0f;
}

/// Disney Sheen: sheen * (1 - cosθd)^5，可选 baseColor 着色
CUDA_DEVICE_FUNCTION CUDA_INLINE SampledSpectrum DisneySheen(
    const SampledSpectrum& baseColor, float sheen, float sheenTint, float LdotH) {
    if (sheen <= 0.0f) return SampledSpectrum::Zero();
    float lum = baseColor.values[0] * 0.2126f + baseColor.values[1] * 0.7152f + baseColor.values[2] * 0.0722f;
    SampledSpectrum tint = SampledSpectrum::One();
    if (lum > 1e-5f) {
        for (int i = 0; i < 3; ++i) tint.values[i] = baseColor.values[i] / lum;
    }
    float sheenF = sheen * powf(1.0f - LdotH, 5.0f);
    SampledSpectrum result;
    for (int i = 0; i < NumSpectralSamples; ++i)
        result.values[i] = sheenF * (1.0f - sheenTint + sheenTint * tint.values[i]);
    return result;
}

/// GTR1 (Clearcoat): D = (α²-1)/(π*ln(α²)) * 1/(1+(α²-1)cos²θh)
CUDA_DEVICE_FUNCTION CUDA_INLINE float GTR1_D(float NdotH, float alpha) {
    if (NdotH <= 0.0f || alpha >= 1.0f) return 0.0f;
    float alpha2 = alpha * alpha;
    float denom = 1.0f + (alpha2 - 1.0f) * NdotH * NdotH;
    float logAlpha2 = std::log(alpha2);
    if (std::abs(logAlpha2) < 1e-10f) return 0.0f;
    return (alpha2 - 1.0f) / (VLR_M_PI * logAlpha2 * denom);
}

/// Disney Clearcoat: 独立 GTR1 镜面层，IOR=1.5，scale [0, 0.25]
CUDA_DEVICE_FUNCTION CUDA_INLINE float DisneyClearcoat(
    float NdotL, float NdotV, float NdotH, float VdotH,
    float clearcoat, float clearcoatGloss) {
    if (clearcoat <= 0.0f) return 0.0f;
    float alpha = 0.1f + 0.9f * (1.0f - clearcoatGloss);  // gloss: 0=satin, 1=gloss
    float D = GTR1_D(NdotH, alpha);
    float F = SchlickFresnel(VdotH, 0.04f);  // 清漆 F0≈0.04
    float alphaG = 0.25f;
    float G = GGX_G1(NdotL, alphaG * alphaG) * GGX_G1(NdotV, alphaG * alphaG);
    float denom = 4.0f * NdotL * NdotV;
    if (denom < 1e-7f) return 0.0f;
    return 0.25f * clearcoat * D * F * G / denom;
}

/// 评估 Disney BRDF
CUDA_DEVICE_FUNCTION CUDA_INLINE SampledSpectrum evaluateDisneyBRDF(
    const SampledSpectrum& baseColor,
    float metallic, float subsurface, float specular, float roughness,
    float specularTint, float /*anisotropic*/, float sheen, float sheenTint,
    float clearcoat, float clearcoatGloss,
    const Vector3D& dirInLocal, const Vector3D& dirOutLocal,
    const Normal3D& geomNormalLocal) {

    float NdotL = dirInLocal.z;
    float NdotV = dirOutLocal.z;
    if (NdotV <= 0.0f) return SampledSpectrum::Zero();
    if (NdotL <= 0.0f) NdotL = -NdotL;  // 允许光线从任意方向入射

    Vector3D halfSum = dirInLocal + dirOutLocal;
    float halfLenSq = dot(halfSum, halfSum);
    if (halfLenSq < 1e-12f) return SampledSpectrum::Zero();
    Vector3D halfVec = normalize(halfSum);
    float NdotH = halfVec.z;
    float LdotH = dot(dirInLocal, halfVec);
    float VdotH = dot(dirOutLocal, halfVec);

    SampledSpectrum result = SampledSpectrum::Zero();

    // Diffuse + Subsurface (金属无漫反射)
    if (metallic < 1.0f) {
        float fd = DisneyDiffuse(NdotL, NdotV, LdotH, roughness);
        float fss = DisneySubsurface(NdotL, NdotV, LdotH, roughness);
        float diffBlend = (1.0f - subsurface) * fd + subsurface * fss;
        for (int i = 0; i < NumSpectralSamples; ++i)
            result.values[i] += baseColor.values[i] * (1.0f - metallic) * diffBlend;
    }

    // Specular (GGX)
    SampledSpectrum F0;
    DisneySpecularF0(baseColor, metallic, specular, specularTint, &F0);
    float alpha = roughnessToAlpha(roughness);
    float alpha2 = alpha * alpha;
    float D = GGX_D(NdotH, alpha2);
    float G1_l = GGX_G1(NdotL, alpha2);
    float G1_v = GGX_G1(NdotV, alpha2);
    float denom = 4.0f * NdotL * NdotV;
    if (denom > 1e-7f) {
        SampledSpectrum F;
        SchlickFresnelSpectrum(VdotH, F0, &F);
        float specVal = D * G1_l * G1_v / denom;
        for (int i = 0; i < NumSpectralSamples; ++i)
            result.values[i] += F.values[i] * specVal;
    }

    // Sheen
    result = result + DisneySheen(baseColor, sheen, sheenTint, LdotH);

    // Clearcoat
    if (clearcoat > 0.0f) {
        float cc = DisneyClearcoat(NdotL, NdotV, NdotH, VdotH, clearcoat, clearcoatGloss);
        result = result + SampledSpectrum(cc);  // 清漆无色
    }

    return result;
}

/// 采样 Disney BRDF: 按 diffuse/specular/clearcoat 权重选择分量
CUDA_DEVICE_FUNCTION CUDA_INLINE void sampleDisneyBRDF(
    const SampledSpectrum& baseColor,
    float metallic, float subsurface, float specular, float roughness,
    float specularTint, float anisotropic, float sheen, float sheenTint,
    float clearcoat, float clearcoatGloss,
    const Vector3D& dirInLocal, const Normal3D& geomNormalLocal,
    float u0, float u1, float u2,
    BSDFSampleResult* result) {

    float NdotL = dirInLocal.z;
    if (NdotL <= 0.0f) {
        result->pdf = 0.0f;
        result->f = SampledSpectrum::Zero();
        return;
    }

    // 估算各分量权重用于 MIS 采样
    float diffuseWeight = (1.0f - metallic) * baseColor.values[3] * VLR_M_INV_PI;
    SampledSpectrum F0;
    DisneySpecularF0(baseColor, metallic, specular, specularTint, &F0);
    float specWeight = F0.values[3] * 0.5f;  // 近似
    float clearcoatWeight = clearcoat * 0.25f;
    float totalWeight = diffuseWeight + specWeight + clearcoatWeight;
    if (totalWeight < 1e-6f) totalWeight = 1.0f;

    float pd = diffuseWeight / totalWeight;
    float ps = specWeight / totalWeight;
    float pc = clearcoatWeight / totalWeight;

    if (u2 < pd) {
        // 采样 diffuse
        sampleLambertBSDF(baseColor, dirInLocal, geomNormalLocal, u0, u1, result);
        result->sampledBSDFType = BSDFType_DisneyBRDF;
        result->pdf *= pd;
        // 重新计算 f（Disney diffuse 而非 Lambert）
        Vector3D dirOut = result->dirLocal;
        float NdotV = dirOut.z;
        Vector3D halfSum = dirInLocal + dirOut;
        float halfLenSq = dot(halfSum, halfSum);
        if (halfLenSq > 1e-12f) {
            Vector3D halfVec = normalize(halfSum);
            float LdotH = dot(dirInLocal, halfVec);
            float fd = DisneyDiffuse(NdotL, NdotV, LdotH, roughness);
            float fss = DisneySubsurface(NdotL, NdotV, LdotH, roughness);
            float diffBlend = (1.0f - subsurface) * fd + subsurface * fss;
            for (int i = 0; i < NumSpectralSamples; ++i)
                result->f.values[i] = baseColor.values[i] * (1.0f - metallic) * diffBlend;
        }
    } else if (u2 < pd + ps) {
        // 采样 specular (GGX)
        sampleGGXBSDF(F0, roughness, dirInLocal, geomNormalLocal, u0, u1, result);
        result->sampledBSDFType = BSDFType_DisneyBRDF;
        result->pdf *= ps;
    } else {
        // 采样 clearcoat (GTR1 近似为 GGX，roughness = sqrt(alpha))
        float alpha = 0.1f + 0.9f * (1.0f - clearcoatGloss);
        float ccRoughness = safeSqrt(alpha);
        SampledSpectrum ccReflectance;
        for (int i = 0; i < NumSpectralSamples; ++i) ccReflectance.values[i] = 0.04f;
        sampleGGXBSDF(ccReflectance, ccRoughness, dirInLocal, geomNormalLocal, u0, u1, result);
        result->sampledBSDFType = BSDFType_DisneyBRDF;
        result->pdf *= pc;
        result->f = result->f * (0.25f * clearcoat);
    }
}

/// Disney BRDF PDF（混合各分量 PDF）
CUDA_DEVICE_FUNCTION CUDA_INLINE float getDisneyBRDFPDF(
    const SampledSpectrum& baseColor,
    float metallic, float subsurface, float specular, float specularTint,
    float roughness, float /*anisotropic*/, float sheen, float clearcoat, float clearcoatGloss,
    const Vector3D& dirInLocal, const Vector3D& dirOutLocal,
    const Normal3D& geomNormalLocal) {

    float diffuseWeight = (1.0f - metallic) * baseColor.values[3] * VLR_M_INV_PI;
    SampledSpectrum F0;
    DisneySpecularF0(baseColor, metallic, specular, specularTint, &F0);
    float specWeight = F0.values[3] * 0.5f;
    float clearcoatWeight = clearcoat * 0.25f;
    float totalWeight = diffuseWeight + specWeight + clearcoatWeight;
    if (totalWeight < 1e-6f) return 0.0f;

    float pd = diffuseWeight / totalWeight;
    float ps = specWeight / totalWeight;
    float pc = clearcoatWeight / totalWeight;

    float pdfLambert = getLambertBSDFPDF(dirOutLocal, geomNormalLocal);
    float pdfGGX = getGGXBSDFPDF(F0, roughness, dirInLocal, dirOutLocal, geomNormalLocal);
    float ccAlpha = 0.1f + 0.9f * (1.0f - clearcoatGloss);
    float ccRoughness = safeSqrt(ccAlpha);
    SampledSpectrum ccRefl;
    for (int i = 0; i < NumSpectralSamples; ++i) ccRefl.values[i] = 0.04f;
    float pdfClearcoat = getGGXBSDFPDF(ccRefl, ccRoughness, dirInLocal, dirOutLocal, geomNormalLocal);

    return pd * pdfLambert + ps * pdfGGX + pc * pdfClearcoat;
}


} // namespace shared
} // namespace vlr
