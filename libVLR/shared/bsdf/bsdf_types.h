#pragma once

#include "../material_types.h"
#include "../kernel_common.h"
#include <cmath>

namespace vlr {
namespace shared {

/// 安全开平方（避免数值不稳定）
CUDA_DEVICE_FUNCTION CUDA_INLINE float safeSqrt(float x) {
    return std::sqrt(::vlr::vlr_max(x, 0.0f));
}

/// 将粗糙度转换为 GGX alpha 参数（Beckmann-style: alpha = roughness^2）
CUDA_DEVICE_FUNCTION CUDA_INLINE float roughnessToAlpha(float roughness) {
    float r = ::vlr::vlr_max(roughness, 0.001f);
    return r * r;
}

} // namespace shared
} // namespace vlr
