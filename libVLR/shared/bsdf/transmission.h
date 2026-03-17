#pragma once

#include "fresnel.h"
#include "microfacet.h"

namespace vlr {
namespace shared {

// ============================================================================
// 4.6 粗糙透射 BSDF（GGX 微表面透射）
// ============================================================================

/// 评估 GGX 透射 BSDF（Walter et al. 2007, PBRT-v4）
CUDA_DEVICE_FUNCTION CUDA_INLINE SampledSpectrum evaluateGGXTransmissionBSDF(
    const SampledSpectrum& transmittance, float roughness, float etaI, float etaT,
    const Vector3D& dirInLocal, const Vector3D& dirOutLocal,
    const Normal3D& geomNormalLocal) {

    float NdotL = dirInLocal.z;
    float NdotV = dirOutLocal.z;
    if (NdotL <= 0.0f || NdotV >= 0.0f) return SampledSpectrum::Zero();  // 透射：入射上侧，出射下侧

    // 确定半向量：透射时 H 在入射和折射方向之间
    float eta = etaI / etaT;
    Vector3D halfSum = -dirInLocal + eta * dirOutLocal;
    if (dot(halfSum, halfSum) < 1e-12f) return SampledSpectrum::Zero();
    Vector3D halfVec = normalize(halfSum);
    float NdotH = halfVec.z;
    if (NdotH <= 0.0f) halfVec = -halfVec;

    float alpha = roughnessToAlpha(roughness);
    float alpha2 = alpha * alpha;

    float D = GGX_D(std::abs(halfVec.z), alpha2);
    float G1_l = GGX_G1(std::abs(NdotL), alpha2);
    float G1_v = GGX_G1(std::abs(NdotV), alpha2);
    float G = G1_l * G1_v;

    float denom = dot(-dirInLocal, halfVec) + eta * dot(dirOutLocal, halfVec);
    denom = denom * denom;
    if (denom < 1e-10f) return SampledSpectrum::Zero();

    float F = FresnelDielectric(dot(-dirInLocal, halfVec), etaI, etaT);
    float dwh_dwo = eta * eta * std::abs(dot(dirOutLocal, halfVec)) / denom;

    SampledSpectrum result;
    float spec = (1.0f - F) * D * G * std::abs(dot(-dirInLocal, halfVec)) * dwh_dwo
        / (std::abs(NdotL) * std::abs(NdotV));
    for (int i = 0; i < NumSpectralSamples; ++i)
        result.values[i] = transmittance.values[i] * spec;

    return result;
}

/// 采样 GGX 透射（以 Fresnel 概率选择反射/透射）
CUDA_DEVICE_FUNCTION CUDA_INLINE void sampleGGXTransmissionBSDF(
    const SampledSpectrum& transmittance, float roughness, float etaI, float etaT,
    const Vector3D& dirInLocal, const Normal3D& geomNormalLocal,
    float u0, float u1, float u2,
    BSDFSampleResult* result) {

    float alpha = roughnessToAlpha(roughness);
    float eta = etaI / etaT;
    Vector3D V = -dirInLocal;  // 观察方向
    Vector3D H = sampleGGXVNDF(V, alpha, u0, u1);

    float F = FresnelDielectric(std::abs(dot(V, H)), etaI, etaT);

    if (u2 < F) {
        // 反射分量：与原始 VLR MicrofacetBSDF 一致 f = coeff * F * D * G / (4 * NdotV * NdotL)
        float VdotH = dot(V, H);
        result->dirLocal = 2.0f * VdotH * H - V;
        float NdotV = std::abs(result->dirLocal.z);
        float NdotL = std::abs(dirInLocal.z);
        float alpha2 = alpha * alpha;
        float NdotH = std::abs(H.z);
        float D = GGX_D(NdotH, alpha2);
        float G1_v = GGX_G1(NdotV, alpha2);
        float G1_l = GGX_G1(NdotL, alpha2);
        float G = G1_l * G1_v;
        float denom = 4.0f * NdotL * NdotV;
        float spec = (denom > 1e-7f) ? (D * G / denom) : 0.0f;
        for (int i = 0; i < NumSpectralSamples; ++i)
            result->f.values[i] = transmittance.values[i] * F * spec;
        float VdotHSafe = ::vlr::vlr_max(std::abs(VdotH), 1e-6f);
        result->pdf = F * D * G1_v * NdotL / (4.0f * VdotHSafe);
        result->sampledBSDFType = BSDFType_GGX;
        result->isDelta = false;
    } else {
        // 透射分量：H 为微表面法线，折射在 H 定义的平面上
        Normal3D H_normal = H.z > 0.0f ? H : Vector3D(-H.x, -H.y, -H.z);
        Vector3D wt;
        if (!refract(dirInLocal, H_normal, eta, &wt)) {
            result->pdf = 0.0f;
            result->f = SampledSpectrum::Zero();
            return;
        }
        result->dirLocal = wt;
        result->sampledBSDFType = BSDFType_GGXTransmission;
        float NdotV = std::abs(wt.z);
        float NdotL = std::abs(dirInLocal.z);
        float alpha2 = alpha * alpha;
        float D = GGX_D(std::abs(H.z), alpha2);
        float G1_l = GGX_G1(NdotL, alpha2);
        float denom = dot(dirInLocal, H) + eta * dot(wt, H);
        denom = denom * denom;
        result->pdf = (1.0f - F) * D * G1_l * eta * eta * std::abs(dot(wt, H)) / (denom * NdotL + 1e-7f);
        float G1_v = GGX_G1(NdotV, alpha2);
        float G = G1_l * G1_v;
        result->f = transmittance * (1.0f - F) * D * G * std::abs(dot(dirInLocal, H))
            * eta * eta * std::abs(dot(wt, H)) / (NdotL * NdotV * denom + 1e-7f);
        result->isDelta = false;
    }
}

} // namespace shared
} // namespace vlr
