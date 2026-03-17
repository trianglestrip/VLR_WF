#pragma once

#include "bsdf_types.h"

namespace vlr {
namespace shared {

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
    result.hasEmission = materialHasEmission(matDesc);

    // In local space, z > 0 means the query direction is on the front
    // (emitting) hemisphere.  Only emit from the front face.
    if (result.hasEmission && dirOutLocal.z > 0.0f) {
        result.Le = radiance;
    } else {
        result.Le = SampledSpectrum::Zero();
        result.hasEmission = false;
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
