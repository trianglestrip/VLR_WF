#pragma once

#include "fresnel.h"
#include "microfacet.h"

namespace vlr {
namespace shared {

// ============================================================================
// GGX 电介质 BSDF（Schlick Fresnel，金属工作流）
// ============================================================================

/// 评估 GGX 镜面 BSDF
CUDA_DEVICE_FUNCTION CUDA_INLINE SampledSpectrum evaluateGGXBSDF(
    const SampledSpectrum& reflectance,
    float roughness,
    const Vector3D& dirInLocal,
    const Vector3D& dirOutLocal,
    const Normal3D& geomNormalLocal) {

    float NdotL = dirInLocal.z;
    float NdotV = dirOutLocal.z;

    if (NdotL <= 0.0f || NdotV <= 0.0f)
        return SampledSpectrum::Zero();

    Vector3D halfSum = dirInLocal + dirOutLocal;
    float halfLenSq = dot(halfSum, halfSum);
    if (halfLenSq < 1e-12f)
        return SampledSpectrum::Zero();  // 掠射角：half 接近零向量
    Vector3D halfVec = normalize(halfSum);
    float NdotH = halfVec.z;

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

    // 构建 Vh 的正交基（Heitz 2018 标准实现）
    float lensq = Vh.x * Vh.x + Vh.y * Vh.y;
    Vector3D T1 = (lensq > 1e-10f)
        ? Vector3D(-Vh.y, Vh.x, 0.0f) * (1.0f / safeSqrt(lensq))
        : Vector3D(1, 0, 0);
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

    float NdotL = dirInLocal.z;
    float NdotV = dirOutLocal.z;

    if (NdotV <= 0.0f) {
        result->pdf = 0.0f;
        result->f = SampledSpectrum::Zero();
        return;
    }

    float NdotH = H.z;

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

    float NdotL = dirInLocal.z;
    float NdotV = dirOutLocal.z;

    if (NdotL <= 0.0f || NdotV <= 0.0f)
        return 0.0f;

    Vector3D halfSum = dirInLocal + dirOutLocal;
    float halfLenSq = dot(halfSum, halfSum);
    if (halfLenSq < 1e-12f)
        return 0.0f;
    Vector3D halfVec = normalize(halfSum);
    float NdotH = halfVec.z;
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

    float NdotL = dirInLocal.z;
    float NdotV = dirOutLocal.z;

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
    float NdotH = halfVec.z;
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

    float NdotL = dirInLocal.z;
    float NdotV = dirOutLocal.z;
    if (NdotL <= 0.0f || NdotV <= 0.0f)
        return SampledSpectrum::Zero();

    Vector3D halfSum = dirInLocal + dirOutLocal;
    float halfLenSq = dot(halfSum, halfSum);
    if (halfLenSq < 1e-12f)
        return SampledSpectrum::Zero();
    
    Vector3D halfVec = normalize(halfSum);
    float NdotH = halfVec.z;
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
/// dirInLocal = V (观察方向, 入射光反方向), dirOutLocal = L (采样反射方向)
CUDA_DEVICE_FUNCTION CUDA_INLINE void sampleMicrofacetReflectionBSDF(
    const SampledSpectrum& coeffR,
    const SampledSpectrum& eta,
    const SampledSpectrum& kappa,
    float roughness,
    const Vector3D& dirInLocal,
    const Normal3D& geomNormalLocal,
    float u0, float u1,
    BSDFSampleResult* result) {

    float NdotV = dirInLocal.z;
    if (NdotV <= 0.0f) {
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
    float NdotL = dirOutLocal.z;
    if (NdotL <= 0.0f) {
        result->pdf = 0.0f;
        result->f = SampledSpectrum::Zero();
        return;
    }

    float NdotH = H.z;
    float alpha2 = alpha * alpha;
    float D = GGX_D(NdotH, alpha2);
    float G1_V = GGX_G1(NdotV, alpha2);
    float G1_L = GGX_G1(NdotL, alpha2);

    // VNDF PDF: pdf(L) = D(H) * G1(V) / (4 * N·V)
    float NdotV_clamped = ::vlr::vlr_max(NdotV, 1e-6f);
    float pdf = D * G1_V / (4.0f * NdotV_clamped);

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

    // f = coeffR * F * D * G / (4 * N·V * N·L)
    float denom = 4.0f * NdotV * NdotL;
    if (denom > 1e-7f) {
        float spec = D * G1_V * G1_L / denom;
        for (int i = 0; i < NumSpectralSamples; ++i) {
            result->f.values[i] = coeffR.values[i] * F.values[i] * spec;
        }
    } else {
        result->f = SampledSpectrum::Zero();
    }
}

/// 采样导体微表面反射 BSDF（各向异性 GGX VNDF + FresnelConductor）
/// dirInLocal = V (观察方向), dirOutLocal = L (采样反射方向)
CUDA_DEVICE_FUNCTION CUDA_INLINE void sampleMicrofacetReflectionBSDF_Aniso(
    const SampledSpectrum& coeffR,
    const SampledSpectrum& eta,
    const SampledSpectrum& kappa,
    float alphaX, float alphaY,
    const Vector3D& dirInLocal,
    const Normal3D& geomNormalLocal,
    float u0, float u1,
    BSDFSampleResult* result) {

    float NdotV = dirInLocal.z;
    if (NdotV <= 0.0f) {
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
    float NdotL = dirOutLocal.z;
    if (NdotL <= 0.0f) {
        result->pdf = 0.0f;
        result->f = SampledSpectrum::Zero();
        return;
    }

    float NdotH = H.z;
    float HdotX = H.x, HdotY = H.y;
    float VdotX = dirInLocal.x, VdotY = dirInLocal.y;
    float LdotX = dirOutLocal.x, LdotY = dirOutLocal.y;

    float D = GGX_D_Aniso(NdotH, HdotX, HdotY, alphaX, alphaY);
    float G1_V = GGX_G1_Aniso(NdotV, VdotX, VdotY, alphaX, alphaY);
    float G1_L = GGX_G1_Aniso(NdotL, LdotX, LdotY, alphaX, alphaY);

    // VNDF PDF: pdf(L) = D(H) * G1(V) / (4 * N·V)
    float NdotV_clamped = ::vlr::vlr_max(NdotV, 1e-6f);
    float pdf = D * G1_V / (4.0f * NdotV_clamped);

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

    // f = coeffR * F * D * G / (4 * N·V * N·L)
    float denom = 4.0f * NdotV * NdotL;
    if (denom > 1e-7f) {
        float spec = D * G1_V * G1_L / denom;
        for (int i = 0; i < NumSpectralSamples; ++i) {
            result->f.values[i] = coeffR.values[i] * F.values[i] * spec;
        }
    } else {
        result->f = SampledSpectrum::Zero();
    }
}

/// MicrofacetReflection BSDF 的 PDF（各向同性）
/// dirInLocal = V (观察方向), dirOutLocal = L (反射方向)
CUDA_DEVICE_FUNCTION CUDA_INLINE float getMicrofacetReflectionBSDFPDF(
    float roughness,
    const Vector3D& dirInLocal,
    const Vector3D& dirOutLocal,
    const Normal3D& geomNormalLocal) {

    float NdotV = dirInLocal.z;
    float NdotL = dirOutLocal.z;
    if (NdotV <= 0.0f || NdotL <= 0.0f)
        return 0.0f;

    Vector3D halfSum = dirInLocal + dirOutLocal;
    float halfLenSq = dot(halfSum, halfSum);
    if (halfLenSq < 1e-12f)
        return 0.0f;

    Vector3D halfVec = normalize(halfSum);
    float NdotH = halfVec.z;

    float alpha = roughnessToAlpha(roughness);
    float alpha2 = alpha * alpha;
    float D = GGX_D(NdotH, alpha2);
    // VNDF PDF: pdf(L) = D(H) * G1(V) / (4 * N·V)
    float G1_V = GGX_G1(NdotV, alpha2);

    float NdotV_clamped = ::vlr::vlr_max(NdotV, 1e-6f);
    return D * G1_V / (4.0f * NdotV_clamped);
}

/// MicrofacetReflection BSDF 的 PDF（各向异性）
/// dirInLocal = V (观察方向), dirOutLocal = L (反射方向)
CUDA_DEVICE_FUNCTION CUDA_INLINE float getMicrofacetReflectionBSDFPDF_Aniso(
    float alphaX, float alphaY,
    const Vector3D& dirInLocal,
    const Vector3D& dirOutLocal,
    const Normal3D& geomNormalLocal) {

    float NdotV = dirInLocal.z;
    float NdotL = dirOutLocal.z;
    if (NdotV <= 0.0f || NdotL <= 0.0f)
        return 0.0f;

    Vector3D halfSum = dirInLocal + dirOutLocal;
    float halfLenSq = dot(halfSum, halfSum);
    if (halfLenSq < 1e-12f)
        return 0.0f;

    Vector3D halfVec = normalize(halfSum);
    float NdotH = halfVec.z;
    float VdotX = dirInLocal.x, VdotY = dirInLocal.y;
    float HdotX = halfVec.x, HdotY = halfVec.y;

    float D = GGX_D_Aniso(NdotH, HdotX, HdotY, alphaX, alphaY);
    // VNDF PDF: pdf(L) = D(H) * G1(V) / (4 * N·V)
    float G1_V = GGX_G1_Aniso(NdotV, VdotX, VdotY, alphaX, alphaY);

    float NdotV_clamped = ::vlr::vlr_max(NdotV, 1e-6f);
    return D * G1_V / (4.0f * NdotV_clamped);
}

} // namespace shared
} // namespace vlr
