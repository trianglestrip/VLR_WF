// 简单的区域光测试 - 修复版
#include <vlr/vlr.h>
#include <cuda_runtime.h>
#include <cstdio>
#include <cmath>
#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

static const float PI = 3.14159265358979323846f;

static void savePNG(const char* filename, uint32_t width, uint32_t height,
                    const float* rgb, uint32_t numSamples) {
    std::vector<unsigned char> pixels(width * height * 3);
    float invSamples = 1.0f / (float)numSamples;
    
    for (uint32_t y = 0; y < height; ++y) {
        uint32_t srcY = height - 1 - y;
        for (uint32_t x = 0; x < width; ++x) {
            uint32_t srcIdx = (srcY * width + x) * 3;
            uint32_t dstIdx = (y * width + x) * 3;
            
            float r = rgb[srcIdx + 0] * invSamples;
            float g = rgb[srcIdx + 1] * invSamples;
            float b = rgb[srcIdx + 2] * invSamples;
            
            r = r < 0 ? 0 : (r > 1 ? 1 : r);
            g = g < 0 ? 0 : (g > 1 ? 1 : g);
            b = b < 0 ? 0 : (b > 1 ? 1 : b);
            
            r = powf(r, 1.0f / 2.2f);
            g = powf(g, 1.0f / 2.2f);
            b = powf(b, 1.0f / 2.2f);
            
            pixels[dstIdx + 0] = (unsigned char)(r * 255.99f);
            pixels[dstIdx + 1] = (unsigned char)(g * 255.99f);
            pixels[dstIdx + 2] = (unsigned char)(b * 255.99f);
        }
    }
    
    stbi_write_png(filename, width, height, 3, pixels.data(), width * 3);
    printf("[Done] Image saved: %s\n", filename);
}

