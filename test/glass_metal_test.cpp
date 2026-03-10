// 玻璃和金属材质测试
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

// 创建球体
static void createSphere(std::vector<float>& vertices, std::vector<uint32_t>& indices,
                        float cx, float cy, float cz, float radius,
                        int segments = 32, int rings = 16) {
    vertices.clear();
    indices.clear();
    
    for (int lat = 0; lat <= rings; ++lat) {
        float phi = PI * float(lat) / float(rings);
        float sinPhi = std::sin(phi);
        float cosPhi = std::cos(phi);
        
        for (int lon = 0; lon <= segments; ++lon) {
            float theta = 2.0f * PI * float(lon) / float(segments);
            float sinTheta = std::sin(theta);
            float cosTheta = std::cos(theta);
            
            float x = cx + radius * sinPhi * cosTheta;
            float y = cy + radius * cosPhi;
            float z = cz + radius * sinPhi * sinTheta;
            
            vertices.push_back(x);
            vertices.push_back(y);
            vertices.push_back(z);
        }
    }
    
    for (int lat = 0; lat < rings; ++lat) {
        for (int lon = 0; lon < segments; ++lon) {
            uint32_t first = lat * (segments + 1) + lon;
            uint32_t second = first + segments + 1;
            
            indices.push_back(first);
            indices.push_back(second);
            indices.push_back(first + 1);
            
            indices.push_back(second);
            indices.push_back(second + 1);
            indices.push_back(first + 1);
        }
    }
}

