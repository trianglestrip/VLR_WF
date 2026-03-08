// ============================================================================
// VLR Image Loader - EXR/HDR Image Loading Utility
//
// 本文件提供 EXR 和 HDR 图像加载功能，用于环境光贴图（IBL）
//
// 作者：VLR 开发团队
// 创建日期：2026-03-08
// 环境：CUDA 13.1, OptiX 8.0.0, VS2022
// ============================================================================

#pragma once

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

namespace vlr {

/// HDR 图像数据
struct HDRImage {
    float* data;           ///< RGB float 数据（线性空间）
    uint32_t width;        ///< 图像宽度
    uint32_t height;       ///< 图像高度
    
    HDRImage() : data(nullptr), width(0), height(0) {}
    
    ~HDRImage() {
        if (data) {
            free(data);
            data = nullptr;
        }
    }
    
    // 禁止拷贝
    HDRImage(const HDRImage&) = delete;
    HDRImage& operator=(const HDRImage&) = delete;
    
    // 支持移动
    HDRImage(HDRImage&& other) noexcept
        : data(other.data), width(other.width), height(other.height) {
        other.data = nullptr;
        other.width = 0;
        other.height = 0;
    }
    
    HDRImage& operator=(HDRImage&& other) noexcept {
        if (this != &other) {
            if (data) free(data);
            data = other.data;
            width = other.width;
            height = other.height;
            other.data = nullptr;
            other.width = 0;
            other.height = 0;
        }
        return *this;
    }
    
    bool isValid() const { return data != nullptr && width > 0 && height > 0; }
};

/// 加载 EXR 文件
/// @param filename EXR 文件路径
/// @param outImage 输出图像数据
/// @param outError 错误信息（可选）
/// @return 成功返回 true
bool loadEXR(const char* filename, HDRImage& outImage, std::string* outError = nullptr);

/// 加载 HDR 文件（Radiance .hdr 格式）
/// @param filename HDR 文件路径
/// @param outImage 输出图像数据
/// @param outError 错误信息（可选）
/// @return 成功返回 true
bool loadHDR(const char* filename, HDRImage& outImage, std::string* outError = nullptr);

/// 自动检测格式并加载（根据扩展名）
/// @param filename 图像文件路径（.exr 或 .hdr）
/// @param outImage 输出图像数据
/// @param outError 错误信息（可选）
/// @return 成功返回 true
bool loadHDRImage(const char* filename, HDRImage& outImage, std::string* outError = nullptr);

// ============================================================================
// 纹理图像加载（PNG/JPG/EXR/HDR）
// ============================================================================

/// 纹理图像格式（与 API format 参数对应）
enum class TextureImageFormat : uint32_t {
    RGBA8 = 0,      ///< 8 位 RGBA（PNG/JPG）
    RGB32F = 1,      ///< 32 位浮点 RGB（EXR/HDR）
    RGBA32F = 2,     ///< 32 位浮点 RGBA
};

/// 纹理图像数据（用于 vlrCreateTexture2D）
struct TextureImage {
    void* data;              ///< 像素数据（RGBA8: uint8_t*, 浮点: float*）
    uint32_t width;
    uint32_t height;
    TextureImageFormat format;

    TextureImage() : data(nullptr), width(0), height(0), format(TextureImageFormat::RGBA8) {}
};

/// 加载纹理图像（支持 PNG, JPG, EXR, HDR）
/// @param filename 图像文件路径
/// @param outImage 输出图像数据（调用者需用 freeTextureImage 释放 data）
/// @param outError 错误信息（可选）
/// @return 成功返回 true
bool loadTextureImage(const char* filename, TextureImage& outImage, std::string* outError = nullptr);

/// 释放 loadTextureImage 分配的数据
void freeTextureImage(TextureImage& image);

} // namespace vlr
