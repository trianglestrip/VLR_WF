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

// 数学常量来自 basic_types.h 宏定义，直接使用 VLR_M_PI 等

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
// 2.1 LambertianScattering 次表面散射 BSDF（双向 Lambert）
// ============================================================================
// 简化次表面散射模型：允许光从入射半球反射，或透射到另一侧半球。
// 与普通 Lambert 的区别：透射分量允许 dirOut 与 dirIn 在法线异侧。
// 公式：f = albedo/π（反射和透射相同，均使用 Lambert 分布）
// PDF：50% 反射 + 50% 透射，各为余弦加权半球，总 pdf = 0.5 * |cos(θ)|/π
// ============================================================================

/// 评估 LambertianScattering BSDF: f = albedo/π（反射和透射均适用）
/// 入射和出射可在同半球（反射）或异半球（透射）
CUDA_DEVICE_FUNCTION CUDA_INLINE SampledSpectrum evaluateLambertianScatteringBSDF(
    const SampledSpectrum& albedo,
    const Vector3D& dirInLocal,
    const Vector3D& dirOutLocal,
    const Normal3D& geomNormalLocal) {

    float cosOut = dot(dirOutLocal, geomNormalLocal);
    if (cosOut == 0.0f)
        return SampledSpectrum::Zero();
    // 反射：cosOut > 0（同半球）；透射：cosOut < 0（异半球），均有效
    return albedo * VLR_M_INV_PI;
}

/// 采样 LambertianScattering BSDF：50% 反射（余弦半球）+ 50% 透射（翻转法线余弦半球）
CUDA_DEVICE_FUNCTION CUDA_INLINE void sampleLambertianScatteringBSDF(
    const SampledSpectrum& albedo,
    const Vector3D& dirInLocal,
    const Normal3D& geomNormalLocal,
    float u0, float u1, float u2,
    BSDFSampleResult* result) {

    // u2 决定反射(0~0.5)还是透射(0.5~1)
    bool sampleReflection = (u2 < 0.5f);
    Normal3D effectiveNormal = sampleReflection ? geomNormalLocal
        : Normal3D(-geomNormalLocal.x, -geomNormalLocal.y, -geomNormalLocal.z);

    // 余弦加权半球采样（与 Lambert 相同）
    float r = safeSqrt(u0);
    float phi = u1 * VLR_M_2PI;
    float x = r * std::cos(phi);
    float y = r * std::sin(phi);
    float z = safeSqrt(1.0f - u0);

    Vector3D tangent = (std::abs(effectiveNormal.z) < 0.999f)
        ? normalize(cross(Vector3D(0, 1, 0), effectiveNormal))
        : normalize(cross(Vector3D(1, 0, 0), effectiveNormal));
    Vector3D bitangent = cross(effectiveNormal, tangent);

    Vector3D dirLocal = normalize(
        tangent * x + bitangent * y + effectiveNormal * z);

    result->dirLocal = dirLocal;
    result->f = albedo * VLR_M_INV_PI;
    // PDF: 0.5 * cos/π（反射和透射各 50% 选择概率）
    result->pdf = 0.5f * std::abs(dot(dirLocal, effectiveNormal)) * VLR_M_INV_PI;
    result->sampledBSDFType = BSDFType_LambertianScattering;
    result->isDelta = false;
}

/// LambertianScattering BSDF 的 PDF: 0.5 * |cos(θ)| / π
/// 反射和透射各 50% 概率，给定方向只属于其一
CUDA_DEVICE_FUNCTION CUDA_INLINE float getLambertianScatteringBSDFPDF(
    const Vector3D& dirInLocal,
    const Vector3D& dirOutLocal,
    const Normal3D& geomNormalLocal) {

    float cosOut = dot(dirOutLocal, geomNormalLocal);
    if (cosOut == 0.0f)
        return 0.0f;
    return 0.5f * std::abs(cosOut) * VLR_M_INV_PI;
}


// ============================================================================
// 3. GGX 微表面 BSDF
// ============================================================================

/// GGX 法线分布函数 D(h)
/// 避免 denom 过小导致数值溢出
CUDA_DEVICE_FUNCTION CUDA_INLINE float GGX_D(
    float NdotH, float alpha2) {

    if (NdotH <= 0.0f)
        return 0.0f;

    float denom = NdotH * NdotH * (alpha2 - 1.0f) + 1.0f;
    denom = denom * denom * VLR_M_PI;
    denom = ::vlr::vlr_max(denom, 1e-10f);
    return alpha2 / denom;
}

/// GGX  Smith 几何项 G1
/// 避免 NdotV 过小导致 tan2 爆炸
CUDA_DEVICE_FUNCTION CUDA_INLINE float GGX_G1(float NdotV, float alpha2) {
    if (NdotV <= 0.0f)
        return 0.0f;
    float NdotVSafe = ::vlr::vlr_max(NdotV, 1e-6f);
    float tan2 = (1.0f - NdotVSafe * NdotVSafe) / (NdotVSafe * NdotVSafe);
    tan2 = ::vlr::vlr_min(tan2, 1e10f);  // 防止 tan2 过大
    return 2.0f / (1.0f + safeSqrt(1.0f + alpha2 * tan2));
}

// ============================================================================
// 2.6 各向异性 GGX（Anisotropic GGX）
// ============================================================================
// 参考：Heitz 2014, Burley 2015, Heitz 2017 "Sampling the GGX Distribution of Visible Normals"
// 切线空间：x=tangent, y=bitangent, z=normal；dir 已在局部坐标，HdotX=H.x, HdotY=H.y

/// 将 roughness + anisotropy 转换为 alphaX, alphaY（Disney/Burley 参数化）
/// anisotropy: 0=各向同性, 0.9=强各向异性
CUDA_DEVICE_FUNCTION CUDA_INLINE void roughnessAnisotropyToAlpha(
    float roughness, float anisotropy,
    float* alphaX, float* alphaY) {
    float r = ::vlr::vlr_max(roughness, 0.001f);
    float r2 = r * r;
    float a = ::vlr::vlr_max(anisotropy, 0.0f);
    float aspect = safeSqrt(1.0f - 0.9f * a);  // Disney: aspect in [0.316, 1]
    *alphaX = r2 / aspect;
    *alphaY = r2 * aspect;
    *alphaX = ::vlr::vlr_max(*alphaX, 0.0001f);
    *alphaY = ::vlr::vlr_max(*alphaY, 0.0001f);
}

/// 各向异性 GGX 法线分布函数 D(h)
/// D(h) = 1 / (π * αx * αy * [(h·x/αx)² + (h·y/αy)² + (h·z)²]²)
/// 局部坐标下：HdotX=H.x, HdotY=H.y, NdotH=H.z
CUDA_DEVICE_FUNCTION CUDA_INLINE float GGX_D_Aniso(
    float NdotH, float HdotX, float HdotY,
    float alphaX, float alphaY) {
    if (NdotH <= 0.0f)
        return 0.0f;
    float ax2 = alphaX * alphaX;
    float ay2 = alphaY * alphaY;
    float denom = (HdotX * HdotX / ax2 + HdotY * HdotY / ay2 + NdotH * NdotH);
    denom = denom * denom * VLR_M_PI * alphaX * alphaY;
    denom = ::vlr::vlr_max(denom, 1e-10f);
    return 1.0f / denom;
}

/// 各向异性 Smith G1 几何项
/// G1(v) = 2 / (1 + sqrt(1 + α²tan²θ))，各向异性：α² = (VdotX²αy² + VdotY²αx² + VdotZ²αx²αy²) / (VdotZ²)
/// 简化：Λ(v) = (-1 + sqrt(1 + (VdotX²/αx² + VdotY²/αy²) / VdotZ²)) / 2
CUDA_DEVICE_FUNCTION CUDA_INLINE float GGX_G1_Aniso(
    float NdotV, float VdotX, float VdotY,
    float alphaX, float alphaY) {
    if (NdotV <= 0.0f)
        return 0.0f;
    float NdotVSafe = ::vlr::vlr_max(NdotV, 1e-6f);
    float ax2 = alphaX * alphaX;
    float ay2 = alphaY * alphaY;
    float tan2 = (VdotX * VdotX / ax2 + VdotY * VdotY / ay2) / (NdotVSafe * NdotVSafe);
    tan2 = ::vlr::vlr_min(tan2, 1e10f);
    return 2.0f / (1.0f + safeSqrt(1.0f + tan2));
}

