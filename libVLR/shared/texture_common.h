// ============================================================================
// VLR 纹理系统 - 采样函数
//
// 本文件实现了 2D 纹理采样、法线贴图采样和凹凸映射。
// 为 ProcessHits kernel 中的表面点计算提供纹理支持。
//
// 作者：VLR 开发团队
// 创建日期：2026-03-07
// 环境：CUDA 13.1, OptiX 8.0.0, VS2022
// ============================================================================

#pragma once

#include "texture_types.h"
#include "geometry_types.h"
#include "../include/vlr/basic_types.h"
#include <cmath>

namespace vlr {
namespace shared {

using ::vlr::Point3D;
using ::vlr::Vector3D;
using ::vlr::Normal3D;
using ::vlr::TexCoord2D;
using ::vlr::ReferenceFrame;
using ::vlr::SurfacePoint;

// ============================================================================
// 1. 纹理坐标环绕
// ============================================================================

/// 应用环绕模式，将 UV 坐标映射到 [0, 1) 范围
CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE
void wrapTexCoord(float u, float v,
                  TextureWrapMode wrapU, TextureWrapMode wrapV,
                  float* outU, float* outV) {
    auto wrap = [](float x, TextureWrapMode mode) -> float {
        if (mode == TextureWrap_Repeat) {
            x = x - std::floor(x);
            if (x < 0.0f) x += 1.0f;
            return x;
        }
        // Clamp
        return (x < 0.0f) ? 0.0f : ((x > 1.0f) ? 1.0f : x);
    };
    *outU = wrap(u, wrapU);
    *outV = wrap(v, wrapV);
}


// ============================================================================
// 2. 纹理坐标到 texel 坐标
// ============================================================================

/// 将 UV [0,1) 转为 texel 中心坐标（用于最近邻）
/// 返回 texel 索引 (ix, iy)
CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE
void uvToTexelNearest(float u, float v, uint32_t width, uint32_t height,
                      uint32_t* ix, uint32_t* iy) {
    float fx = u * width;
    float fy = v * height;
    *ix = static_cast<uint32_t>(std::floor(fx)) % width;
    *iy = static_cast<uint32_t>(std::floor(fy)) % height;
}

/// 将 UV 转为用于双线性插值的四个 texel 坐标及权重
/// (ix0, iy0)-(ix1, iy1) 为四个角，fracU/fracV 为插值权重
CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE
void uvToTexelBilinear(float u, float v, uint32_t width, uint32_t height,
                       uint32_t* ix0, uint32_t* iy0, uint32_t* ix1, uint32_t* iy1,
                       float* fracU, float* fracV) {
    // 从 texel 中心采样：uv * size - 0.5
    float fx = u * width - 0.5f;
    float fy = v * height - 0.5f;
    int i0 = static_cast<int>(std::floor(fx));
    int j0 = static_cast<int>(std::floor(fy));
    *fracU = fx - i0;
    *fracV = fy - j0;
    *ix0 = (static_cast<uint32_t>(i0) % width + width) % width;
    *iy0 = (static_cast<uint32_t>(j0) % height + height) % height;
    *ix1 = (*ix0 + 1) % width;
    *iy1 = (*iy0 + 1) % height;
}


// ============================================================================
// 3. 读取 texel（RGBA8 格式）
// ============================================================================

/// 从 RGBA8 纹理读取单个 texel
/// data 为连续内存，每 texel 4 字节
CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE
TextureSampleRGBA readTexelRGBA8(const uint8_t* data, uint32_t ix, uint32_t iy,
                                 uint32_t width, uint32_t rowPitch) {
    if (rowPitch == 0)
        rowPitch = width * 4;
    const uint8_t* p = data + (iy * rowPitch + ix * 4);
    return TextureSampleRGBA(
        p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f, p[3] / 255.0f);
}

/// 从 RGBA32F 纹理读取单个 texel
CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE
TextureSampleRGBA readTexelRGBA32F(const float* data, uint32_t ix, uint32_t iy,
                                  uint32_t width, uint32_t rowPitch) {
    if (rowPitch == 0)
        rowPitch = width * 4 * static_cast<uint32_t>(sizeof(float));
    const float* p = reinterpret_cast<const float*>(
        reinterpret_cast<const uint8_t*>(data) + iy * rowPitch + ix * 4 * sizeof(float));
    return TextureSampleRGBA(p[0], p[1], p[2], p[3]);
}

/// 从 RGB32F 纹理读取单个 texel（alpha 默认为 1）
CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE
TextureSampleRGBA readTexelRGB32F(const float* data, uint32_t ix, uint32_t iy,
                                  uint32_t width, uint32_t rowPitch) {
    if (rowPitch == 0)
        rowPitch = width * 3 * static_cast<uint32_t>(sizeof(float));
    const float* p = reinterpret_cast<const float*>(
        reinterpret_cast<const uint8_t*>(data) + iy * rowPitch + ix * 3 * sizeof(float));
    return TextureSampleRGBA(p[0], p[1], p[2], 1.0f);
}


// ============================================================================
// 4. sampleTexture2D - 2D 纹理采样
// ============================================================================

/// 使用最近邻滤波采样 2D 纹理
CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE
TextureSampleRGBA sampleTexture2DNearest(const Texture2DDescriptor& tex,
                                        float u, float v,
                                        TextureWrapMode wrapU, TextureWrapMode wrapV) {
    if (!tex.isValid())
        return TextureSampleRGBA(0, 0, 0, 1);

    float su, sv;
    wrapTexCoord(u, v, wrapU, wrapV, &su, &sv);

    uint32_t ix, iy;
    uvToTexelNearest(su, sv, tex.width, tex.height, &ix, &iy);

    uint32_t pitch = tex.getEffectiveRowPitch();
    if (tex.format == TextureFormat_RGBA8) {
        return readTexelRGBA8(static_cast<const uint8_t*>(tex.data), ix, iy,
                              tex.width, pitch);
    }
    if (tex.format == TextureFormat_RGBA32F) {
        return readTexelRGBA32F(static_cast<const float*>(tex.data), ix, iy,
                               tex.width, pitch);
    }
    // RGB32F：读取为 RGBA，a=1
    if (tex.format == TextureFormat_RGB32F) {
        if (pitch == 0) pitch = tex.width * 3 * sizeof(float);
        const float* p = reinterpret_cast<const float*>(
            reinterpret_cast<const uint8_t*>(tex.data) + iy * pitch + ix * 3 * sizeof(float));
        return TextureSampleRGBA(p[0], p[1], p[2], 1.0f);
    }
    return TextureSampleRGBA(0, 0, 0, 1);
}

/// 使用双线性滤波采样 2D 纹理
CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE
TextureSampleRGBA sampleTexture2DLinear(const Texture2DDescriptor& tex,
                                       float u, float v,
                                       TextureWrapMode wrapU, TextureWrapMode wrapV) {
    if (!tex.isValid())
        return TextureSampleRGBA(0, 0, 0, 1);

    float su, sv;
    wrapTexCoord(u, v, wrapU, wrapV, &su, &sv);

    uint32_t ix0, iy0, ix1, iy1;
    float fu, fv;
    uvToTexelBilinear(su, sv, tex.width, tex.height,
                      &ix0, &iy0, &ix1, &iy1, &fu, &fv);

    auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };

