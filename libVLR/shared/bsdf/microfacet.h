#pragma once

#include "bsdf_types.h"

namespace vlr {
namespace shared {

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
    tan2 = ::vlr::vlr_min(tan2, 1e10f);  // 防止 tan2 过大
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

    float lensq = Vh.x * Vh.x + Vh.y * Vh.y;
    Vector3D T1 = (lensq > 1e-10f)
        ? Vector3D(-Vh.y, Vh.x, 0.0f) * (1.0f / safeSqrt(lensq))
        : Vector3D(1, 0, 0);
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

} // namespace shared
} // namespace vlr