/// 各向异性 GGX VNDF 采样（Heitz 2017）
/// Vh = normalize(αx*V.x, αy*V.y, V.z)，椭圆变换后采样
CUDA_DEVICE_FUNCTION CUDA_INLINE Vector3D sampleGGXVNDF_Aniso(
    const Vector3D& V,
    float alphaX, float alphaY,
    float u1, float u2) {
    Vector3D Vh = normalize(Vector3D(
        alphaX * V.x,
        alphaY * V.y,
        ::vlr::vlr_max(V.z, 1e-6f)));

    float lensq = Vh.y * Vh.y + Vh.z * Vh.z;
    Vector3D T1 = (lensq > 1e-10f)
        ? normalize(Vector3D(0, -Vh.z, Vh.y))
        : Vector3D(0, 0, 1);
    Vector3D T2 = cross(Vh, T1);

    float r = safeSqrt(u1);
    float phi = u2 * VLR_M_2PI;
    float t1 = r * std::cos(phi);
    float t2 = r * std::sin(phi);
    float s = 0.5f * (1.0f + Vh.z);
    t2 = (1.0f - s) * safeSqrt(::vlr::vlr_max(0.0f, 1.0f - t1 * t1)) + s * t2;

    Vector3D Nh = t1 * T1 + t2 * T2 + safeSqrt(::vlr::vlr_max(0.0f, 1.0f - t1 * t1 - t2 * t2)) * Vh;

    Vector3D H = normalize(Vector3D(
        alphaX * Nh.x,
        alphaY * Nh.y,
        ::vlr::vlr_max(0.0f, Nh.z)));
    return H;
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

    Vector3D halfSum = dirInLocal + dirOutLocal;
    float halfLenSq = dot(halfSum, halfSum);
    if (halfLenSq < 1e-12f)
        return SampledSpectrum::Zero();  // 掠射角：half 接近零向量
    Vector3D halfVec = normalize(halfSum);
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
    
    float denom = 4.0f * NdotL * NdotV;
    if (denom < 1e-7f)
        return SampledSpectrum::Zero();

    float spec = D * G / denom;

    // Use reflectance as F0 for metallic materials (Schlick Fresnel with colored F0)
    SampledSpectrum result;
    for (int i = 0; i < NumSpectralSamples; ++i) {
        float F0 = reflectance.values[i];
        float F = F0 + (1.0f - F0) * powf(1.0f - VdotH, 5.0f);
        result.values[i] = F * spec;
    }

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

    float denom = 4.0f * std::abs(NdotL) * NdotV;
    float spec = (denom > 1e-7f) ? (D * G / denom) : 0.0f;

    result->dirLocal = dirOutLocal;
    result->pdf = pdf;
    result->isDelta = false;
    result->sampledBSDFType = BSDFType_GGX;

    // Use reflectance as F0 for metallic materials (Schlick Fresnel with colored F0)
    result->f = SampledSpectrum::Zero();
    for (int i = 0; i < NumSpectralSamples; ++i) {
        float F0 = reflectance.values[i];
        float F = F0 + (1.0f - F0) * powf(1.0f - VdotH, 5.0f);
        result->f.values[i] = F * spec;
    }
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

    Vector3D halfSum = dirInLocal + dirOutLocal;
    float halfLenSq = dot(halfSum, halfSum);
    if (halfLenSq < 1e-12f)
        return 0.0f;
    Vector3D halfVec = normalize(halfSum);
    float NdotH = dot(halfVec, geomNormalLocal);
    float VdotH = dot(dirOutLocal, halfVec);

    if (NdotH <= 0.0f || VdotH <= 0.0f)
        return 0.0f;

    float alpha = roughnessToAlpha(roughness);
    float alpha2 = alpha * alpha;

    float D = GGX_D(NdotH, alpha2);
    float G1_v = GGX_G1(NdotV, alpha2);

    float VdotHSafe = ::vlr::vlr_max(VdotH, 1e-6f);
    return D * G1_v * std::abs(NdotL) / (4.0f * VdotHSafe);
}


// ============================================================================
// 3.1 导体微表面反射 BSDF（MicrofacetReflection）
// ============================================================================

/// 评估导体微表面反射 BSDF（GGX + FresnelConductor）- 各向同性
CUDA_DEVICE_FUNCTION CUDA_INLINE SampledSpectrum evaluateMicrofacetReflectionBSDF(
    const SampledSpectrum& coeffR,
    const SampledSpectrum& eta,
    const SampledSpectrum& kappa,
    float roughness,
    const Vector3D& dirInLocal,
    const Vector3D& dirOutLocal,
    const Normal3D& geomNormalLocal) {

    float NdotL = dot(dirInLocal, geomNormalLocal);
    float NdotV = dot(dirOutLocal, geomNormalLocal);

#if defined(__CUDA_ARCH__) && defined(VLR_DEBUG_BSDF_VERBOSE)
    if (threadIdx.x == 0 && blockIdx.x == 0) {
        printf("[GPU evaluateMicrofacetReflectionBSDF]\n");
        printf("  eta=(%.3f,%.3f,%.3f), kappa=(%.3f,%.3f,%.3f), roughness=%.3f\n",
            eta.values[0], eta.values[1], eta.values[2],
            kappa.values[0], kappa.values[1], kappa.values[2], roughness);
        printf("  NdotL=%.3f, NdotV=%.3f\n", NdotL, NdotV);
    }
#endif

    if (NdotL <= 0.0f || NdotV <= 0.0f)
        return SampledSpectrum::Zero();

    Vector3D halfSum = dirInLocal + dirOutLocal;
    float halfLenSq = dot(halfSum, halfSum);
    if (halfLenSq < 1e-12f)
        return SampledSpectrum::Zero();
    
    Vector3D halfVec = normalize(halfSum);
    float NdotH = dot(halfVec, geomNormalLocal);
    if (NdotH <= 0.0f)
        return SampledSpectrum::Zero();

    float VdotH = dot(dirOutLocal, halfVec);
    float alpha = roughnessToAlpha(roughness);
    float alpha2 = alpha * alpha;

    float D = GGX_D(NdotH, alpha2);
    float G1_l = GGX_G1(NdotL, alpha2);
    float G1_v = GGX_G1(NdotV, alpha2);
    float G = G1_l * G1_v;

    float denom = 4.0f * NdotL * NdotV;
    if (denom < 1e-7f)
        return SampledSpectrum::Zero();

    SampledSpectrum F;
    float cosTheta = std::abs(VdotH);
    for (int i = 0; i < NumSpectralSamples; ++i) {
        float fresnelValue = FresnelConductor(cosTheta, eta.values[i], kappa.values[i]);
        F.values[i] = ::vlr::vlr_min(1.0f, fresnelValue);  // Clamp 到 [0,1] 确保能量守恒
    }

#if defined(__CUDA_ARCH__) && defined(VLR_DEBUG_BSDF_VERBOSE)
    if (threadIdx.x == 0 && blockIdx.x == 0) {
        printf("  cosTheta=%.3f, D=%.6f, G=%.6f, denom=%.6f\n", cosTheta, D, G, denom);
        printf("  Fresnel F=(%.6f,%.6f,%.6f,%.6f)\n", 
            F.values[0], F.values[1], F.values[2], F.values[3]);
    }
#endif

    SampledSpectrum result;
    float spec = D * G / denom;
    for (int i = 0; i < NumSpectralSamples; ++i) {
        result.values[i] = coeffR.values[i] * F.values[i] * spec;
    }
    
#if defined(__CUDA_ARCH__) && defined(VLR_DEBUG_BSDF_VERBOSE)
    if (threadIdx.x == 0 && blockIdx.x == 0) {
        printf("  Final BSDF=(%.6f,%.6f,%.6f,%.6f)\n",
            result.values[0], result.values[1], result.values[2], result.values[3]);
    }
#endif
    
    return result;
}

