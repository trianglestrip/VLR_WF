// ============================================================================
// VLR LambertianScattering 次表面散射 BSDF 测试
//
// 测试 LambertianScattering 材质：渲染薄球壳/球体，观察光线透过材质的效果。
// 与普通 Lambert 对比：LambertianScattering 允许光从背面透射，产生次表面散射效果。
//
// 场景：简化 Cornell Box + LambertianScattering 球体（中心）
// 预期：球体应呈现柔和的半透明发光，光线可从背面透出
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

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

static const float PI = 3.14159265358979323846f;
static const float TWO_PI = 6.28318530717958647692f;

// UV sphere: center (cx,cy,cz), radius
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

// Cornell Box 3x3x3 几何
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

// Area light: y=2.9, size 1.0x1.0 (vertex order matches cornell_box_improved_test.cpp)
static const float kLightVertices[] = {
    -0.5f, 2.9f, -0.5f,  0.5f, 2.9f, -0.5f,  0.5f, 2.9f, 0.5f,  -0.5f, 2.9f, 0.5f
};
static const uint32_t kLightIndices[] = { 0, 1, 2,  0, 2, 3 };

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

int main(int argc, char** argv) {
    printf("=== LambertianScattering BSDF Test ===\n");
    uint32_t width = 512;
    uint32_t height = 512;
    uint32_t numSamples = 512;
    float exposure = 0.5f;
    const char* outputFile = "lambertian_scattering_test.png";

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-w") == 0 && i + 1 < argc) width = (uint32_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "-h") == 0 && i + 1 < argc) height = (uint32_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) numSamples = (uint32_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) outputFile = argv[++i];
    }

    // All variable declarations at function start (before any goto cleanup)
    VLRContext context = nullptr;
    VLRScene scene = nullptr;
    VLRResult res;
    VLRCameraParams camera = {};
    std::vector<float> sphereVerts;
    std::vector<uint32_t> sphereInds;
    
    // 声明所有变量（避免 goto cleanup 跳过初始化）
    float targetX = 0.0f, targetY = 1.0f, targetZ = 0.0f;
    const float lookAtY = 1.0f;
    float dirX, dirY, dirZ, dirLen;

    float whiteColor[] = { 0.522f, 0.522f, 0.522f };
    float redColor[] = { 0.522f, 0.0508f, 0.0508f };
    float blueColor[] = { 0.0508f, 0.0508f, 0.522f };
    float blackColor[] = { 0.05f, 0.05f, 0.05f };
    float sssColor[] = { 0.9f, 0.4f, 0.4f };
    float lightEmission[] = { 30.0f, 30.0f, 30.0f };

    VLRMaterial matWhite = nullptr;
    VLRMaterial matRed = nullptr;
    VLRMaterial matBlue = nullptr;
    VLRMaterial matFloor = nullptr;
    VLRMaterial matLight = nullptr;
    VLRMaterial matSSS = nullptr;

    VLRTriangleMesh meshFloor = nullptr, meshCeiling = nullptr, meshBack = nullptr;
    VLRTriangleMesh meshLeft = nullptr, meshRight = nullptr, meshFront = nullptr;
    VLRTriangleMesh meshLight = nullptr, meshSphere = nullptr;

    float origin[] = { 0, 0, 0 };
    float scale[] = { 1, 1, 1 };
    float axis[] = { 0, 1, 0 };
    VLRInstance instFloor = nullptr, instCeiling = nullptr, instBack = nullptr;
    VLRInstance instLeft = nullptr, instRight = nullptr, instFront = nullptr;
    VLRInstance instLight = nullptr, instSphere = nullptr;

    void* deviceBuffer = nullptr;
    size_t bufferSize = 0;
    std::vector<float> rgb;
    cudaError_t cudaErr = cudaSuccess;

    res = vlrCreateContext(nullptr, 0, &context);
    if (res != VLRResult_Success || !context) {
        fprintf(stderr, "[Error] Context creation failed\n");
        return 1;
    }

    res = vlrCreateScene(context, &scene);
    if (res != VLRResult_Success || !scene) {
        fprintf(stderr, "[Error] Scene creation failed\n");
        vlrDestroyContext(context);
        return 1;
    }

    // Materials: sRGB to linear
    res = vlrCreateMaterial(scene, 0, whiteColor, nullptr, &matWhite);
    if (res != VLRResult_Success) { fprintf(stderr, "[Error] White material\n"); goto cleanup; }
    res = vlrCreateMaterial(scene, 0, redColor, nullptr, &matRed);
    if (res != VLRResult_Success) { fprintf(stderr, "[Error] Red material\n"); goto cleanup; }
    res = vlrCreateMaterial(scene, 0, blueColor, nullptr, &matBlue);
    if (res != VLRResult_Success) { fprintf(stderr, "[Error] Blue material\n"); goto cleanup; }
    res = vlrCreateMaterial(scene, 0, blackColor, nullptr, &matFloor);
    if (res != VLRResult_Success) { fprintf(stderr, "[Error] Floor material\n"); goto cleanup; }
    res = vlrCreateMaterial(scene, 0, whiteColor, lightEmission, &matLight);
    if (res != VLRResult_Success) { fprintf(stderr, "[Error] Light material\n"); goto cleanup; }

    // LambertianScattering 材质
    res = vlrCreateMaterialLambertianScattering(scene, sssColor, &matSSS);
    if (res != VLRResult_Success) {
        fprintf(stderr, "[Error] LambertianScattering material creation failed\n");
        goto cleanup;
    }
    printf("[OK] LambertianScattering material created\n");

    // Geometry
    res = vlrCreateTriangleMesh(scene, kFloorVertices, 4, kFloorIndices, 2, matFloor, &meshFloor);
    if (res != VLRResult_Success) { fprintf(stderr, "[Error] Floor mesh\n"); goto cleanup; }
    res = vlrCreateTriangleMesh(scene, kCeilingVertices, 4, kCeilingIndices, 2, matWhite, &meshCeiling);
    if (res != VLRResult_Success) { fprintf(stderr, "[Error] Ceiling mesh\n"); goto cleanup; }
    res = vlrCreateTriangleMesh(scene, kBackWallVertices, 4, kBackWallIndices, 2, matWhite, &meshBack);
    if (res != VLRResult_Success) { fprintf(stderr, "[Error] Back wall mesh\n"); goto cleanup; }
    res = vlrCreateTriangleMesh(scene, kLeftWallVertices, 4, kLeftWallIndices, 2, matBlue, &meshLeft);
    if (res != VLRResult_Success) { fprintf(stderr, "[Error] Left wall mesh\n"); goto cleanup; }
    res = vlrCreateTriangleMesh(scene, kRightWallVertices, 4, kRightWallIndices, 2, matRed, &meshRight);
    if (res != VLRResult_Success) { fprintf(stderr, "[Error] Right wall mesh\n"); goto cleanup; }
    // 前墙已移除：相机在 z=6.0，需要开放式 Cornell Box
    // res = vlrCreateTriangleMesh(scene, kFrontWallVertices, 4, kFrontWallIndices, 2, matWhite, &meshFront);
    // if (res != VLRResult_Success) { fprintf(stderr, "[Error] Front wall mesh\n"); goto cleanup; }
    res = vlrCreateTriangleMesh(scene, kLightVertices, 4, kLightIndices, 2, matLight, &meshLight);
    if (res != VLRResult_Success) { fprintf(stderr, "[Error] Light mesh\n"); goto cleanup; }

    // 创建球体：中心 (0, 1, 0), 半径 0.5（居中位置）
    createSphere(sphereVerts, sphereInds, 0.0f, 1.0f, 0.0f, 0.5f, 64, 48);
    res = vlrCreateTriangleMesh(scene, sphereVerts.data(), (uint32_t)(sphereVerts.size() / 3),
                               sphereInds.data(), (uint32_t)(sphereInds.size() / 3), matSSS, &meshSphere);
    if (res != VLRResult_Success) { fprintf(stderr, "[Error] Sphere mesh\n"); goto cleanup; }

    // Instances
    res = vlrCreateInstance(scene, meshFloor, origin, scale, axis, 0.0f, &instFloor);
    if (res != VLRResult_Success) { fprintf(stderr, "[Error] Floor instance\n"); goto cleanup; }
    res = vlrCreateInstance(scene, meshCeiling, origin, scale, axis, 0.0f, &instCeiling);
    if (res != VLRResult_Success) { fprintf(stderr, "[Error] Ceiling instance\n"); goto cleanup; }
    res = vlrCreateInstance(scene, meshBack, origin, scale, axis, 0.0f, &instBack);
    if (res != VLRResult_Success) { fprintf(stderr, "[Error] Back instance\n"); goto cleanup; }
    res = vlrCreateInstance(scene, meshLeft, origin, scale, axis, 0.0f, &instLeft);
    if (res != VLRResult_Success) { fprintf(stderr, "[Error] Left instance\n"); goto cleanup; }
    res = vlrCreateInstance(scene, meshRight, origin, scale, axis, 0.0f, &instRight);
    if (res != VLRResult_Success) { fprintf(stderr, "[Error] Right instance\n"); goto cleanup; }
    // 前墙实例已移除
    // res = vlrCreateInstance(scene, meshFront, origin, scale, axis, 0.0f, &instFront);
    // if (res != VLRResult_Success) { fprintf(stderr, "[Error] Front instance\n"); goto cleanup; }
    res = vlrCreateInstance(scene, meshLight, origin, scale, axis, 0.0f, &instLight);
    if (res != VLRResult_Success) { fprintf(stderr, "[Error] Light instance\n"); goto cleanup; }
    res = vlrCreateInstance(scene, meshSphere, origin, scale, axis, 0.0f, &instSphere);
    if (res != VLRResult_Success) { fprintf(stderr, "[Error] Sphere instance\n"); goto cleanup; }

    res = vlrAddAreaLight(scene, instLight);
    if (res != VLRResult_Success) { fprintf(stderr, "[Error] Add area light\n"); goto cleanup; }

    printf("Scene setup complete.\n\n");

    // 设置相机（参考 cornell_box_improved_test.cpp）
    // 位置: (0, 1.5, 6.0), 看向 (0, 1.0, 0) - 看向球体中心
    camera.position[0] = 0.0f;
    camera.position[1] = 1.5f;
    camera.position[2] = 6.0f;
    
    // 计算方向向量（看向球体中心 y=1.0）
    dirX = targetX - camera.position[0];
    dirY = lookAtY - camera.position[1];
    dirZ = targetZ - camera.position[2];
    dirLen = std::sqrt(dirX*dirX + dirY*dirY + dirZ*dirZ);
    camera.direction[0] = dirX / dirLen;
    camera.direction[1] = dirY / dirLen;
    camera.direction[2] = dirZ / dirLen;
    
    camera.up[0] = 0.0f;
    camera.up[1] = 1.0f;
    camera.up[2] = 0.0f;
    camera.fovY = 40.0f * PI / 180.0f;
    camera.aspect = (float)width / (float)height;
    camera.lensRadius = 0.0f;
    camera.focusDistance = 6.0f;
    camera.focalLength = 0.0f;
    camera.cameraType = 0;

    res = vlrSetCamera(scene, &camera);
    if (res != VLRResult_Success) { fprintf(stderr, "[Error] Set camera\n"); goto cleanup; }

    printf("Camera: pos=(%.2f, %.2f, %.2f), target=(%.2f, %.2f, %.2f)\n",
           camera.position[0], camera.position[1], camera.position[2],
           targetX, targetY, targetZ);
    printf("        dir=(%.3f, %.3f, %.3f), fovY=%.1f°\n\n",
           camera.direction[0], camera.direction[1], camera.direction[2],
           camera.fovY * 180.0f / PI);

    printf("Rendering %u x %u, %u spp...\n", width, height, numSamples);
    res = vlrRender(context, scene, width, height, numSamples, vlr::VLRRenderer_WavefrontPathTracing);
    if (res != VLRResult_Success) {
        fprintf(stderr, "[Error] Render failed: %d\n", res);
        goto cleanup;
    }

    deviceBuffer = vlrGetOutputBuffer(context);
    if (!deviceBuffer) {
        fprintf(stderr, "[Error] No output buffer\n");
        goto cleanup;
    }

    bufferSize = width * height * 3 * sizeof(float);
    rgb.resize(width * height * 3);
    cudaErr = cudaMemcpy(rgb.data(), deviceBuffer, bufferSize, cudaMemcpyDeviceToHost);
    if (cudaErr != cudaSuccess) {
        fprintf(stderr, "[Error] cudaMemcpy failed: %s\n", cudaGetErrorString(cudaErr));
        goto cleanup;
    }

    savePNG(outputFile, width, height, rgb.data(), numSamples, exposure);

    printf("\n=== LambertianScattering Test Complete ===\n");
    printf("Expected: Sphere shows subsurface scattering - soft glow, light can transmit through.\n");
    printf("Compare with Lambert: Lambert would only show lit side; SSS shows back-lit glow.\n");

cleanup:
    vlrDestroyScene(scene);
    vlrDestroyContext(context);
    return 0;
}
