#pragma once

#include "fresnel.h"
#include "microfacet.h"

namespace vlr {
namespace shared {

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

    // Use z-component (shading normal direction) for hemisphere tests
    float NdotL = dirInLocal.z;
    float NdotV = dirOutLocal.z;
    float dotNVdotNL = NdotL * NdotV;

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
        float NdotH = halfVec.z;
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
            float D_wl = GGX_D(std::abs(halfVec.z), alpha2);

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

/// 计算 GGX VNDF 的 PDF：p(m) = D*G1(v,m)*dotHV / |V.z|
/// V must be in the z>0 hemisphere (post-flip for entering/exiting)
CUDA_DEVICE_FUNCTION CUDA_INLINE float getGGXVNDFPDF(
    const Vector3D& H,
    const Vector3D& V,
    float alpha2) {
    float dotHV = dot(V, H);
    if (dotHV <= 0.0f) return 0.0f;
    float NdotV = std::abs(V.z);
    if (NdotV <= 1e-6f) return 0.0f;
    float NdotH = std::abs(H.z);
    float D = GGX_D(NdotH, alpha2);
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

    // In shading-local coords, z-axis IS the shading normal.
    // Use dirInLocal.z (not dot with geomNormal) to determine entering side,
    // since VNDF sampling requires V.z > 0.
    bool entering = (dirInLocal.z >= 0.0f);
    float eEnter = entering ? 1.0f : ior;
    float eExit = entering ? ior : 1.0f;
    float recRelIOR = eEnter / eExit;

    // Flip to ensure dirV is in the z>0 hemisphere (required by sampleGGXVNDF)
    Vector3D dirV = entering ? dirInLocal : -dirInLocal;

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
        float mPDF = getGGXVNDFPDF(H, dirV, alpha2);
        float dotHV_safe = ::vlr::vlr_max(dotHV, 1e-6f);
        result->pdf = (reflectProb / (4.0f * dotHV_safe)) * mPDF;

        // BSDF = coeff*F*D*G/(4*dirV.z*dirL.z)
        float D = GGX_D(std::abs(H.z), alpha2);
        float G1_v = GGX_G1(std::abs(dirV.z), alpha2);
        float G1_l = GGX_G1(std::abs(dirL.z), alpha2);
        float G = G1_v * G1_l;
        float denom = 4.0f * std::abs(dirV.z * dirL.z);
        if (denom > 1e-7f) {
            // 应用coeff系数(参考libVLR_reference第1319行)
            result->f = coeff * (F * D * G / denom);
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
        float mPDF = getGGXVNDFPDF(H, dirV, alpha2);
        result->pdf = (1.0f - reflectProb) / denomPdfSq * mPDF * (eExit * eExit) * std::fabs(dotHL);

        // 直接计算BSDF,不调用evaluate(参考第1354-1370行)
        SampledSpectrum ret = SampledSpectrum::Zero();
        for (int wlIdx = 0; wlIdx < NumSpectralSamples; ++wlIdx) {
            Normal3D m_wl = normalize(-(eEnter * dirV + eExit * dirL));
            float dotHV_wl = dot(dirV, m_wl);
            float dotHL_wl = dot(dirL, m_wl);
            float F_wl = FresnelDielectric(std::abs(dotHV_wl), eEnter, eExit);
            float G1_v = GGX_G1(std::abs(dirV.z), alpha2);
            float G1_l = GGX_G1(std::abs(dirL.z), alpha2);
            float G_wl = G1_v * G1_l;
            float D_wl = GGX_D(std::abs(m_wl.z), alpha2);
            float denomBsdf = eEnter * dotHV_wl + eExit * dotHL_wl;
            ret.values[wlIdx] = std::fabs(dotHV_wl * dotHL_wl) * (1.0f - F_wl) * G_wl * D_wl / (denomBsdf * denomBsdf);
        }
        ret /= std::fabs(dirV.z * dirL.z);
        ret *= coeff;
        float adjoint = (transportMode == ::vlr::TransportMode::Radiance) ? (eEnter * eEnter) : (eExit * eExit);
        ret = ret * adjoint;
        result->f = ret;

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

    // Use z-component (shading normal direction) for hemisphere tests
    float NdotL = dirInLocal.z;
    float NdotV = dirOutLocal.z;
    float dotNVdotNL = NdotL * NdotV;

    if (dotNVdotNL == 0.0f)
        return 0.0f;

    float alpha = roughnessToAlpha(roughness);
    float alpha2 = alpha * alpha;

    bool entering = (NdotL > 0.0f);
    float eEnter = entering ? 1.0f : ior;
    float eExit = entering ? ior : 1.0f;

    // dirV: flipped to z>0 hemisphere (needed for VNDF PDF)
    Vector3D dirV = entering ? dirInLocal : -dirInLocal;

    Normal3D m;
    if (dotNVdotNL > 0.0f) {
        // Reflection: half-vector of dirV and flipped dirOut
        Vector3D dirL = entering ? dirOutLocal : -dirOutLocal;
        Vector3D halfSum = dirV + dirL;
        float halfLenSq = dot(halfSum, halfSum);
        if (halfLenSq < 1e-12f) return 0.0f;
        m = normalize(halfSum);
    } else {
        // Refraction: m = normalize(-(eEnter*dirV + eExit*dirL))
        Vector3D dirL = entering ? dirOutLocal : -dirOutLocal;
        m = normalize(-(eEnter * dirV + eExit * dirL));
    }

    float dotHV = dot(dirV, m);
    if (dotHV <= 0.0f)
        return 0.0f;

    float mPDF = getGGXVNDFPDF(m, dirV, alpha2);
    float F = FresnelDielectric(dotHV, eEnter, eExit);
    float reflectProb = F;

    if (dotNVdotNL > 0.0f) {
        // 反射 PDF：commonPDFTerm * mPDF = (reflectProb / (4*dotHV)) * mPDF
        float dotHV_safe = ::vlr::vlr_max(dotHV, 1e-6f);
        return (reflectProb / (4.0f * dotHV_safe)) * mPDF;
    } else {
        // 折射 PDF：(1-reflectProb) / (eEnter*dotHV + eExit*dotHL)^2 * mPDF * eExit^2 * |dotHL|
        Vector3D dirL = entering ? dirOutLocal : -dirOutLocal;
        float dotHL = dot(dirL, m);
        float denomPdf = eEnter * dotHV + eExit * dotHL;
        float denomPdfSq = denomPdf * denomPdf;
        if (denomPdfSq < 1e-14f) return 0.0f;
        return (1.0f - reflectProb) / denomPdfSq * mPDF * (eExit * eExit) * std::fabs(dotHL);
    }
}

} // namespace shared
} // namespace vlr
