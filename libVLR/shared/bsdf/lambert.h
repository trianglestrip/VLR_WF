#pragma once

#include "bsdf_types.h"

namespace vlr {
namespace shared {

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

} // namespace shared
} // namespace vlr
