// ============================================================================
// VLR 材质系统 - BSDF 公共函数
//
// 本文件实现 BSDF 的评估、采样和 PDF 计算，为 ProcessHits 和 SampleBSDF
// kernel 提供核心材质计算能力。支持所有高级 BSDF 类型：
// Lambert, GGX, Specular, SpecularTransmission（色散）, GGXTransmission,
// FresnelBlend, UE4BRDF, FrostbiteBRDF, MixedBSDF。
// Fresnel：Schlick 近似、完整电介质/导体 Fresnel 方程。
//
// 作者：VLR 开发团队
// 创建日期：2026-03-07
// 环境：CUDA 13.1, OptiX 8.0.0, VS2022
// ============================================================================

#pragma once

#include "material_types.h"
#include "kernel_common.h"
#include <cmath>

namespace vlr {
namespace shared {

// 使用基础类型的数学常量
using ::vlr::VLR_M_PI;
using ::vlr::VLR_M_INV_PI;
using ::vlr::VLR_M_2PI;
using ::vlr::VLR_M_INV_2PI;

// ============================================================================
// 1. 工具函数
// ============================================================================

/// 安全开平方（避免数值不稳定）
CUDA_DEVICE_FUNCTION CUDA_INLINE float safeSqrt(float x) {
    return std::sqrt(::vlr::vlr_max(x, 0.0f));
}

/// 将粗糙度转换为 GGX alpha 参数（Beckmann-style: alpha = roughness^2）
CUDA_DEVICE_FUNCTION CUDA_INLINE float roughnessToAlpha(float roughness) {
    float r = ::vlr::vlr_max(roughness, 0.001f);
    return r * r;
}


// ============================================================================
// 2. Lambert 漫反射 BSDF
// ============================================================================

/// 评估 Lambert BSDF: f = albedo / pi
/// dirInLocal: 入射方向（朝向表面）, dirOutLocal: 出射方向（离开表面）
CUDA_DEVICE_FUNCTION CUDA_INLINE SampledSpectrum evaluateLambertBSDF(
    const SampledSpectrum& albedo,
    const Vector3D& dirInLocal,
    const Vector3D& dirOutLocal,
    const Normal3D& geomNormalLocal) {

    float cosOut = dot(dirOutLocal, geomNormalLocal);
    if (cosOut <= 0.0f)
        return SampledSpectrum::Zero();

    return albedo * VLR_M_INV_PI;
}

/// 采样 Lambert BSDF（余弦加权半球）
/// 返回采样的出射方向和 PDF
CUDA_DEVICE_FUNCTION CUDA_INLINE void sampleLambertBSDF(
    const SampledSpectrum& albedo,
    const Vector3D& dirInLocal,
    const Normal3D& geomNormalLocal,
    float u0, float u1,
    BSDFSampleResult* result) {

    // 余弦加权半球采样
    float r = safeSqrt(u0);
    float phi = u1 * VLR_M_2PI;
    float x = r * std::cos(phi);
    float y = r * std::sin(phi);
    float z = safeSqrt(1.0f - u0);

    // 构建局部坐标系（z 轴为法线）
    Vector3D tangent = (std::abs(geomNormalLocal.z) < 0.999f)
        ? normalize(cross(Vector3D(0, 1, 0), geomNormalLocal))
        : normalize(cross(Vector3D(1, 0, 0), geomNormalLocal));
    Vector3D bitangent = cross(geomNormalLocal, tangent);

    Vector3D dirLocal = normalize(
        tangent * x + bitangent * y + geomNormalLocal * z);

    result->dirLocal = dirLocal;
    result->f = albedo * VLR_M_INV_PI;
    result->pdf = dot(dirLocal, geomNormalLocal) * VLR_M_INV_PI;
    result->sampledBSDFType = BSDFType_Lambert;
    result->isDelta = false;
}

/// Lambert BSDF 的 PDF: cos(theta) / pi
CUDA_DEVICE_FUNCTION CUDA_INLINE float getLambertBSDFPDF(
    const Vector3D& dirOutLocal,
    const Normal3D& geomNormalLocal) {

    float cosOut = dot(dirOutLocal, geomNormalLocal);
    if (cosOut <= 0.0f)
        return 0.0f;
    return cosOut * VLR_M_INV_PI;
}


// ============================================================================
// 3. GGX 微表面 BSDF
// ============================================================================

/// GGX 法线分布函数 D(h)
CUDA_DEVICE_FUNCTION CUDA_INLINE float GGX_D(
    float NdotH, float alpha2) {

    if (NdotH <= 0.0f)
        return 0.0f;

    float denom = NdotH * NdotH * (alpha2 - 1.0f) + 1.0f;
    denom = denom * denom * VLR_M_PI;
    return alpha2 / denom;
}

/// GGX  Smith 几何项 G1
CUDA_DEVICE_FUNCTION CUDA_INLINE float GGX_G1(float NdotV, float alpha2) {
    if (NdotV <= 0.0f)
        return 0.0f;
    float tan2 = (1.0f - NdotV * NdotV) / (NdotV * NdotV);
    return 2.0f / (1.0f + safeSqrt(1.0f + alpha2 * tan2));
}

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
    float rPar = (etaT * cosThetaI - etaI * cosThetaT) / (etaT * cosThetaI + etaI * cosThetaT);
    float rPerp = (etaI * cosThetaI - etaT * cosThetaT) / (etaI * cosThetaI + etaT * cosThetaT);
    return 0.5f * (rPar * rPar + rPerp * rPerp);
}

/// 完整导体 Fresnel 方程（复折射率 n - ik）
/// eta, kappa: 导体复折射率的实部和虚部
/// 参考 PBRT-v4 / pbr-book 导体 BRDF
CUDA_DEVICE_FUNCTION CUDA_INLINE float FresnelConductor(
    float cosThetaI, float eta, float kappa) {
    float cos2 = cosThetaI * cosThetaI;
    float sin2 = 1.0f - cos2;
    float eta2 = eta * eta;
    float kappa2 = kappa * kappa;

    float t0 = eta2 - kappa2 - sin2;
    float t1 = eta2 - kappa2 + sin2;
    float t2 = 4.0f * eta2 * kappa2;

    float rPar2 = (t0 * t0 + t2) / (t1 * t1 + t2);
    float t3 = (eta2 + kappa2) * cos2;
    float t4 = 2.0f * eta * cosThetaI;
    float rPerp2 = (t3 - t4 + sin2) / (t3 + t4 + sin2);

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

/// 评估 GGX 镜面 BSDF
CUDA_DEVICE_FUNCTION CUDA_INLINE SampledSpectrum evaluateGGXBSDF(
    const SampledSpectrum& reflectance,
    float roughness,
    const Vector3D& dirInLocal,
    const Vector3D& dirOutLocal,
    const Normal3D& geomNormalLocal) {

    float NdotL = dot(dirInLocal, geomNormalLocal);
    float NdotV = dot(dirOutLocal, geomNormalLocal);

    if (NdotL <= 0.0f || NdotV <= 0.0f)
        return SampledSpectrum::Zero();

    Vector3D halfVec = normalize(dirInLocal + dirOutLocal);
    float NdotH = dot(halfVec, geomNormalLocal);

    if (NdotH <= 0.0f)
        return SampledSpectrum::Zero();

    float alpha = roughnessToAlpha(roughness);
    float alpha2 = alpha * alpha;

    float D = GGX_D(NdotH, alpha2);
    float G1_l = GGX_G1(NdotL, alpha2);
    float G1_v = GGX_G1(NdotV, alpha2);
    float G = G1_l * G1_v;

    float VdotH = dot(dirOutLocal, halfVec);
    float F = SchlickFresnel(VdotH, 0.04f);

    float denom = 4.0f * NdotL * NdotV;
    if (denom < 1e-7f)
        return SampledSpectrum::Zero();

    float spec = D * G * F / denom;

    SampledSpectrum result;
    for (int i = 0; i < NumSpectralSamples; ++i)
        result.values[i] = reflectance.values[i] * spec;

    return result;
}

/// 采样 GGX 可见法线（VNDF 采样，基于 Heitz 2017）
CUDA_DEVICE_FUNCTION CUDA_INLINE Vector3D sampleGGXVNDF(
    const Vector3D& V,
    float alpha,
    float u1, float u2) {

    // 将 V 变换到 GGX 的椭圆空间（alpha 缩放）
    Vector3D Vh = normalize(Vector3D(alpha * V.x, alpha * V.y, ::vlr::vlr_max(V.z, 1e-6f)));

    // 构建 Vh 的正交基
    float lensq = Vh.y * Vh.y + Vh.z * Vh.z;
    Vector3D T1 = (lensq > 1e-10f)
        ? normalize(Vector3D(0, -Vh.z, Vh.y))
        : Vector3D(0, 0, 1);
    Vector3D T2 = cross(Vh, T1);

    // 在球面上采样
    float r = safeSqrt(u1);
    float phi = u2 * VLR_M_2PI;
    float t1 = r * std::cos(phi);
    float t2 = r * std::sin(phi);
    float s = 0.5f * (1.0f + Vh.z);
    t2 = (1.0f - s) * safeSqrt(::vlr::vlr_max(0.0f, 1.0f - t1 * t1)) + s * t2;

    Vector3D Nh = t1 * T1 + t2 * T2 + safeSqrt(::vlr::vlr_max(0.0f, 1.0f - t1 * t1 - t2 * t2)) * Vh;

    // 变换回切线空间
    Vector3D H = normalize(Vector3D(alpha * Nh.x, alpha * Nh.y, ::vlr::vlr_max(0.0f, Nh.z)));
    return H;
}

/// 采样 GGX BSDF
CUDA_DEVICE_FUNCTION CUDA_INLINE void sampleGGXBSDF(
    const SampledSpectrum& reflectance,
    float roughness,
    const Vector3D& dirInLocal,
    const Normal3D& geomNormalLocal,
    float u0, float u1,
    BSDFSampleResult* result) {

    float alpha = roughnessToAlpha(roughness);

    // 可见法线采样
    Vector3D H = sampleGGXVNDF(dirInLocal, alpha, u0, u1);

    // 反射出射方向
    float VdotH = dot(dirInLocal, H);
    Vector3D dirOutLocal = 2.0f * VdotH * H - dirInLocal;

    float NdotL = dot(dirInLocal, geomNormalLocal);
    float NdotV = dot(dirOutLocal, geomNormalLocal);

    if (NdotV <= 0.0f) {
        result->pdf = 0.0f;
        result->f = SampledSpectrum::Zero();
        return;
    }

    float NdotH = dot(H, geomNormalLocal);

    float alpha2 = alpha * alpha;
    float D = GGX_D(NdotH, alpha2);
    float G1_v = GGX_G1(NdotV, alpha2);

    float VdotH_clamped = ::vlr::vlr_max(VdotH, 1e-6f);
    float pdf = D * G1_v * std::abs(NdotL) / (4.0f * VdotH_clamped);

    float G1_l = GGX_G1(NdotL, alpha2);
    float G = G1_l * G1_v;
    float F = SchlickFresnel(VdotH, 0.04f);

    float denom = 4.0f * std::abs(NdotL) * NdotV;
    float spec = (denom > 1e-7f) ? (D * G * F / denom) : 0.0f;

    result->dirLocal = dirOutLocal;
    result->pdf = pdf;
    result->isDelta = false;
    result->sampledBSDFType = BSDFType_GGX;

    result->f = SampledSpectrum::Zero();
    for (int i = 0; i < NumSpectralSamples; ++i)
        result->f.values[i] = reflectance.values[i] * spec;
}

/// GGX BSDF 的 PDF
CUDA_DEVICE_FUNCTION CUDA_INLINE float getGGXBSDFPDF(
    const SampledSpectrum& reflectance,
    float roughness,
    const Vector3D& dirInLocal,
    const Vector3D& dirOutLocal,
    const Normal3D& geomNormalLocal) {

    float NdotL = dot(dirInLocal, geomNormalLocal);
    float NdotV = dot(dirOutLocal, geomNormalLocal);

    if (NdotL <= 0.0f || NdotV <= 0.0f)
        return 0.0f;

    Vector3D halfVec = normalize(dirInLocal + dirOutLocal);
    float NdotH = dot(halfVec, geomNormalLocal);
    float VdotH = dot(dirOutLocal, halfVec);

    if (NdotH <= 0.0f || VdotH <= 0.0f)
        return 0.0f;

    float alpha = roughnessToAlpha(roughness);
    float alpha2 = alpha * alpha;

    float D = GGX_D(NdotH, alpha2);
    float G1_v = GGX_G1(NdotV, alpha2);

    return D * G1_v * std::abs(NdotL) / (4.0f * VdotH);
}


// ============================================================================
// 4. 完美镜面 BSDF
// ============================================================================

/// 评估完美镜面 BSDF（Delta 分布，评估时返回 0，需特殊处理）
CUDA_DEVICE_FUNCTION CUDA_INLINE SampledSpectrum evaluateSpecularBSDF(
    const SampledSpectrum& reflectance,
    const Vector3D& dirInLocal,
    const Vector3D& dirOutLocal,
    const Normal3D& geomNormalLocal) {

    // 镜面反射：仅当 dirOut 恰好是反射方向时有值
    Vector3D reflected = 2.0f * dot(dirInLocal, geomNormalLocal) * geomNormalLocal - dirInLocal;
    float cosOut = dot(dirOutLocal, geomNormalLocal);

    if (cosOut <= 0.0f)
        return SampledSpectrum::Zero();

    float diff = std::abs(dot(reflected, dirOutLocal) - 1.0f);
    if (diff < 1e-5f)
        return reflectance;  // 精确命中反射方向（数值上）

    return SampledSpectrum::Zero();
}

/// 采样完美镜面：反射方向，PDF 为 1
CUDA_DEVICE_FUNCTION CUDA_INLINE void sampleSpecularBSDF(
    const SampledSpectrum& reflectance,
    const Vector3D& dirInLocal,
    const Normal3D& geomNormalLocal,
    float /*u0*/, float /*u1*/,
    BSDFSampleResult* result) {

    float NdotL = dot(dirInLocal, geomNormalLocal);

    if (NdotL <= 0.0f) {
        result->pdf = 0.0f;
        result->f = SampledSpectrum::Zero();
        return;
    }

    Vector3D reflected = 2.0f * NdotL * geomNormalLocal - dirInLocal;
    result->dirLocal = reflected;
    result->f = reflectance;
    result->pdf = 1.0f;  // Delta 分布的 PDF 视为 1（在 MIS 中特殊处理）
    result->sampledBSDFType = BSDFType_Specular;
    result->isDelta = true;
}

/// 完美镜面 PDF：Delta 分布，对任意非反射方向返回 0
CUDA_DEVICE_FUNCTION CUDA_INLINE float getSpecularBSDFPDF(
    const Vector3D& dirInLocal,
    const Vector3D& dirOutLocal,
    const Normal3D& geomNormalLocal) {

    Vector3D reflected = 2.0f * dot(dirInLocal, geomNormalLocal) * geomNormalLocal - dirInLocal;
    float diff = std::abs(dot(reflected, dirOutLocal) - 1.0f);
    return (diff < 1e-5f) ? 1.0f : 0.0f;
}


// ============================================================================
// 4.5 完美镜面透射 BSDF（SpecularTransmission，支持色散）
// ============================================================================

/// 计算折射方向（斯涅尔定律）
/// wi: 入射方向（指向表面）, n: 几何法线（指向入射侧）, eta = etaI/etaT
/// 返回 false 表示全内反射
CUDA_DEVICE_FUNCTION CUDA_INLINE bool refract(
    const Vector3D& wi, const Normal3D& n, float eta, Vector3D* wt) {
    float cosThetaI = dot(wi, n);
    float sin2ThetaI = ::vlr::vlr_max(0.0f, 1.0f - cosThetaI * cosThetaI);
    float sin2ThetaT = eta * eta * sin2ThetaI;
    if (sin2ThetaT >= 1.0f) return false;  // 全内反射
    float cosThetaT = safeSqrt(1.0f - sin2ThetaT);
    *wt = eta * wi - (eta * cosThetaI - cosThetaT) * n;
    return true;
}

/// 评估完美镜面透射（Delta，评估时通常返回 0）
CUDA_DEVICE_FUNCTION CUDA_INLINE SampledSpectrum evaluateSpecularTransmissionBSDF(
    float etaI, float etaT, const SampledSpectrum& transmittance,
    const Vector3D& dirInLocal, const Vector3D& dirOutLocal,
    const Normal3D& geomNormalLocal) {
    float cosOut = dot(dirOutLocal, geomNormalLocal);
    if (cosOut >= 0.0f) return SampledSpectrum::Zero();  // 透射出射在法线负侧

    Vector3D wt;
    if (!refract(dirInLocal, geomNormalLocal, etaI / etaT, &wt))
        return SampledSpectrum::Zero();
    float diff = std::abs(dot(wt, dirOutLocal) - 1.0f);
    if (diff < 1e-5f) {
        float F = FresnelDielectric(dot(dirInLocal, geomNormalLocal), etaI, etaT);
        return transmittance * (1.0f - F);  // 透射 = (1-F) * 透射率
    }
    return SampledSpectrum::Zero();
}

/// 采样完美镜面透射（支持波长相关折射率）
/// wls: 波长采样，当 singleWlSelected 时用 selectedLambda 对应波长计算 IOR
CUDA_DEVICE_FUNCTION CUDA_INLINE void sampleSpecularTransmissionBSDF(
    float etaI, float etaT, float dispersionStrength,
    const WavelengthSamples* wls, bool singleWlSelected,
    const Vector3D& dirInLocal, const Normal3D& geomNormalLocal,
    float /*u0*/, float /*u1*/,
    BSDFSampleResult* result) {

    // 单波长选择：使用选定波长的 IOR
    float etaT_eff = etaT;
    if (wls && singleWlSelected && dispersionStrength > 0.0f) {
        uint32_t idx = wls->selectedLambdaIndex() % NumSpectralSamples;
        float lambda = wls->lambdas[idx];
        etaT_eff = iorAtWavelength(lambda, etaT, dispersionStrength);
    }

    Vector3D wt;
    if (!refract(dirInLocal, geomNormalLocal, etaI / etaT_eff, &wt)) {
        // 全内反射：采样反射方向
        float NdotL = dot(dirInLocal, geomNormalLocal);
        result->dirLocal = 2.0f * NdotL * geomNormalLocal - dirInLocal;
        result->f = SampledSpectrum::One();  // 全反射
        result->pdf = 1.0f;
        result->sampledBSDFType = BSDFType_Specular;  // 反射分量
        result->isDelta = true;
        return;
    }

    float F = FresnelDielectric(dot(dirInLocal, geomNormalLocal), etaI, etaT_eff);
    SampledSpectrum transmittance;
    for (int i = 0; i < NumSpectralSamples; ++i)
        transmittance.values[i] = (1.0f - F) / (etaT_eff * etaT_eff);  // 透射权重校正

    result->dirLocal = wt;
    result->f = transmittance;
    result->pdf = 1.0f;
    result->sampledBSDFType = BSDFType_SpecularTransmission;
    result->isDelta = true;
}

/// SpecularTransmission PDF（Delta）
CUDA_DEVICE_FUNCTION CUDA_INLINE float getSpecularTransmissionBSDFPDF(
    float etaI, float etaT,
    const Vector3D& dirInLocal, const Vector3D& dirOutLocal,
    const Normal3D& geomNormalLocal) {
    Vector3D wt;
    if (!refract(dirInLocal, geomNormalLocal, etaI / etaT, &wt))
        return 0.0f;
    float diff = std::abs(dot(wt, dirOutLocal) - 1.0f);
    return (diff < 1e-5f) ? 1.0f : 0.0f;
}


// ============================================================================
// 4.6 粗糙透射 BSDF（GGX 微表面透射）
// ============================================================================

/// 评估 GGX 透射 BSDF（Walter et al. 2007, PBRT-v4）
CUDA_DEVICE_FUNCTION CUDA_INLINE SampledSpectrum evaluateGGXTransmissionBSDF(
    const SampledSpectrum& transmittance, float roughness, float etaI, float etaT,
    const Vector3D& dirInLocal, const Vector3D& dirOutLocal,
    const Normal3D& geomNormalLocal) {

    float NdotL = dot(dirInLocal, geomNormalLocal);
    float NdotV = dot(dirOutLocal, geomNormalLocal);
    if (NdotL <= 0.0f || NdotV >= 0.0f) return SampledSpectrum::Zero();  // 透射：入射上侧，出射下侧

    // 确定半向量：透射时 H 在入射和折射方向之间
    float eta = etaI / etaT;
    Vector3D halfVec = normalize(-dirInLocal + eta * dirOutLocal);
    float NdotH = dot(halfVec, geomNormalLocal);
    if (NdotH <= 0.0f) halfVec = -halfVec;

    float alpha = roughnessToAlpha(roughness);
    float alpha2 = alpha * alpha;

    float D = GGX_D(std::abs(dot(halfVec, geomNormalLocal)), alpha2);
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
        // 反射分量
        float VdotH = dot(V, H);
        result->dirLocal = 2.0f * VdotH * H - V;
        SampledSpectrum refl;
        for (int i = 0; i < NumSpectralSamples; ++i) refl.values[i] = 1.0f;
        result->f = refl;  // 简化，完整需乘以 F
        float NdotV = std::abs(dot(result->dirLocal, geomNormalLocal));
        float alpha2 = alpha * alpha;
        float D = GGX_D(std::abs(dot(H, geomNormalLocal)), alpha2);
        float G1_v = GGX_G1(NdotV, alpha2);
        result->pdf = F * D * G1_v * std::abs(dot(result->dirLocal, geomNormalLocal)) / (4.0f * std::abs(VdotH));
        result->sampledBSDFType = BSDFType_GGX;
        result->isDelta = false;
    } else {
        // 透射分量：H 为微表面法线，折射在 H 定义的平面上
        Normal3D H_normal = dot(H, geomNormalLocal) > 0.0f ? H : Vector3D(-H.x, -H.y, -H.z);
        Vector3D wt;
        if (!refract(dirInLocal, H_normal, eta, &wt)) {
            result->pdf = 0.0f;
            result->f = SampledSpectrum::Zero();
            return;
        }
        result->dirLocal = wt;
        result->sampledBSDFType = BSDFType_GGXTransmission;
        float NdotV = std::abs(dot(wt, geomNormalLocal));
        float NdotL = std::abs(dot(dirInLocal, geomNormalLocal));
        float alpha2 = alpha * alpha;
        float D = GGX_D(std::abs(dot(H, geomNormalLocal)), alpha2);
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

    float NdotL = dot(dirInLocal, geomNormalLocal);
    float NdotV = dot(dirOutLocal, geomNormalLocal);
    if (NdotL <= 0.0f || NdotV <= 0.0f) return SampledSpectrum::Zero();

    Vector3D halfVec = normalize(dirInLocal + dirOutLocal);
    float F = SchlickFresnel(dot(dirOutLocal, halfVec), 0.04f);

    SampledSpectrum diffTerm = diffuse * (1.0f - F) * VLR_M_INV_PI;

    float alpha = roughnessToAlpha(roughness);
    float alpha2 = alpha * alpha;
    float NdotH = dot(halfVec, geomNormalLocal);
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
CUDA_DEVICE_FUNCTION CUDA_INLINE void sampleFresnelBlendBSDF(
    const SampledSpectrum& diffuse, const SampledSpectrum& specular,
    float roughness,
    const Vector3D& dirInLocal, const Normal3D& geomNormalLocal,
    float u0, float u1, float u2,
    BSDFSampleResult* result) {

    float NdotL = dot(dirInLocal, geomNormalLocal);
    if (NdotL <= 0.0f) { result->pdf = 0.0f; result->f = SampledSpectrum::Zero(); return; }

    // 使用平均 F 决定采样概率
    float avgF = 0.04f + 0.96f * 0.2f;  // 简化
    if (u2 < avgF) {
        SampledSpectrum reflectance;
        for (int i = 0; i < NumSpectralSamples; ++i) reflectance.values[i] = (specular.values[i] + 0.04f) * 0.5f;
        sampleGGXBSDF(reflectance, roughness, dirInLocal, geomNormalLocal, u0, u1, result);
    } else {
        sampleLambertBSDF(diffuse, dirInLocal, geomNormalLocal, u0, u1, result);
        result->sampledBSDFType = BSDFType_FresnelBlend;
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

    float NdotL = dot(dirInLocal, geomNormalLocal);
    float NdotV = dot(dirOutLocal, geomNormalLocal);
    if (NdotL <= 0.0f || NdotV <= 0.0f) return SampledSpectrum::Zero();

    Vector3D halfVec = normalize(dirInLocal + dirOutLocal);
    SampledSpectrum F;
    SchlickFresnelSpectrum(dot(dirOutLocal, halfVec), F0, &F);

    SampledSpectrum kD = SampledSpectrum::One();
    for (int i = 0; i < NumSpectralSamples; ++i)
        kD.values[i] = (1.0f - metallic) * (1.0f - F.values[i]);

    SampledSpectrum diffuseTerm = baseColor * kD * VLR_M_INV_PI;

    float alpha = roughnessToAlpha(roughness);
    float alpha2 = alpha * alpha;
    float NdotH = dot(halfVec, geomNormalLocal);
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
        getLambertAlbedo(matDesc, &albedo);
        return evaluateLambertBSDF(albedo, dirInLocal, dirOutLocal, ctx.geomNormalLocal);
    }
    case BSDFType_GGX: {
        SampledSpectrum reflectance;
        float roughness;
        getGGXParams(matDesc, &reflectance, &roughness);
        return evaluateGGXBSDF(reflectance, roughness, dirInLocal, dirOutLocal, ctx.geomNormalLocal);
    }
    case BSDFType_Specular: {
        SampledSpectrum reflectance;
        getSpecularReflectance(matDesc, &reflectance);
        return evaluateSpecularBSDF(reflectance, dirInLocal, dirOutLocal, ctx.geomNormalLocal);
    }
    case BSDFType_SpecularTransmission: {
        float ior, disp;
        getTransmissionParams(matDesc, &ior, &disp);
        SampledSpectrum trans;
        for (int i = 0; i < NumSpectralSamples; ++i) trans.values[i] = 1.0f;
        return evaluateSpecularTransmissionBSDF(1.0f, ior, trans, dirInLocal, dirOutLocal, ctx.geomNormalLocal);
    }
    case BSDFType_GGXTransmission: {
        float ior, disp;
        getTransmissionParams(matDesc, &ior, &disp);
        SampledSpectrum refl;
        float roughness;
        getGGXParams(matDesc, &refl, &roughness);
        SampledSpectrum trans;
        for (int i = 0; i < NumSpectralSamples; ++i) trans.values[i] = 1.0f;
        return evaluateGGXTransmissionBSDF(trans, roughness, 1.0f, ior, dirInLocal, dirOutLocal, ctx.geomNormalLocal);
    }
    case BSDFType_FresnelBlend: {
        SampledSpectrum diff, spec;
        float roughness;
        getFresnelBlendParams(matDesc, &diff, &spec, &roughness);
        return evaluateFresnelBlendBSDF(diff, spec, roughness, dirInLocal, dirOutLocal, ctx.geomNormalLocal);
    }
    case BSDFType_UE4BRDF:
    case BSDFType_FrostbiteBRDF: {
        SampledSpectrum baseColor;
        float metallic, roughness;
        getUE4Params(matDesc, &baseColor, &metallic, &roughness);
        return evaluateUE4BRDF(baseColor, metallic, roughness, dirInLocal, dirOutLocal, ctx.geomNormalLocal);
    }
    case BSDFType_MixedBSDF: {
        SampledSpectrum alb0, alb1;
        float weight, roughness;
        getMixedParams(matDesc, &alb0, &alb1, &weight, &roughness);
        return evaluateMixedBSDF(alb0, alb1, weight, roughness, dirInLocal, dirOutLocal, ctx.geomNormalLocal);
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
    sampleBSDFWithU2(ctx, dirInLocal, u0, u1, u0, result);  // u2=u0 作为第三随机数
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

    switch (type) {
    case BSDFType_Lambert: {
        SampledSpectrum albedo;
        getLambertAlbedo(matDesc, &albedo);
        sampleLambertBSDF(albedo, dirInLocal, ctx.geomNormalLocal, u0, u1, result);
        break;
    }
    case BSDFType_GGX: {
        SampledSpectrum reflectance;
        float roughness;
        getGGXParams(matDesc, &reflectance, &roughness);
        sampleGGXBSDF(reflectance, roughness, dirInLocal, ctx.geomNormalLocal, u0, u1, result);
        break;
    }
    case BSDFType_Specular: {
        SampledSpectrum reflectance;
        getSpecularReflectance(matDesc, &reflectance);
        sampleSpecularBSDF(reflectance, dirInLocal, ctx.geomNormalLocal, u0, u1, result);
        break;
    }
    case BSDFType_SpecularTransmission: {
        float ior, disp;
        getTransmissionParams(matDesc, &ior, &disp);
        sampleSpecularTransmissionBSDF(1.0f, ior, disp, wls, singleWl,
            dirInLocal, ctx.geomNormalLocal, u0, u1, result);
        break;
    }
    case BSDFType_GGXTransmission: {
        float ior, disp;
        getTransmissionParams(matDesc, &ior, &disp);
        SampledSpectrum trans;
        for (int i = 0; i < NumSpectralSamples; ++i) trans.values[i] = 1.0f;
        SampledSpectrum refl;
        float roughness;
        getGGXParams(matDesc, &refl, &roughness);
        sampleGGXTransmissionBSDF(trans, roughness, 1.0f, ior,
            dirInLocal, ctx.geomNormalLocal, u0, u1, u2, result);
        break;
    }
    case BSDFType_FresnelBlend: {
        SampledSpectrum diff, spec;
        float roughness;
        getFresnelBlendParams(matDesc, &diff, &spec, &roughness);
        sampleFresnelBlendBSDF(diff, spec, roughness, dirInLocal, ctx.geomNormalLocal, u0, u1, u2, result);
        break;
    }
    case BSDFType_UE4BRDF:
    case BSDFType_FrostbiteBRDF: {
        SampledSpectrum baseColor;
        float metallic, roughness;
        getUE4Params(matDesc, &baseColor, &metallic, &roughness);
        sampleGGXBSDF(baseColor, roughness, dirInLocal, ctx.geomNormalLocal, u0, u1, result);
        result->sampledBSDFType = type;
        break;
    }
    case BSDFType_MixedBSDF: {
        SampledSpectrum alb0, alb1;
        float weight, roughness;
        getMixedParams(matDesc, &alb0, &alb1, &weight, &roughness);
        if (u2 < weight)
            sampleGGXBSDF(alb1, roughness, dirInLocal, ctx.geomNormalLocal, u0, u1, result);
        else
            sampleLambertBSDF(alb0, dirInLocal, ctx.geomNormalLocal, u0, u1, result);
        result->sampledBSDFType = BSDFType_MixedBSDF;
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
    case BSDFType_GGX: {
        SampledSpectrum reflectance;
        float roughness;
        getGGXParams(matDesc, &reflectance, &roughness);
        return getGGXBSDFPDF(reflectance, roughness, dirInLocal, dirOutLocal, ctx.geomNormalLocal);
    }
    case BSDFType_Specular:
        return getSpecularBSDFPDF(dirInLocal, dirOutLocal, ctx.geomNormalLocal);
    case BSDFType_SpecularTransmission: {
        float ior, disp;
        getTransmissionParams(matDesc, &ior, &disp);
        return getSpecularTransmissionBSDFPDF(1.0f, ior, dirInLocal, dirOutLocal, ctx.geomNormalLocal);
    }
    case BSDFType_GGXTransmission: {
        SampledSpectrum refl;
        float roughness;
        getGGXParams(matDesc, &refl, &roughness);
        return getGGXBSDFPDF(refl, roughness, dirInLocal, dirOutLocal, ctx.geomNormalLocal);
    }
    case BSDFType_FresnelBlend:
    case BSDFType_UE4BRDF:
    case BSDFType_FrostbiteBRDF: {
        SampledSpectrum refl;
        float roughness;
        getGGXParams(matDesc, &refl, &roughness);
        return getGGXBSDFPDF(refl, roughness, dirInLocal, dirOutLocal, ctx.geomNormalLocal);
    }
    case BSDFType_MixedBSDF: {
        float weight, roughness;
        SampledSpectrum a0, a1;
        getMixedParams(matDesc, &a0, &a1, &weight, &roughness);
        SampledSpectrum refl;
        getGGXParams(matDesc, &refl, &roughness);
        float pdfGGX = getGGXBSDFPDF(refl, roughness, dirInLocal, dirOutLocal, ctx.geomNormalLocal);
        float pdfLambert = getLambertBSDFPDF(dirOutLocal, ctx.geomNormalLocal);
        return (1.0f - weight) * pdfLambert + weight * pdfGGX;
    }
    default:
        return 0.0f;
    }
}


// ============================================================================
// 6. EDF 发光评估
// ============================================================================

/// 评估 EDF 发光辐射度
/// 简化为 Lambertian 发光：各向同性
CUDA_DEVICE_FUNCTION CUDA_INLINE EDFEvaluateResult evaluateEDF(
    const EDFContext& ctx,
    const Vector3D& dirOutLocal) {

    const SurfaceMaterialDescriptor& matDesc = *ctx.matDesc;

    SampledSpectrum radiance;
    getEmissiveRadiance(matDesc, &radiance);

    EDFEvaluateResult result;
    result.Le = radiance;
    result.hasEmission = materialHasEmission(matDesc);

    // Lambertian EDF: 出射强度 = Le (与方向无关，cos 在渲染方程中处理)
    if (result.hasEmission && dot(dirOutLocal, ctx.surfPt->shadingFrame.z) > 0.0f) {
        result.Le = radiance;
    } else if (!result.hasEmission) {
        result.Le = SampledSpectrum::Zero();
    }

    return result;
}

/// 获取 EDF 发光辐射度（不关心方向时使用）
CUDA_DEVICE_FUNCTION CUDA_INLINE SampledSpectrum evaluateEmittance(
    const SurfaceMaterialDescriptor& matDesc) {

    SampledSpectrum radiance;
    getEmissiveRadiance(matDesc, &radiance);
    return radiance;
}

} // namespace shared
} // namespace vlr
