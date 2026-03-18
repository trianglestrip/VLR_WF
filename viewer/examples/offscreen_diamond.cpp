// ============================================================================
// VLR Diamond Dispersion Renderer
//
// Offscreen render: procedurally generate a Round Brilliant Cut diamond,
// illuminate with surrounding lighting, and capture the chromatic dispersion
// (fire) effect caused by wavelength-dependent refraction.
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

static uint32_t g_width  = 800;
static uint32_t g_height = 800;
static int      g_spp    = 512;
static float    g_exposure = 2.5f;
static const char* g_outPath = "docs/images/diamond_dispersion.png";

static constexpr float PI = 3.14159265358979323846f;

// ============================================================================
// Brilliant Cut Diamond Geometry Generator (GIA Ideal Proportions)
//
// All triangles are wound CCW when viewed from outside → outward-facing normals.
// ============================================================================
struct DiamondParams {
    float girdleRadius     = 1.0f;
    float tableRatio       = 0.56f;
    float crownAngleDeg    = 34.5f;
    float pavilionAngleDeg = 40.75f;
    float girdleHeight     = 0.025f;
};

static void generateBrilliantCut(
    const DiamondParams& p,
    std::vector<float>& vertices,
    std::vector<uint32_t>& indices)
{
    const int N = 16;   // girdle facets
    const float R = p.girdleRadius;
    const float tableR = R * p.tableRatio;
    const float crownAngle = p.crownAngleDeg * PI / 180.0f;
    const float pavilionAngle = p.pavilionAngleDeg * PI / 180.0f;

    const float crownHeight = (R - tableR) * tanf(crownAngle);
    const float pavilionDepth = R * tanf(pavilionAngle);
    const float gH = p.girdleHeight;

    auto addVert = [&](float x, float y, float z) -> uint32_t {
        uint32_t idx = (uint32_t)(vertices.size() / 3);
        vertices.push_back(x);
        vertices.push_back(y);
        vertices.push_back(z);
        return idx;
    };

    // Outward-facing CCW triangle
    auto addTriCCW = [&](uint32_t a, uint32_t b, uint32_t c) {
        indices.push_back(a);
        indices.push_back(b);
        indices.push_back(c);
    };

    // --- Vertices ---
    uint32_t culetIdx = addVert(0.0f, -pavilionDepth, 0.0f);

    std::vector<uint32_t> girdleLow(N), girdleHigh(N);
    for (int i = 0; i < N; i++) {
        float angle = 2.0f * PI * i / N;
        float x = R * cosf(angle), z = R * sinf(angle);
        girdleLow[i]  = addVert(x, 0.0f, z);
        girdleHigh[i] = addVert(x, gH, z);
    }

    // Crown bezel vertices (halfway between girdle and table, offset by half-step)
    float bezelR = (R + tableR) * 0.5f;
    float bezelY = gH + crownHeight * 0.45f;
    std::vector<uint32_t> bezelVerts(N);
    for (int i = 0; i < N; i++) {
        float angle = 2.0f * PI * (i + 0.5f) / N;
        bezelVerts[i] = addVert(bezelR * cosf(angle), bezelY, bezelR * sinf(angle));
    }

    float tableY = gH + crownHeight;
    std::vector<uint32_t> tableVerts(N);
    for (int i = 0; i < N; i++) {
        float angle = 2.0f * PI * i / N;
        tableVerts[i] = addVert(tableR * cosf(angle), tableY, tableR * sinf(angle));
    }

    uint32_t tableCenterIdx = addVert(0.0f, tableY, 0.0f);

    // --- Pavilion (viewed from below/outside, normals point downward/outward) ---
    // Each triangle: culet → girdleLow[i] → girdleLow[next]
    // From outside (below), vertices go CCW
    for (int i = 0; i < N; i++) {
        int next = (i + 1) % N;
        addTriCCW(culetIdx, girdleLow[i], girdleLow[next]);
    }

    // --- Girdle band (normals point outward radially) ---
    for (int i = 0; i < N; i++) {
        int next = (i + 1) % N;
        addTriCCW(girdleLow[i], girdleHigh[i], girdleHigh[next]);
        addTriCCW(girdleLow[i], girdleHigh[next], girdleLow[next]);
    }

    // --- Crown (viewed from above/outside, normals point upward/outward) ---
    // Lower crown: girdleHigh[i] → bezelVerts[i] → girdleHigh[next]  (kite lower half)
    //              girdleHigh[i] → bezelVerts[prev] → bezelVerts[i]   (kite side)
    for (int i = 0; i < N; i++) {
        int next = (i + 1) % N;
        int prev = (i + N - 1) % N;
        addTriCCW(girdleHigh[i], bezelVerts[i], girdleHigh[next]);
        addTriCCW(girdleHigh[i], bezelVerts[prev], bezelVerts[i]);
    }

    // Upper crown: bezelVerts[i] → tableVerts[next] → tableVerts[i]  (star facet)
    //              bezelVerts[i] → bezelVerts[next] → tableVerts[next] (upper kite)
    for (int i = 0; i < N; i++) {
        int next = (i + 1) % N;
        addTriCCW(bezelVerts[i], tableVerts[next], tableVerts[i]);
        addTriCCW(bezelVerts[i], bezelVerts[next], tableVerts[next]);
    }

    // --- Table (from above, CCW) ---
    for (int i = 0; i < N; i++) {
        int next = (i + 1) % N;
        addTriCCW(tableCenterIdx, tableVerts[i], tableVerts[next]);
    }
}

