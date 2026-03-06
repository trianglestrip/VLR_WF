// ============================================================================
// VLR 材质系统 - BSDF 公共函数
//
// 本文件实现 BSDF 的评估、采样和 PDF 计算，为 ProcessHits 和 SampleBSDF
// kernel 提供核心材质计算能力。支持 Diffuse（Lambert）、Glossy（GGX）、Specular。
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
    return std::sqrt(std::max(x, 0.0f));
}

/// 将粗糙度转换为 GGX alpha 参数（Beckmann-style: alpha = roughness^2）
CUDA_DEVICE_FUNCTION CUDA_INLINE float roughnessToAlpha(float roughness) {
    float r = std::max(roughness, 0.001f);
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

/// Schlick Fresnel 近似
CUDA_DEVICE_FUNCTION CUDA_INLINE float SchlickFresnel(
    float cosTheta, float F0) {
    float t = 1.0f - cosTheta;
    float t5 = t * t * t * t * t;
    return F0 + (1.0f - F0) * t5;
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
    Vector3D Vh = normalize(Vector3D(alpha * V.x, alpha * V.y, std::max(V.z, 1e-6f)));

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
    t2 = (1.0f - s) * safeSqrt(std::max(0.0f, 1.0f - t1 * t1)) + s * t2;

    Vector3D Nh = t1 * T1 + t2 * T2 + safeSqrt(std::max(0.0f, 1.0f - t1 * t1 - t2 * t2)) * Vh;

    // 变换回切线空间
    Vector3D H = normalize(Vector3D(alpha * Nh.x, alpha * Nh.y, std::max(0.0f, Nh.z)));
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

    float VdotH_clamped = std::max(VdotH, 1e-6f);
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
// 5. 统一 BSDF 接口（分发给具体实现）
// ============================================================================

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
    default:
        return SampledSpectrum::Zero();
    }
}

/// 根据材质类型采样 BSDF
CUDA_DEVICE_FUNCTION CUDA_INLINE void sampleBSDF(
    const BSDFContext& ctx,
    const Vector3D& dirInLocal,
    float u0, float u1,
    BSDFSampleResult* result) {

    const SurfaceMaterialDescriptor& matDesc = *ctx.matDesc;
    BSDFType type = getBSDFType(matDesc);

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
