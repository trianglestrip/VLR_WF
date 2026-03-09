// ============================================================================
// VLR Wavefront - Debug Rendering Kernel
//
// 本文件实现调试渲染模式的可视化内核�?
// 流程：generateRays �?traceRays �?processHits �?renderDebugMode
// 调试模式只需单次采样，不需�?BSDF 采样和多次反弹�?
//
// 作者：VLR 开发团�?
// 创建日期�?026-03-08
// 环境：CUDA 13.1, OptiX 8.0.0, VS2022
// ============================================================================

#include "../shared/path_types_minimal.h"
#include "../shared/material_types.h"
#include "../include/vlr/basic_types.h"
#include "kernel_launch.h"

// 使用�?VLRDebugMode 枚举一致的数值，避免包含 public_types.h（防�?CUDA 编译重复定义�?
namespace { namespace dbg {
    constexpr uint32_t BaseColor = 1, GeometricNormal = 2, ShadingNormal = 3, Depth = 4;
    constexpr uint32_t UV = 5, Tangent = 6, Bitangent = 7, Roughness = 8, Metallic = 9;
    constexpr uint32_t MaterialID = 10, InstanceID = 11, PrimitiveID = 12;
    constexpr uint32_t DirectLighting = 13, IndirectLighting = 14;
    constexpr uint32_t DenoiserAlbedo = 15, DenoiserNormal = 16, NumModes = 17;
}}
#include "../utils/cuda_util.h"

#include <cuda_runtime.h>
#include <cmath>

namespace {

using namespace vlr;
using namespace vlr::shared;

/// Wang hash for ID-to-color mapping
CUDA_DEVICE_FUNCTION CUDA_INLINE uint32_t wangHash(uint32_t x) {
    x = (x ^ 61u) ^ (x >> 16u);
    x = x + (x << 3u);
    x = x ^ (x >> 4u);
    x = x * 0x27d4eb2du;
    x = x ^ (x >> 15u);
    return x;
}

/// Map normal [-1,1] to color [0,1]
CUDA_DEVICE_FUNCTION CUDA_INLINE void visualizeNormal(
    const Normal3D& n,
    float* r, float* g, float* b) {
    *r = (n.x + 1.0f) * 0.5f;
    *g = (n.y + 1.0f) * 0.5f;
    *b = (n.z + 1.0f) * 0.5f;
}

/// Map UV [0,1] to color
CUDA_DEVICE_FUNCTION CUDA_INLINE void visualizeUV(
    float u, float v,
    float* r, float* g, float* b) {
    *r = u;
    *g = v;
    *b = 0.0f;
}

/// Map depth to grayscale
CUDA_DEVICE_FUNCTION CUDA_INLINE void visualizeDepth(
    float depth, float maxDepth,
    float* r, float* g, float* b) {
    float t = (maxDepth > 0.0f) ? (depth / maxDepth) : 0.0f;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    *r = *g = *b = t;
}

/// Map ID to distinct color via hash (稳定且易区分的颜色映�?
/// 使用 0.2-1.0 范围避免过暗，提高相�?ID 的可区分�?
CUDA_DEVICE_FUNCTION CUDA_INLINE void visualizeID(
    uint32_t id,
    float* r, float* g, float* b) {
    uint32_t h = wangHash(id);
    const float lo = 0.2f;
    const float scale = (1.0f - lo) / 255.0f;
    *r = lo + static_cast<float>((h >> 16u) & 0xFFu) * scale;
    *g = lo + static_cast<float>((h >> 8u) & 0xFFu) * scale;
    *b = lo + static_cast<float>(h & 0xFFu) * scale;
}

}  // anonymous namespace

// ============================================================================
// renderDebugMode Kernel
// ============================================================================
// 根据调试模式将表面信息可视化�?RGB，写�?accumBuffer�?
// 每个像素对应 pathIndex，使�?pathStateBuffer、hitInfoBuffer、surfacePointBuffer�?