// ============================================================================
// Build Diamond Scene
// ============================================================================
static bool buildScene(VLRScene scene) {
    VLRResult res;

    // Materials
    float darkFloor[]    = { 0.04f, 0.04f, 0.05f };
    float warmGray[]     = { 0.35f, 0.33f, 0.30f };
    float diamondColor[] = { 0.999f, 0.999f, 0.999f };

    VLRMaterial floorMat = nullptr, wallMat = nullptr, diamondMat = nullptr;

    res = vlrCreateMaterial(scene, 0, darkFloor, nullptr, &floorMat);
    if (res != VLRResult_Success) return false;
    res = vlrCreateMaterial(scene, 0, warmGray, nullptr, &wallMat);
    if (res != VLRResult_Success) return false;

    // Diamond: IOR 2.42, exaggerated dispersion for vivid fire
    res = vlrCreateMaterialDispersive(scene, diamondColor, 2.42f, 0.15f, &diamondMat);
    if (res != VLRResult_Success) return false;

    float pos0[] = {0,0,0}, sc1[] = {1,1,1}, ay[] = {0,1,0};
    VLRInstance inst = nullptr;
    VLRTriangleMesh m = nullptr;

    // Floor
    float floorV[] = { -12,0,-12, 12,0,-12, 12,0,12, -12,0,12 };
    uint32_t floorI[] = { 0,1,2, 0,2,3 };
    vlrCreateTriangleMesh(scene, floorV, 4, floorI, 2, floorMat, &m);
    vlrCreateInstance(scene, m, pos0, sc1, ay, 0, &inst);

    // Back wall (catches caustics)
    float bwV[] = { -12,0,-6, 12,0,-6, 12,10,-6, -12,10,-6 };
    uint32_t bwI[] = { 0,1,2, 0,2,3 };
    vlrCreateTriangleMesh(scene, bwV, 4, bwI, 2, wallMat, &m);
    vlrCreateInstance(scene, m, pos0, sc1, ay, 0, &inst);

    // Side walls to reflect more light into diamond
    // Left wall
    float lwV[] = { -6,0,-6, -6,0,6, -6,8,6, -6,8,-6 };
    uint32_t lwI[] = { 0,1,2, 0,2,3 };
    vlrCreateTriangleMesh(scene, lwV, 4, lwI, 2, wallMat, &m);
    vlrCreateInstance(scene, m, pos0, sc1, ay, 0, &inst);

    // Right wall
    float rwV[] = { 6,0,-6, 6,0,6, 6,8,6, 6,8,-6 };
    uint32_t rwI[] = { 0,2,1, 0,3,2 };
    vlrCreateTriangleMesh(scene, rwV, 4, rwI, 2, wallMat, &m);
    vlrCreateInstance(scene, m, pos0, sc1, ay, 0, &inst);

    // Ceiling (soft bounce light)
    float ceilV[] = { -8,6,-8, 8,6,-8, 8,6,8, -8,6,8 };
    uint32_t ceilI[] = { 0,2,1, 0,3,2 };
    vlrCreateTriangleMesh(scene, ceilV, 4, ceilI, 2, wallMat, &m);
    vlrCreateInstance(scene, m, pos0, sc1, ay, 0, &inst);

    // --- Diamond ---
    {
        DiamondParams dp;
        dp.girdleRadius = 0.8f;
        std::vector<float> dv;
        std::vector<uint32_t> di;
        generateBrilliantCut(dp, dv, di);

        // Tilt diamond for an appealing 3/4 view
        float tiltX = 5.0f * PI / 180.0f;
        float tiltZ = 10.0f * PI / 180.0f;
        float cxr = cosf(tiltX), sxr = sinf(tiltX);
        float czr = cosf(tiltZ), szr = sinf(tiltZ);
        float elevateY = dp.girdleRadius * tanf(dp.pavilionAngleDeg * PI / 180.0f) + 0.05f;

        for (size_t i = 0; i < dv.size(); i += 3) {
            float x = dv[i], y = dv[i+1], z = dv[i+2];
            // Rotate around X
            float y1 = y * cxr - z * sxr;
            float z1 = y * sxr + z * cxr;
            // Rotate around Z
            float x2 = x * czr - y1 * szr;
            float y2 = x * szr + y1 * czr;
            dv[i]   = x2;
            dv[i+1] = y2 + elevateY;
            dv[i+2] = z1;
        }

        VLRTriangleMesh dm = nullptr;
        vlrCreateTriangleMesh(scene, dv.data(), (uint32_t)(dv.size()/3),
                              di.data(), (uint32_t)(di.size()/3), diamondMat, &dm);
        VLRInstance dinst = nullptr;
        vlrCreateInstance(scene, dm, pos0, sc1, ay, 0, &dinst);
    }

    // --- Lights (surrounding the diamond for maximum brilliance) ---
    auto addLight = [&](float x0, float y, float z0, float x1, float z1,
                        float er, float eg, float eb) {
        float emission[] = { er, eg, eb };
        float white[] = {1,1,1};
        VLRMaterial lmat = nullptr;
        vlrCreateMaterial(scene, 0, white, emission, &lmat);
        float lv[] = { x0,y,z0, x1,y,z0, x1,y,z1, x0,y,z1 };
        uint32_t li[] = { 0,2,1, 0,3,2 };
        VLRTriangleMesh lm = nullptr;
        vlrCreateTriangleMesh(scene, lv, 4, li, 2, lmat, &lm);
        VLRInstance linst = nullptr;
        vlrCreateInstance(scene, lm, pos0, sc1, ay, 0, &linst);
        vlrAddAreaLight(scene, linst);
    };

    // Key light: large overhead, slightly forward
    addLight(-1.5f, 5.0f, -1.0f, 1.5f, 1.0f, 60.0f, 60.0f, 60.0f);
    // Fill light: upper left
    addLight(-4.0f, 4.5f, -0.5f, -2.5f, 1.0f, 30.0f, 28.0f, 25.0f);
    // Accent: upper right, smaller but brighter
    addLight(2.0f, 4.5f, -1.5f, 3.5f, -0.2f, 50.0f, 50.0f, 55.0f);
    // Back fill: behind diamond above
    addLight(-1.0f, 4.0f, -3.0f, 1.0f, -2.0f, 25.0f, 25.0f, 30.0f);
    // Front fill: gentle light from camera direction
    addLight(-1.0f, 3.5f, 3.0f, 1.0f, 4.5f, 20.0f, 20.0f, 20.0f);

    // Camera: elevated oblique view showing crown, table, and fire
    VLRCameraParams cam = {};
    cam.position[0] =  2.0f;
    cam.position[1] =  2.2f;
    cam.position[2] =  3.2f;
    cam.direction[0] = -0.45f;
    cam.direction[1] = -0.40f;
    cam.direction[2] = -1.0f;
    cam.up[0] = 0; cam.up[1] = 1; cam.up[2] = 0;
    cam.fovY = 26.0f * PI / 180.0f;
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

    printf("=== VLR Diamond Dispersion Renderer ===\n");
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

    printf("[VLR] Building diamond scene...\n");
    if (!buildScene(scene)) { fprintf(stderr, "Failed to build diamond scene\n"); return 1; }

    vlrSetDenoiserConfig(context, true, false, false, 1.0f);

    printf("[VLR] Starting render (%d spp, denoiser ON)...\n", g_spp);
    vlrBeginProgressive(context, scene, g_width, g_height);

    auto t0 = std::chrono::steady_clock::now();
    uint32_t accumFrames = 0;
    for (int s = 0; s < g_spp; s++) {
        vlrRenderOneSample(context, &accumFrames);
        if ((s+1) % 64 == 0 || s+1 == g_spp)
            printf("  [%d/%d spp]\n", s+1, g_spp);
    }
    auto t1 = std::chrono::steady_clock::now();
    float elapsed = std::chrono::duration<float>(t1 - t0).count();
    printf("[VLR] Render complete: %d spp in %.2f s (%.1f spp/s)\n",
           g_spp, elapsed, g_spp / elapsed);

    uint32_t numPixels = g_width * g_height;
    uint8_t* d_rgba = nullptr;
    cudaMalloc(&d_rgba, numPixels * 4);
    vlrTonemapToRGBA8(context, d_rgba, g_exposure, 2.2f);

    std::vector<uint8_t> h_rgba(numPixels * 4);
    cudaMemcpy(h_rgba.data(), d_rgba, numPixels * 4, cudaMemcpyDeviceToHost);
    cudaFree(d_rgba);

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
