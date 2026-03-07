// ============================================================================
// VLR Wavefront 简单渲染测试
//
// 用于验证 Wavefront 路径追踪渲染器的基本功能。
// 创建最小测试场景（Cornell Box 风格），使用 VLR C API 完成渲染并输出 PPM 图像。
//
// 作者：VLR 开发团队
// 创建日期：2026-03-07
// 环境：CUDA 13.1, OptiX 8.0.0, VS2022
// ============================================================================

#include <vlr/vlr.h>
#include <cuda_runtime.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>

// ============================================================================
// 辅助函数：保存 PPM 图像
// ============================================================================

/// 将线性 RGB [0,inf) 转换为 sRGB 并写入 PPM 文件
static void savePPM(const char* filename, uint32_t width, uint32_t height,
                    const float* rgb) {
    FILE* fp = fopen(filename, "wb");
    if (!fp) {
        fprintf(stderr, "[Error] Cannot create output file: %s\n", filename);
        return;
    }
    fprintf(fp, "P6\n%u %u\n255\n", width, height);

    for (uint32_t i = 0; i < width * height; ++i) {
        float r = rgb[i * 3 + 0];
        float g = rgb[i * 3 + 1];
        float b = rgb[i * 3 + 2];

        // 简单 tone mapping：clamp + gamma 2.2
        r = r < 0 ? 0 : (r > 1 ? 1 : r);
        g = g < 0 ? 0 : (g > 1 ? 1 : g);
        b = b < 0 ? 0 : (b > 1 ? 1 : b);

        r = powf(r, 1.0f / 2.2f);
        g = powf(g, 1.0f / 2.2f);
        b = powf(b, 1.0f / 2.2f);

        unsigned char ur = (unsigned char)(r * 255.99f);
        unsigned char ug = (unsigned char)(g * 255.99f);
        unsigned char ub = (unsigned char)(b * 255.99f);
        fputc(ur, fp);
        fputc(ug, fp);
        fputc(ub, fp);
    }
    fclose(fp);
    printf("[Done] Image saved: %s (%u x %u)\n", filename, width, height);
}

// ============================================================================
// Cornell Box 几何数据
// ============================================================================

/// Cornell Box 墙壁网格顶点（地板、天花板、后墙、左墙、右墙，不含区域光）
static const float kWallVertices[] = {
    // 地板 y=0
    0, 0, 0,  1, 0, 0,  1, 0, 1,  0, 0, 1,
    // 天花板 y=1
    0, 1, 0,  0, 1, 1,  1, 1, 1,  1, 1, 0,
    // 后墙 z=0
    0, 0, 0,  0, 1, 0,  1, 1, 0,  1, 0, 0,
    // 左墙 x=0
    0, 0, 0,  0, 0, 1,  0, 1, 1,  0, 1, 0,
    // 右墙 x=1
    1, 0, 0,  1, 0, 1,  1, 1, 1,  1, 1, 0
};

/// Cornell Box 墙壁三角形索引（5 个面 × 2 三角形 = 10 三角形）
static const uint32_t kWallIndices[] = {
    0, 1, 2,  0, 2, 3,       // 地板
    4, 5, 6,  4, 6, 7,       // 天花板
    8, 9, 10, 8, 10, 11,     // 后墙
    12, 13, 14, 12, 14, 15,  // 左墙
    16, 17, 18, 16, 18, 19   // 右墙
};

/// 区域光四边形（天花板中央，y 略低于 1 避免 z-fighting）
static const float kLightVertices[] = {
    0.25f, 0.999f, 0.25f,  0.75f, 0.999f, 0.25f,
    0.75f, 0.999f, 0.75f,  0.25f, 0.999f, 0.75f
};
static const uint32_t kLightIndices[] = { 0, 1, 2,  0, 2, 3 };

// ============================================================================
// 主函数
// ============================================================================