extern "C" __global__ void renderDebugMode(
    vlr::shared::WavefrontLaunchParameters* params)
{
    using namespace vlr::shared;
    WavefrontLaunchParameters& wlp = *params;

#ifdef __CUDACC__
    uint32_t pathIndex = blockIdx.x * blockDim.x + threadIdx.x;
    uint32_t numPixels = wlp.imageSize.x * wlp.imageSize.y;

    if (pathIndex >= numPixels)
        return;

    uint32_t debugMode = wlp.debugMode;
    if (debugMode == 0 || debugMode >= dbg::NumModes)
        return;  // Normal 或无效模式，由主流程处理

    SpectrumStorage* accum = wlp.accumBuffer.data;
    if (accum == nullptr)
        return;

    const WavefrontPathState& pathState = wlp.pathStateBuffer[pathIndex];
    const WavefrontHitInfo& hitInfo = wlp.hitInfoBuffer[pathIndex];

    uint32_t stride = (wlp.imageStrideInPixels > 0) ? wlp.imageStrideInPixels : wlp.imageSize.x;
    uint32_t pixelIdx = pathState.pixelY * stride + pathState.pixelX;

    float r = 0.0f, g = 0.0f, b = 0.0f;

    // 无有效命中（Miss）：输出黑色
    if (!hitInfo.hasHit() || hitInfo.hitInfinity()) {
        accum[pixelIdx].r = r;
        accum[pixelIdx].g = g;
        accum[pixelIdx].b = b;
        return;
    }

    const SurfacePoint& surfPt = wlp.surfacePointBuffer[pathIndex];
    const GeometryInstance& geomInst = wlp.geomInstBuffer[hitInfo.geomInstIndex];
    const SurfaceMaterialDescriptor& matDesc = wlp.materialDescriptorBuffer[geomInst.materialIndex];
    const float* matData = getMaterialDataAsFloats(matDesc);

    switch (debugMode) {
        case dbg::BaseColor: {
            BSDFType type = getBSDFType(matDesc);
            if (type == BSDFType_LambertCheckerboard) {
                SampledSpectrum albedo;
                getLambertAlbedoCheckerboard(matDesc, &surfPt, &albedo);
                r = albedo.values[0];
                g = albedo.values[1];
                b = albedo.values[2];
            } else {
                r = matData[MaterialDataLayout::AlbedoR];
                g = matData[MaterialDataLayout::AlbedoG];
                b = matData[MaterialDataLayout::AlbedoB];
            }
            break;
        }
        case dbg::GeometricNormal:
            visualizeNormal(surfPt.geometricNormal, &r, &g, &b);
            break;
        case dbg::ShadingNormal:
            visualizeNormal(surfPt.shadingFrame.z, &r, &g, &b);
            break;
        case dbg::Depth:
            visualizeDepth(hitInfo.t, 100.0f, &r, &g, &b);
            break;
        case dbg::UV:
            visualizeUV(surfPt.texCoord.x, surfPt.texCoord.y, &r, &g, &b);
            break;
        case dbg::Tangent:
            visualizeNormal(Normal3D(surfPt.shadingFrame.x.x, surfPt.shadingFrame.x.y, surfPt.shadingFrame.x.z), &r, &g, &b);
            break;
        case dbg::Bitangent:
            visualizeNormal(Normal3D(surfPt.shadingFrame.y.x, surfPt.shadingFrame.y.y, surfPt.shadingFrame.y.z), &r, &g, &b);
            break;
        case dbg::Roughness: {
            float rough = matData[MaterialDataLayout::Roughness];
            rough = (rough < 0.001f) ? 0.001f : rough;
            rough = (rough > 1.0f) ? 1.0f : rough;
            r = g = b = rough;
            break;
        }
        case dbg::Metallic: {
            float metallic = 0.0f;
            BSDFType bsdfType = getBSDFType(matDesc);
            if (bsdfType == BSDFType_UE4BRDF) {
                metallic = matData[MaterialDataLayout::Metallic];
            } else if (bsdfType == BSDFType_DisneyBRDF) {
                metallic = matData[MaterialDataLayout::Disney_Metallic];
            }
            metallic = (metallic < 0.0f) ? 0.0f : metallic;
            metallic = (metallic > 1.0f) ? 1.0f : metallic;
            r = g = b = metallic;
            break;
        }
        case dbg::MaterialID:
            visualizeID(geomInst.materialIndex, &r, &g, &b);
            break;
        case dbg::InstanceID:
            visualizeID(hitInfo.instIndex, &r, &g, &b);
            break;
        case dbg::PrimitiveID:
            visualizeID(hitInfo.primIndex, &r, &g, &b);
            break;
        case dbg::DenoiserAlbedo:
            if (wlp.accumAlbedoBuffer != nullptr) {
                r = wlp.accumAlbedoBuffer[pixelIdx].r;
                g = wlp.accumAlbedoBuffer[pixelIdx].g;
                b = wlp.accumAlbedoBuffer[pixelIdx].b;
            }
            break;
        case dbg::DenoiserNormal:
            if (wlp.accumNormalBuffer != nullptr) {
                visualizeNormal(wlp.accumNormalBuffer[pixelIdx], &r, &g, &b);
            }
            break;
        case dbg::DirectLighting:
        case dbg::IndirectLighting:
            // 需要完�?path tracing，暂输出灰色占位
            r = g = b = 0.5f;
            break;
        default:
            r = 1.0f; g = 0.0f; b = 1.0f;  // 错误颜色：洋�?
    }

    accum[pixelIdx].r = r;
    accum[pixelIdx].g = g;
    accum[pixelIdx].b = b;
#endif
}