/// 评估导体微表面反射 BSDF（各向异性 GGX + FresnelConductor）
/// dirInLocal/dirOutLocal 已在切线空间，x=tangent, y=bitangent, z=normal
CUDA_DEVICE_FUNCTION CUDA_INLINE SampledSpectrum evaluateMicrofacetReflectionBSDF_Aniso(
    const SampledSpectrum& coeffR,
    const SampledSpectrum& eta,
    const SampledSpectrum& kappa,
    float alphaX, float alphaY,
    const Vector3D& dirInLocal,
    const Vector3D& dirOutLocal,
    const Normal3D& geomNormalLocal) {

    float NdotL = dot(dirInLocal, geomNormalLocal);
    float NdotV = dot(dirOutLocal, geomNormalLocal);
    if (NdotL <= 0.0f || NdotV <= 0.0f)
        return SampledSpectrum::Zero();

    Vector3D halfSum = dirInLocal + dirOutLocal;
    float halfLenSq = dot(halfSum, halfSum);
    if (halfLenSq < 1e-12f)
        return SampledSpectrum::Zero();
    
    Vector3D halfVec = normalize(halfSum);
    float NdotH = dot(halfVec, geomNormalLocal);
    if (NdotH <= 0.0f)
        return SampledSpectrum::Zero();

    float HdotX = halfVec.x;
    float HdotY = halfVec.y;
    float VdotH = dot(dirOutLocal, halfVec);
    float LdotX = dirInLocal.x, LdotY = dirInLocal.y;
    float VdotX = dirOutLocal.x, VdotY = dirOutLocal.y;

    float D = GGX_D_Aniso(NdotH, HdotX, HdotY, alphaX, alphaY);
    float G1_l = GGX_G1_Aniso(NdotL, LdotX, LdotY, alphaX, alphaY);
    float G1_v = GGX_G1_Aniso(NdotV, VdotX, VdotY, alphaX, alphaY);
    float G = G1_l * G1_v;

    float denom = 4.0f * NdotL * NdotV;
    if (denom < 1e-7f)
        return SampledSpectrum::Zero();

    SampledSpectrum F;
    float cosTheta = std::abs(VdotH);
    for (int i = 0; i < NumSpectralSamples; ++i) {
        float fresnelValue = FresnelConductor(cosTheta, eta.values[i], kappa.values[i]);
        F.values[i] = ::vlr::vlr_min(1.0f, fresnelValue);
    }

    SampledSpectrum result;
    float spec = D * G / denom;
    for (int i = 0; i < NumSpectralSamples; ++i) {
        result.values[i] = coeffR.values[i] * F.values[i] * spec;
    }
    return result;
}

/// 采样导体微表面反射 BSDF（GGX VNDF + FresnelConductor）- 各向同性
CUDA_DEVICE_FUNCTION CUDA_INLINE void sampleMicrofacetReflectionBSDF(
    const SampledSpectrum& coeffR,
    const SampledSpectrum& eta,
    const SampledSpectrum& kappa,
    float roughness,
    const Vector3D& dirInLocal,
    const Normal3D& geomNormalLocal,
    float u0, float u1,
    BSDFSampleResult* result) {

    float NdotL = dot(dirInLocal, geomNormalLocal);
    if (NdotL <= 0.0f) {
        result->pdf = 0.0f;
        result->f = SampledSpectrum::Zero();
        return;
    }

    float alpha = roughnessToAlpha(roughness);
    Vector3D H = sampleGGXVNDF(dirInLocal, alpha, u0, u1);

    float VdotH = dot(dirInLocal, H);
    if (VdotH <= 0.0f) {
        result->pdf = 0.0f;
        result->f = SampledSpectrum::Zero();
        return;
    }

    Vector3D dirOutLocal = 2.0f * VdotH * H - dirInLocal;
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
    float G1_l = GGX_G1(NdotL, alpha2);
    float G = G1_l * G1_v;

    float VdotH_clamped = ::vlr::vlr_max(VdotH, 1e-6f);
    // VNDF PDF: G1 应使用观察方向 dirInLocal，即 G1(NdotL)
    float pdf = D * G1_l * NdotV / (4.0f * VdotH_clamped);

    result->dirLocal = dirOutLocal;
    result->pdf = pdf;
    result->isDelta = false;
    result->sampledBSDFType = BSDFType_MicrofacetReflection;

    SampledSpectrum F;
    float cosTheta = std::abs(VdotH);
    for (int i = 0; i < NumSpectralSamples; ++i) {
        float fresnelValue = FresnelConductor(cosTheta, eta.values[i], kappa.values[i]);
        F.values[i] = ::vlr::vlr_min(1.0f, fresnelValue);  // Clamp 到 [0,1] 确保能量守恒
    }

    float denom = 4.0f * NdotL * NdotV;
    if (denom > 1e-7f) {
        float spec = D * G / denom;
        for (int i = 0; i < NumSpectralSamples; ++i) {
            result->f.values[i] = coeffR.values[i] * F.values[i] * spec;
        }
    } else {
        result->f = SampledSpectrum::Zero();
    }
}

/// 采样导体微表面反射 BSDF（各向异性 GGX VNDF + FresnelConductor）
CUDA_DEVICE_FUNCTION CUDA_INLINE void sampleMicrofacetReflectionBSDF_Aniso(
    const SampledSpectrum& coeffR,
    const SampledSpectrum& eta,
    const SampledSpectrum& kappa,
    float alphaX, float alphaY,
    const Vector3D& dirInLocal,
    const Normal3D& geomNormalLocal,
    float u0, float u1,
    BSDFSampleResult* result) {

    float NdotL = dot(dirInLocal, geomNormalLocal);
    if (NdotL <= 0.0f) {
        result->pdf = 0.0f;
        result->f = SampledSpectrum::Zero();
        return;
    }

    Vector3D H = sampleGGXVNDF_Aniso(dirInLocal, alphaX, alphaY, u0, u1);

    float VdotH = dot(dirInLocal, H);
    if (VdotH <= 0.0f) {
        result->pdf = 0.0f;
        result->f = SampledSpectrum::Zero();
        return;
    }

    Vector3D dirOutLocal = 2.0f * VdotH * H - dirInLocal;
    float NdotV = dot(dirOutLocal, geomNormalLocal);
    if (NdotV <= 0.0f) {
        result->pdf = 0.0f;
        result->f = SampledSpectrum::Zero();
        return;
    }

    float NdotH = dot(H, geomNormalLocal);
    float HdotX = H.x, HdotY = H.y;
    float LdotX = dirInLocal.x, LdotY = dirInLocal.y;
    float VdotX = dirOutLocal.x, VdotY = dirOutLocal.y;

    float D = GGX_D_Aniso(NdotH, HdotX, HdotY, alphaX, alphaY);
    float G1_l = GGX_G1_Aniso(NdotL, LdotX, LdotY, alphaX, alphaY);
    float G1_v = GGX_G1_Aniso(NdotV, VdotX, VdotY, alphaX, alphaY);
    float G = G1_l * G1_v;

    float VdotH_clamped = ::vlr::vlr_max(VdotH, 1e-6f);
    float pdf = D * G1_l * NdotV / (4.0f * VdotH_clamped);

    result->dirLocal = dirOutLocal;
    result->pdf = pdf;
    result->isDelta = false;
    result->sampledBSDFType = BSDFType_MicrofacetReflection;

    SampledSpectrum F;
    float cosTheta = std::abs(VdotH);
    for (int i = 0; i < NumSpectralSamples; ++i) {
        float fresnelValue = FresnelConductor(cosTheta, eta.values[i], kappa.values[i]);
        F.values[i] = ::vlr::vlr_min(1.0f, fresnelValue);
    }

    float denom = 4.0f * NdotL * NdotV;
    if (denom > 1e-7f) {
        float spec = D * G / denom;
        for (int i = 0; i < NumSpectralSamples; ++i) {
            result->f.values[i] = coeffR.values[i] * F.values[i] * spec;
        }
    } else {
        result->f = SampledSpectrum::Zero();
    }
}

/// MicrofacetReflection BSDF 的 PDF（各向同性）
CUDA_DEVICE_FUNCTION CUDA_INLINE float getMicrofacetReflectionBSDFPDF(
    float roughness,
    const Vector3D& dirInLocal,
    const Vector3D& dirOutLocal,
    const Normal3D& geomNormalLocal) {

    float NdotL = dot(dirInLocal, geomNormalLocal);
    float NdotV = dot(dirOutLocal, geomNormalLocal);
    if (NdotL <= 0.0f || NdotV <= 0.0f)
        return 0.0f;

    Vector3D halfSum = dirInLocal + dirOutLocal;
    float halfLenSq = dot(halfSum, halfSum);
    if (halfLenSq < 1e-12f)
        return 0.0f;
    
    Vector3D halfVec = normalize(halfSum);
    float NdotH = dot(halfVec, geomNormalLocal);
    float VdotH = dot(dirOutLocal, halfVec);
    
    float alpha = roughnessToAlpha(roughness);
    float alpha2 = alpha * alpha;
    float D = GGX_D(NdotH, alpha2);
    // VNDF PDF: 应使用观察方向 G1(V)，即 dirInLocal 的 G1，即 G1(NdotL)
    float G1_v = GGX_G1(NdotL, alpha2);

    float VdotHSafe = ::vlr::vlr_max(VdotH, 1e-6f);
    return D * G1_v * NdotV / (4.0f * VdotHSafe);
}

