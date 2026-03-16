// ============================================================================
// VLR 采样工具函数
//
// 提供 GPU kernel 通用的采样函数，避免在各 kernel 中重复定义。
// ============================================================================

#pragma once

#include "vlr/basic_types.h"
#include <cmath>

namespace vlr {
namespace shared {

CUDA_DEVICE_FUNCTION CUDA_INLINE Vector3D sampleCosineHemisphere(float u0, float u1) {
    float r = std::sqrt(u0);
    float phi = u1 * VLR_M_2PI;
    float x = r * std::cos(phi);
    float y = r * std::sin(phi);
    float z = std::sqrt(1.0f - u0);
    return normalize(Vector3D(x, y, z));
}

CUDA_DEVICE_FUNCTION CUDA_INLINE Vector3D sampleUniformSphere(float u0, float u1) {
    float z = 1.0f - 2.0f * u0;
    float r = std::sqrt(1.0f - z * z);
    float phi = u1 * VLR_M_2PI;
    return normalize(Vector3D(r * std::cos(phi), r * std::sin(phi), z));
}

CUDA_DEVICE_FUNCTION CUDA_INLINE Vector3D sampleUniformHemisphere(float u0, float u1) {
    float z = u0;
    float r = std::sqrt(1.0f - z * z);
    float phi = u1 * VLR_M_2PI;
    return normalize(Vector3D(r * std::cos(phi), r * std::sin(phi), z));
}

} // namespace shared
} // namespace vlr