    TextureSampleRGBA c00, c10, c01, c11;
    uint32_t pitch = tex.getEffectiveRowPitch();

    if (tex.format == TextureFormat_RGBA8) {
        const uint8_t* data = static_cast<const uint8_t*>(tex.data);
        c00 = readTexelRGBA8(data, ix0, iy0, tex.width, pitch);
        c10 = readTexelRGBA8(data, ix1, iy0, tex.width, pitch);
        c01 = readTexelRGBA8(data, ix0, iy1, tex.width, pitch);
        c11 = readTexelRGBA8(data, ix1, iy1, tex.width, pitch);
    } else if (tex.format == TextureFormat_RGBA32F) {
        const float* data = static_cast<const float*>(tex.data);
        c00 = readTexelRGBA32F(data, ix0, iy0, tex.width, pitch);
        c10 = readTexelRGBA32F(data, ix1, iy0, tex.width, pitch);
        c01 = readTexelRGBA32F(data, ix0, iy1, tex.width, pitch);
        c11 = readTexelRGBA32F(data, ix1, iy1, tex.width, pitch);
    } else if (tex.format == TextureFormat_RGB32F) {
        const float* data = static_cast<const float*>(tex.data);
        c00 = readTexelRGB32F(data, ix0, iy0, tex.width, pitch);
        c10 = readTexelRGB32F(data, ix1, iy0, tex.width, pitch);
        c01 = readTexelRGB32F(data, ix0, iy1, tex.width, pitch);
        c11 = readTexelRGB32F(data, ix1, iy1, tex.width, pitch);
    } else {
        return TextureSampleRGBA(0, 0, 0, 1);
    }

    TextureSampleRGBA c0, c1;
    c0.r = lerp(c00.r, c10.r, fu); c0.g = lerp(c00.g, c10.g, fu);
    c0.b = lerp(c00.b, c10.b, fu); c0.a = lerp(c00.a, c10.a, fu);
    c1.r = lerp(c01.r, c11.r, fu); c1.g = lerp(c01.g, c11.g, fu);
    c1.b = lerp(c01.b, c11.b, fu); c1.a = lerp(c01.a, c11.a, fu);