int main() {
    printf("=== Glass and Metal Material Test ===\n");
    
    VLRContext context = nullptr;
    VLRScene scene = nullptr;
    VLRResult res;
    
    res = vlrCreateContext(nullptr, 0, &context);
    if (res != VLRResult_Success) {
        printf("[Error] Failed to create context\n");
        return 1;
    }
    
    res = vlrCreateScene(context, &scene);
    if (res != VLRResult_Success) {
        printf("[Error] Failed to create scene\n");
        return 1;
    }
    
    // 材质
    float whiteColor[] = { 0.8f, 0.8f, 0.8f };
    float glassColor[] = { 0.999f, 0.999f, 0.999f };
    float lightEmission[] = { 200.0f, 200.0f, 200.0f };  // 非常强的光
    float etaGold[] = { 0.143f, 0.374f, 1.442f };
    float kappaGold[] = { 3.984f, 2.386f, 1.603f };
    
    VLRMaterial matWhite = nullptr;
    VLRMaterial matGlass = nullptr;
    VLRMaterial matGold = nullptr;
    VLRMaterial matLight = nullptr;
    
    res = vlrCreateMaterial(scene, 0, whiteColor, nullptr, &matWhite);
    if (res != VLRResult_Success) {
        printf("[Error] Failed to create white material\n");
        return 1;
    }
    
    // 玻璃材质: SpecularTransmission (type 6), IOR 1.5
    res = vlrCreateMaterialEx(scene, 6, glassColor, 0.0f, 0.0f, 1.5f, nullptr, &matGlass);
    if (res != VLRResult_Success) {
        printf("[Error] Failed to create glass material\n");
        return 1;
    }
    printf("[OK] Glass material created (type 6, IOR 1.5)\n");
    
    // 金属材质: Conductor
    res = vlrCreateMaterialConductor(scene, etaGold, kappaGold, 0.15f, &matGold);
    if (res != VLRResult_Success) {
        printf("[Error] Failed to create gold material\n");
        return 1;
    }
    printf("[OK] Gold material created\n");
    
    // 发光材质
    res = vlrCreateMaterial(scene, 0, whiteColor, lightEmission, &matLight);
    if (res != VLRResult_Success) {
        printf("[Error] Failed to create light material\n");
        return 1;
    }
    
    // 地面 (大平面)
    float floorVerts[] = {
        -5.0f, 0.0f,  5.0f,
        -5.0f, 0.0f, -5.0f,
         5.0f, 0.0f, -5.0f,
         5.0f, 0.0f,  5.0f
    };
    uint32_t floorIndices[] = { 0, 1, 2,  0, 2, 3 };
    
    // 背景墙 (大平面, z=-3)
    float backWallVerts[] = {
        -5.0f, 0.0f, -3.0f,
         5.0f, 0.0f, -3.0f,
         5.0f, 5.0f, -3.0f,
        -5.0f, 5.0f, -3.0f
    };
    uint32_t backWallIndices[] = { 0, 1, 2,  0, 2, 3 };
    
    // 区域光 (y=4, 2x2, 在球体上方)
    float lightVerts[] = {
        -1.0f, 4.0f, -1.0f,
         1.0f, 4.0f, -1.0f,
         1.0f, 4.0f,  1.0f,
        -1.0f, 4.0f,  1.0f
    };
    uint32_t lightIndices[] = { 0, 1, 2,  0, 2, 3 };
    
    // 创建球体
    std::vector<float> glassSphereVerts, goldSphereVerts;
    std::vector<uint32_t> glassSphereInds, goldSphereInds;
    
    createSphere(glassSphereVerts, glassSphereInds, -1.2f, 1.0f, 0.0f, 0.8f, 32, 16);  // 玻璃球(左)
    createSphere(goldSphereVerts, goldSphereInds, 1.2f, 1.0f, 0.0f, 0.8f, 32, 16);     // 金属球(右)
    
    VLRTriangleMesh meshFloor = nullptr;
    VLRTriangleMesh meshBackWall = nullptr;
    VLRTriangleMesh meshLight = nullptr;
    VLRTriangleMesh meshGlassSphere = nullptr;
    VLRTriangleMesh meshGoldSphere = nullptr;
    
    res = vlrCreateTriangleMesh(scene, floorVerts, 4, floorIndices, 2, matWhite, &meshFloor);
    if (res != VLRResult_Success) {
        printf("[Error] Failed to create floor mesh\n");
        return 1;
    }
    
    res = vlrCreateTriangleMesh(scene, backWallVerts, 4, backWallIndices, 2, matWhite, &meshBackWall);
    if (res != VLRResult_Success) {
        printf("[Error] Failed to create back wall mesh\n");
        return 1;
    }
    
    res = vlrCreateTriangleMesh(scene, lightVerts, 4, lightIndices, 2, matLight, &meshLight);
    if (res != VLRResult_Success) {
        printf("[Error] Failed to create light mesh\n");
        return 1;
    }
    
    res = vlrCreateTriangleMesh(scene, glassSphereVerts.data(), (uint32_t)(glassSphereVerts.size() / 3),
                               glassSphereInds.data(), (uint32_t)(glassSphereInds.size() / 3),
                               matGlass, &meshGlassSphere);
    if (res != VLRResult_Success) {
        printf("[Error] Failed to create glass sphere mesh\n");
        return 1;
    }
    
    res = vlrCreateTriangleMesh(scene, goldSphereVerts.data(), (uint32_t)(goldSphereVerts.size() / 3),
                               goldSphereInds.data(), (uint32_t)(goldSphereInds.size() / 3),
                               matGold, &meshGoldSphere);
    if (res != VLRResult_Success) {
        printf("[Error] Failed to create gold sphere mesh\n");
        return 1;
    }
    
    printf("[OK] All meshes created\n");
    
    // 创建实例
    float origin[] = { 0, 0, 0 };
    float scale[] = { 1, 1, 1 };
    float axis[] = { 0, 1, 0 };
    
    VLRInstance instFloor = nullptr;
    VLRInstance instBackWall = nullptr;
    VLRInstance instLight = nullptr;
    VLRInstance instGlassSphere = nullptr;
    VLRInstance instGoldSphere = nullptr;
    
    res = vlrCreateInstance(scene, meshFloor, origin, scale, axis, 0.0f, &instFloor);
    if (res != VLRResult_Success) return 1;
    
    res = vlrCreateInstance(scene, meshBackWall, origin, scale, axis, 0.0f, &instBackWall);
    if (res != VLRResult_Success) return 1;
    
    res = vlrCreateInstance(scene, meshLight, origin, scale, axis, 0.0f, &instLight);
    if (res != VLRResult_Success) return 1;
    
    res = vlrCreateInstance(scene, meshGlassSphere, origin, scale, axis, 0.0f, &instGlassSphere);
    if (res != VLRResult_Success) return 1;
    
    res = vlrCreateInstance(scene, meshGoldSphere, origin, scale, axis, 0.0f, &instGoldSphere);
    if (res != VLRResult_Success) return 1;
    
    res = vlrAddAreaLight(scene, instLight);
    if (res != VLRResult_Success) return 1;
    
    printf("[OK] Scene created with glass sphere (left) and gold sphere (right)\n");
    
    // 设置相机 - 从前方看两个球体
    VLRCameraParams camera = {};
    camera.position[0] = 0.0f;
    camera.position[1] = 1.5f;
    camera.position[2] = 4.0f;
    
    float targetX = 0.0f, targetY = 1.0f, targetZ = 0.0f;
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
    camera.fovY = 50.0f * PI / 180.0f;
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
    
    printf("[OK] Camera set: pos=(%.1f, %.1f, %.1f), target=(%.1f, %.1f, %.1f)\n",
           camera.position[0], camera.position[1], camera.position[2],
           targetX, targetY, targetZ);
    
    // 渲染
    uint32_t width = 512;
    uint32_t height = 512;
    uint32_t numSamples = 512;
    
    printf("[Rendering] %ux%u, %u samples...\n", width, height, numSamples);
    
    res = vlrRender(context, scene, width, height, numSamples, 3 /* WavefrontPathTracing */);
    if (res != VLRResult_Success) {
        printf("[Error] Render failed\n");
        return 1;
    }
    
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
    
    savePNG("glass_metal_test.png", width, height, outputBuffer.data(), numSamples);
    
    vlrDestroyScene(scene);
    vlrDestroyContext(context);
    
    printf("=== Test Complete ===\n");
    return 0;
}