/// MicrofacetReflection BSDF 的 PDF（各向异性）
CUDA_DEVICE_FUNCTION CUDA_INLINE float getMicrofacetReflectionBSDFPDF_Aniso(
    float alphaX, float alphaY,
    const Vector3D& dirInLocal,
    const Vector3D& dirOutLocal,
    const Normal3D& geomNormalLocal) {

    float NdotL = dot(dirInLocal, geomNormalLocal);
    float NdotV = dot(dirOutLocal, geomNormalLocal);
    if (NdotL <= 0.0f || NdotV <= 0.0f)
        return 0.0f;

    Vector3D halfSum = dirInLocal + dirOutLocal;
    float halfLenSq = dot(halfSum, halfSum);
    if (halfLenSq < 1e-12f)
        return 0.0f;
    
    Vector3D halfVec = normalize(halfSum);
    float NdotH = dot(halfVec, geomNormalLocal);
    float VdotH = dot(dirOutLocal, halfVec);
    float HdotX = halfVec.x, HdotY = halfVec.y;
    float LdotX = dirInLocal.x, LdotY = dirInLocal.y;

    float D = GGX_D_Aniso(NdotH, HdotX, HdotY, alphaX, alphaY);
    float G1_l = GGX_G1_Aniso(NdotL, LdotX, LdotY, alphaX, alphaY);

    float VdotHSafe = ::vlr::vlr_max(VdotH, 1e-6f);
    return D * G1_l * NdotV / (4.0f * VdotHSafe);
}


// ============================================================================
// 8. MicrofacetScattering BSDF（GGX 微表面散射：反射+折射）
// ============================================================================
// 参考 VLR materials.cu MicrofacetBSDF (第1246-1446行)
// - 反射：dirL = 2*dotHV*m - dirV, BSDF = coeff*F*D*G/(4*dirV.z*dirL.z)
// - 折射：dirL = (recRelIOR*dotHV - sqrt(innerRoot))*m - recRelIOR*dirV
// - 折射半向量：m = normalize(-(eEnter*dirV + eExit*dirL))
// - 折射 BSDF 分母：(eEnter*dotHV + eExit*dotHL)^2 * |dirV.z*dirL.z|
// - 折射需每波长单独计算（色散时），并应用 transport mode adjoint
// ============================================================================

// 前向声明
CUDA_DEVICE_FUNCTION CUDA_INLINE float getMicrofacetScatteringBSDFPDF(
    float ior,
    float roughness,
    const Vector3D& dirInLocal,
    const Vector3D& dirOutLocal,
    const Normal3D& geomNormalLocal);

/// 评估微表面散射 BSDF（电介质，反射+折射）
/// transportMode: Radiance=路径追踪, Importance=光追，用于 adjoint 修正
CUDA_DEVICE_FUNCTION CUDA_INLINE SampledSpectrum evaluateMicrofacetScatteringBSDF(
    float ior,
    float roughness,
    const SampledSpectrum& coeff,
    const Vector3D& dirInLocal,
    const Vector3D& dirOutLocal,
    const Normal3D& geomNormalLocal,
    ::vlr::TransportMode transportMode = ::vlr::TransportMode::Radiance) {

    float NdotL = dot(dirInLocal, geomNormalLocal);
    float NdotV = dot(dirOutLocal, geomNormalLocal);
    float dotNVdotNL = NdotL * NdotV;

    // 判断是从外部进入还是从内部出射(参考materials.cu第1388行)
    bool entering = (NdotL >= 0.0f);
    float eEnter = entering ? 1.0f : ior;
    float eExit = entering ? ior : 1.0f;

    float alpha = roughnessToAlpha(roughness);
    float alpha2 = alpha * alpha;

    // 反射：dotNVdotNL > 0（同半球）
    if (dotNVdotNL > 0.0f) {
        Vector3D halfSum = dirInLocal + dirOutLocal;
        float halfLenSq = dot(halfSum, halfSum);
        if (halfLenSq < 1e-12f)
            return SampledSpectrum::Zero();

        Vector3D halfVec = normalize(halfSum);
        float NdotH = dot(halfVec, geomNormalLocal);
        if (NdotH <= 0.0f)
            return SampledSpectrum::Zero();

        float VdotH = dot(dirOutLocal, halfVec);
        float D = GGX_D(NdotH, alpha2);
        float G1_l = GGX_G1(std::abs(NdotL), alpha2);
        float G1_v = GGX_G1(std::abs(NdotV), alpha2);
        float G = G1_l * G1_v;

        float F = FresnelDielectric(std::abs(VdotH), eEnter, eExit);

        float denom = 4.0f * std::abs(dotNVdotNL);
        if (denom < 1e-7f)
            return SampledSpectrum::Zero();

        float spec = D * G * F / denom;
        // 应用透射系数
        return coeff * spec;
    }
    // 折射：dotNVdotNL < 0（异半球）
    else if (dotNVdotNL < 0.0f) {
        // 进入：eEnter=1, eExit=ior；离开：eEnter=ior, eExit=1
        bool entering = (NdotL > 0.0f);
        float eEnter = entering ? 1.0f : ior;
        float eExit = entering ? ior : 1.0f;

        // 折射半向量：m = normalize(-(eEnter*dirV + eExit*dirL))
        // dirV=入射, dirL=出射（透射方向）
        Vector3D dirV = dirInLocal;
        Vector3D dirL = dirOutLocal;
        Vector3D halfVec = normalize(-(eEnter * dirV + eExit * dirL) * (entering ? 1.0f : -1.0f));

        // 每波长分量单独计算（参考 materials.cu 第1355-1366行）
        // 单 IOR 时各波长相同，但保持公式结构一致
        SampledSpectrum ret = SampledSpectrum::Zero();
        for (int wlIdx = 0; wlIdx < NumSpectralSamples; ++wlIdx) {
            float dotHV_wl = dot(dirV, halfVec);
            float dotHL_wl = dot(dirL, halfVec);
            float F_wl = FresnelDielectric(std::abs(dotHV_wl), eEnter, eExit);
            float G1_v = GGX_G1(std::abs(NdotL), alpha2);
            float G1_l = GGX_G1(std::abs(NdotV), alpha2);
            float G_wl = G1_v * G1_l;
            float D_wl = GGX_D(std::abs(dot(halfVec, geomNormalLocal)), alpha2);

            float denomSq = eEnter * dotHV_wl + eExit * dotHL_wl;
            denomSq = denomSq * denomSq;
            if (denomSq < 1e-14f)
                continue;
            ret.values[wlIdx] = std::fabs(dotHV_wl * dotHL_wl) * (1.0f - F_wl) * G_wl * D_wl / denomSq;
        }
        ret /= std::fabs(dotNVdotNL);

        // Transport mode adjoint 修正（参考 materials.cu 第1427-1428行）
        float adjoint = (transportMode == ::vlr::TransportMode::Radiance) ? (eEnter * eEnter) : (eExit * eExit);
        ret = ret * adjoint;

        // 应用透射系数
        return ret * coeff;
    }

    return SampledSpectrum::Zero();
}

/// 计算 GGX VNDF 的 PDF：p(m) = D*G1(v,m)/(4)（用于反射/折射采样）
CUDA_DEVICE_FUNCTION CUDA_INLINE float getGGXVNDFPDF(
    const Vector3D& H,
    const Vector3D& V,
    const Normal3D& geomNormalLocal,
    float alpha2) {
    float dotHV = dot(V, H);
    if (dotHV <= 0.0f) return 0.0f;
    float NdotV = std::abs(dot(V, geomNormalLocal));
    if (NdotV <= 1e-6f) return 0.0f;
    float NdotH = dot(H, geomNormalLocal);
    float D = GGX_D(std::abs(NdotH), alpha2);
    float G1 = GGX_G1(NdotV, alpha2);
    return D * G1 * dotHV / NdotV;
}

