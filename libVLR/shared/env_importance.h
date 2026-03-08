// ============================================================================
// VLR 环境光重要性采样
//
// 本文件定义 EnvironmentImportanceMap 结构及 GPU 端采样/PDF 评估函数。
// 基于亮度的分层采样：先采样 theta（天顶角），再采样 phi（方位角）。
//
// 作者：VLR 开发团队
// 创建日期：2026-03-08
// 环境：CUDA 13.1, OptiX 8.0.0, VS2022
// ============================================================================

#pragma once

#include "../include/vlr/basic_types.h"
#include <cstdint>
#include <cmath>

namespace vlr {
namespace shared {

// ============================================================================
// 1. 环境光重要性贴图数据结构
// ============================================================================

/// 环境光重要性贴图：基于亮度的 2D CDF 分布
/// 用于环境贴图的重要性采样，支持旋转
struct EnvironmentImportanceMap {
    const float* cdfTheta;        ///< theta 的边际 CDF [thetaRes+1]
    const float* cdfPhi;          ///< 每个 theta 行的 phi 条件 CDF [thetaRes * (phiRes+1)]
    uint32_t thetaRes;            ///< theta 分辨率（对应贴图高度）
    uint32_t phiRes;              ///< phi 分辨率（对应贴图宽度）
    float totalLuminance;         ///< 总亮度（用于 PDF 归一化）

    CUDA_DEVICE_FUNCTION CUDA_INLINE bool isValid() const {
        return cdfTheta != nullptr && cdfPhi != nullptr &&
               thetaRes > 0 && phiRes > 0 && totalLuminance > 1e-10f;
    }

    /// 采样：给定 u0, u1 in [0,1)，输出 theta, phi 及 PDF
    /// theta: [0, pi], phi: [0, 2*pi]
    /// PDF 为立体角空间：p(omega) = luminance / totalLuminance
    CUDA_DEVICE_FUNCTION CUDA_INLINE void sample(
        float u0, float u1,
        float* outTheta, float* outPhi,
        float* outPDF) const {

        if (!isValid()) {
            *outTheta = u0 * VLR_M_PI;
            *outPhi = u1 * VLR_M_2PI;
            *outPDF = 1.0f / (VLR_M_2PI * VLR_M_PI);
            return;
        }

        // 1. 边际采样 theta（二分查找 CDF）
        float uTheta = u0 * cdfTheta[thetaRes];
        uint32_t thetaIdx = 0;
        for (uint32_t i = 1; i <= thetaRes; ++i) {
            if (uTheta < cdfTheta[i]) {
                thetaIdx = i - 1;
                break;
            }
            thetaIdx = i - 1;
        }
        if (thetaIdx >= thetaRes) thetaIdx = thetaRes - 1;

        // 2. 条件采样 phi（在 theta 行内二分查找）
        const float* rowCdf = cdfPhi + thetaIdx * (phiRes + 1);
        float rowSum = rowCdf[phiRes];
        float uPhi = (rowSum > 1e-10f) ? (u1 * rowSum) : 0.0f;
        uint32_t phiIdx = 0;
        for (uint32_t i = 1; i <= phiRes; ++i) {
            if (uPhi < rowCdf[i]) {
                phiIdx = i - 1;
                break;
            }
            phiIdx = i - 1;
        }
        if (phiIdx >= phiRes) phiIdx = phiRes - 1;

        // 3. 逆 CDF 映射到连续 (theta, phi)
        float dTheta = VLR_M_PI / thetaRes;
        float dPhi = VLR_M_2PI / phiRes;
        float fracTheta = (cdfTheta[thetaIdx + 1] > cdfTheta[thetaIdx])
            ? (uTheta - cdfTheta[thetaIdx]) / (cdfTheta[thetaIdx + 1] - cdfTheta[thetaIdx])
            : 0.5f;
        float fracPhi = (rowSum > 1e-10f && rowCdf[phiIdx + 1] > rowCdf[phiIdx])
            ? (uPhi - rowCdf[phiIdx]) / (rowCdf[phiIdx + 1] - rowCdf[phiIdx])
            : 0.5f;
        fracTheta = (fracTheta < 0.0f) ? 0.0f : ((fracTheta > 1.0f) ? 1.0f : fracTheta);
        fracPhi = (fracPhi < 0.0f) ? 0.0f : ((fracPhi > 1.0f) ? 1.0f : fracPhi);

        *outTheta = (thetaIdx + fracTheta) * dTheta;
        *outPhi = (phiIdx + fracPhi) * dPhi;

        // 4. PDF = P(cell) / solidAngle(cell)
        float sinTheta = std::sin(*outTheta);
        float sinThetaSafe = (sinTheta > 1e-6f) ? sinTheta : 1e-6f;
        float cellSolidAngle = sinThetaSafe * dTheta * dPhi;
        float marginalProb = (cdfTheta[thetaRes] > 1e-10f)
            ? (cdfTheta[thetaIdx + 1] - cdfTheta[thetaIdx]) / cdfTheta[thetaRes]
            : 1.0f / thetaRes;
        float condProb = (rowSum > 1e-10f)
            ? (rowCdf[phiIdx + 1] - rowCdf[phiIdx]) / rowSum
            : 1.0f / phiRes;
        float cellProb = marginalProb * condProb;
        *outPDF = (cellSolidAngle > 1e-12f)
            ? (cellProb / cellSolidAngle)
            : (1.0f / (VLR_M_2PI * VLR_M_PI * sinThetaSafe));
    }

