// ============================================================================
// VLR 测试工具 - 图像 I/O 与色调映射
// ============================================================================

#pragma once

#include <cstdint>
#include <cstdio>
#include <cmath>
#include <vector>

#ifndef STB_IMAGE_WRITE_INCLUDED
#include "stb_image_write.h"
#endif

namespace vlr_test {

enum class ToneMapMode {
    None = 0,
    ACES = 1,
    Reinhard = 2
};

static inline float saturate(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

static inline float toneMapACES(float x) {
    const float a = 2.51f;
    const float b = 0.03f;
    const float c = 2.43f;
    const float d = 0.59f;
    const float e = 0.14f;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

static inline float toneMapReinhard(float x) { return x / (1.0f + x); }

static void savePNG(const char* filename, uint32_t width, uint32_t height,
                    const float* rgb, uint32_t numSamples, float exposure,
                    ToneMapMode toneMap = ToneMapMode::ACES) {
    std::vector<unsigned char> pixels(width * height * 3);
    float invSamples = (numSamples > 0) ? (1.0f / (float)numSamples) : 1.0f;

    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            uint32_t idx = (y * width + x) * 3;

            float r = fmaxf(0.0f, rgb[idx + 0] * invSamples * exposure);
            float g = fmaxf(0.0f, rgb[idx + 1] * invSamples * exposure);
            float b = fmaxf(0.0f, rgb[idx + 2] * invSamples * exposure);

            switch (toneMap) {
            case ToneMapMode::ACES:
                r = toneMapACES(r); g = toneMapACES(g); b = toneMapACES(b);
                break;
            case ToneMapMode::Reinhard:
                r = toneMapReinhard(r); g = toneMapReinhard(g); b = toneMapReinhard(b);
                break;
            default:
                r = saturate(r); g = saturate(g); b = saturate(b);
                break;
            }

            r = powf(r, 1.0f / 2.2f);
            g = powf(g, 1.0f / 2.2f);
            b = powf(b, 1.0f / 2.2f);

            pixels[idx + 0] = (unsigned char)(r * 255.99f);
            pixels[idx + 1] = (unsigned char)(g * 255.99f);
            pixels[idx + 2] = (unsigned char)(b * 255.99f);
        }
    }

    if (stbi_write_png(filename, width, height, 3, pixels.data(), width * 3)) {
        printf("[Done] Image saved: %s (%u x %u)\n", filename, width, height);
    } else {
        fprintf(stderr, "[Error] Failed to save PNG: %s\n", filename);
    }
}

} // namespace vlr_test