/// 采样微表面散射 BSDF（反射+折射）
/// 参考 VLR materials.cu 第1269-1382 行
CUDA_DEVICE_FUNCTION CUDA_INLINE void sampleMicrofacetScatteringBSDF(
    float ior,
    float roughness,
    const SampledSpectrum& coeff,
    const Vector3D& dirInLocal,
    const Normal3D& geomNormalLocal,
    float u0, float u1, float u2,
    BSDFSampleResult* result,
    ::vlr::TransportMode transportMode = ::vlr::TransportMode::Radiance) {

    float NdotL = dot(dirInLocal, geomNormalLocal);
    
    // 判断是从外部进入(entering=true)还是从内部出射(entering=false)
    // 参考materials.cu第1277-1284行
    bool entering = (NdotL >= 0.0f);
    float eEnter = entering ? 1.0f : ior;
    float eExit = entering ? ior : 1.0f;
    float recRelIOR = eEnter / eExit;
    
    // 将方向转换到统一的坐标系(参考第1284行)
    Vector3D dirV = entering ? dirInLocal : -dirInLocal;

#ifdef __CUDA_ARCH__
    // 调试输出:检查采样参数(只输出前10个路径)
    if (blockIdx.x * blockDim.x + threadIdx.x < 10) {
        printf("[MicrofacetScattering Sample pathIdx=%u] entering=%d, NdotL=%.3f, dirV.z=%.3f, ior=%.2f, roughness=%.4f\n",
               blockIdx.x * blockDim.x + threadIdx.x, entering, NdotL, dirV.z, ior, roughness);
        printf("  coeff=(%.3f,%.3f,%.3f), eEnter=%.2f, eExit=%.2f\n",
               coeff.values[0], coeff.values[1], coeff.values[2], eEnter, eExit);
    }
#endif

    float alpha = roughnessToAlpha(roughness);
    float alpha2 = alpha * alpha;
    Vector3D H = sampleGGXVNDF(dirV, alpha, u0, u1);

    float dotHV = dot(dirV, H);
    if (dotHV <= 0.0f) {
        result->pdf = 0.0f;
        result->f = SampledSpectrum::Zero();
        return;
    }

    float F = FresnelDielectric(dotHV, eEnter, eExit);
    float reflectProb = F;
    bool sampleReflection = (u2 < reflectProb);

#ifdef __CUDA_ARCH__
    if (blockIdx.x * blockDim.x + threadIdx.x < 10) {
        printf("  dotHV=%.3f, F=%.3f, reflectProb=%.3f, u2=%.3f, sampleReflection=%d\n",
               dotHV, F, reflectProb, u2, sampleReflection);
    }
#endif

    if (sampleReflection) {
        // 反射：dirL = 2*dotHV*m - dirV（参考第1308行）
        Vector3D dirL = 2.0f * dotHV * H - dirV;
        if (dirL.z * dirV.z <= 0.0f) {
            result->pdf = 0.0f;
            result->f = SampledSpectrum::Zero();
            return;
        }

        result->dirLocal = entering ? dirL : -dirL;
        result->isDelta = false;
        result->sampledBSDFType = BSDFType_MicrofacetScattering;

        // PDF = reflectProb / (4*dotHV) * mPDF（参考第1314-1315行）
        float mPDF = getGGXVNDFPDF(H, dirV, geomNormalLocal, alpha2);
        float dotHV_safe = ::vlr::vlr_max(dotHV, 1e-6f);
        result->pdf = (reflectProb / (4.0f * dotHV_safe)) * mPDF;

        // BSDF = coeff*F*D*G/(4*dirV.z*dirL.z)（参考第1319行）
        float D = GGX_D(std::abs(dot(H, geomNormalLocal)), alpha2);
        float G1_v = GGX_G1(std::abs(dirV.z), alpha2);
        float G1_l = GGX_G1(std::abs(dirL.z), alpha2);
        float G = G1_v * G1_l;
        float denom = 4.0f * std::abs(dirV.z * dirL.z);
        if (denom > 1e-7f) {
            // 应用coeff系数(参考libVLR_reference第1319行)
            result->f = coeff * (F * D * G / denom);
#ifdef __CUDA_ARCH__
            if (blockIdx.x * blockDim.x + threadIdx.x < 10) {
                printf("  [Reflection] dirL.z=%.3f, F=%.3f, D=%.3f, G=%.3f, denom=%.3f\n",
                       dirL.z, F, D, G, denom);
                printf("  BSDF=(%.4f,%.4f,%.4f)\n",
                       result->f.values[0], result->f.values[1], result->f.values[2]);
            }
#endif
        } else {
            result->f = SampledSpectrum::Zero();
        }
    } else {
        // 折射：dirL = (recRelIOR*dotHV - sqrt(innerRoot))*m - recRelIOR*dirV（参考第1341行）
        float innerRoot = 1.0f + recRelIOR * recRelIOR * (dotHV * dotHV - 1.0f);
        if (innerRoot < 0.0f) {
            result->pdf = 0.0f;
            result->f = SampledSpectrum::Zero();
            return;
        }
        Vector3D dirL = (recRelIOR * dotHV - safeSqrt(innerRoot)) * H - recRelIOR * dirV;
        if (dirL.z * dirV.z >= 0.0f) {
            result->pdf = 0.0f;
            result->f = SampledSpectrum::Zero();
            return;
        }

        result->dirLocal = entering ? dirL : -dirL;
        result->isDelta = false;
        result->sampledBSDFType = BSDFType_MicrofacetScattering;

        float dotHL = dot(dirL, H);
        // PDF = (1-reflectProb) / (eEnter*dotHV + eExit*dotHL)^2 * mPDF * eExit^2 * |dotHL|（参考第1348-1349行）
        float denomPdf = eEnter * dotHV + eExit * dotHL;
        float denomPdfSq = denomPdf * denomPdf;
        if (denomPdfSq < 1e-14f) {
            result->pdf = 0.0f;
            result->f = SampledSpectrum::Zero();
            return;
        }
        float mPDF = getGGXVNDFPDF(H, dirV, geomNormalLocal, alpha2);
        result->pdf = (1.0f - reflectProb) / denomPdfSq * mPDF * (eExit * eExit) * std::fabs(dotHL);

        // 直接计算BSDF,不调用evaluate(参考第1354-1370行)
        SampledSpectrum ret = SampledSpectrum::Zero();
        for (int wlIdx = 0; wlIdx < NumSpectralSamples; ++wlIdx) {
            Normal3D m_wl = normalize(-(eEnter * dirV + eExit * dirL) * (entering ? 1.0f : -1.0f));
            float dotHV_wl = dot(dirV, m_wl);
            float dotHL_wl = dot(dirL, m_wl);
            float F_wl = FresnelDielectric(std::abs(dotHV_wl), eEnter, eExit);
            float G1_v = GGX_G1(std::abs(dirV.z), alpha2);
            float G1_l = GGX_G1(std::abs(dirL.z), alpha2);
            float G_wl = G1_v * G1_l;
            float D_wl = GGX_D(std::abs(dot(m_wl, geomNormalLocal)), alpha2);
            float denomBsdf = eEnter * dotHV_wl + eExit * dotHL_wl;
            ret.values[wlIdx] = std::fabs(dotHV_wl * dotHL_wl) * (1.0f - F_wl) * G_wl * D_wl / (denomBsdf * denomBsdf);
        }
        ret /= std::fabs(dirV.z * dirL.z);
        ret *= coeff;
        float adjoint = (transportMode == ::vlr::TransportMode::Radiance) ? (eEnter * eEnter) : (eExit * eExit);
        ret = ret * adjoint;
        result->f = ret;

#ifdef __CUDA_ARCH__
        if (blockIdx.x * blockDim.x + threadIdx.x < 10) {
            printf("  [Refraction] dirL.z=%.3f, dotHL=%.3f, pdf=%.6f\n", dirL.z, dotHL, result->pdf);
            printf("  BSDF=(%.4f,%.4f,%.4f), adjoint=%.2f\n",
                   ret.values[0], ret.values[1], ret.values[2], adjoint);
        }
#endif
    }
}

/// 计算微表面散射 BSDF 的 PDF
/// 参考 VLR materials.cu evaluatePDFInternal 第1448-1508 行
CUDA_DEVICE_FUNCTION CUDA_INLINE float getMicrofacetScatteringBSDFPDF(
    float ior,
    float roughness,
    const Vector3D& dirInLocal,
    const Vector3D& dirOutLocal,
    const Normal3D& geomNormalLocal) {

    float NdotL = dot(dirInLocal, geomNormalLocal);
    float NdotV = dot(dirOutLocal, geomNormalLocal);
    float dotNVdotNL = NdotL * NdotV;

    if (dotNVdotNL == 0.0f)
        return 0.0f;

    float alpha = roughnessToAlpha(roughness);
    float alpha2 = alpha * alpha;

    bool entering = (NdotL > 0.0f);
    float eEnter = entering ? 1.0f : ior;
    float eExit = entering ? ior : 1.0f;

    Normal3D m;
    if (dotNVdotNL > 0.0f) {
        // 反射：halfVector(dirV, dirL)
        Vector3D halfSum = dirInLocal + dirOutLocal;
        float halfLenSq = dot(halfSum, halfSum);
        if (halfLenSq < 1e-12f) return 0.0f;
        m = normalize(halfSum);
    } else {
        // 折射：m = normalize(-(eEnter*dirV + eExit*dirL) * sign)
        m = normalize(-(eEnter * dirInLocal + eExit * dirOutLocal) * (entering ? 1.0f : -1.0f));
    }

    float dotHV = dot(dirInLocal, m);
    if (dotHV <= 0.0f)
        return 0.0f;

    float mPDF = getGGXVNDFPDF(m, dirInLocal, geomNormalLocal, alpha2);
    float F = FresnelDielectric(dotHV, eEnter, eExit);
    float reflectProb = F;

    if (dotNVdotNL > 0.0f) {
        // 反射 PDF：commonPDFTerm * mPDF = (reflectProb / (4*dotHV)) * mPDF
        float dotHV_safe = ::vlr::vlr_max(dotHV, 1e-6f);
        return (reflectProb / (4.0f * dotHV_safe)) * mPDF;
    } else {
        // 折射 PDF：(1-reflectProb) / (eEnter*dotHV + eExit*dotHL)^2 * mPDF * eExit^2 * |dotHL|
        float dotHL = dot(dirOutLocal, m);
        float denomPdf = eEnter * dotHV + eExit * dotHL;
        float denomPdfSq = denomPdf * denomPdf;
        if (denomPdfSq < 1e-14f) return 0.0f;
        return (1.0f - reflectProb) / denomPdfSq * mPDF * (eExit * eExit) * std::fabs(dotHL);
    }
}