    return TextureSampleRGBA(
        lerp(c0.r, c1.r, fv), lerp(c0.g, c1.g, fv),
        lerp(c0.b, c1.b, fv), lerp(c0.a, c1.a, fv));
}

/// 2D 纹理采样（根据采样器滤波模式选择）
CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE
TextureSampleRGBA sampleTexture2D(const TextureSampler& sampler,
                                  float u, float v) {
    if (!sampler.isValid())
        return TextureSampleRGBA(0, 0, 0, 1);

    if (sampler.filterMode == TextureFilter_Nearest) {
        return sampleTexture2DNearest(sampler.tex, u, v,
                                     sampler.wrapU, sampler.wrapV);
    }
    return sampleTexture2DLinear(sampler.tex, u, v,
                                sampler.wrapU, sampler.wrapV);
}


// ============================================================================
// 5. 切线空间构建
// ============================================================================

/// 从 shading 参考系构建 TBN 矩阵（切线-副切线-法线）
/// shadingFrame: x=tangent, y=bitangent, z=normal
/// 返回的 frame 可直接用于将切线空间法线变换到世界空间
CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE
ReferenceFrame buildTangentSpace(const ReferenceFrame& shadingFrame) {
    return shadingFrame;  // ReferenceFrame 本身即为 TBN
}


// ============================================================================
// 6. 法线贴图采样
// ============================================================================

/// 从法线贴图采样并解码为切线空间法线
/// 法线贴图格式：RGB 存储 (0,1) 范围，解码为 (-1,1) 方向
/// 返回单位化的切线空间法线
CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE
Normal3D sampleNormalMap(const TextureSampler& sampler,
                        float u, float v) {
    TextureSampleRGBA sample = sampleTexture2D(sampler, u, v);
    // 从 [0,1] 映射到 [-1,1]，z 向上（OpenGL 风格）或 y 向上（DirectX）
    // 使用 OpenGL 约定：RGB -> (x, y, z) 切线空间，z 为表面法线方向
    float nx = 2.0f * sample.r - 1.0f;
    float ny = 2.0f * sample.g - 1.0f;
    float nz = 2.0f * sample.b - 1.0f;
    Normal3D n(nx, ny, nz);
    float len = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
    if (len > 1e-6f)
        n = n / len;
    else
        n = Normal3D(0, 0, 1);  // 平坦表面
    return n;
}


// ============================================================================
// 7. 切线空间到世界空间转换
// ============================================================================

/// 将切线空间法线变换到世界空间
/// frame: 着色参考系 (T, B, N)，toWorld 将局部向量转为世界空间
CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE
Normal3D tangentToWorld(const Normal3D& tangentSpaceNormal,
                       const ReferenceFrame& frame) {
    return ::vlr::normalize(frame.toWorld(tangentSpaceNormal));
}


// ============================================================================
// 8. applyBumpMapping - 应用法线贴图
// ============================================================================

/// 应用法线贴图扰动
/// 从纹理采样切线空间法线，变换到世界空间，更新 surfacePoint 的着色法线
/// texSampler: 法线贴图纹理采样器；nullptr 表示无贴图，不修改
CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE
void applyBumpMapping(const Normal3D& localNormal,
                     SurfacePoint* surfPt,
                     const TextureSampler* normalMapSampler = nullptr) {
    if (surfPt == nullptr)
        return;

    // 若无法线贴图，localNormal 作为默认（来自节点或 (0,0,1)）
    Normal3D effectiveLocal = localNormal;
    if (normalMapSampler != nullptr && normalMapSampler->isValid()) {
        effectiveLocal = sampleNormalMap(*normalMapSampler,
                                        surfPt->texCoord.x, surfPt->texCoord.y);
    }

    // 将切线空间法线变换到世界空间
    Normal3D worldNormal = tangentToWorld(effectiveLocal, surfPt->shadingFrame);

    // 确保法线与几何法线同侧（避免背面光照）
    if (::vlr::dot(worldNormal, surfPt->geometricNormal) < 0.0f)
        worldNormal = worldNormal * (-1.0f);

    // 更新 shadingFrame 的法线分量，重新构建正交基
    surfPt->shadingFrame = ReferenceFrame(surfPt->shadingFrame.x, worldNormal);
}


/// 无纹理版本：仅使用传入的 localNormal 更新着色法线
/// 与 basic_types.h 中 applyBumpMapping 签名兼容
CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE
void applyBumpMappingDefault(const Normal3D& localNormal, SurfacePoint* surfPt) {
    applyBumpMapping(localNormal, surfPt, nullptr);
}


// ============================================================================
// 9. 从材质/纹理缓冲区获取法线贴图采样器
// ============================================================================

/// 从纹理缓冲区获取法线贴图采样器
/// textureBuffer: 纹理描述符数组
/// normalMapTextureIndex: 材质中的法线贴图索引；InvalidTextureIndex 表示无
CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE
TextureSampler getNormalMapSampler(const Texture2DDescriptor* textureBuffer,
                                  uint32_t normalMapTextureIndex,
                                  TextureFilterMode filter = TextureFilter_Linear) {
    if (textureBuffer == nullptr || normalMapTextureIndex == MaterialTextureSlots::InvalidTextureIndex)
        return TextureSampler();

    const Texture2DDescriptor& tex = textureBuffer[normalMapTextureIndex];
    if (!tex.isValid())
        return TextureSampler();

    return TextureSampler(tex, filter);
}

} // namespace shared
} // namespace vlr