int main(int argc, char** argv) {
    // 立即输出，检测崩溃位置
    printf("=== VLR Simple Render Test START ===\n");
    fflush(stdout);
    
    // 默认参数
    uint32_t width = 512;
    uint32_t height = 512;
    uint32_t numSamples = 16;
    const char* outputFile = "output.ppm";

    // 解析命令行参数
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-w") == 0 && i + 1 < argc) {
            width = (uint32_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "-h") == 0 && i + 1 < argc) {
            height = (uint32_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            numSamples = (uint32_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            outputFile = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-?") == 0) {
            printf("Usage: %s [options]\n", argv[0]);
            printf("Options:\n");
            printf("  -w <width>   Image width (default: 512)\n");
            printf("  -h <height>  Image height (default: 512)\n");
            printf("  -s <samples> Samples per pixel (default: 16)\n");
            printf("  -o <file>    Output PPM filename (default: output.ppm)\n");
            return 0;
        }
    }

    printf("=== VLR Wavefront Simple Render Test ===\n");
    printf("Resolution: %u x %u, Samples: %u, Output: %s\n", width, height, numSamples, outputFile);
    fflush(stdout);

    // 创建 Context 和 Scene
    printf("[Step 1] Creating VLR Context...\n");
    fflush(stdout);
    
    VLRContext context = nullptr;
    VLRScene scene = nullptr;
    VLRResult res;

    res = vlrCreateContext(nullptr, 0, &context);
    
    printf("[Step 1] vlrCreateContext returned: %d\n", res);
    fflush(stdout);
    if (res != VLRResult_Success || !context) {
        fprintf(stderr, "[Error] Failed to create Context: %d\n", res);
        return 1;
    }

    res = vlrCreateScene(context, &scene);
    if (res != VLRResult_Success || !scene) {
        fprintf(stderr, "[Error] Failed to create Scene: %d\n", res);
        vlrDestroyContext(context);
        return 1;
    }

    // 创建材质：漫反射（地板、墙）和发光（区域光）
    VLRMaterial matWalls = nullptr;
    float wallColor[] = { 0.73f, 0.73f, 0.73f };
    res = vlrCreateMaterial(scene, 0 /* Matte */, wallColor, nullptr /* 不发光 */, &matWalls);
    if (res != VLRResult_Success) {
        fprintf(stderr, "[Error] Failed to create wall material\n");
        goto cleanup;
    }

    VLRMaterial matLight = nullptr;
    float lightEmission[] = { 15.0f, 15.0f, 15.0f };  // 强白光
    res = vlrCreateMaterial(scene, 0 /* Matte */, wallColor, lightEmission, &matLight);
    if (res != VLRResult_Success) {
        fprintf(stderr, "[Error] Failed to create emissive material\n");
        goto cleanup;
    }

    // 创建墙面网格（地板、天花板、后墙、左墙、右墙）
    VLRTriangleMesh meshWalls = nullptr;
    res = vlrCreateTriangleMesh(scene,
        kWallVertices, 20,
        kWallIndices, 10,
        matWalls,
        &meshWalls);
    if (res != VLRResult_Success) {
        fprintf(stderr, "[Error] Failed to create wall mesh\n");
        goto cleanup;
    }

    // 创建区域光网格（天花板中央发光矩形）
    VLRTriangleMesh meshLight = nullptr;
    res = vlrCreateTriangleMesh(scene,
        kLightVertices, 4,
        kLightIndices, 2,
        matLight,
        &meshLight);
    if (res != VLRResult_Success) {
        fprintf(stderr, "[Error] Failed to create area light mesh\n");
        vlrDestroyTriangleMesh(meshWalls);
        goto cleanup;
    }

    // 创建实例：墙面与区域光
    float origin[] = { 0, 0, 0 };
    float scale[] = { 1, 1, 1 };
    float axis[] = { 0, 1, 0 };

    VLRInstance instWalls = nullptr;
    res = vlrCreateInstance(scene, meshWalls, origin, scale, axis, 0.0f, &instWalls);
    if (res != VLRResult_Success) {
        fprintf(stderr, "[Error] Failed to create wall instance\n");
        vlrDestroyTriangleMesh(meshLight);
        vlrDestroyTriangleMesh(meshWalls);
        goto cleanup;
    }

    VLRInstance instLight = nullptr;
    res = vlrCreateInstance(scene, meshLight, origin, scale, axis, 0.0f, &instLight);
    if (res != VLRResult_Success) {
        fprintf(stderr, "[Error] Failed to create area light instance\n");
        vlrDestroyInstance(instWalls);
        vlrDestroyTriangleMesh(meshLight);
        vlrDestroyTriangleMesh(meshWalls);
        goto cleanup;
    }

    // 添加区域光
    res = vlrAddAreaLight(scene, instLight);
    if (res != VLRResult_Success) {
        fprintf(stderr, "[Warning] Failed to add area light: %d, continuing render\n", res);
    }

    // 设置透视相机（位于盒前，望向盒子内部）
    VLRCameraParams cam = {};
    cam.position[0] = 0.5f;
    cam.position[1] = 0.5f;
    cam.position[2] = 1.8f;
    cam.direction[0] = 0.0f;
    cam.direction[1] = 0.0f;
    cam.direction[2] = -1.0f;
    cam.up[0] = 0.0f;
    cam.up[1] = 1.0f;
    cam.up[2] = 0.0f;
    cam.fovY = 45.0f * 3.14159265f / 180.0f;
    cam.aspect = (float)width / (float)height;
    cam.lensRadius = 0.0f;
    cam.focusDistance = 2.0f;
    cam.focalLength = 0.0f;
    cam.cameraType = 0;  // 透视

    res = vlrSetCamera(scene, &cam);
    if (res != VLRResult_Success) {
        fprintf(stderr, "[Error] Failed to set camera\n");
        goto cleanup_inst;
    }

    // 执行渲染（使用 Wavefront 渲染器）
    printf("[Render] Starting render...\n");
    fflush(stdout);
    res = vlrRender(context, scene, width, height, numSamples, VLRRenderer_WavefrontPathTracing);
    fflush(stdout);
    if (res != VLRResult_Success) {
        fprintf(stderr, "[Error] Render failed: %d\n", res);
        fflush(stderr);
        goto cleanup_inst;
    }
    printf("[Render] Done\n");
    fflush(stdout);

    // 获取输出缓冲区并保存为 PPM
    void* devBuffer = vlrGetOutputBuffer(context);
    if (!devBuffer) {
        fprintf(stderr, "[Error] Cannot get output buffer\n");
        goto cleanup_inst;
    }

    size_t pixelCount = (size_t)width * height;
    size_t bufferSize = pixelCount * sizeof(float) * 3;  // DiscretizedSpectrum: r,g,b
    float* hostRGB = (float*)malloc(bufferSize);
    if (!hostRGB) {
        fprintf(stderr, "[Error] Out of memory\n");
        goto cleanup_inst;
    }

    cudaError_t err = cudaMemcpy(hostRGB, devBuffer, bufferSize, cudaMemcpyDeviceToHost);
    if (err != cudaSuccess) {
        fprintf(stderr, "[Error] cudaMemcpy failed: %s\n", cudaGetErrorString(err));
        free(hostRGB);
        goto cleanup_inst;
    }

    // 归一化：除以采样数以得到平均辐射
    float invSamples = 1.0f / (float)numSamples;
    for (size_t i = 0; i < pixelCount * 3; ++i) {
        hostRGB[i] *= invSamples;
    }

    savePPM(outputFile, width, height, hostRGB);
    free(hostRGB);

cleanup_inst:
    vlrDestroyInstance(instLight);
    vlrDestroyInstance(instWalls);
    vlrDestroyTriangleMesh(meshLight);
    vlrDestroyTriangleMesh(meshWalls);
cleanup:
    vlrDestroyMaterial(matLight);
    vlrDestroyMaterial(matWalls);
    vlrDestroyScene(scene);
    vlrDestroyContext(context);

    printf("=== Test complete ===\n");
    return 0;
}
