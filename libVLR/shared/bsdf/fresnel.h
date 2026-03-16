#pragma once

#include "bsdf_types.h"

namespace vlr {
namespace shared {

// ============================================================================
// 2.5 完整 Fresnel 计算
// ============================================================================

/// Schlick Fresnel 近似（快速近似）
/// cosTheta: 入射角余弦 (H·V 或 N·V), F0: 法向入射反射率
/// 用于电介质: F0 = ((n1-n2)/(n1+n2))^2
CUDA_DEVICE_FUNCTION CUDA_INLINE float SchlickFresnel(
    float cosTheta, float F0) {
    float t = 1.0f - cosTheta;
    float t5 = t * t * t * t * t;
    return F0 + (1.0f - F0) * t5;
}

/// 电介质 Schlick Fresnel（从折射率计算 F0）
/// etaI: 入射介质折射率, etaT: 透射介质折射率
CUDA_DEVICE_FUNCTION CUDA_INLINE float SchlickFresnelDielectric(
    float cosTheta, float etaI, float etaT) {
    float F0 = (etaI - etaT) / (etaI + etaT);
    F0 = F0 * F0;
    return SchlickFresnel(cosTheta, F0);
}

/// 完整电介质 Fresnel 方程（精确）
/// 菲涅尔反射率 F = (r_par^2 + r_perp^2) / 2
/// etaI, etaT: 入射/透射折射率, cosThetaI: 入射角余弦
CUDA_DEVICE_FUNCTION CUDA_INLINE float FresnelDielectric(
    float cosThetaI, float etaI, float etaT) {
    if (cosThetaI < 0.0f) return 1.0f;  // 全内反射情况由调用方处理
    float sinThetaI = safeSqrt(1.0f - cosThetaI * cosThetaI);
    float sinThetaT = etaI / etaT * sinThetaI;
    if (sinThetaT >= 1.0f) return 1.0f;  // 全内反射

    float cosThetaT = safeSqrt(1.0f - sinThetaT * sinThetaT);
    float denomPar = etaT * cosThetaI + etaI * cosThetaT;
    float denomPerp = etaI * cosThetaI + etaT * cosThetaT;
    denomPar = (std::abs(denomPar) > 1e-8f) ? denomPar : 1e-8f;
    denomPerp = (std::abs(denomPerp) > 1e-8f) ? denomPerp : 1e-8f;
    float rPar = (etaT * cosThetaI - etaI * cosThetaT) / denomPar;
    float rPerp = (etaI * cosThetaI - etaT * cosThetaT) / denomPerp;
    return 0.5f * (rPar * rPar + rPerp * rPerp);
}

/// 完整导体 Fresnel 方程（复折射率 n - ik）
/// eta, kappa: 导体复折射率的实部和虚部
/// 参考 PBRT-v4 / pbr-book 导体 BRDF
/// 避免分母为零导致 NaN
CUDA_DEVICE_FUNCTION CUDA_INLINE float FresnelConductor(
    float cosThetaI, float eta, float kappa) {
    float cos2 = cosThetaI * cosThetaI;
    float sin2 = 1.0f - cos2;
    float eta2 = eta * eta;
    float kappa2 = kappa * kappa;

    float t0 = eta2 - kappa2 - sin2;
    float t1 = eta2 - kappa2 + sin2;
    float t2 = 4.0f * eta2 * kappa2;

    float denomPar = t1 * t1 + t2;
    denomPar = (denomPar > 1e-10f) ? denomPar : 1e-10f;
    float rPar2 = (t0 * t0 + t2) / denomPar;
    float t3 = (eta2 + kappa2) * cos2;
    float t4 = 2.0f * eta * cosThetaI;
    float denomPerp = t3 + t4 + sin2;
    denomPerp = (std::abs(denomPerp) > 1e-10f) ? denomPerp : 1e-10f;
    float rPerp2 = (t3 - t4 + sin2) / denomPerp;

    return 0.5f * (rPar2 + rPerp2);
}

/// 光谱型 Schlick Fresnel（每通道 F0）
CUDA_DEVICE_FUNCTION CUDA_INLINE void SchlickFresnelSpectrum(
    float cosTheta, const SampledSpectrum& F0, SampledSpectrum* F) {
    float t = 1.0f - cosTheta;
    float t5 = t * t * t * t * t;
    for (int i = 0; i < NumSpectralSamples; ++i)
        F->values[i] = F0.values[i] + (1.0f - F0.values[i]) * t5;
}

/// 从金属度插值 F0：绝缘体 0.04，金属用 baseColor
/// UE4 金属工作流
CUDA_DEVICE_FUNCTION CUDA_INLINE void computeF0FromMetallic(
    const SampledSpectrum& baseColor, float metallic, SampledSpectrum* F0) {
    constexpr float dielectricF0 = 0.04f;
    for (int i = 0; i < NumSpectralSamples; ++i)
        F0->values[i] = baseColor.values[i] * metallic + dielectricF0 * (1.0f - metallic);
}

/// 波长相关折射率：简化 Cauchy 近似 n(λ) ≈ n_base + 色散项
/// lambda: 波长(nm), nBase: 基准折射率, dispersionStrength: 色散强度 (0~0.1 典型)
/// 玻璃：n 在蓝光更高，红光较低
CUDA_DEVICE_FUNCTION CUDA_INLINE float iorAtWavelength(
    float lambda, float nBase, float dispersionStrength) {
    if (dispersionStrength <= 0.0f) return nBase;
    constexpr float lambdaRef = 550.0f;
    float delta = (lambdaRef - lambda) / 100.0f;  // 每100nm变化
    return nBase + dispersionStrength * 0.02f * delta;
}

} // namespace shared
} // namespace vlr
