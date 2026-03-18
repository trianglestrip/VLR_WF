// ============================================================================
// VLR Offscreen Renderer
//
// 无窗口离屏渲染：构建 Cornell Box 场景 → 渲染 N spp → 保存 PNG。
// 不依赖 GLFW/OpenGL/ImGui。
// ============================================================================

#include <vlr/vlr.h>
#include <cuda_runtime.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <vector>
#include <chrono>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../../test/stb_image_write.h"

static uint32_t g_width  = 512;
static uint32_t g_height = 512;
static int      g_spp    = 256;
static float    g_exposure = 2.0f;
static const char* g_outPath = "docs/images/cornell_box_render.png";

// ============================================================================
// Cornell Box 场景（与 interactive_viewer 参数完全一致）
// ============================================================================
static bool buildScene(VLRScene scene) {
    VLRResult res;

    float white[]  = { 0.5225f, 0.5225f, 0.5225f };
    float red[]    = { 0.5225f, 0.0509f, 0.0509f };
    float blue[]   = { 0.0509f, 0.0509f, 0.5225f };
    float gold_eta[]   = { 0.12481f, 0.46823f, 1.44476f };
    float gold_kappa[] = { 3.32107f, 2.23761f, 1.69196f };
    float lightEmission[] = { 30.0f, 30.0f, 30.0f };

    VLRMaterial whiteMat = nullptr, redMat = nullptr, blueMat = nullptr;
    VLRMaterial goldMat = nullptr, lightMat = nullptr, diamondMat = nullptr;
    VLRMaterial checkerMat = nullptr;

    res = vlrCreateMaterial(scene, 0, white, nullptr, &whiteMat);
    if (res != VLRResult_Success) return false;
    res = vlrCreateMaterial(scene, 0, red, nullptr, &redMat);
    if (res != VLRResult_Success) return false;
    res = vlrCreateMaterial(scene, 0, blue, nullptr, &blueMat);
    if (res != VLRResult_Success) return false;
    res = vlrCreateMaterialConductor(scene, gold_eta, gold_kappa, 0.01f, &goldMat);
    if (res != VLRResult_Success) return false;
    res = vlrCreateMaterial(scene, 0, white, lightEmission, &lightMat);
    if (res != VLRResult_Success) return false;
    float diamondColor[] = { 0.999f, 0.999f, 0.999f };
    res = vlrCreateMaterialEx(scene, 6, diamondColor, 0.0f, 0.0f, 2.42f, nullptr, &diamondMat);
    if (res != VLRResult_Success) return false;
    float checkerDark[] = { 0.05f, 0.05f, 0.05f };
    float checkerLight[] = { 0.50f, 0.50f, 0.50f };
    res = vlrCreateMaterialCheckerboard(scene, checkerDark, checkerLight, 64, 3.0f, &checkerMat);
    if (res != VLRResult_Success) return false;

    float pos0[] = {0,0,0}, sc1[] = {1,1,1}, ay[] = {0,1,0};
    VLRInstance inst = nullptr;
    VLRTriangleMesh m = nullptr;

    // 地板
    float floorV[] = { -1.5f,0,-1.5f, 1.5f,0,-1.5f, 1.5f,0,1.5f, -1.5f,0,1.5f };
    uint32_t floorI[] = { 0,1,2, 0,2,3 };
    vlrCreateTriangleMesh(scene, floorV, 4, floorI, 2, checkerMat, &m);
    vlrCreateInstance(scene, m, pos0, sc1, ay, 0, &inst);

    // 天花板
    float ceilV[] = { -1.5f,3,-1.5f, 1.5f,3,-1.5f, 1.5f,3,1.5f, -1.5f,3,1.5f };
    uint32_t ceilI[] = { 0,2,1, 0,3,2 };
    vlrCreateTriangleMesh(scene, ceilV, 4, ceilI, 2, whiteMat, &m);
    vlrCreateInstance(scene, m, pos0, sc1, ay, 0, &inst);

    // 后墙
    float backV[] = { -1.5f,0,-1.5f, 1.5f,0,-1.5f, 1.5f,3,-1.5f, -1.5f,3,-1.5f };
    uint32_t backI[] = { 0,1,2, 0,2,3 };
    vlrCreateTriangleMesh(scene, backV, 4, backI, 2, whiteMat, &m);
    vlrCreateInstance(scene, m, pos0, sc1, ay, 0, &inst);

    // 左墙（红）
    float leftV[] = { -1.5f,0,-1.5f, -1.5f,0,1.5f, -1.5f,3,1.5f, -1.5f,3,-1.5f };
    uint32_t leftI[] = { 0,1,2, 0,2,3 };
    vlrCreateTriangleMesh(scene, leftV, 4, leftI, 2, redMat, &m);
    vlrCreateInstance(scene, m, pos0, sc1, ay, 0, &inst);

    // 右墙（蓝）
    float rightV[] = { 1.5f,0,-1.5f, 1.5f,0,1.5f, 1.5f,3,1.5f, 1.5f,3,-1.5f };
    uint32_t rightI[] = { 0,2,1, 0,3,2 };
    vlrCreateTriangleMesh(scene, rightV, 4, rightI, 2, blueMat, &m);
    vlrCreateInstance(scene, m, pos0, sc1, ay, 0, &inst);

    // 光源
    float lightV[] = { -0.5f,2.9f,-0.5f, 0.5f,2.9f,-0.5f, 0.5f,2.9f,0.5f, -0.5f,2.9f,0.5f };
    uint32_t lightI[] = { 0,2,1, 0,3,2 };
    vlrCreateTriangleMesh(scene, lightV, 4, lightI, 2, lightMat, &m);
    VLRInstance li = nullptr;
    vlrCreateInstance(scene, m, pos0, sc1, ay, 0, &li);
    vlrAddAreaLight(scene, li);

    // 金属盒子
    float bx=-0.7f, by=0, bz=-0.25f, bs=0.5f;
    float boxV[] = {
        bx-bs,by,bz-bs, bx+bs,by,bz-bs, bx+bs,by,bz+bs, bx-bs,by,bz+bs,
        bx-bs,by+1,bz-bs, bx+bs,by+1,bz-bs, bx+bs,by+1,bz+bs, bx-bs,by+1,bz+bs,
    };
    uint32_t boxI[] = { 0,1,2,0,2,3, 4,6,5,4,7,6, 0,4,5,0,5,1, 2,6,7,2,7,3, 0,3,7,0,7,4, 1,5,6,1,6,2 };
    vlrCreateTriangleMesh(scene, boxV, 8, boxI, 12, goldMat, &m);
    vlrCreateInstance(scene, m, pos0, sc1, ay, 0, &inst);

    // 钻石球
    {
        constexpr int SL = 64, ST = 64;
        float cx=0.7f, cy=0.6f, cz=0.5f, cr=0.6f;
        std::vector<float> sv; std::vector<uint32_t> si;
        for (int j = 0; j <= ST; ++j) {
            float t = 3.14159265f * j / ST, sn = sinf(t), cs = cosf(t);
            for (int i = 0; i <= SL; ++i) {
                float p = 6.28318530f * i / SL;
                sv.push_back(cx+cr*sn*cosf(p)); sv.push_back(cy+cr*cs); sv.push_back(cz+cr*sn*sinf(p));
            }
        }
        for (int j = 0; j < ST; ++j)
            for (int i = 0; i < SL; ++i) {
                uint32_t a = j*(SL+1)+i, b = a+SL+1;
                si.push_back(a); si.push_back(a+1); si.push_back(b);
                si.push_back(a+1); si.push_back(b+1); si.push_back(b);
            }
        VLRTriangleMesh sm = nullptr;
        vlrCreateTriangleMesh(scene, sv.data(), (uint32_t)(sv.size()/3), si.data(), (uint32_t)(si.size()/3), diamondMat, &sm);
        VLRInstance si2 = nullptr;
        vlrCreateInstance(scene, sm, pos0, sc1, ay, 0, &si2);
    }

    // 相机
    VLRCameraParams cam = {};
    cam.position[0]=0; cam.position[1]=1.5f; cam.position[2]=6.0f;
    cam.direction[0]=0; cam.direction[1]=0; cam.direction[2]=-1;
    cam.up[0]=0; cam.up[1]=1; cam.up[2]=0;
    cam.fovY = 40.0f * 3.14159265f / 180.0f;
    cam.aspect = (float)g_width / g_height;
    vlrSetCamera(scene, &cam);
    return true;
}

