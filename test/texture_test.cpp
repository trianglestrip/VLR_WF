// ============================================================================
// VLR Wavefront - Texture System Test
//
// 测试内容：
// 1. 加载各种格式的纹理（PNG, JPG, EXR, HDR）
// 2. 程序化纹理应用到材质（棋盘格 BaseColor）
// 3. 纹理坐标变换（scale via extent/gridSize）
// 4. 渲染并保存多个测试图像
//
// 场景：Cornell Box + 中心平面（纹理化材质）
//
// 注：图像纹理（BaseColor/Roughness/Normal Map）的完整 API 尚在开发中，
//     当前使用程序化棋盘格纹理验证纹理系统。
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
// Texture Loading Test (verify resource files exist - full load uses libVLR image_loader)
// ============================================================================
static void testTextureResources(const char* resourceDir) {
    printf("\n--- Texture Resource Check ---\n");
    struct TestFile { const char* path; const char* format; };
    const TestFile files[] = {
        { "checkerboard_line.png", "PNG" },
        { "material_test/jumping_colors.png", "PNG" },
        { "material_test/grid_80p_white_18p_gray.png", "PNG" },
        { "environments/WhiteOne.exr", "EXR" },
    };
    for (const auto& f : files) {
        std::string path = std::string(resourceDir) + "/" + f.path;
        FILE* fp = fopen(path.c_str(), "rb");
        if (fp) {
            fclose(fp);
            printf("  [OK] %s (%s)\n", f.path, f.format);
        } else {
            printf("  [Skip] %s (file not found)\n", f.path);
        }
    }
    printf("Note: Full texture load (PNG/JPG/EXR/HDR) uses libVLR image_loader.\n");
}

// ============================================================================
// Cornell Box Geometry
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

// Center plane (horizontal, y=0.8, 1.2x1.2) - for textured material
static const float kCenterPlaneVertices[] = {
    -0.6f, 0.8f, -0.6f,  -0.6f, 0.8f, 0.6f,  0.6f, 0.8f, 0.6f,  0.6f, 0.8f, -0.6f
};
static const uint32_t kCenterPlaneIndices[] = { 0, 1, 2,  0, 2, 3 };