// ============================================================================
// 4. 完美镜面 BSDF
// ============================================================================

/// 评估完美镜面 BSDF（Delta 分布，评估时返回 0，需特殊处理）
/// 与原始 VLR SpecularBRDF 一致：coeffR * FresnelConductor(eta,k)
CUDA_DEVICE_FUNCTION CUDA_INLINE SampledSpectrum evaluateSpecularBSDF(
    const SampledSpectrum& coeffR,
    const SampledSpectrum& eta,
    const SampledSpectrum& kappa,
    const Vector3D& dirInLocal,
    const Vector3D& dirOutLocal,
    const Normal3D& geomNormalLocal) {

    Vector3D reflected = 2.0f * dot(dirInLocal, geomNormalLocal) * geomNormalLocal - dirInLocal;
    float cosOut = dot(dirOutLocal, geomNormalLocal);

    if (cosOut <= 0.0f)
        return SampledSpectrum::Zero();

    float diff = std::abs(dot(reflected, dirOutLocal) - 1.0f);
    if (diff < 1e-5f) {
        float cosTheta = std::abs(dot(dirInLocal, geomNormalLocal));
        float F = (eta.values[0] * eta.values[0] + kappa.values[0] * kappa.values[0] < 1e-10f)
            ? 1.0f : ::vlr::vlr_min(1.0f, FresnelConductor(cosTheta, eta.values[0], kappa.values[0]));
        SampledSpectrum ret;
        for (int i = 0; i < NumSpectralSamples; ++i) {
            float Fi = (eta.values[i] * eta.values[i] + kappa.values[i] * kappa.values[i] < 1e-10f)
                ? 1.0f : ::vlr::vlr_min(1.0f, FresnelConductor(cosTheta, eta.values[i], kappa.values[i]));
            ret.values[i] = coeffR.values[i] * Fi;
        }
        return ret;
    }
    return SampledSpectrum::Zero();
}