// ============================================================================
// main
// ============================================================================
int main(int argc, char* argv[]) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-w") == 0 && i+1 < argc) g_width = atoi(argv[++i]);
        else if (strcmp(argv[i], "-h") == 0 && i+1 < argc) g_height = atoi(argv[++i]);
        else if (strcmp(argv[i], "-s") == 0 && i+1 < argc) g_spp = atoi(argv[++i]);
        else if (strcmp(argv[i], "-e") == 0 && i+1 < argc) g_exposure = (float)atof(argv[++i]);
        else if (strcmp(argv[i], "-o") == 0 && i+1 < argc) g_outPath = argv[++i];
    }

    printf("=== VLR Offscreen Renderer ===\n");
    printf("  Resolution: %ux%u\n", g_width, g_height);
    printf("  SPP: %d\n", g_spp);
    printf("  Exposure: %.2f\n", g_exposure);
    printf("  Output: %s\n\n", g_outPath);

    VLRContext context = nullptr;
    VLRScene scene = nullptr;

    VLRResult res = vlrCreateContext(nullptr, 0, &context);
    if (res != VLRResult_Success) { fprintf(stderr, "vlrCreateContext failed: %d\n", res); return 1; }
    res = vlrCreateScene(context, &scene);
    if (res != VLRResult_Success) { fprintf(stderr, "vlrCreateScene failed: %d\n", res); return 1; }

    printf("[VLR] Building scene...\n");
    if (!buildScene(scene)) { fprintf(stderr, "Failed to build scene\n"); return 1; }

    vlrSetDenoiserConfig(context, true, false, false, 1.0f);

    printf("[VLR] Starting render (%d spp, denoiser ON)...\n", g_spp);
    vlrBeginProgressive(context, scene, g_width, g_height);

    auto t0 = std::chrono::steady_clock::now();
    uint32_t accumFrames = 0;
    for (int s = 0; s < g_spp; s++) {
        vlrRenderOneSample(context, &accumFrames);
        if ((s+1) % 32 == 0 || s+1 == g_spp)
            printf("  [%d/%d spp]\n", s+1, g_spp);
    }
    auto t1 = std::chrono::steady_clock::now();
    float elapsed = std::chrono::duration<float>(t1 - t0).count();
    printf("[VLR] Render complete: %d spp in %.2f s (%.1f spp/s)\n",
           g_spp, elapsed, g_spp / elapsed);

    // Tonemap to RGBA8 (CUDA device buffer → host)
    uint32_t numPixels = g_width * g_height;
    uint8_t* d_rgba = nullptr;
    cudaMalloc(&d_rgba, numPixels * 4);
    vlrTonemapToRGBA8(context, d_rgba, g_exposure, 2.2f);

    std::vector<uint8_t> h_rgba(numPixels * 4);
    cudaMemcpy(h_rgba.data(), d_rgba, numPixels * 4, cudaMemcpyDeviceToHost);
    cudaFree(d_rgba);

    // 保存 PNG
    int ok = stbi_write_png(g_outPath, g_width, g_height, 4, h_rgba.data(), g_width * 4);
    if (ok) {
        printf("[OK] Saved: %s\n", g_outPath);
    } else {
        fprintf(stderr, "[FAIL] Could not save: %s\n", g_outPath);
    }

    vlrDestroyScene(scene);
    vlrDestroyContext(context);
    return ok ? 0 : 1;
}
