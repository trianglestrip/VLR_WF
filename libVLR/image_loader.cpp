// ============================================================================
// VLR Image Loader Implementation
// ============================================================================

#include "image_loader.h"
#include <cstdio>
#include <cstring>
#include <algorithm>

// stb_image 用于加载 HDR（不定义 IMPLEMENTATION，在 tinyexr_impl.cpp 中统一定义）
#include "stb/stb_image.h"

// tinyexr 用于加载 EXR（不定义 IMPLEMENTATION，在 tinyexr_impl.cpp 中编译）
#define TINYEXR_USE_MINIZ 0
#define TINYEXR_USE_STB_ZLIB 1
#pragma warning(push)
#pragma warning(disable: 4267 4244 4996)
#include "tinyexr/tinyexr.h"
#pragma warning(pop)

namespace vlr {

bool loadEXR(const char* filename, HDRImage& outImage, std::string* outError) {
    if (!filename) {
        if (outError) *outError = "Filename is null";
        return false;
    }
    
    float* rgba = nullptr;
    int width = 0, height = 0;
    const char* err = nullptr;
    
    int ret = LoadEXR(&rgba, &width, &height, filename, &err);
    
    if (ret != TINYEXR_SUCCESS) {
        if (outError && err) {
            *outError = std::string("LoadEXR failed: ") + err;
            FreeEXRErrorMessage(err);
        }
        return false;
    }
    
    // 转换 RGBA 到 RGB
    size_t numPixels = static_cast<size_t>(width) * height;
    float* rgb = (float*)malloc(numPixels * 3 * sizeof(float));
    if (!rgb) {
        free(rgba);
        if (outError) *outError = "Memory allocation failed";
        return false;
    }
    
    for (size_t i = 0; i < numPixels; ++i) {
        rgb[i * 3 + 0] = rgba[i * 4 + 0];
        rgb[i * 3 + 1] = rgba[i * 4 + 1];
        rgb[i * 3 + 2] = rgba[i * 4 + 2];
        // 忽略 alpha 通道
    }
    
    free(rgba);
    
    // 设置输出
    outImage.data = rgb;
    outImage.width = static_cast<uint32_t>(width);
    outImage.height = static_cast<uint32_t>(height);
    
    return true;
}

bool loadHDR(const char* filename, HDRImage& outImage, std::string* outError) {
    if (!filename) {
        if (outError) *outError = "Filename is null";
        return false;
    }
    
    int width = 0, height = 0, channels = 0;
    float* data = stbi_loadf(filename, &width, &height, &channels, 3); // 强制 RGB
    
    if (!data) {
        if (outError) {
            *outError = std::string("stbi_loadf failed: ") + stbi_failure_reason();
        }
        return false;
    }
    
    outImage.data = data;
    outImage.width = static_cast<uint32_t>(width);
    outImage.height = static_cast<uint32_t>(height);
    
    return true;
}

bool loadHDRImage(const char* filename, HDRImage& outImage, std::string* outError) {
    if (!filename) {
        if (outError) *outError = "Filename is null";
        return false;
    }
    
    // 检测文件扩展名
    size_t len = strlen(filename);
    if (len < 4) {
        if (outError) *outError = "Invalid filename";
        return false;
    }
    
    const char* ext = filename + len - 4;
    if (strcmp(ext, ".exr") == 0 || strcmp(ext, ".EXR") == 0) {
        return loadEXR(filename, outImage, outError);
    } else if (strcmp(ext, ".hdr") == 0 || strcmp(ext, ".HDR") == 0) {
        return loadHDR(filename, outImage, outError);
    } else {
        if (outError) *outError = std::string("Unsupported format: ") + ext;
        return false;
    }
}

// ============================================================================
// 纹理图像加载（PNG/JPG/EXR/HDR）
// ============================================================================

bool loadTextureImage(const char* filename, TextureImage& outImage, std::string* outError) {
    if (!filename) {
        if (outError) *outError = "Filename is null";
        return false;
    }

    size_t len = strlen(filename);
    if (len < 4) {
        if (outError) *outError = "Invalid filename";
        return false;
    }

    const char* ext = filename + len - 4;
    const char* ext5 = (len >= 5) ? filename + len - 5 : nullptr;

    // PNG, JPG, JPEG - 使用 stb_image
    if (strcmp(ext, ".png") == 0 || strcmp(ext, ".PNG") == 0 ||
        strcmp(ext, ".jpg") == 0 || strcmp(ext, ".JPG") == 0 ||
        (ext5 && (strcmp(ext5, ".jpeg") == 0 || strcmp(ext5, ".JPEG") == 0))) {
        int width = 0, height = 0, channels = 0;
        unsigned char* data = stbi_load(filename, &width, &height, &channels, 4);
        if (!data) {
            if (outError) *outError = std::string("stbi_load failed: ") + stbi_failure_reason();
            return false;
        }
        outImage.data = data;
        outImage.width = static_cast<uint32_t>(width);
        outImage.height = static_cast<uint32_t>(height);
        outImage.format = TextureImageFormat::RGBA8;
        return true;
    }

    // EXR, HDR - 使用现有加载器
    if (strcmp(ext, ".exr") == 0 || strcmp(ext, ".EXR") == 0) {
        HDRImage hdr;
        if (!loadEXR(filename, hdr, outError))
            return false;
        // HDRImage 是 RGB，转为 RGBA32F（alpha=1）
        size_t numPixels = static_cast<size_t>(hdr.width) * hdr.height;
        float* rgba = (float*)malloc(numPixels * 4 * sizeof(float));
        if (!rgba) {
            if (outError) *outError = "Memory allocation failed";
            return false;
        }
        for (size_t i = 0; i < numPixels; ++i) {
            rgba[i * 4 + 0] = hdr.data[i * 3 + 0];
            rgba[i * 4 + 1] = hdr.data[i * 3 + 1];
            rgba[i * 4 + 2] = hdr.data[i * 3 + 2];
            rgba[i * 4 + 3] = 1.0f;
        }
        free(hdr.data);
        hdr.data = nullptr;
        outImage.data = rgba;
        outImage.width = hdr.width;
        outImage.height = hdr.height;
        outImage.format = TextureImageFormat::RGBA32F;
        return true;
    }

    if (strcmp(ext, ".hdr") == 0 || strcmp(ext, ".HDR") == 0) {
        HDRImage hdr;
        if (!loadHDR(filename, hdr, outError))
            return false;
        size_t numPixels = static_cast<size_t>(hdr.width) * hdr.height;
        float* rgba = (float*)malloc(numPixels * 4 * sizeof(float));
        if (!rgba) {
            if (outError) *outError = "Memory allocation failed";
            return false;
        }
        for (size_t i = 0; i < numPixels; ++i) {
            rgba[i * 4 + 0] = hdr.data[i * 3 + 0];
            rgba[i * 4 + 1] = hdr.data[i * 3 + 1];
            rgba[i * 4 + 2] = hdr.data[i * 3 + 2];
            rgba[i * 4 + 3] = 1.0f;
        }
        free(hdr.data);
        hdr.data = nullptr;
        outImage.data = rgba;
        outImage.width = hdr.width;
        outImage.height = hdr.height;
        outImage.format = TextureImageFormat::RGBA32F;
        return true;
    }

    if (outError) *outError = std::string("Unsupported texture format: ") + ext;
    return false;
}

void freeTextureImage(TextureImage& image) {
    if (image.data) {
        if (image.format == TextureImageFormat::RGBA8) {
            stbi_image_free(image.data);
        } else {
            free(image.data);
        }
        image.data = nullptr;
        image.width = 0;
        image.height = 0;
    }
}

} // namespace vlr
