// ============================================================================
// VLR Wavefront - Light System Test
//
// 测试内容：
// 1. 区域光（Area Light）- Cornell Box + 区域光
// 2. 点光源（Point Light）- Cornell Box + 中心点光源
// 3. 方向光（Directional Light）- 平面 + 方向光（模拟太阳光）
// 4. 环境光（Environment Light）- 球体 + HDR 环境贴图
// 5. 混合光源 - 区域光 + 点光源 + 环境光
//
// 使用 API：vlrAddAreaLight, vlrAddPointLight, vlrAddDirectionalLight,
//          vlrSetEnvironmentLight, vlrSetEnvironmentLightFromImage
//
// Author: VLR Development Team
// Created: 2026-03-08
// ============================================================================

#include <vlr/vlr.h>
#include <cuda_runtime.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <vector>
#include <string>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

static const float PI = 3.14159265358979323846f;
static const float TWO_PI = 6.28318530717958647692f;

// ============================================================================
// Helper: Save PNG Image
// ============================================================================
static void savePNG(const char* filename, uint32_t width, uint32_t height,
                    const float* rgb, uint32_t numSamples, float exposure) {
    std::vector<unsigned char> pixels(width * height * 3);
    float invSamples = (numSamples > 0) ? (1.0f / (float)numSamples) : 1.0f;

    for (uint32_t y = 0; y < height; ++y) {
        uint32_t srcY = height - 1 - y;
        for (uint32_t x = 0; x < width; ++x) {
            uint32_t srcIdx = (srcY * width + x) * 3;
            uint32_t dstIdx = (y * width + x) * 3;

            float r = rgb[srcIdx + 0] * invSamples * exposure;
            float g = rgb[srcIdx + 1] * invSamples * exposure;
            float b = rgb[srcIdx + 2] * invSamples * exposure;

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

    if (stbi_write_png(filename, width, height, 3, pixels.data(), width * 3)) {
        printf("[Done] Image saved: %s (%u x %u)\n", filename, width, height);
    } else {
        fprintf(stderr, "[Error] Failed to save PNG: %s\n", filename);
    }
}

// ============================================================================
// Geometry Helpers
// ============================================================================

/// UV sphere: center (cx,cy,cz), radius
static void createSphere(std::vector<float>& vertices, std::vector<uint32_t>& indices,
                        float cx, float cy, float cz, float radius,
                        int segments = 64, int rings = 48) {
    vertices.clear();
    indices.clear();

    for (int lat = 0; lat <= rings; ++lat) {
        float phi = PI * float(lat) / float(rings);
        float sinPhi = std::sin(phi);
        float cosPhi = std::cos(phi);

        for (int lon = 0; lon <= segments; ++lon) {
            float theta = TWO_PI * float(lon) / float(segments);
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

            if (lat != 0) {
                indices.push_back(first);
                indices.push_back(first + 1);
                indices.push_back(second);
            }
            if (lat != rings - 1) {
                indices.push_back(first + 1);
                indices.push_back(second + 1);
                indices.push_back(second);
            }
        }
    }
}

// ============================================================================
// Cornell Box Geometry (3x3x3)
// ============================================================================
static const float kFloorVertices[] = {
    -1.5f, 0.0f, 1.5f,  -1.5f, 0.0f, -1.5f,  1.5f, 0.0f, -1.5f,  1.5f, 0.0f, 1.5f
};
static const uint32_t kFloorIndices[] = { 0, 1, 2,  0, 2, 3 };

static const float kCeilingVertices[] = {
    -1.5f, 3.0f, -1.5f,  -1.5f, 3.0f, 1.5f,  1.5f, 3.0f, 1.5f,  1.5f, 3.0f, -1.5f
};
static const uint32_t kCeilingIndices[] = { 0, 1, 2,  0, 2, 3 };

static const float kBackWallVertices[] = {
    -1.5f, 0.0f, -1.5f,  1.5f, 0.0f, -1.5f,  1.5f, 3.0f, -1.5f,  -1.5f, 3.0f, -1.5f
};
static const uint32_t kBackWallIndices[] = { 0, 1, 2,  0, 2, 3 };

static const float kLeftWallVertices[] = {
    -1.5f, 0.0f, 1.5f,  -1.5f, 0.0f, -1.5f,  -1.5f, 3.0f, -1.5f,  -1.5f, 3.0f, 1.5f
};
static const uint32_t kLeftWallIndices[] = { 0, 1, 2,  0, 2, 3 };

static const float kRightWallVertices[] = {
    1.5f, 0.0f, -1.5f,  1.5f, 0.0f, 1.5f,  1.5f, 3.0f, 1.5f,  1.5f, 3.0f, -1.5f
};
static const uint32_t kRightWallIndices[] = { 0, 1, 2,  0, 2, 3 };

static const float kFrontWallVertices[] = {
    1.5f, 0.0f, 1.5f,  -1.5f, 0.0f, 1.5f,  -1.5f, 3.0f, 1.5f,  1.5f, 3.0f, 1.5f
};
static const uint32_t kFrontWallIndices[] = { 0, 1, 2,  0, 2, 3 };

static const float kLightVertices[] = {
    -0.5f, 2.9f, -0.5f,  0.5f, 2.9f, -0.5f,  0.5f, 2.9f, 0.5f,  -0.5f, 2.9f, 0.5f
};
static const uint32_t kLightIndices[] = { 0, 1, 2,  0, 2, 3 };

// Large ground plane for directional light test (y=0, 10x10)
static const float kGroundPlaneVertices[] = {
    -5.0f, 0.0f,  5.0f,  -5.0f, 0.0f, -5.0f,  5.0f, 0.0f, -5.0f,  5.0f, 0.0f,  5.0f
};
static const uint32_t kGroundPlaneIndices[] = { 0, 1, 2,  0, 2, 3 };

// ============================================================================
// Run single light test - generic render helper
// ============================================================================
static int runLightTest(
    VLRContext context, VLRScene scene,
    uint32_t width, uint32_t height, uint32_t numSamples, float exposure,
    const char* outputFile)
{
    VLRResult res = vlrRender(context, scene, width, height, numSamples, vlr::VLRRenderer_WavefrontPathTracing);
    if (res != VLRResult_Success) return 1;

    void* deviceBuffer = vlrGetOutputBuffer(context);
    if (!deviceBuffer) return 1;

    size_t bufferSize = width * height * sizeof(float) * 3;
    std::vector<float> outputBuffer(width * height * 3);
    cudaError_t cudaErr = cudaMemcpy(outputBuffer.data(), deviceBuffer, bufferSize, cudaMemcpyDeviceToHost);
    if (cudaErr != cudaSuccess) return 1;

    savePNG(outputFile, width, height, outputBuffer.data(), numSamples, exposure);
    return 0;
}

// ============================================================================
// Test 1: Area Light - Cornell Box + 区域光
// ============================================================================
static int testAreaLight(VLRContext context, VLRScene scene,
    uint32_t width, uint32_t height, uint32_t numSamples, float exposure)
{
    VLRResult res = VLRResult_Success;
    float origin[] = { 0, 0, 0 };
    float scale[] = { 1, 1, 1 };
    float axis[] = { 0, 1, 0 };

    float whiteColor[] = { 0.522f, 0.522f, 0.522f };
    float redColor[] = { 0.522f, 0.0508f, 0.0508f };
    float blueColor[] = { 0.0508f, 0.0508f, 0.522f };
    float blackColor[] = { 0.0508f, 0.0508f, 0.0508f };
    float lightEmission[] = { 80.0f, 80.0f, 80.0f };

    VLRMaterial matWhite = nullptr, matRed = nullptr, matBlue = nullptr;
    VLRMaterial matFloor = nullptr, matLight = nullptr;

    res = vlrCreateMaterial(scene, 0, whiteColor, nullptr, &matWhite);
    if (res != VLRResult_Success) return 1;
    res = vlrCreateMaterial(scene, 0, redColor, nullptr, &matRed);
    if (res != VLRResult_Success) return 1;
    res = vlrCreateMaterial(scene, 0, blueColor, nullptr, &matBlue);
    if (res != VLRResult_Success) return 1;
    res = vlrCreateMaterialCheckerboard(scene, blackColor, whiteColor, 20, 1.5f, &matFloor);
    if (res != VLRResult_Success) return 1;
    res = vlrCreateMaterial(scene, 0, whiteColor, lightEmission, &matLight);
    if (res != VLRResult_Success) return 1;

    VLRTriangleMesh meshFloor = nullptr, meshCeiling = nullptr, meshBackWall = nullptr;
    VLRTriangleMesh meshLeftWall = nullptr, meshRightWall = nullptr, meshFrontWall = nullptr;
    VLRTriangleMesh meshLight = nullptr;

    res = vlrCreateTriangleMesh(scene, kFloorVertices, 4, kFloorIndices, 2, matFloor, &meshFloor);
    if (res != VLRResult_Success) return 1;
    res = vlrCreateTriangleMesh(scene, kCeilingVertices, 4, kCeilingIndices, 2, matWhite, &meshCeiling);
    if (res != VLRResult_Success) return 1;
    res = vlrCreateTriangleMesh(scene, kBackWallVertices, 4, kBackWallIndices, 2, matWhite, &meshBackWall);
    if (res != VLRResult_Success) return 1;
    res = vlrCreateTriangleMesh(scene, kLeftWallVertices, 4, kLeftWallIndices, 2, matBlue, &meshLeftWall);
    if (res != VLRResult_Success) return 1;
    res = vlrCreateTriangleMesh(scene, kRightWallVertices, 4, kRightWallIndices, 2, matRed, &meshRightWall);
    if (res != VLRResult_Success) return 1;
    res = vlrCreateTriangleMesh(scene, kFrontWallVertices, 4, kFrontWallIndices, 2, matWhite, &meshFrontWall);
    if (res != VLRResult_Success) return 1;
    res = vlrCreateTriangleMesh(scene, kLightVertices, 4, kLightIndices, 2, matLight, &meshLight);
    if (res != VLRResult_Success) return 1;

    VLRInstance instFloor = nullptr, instCeiling = nullptr, instBackWall = nullptr;
    VLRInstance instLeftWall = nullptr, instRightWall = nullptr, instFrontWall = nullptr;
    VLRInstance instLight = nullptr;

    res = vlrCreateInstance(scene, meshFloor, origin, scale, axis, 0.0f, &instFloor);
    if (res != VLRResult_Success) return 1;
    res = vlrCreateInstance(scene, meshCeiling, origin, scale, axis, 0.0f, &instCeiling);
    if (res != VLRResult_Success) return 1;
    res = vlrCreateInstance(scene, meshBackWall, origin, scale, axis, 0.0f, &instBackWall);
    if (res != VLRResult_Success) return 1;
    res = vlrCreateInstance(scene, meshLeftWall, origin, scale, axis, 0.0f, &instLeftWall);
    if (res != VLRResult_Success) return 1;
    res = vlrCreateInstance(scene, meshRightWall, origin, scale, axis, 0.0f, &instRightWall);
    if (res != VLRResult_Success) return 1;
    res = vlrCreateInstance(scene, meshFrontWall, origin, scale, axis, 0.0f, &instFrontWall);
    if (res != VLRResult_Success) return 1;
    res = vlrCreateInstance(scene, meshLight, origin, scale, axis, 0.0f, &instLight);
    if (res != VLRResult_Success) return 1;

    res = vlrAddAreaLight(scene, instLight);
    if (res != VLRResult_Success) return 1;

    VLRCameraParams camera = {};
    // 修复: 将相机放在盒子内部,看向后墙
    float camPosX = 0.0f, camPosY = 1.5f, camPosZ = 1.2f;  // 在盒子内部靠近前墙
    float targetX = 0.0f, targetY = 1.5f, targetZ = -1.0f;  // 看向后墙
    float dx = targetX - camPosX;
    float dy = targetY - camPosY;
    float dz = targetZ - camPosZ;
    float len = std::sqrt(dx*dx + dy*dy + dz*dz);
    if (len < 1e-6f) len = 1.0f;
    camera.position[0] = camPosX; camera.position[1] = camPosY; camera.position[2] = camPosZ;
    camera.direction[0] = dx/len; camera.direction[1] = dy/len; camera.direction[2] = dz/len;
    camera.up[0] = 0.0f; camera.up[1] = 1.0f; camera.up[2] = 0.0f;
    camera.fovY = 40.0f * PI / 180.0f;
    camera.aspect = (float)width / (float)height;
    camera.lensRadius = 0.0f;
    camera.focusDistance = 1.0f;
    camera.focalLength = 0.0f;
    camera.cameraType = 0;

    res = vlrSetCamera(scene, &camera);
    if (res != VLRResult_Success) return 1;

    return runLightTest(context, scene, width, height, numSamples, exposure, "light_system_test_01_area.png");
}

// ============================================================================
// Test 2: Point Light - Cornell Box + 中心点光源
// ============================================================================
static int testPointLight(VLRContext context, VLRScene scene,
    uint32_t width, uint32_t height, uint32_t numSamples, float exposure)
{
    VLRResult res = VLRResult_Success;
    float origin[] = { 0, 0, 0 };
    float scale[] = { 1, 1, 1 };
    float axis[] = { 0, 1, 0 };

    float whiteColor[] = { 0.522f, 0.522f, 0.522f };
    float redColor[] = { 0.522f, 0.0508f, 0.0508f };
    float blueColor[] = { 0.0508f, 0.0508f, 0.522f };
    float blackColor[] = { 0.0508f, 0.0508f, 0.0508f };

    VLRMaterial matWhite = nullptr, matRed = nullptr, matBlue = nullptr;
    VLRMaterial matFloor = nullptr;

    res = vlrCreateMaterial(scene, 0, whiteColor, nullptr, &matWhite);
    if (res != VLRResult_Success) return 1;
    res = vlrCreateMaterial(scene, 0, redColor, nullptr, &matRed);
    if (res != VLRResult_Success) return 1;
    res = vlrCreateMaterial(scene, 0, blueColor, nullptr, &matBlue);
    if (res != VLRResult_Success) return 1;
    res = vlrCreateMaterialCheckerboard(scene, blackColor, whiteColor, 20, 1.5f, &matFloor);
    if (res != VLRResult_Success) return 1;

    VLRTriangleMesh meshFloor = nullptr, meshCeiling = nullptr, meshBackWall = nullptr;
    VLRTriangleMesh meshLeftWall = nullptr, meshRightWall = nullptr, meshFrontWall = nullptr;

    res = vlrCreateTriangleMesh(scene, kFloorVertices, 4, kFloorIndices, 2, matFloor, &meshFloor);
    if (res != VLRResult_Success) return 1;
    res = vlrCreateTriangleMesh(scene, kCeilingVertices, 4, kCeilingIndices, 2, matWhite, &meshCeiling);
    if (res != VLRResult_Success) return 1;
    res = vlrCreateTriangleMesh(scene, kBackWallVertices, 4, kBackWallIndices, 2, matWhite, &meshBackWall);
    if (res != VLRResult_Success) return 1;
    res = vlrCreateTriangleMesh(scene, kLeftWallVertices, 4, kLeftWallIndices, 2, matBlue, &meshLeftWall);
    if (res != VLRResult_Success) return 1;
    res = vlrCreateTriangleMesh(scene, kRightWallVertices, 4, kRightWallIndices, 2, matRed, &meshRightWall);
    if (res != VLRResult_Success) return 1;
    res = vlrCreateTriangleMesh(scene, kFrontWallVertices, 4, kFrontWallIndices, 2, matWhite, &meshFrontWall);
    if (res != VLRResult_Success) return 1;

    VLRInstance instFloor = nullptr, instCeiling = nullptr, instBackWall = nullptr;
    VLRInstance instLeftWall = nullptr, instRightWall = nullptr, instFrontWall = nullptr;

    res = vlrCreateInstance(scene, meshFloor, origin, scale, axis, 0.0f, &instFloor);
    if (res != VLRResult_Success) return 1;
    res = vlrCreateInstance(scene, meshCeiling, origin, scale, axis, 0.0f, &instCeiling);
    if (res != VLRResult_Success) return 1;
    res = vlrCreateInstance(scene, meshBackWall, origin, scale, axis, 0.0f, &instBackWall);
    if (res != VLRResult_Success) return 1;
    res = vlrCreateInstance(scene, meshLeftWall, origin, scale, axis, 0.0f, &instLeftWall);
    if (res != VLRResult_Success) return 1;
    res = vlrCreateInstance(scene, meshRightWall, origin, scale, axis, 0.0f, &instRightWall);
    if (res != VLRResult_Success) return 1;
    res = vlrCreateInstance(scene, meshFrontWall, origin, scale, axis, 0.0f, &instFrontWall);
    if (res != VLRResult_Success) return 1;

    // 点光源：中心位置 (0, 2.9, 0)，强度 (50, 50, 50) W/sr
    float pointLightPos[] = { 0.0f, 2.9f, 0.0f };
    float pointLightIntensity[] = { 50.0f, 50.0f, 50.0f };
    res = vlrAddPointLight(scene, pointLightPos, pointLightIntensity);
    if (res != VLRResult_Success) return 1;

    VLRCameraParams camera = {};
    // 修复: 将相机放在盒子内部,看向后墙
    float camPosX = 0.0f, camPosY = 1.5f, camPosZ = 1.2f;  // 在盒子内部靠近前墙
    float targetX = 0.0f, targetY = 1.5f, targetZ = -1.0f;  // 看向后墙
    float dx = targetX - camPosX;
    float dy = targetY - camPosY;
    float dz = targetZ - camPosZ;
    float len = std::sqrt(dx*dx + dy*dy + dz*dz);
    if (len < 1e-6f) len = 1.0f;
    camera.position[0] = camPosX; camera.position[1] = camPosY; camera.position[2] = camPosZ;
    camera.direction[0] = dx/len; camera.direction[1] = dy/len; camera.direction[2] = dz/len;
    camera.up[0] = 0.0f; camera.up[1] = 1.0f; camera.up[2] = 0.0f;
    camera.fovY = 40.0f * PI / 180.0f;
    camera.aspect = (float)width / (float)height;
    camera.lensRadius = 0.0f;
    camera.focusDistance = 1.0f;
    camera.focalLength = 0.0f;
    camera.cameraType = 0;

    res = vlrSetCamera(scene, &camera);
    if (res != VLRResult_Success) return 1;

    return runLightTest(context, scene, width, height, numSamples, exposure, "light_system_test_02_point.png");
}

// ============================================================================
// Test 3: Directional Light - 平面 + 方向光（模拟太阳光）
// ============================================================================
static int testDirectionalLight(VLRContext context, VLRScene scene,
    uint32_t width, uint32_t height, uint32_t numSamples, float exposure)
{
    VLRResult res = VLRResult_Success;
    float origin[] = { 0, 0, 0 };
    float scale[] = { 1, 1, 1 };
    float axis[] = { 0, 1, 0 };

    float grayColor[] = { 0.4f, 0.4f, 0.4f };
    float sphereColor[] = { 0.8f, 0.6f, 0.2f };  // 金色球体

    VLRMaterial matGround = nullptr, matSphere = nullptr;

    res = vlrCreateMaterial(scene, 0, grayColor, nullptr, &matGround);
    if (res != VLRResult_Success) return 1;
    res = vlrCreateMaterial(scene, 0, sphereColor, nullptr, &matSphere);
    if (res != VLRResult_Success) return 1;

    std::vector<float> sphereVerts;
    std::vector<uint32_t> sphereInds;
    createSphere(sphereVerts, sphereInds, 0.0f, 0.5f, 0.0f, 0.4f, 48, 32);

    VLRTriangleMesh meshGround = nullptr, meshSphere = nullptr;

    res = vlrCreateTriangleMesh(scene, kGroundPlaneVertices, 4, kGroundPlaneIndices, 2, matGround, &meshGround);
    if (res != VLRResult_Success) return 1;
    res = vlrCreateTriangleMesh(scene, sphereVerts.data(), (uint32_t)(sphereVerts.size()/3),
                                sphereInds.data(), (uint32_t)(sphereInds.size()/3), matSphere, &meshSphere);
    if (res != VLRResult_Success) return 1;

    VLRInstance instGround = nullptr, instSphere = nullptr;

    res = vlrCreateInstance(scene, meshGround, origin, scale, axis, 0.0f, &instGround);
    if (res != VLRResult_Success) return 1;
    res = vlrCreateInstance(scene, meshSphere, origin, scale, axis, 0.0f, &instSphere);
    if (res != VLRResult_Success) return 1;

    // Directional light: from above (simulate sun), direction (0.3, -0.9, 0.2) normalized
    float lightDirX = 0.3f, lightDirY = -0.9f, lightDirZ = 0.2f;
    float dirLen = std::sqrt(lightDirX*lightDirX + lightDirY*lightDirY + lightDirZ*lightDirZ);
    float lightDirection[] = { lightDirX/dirLen, lightDirY/dirLen, lightDirZ/dirLen };
    float radiance[] = { 8.0f, 8.0f, 8.0f };  // W/(m²·sr)
    res = vlrAddDirectionalLight(scene, lightDirection, radiance);
    if (res != VLRResult_Success) return 1;

    VLRCameraParams camera = {};
    float camPos[] = { 2.5f, 1.5f, 4.0f };
    float camTarget[] = { 0.0f, 0.3f, 0.0f };
    float dx = camTarget[0] - camPos[0], dy = camTarget[1] - camPos[1], dz = camTarget[2] - camPos[2];
    float len = std::sqrt(dx*dx + dy*dy + dz*dz);
    if (len < 1e-6f) len = 1.0f;
    camera.position[0] = camPos[0]; camera.position[1] = camPos[1]; camera.position[2] = camPos[2];
    camera.direction[0] = dx/len; camera.direction[1] = dy/len; camera.direction[2] = dz/len;
    camera.up[0] = 0.0f; camera.up[1] = 1.0f; camera.up[2] = 0.0f;
    camera.fovY = 45.0f * PI / 180.0f;
    camera.aspect = (float)width / (float)height;
    camera.lensRadius = 0.0f;
    camera.focusDistance = 1.0f;
    camera.focalLength = 0.0f;
    camera.cameraType = 0;

    res = vlrSetCamera(scene, &camera);
    if (res != VLRResult_Success) return 1;

    return runLightTest(context, scene, width, height, numSamples, exposure, "light_system_test_03_directional.png");
}

// ============================================================================
// Test 4: Environment Light - 球体 + HDR 环境贴图
// ============================================================================
static int testEnvironmentLight(VLRContext context, VLRScene scene,
    uint32_t width, uint32_t height, uint32_t numSamples, float exposure,
    const char* envImagePath)
{
    VLRResult res = VLRResult_Success;
    float origin[] = { 0, 0, 0 };
    float scale[] = { 1, 1, 1 };
    float axis[] = { 0, 1, 0 };

    float matteColor[] = { 0.9f, 0.9f, 0.9f };

    VLRMaterial matSphere = nullptr;

    res = vlrCreateMaterial(scene, 0, matteColor, nullptr, &matSphere);
    if (res != VLRResult_Success) return 1;

    std::vector<float> sphereVerts;
    std::vector<uint32_t> sphereInds;
    createSphere(sphereVerts, sphereInds, 0.0f, 0.0f, 0.0f, 0.8f, 64, 48);

    VLRTriangleMesh meshSphere = nullptr;

    res = vlrCreateTriangleMesh(scene, sphereVerts.data(), (uint32_t)(sphereVerts.size()/3),
                                sphereInds.data(), (uint32_t)(sphereInds.size()/3), matSphere, &meshSphere);
    if (res != VLRResult_Success) return 1;

    VLRInstance instSphere = nullptr;

    res = vlrCreateInstance(scene, meshSphere, origin, scale, axis, 0.0f, &instSphere);
    if (res != VLRResult_Success) return 1;

    // 环境光：从 HDR/EXR 图像加载
    res = vlrSetEnvironmentLightFromImage(scene, envImagePath, 0.0f);
    if (res != VLRResult_Success) {
        fprintf(stderr, "[Warning] Failed to load env map '%s', using constant fallback\n", envImagePath);
        float envColor[] = { 0.5f, 0.5f, 0.6f };
        res = vlrSetEnvironmentLight(scene, envColor);
        if (res != VLRResult_Success) return 1;
    }

    VLRCameraParams camera = {};
    float camPos[] = { 0.0f, 0.0f, 3.5f };
    float camTarget[] = { 0.0f, 0.0f, 0.0f };
    float dx = camTarget[0] - camPos[0], dy = camTarget[1] - camPos[1], dz = camTarget[2] - camPos[2];
    float len = std::sqrt(dx*dx + dy*dy + dz*dz);
    if (len < 1e-6f) len = 1.0f;
    camera.position[0] = camPos[0]; camera.position[1] = camPos[1]; camera.position[2] = camPos[2];
    camera.direction[0] = dx/len; camera.direction[1] = dy/len; camera.direction[2] = dz/len;
    camera.up[0] = 0.0f; camera.up[1] = 1.0f; camera.up[2] = 0.0f;
    camera.fovY = 50.0f * PI / 180.0f;
    camera.aspect = (float)width / (float)height;
    camera.lensRadius = 0.0f;
    camera.focusDistance = 1.0f;
    camera.focalLength = 0.0f;
    camera.cameraType = 0;

    res = vlrSetCamera(scene, &camera);
    if (res != VLRResult_Success) return 1;

    return runLightTest(context, scene, width, height, numSamples, exposure, "light_system_test_04_environment.png");
}

// ============================================================================
// Test 5: Mixed Lights - 区域光 + 点光源 + 环境光
// ============================================================================
static int testMixedLights(VLRContext context, VLRScene scene,
    uint32_t width, uint32_t height, uint32_t numSamples, float exposure,
    const char* envImagePath)
{
    VLRResult res = VLRResult_Success;
    float origin[] = { 0, 0, 0 };
    float scale[] = { 1, 1, 1 };
    float axis[] = { 0, 1, 0 };

    float whiteColor[] = { 0.522f, 0.522f, 0.522f };
    float redColor[] = { 0.522f, 0.0508f, 0.0508f };
    float blueColor[] = { 0.0508f, 0.0508f, 0.522f };
    float blackColor[] = { 0.0508f, 0.0508f, 0.0508f };
    float lightEmission[] = { 40.0f, 40.0f, 40.0f };

    VLRMaterial matWhite = nullptr, matRed = nullptr, matBlue = nullptr;
    VLRMaterial matFloor = nullptr, matLight = nullptr;

    res = vlrCreateMaterial(scene, 0, whiteColor, nullptr, &matWhite);
    if (res != VLRResult_Success) return 1;
    res = vlrCreateMaterial(scene, 0, redColor, nullptr, &matRed);
    if (res != VLRResult_Success) return 1;
    res = vlrCreateMaterial(scene, 0, blueColor, nullptr, &matBlue);
    if (res != VLRResult_Success) return 1;
    res = vlrCreateMaterialCheckerboard(scene, blackColor, whiteColor, 20, 1.5f, &matFloor);
    if (res != VLRResult_Success) return 1;
    res = vlrCreateMaterial(scene, 0, whiteColor, lightEmission, &matLight);
    if (res != VLRResult_Success) return 1;

    VLRTriangleMesh meshFloor = nullptr, meshCeiling = nullptr, meshBackWall = nullptr;
    VLRTriangleMesh meshLeftWall = nullptr, meshRightWall = nullptr, meshFrontWall = nullptr;
    VLRTriangleMesh meshLight = nullptr;

    res = vlrCreateTriangleMesh(scene, kFloorVertices, 4, kFloorIndices, 2, matFloor, &meshFloor);
    if (res != VLRResult_Success) return 1;
    res = vlrCreateTriangleMesh(scene, kCeilingVertices, 4, kCeilingIndices, 2, matWhite, &meshCeiling);
    if (res != VLRResult_Success) return 1;
    res = vlrCreateTriangleMesh(scene, kBackWallVertices, 4, kBackWallIndices, 2, matWhite, &meshBackWall);
    if (res != VLRResult_Success) return 1;
    res = vlrCreateTriangleMesh(scene, kLeftWallVertices, 4, kLeftWallIndices, 2, matBlue, &meshLeftWall);
    if (res != VLRResult_Success) return 1;
    res = vlrCreateTriangleMesh(scene, kRightWallVertices, 4, kRightWallIndices, 2, matRed, &meshRightWall);
    if (res != VLRResult_Success) return 1;
    res = vlrCreateTriangleMesh(scene, kFrontWallVertices, 4, kFrontWallIndices, 2, matWhite, &meshFrontWall);
    if (res != VLRResult_Success) return 1;
    res = vlrCreateTriangleMesh(scene, kLightVertices, 4, kLightIndices, 2, matLight, &meshLight);
    if (res != VLRResult_Success) return 1;

    VLRInstance instFloor = nullptr, instCeiling = nullptr, instBackWall = nullptr;
    VLRInstance instLeftWall = nullptr, instRightWall = nullptr, instFrontWall = nullptr;
    VLRInstance instLight = nullptr;

    res = vlrCreateInstance(scene, meshFloor, origin, scale, axis, 0.0f, &instFloor);
    if (res != VLRResult_Success) return 1;
    res = vlrCreateInstance(scene, meshCeiling, origin, scale, axis, 0.0f, &instCeiling);
    if (res != VLRResult_Success) return 1;
    res = vlrCreateInstance(scene, meshBackWall, origin, scale, axis, 0.0f, &instBackWall);
    if (res != VLRResult_Success) return 1;
    res = vlrCreateInstance(scene, meshLeftWall, origin, scale, axis, 0.0f, &instLeftWall);
    if (res != VLRResult_Success) return 1;
    res = vlrCreateInstance(scene, meshRightWall, origin, scale, axis, 0.0f, &instRightWall);
    if (res != VLRResult_Success) return 1;
    res = vlrCreateInstance(scene, meshFrontWall, origin, scale, axis, 0.0f, &instFrontWall);
    if (res != VLRResult_Success) return 1;
    res = vlrCreateInstance(scene, meshLight, origin, scale, axis, 0.0f, &instLight);
    if (res != VLRResult_Success) return 1;

    res = vlrAddAreaLight(scene, instLight);
    if (res != VLRResult_Success) return 1;

    float pointLightPos[] = { -0.5f, 0.8f, 0.5f };
    float pointLightIntensity[] = { 15.0f, 15.0f, 20.0f };
    res = vlrAddPointLight(scene, pointLightPos, pointLightIntensity);
    if (res != VLRResult_Success) return 1;

    res = vlrSetEnvironmentLightFromImage(scene, envImagePath, 0.0f);
    if (res != VLRResult_Success) {
        float envColor[] = { 0.08f, 0.08f, 0.1f };
        res = vlrSetEnvironmentLight(scene, envColor);
        if (res != VLRResult_Success) return 1;
    }

    VLRCameraParams camera = {};
    float dx = 0.0f - 0.0f, dy = 1.5f - 1.5f, dz = 0.0f - 6.0f;
    float len = std::sqrt(dx*dx + dy*dy + dz*dz);
    if (len < 1e-6f) len = 1.0f;
    camera.position[0] = 0.0f; camera.position[1] = 1.5f; camera.position[2] = 6.0f;
    camera.direction[0] = dx/len; camera.direction[1] = dy/len; camera.direction[2] = dz/len;
    camera.up[0] = 0.0f; camera.up[1] = 1.0f; camera.up[2] = 0.0f;
    camera.fovY = 40.0f * PI / 180.0f;
    camera.aspect = (float)width / (float)height;
    camera.lensRadius = 0.0f;
    camera.focusDistance = 1.0f;
    camera.focalLength = 0.0f;
    camera.cameraType = 0;

    res = vlrSetCamera(scene, &camera);
    if (res != VLRResult_Success) return 1;

    return runLightTest(context, scene, width, height, numSamples, exposure, "light_system_test_05_mixed.png");
}

// ============================================================================
// Main
// ============================================================================
int main(int argc, char** argv) {
    printf("=== VLR Light System Test START ===\n");
    fflush(stdout);

    uint32_t width = 512;
    uint32_t height = 512;
    uint32_t numSamples = 256;
    float exposure = 0.5f;

    std::string resourceDir = "test/resources";
    if (argc >= 2) resourceDir = argv[1];

    std::string envPath = resourceDir + "/environments/WhiteOne.exr";

    VLRContext context = nullptr;
    VLRScene scene = nullptr;
    VLRResult res = vlrCreateContext(nullptr, 0, &context);
    if (res != VLRResult_Success || !context) {
        fprintf(stderr, "[Error] Failed to create Context\n");
        return 1;
    }

    printf("Resolution: %u x %u, Samples: %u, Exposure: %.2f\n", width, height, numSamples, exposure);
    printf("Environment map: %s\n", envPath.c_str());
    printf("\n--- Rendering light test images ---\n");

    int r1 = 1, r2 = 1, r3 = 1, r4 = 1, r5 = 1;

    res = vlrCreateScene(context, &scene);
    if (res == VLRResult_Success && scene) {
        r1 = testAreaLight(context, scene, width, height, numSamples, exposure);
        vlrDestroyScene(scene);
    }

    res = vlrCreateScene(context, &scene);
    if (res == VLRResult_Success && scene) {
        r2 = testPointLight(context, scene, width, height, numSamples, exposure);
        vlrDestroyScene(scene);
    }

    res = vlrCreateScene(context, &scene);
    if (res == VLRResult_Success && scene) {
        r3 = testDirectionalLight(context, scene, width, height, numSamples, exposure);
        vlrDestroyScene(scene);
    }

    res = vlrCreateScene(context, &scene);
    if (res == VLRResult_Success && scene) {
        r4 = testEnvironmentLight(context, scene, width, height, numSamples, exposure, envPath.c_str());
        vlrDestroyScene(scene);
    }

    res = vlrCreateScene(context, &scene);
    if (res == VLRResult_Success && scene) {
        r5 = testMixedLights(context, scene, width, height, numSamples, exposure, envPath.c_str());
        vlrDestroyScene(scene);
    }

    vlrDestroyContext(context);

    int total = r1 + r2 + r3 + r4 + r5;
    printf("\n=== Light System Test Complete ===\n");
    printf("Output: light_system_test_01_area.png .. light_system_test_05_mixed.png\n");
    return (total == 0) ? 0 : 1;
}