// ============================================================================
// Run single render with given checkerboard params
// ============================================================================
static int runTextureTest(
    VLRContext context, VLRScene scene,
    uint32_t width, uint32_t height, uint32_t numSamples, float exposure,
    const char* outputFile,
    float floorColor0[3], float floorColor1[3], uint32_t floorGrid, float floorExtent,
    float centerColor0[3], float centerColor1[3], uint32_t centerGrid, float centerExtent)
{
    VLRResult res = VLRResult_Success;
    float origin[] = { 0, 0, 0 };
    float scale[] = { 1, 1, 1 };
    float axis[] = { 0, 1, 0 };

    float whiteColor[] = { 0.522f, 0.522f, 0.522f };
    float redColor[] = { 0.522f, 0.0508f, 0.0508f };
    float blueColor[] = { 0.0508f, 0.0508f, 0.522f };
    float lightEmission[] = { 30.0f, 30.0f, 30.0f };

    VLRMaterial matWhite = nullptr;
    VLRMaterial matRed = nullptr;
    VLRMaterial matBlue = nullptr;
    VLRMaterial matFloor = nullptr;
    VLRMaterial matCenter = nullptr;
    VLRMaterial matLight = nullptr;

    VLRTriangleMesh meshFloor = nullptr, meshCeiling = nullptr, meshBackWall = nullptr;
    VLRTriangleMesh meshLeftWall = nullptr, meshRightWall = nullptr, meshFrontWall = nullptr;
    VLRTriangleMesh meshLight = nullptr, meshCenter = nullptr;

    VLRInstance instFloor = nullptr, instCeiling = nullptr, instBackWall = nullptr;
    VLRInstance instLeftWall = nullptr, instRightWall = nullptr, instFrontWall = nullptr;
    VLRInstance instLight = nullptr, instCenter = nullptr;

    res = vlrCreateMaterial(scene, 0, whiteColor, nullptr, &matWhite);
    if (res != VLRResult_Success) return 1;
    res = vlrCreateMaterial(scene, 0, redColor, nullptr, &matRed);
    if (res != VLRResult_Success) return 1;
    res = vlrCreateMaterial(scene, 0, blueColor, nullptr, &matBlue);
    if (res != VLRResult_Success) return 1;
    res = vlrCreateMaterialCheckerboard(scene, floorColor0, floorColor1, floorGrid, floorExtent, &matFloor);
    if (res != VLRResult_Success) return 1;
    res = vlrCreateMaterialCheckerboard(scene, centerColor0, centerColor1, centerGrid, centerExtent, &matCenter);
    if (res != VLRResult_Success) return 1;
    res = vlrCreateMaterial(scene, 0, whiteColor, lightEmission, &matLight);
    if (res != VLRResult_Success) return 1;

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
    res = vlrCreateTriangleMesh(scene, kCenterPlaneVertices, 4, kCenterPlaneIndices, 2, matCenter, &meshCenter);
    if (res != VLRResult_Success) return 1;

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
    res = vlrCreateInstance(scene, meshCenter, origin, scale, axis, 0.0f, &instCenter);
    if (res != VLRResult_Success) return 1;

    res = vlrAddAreaLight(scene, instLight);
    if (res != VLRResult_Success) return 1;

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

    res = vlrRender(context, scene, width, height, numSamples, vlr::VLRRenderer_WavefrontPathTracing);
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
// Main
// ============================================================================
int main(int argc, char** argv) {
    printf("=== VLR Texture System Test START ===\n");
    fflush(stdout);

    uint32_t width = 512;
    uint32_t height = 512;
    uint32_t numSamples = 256;
    float exposure = 0.5f;

    std::string resourceDir = "test/resources";
    if (argc >= 2) resourceDir = argv[1];

    // 1. Texture resource check
    testTextureResources(resourceDir.c_str());

    VLRContext context = nullptr;
    VLRScene scene = nullptr;
    VLRResult res = VLRResult_Success;

    res = vlrCreateContext(nullptr, 0, &context);
    if (res != VLRResult_Success || !context) {
        fprintf(stderr, "[Error] Failed to create Context\n");
        return 1;
    }

    printf("\n--- Rendering texture test images ---\n");

    float black[] = { 0.05f, 0.05f, 0.05f };
    float white[] = { 0.9f, 0.9f, 0.9f };
    float red[] = { 0.6f, 0.1f, 0.1f };
    float blue[] = { 0.1f, 0.1f, 0.6f };

    int r1 = 1, r2 = 1, r3 = 1, r4 = 1, r5 = 1;

    // Test 1: Default checkerboard
    res = vlrCreateScene(context, &scene);
    if (res == VLRResult_Success && scene) {
        r1 = runTextureTest(context, scene, width, height, numSamples, exposure,
            "texture_test_01_default.png",
            black, white, 20, 1.5f, black, white, 8, 0.6f);
        vlrDestroyScene(scene);
    }

    // Test 2: Fine scale
    res = vlrCreateScene(context, &scene);
    if (res == VLRResult_Success && scene) {
        r2 = runTextureTest(context, scene, width, height, numSamples, exposure,
            "texture_test_02_fine.png",
            black, white, 20, 1.5f, black, white, 16, 0.6f);
        vlrDestroyScene(scene);
    }

    // Test 3: Coarse scale
    res = vlrCreateScene(context, &scene);
    if (res == VLRResult_Success && scene) {
        r3 = runTextureTest(context, scene, width, height, numSamples, exposure,
            "texture_test_03_coarse.png",
            black, white, 20, 1.5f, black, white, 4, 0.6f);
        vlrDestroyScene(scene);
    }

    // Test 4: Colored checkerboard
    res = vlrCreateScene(context, &scene);
    if (res == VLRResult_Success && scene) {
        r4 = runTextureTest(context, scene, width, height, numSamples, exposure,
            "texture_test_04_colored.png",
            black, white, 20, 1.5f, red, blue, 8, 0.6f);
        vlrDestroyScene(scene);
    }

    // Test 5: Scale variation
    res = vlrCreateScene(context, &scene);
    if (res == VLRResult_Success && scene) {
        r5 = runTextureTest(context, scene, width, height, numSamples, exposure,
            "texture_test_05_scale.png",
            black, white, 10, 1.5f, black, white, 8, 1.2f);
        vlrDestroyScene(scene);
    }
    vlrDestroyContext(context);

    int total = r1 + r2 + r3 + r4 + r5;
    printf("\n=== Texture Test Complete ===\n");
    printf("Output: texture_test_01_default.png .. texture_test_05_scale.png\n");
    return (total == 0) ? 0 : 1;
}
