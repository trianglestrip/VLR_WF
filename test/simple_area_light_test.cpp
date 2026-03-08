#include <vlr/vlr.h>
#include <cuda_runtime.h>
#include <stdio.h>
#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../external/stb/stb_image_write.h"

int main() {
    printf("=== Simple Area Light Test ===\n\n");
    
    VLRContext context = nullptr;
    VLRScene scene = nullptr;
    
    VLRResult res = vlrCreateContext(nullptr, 1, &context);
    if (res != VLRResult_Success) {
        fprintf(stderr, "Failed to create context\n");
        return 1;
    }
    
    res = vlrCreateScene(context, &scene);
    if (res != VLRResult_Success) {
        fprintf(stderr, "Failed to create scene\n");
        return 1;
    }
    
    // 创建简单的发光平面
    float lightVerts[] = {
        -0.5f, 2.9f, -0.5f,
         0.5f, 2.9f, -0.5f,
         0.5f, 2.9f,  0.5f,
        -0.5f, 2.9f,  0.5f
    };
    uint32_t lightInds[] = { 0, 1, 2,  0, 2, 3 };
    
    // 创建发光材质
    float whiteColor[] = { 0.8f, 0.8f, 0.8f };
    float emission[] = { 80.0f, 80.0f, 80.0f };
    VLRMaterial matLight = nullptr;
    res = vlrCreateMaterial(scene, 0, whiteColor, emission, &matLight);
    if (res != VLRResult_Success) {
        fprintf(stderr, "Failed to create material\n");
        return 1;
    }
    
    VLRTriangleMesh meshLight = nullptr;
    res = vlrCreateTriangleMesh(scene, lightVerts, 4, lightInds, 2, matLight, &meshLight);
    if (res != VLRResult_Success) {
        fprintf(stderr, "Failed to create mesh\n");
        return 1;
    }
    
    float origin[] = { 0, 0, 0 };
    float scale[] = { 1, 1, 1 };
    float axis[] = { 0, 1, 0 };
    VLRInstance instLight = nullptr;
    res = vlrCreateInstance(scene, meshLight, origin, scale, axis, 0.0f, &instLight);
    if (res != VLRResult_Success) {
        fprintf(stderr, "Failed to create instance\n");
        return 1;
    }
    
    // 添加区域光
    res = vlrAddAreaLight(scene, instLight);
    if (res != VLRResult_Success) {
        fprintf(stderr, "Failed to add area light\n");
        return 1;
    }
    
    printf("Area light added successfully\n");
    
    // 创建地板
    float floorVerts[] = {
        -1.5f, 0.0f,  1.5f,
        -1.5f, 0.0f, -1.5f,
         1.5f, 0.0f, -1.5f,
         1.5f, 0.0f,  1.5f
    };
    uint32_t floorInds[] = { 0, 1, 2,  0, 2, 3 };
    
    float grayColor[] = { 0.5f, 0.5f, 0.5f };
    VLRMaterial matFloor = nullptr;
    res = vlrCreateMaterial(scene, 0, grayColor, nullptr, &matFloor);
    
    VLRTriangleMesh meshFloor = nullptr;
    res = vlrCreateTriangleMesh(scene, floorVerts, 4, floorInds, 2, matFloor, &meshFloor);
    
    VLRInstance instFloor = nullptr;
    res = vlrCreateInstance(scene, meshFloor, origin, scale, axis, 0.0f, &instFloor);
    
    // 设置相机
    VLRCameraParams camera = {};
    camera.position[0] = 0.0f;
    camera.position[1] = 1.5f;
    camera.position[2] = 4.0f;
    camera.direction[0] = 0.0f;
    camera.direction[1] = 0.0f;
    camera.direction[2] = -1.0f;
    camera.up[0] = 0.0f;
    camera.up[1] = 1.0f;
    camera.up[2] = 0.0f;
    camera.fovY = 45.0f * 3.14159f / 180.0f;
    camera.aspect = 1.0f;
    camera.lensRadius = 0.0f;
    camera.focusDistance = 1.0f;
    camera.focalLength = 0.0f;
    camera.cameraType = 0;
    
    res = vlrSetCamera(scene, &camera);
    if (res != VLRResult_Success) {
        fprintf(stderr, "Failed to set camera\n");
        return 1;
    }
    
    printf("Camera set successfully\n");
    
    // 渲染
    uint32_t width = 512, height = 512, numSamples = 64;
    printf("Rendering %ux%u, %u samples...\n", width, height, numSamples);
    
    res = vlrRender(context, scene, width, height, numSamples, 0);
    if (res != VLRResult_Success) {
        fprintf(stderr, "Render failed: %d\n", res);
        return 1;
    }
    
    printf("Render completed\n");
    
    void* deviceBuffer = vlrGetOutputBuffer(context);
    if (!deviceBuffer) {
        fprintf(stderr, "Failed to get output buffer\n");
        return 1;
    }
    
    size_t bufferSize = width * height * sizeof(float) * 3;
    std::vector<float> outputBuffer(width * height * 3);
    
    cudaError_t cudaErr = cudaMemcpy(outputBuffer.data(), deviceBuffer, bufferSize, cudaMemcpyDeviceToHost);
    if (cudaErr != cudaSuccess) {
        fprintf(stderr, "cudaMemcpy failed: %s\n", cudaGetErrorString(cudaErr));
        return 1;
    }
    
    // 检查前10个像素的值
    printf("\nFirst 10 pixels (RGB):\n");
    for (int i = 0; i < 10; ++i) {
        float r = outputBuffer[i * 3 + 0];
        float g = outputBuffer[i * 3 + 1];
        float b = outputBuffer[i * 3 + 2];
        printf("  Pixel %d: (%.6f, %.6f, %.6f)\n", i, r, g, b);
    }
    
    // 保存图像
    std::vector<unsigned char> pixels(width * height * 3);
    float exposure = 1.0f;
    float invSamples = 1.0f / (float)numSamples;
    
    for (uint32_t i = 0; i < width * height; ++i) {
        float r = outputBuffer[i * 3 + 0] * invSamples * exposure;
        float g = outputBuffer[i * 3 + 1] * invSamples * exposure;
        float b = outputBuffer[i * 3 + 2] * invSamples * exposure;
        
        r = r < 0 ? 0 : (r > 1 ? 1 : r);
        g = g < 0 ? 0 : (g > 1 ? 1 : g);
        b = b < 0 ? 0 : (b > 1 ? 1 : b);
        
        r = powf(r, 1.0f / 2.2f);
        g = powf(g, 1.0f / 2.2f);
        b = powf(b, 1.0f / 2.2f);
        
        pixels[i * 3 + 0] = (unsigned char)(r * 255.99f);
        pixels[i * 3 + 1] = (unsigned char)(g * 255.99f);
        pixels[i * 3 + 2] = (unsigned char)(b * 255.99f);
    }
    
    if (stbi_write_png("simple_area_light.png", width, height, 3, pixels.data(), width * 3)) {
        printf("\n[Done] Image saved: simple_area_light.png\n");
    } else {
        fprintf(stderr, "\n[Error] Failed to save PNG\n");
    }
    
    vlrDestroyScene(scene);
    vlrDestroyContext(context);
    
    printf("\n=== Test Complete ===\n");
    return 0;
}
