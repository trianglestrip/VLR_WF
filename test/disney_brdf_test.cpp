// ============================================================================
// VLR Wavefront - Disney Principled BRDF Test
//
// 渲染多个球体展示不同 Disney 参数组合：
// - 塑料（metallic=0, specular=0.5）
// - 金属（metallic=1）
// - 织物（sheen=1）
// - 清漆木材（clearcoat=1）
// - 各向异性金属（anisotropic=0.8）
//
// 场景设置参考 cornell_box_improved_test.cpp（已验证正确）
//
// Author: VLR Development Team
// Created: 2026-03-08
// Updated: 2026-03-08 - 使用 Cornell Box 场景设置
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
        printf("[Done] Saved: %s\n", filename);
    } else {
        fprintf(stderr, "[Error] Failed to save: %s\n", filename);
    }
}

// ============================================================================
// Cornell Box 3×3×3 Geometry (参考 cornell_box_improved_test.cpp)
// ============================================================================

// Floor (y=0): normal +Y
static const float kFloorVertices[] = {
    -1.5f, 0.0f, 1.5f,  -1.5f, 0.0f, -1.5f,  1.5f, 0.0f, -1.5f,  1.5f, 0.0f, 1.5f
};
static const uint32_t kFloorIndices[] = { 0, 1, 2,  0, 2, 3 };

// Ceiling (y=3): normal -Y
static const float kCeilingVertices[] = {
    -1.5f, 3.0f, -1.5f,  -1.5f, 3.0f, 1.5f,  1.5f, 3.0f, 1.5f,  1.5f, 3.0f, -1.5f
};
static const uint32_t kCeilingIndices[] = { 0, 1, 2,  0, 2, 3 };

// Back wall (z=-1.5): normal +Z
static const float kBackWallVertices[] = {
    -1.5f, 0.0f, -1.5f,  1.5f, 0.0f, -1.5f,  1.5f, 3.0f, -1.5f,  -1.5f, 3.0f, -1.5f
};
static const uint32_t kBackWallIndices[] = { 0, 1, 2,  0, 2, 3 };

// Left wall (x=-1.5): normal +X (蓝色)
static const float kLeftWallVertices[] = {
    -1.5f, 0.0f, 1.5f,  -1.5f, 0.0f, -1.5f,  -1.5f, 3.0f, -1.5f,  -1.5f, 3.0f, 1.5f
};
static const uint32_t kLeftWallIndices[] = { 0, 1, 2,  0, 2, 3 };

// Right wall (x=1.5): normal -X (红色)
static const float kRightWallVertices[] = {
    1.5f, 0.0f, -1.5f,  1.5f, 0.0f, 1.5f,  1.5f, 3.0f, 1.5f,  1.5f, 3.0f, -1.5f
};
static const uint32_t kRightWallIndices[] = { 0, 1, 2,  0, 2, 3 };

// Area light: y=2.9, size 1.0×1.0
static const float kLightVertices[] = {
    -0.5f, 2.9f, -0.5f,  0.5f, 2.9f, -0.5f,  0.5f, 2.9f, 0.5f,  -0.5f, 2.9f, 0.5f
};
static const uint32_t kLightIndices[] = { 0, 1, 2,  0, 2, 3 };

