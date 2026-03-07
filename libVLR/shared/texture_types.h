// ============================================================================
// VLR 纹理系统 - 类型定义
//
// 本文件定义了 Wavefront 路径追踪所需的纹理类型和采样器。
// 为 ProcessHits 和 SampleBSDF kernel 提供纹理采样支持。
//
// 作者：VLR 开发团队
// 创建日期：2026-03-07
// 环境：CUDA 13.1, OptiX 8.0.0, VS2022
// ============================================================================

#pragma once

#include "../include/vlr/basic_types.h"
#include <cstdint>
#include <cmath>

namespace vlr {
namespace shared {

// 前向声明几何类型
using ::vlr::TexCoord2D;
using ::vlr::Vector3D;
using ::vlr::Normal3D;

// ============================================================================
// 1. 纹理采样器类型
// ============================================================================

/// 纹理滤波模式：控制 texel 到 pixel 的插值方式
enum TextureFilterMode : uint32_t {
    TextureFilter_Nearest = 0,   ///< 最近邻采样，无插值
    TextureFilter_Linear,        ///< 双线性插值
    NumTextureFilterModes
};

/// 纹理环绕模式（预留，当前使用重复）
enum TextureWrapMode : uint32_t {
    TextureWrap_Repeat = 0,     ///< 重复纹理
    TextureWrap_Clamp,           ///<  clamping 到边缘
    NumTextureWrapModes
};


// ============================================================================
// 2. 纹理格式
// ============================================================================

/// 纹理像素格式
enum TextureFormat : uint32_t {
    TextureFormat_RGBA8 = 0,     ///< 8 位 RGBA（每通道 0-255）
    TextureFormat_RGB32F,        ///< 32 位浮点 RGB（用于法线贴图精确存储）
    TextureFormat_RGBA32F,       ///< 32 位浮点 RGBA
    NumTextureFormats
};


// ============================================================================
// 3. Texture2D 描述符
// ============================================================================

/// 2D 纹理描述符
/// 描述纹理的尺寸、格式、Mipmap 层级及数据指针
struct Texture2DDescriptor {
    const void* data;            ///< 纹理数据指针（RGBA8 或 float4 格式）
    uint32_t width;              ///< 纹理宽度（texel 数量）
    uint32_t height;             ///< 纹理高度（texel 数量）
    TextureFormat format;        ///< 像素格式
    uint32_t numMipLevels;       ///< Mipmap 层级数（1 = 无 mipmap）
    uint32_t rowPitch;           ///< 每行字节步长（0 表示紧密排列）

    CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE
    Texture2DDescriptor()
        : data(nullptr)
        , width(0)
        , height(0)
        , format(TextureFormat_RGBA8)
        , numMipLevels(1)
        , rowPitch(0)
    {}

    CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE
    Texture2DDescriptor(const void* ptr, uint32_t w, uint32_t h,
                        TextureFormat fmt = TextureFormat_RGBA8,
                        uint32_t mipLevels = 1, uint32_t pitch = 0)
        : data(ptr)
        , width(w)
        , height(h)
        , format(fmt)
        , numMipLevels(mipLevels)
        , rowPitch(pitch)
    {}

    /// 纹理是否有效
    CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE
    bool isValid() const {
        return data != nullptr && width > 0 && height > 0;
    }

    /// 获取基础层级（mip 0）的行 pitch
    CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE
    uint32_t getEffectiveRowPitch() const {
        if (rowPitch > 0)
            return rowPitch;
        // 默认紧密排列
        if (format == TextureFormat_RGBA8)
            return width * 4;
        if (format == TextureFormat_RGB32F || format == TextureFormat_RGBA32F)
            return width * sizeof(float) * (format == TextureFormat_RGB32F ? 3 : 4);
        return width * 4;
    }

    /// 获取 mip 层级 k 的尺寸
    CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE
    void getMipDimensions(uint32_t k, uint32_t* outW, uint32_t* outH) const {
        uint32_t w = width >> k;
        uint32_t h = height >> k;
        if (w < 1) w = 1;
        if (h < 1) h = 1;
        *outW = w;
        *outH = h;
    }
};


// ============================================================================
// 4. 纹理采样器描述符
// ============================================================================

/// 纹理采样器：组合 Texture2D 与采样参数
struct TextureSampler {
    Texture2DDescriptor tex;        ///< 2D 纹理描述符
    TextureFilterMode filterMode;    ///< 滤波模式（线性/最近邻）
    TextureWrapMode wrapU;           ///< U 方向环绕
    TextureWrapMode wrapV;           ///< V 方向环绕

    CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE
    TextureSampler()
        : filterMode(TextureFilter_Linear)
        , wrapU(TextureWrap_Repeat)
        , wrapV(TextureWrap_Repeat)
    {}

    CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE
    TextureSampler(const Texture2DDescriptor& t,
                   TextureFilterMode filter = TextureFilter_Linear)
        : tex(t)
        , filterMode(filter)
        , wrapU(TextureWrap_Repeat)
        , wrapV(TextureWrap_Repeat)
    {}

    CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE
    bool isValid() const {
        return tex.data != nullptr && tex.width > 0 && tex.height > 0;
    }
};


// ============================================================================
// 5. 纹理采样结果（RGBA）
// ============================================================================

/// 纹理采样结果：RGBA 颜色
struct TextureSampleRGBA {
    float r, g, b, a;

    CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE
    TextureSampleRGBA() : r(0), g(0), b(0), a(1) {}

    CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE
    TextureSampleRGBA(float pr, float pg, float pb, float pa = 1.0f)
        : r(pr), g(pg), b(pb), a(pa) {}
};


// ============================================================================
// 6. 程序化纹理类型
// ============================================================================

/// 程序化纹理类型
enum ProceduralTextureType : uint32_t {
    ProceduralTexture_None = 0,
    ProceduralTexture_Checkerboard,   ///< 棋盘格纹理
    NumProceduralTextureTypes
};


// ============================================================================
// 7. 材质纹理插槽索引（与 material_types.h 协同）
// ============================================================================

/// 材质纹理参数插槽 - 使用 MaterialDataLayout 中的索引
/// 本命名空间仅为兼容性保留，实际索引见 material_types.h::MaterialDataLayout
namespace MaterialTextureSlots {
    // 与 MaterialDataLayout 保持一致
    constexpr int NormalMapTextureIndex = 10;
    constexpr int AlbedoTextureIndex = 11;
    constexpr int RoughnessTextureIndex = 12;
    constexpr uint32_t InvalidTextureIndex = 0xFFFFFFFF;
}

} // namespace shared
} // namespace vlr
