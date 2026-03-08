// ============================================================================
// VLR 环境光重要性贴图 - CPU 端生成
//
// 从 HDR 环境贴图构建基于亮度的 2D CDF，用于 GPU 重要性采样。
//
// 作者：VLR 开发团队
// 创建日期：2026-03-08
// ============================================================================

#include "shared/env_importance.h"
#include "include/vlr/basic_types.h"
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <vector>

namespace vlr {
namespace shared {

namespace {

constexpr float VLR_PI = 3.14159265358979323846f;
constexpr float VLR_2PI = 6.28318530717958647692f;

/// sRGB 亮度权重 (Rec. 709)
inline float luminance(float r, float g, float b) {
    return 0.2126f * r + 0.7152f * g + 0.0722f * b;
}

} // anonymous namespace

// ============================================================================
// 重要性贴图构建
// ============================================================================

/// 从 HDR 环境贴图构建重要性贴图
///
/// @param textureData   RGB float 数据，行优先，每像素 3 个 float
/// @param width         贴图宽度（phi 方向）
/// @param height        贴图高度（theta 方向）
/// @param outCdfTheta    输出：theta 边际 CDF [height+1]，调用者负责释放
/// @param outCdfPhi      输出：每行 phi 条件 CDF [height * (width+1)]，调用者负责释放
/// @param outTotalLum    输出：总亮度
/// @return 成功返回 true
bool buildEnvironmentImportanceMap(
    const float* textureData,
    uint32_t width,
    uint32_t height,
    float** outCdfTheta,
    float** outCdfPhi,
    float* outTotalLum) {

    if (!textureData || width == 0 || height == 0 || !outCdfTheta || !outCdfPhi || !outTotalLum) {
        return false;
    }

    const uint32_t thetaRes = height;
    const uint32_t phiRes = width;

    // 分配行亮度（每 theta 行的加权和）
    std::vector<float> rowSums(thetaRes, 0.0f);

    for (uint32_t j = 0; j < thetaRes; ++j) {
        float theta = (j + 0.5f) / thetaRes * VLR_PI;
        float sinTheta = std::sin(theta);
        if (sinTheta < 1e-6f) sinTheta = 1e-6f;

        for (uint32_t i = 0; i < phiRes; ++i) {
            size_t idx = (j * phiRes + i) * 3;
            float r = textureData[idx + 0];
            float g = textureData[idx + 1];
            float b = textureData[idx + 2];
            float L = luminance(r, g, b);
            float weight = L * sinTheta;
            rowSums[j] += weight;
        }
    }

    // 总亮度
    float totalLuminance = 0.0f;
    for (uint32_t j = 0; j < thetaRes; ++j) {
        totalLuminance += rowSums[j];
    }
    if (totalLuminance < 1e-10f) {
        totalLuminance = 1.0f;
    }
    *outTotalLum = totalLuminance;

    // 分配 CDF 数组
    float* cdfTheta = static_cast<float*>(malloc((thetaRes + 1) * sizeof(float)));
    float* cdfPhi = static_cast<float*>(malloc(thetaRes * (phiRes + 1) * sizeof(float)));
    if (!cdfTheta || !cdfPhi) {
        free(cdfTheta);
        free(cdfPhi);
        return false;
    }

    cdfTheta[0] = 0.0f;
    for (uint32_t j = 0; j < thetaRes; ++j) {
        cdfTheta[j + 1] = cdfTheta[j] + rowSums[j];
    }

    for (uint32_t j = 0; j < thetaRes; ++j) {
        float theta = (j + 0.5f) / thetaRes * VLR_PI;
        float sinTheta = std::sin(theta);
        if (sinTheta < 1e-6f) sinTheta = 1e-6f;

        float* rowCdf = cdfPhi + j * (phiRes + 1);
        rowCdf[0] = 0.0f;

        for (uint32_t i = 0; i < phiRes; ++i) {
            size_t idx = (j * phiRes + i) * 3;
            float r = textureData[idx + 0];
            float g = textureData[idx + 1];
            float b = textureData[idx + 2];
            float L = luminance(r, g, b);
            float weight = L * sinTheta;
            rowCdf[i + 1] = rowCdf[i] + weight;
        }
    }

    *outCdfTheta = cdfTheta;
    *outCdfPhi = cdfPhi;
    return true;
}

/// 释放 buildEnvironmentImportanceMap 分配的内存
void freeEnvironmentImportanceMap(float* cdfTheta, float* cdfPhi) {
    free(cdfTheta);
    free(cdfPhi);
}

} // namespace shared
} // namespace vlr