    /// 评估给定 (theta, phi) 的 PDF（用于 MIS）
    /// u = phi/(2*pi), v = theta/pi
    CUDA_DEVICE_FUNCTION CUDA_INLINE float evaluatePDF(float u, float v) const {
        if (!isValid())
            return 1.0f / (VLR_M_2PI * VLR_M_PI);

        float theta = v * VLR_M_PI;
        float sinTheta = std::sin(theta);
        float sinThetaSafe = (sinTheta > 1e-6f) ? sinTheta : 1e-6f;

        uint32_t thetaIdx = static_cast<uint32_t>(v * thetaRes);
        uint32_t phiIdx = static_cast<uint32_t>(u * phiRes);
        if (thetaIdx >= thetaRes) thetaIdx = thetaRes - 1;
        if (phiIdx >= phiRes) phiIdx = phiRes - 1;

        const float* rowCdf = cdfPhi + thetaIdx * (phiRes + 1);
        float rowSum = rowCdf[phiRes];
        float dTheta = VLR_M_PI / thetaRes;
        float dPhi = VLR_M_2PI / phiRes;
        float cellSolidAngle = sinThetaSafe * dTheta * dPhi;

        float marginalProb = (cdfTheta[thetaRes] > 1e-10f)
            ? (cdfTheta[thetaIdx + 1] - cdfTheta[thetaIdx]) / cdfTheta[thetaRes]
            : 1.0f / thetaRes;
        float condProb = (rowSum > 1e-10f)
            ? (rowCdf[phiIdx + 1] - rowCdf[phiIdx]) / rowSum
            : 1.0f / phiRes;
        float cellProb = marginalProb * condProb;

        if (cellSolidAngle < 1e-12f)
            return 1.0f / (VLR_M_2PI * VLR_M_PI * sinThetaSafe);
        return cellProb / cellSolidAngle;
    }
};

// ============================================================================
// CPU 端：重要性贴图构建（在 env_importance.cpp 中实现）
// ============================================================================

/// 从 HDR 环境贴图构建重要性贴图
bool buildEnvironmentImportanceMap(
    const float* textureData,
    uint32_t width,
    uint32_t height,
    float** outCdfTheta,
    float** outCdfPhi,
    float* outTotalLum);

/// 释放 buildEnvironmentImportanceMap 分配的内存
void freeEnvironmentImportanceMap(float* cdfTheta, float* cdfPhi);

} // namespace shared
} // namespace vlr