int main() {
    printf("=== Simple Area Light Test (Fixed) ===\n");
    
    VLRContext context = nullptr;
    VLRScene scene = nullptr;
    VLRResult res;
    
    // 创建context
    res = vlrCreateContext(nullptr, 0, &context);
    if (res != VLRResult_Success) {
        printf("[Error] Failed to create context\n");
        return 1;
    }
    
    // 创建scene
    res = vlrCreateScene(context, &scene);
    if (res != VLRResult_Success) {
        printf("[Error] Failed to create scene\n");
        return 1;
    }
    
    // 材质
    float whiteColor[] = { 0.8f, 0.8f, 0.8f };
    float lightEmission[] = { 50.0f, 50.0f, 50.0f };
    
    VLRMaterial matWhite = nullptr;
    VLRMaterial matLight = nullptr;
    
    res = vlrCreateMaterial(scene, 0, whiteColor, nullptr, &matWhite);
    if (res != VLRResult_Success) {
        printf("[Error] Failed to create white material\n");
        return 1;
    }
    
    res = vlrCreateMaterial(scene, 0, whiteColor, lightEmission, &matLight);
    if (res != VLRResult_Success) {
        printf("[Error] Failed to create light material\n");
        return 1;
    }
    
    // 地面 (y=0, 4x4)
    float floorVerts[] = {
        -2.0f, 0.0f,  2.0f,
        -2.0f, 0.0f, -2.0f,
         2.0f, 0.0f, -2.0f,
         2.0f, 0.0f,  2.0f
    };
    uint32_t floorIndices[] = { 0, 1, 2,  0, 2, 3 };
    
    // 区域光 (y=2, 1x1, 在中心上方)
    float lightVerts[] = {
        -0.5f, 2.0f, -0.5f,
         0.5f, 2.0f, -0.5f,
         0.5f, 2.0f,  0.5f,
        -0.5f, 2.0f,  0.5f
    };
    uint32_t lightIndices[] = { 0, 1, 2,  0, 2, 3 };
    
    VLRTriangleMesh meshFloor = nullptr;
    VLRTriangleMesh meshLight = nullptr;
    
    res = vlrCreateTriangleMesh(scene, floorVerts, 4, floorIndices, 2, matWhite, &meshFloor);
    if (res != VLRResult_Success) {
        printf("[Error] Failed to create floor mesh\n");
        return 1;
    }
    
    res = vlrCreateTriangleMesh(scene, lightVerts, 4, lightIndices, 2, matLight, &meshLight);
    if (res != VLRResult_Success) {
        printf("[Error] Failed to create light mesh\n");
        return 1;
    }
    
    // 创建实例
    float origin[] = { 0, 0, 0 };
    float scale[] = { 1, 1, 1 };
    float axis[] = { 0, 1, 0 };
    
    VLRInstance instFloor = nullptr;
    VLRInstance instLight = nullptr;
    
    res = vlrCreateInstance(scene, meshFloor, origin, scale, axis, 0.0f, &instFloor);
    if (res != VLRResult_Success) {
        printf("[Error] Failed to create floor instance\n");
        return 1;
    }
    
    res = vlrCreateInstance(scene, meshLight, origin, scale, axis, 0.0f, &instLight);
    if (res != VLRResult_Success) {
        printf("[Error] Failed to create light instance\n");
        return 1;
    }
    
    // 添加区域光
    res = vlrAddAreaLight(scene, instLight);
    if (res != VLRResult_Success) {
        printf("[Error] Failed to add area light\n");
        return 1;
    }
    
    printf("[OK] Scene created with floor and area light\n");
    
    // 设置相机
    VLRCameraParams camera = {};
    camera.position[0] = 0.0f;
    camera.position[1] = 1.0f;
    camera.position[2] = 3.0f;
    
    float targetX = 0.0f, targetY = 0.5f, targetZ = 0.0f;
    float dx = targetX - camera.position[0];
    float dy = targetY - camera.position[1];
    float dz = targetZ - camera.position[2];
    float len = std::sqrt(dx*dx + dy*dy + dz*dz);
    
    camera.direction[0] = dx / len;
    camera.direction[1] = dy / len;
    camera.direction[2] = dz / len;
    camera.up[0] = 0.0f;
    camera.up[1] = 1.0f;
    camera.up[2] = 0.0f;
    camera.fovY = 45.0f * PI / 180.0f;
    camera.aspect = 1.0f;
    camera.lensRadius = 0.0f;
    camera.focusDistance = 1.0f;
    camera.focalLength = 0.0f;
    camera.cameraType = 0;
    
    res = vlrSetCamera(scene, &camera);
    if (res != VLRResult_Success) {
        printf("[Error] Failed to set camera\n");
        return 1;
    }
    
    printf("[OK] Camera set: pos=(%.1f, %.1f, %.1f), dir=(%.2f, %.2f, %.2f)\n",
           camera.position[0], camera.position[1], camera.position[2],
           camera.direction[0], camera.direction[1], camera.direction[2]);
    
    // 渲染
    uint32_t width = 512;
    uint32_t height = 512;
    uint32_t numSamples = 128;
    
    printf("[Rendering] %ux%u, %u samples...\n", width, height, numSamples);
    
    res = vlrRender(context, scene, width, height, numSamples, 3 /* VLRRenderer_WavefrontPathTracing */);
    if (res != VLRResult_Success) {
        printf("[Error] Render failed\n");
        return 1;
    }
    
    // 获取结果
    void* deviceBuffer = vlrGetOutputBuffer(context);
    if (!deviceBuffer) {
        printf("[Error] Failed to get output buffer\n");
        return 1;
    }
    
    size_t bufferSize = width * height * sizeof(float) * 3;
    std::vector<float> outputBuffer(width * height * 3);
    cudaError_t cudaErr = cudaMemcpy(outputBuffer.data(), deviceBuffer, bufferSize, cudaMemcpyDeviceToHost);
    if (cudaErr != cudaSuccess) {
        printf("[Error] cudaMemcpy failed\n");
        return 1;
    }
    
    savePNG("simple_area_light_fixed.png", width, height, outputBuffer.data(), numSamples);
    
    // 清理
    vlrDestroyScene(scene);
    vlrDestroyContext(context);
    
    printf("=== Test Complete ===\n");
    return 0;
}