int main(int argc, char** argv) {
    uint32_t width = 800;
    uint32_t height = 600;
    uint32_t numSamples = 256;
    const char* outputFile = "disney_brdf_test.png";

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-w") == 0 && i + 1 < argc) width = (uint32_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "-h") == 0 && i + 1 < argc) height = (uint32_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) numSamples = (uint32_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) outputFile = argv[++i];
    }

    printf("=== Disney BRDF Test ===\n");
    printf("  Resolution: %u x %u\n", width, height);
    printf("  Samples: %u\n", numSamples);
    printf("  Output: %s\n", outputFile);
    printf("  Scene: Cornell Box 3x3x3 with 5 Disney BRDF spheres\n\n");

    VLRContext context = nullptr;
    VLRScene scene = nullptr;
    VLRResult res;
    
    // 声明所有变量
    const float sphereY = 0.5f;
    const float sphereRadius = 0.5f;
    const float targetX = 0.0f, targetY = 1.0f, targetZ = 0.0f;
    const float lookAtY = 1.0f;
    float dirX, dirY, dirZ, dirLen;

    res = vlrCreateContext(nullptr, 0, &context);
    if (res != VLRResult_Success || !context) {
        fprintf(stderr, "[Error] Context creation failed\n");
        return 1;
    }

    // 启用降噪器
    printf("Enabling denoiser...\n");
    res = vlrSetDenoiserConfig(context, true, true, true, 1.0f);
    if (res != VLRResult_Success) {
        fprintf(stderr, "[Warning] Failed to enable denoiser, continuing without it\n");
    } else {
        printf("[OK] Denoiser enabled (Albedo: ON, Normal: ON, HDR: 1.0)\n");
    }

    res = vlrCreateScene(context, &scene);
    if (res != VLRResult_Success || !scene) {
        fprintf(stderr, "[Error] Scene creation failed\n");
        vlrDestroyContext(context);
        return 1;
    }

    // 材质颜色（sRGB 转线性）
    float whiteColor[] = { 0.522f, 0.522f, 0.522f };
    float redColor[] = { 0.522f, 0.0508f, 0.0508f };
    float blueColor[] = { 0.0508f, 0.0508f, 0.522f };
    float lightEmission[] = { 30.0f, 30.0f, 30.0f };
    
    // Disney BRDF 球体颜色
    float plasticColor[] = { 0.9f, 0.2f, 0.2f };
    float metalColor[] = { 0.95f, 0.64f, 0.54f };
    float fabricColor[] = { 0.4f, 0.2f, 0.6f };
    float woodColor[] = { 0.6f, 0.4f, 0.2f };
    float anisoColor[] = { 0.9f, 0.9f, 0.9f };

    // 材质
    VLRMaterial matWhite = nullptr;
    VLRMaterial matRed = nullptr;
    VLRMaterial matBlue = nullptr;
    VLRMaterial matLight = nullptr;
    VLRMaterial matPlastic = nullptr;
    VLRMaterial matMetal = nullptr;
    VLRMaterial matFabric = nullptr;
    VLRMaterial matClearcoatWood = nullptr;
    VLRMaterial matAnisoMetal = nullptr;

    // 网格
    VLRTriangleMesh meshFloor = nullptr, meshCeiling = nullptr, meshBack = nullptr;
    VLRTriangleMesh meshLeft = nullptr, meshRight = nullptr;
    VLRTriangleMesh meshLight = nullptr;
    VLRTriangleMesh meshS0 = nullptr, meshS1 = nullptr, meshS2 = nullptr, meshS3 = nullptr, meshS4 = nullptr;

    // 实例
    float origin[] = { 0, 0, 0 };
    float scale[] = { 1, 1, 1 };
    float axis[] = { 0, 1, 0 };
    VLRInstance instFloor = nullptr, instCeiling = nullptr, instBack = nullptr;
    VLRInstance instLeft = nullptr, instRight = nullptr;
    VLRInstance instLight = nullptr;
    VLRInstance instS0 = nullptr, instS1 = nullptr, instS2 = nullptr, instS3 = nullptr, instS4 = nullptr;

    std::vector<float> sphereVerts;
    std::vector<uint32_t> sphereInds;
    VLRCameraParams camera = {};
    std::vector<float> hostBuffer(width * height * 3);
    void* deviceBuffer = nullptr;
    size_t bufferSize = 0;
    cudaError_t cudaErr = cudaSuccess;

    // 创建材质
    res = vlrCreateMaterial(scene, 0, whiteColor, nullptr, &matWhite);
    if (res != VLRResult_Success) { fprintf(stderr, "[Error] White material\n"); goto cleanup; }
    res = vlrCreateMaterial(scene, 0, redColor, nullptr, &matRed);
    if (res != VLRResult_Success) { fprintf(stderr, "[Error] Red material\n"); goto cleanup; }
    res = vlrCreateMaterial(scene, 0, blueColor, nullptr, &matBlue);
    if (res != VLRResult_Success) { fprintf(stderr, "[Error] Blue material\n"); goto cleanup; }
    res = vlrCreateMaterial(scene, 0, whiteColor, lightEmission, &matLight);
    if (res != VLRResult_Success) { fprintf(stderr, "[Error] Light material\n"); goto cleanup; }

    // 创建 Disney BRDF 材质
    printf("Creating Disney BRDF materials...\n");
    
    // 塑料: metallic=0, specular=0.5
    res = vlrCreateMaterialDisney(scene, plasticColor,
        0.0f, 0.0f, 0.5f, 0.3f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f,
        &matPlastic);
    if (res != VLRResult_Success) { fprintf(stderr, "[Error] Plastic material\n"); goto cleanup; }

    // 金属: metallic=1, roughness=0.2
    res = vlrCreateMaterialDisney(scene, metalColor,
        1.0f, 0.0f, 0.0f, 0.2f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f,
        &matMetal);
    if (res != VLRResult_Success) { fprintf(stderr, "[Error] Metal material\n"); goto cleanup; }

    // 织物: sheen=1, sheenTint=0.5
    res = vlrCreateMaterialDisney(scene, fabricColor,
        0.0f, 0.0f, 0.2f, 0.8f, 0.0f, 0.0f, 1.0f, 0.5f, 0.0f, 1.0f,
        &matFabric);
    if (res != VLRResult_Success) { fprintf(stderr, "[Error] Fabric material\n"); goto cleanup; }

    // 清漆木材: clearcoat=1, clearcoatGloss=0.8
    res = vlrCreateMaterialDisney(scene, woodColor,
        0.0f, 0.0f, 0.3f, 0.6f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.8f,
        &matClearcoatWood);
    if (res != VLRResult_Success) { fprintf(stderr, "[Error] Clearcoat wood material\n"); goto cleanup; }

    // 各向异性金属: metallic=1, anisotropic=0.8
    res = vlrCreateMaterialDisney(scene, anisoColor,
        1.0f, 0.0f, 0.0f, 0.15f, 0.0f, 0.8f, 0.0f, 0.0f, 0.0f, 1.0f,
        &matAnisoMetal);
    if (res != VLRResult_Success) { fprintf(stderr, "[Error] Anisotropic metal material\n"); goto cleanup; }

    printf("Disney BRDF materials created successfully.\n");

    // 创建 Cornell Box 几何体
    printf("Creating Cornell Box geometry...\n");
    
    res = vlrCreateTriangleMesh(scene, kFloorVertices, 4, kFloorIndices, 2, matWhite, &meshFloor);
    if (res != VLRResult_Success) { fprintf(stderr, "[Error] Floor mesh\n"); goto cleanup; }
    res = vlrCreateTriangleMesh(scene, kCeilingVertices, 4, kCeilingIndices, 2, matWhite, &meshCeiling);
    if (res != VLRResult_Success) { fprintf(stderr, "[Error] Ceiling mesh\n"); goto cleanup; }
    res = vlrCreateTriangleMesh(scene, kBackWallVertices, 4, kBackWallIndices, 2, matWhite, &meshBack);
    if (res != VLRResult_Success) { fprintf(stderr, "[Error] Back wall mesh\n"); goto cleanup; }
    res = vlrCreateTriangleMesh(scene, kLeftWallVertices, 4, kLeftWallIndices, 2, matBlue, &meshLeft);
    if (res != VLRResult_Success) { fprintf(stderr, "[Error] Left wall mesh\n"); goto cleanup; }
    res = vlrCreateTriangleMesh(scene, kRightWallVertices, 4, kRightWallIndices, 2, matRed, &meshRight);
    if (res != VLRResult_Success) { fprintf(stderr, "[Error] Right wall mesh\n"); goto cleanup; }
    res = vlrCreateTriangleMesh(scene, kLightVertices, 4, kLightIndices, 2, matLight, &meshLight);
    if (res != VLRResult_Success) { fprintf(stderr, "[Error] Light mesh\n"); goto cleanup; }

    // 创建 5 个球体（排成一行，z=0.0 居中，间距加大避免重叠）
    // 球体位置：x 从 -1.2 到 1.2，间隔 0.6，y=0.4（底部），半径 0.4
    printf("Creating Disney BRDF spheres...\n");
    
    createSphere(sphereVerts, sphereInds, -1.2f, 0.4f, 0.0f, 0.4f);
    res = vlrCreateTriangleMesh(scene, sphereVerts.data(), (uint32_t)(sphereVerts.size() / 3),
                               sphereInds.data(), (uint32_t)(sphereInds.size() / 3), matPlastic, &meshS0);
    if (res != VLRResult_Success) { fprintf(stderr, "[Error] Sphere 0 (Plastic)\n"); goto cleanup; }

    createSphere(sphereVerts, sphereInds, -0.6f, 0.4f, 0.0f, 0.4f);
    res = vlrCreateTriangleMesh(scene, sphereVerts.data(), (uint32_t)(sphereVerts.size() / 3),
                               sphereInds.data(), (uint32_t)(sphereInds.size() / 3), matMetal, &meshS1);
    if (res != VLRResult_Success) { fprintf(stderr, "[Error] Sphere 1 (Metal)\n"); goto cleanup; }

    createSphere(sphereVerts, sphereInds, 0.0f, 0.4f, 0.0f, 0.4f);
    res = vlrCreateTriangleMesh(scene, sphereVerts.data(), (uint32_t)(sphereVerts.size() / 3),
                               sphereInds.data(), (uint32_t)(sphereInds.size() / 3), matFabric, &meshS2);
    if (res != VLRResult_Success) { fprintf(stderr, "[Error] Sphere 2 (Fabric)\n"); goto cleanup; }

    createSphere(sphereVerts, sphereInds, 0.6f, 0.4f, 0.0f, 0.4f);
    res = vlrCreateTriangleMesh(scene, sphereVerts.data(), (uint32_t)(sphereVerts.size() / 3),
                               sphereInds.data(), (uint32_t)(sphereInds.size() / 3), matClearcoatWood, &meshS3);
    if (res != VLRResult_Success) { fprintf(stderr, "[Error] Sphere 3 (Clearcoat Wood)\n"); goto cleanup; }

    createSphere(sphereVerts, sphereInds, 1.2f, 0.4f, 0.0f, 0.4f);
    res = vlrCreateTriangleMesh(scene, sphereVerts.data(), (uint32_t)(sphereVerts.size() / 3),
                               sphereInds.data(), (uint32_t)(sphereInds.size() / 3), matAnisoMetal, &meshS4);
    if (res != VLRResult_Success) { fprintf(stderr, "[Error] Sphere 4 (Anisotropic Metal)\n"); goto cleanup; }

    // 创建实例
    printf("Creating instances...\n");
    
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
    res = vlrCreateInstance(scene, meshLight, origin, scale, axis, 0.0f, &instLight);
    if (res != VLRResult_Success) { fprintf(stderr, "[Error] Light instance\n"); goto cleanup; }
    res = vlrCreateInstance(scene, meshS0, origin, scale, axis, 0.0f, &instS0);
    if (res != VLRResult_Success) { fprintf(stderr, "[Error] Sphere 0 instance\n"); goto cleanup; }
    res = vlrCreateInstance(scene, meshS1, origin, scale, axis, 0.0f, &instS1);
    if (res != VLRResult_Success) { fprintf(stderr, "[Error] Sphere 1 instance\n"); goto cleanup; }
    res = vlrCreateInstance(scene, meshS2, origin, scale, axis, 0.0f, &instS2);
    if (res != VLRResult_Success) { fprintf(stderr, "[Error] Sphere 2 instance\n"); goto cleanup; }
    res = vlrCreateInstance(scene, meshS3, origin, scale, axis, 0.0f, &instS3);
    if (res != VLRResult_Success) { fprintf(stderr, "[Error] Sphere 3 instance\n"); goto cleanup; }
    res = vlrCreateInstance(scene, meshS4, origin, scale, axis, 0.0f, &instS4);
    if (res != VLRResult_Success) { fprintf(stderr, "[Error] Sphere 4 instance\n"); goto cleanup; }

    res = vlrAddAreaLight(scene, instLight);
    if (res != VLRResult_Success) { fprintf(stderr, "[Error] Add area light\n"); goto cleanup; }

    printf("Scene setup complete.\n\n");

    // 设置相机（参考 cornell_box_improved_test.cpp）
    // 位置: (0, 1.5, 6.0), 看向 (0, 1.0, 0) - 看向球体中心高度
    camera.position[0] = 0.0f;
    camera.position[1] = 1.5f;
    camera.position[2] = 6.0f;
    
    // 计算方向向量（看向球体中心高度 y=1.0）
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
    res = vlrRender(context, scene, width, height, numSamples, 3 /* Wavefront */);
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
    cudaErr = cudaMemcpy(hostBuffer.data(), deviceBuffer, bufferSize, cudaMemcpyDeviceToHost);
    if (cudaErr != cudaSuccess) {
        fprintf(stderr, "[Error] cudaMemcpy: %s\n", cudaGetErrorString(cudaErr));
        goto cleanup;
    }

    savePNG(outputFile, width, height, hostBuffer.data(), numSamples, 0.5f);

    printf("\n=== Disney BRDF Test Complete ===\n");
    printf("Spheres (left to right):\n");
    printf("  1. Plastic (metallic=0, specular=0.5)\n");
    printf("  2. Metal (metallic=1, roughness=0.2)\n");
    printf("  3. Fabric (sheen=1, sheenTint=0.5)\n");
    printf("  4. Clearcoat Wood (clearcoat=1, clearcoatGloss=0.8)\n");
    printf("  5. Anisotropic Metal (metallic=1, anisotropic=0.8)\n");

cleanup:
    vlrDestroyScene(scene);
    vlrDestroyContext(context);
    return (res != VLRResult_Success) ? 1 : 0;
}