/// 采样完美镜面：反射方向，PDF 为 1
/// 与原始 VLR SpecularBRDF 一致：f = coeffR * Fresnel(cosTheta) / |cos|，pdf = 1
CUDA_DEVICE_FUNCTION CUDA_INLINE void sampleSpecularBSDF(
    const SampledSpectrum& coeffR,
    const SampledSpectrum& eta,
    const SampledSpectrum& kappa,
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
    result->sampledBSDFType = BSDFType_Specular;
    result->isDelta = true;
    result->pdf = 1.0f;

    // f = coeffR * F / |cos|，与原始 VLR SpecularBRDF 一致
    float cosAbs = std::abs(NdotL);
    float cosSafe = (cosAbs < 1e-6f) ? 1e-6f : cosAbs;
    bool useFresnel = (eta.values[0] * eta.values[0] + kappa.values[0] * kappa.values[0] >= 1e-10f);
    for (int i = 0; i < NumSpectralSamples; ++i) {
        float F = 1.0f;
        if (useFresnel || (eta.values[i] * eta.values[i] + kappa.values[i] * kappa.values[i] >= 1e-10f))
            F = FresnelConductor(cosAbs, eta.values[i], kappa.values[i]);
        result->f.values[i] = coeffR.values[i] * F / cosSafe;
    }
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
// Front-face aware variants.
// These assume geomNormalLocal has been faceforwarded to the incident side,
// and use the original front/back information to choose etaI/etaT.
CUDA_DEVICE_FUNCTION CUDA_INLINE SampledSpectrum evaluateSpecularTransmissionBSDF_FrontFace(
    float ior, const SampledSpectrum& transmittance,
    const Vector3D& dirInLocal, const Vector3D& dirOutLocal,
    const Normal3D& geomNormalLocal,
    bool frontFace);

CUDA_DEVICE_FUNCTION CUDA_INLINE void sampleSpecularTransmissionBSDF_FrontFace(
    float etaI, float etaT, float dispersionStrength,
    const SampledSpectrum& transmittance,
    const WavelengthSamples* wls, bool singleWlSelected,
    const Vector3D& dirInLocal, const Normal3D& geomNormalLocal,
    bool frontFace,
    TransportMode mode,
    float u0, float u1,
    BSDFSampleResult* result);

CUDA_DEVICE_FUNCTION CUDA_INLINE float getSpecularTransmissionBSDFPDF_FrontFace(
    float ior,
    const Vector3D& dirInLocal, const Vector3D& dirOutLocal,
    const Normal3D& geomNormalLocal,
    bool frontFace);

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

/// 评估完美镜面透射 BSDF（支持反射+折射）
/// 参考libVLR_reference SpecularBSDF::evaluateInternal
/// 正确处理进入/离开：cosThetaI<0 表示从外进入(etaI=1,etaT=ior)，cosThetaI>0 表示从内离开(etaI=ior,etaT=1)
CUDA_DEVICE_FUNCTION CUDA_INLINE SampledSpectrum evaluateSpecularTransmissionBSDF(
    float ior, const SampledSpectrum& transmittance,
    const Vector3D& dirInLocal, const Vector3D& dirOutLocal,
    const Normal3D& geomNormalLocal) {
    
    float cosThetaI = dot(dirInLocal, geomNormalLocal);
    float cosOut = dot(dirOutLocal, geomNormalLocal);
    
    // 判断进入/离开
    bool entering = (cosThetaI >= 0.0f);
    float etaI, etaT;
    Normal3D nEff;
    if (entering) {
        nEff = geomNormalLocal;
        etaI = 1.0f;
        etaT = ior;
    } else {
        nEff = Vector3D(-geomNormalLocal.x, -geomNormalLocal.y, -geomNormalLocal.z);
        etaI = ior;
        etaT = 1.0f;
    }
    
    float F = FresnelDielectric(std::abs(cosThetaI), etaI, etaT);
    float etaRatio = etaI / etaT;
    
    // 检查是否为反射方向(同半球)
    if (cosThetaI * cosOut > 0.0f) {
        // 反射: 检查是否为镜面反射方向
        Vector3D dirReflected = 2.0f * cosThetaI * geomNormalLocal - dirInLocal;
        float diff = std::abs(dot(dirReflected, dirOutLocal) - 1.0f);
        if (diff < 1e-5f) {
            float cosAbs = std::abs(cosThetaI);
            if (cosAbs < 1e-6f) return SampledSpectrum::Zero();
            // 反射: f = transmittance * F / |cos|
            // 参考libVLR_reference SpecularBSDF line 925
            return transmittance * (F / cosAbs);
        }
        return SampledSpectrum::Zero();
    }
    
    // 检查是否为折射方向(异半球)
    Vector3D wt;
    if (!refract(dirInLocal, nEff, etaRatio, &wt))
        return SampledSpectrum::Zero();
    float diff = std::abs(dot(wt, dirOutLocal) - 1.0f);
    if (diff < 1e-5f) {
        // 折射: f = transmittance * (1-F)
        // 参考libVLR_reference SpecularBSDF line 941
        return transmittance * (1.0f - F);
    }
    return SampledSpectrum::Zero();
}

/// 采样完美镜面透射（支持波长相关折射率）
/// 玻璃材质：根据 Fresnel 方程随机选择反射或透射
/// transmittance: 透射系数（有色玻璃），(1,1,1) 为透明
/// wls: 波长采样，当 singleWlSelected 时用 selectedLambda 对应波长计算 IOR
CUDA_DEVICE_FUNCTION CUDA_INLINE void sampleSpecularTransmissionBSDF(
    float etaI, float etaT, float dispersionStrength,
    const SampledSpectrum& transmittance,
    const WavelengthSamples* wls, bool singleWlSelected,
    const Vector3D& dirInLocal, const Normal3D& geomNormalLocal,
    float u0, float /*u1*/,
    BSDFSampleResult* result) {

    // 单波长选择：使用选定波长的 IOR
    float etaT_eff = etaT;
    if (wls && singleWlSelected && dispersionStrength > 0.0f) {
        uint32_t idx = wls->selectedLambdaIndex() % NumSpectralSamples;
        float lambda = wls->lambdas[idx];
        etaT_eff = iorAtWavelength(lambda, etaT, dispersionStrength);
    }

    float cosThetaI = dot(dirInLocal, geomNormalLocal);
    // VLR 原版约定：entering = dirLocal.z >= 0（指向 +z 半球）
    // 由于 geomNormalLocal 通常指向 +z，cosThetaI >= 0 表示 entering
    // 进入：etaI -> etaT（外部 -> 内部），eEnter=etaI, eExit=etaT
    // 离开：etaT -> etaI（内部 -> 外部），eEnter=etaT, eExit=etaI
    bool entering = (cosThetaI >= 0.0f);
    float etaRatio;
    Normal3D nEff;
    float etaIncident, etaTransmitted;
    if (entering) {
        nEff = geomNormalLocal;
        etaRatio = etaI / etaT_eff;  // 1/2.4 进入玻璃
        etaIncident = etaI;
        etaTransmitted = etaT_eff;
    } else {
        nEff = Vector3D(-geomNormalLocal.x, -geomNormalLocal.y, -geomNormalLocal.z);
        etaRatio = etaT_eff / etaI;  // 2.4/1 离开玻璃
        etaIncident = etaT_eff;
        etaTransmitted = etaI;
    }
    float F = FresnelDielectric(std::abs(cosThetaI), etaIncident, etaTransmitted);

    Vector3D wt;
    bool canRefract = refract(dirInLocal, nEff, etaRatio, &wt);

    // 全内反射：必须反射，能量守恒 f = F/|cos| = 1/|cos|，pdf = 1（反射不经过介质，无透射系数）
    if (!canRefract) {
        float NdotL = dot(dirInLocal, geomNormalLocal);
        result->dirLocal = 2.0f * NdotL * geomNormalLocal - dirInLocal;
        float cosAbs = std::abs(NdotL);
        float fVal = (cosAbs > 1e-6f) ? (1.0f / cosAbs) : 0.0f;
        for (int i = 0; i < NumSpectralSamples; ++i)
            result->f.values[i] = fVal;
        result->pdf = 1.0f;
        result->sampledBSDFType = BSDFType_Specular;
        result->isDelta = true;
        return;
    }

    // 根据 Fresnel 概率选择反射或透射（离散采样）
    // 参考libVLR_reference SpecularBSDF::sampleInternal line 1305-1330
    // 反射: f = coeff*F/|cos|, pdf = F, throughput *= f*cos/pdf = coeff
    // 透射: f = coeff*(1-F)*(etaI/etaT)^2/|cosT|, pdf = (1-F)*(etaI/etaT)^2, throughput *= f*cosT/pdf = coeff
    if (u0 < F) {
        // 反射（应用transmittance系数,参考line 1319）
        float NdotL = dot(dirInLocal, geomNormalLocal);
        result->dirLocal = 2.0f * NdotL * geomNormalLocal - dirInLocal;
        float cosAbs = std::abs(NdotL);
        if (cosAbs < 1e-6f) {
            result->pdf = 0.0f;
            result->f = SampledSpectrum::Zero();
            return;
        }
        // f = transmittance * F / |cos|
        result->f = transmittance * (F / cosAbs);
        result->pdf = F;
        result->sampledBSDFType = BSDFType_Specular;
        result->isDelta = true;
    } else {
        // 透射（严格按照 VLR 原版 materials.cu SpecularBSDF::sampleInternal line 864-901）
        float sin2ThetaI = ::vlr::vlr_max(0.0f, 1.0f - cosThetaI * cosThetaI);
        float sin2ThetaT = etaRatio * etaRatio * sin2ThetaI;
        float cosThetaT = safeSqrt(1.0f - sin2ThetaT);
        
        // VLR 原版 line 830: dirV = entering ? query.dirLocal : -query.dirLocal
        // dirV 总是指向 +z 半球（dirV.z >= 0）
        Vector3D dirV = entering ? dirInLocal : Vector3D(-dirInLocal.x, -dirInLocal.y, -dirInLocal.z);
        
        // VLR 原版 line 874: dirL = (recRelIOR * -dirV.x, recRelIOR * -dirV.y, -cosExit)
        Vector3D dirL = Vector3D(etaRatio * -dirV.x, etaRatio * -dirV.y, -cosThetaT);
        
        // VLR 原版 line 875: result->dirLocal = entering ? dirL : -dirL
        if (!entering) {
            dirL = Vector3D(-dirL.x, -dirL.y, -dirL.z);
        }
        
        float etaRatio2 = (etaIncident * etaIncident) / (etaTransmitted * etaTransmitted);
        float fVal = (cosThetaT > 1e-8f) ? ((1.0f - F) * etaRatio2 / cosThetaT) : 0.0f;
        for (int i = 0; i < NumSpectralSamples; ++i)
            result->f.values[i] = transmittance.values[i] * fVal;
        result->dirLocal = dirL;
        result->pdf = (1.0f - F) * etaRatio2;
        result->sampledBSDFType = BSDFType_SpecularTransmission;
        result->isDelta = true;
    }
}

/// SpecularTransmission PDF（Delta）
/// 正确处理进入/离开
CUDA_DEVICE_FUNCTION CUDA_INLINE float getSpecularTransmissionBSDFPDF(
    float ior,
    const Vector3D& dirInLocal, const Vector3D& dirOutLocal,
    const Normal3D& geomNormalLocal) {
    float cosThetaI = dot(dirInLocal, geomNormalLocal);
    float etaRatio;
    Normal3D nEff;
    if (cosThetaI < 0.0f) {
        nEff = Vector3D(-geomNormalLocal.x, -geomNormalLocal.y, -geomNormalLocal.z);
        etaRatio = 1.0f / ior;
    } else {
        nEff = geomNormalLocal;
        etaRatio = ior;
    }
    Vector3D wt;
    if (!refract(dirInLocal, nEff, etaRatio, &wt))
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
    Vector3D halfSum = -dirInLocal + eta * dirOutLocal;
    if (dot(halfSum, halfSum) < 1e-12f) return SampledSpectrum::Zero();
    Vector3D halfVec = normalize(halfSum);
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
        // 反射分量：与原始 VLR MicrofacetBSDF 一致 f = coeff * F * D * G / (4 * NdotV * NdotL)
        float VdotH = dot(V, H);
        result->dirLocal = 2.0f * VdotH * H - V;
        float NdotV = std::abs(dot(result->dirLocal, geomNormalLocal));
        float NdotL = std::abs(dot(dirInLocal, geomNormalLocal));
        float alpha2 = alpha * alpha;
        float NdotH = std::abs(dot(H, geomNormalLocal));
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

    Vector3D halfSum = dirInLocal + dirOutLocal;
    if (dot(halfSum, halfSum) < 1e-12f) return SampledSpectrum::Zero();
    Vector3D halfVec = normalize(halfSum);
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
/// PDF 需乘以选择概率：pdf = probSelection * componentPDF
CUDA_DEVICE_FUNCTION CUDA_INLINE void sampleFresnelBlendBSDF(
    const SampledSpectrum& diffuse, const SampledSpectrum& specular,
    float roughness,
    const Vector3D& dirInLocal, const Normal3D& geomNormalLocal,
    float u0, float u1, float u2,
    BSDFSampleResult* result) {

    float NdotL = dot(dirInLocal, geomNormalLocal);
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

    float NdotL = dot(dirInLocal, geomNormalLocal);
    float NdotV = dot(dirOutLocal, geomNormalLocal);
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

    float NdotL = dot(dirInLocal, geomNormalLocal);
    float NdotV = dot(dirOutLocal, geomNormalLocal);
    if (NdotV <= 0.0f) return SampledSpectrum::Zero();
    if (NdotL <= 0.0f) NdotL = -NdotL;  // 允许光线从任意方向入射

    Vector3D halfSum = dirInLocal + dirOutLocal;
    float halfLenSq = dot(halfSum, halfSum);
    if (halfLenSq < 1e-12f) return SampledSpectrum::Zero();
    Vector3D halfVec = normalize(halfSum);
    float NdotH = dot(halfVec, geomNormalLocal);
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

    float NdotL = dot(dirInLocal, geomNormalLocal);
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
        float NdotV = dot(dirOut, geomNormalLocal);
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
#if defined(__CUDA_ARCH__) && defined(VLR_DEBUG_MATERIAL)
        if (threadIdx.x == 0 && blockIdx.x == 0) {
            printf("[GPU sampleBSDFWithU2] Entering SpecularTransmission branch\n");
        }
#endif
        float ior, disp;
        getTransmissionParams(matDesc, &ior, &disp);
#if defined(__CUDA_ARCH__) && defined(VLR_DEBUG_MATERIAL)
        if (threadIdx.x == 0 && blockIdx.x == 0) {
            printf("  ior=%.3f, dispersion=%.3f\n", ior, disp);
        }
#endif
        SampledSpectrum transmittance;
        getEffectiveLambertAlbedo(matDesc, ctx.texturedParams, &transmittance);
        sampleSpecularTransmissionBSDF_FrontFace(1.0f, ior, disp, transmittance, wls, singleWl,
            dirInLocal, ctx.geomNormalLocal, ctx.surfPt->isFrontFace, 
            ::vlr::TransportMode::Radiance, u0, u1, result);
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
            result->pdf *= weight;  // 选择概率
        } else {
            sampleLambertBSDF(alb0, dirInLocal, ctx.geomNormalLocal, u0, u1, result);
            result->pdf *= (1.0f - weight);  // 选择概率
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

// ============================================================================
// Front-face aware specular transmission definitions
// ============================================================================

CUDA_DEVICE_FUNCTION CUDA_INLINE SampledSpectrum evaluateSpecularTransmissionBSDF_FrontFace(
    float ior, const SampledSpectrum& transmittance,
    const Vector3D& dirInLocal, const Vector3D& dirOutLocal,
    const Normal3D& geomNormalLocal,
    bool frontFace) {
    (void)geomNormalLocal;
    // Convention: dirInLocal is the outgoing direction (wo) in the local shading frame (z = shading normal).
    // Return a non-zero value only when dirOutLocal matches the delta direction.
    Normal3D shadingNormalLocal(0.0f, 0.0f, 1.0f);
    float cosThetaO = dirInLocal.z;
    float cosOut = dirOutLocal.z;
    float cosAbs = std::abs(cosThetaO);

    float etaIncident = frontFace ? 1.0f : ior;
    float etaTransmitted = frontFace ? ior : 1.0f;
    float etaRatio = etaIncident / etaTransmitted;

    float F = FresnelDielectric(cosAbs, etaIncident, etaTransmitted);

    // Reflection: wo and wi lie on the same hemisphere.
    if (cosThetaO * cosOut > 0.0f) {
        Vector3D dirReflected(-dirInLocal.x, -dirInLocal.y, dirInLocal.z);
        float diff = std::abs(dot(dirReflected, dirOutLocal) - 1.0f);
        if (diff < 1e-5f) {
            if (cosAbs < 1e-6f) return SampledSpectrum::Zero();
            return transmittance * (F / cosAbs);
        }
        return SampledSpectrum::Zero();
    }

    // Transmission: match reference implementation
    float sin2ThetaO = ::vlr::vlr_max(0.0f, 1.0f - cosThetaO * cosThetaO);
    float sin2ThetaT = etaRatio * etaRatio * sin2ThetaO;
    if (sin2ThetaT >= 1.0f) return SampledSpectrum::Zero();
    float cosThetaT = safeSqrt(1.0f - sin2ThetaT);
    Vector3D wt = etaRatio * -dirInLocal + (etaRatio * cosThetaO - cosThetaT) * shadingNormalLocal;
    float diff = std::abs(dot(wt, dirOutLocal) - 1.0f);
    if (diff < 1e-5f) {
        float cosTAbs = std::abs(wt.z);
        if (cosTAbs < 1e-8f) return SampledSpectrum::Zero();
        
        // Reference formula: coeff * (1-F) * squeezeFactor / |cos(theta_t)|
        // Note: For evaluate, we assume Radiance mode (camera path tracing)
        float squeezeFactor = (etaIncident * etaIncident) / (etaTransmitted * etaTransmitted);
        return transmittance * ((1.0f - F) * squeezeFactor / cosTAbs);
    }
    return SampledSpectrum::Zero();
}

CUDA_DEVICE_FUNCTION CUDA_INLINE void sampleSpecularTransmissionBSDF_FrontFace(
    float etaI, float etaT, float dispersionStrength,
    const SampledSpectrum& transmittance,
    const WavelengthSamples* wls, bool singleWlSelected,
    const Vector3D& dirInLocal, const Normal3D& geomNormalLocal,
    bool frontFace,
    TransportMode mode,
    float u0, float /*u1*/,
    BSDFSampleResult* result) {
    float etaT_eff = etaT;
    if (wls && singleWlSelected && dispersionStrength > 0.0f) {
        uint32_t idx = wls->selectedLambdaIndex() % NumSpectralSamples;
        float lambda = wls->lambdas[idx];
        etaT_eff = iorAtWavelength(lambda, etaT, dispersionStrength);
    }

    (void)geomNormalLocal;
    Normal3D shadingNormalLocal(0.0f, 0.0f, 1.0f);
    // Convention: dirInLocal is the outgoing direction (wo) in the local shading frame.
    float cosThetaO = dirInLocal.z;
    float cosAbs = std::abs(cosThetaO);

    float etaIncident = frontFace ? etaI : etaT_eff;
    float etaTransmitted = frontFace ? etaT_eff : etaI;
    float etaRatio = etaIncident / etaTransmitted;

    float F = FresnelDielectric(cosAbs, etaIncident, etaTransmitted);

    // Compute transmission direction and detect TIR.
    float sin2ThetaO = ::vlr::vlr_max(0.0f, 1.0f - cosThetaO * cosThetaO);
    float sin2ThetaT = etaRatio * etaRatio * sin2ThetaO;
    bool canRefract = (sin2ThetaT < 1.0f);
    float cosThetaT = canRefract ? safeSqrt(1.0f - sin2ThetaT) : 0.0f;
    Vector3D wt = canRefract ?
        (etaRatio * -dirInLocal + (etaRatio * cosThetaO - cosThetaT) * shadingNormalLocal) :
        Vector3D(0.0f, 0.0f, 0.0f);

    if (!canRefract) {
        result->dirLocal = Vector3D(-dirInLocal.x, -dirInLocal.y, dirInLocal.z);
        float fVal = (cosAbs > 1e-6f) ? (1.0f / cosAbs) : 0.0f;
        for (int i = 0; i < NumSpectralSamples; ++i)
            result->f.values[i] = fVal;
        result->pdf = 1.0f;
        result->sampledBSDFType = BSDFType_Specular;
        result->isDelta = true;
        return;
    }

    // Sample reflection or transmission based on Fresnel
    if (u0 < F) {
        // Reflection
        result->dirLocal = Vector3D(-dirInLocal.x, -dirInLocal.y, dirInLocal.z);
        if (cosAbs < 1e-6f) {
            result->pdf = 0.0f;
            result->f = SampledSpectrum::Zero();
            return;
        }
        result->f = transmittance * (F / cosAbs);
        result->pdf = F;
        result->sampledBSDFType = BSDFType_Specular;
        result->isDelta = true;
    } else {
        // Transmission
        float cosTAbs = std::abs(wt.z);
        if (cosTAbs < 1e-8f) {
            result->pdf = 0.0f;
            result->f = SampledSpectrum::Zero();
            return;
        }
        
        // Base BSDF value: coeff * (1 - F)
        result->f = transmittance * (1.0f - F);
        
        // Apply non-symmetric scattering correction for radiance transport
        // Reference: squeezeFactor = (eEnter/eExit)^2 in Radiance mode
        float squeezeFactor = 1.0f;
        if (mode == ::vlr::TransportMode::Radiance) {
            squeezeFactor = (etaIncident * etaIncident) / (etaTransmitted * etaTransmitted);
        }
        
        // Final BSDF = coeff * (1-F) * squeezeFactor / |cos(theta_t)|
        result->f = result->f * (squeezeFactor / cosTAbs);
        result->dirLocal = wt;
        // PDF also needs squeezeFactor correction (reference: line 898)
        result->pdf = (1.0f - F) * squeezeFactor;
        result->sampledBSDFType = BSDFType_SpecularTransmission;
        result->isDelta = true;
    }
}

CUDA_DEVICE_FUNCTION CUDA_INLINE float getSpecularTransmissionBSDFPDF_FrontFace(
    float ior,
    const Vector3D& dirInLocal, const Vector3D& dirOutLocal,
    const Normal3D& geomNormalLocal,
    bool frontFace) {
    (void)geomNormalLocal;
    Normal3D shadingNormalLocal(0.0f, 0.0f, 1.0f);
    float cosThetaO = dirInLocal.z;
    float etaIncident = frontFace ? 1.0f : ior;
    float etaTransmitted = frontFace ? ior : 1.0f;
    float etaRatio = etaIncident / etaTransmitted;

    float sin2ThetaO = ::vlr::vlr_max(0.0f, 1.0f - cosThetaO * cosThetaO);
    float sin2ThetaT = etaRatio * etaRatio * sin2ThetaO;
    if (sin2ThetaT >= 1.0f)
        return 0.0f;
    float cosThetaT = safeSqrt(1.0f - sin2ThetaT);
    Vector3D wt = etaRatio * -dirInLocal + (etaRatio * cosThetaO - cosThetaT) * shadingNormalLocal;

    float diff = std::abs(dot(wt, dirOutLocal) - 1.0f);
    return (diff < 1e-5f) ? 1.0f : 0.0f;
}

} // namespace shared
} // namespace vlr
