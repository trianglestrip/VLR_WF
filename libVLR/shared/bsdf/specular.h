#pragma once

#include "fresnel.h"

namespace vlr {
namespace shared {

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

    Vector3D reflected(-dirInLocal.x, -dirInLocal.y, dirInLocal.z);
    float cosOut = dirOutLocal.z;

    if (cosOut <= 0.0f)
        return SampledSpectrum::Zero();

    float diff = std::abs(dot(reflected, dirOutLocal) - 1.0f);
    if (diff < 1e-5f) {
        float cosTheta = std::abs(dirInLocal.z);
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

    float NdotL = dirInLocal.z;

    if (NdotL <= 0.0f) {
        result->pdf = 0.0f;
        result->f = SampledSpectrum::Zero();
        return;
    }

    Vector3D reflected(-dirInLocal.x, -dirInLocal.y, dirInLocal.z);
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

    Vector3D reflected(-dirInLocal.x, -dirInLocal.y, dirInLocal.z);
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
    bool frontFace) {
    (void)geomNormalLocal;

    // Undo face-forwarding so the sign convention matches the BSDF formulas
    Vector3D woOrig = dirInLocal;
    Vector3D wiOrig = dirOutLocal;
    if (!frontFace) {
        woOrig.z = -woOrig.z;
        wiOrig.z = -wiOrig.z;
    }

    Normal3D shadingNormalLocal(0.0f, 0.0f, 1.0f);
    float cosThetaO = woOrig.z;
    float cosOut = wiOrig.z;
    float cosAbs = std::abs(cosThetaO);

    float etaIncident = frontFace ? 1.0f : ior;
    float etaTransmitted = frontFace ? ior : 1.0f;
    float etaRatio = etaIncident / etaTransmitted;

    float F = FresnelDielectric(cosAbs, etaIncident, etaTransmitted);

    if (cosThetaO * cosOut > 0.0f) {
        Vector3D dirReflected(-woOrig.x, -woOrig.y, woOrig.z);
        float diff = std::abs(dot(dirReflected, wiOrig) - 1.0f);
        if (diff < 1e-5f) {
            if (cosAbs < 1e-6f) return SampledSpectrum::Zero();
            return transmittance * (F / cosAbs);
        }
        return SampledSpectrum::Zero();
    }

    float sin2ThetaO = ::vlr::vlr_max(0.0f, 1.0f - cosAbs * cosAbs);
    float sin2ThetaT = etaRatio * etaRatio * sin2ThetaO;
    if (sin2ThetaT >= 1.0f) return SampledSpectrum::Zero();
    float cosThetaT = safeSqrt(1.0f - sin2ThetaT);
    Vector3D wt = etaRatio * -woOrig + (etaRatio * cosAbs - cosThetaT) * shadingNormalLocal;
    float diff = std::abs(dot(wt, wiOrig) - 1.0f);
    if (diff < 1e-5f) {
        float cosTAbs = std::abs(wt.z);
        if (cosTAbs < 1e-8f) return SampledSpectrum::Zero();

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
    float u0, float u1,
    BSDFSampleResult* result);

// ============================================================================
// PBRT-style Dielectric BSDF (Glass) - Correct Implementation
// ============================================================================

/// Reflect vector around normal
CUDA_DEVICE_FUNCTION CUDA_INLINE Vector3D reflectVector(
    const Vector3D& wi, const Normal3D& n) {
    return wi - 2.0f * dot(wi, n) * Vector3D(n.x, n.y, n.z);
}

/// Refract vector using Snell's law (PBRT style)
/// Returns false if total internal reflection occurs
/// wi: outgoing direction (from surface toward viewer, i.e., -ray.dir)
/// n: surface normal (pointing toward incident side)
/// eta: etaI / etaT (ratio of indices of refraction)
/// wt: output transmitted direction (pointing away from surface)
CUDA_DEVICE_FUNCTION CUDA_INLINE bool refractVector(
    const Vector3D& wo,
    const Normal3D& n,
    float eta,
    Vector3D* wt)
{
    // PBRT-style refraction with proper sign handling
    // wo: outgoing direction (from surface toward observer)
    // n: surface normal (outward)
    // eta: etaI / etaT
    
    float cosThetaO = dot(n, wo);
    float sin2ThetaO = std::max(0.0f, 1.0f - cosThetaO * cosThetaO);
    float sin2ThetaT = eta * eta * sin2ThetaO;
    
    if (sin2ThetaT >= 1.0f)
        return false; // Total internal reflection
    
    float cosThetaT = safeSqrt(1.0f - sin2ThetaT);
    
    // Determine sign of cosThetaT based on which side we're on
    // If cosThetaO > 0, we're leaving from outside, so cosThetaT should be negative (into medium)
    // If cosThetaO < 0, we're leaving from inside, so cosThetaT should be positive (out of medium)
    if (cosThetaO > 0.0f)
        cosThetaT = -cosThetaT;
    
    // Refraction formula: wt = eta * wo + (eta * cosThetaO + cosThetaT) * n
    *wt = eta * wo + (eta * cosThetaO + cosThetaT) * Vector3D(n.x, n.y, n.z);
    
    return true;
}

/// Fresnel reflectance using Schlick approximation (optimized for GPU)
CUDA_DEVICE_FUNCTION CUDA_INLINE float fresnelSchlick(float cosTheta, float etaI, float etaT)
{
    float r0 = (etaI - etaT) / (etaI + etaT);
    r0 = r0 * r0;

    float m = 1.0f - cosTheta;
    float m2 = m * m;
    float m5 = m2 * m2 * m;

    return r0 + (1.0f - r0) * m5;
}

/// Fresnel reflectance for dielectric interface (PBRT style)
CUDA_DEVICE_FUNCTION CUDA_INLINE float fresnelDielectricPBRT(
    float cosThetaI, float etaI, float etaT)
{
    // Clamp cosThetaI to [-1, 1]
    if (cosThetaI < -1.0f) cosThetaI = -1.0f;
    if (cosThetaI > 1.0f) cosThetaI = 1.0f;

    bool entering = cosThetaI > 0.0f;
    if (!entering) {
        float tmp = etaI;
        etaI = etaT;
        etaT = tmp;
        cosThetaI = std::abs(cosThetaI);
    }

    float sinThetaI = safeSqrt(::vlr::vlr_max(0.0f, 1.0f - cosThetaI * cosThetaI));
    float sinThetaT = etaI / etaT * sinThetaI;

    if (sinThetaT >= 1.0f)
        return 1.0f; // Total internal reflection

    float cosThetaT = safeSqrt(::vlr::vlr_max(0.0f, 1.0f - sinThetaT * sinThetaT));

    float Rparl = ((etaT * cosThetaI) - (etaI * cosThetaT)) /
                  ((etaT * cosThetaI) + (etaI * cosThetaT));
    float Rperp = ((etaI * cosThetaI) - (etaT * cosThetaT)) /
                  ((etaI * cosThetaI) + (etaT * cosThetaT));

    return (Rparl * Rparl + Rperp * Rperp) * 0.5f;
}

/// Sample PBRT-style Dielectric (Glass) BSDF
/// Verified implementation from PBRT/Mitsuba/OptiX renderers
/// Delta reflection + delta transmission with correct energy conservation
CUDA_DEVICE_FUNCTION CUDA_INLINE void sampleDielectricBSDF_PBRT(
    float etaExt,
    float etaInt,
    const SampledSpectrum& transmittance,
    const Vector3D& dirInLocal,
    const Normal3D& geomNormalLocal,
    bool frontFace,
    float u0,
    BSDFSampleResult* result)
{
    (void)geomNormalLocal;

    Vector3D wo = dirInLocal;
    if (!frontFace)
        wo.z = -wo.z;

    float cosThetaO = wo.z;
    bool entering = (cosThetaO > 0.0f);

    float etaI = entering ? etaExt : etaInt;
    float etaT = entering ? etaInt : etaExt;
    float eta = etaI / etaT;

    float absCosTheta = std::abs(cosThetaO);

    float Fr = FresnelDielectric(absCosTheta, etaI, etaT);

    if (u0 < Fr) {
        Vector3D wi = Vector3D(-wo.x, -wo.y, wo.z);
        // Transform back to the face-forwarded frame
        if (!frontFace)
            wi.z = -wi.z;

        result->dirLocal = wi;
        result->pdf = Fr;
        result->f = transmittance * Fr;
        result->sampledBSDFType = BSDFType_Specular;
        result->isDelta = true;
    } else {
        float sin2ThetaI = ::vlr::vlr_max(0.0f, 1.0f - absCosTheta * absCosTheta);
        float sin2ThetaT = eta * eta * sin2ThetaI;

        if (sin2ThetaT >= 1.0f) {
            Vector3D wr = Vector3D(-wo.x, -wo.y, wo.z);
            if (!frontFace)
                wr.z = -wr.z;
            result->dirLocal = wr;
            result->pdf = 1.0f;
            result->f = transmittance;
            result->sampledBSDFType = BSDFType_Specular;
            result->isDelta = true;
            return;
        }

        float cosThetaT = safeSqrt(1.0f - sin2ThetaT);

        Vector3D wi;
        wi.x = -eta * wo.x;
        wi.y = -eta * wo.y;
        wi.z = entering ? -cosThetaT : cosThetaT;

        float len = safeSqrt(wi.x * wi.x + wi.y * wi.y + wi.z * wi.z);
        if (len > 1e-7f) {
            wi.x /= len;
            wi.y /= len;
            wi.z /= len;
        }

        // Transform back to the face-forwarded frame
        if (!frontFace)
            wi.z = -wi.z;

        float Ft = 1.0f - Fr;

        result->dirLocal = wi;
        result->pdf = Ft;
        result->f = transmittance * Ft;
        result->sampledBSDFType = BSDFType_SpecularTransmission;
        result->isDelta = true;
    }
}

/// Get PDF for PBRT-style Dielectric BSDF (always returns 0 for delta BSDF)
CUDA_DEVICE_FUNCTION CUDA_INLINE float getSpecularTransmissionBSDFPDF_FrontFace(
    float ior,
    const Vector3D& dirInLocal, const Vector3D& dirOutLocal,
    const Normal3D& geomNormalLocal,
    bool frontFace) {
    // Delta BSDFs have zero PDF for any non-exact direction match
    (void)ior; (void)dirInLocal; (void)dirOutLocal; (void)geomNormalLocal; (void)frontFace;
    return 0.0f;
}

CUDA_DEVICE_FUNCTION CUDA_INLINE bool refract(
    const Vector3D& wi, const Normal3D& n, float eta, Vector3D* wt) {
    // wi: incident direction (pointing toward surface, i.e., -wo)
    // n: surface normal (pointing toward incident side)
    // eta: etaI / etaT
    // wt: output transmitted direction (pointing away from surface)
    float cosThetaI = dot(wi, n);
    float sin2ThetaI = ::vlr::vlr_max(0.0f, 1.0f - cosThetaI * cosThetaI);
    float sin2ThetaT = eta * eta * sin2ThetaI;
    if (sin2ThetaT >= 1.0f) return false;  // 全内反射
    float cosThetaT = safeSqrt(1.0f - sin2ThetaT);
    // CRITICAL: Use + not - for correct refraction direction
    // Formula: wt = eta * wi + (eta * cosThetaI - cosThetaT) * n
    *wt = eta * wi + (eta * cosThetaI - cosThetaT) * n;
    return true;
}

/// 评估完美镜面透射 BSDF（支持反射+折射）
/// 参考libVLR_reference SpecularBSDF::evaluateInternal
/// 正确处理进入/离开：cosThetaI<0 表示从外进入(etaI=1,etaT=ior)，cosThetaI>0 表示从内离开(etaI=ior,etaT=1)
CUDA_DEVICE_FUNCTION CUDA_INLINE SampledSpectrum evaluateSpecularTransmissionBSDF(
    float ior, const SampledSpectrum& transmittance,
    const Vector3D& dirInLocal, const Vector3D& dirOutLocal,
    const Normal3D& geomNormalLocal) {
    
    float cosThetaI = dirInLocal.z;
    float cosOut = dirOutLocal.z;
    
    // 判断进入/离开
    bool entering = (cosThetaI >= 0.0f);
    float etaI, etaT;
    Normal3D nEff;
    if (entering) {
        nEff = Normal3D(0, 0, 1);
        etaI = 1.0f;
        etaT = ior;
    } else {
        nEff = Normal3D(0, 0, -1);
        etaI = ior;
        etaT = 1.0f;
    }
    
    float F = FresnelDielectric(std::abs(cosThetaI), etaI, etaT);
    float etaRatio = etaI / etaT;
    
    // 检查是否为反射方向(同半球)
    if (cosThetaI * cosOut > 0.0f) {
        // 反射: 检查是否为镜面反射方向（local coords: normal = (0,0,1)）
        Vector3D dirReflected(-dirInLocal.x, -dirInLocal.y, dirInLocal.z);
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
    bool frontFace,
    float u0, float /*u1*/,
    BSDFSampleResult* result) {

    (void)geomNormalLocal;

    // Undo face-forwarding to get the true geometric orientation
    Vector3D wo = dirInLocal;
    if (!frontFace)
        wo.z = -wo.z;

    float cosThetaO = wo.z;
    bool entering = (cosThetaO > 0.0f);

    // Wavelength-dependent IOR for dispersion: always use the path's selected
    // wavelength channel. The external code (sample_bsdf.cu) handles the pdf
    // compensation for single-wavelength selection.
    float etaT_eff = etaT;
    if (wls && dispersionStrength > 0.0f) {
        uint32_t idx = wls->selectedLambdaIndex() % NumSpectralSamples;
        float lambda = wls->lambdas[idx];
        etaT_eff = iorAtWavelength(lambda, etaT, dispersionStrength);
    }

    float etaILocal = entering ? etaI : etaT_eff;
    float etaTLocal = entering ? etaT_eff : etaI;
    float eta = etaILocal / etaTLocal;

    float absCosTheta = std::abs(cosThetaO);
    float Fr = FresnelDielectric(absCosTheta, etaILocal, etaTLocal);

    if (u0 < Fr) {
        // Reflection
        Vector3D wi = Vector3D(-wo.x, -wo.y, wo.z);
        if (!frontFace)
            wi.z = -wi.z;

        result->dirLocal = wi;
        result->pdf = Fr;
        result->f = transmittance * Fr;
        result->sampledBSDFType = BSDFType_Specular;
        result->isDelta = true;
    } else {
        // Refraction
        float sin2ThetaI = ::vlr::vlr_max(0.0f, 1.0f - absCosTheta * absCosTheta);
        float sin2ThetaT = eta * eta * sin2ThetaI;

        if (sin2ThetaT >= 1.0f) {
            // Total internal reflection fallback
            Vector3D wr = Vector3D(-wo.x, -wo.y, wo.z);
            if (!frontFace)
                wr.z = -wr.z;
            result->dirLocal = wr;
            result->pdf = 1.0f;
            result->f = transmittance;
            result->sampledBSDFType = BSDFType_Specular;
            result->isDelta = true;
            return;
        }

        float cosThetaT = safeSqrt(1.0f - sin2ThetaT);

        Vector3D wi;
        wi.x = -eta * wo.x;
        wi.y = -eta * wo.y;
        wi.z = entering ? -cosThetaT : cosThetaT;

        float len = safeSqrt(wi.x * wi.x + wi.y * wi.y + wi.z * wi.z);
        if (len > 1e-7f) {
            wi.x /= len; wi.y /= len; wi.z /= len;
        }

        if (!frontFace)
            wi.z = -wi.z;

        float Ft = 1.0f - Fr;

        result->dirLocal = wi;
        result->pdf = Ft;
        result->f = transmittance * Ft;
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
    float cosThetaI = dirInLocal.z;
    
    // In shading-local: cosThetaI < 0 = entering (from outside), cosThetaI > 0 = exiting
    bool entering = (cosThetaI < 0.0f);
    
    Normal3D nEff = entering ? Normal3D(0, 0, 1) : Normal3D(0, 0, -1);
    float etaRatio = entering ? (1.0f / ior) : ior;
    
    Vector3D wt;
    // CRITICAL FIX: refract() expects incident direction (wi = -wo)
    // NOT outgoing direction (wo)
    if (!refract(-dirInLocal, nEff, etaRatio, &wt))
        return 0.0f;
    
    float diff = std::abs(dot(wt, dirOutLocal) - 1.0f);
    return (diff < 1e-5f) ? 1.0f : 0.0f;
}

} // namespace shared
} // namespace vlr
