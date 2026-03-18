// ============================================================================
// VLR Diamond Dispersion Renderer
//
// Offscreen render: procedurally generate a Round Brilliant Cut diamond,
// illuminate with directional lighting, and capture the chromatic dispersion
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
// ============================================================================
struct DiamondParams {
    float girdleRadius  = 1.0f;
    float tableRatio    = 0.53f;  // table/girdle ratio
    float crownAngleDeg = 34.5f;
    float pavilionAngleDeg = 40.75f;
    float girdleHeight  = 0.03f;  // thin girdle ring height
};

static void generateBrilliantCut(
    const DiamondParams& p,
    std::vector<float>& vertices,
    std::vector<uint32_t>& indices)
{
    const int N = 16; // facets around the girdle (must be multiple of 8)
    const float R = p.girdleRadius;
    const float tableR = R * p.tableRatio;
    const float crownAngle = p.crownAngleDeg * PI / 180.0f;
    const float pavilionAngle = p.pavilionAngleDeg * PI / 180.0f;

    const float crownHeight = (R - tableR) * tanf(crownAngle);
    const float pavilionDepth = R * tanf(pavilionAngle);
    const float gH = p.girdleHeight;

    // Y-axis is up. Girdle plane at y=0, crown goes up, pavilion goes down.
    auto addVert = [&](float x, float y, float z) -> uint32_t {
        uint32_t idx = (uint32_t)(vertices.size() / 3);
        vertices.push_back(x);
        vertices.push_back(y);
        vertices.push_back(z);
        return idx;
    };

    auto addTri = [&](uint32_t a, uint32_t b, uint32_t c) {
        indices.push_back(a);
        indices.push_back(b);
        indices.push_back(c);
    };

    // Key vertex rings (all in XZ plane, Y is height)
    // Culet (bottom point)
    uint32_t culetIdx = addVert(0.0f, -pavilionDepth, 0.0f);

    // Girdle vertices (lower edge y=0, upper edge y=gH)
    std::vector<uint32_t> girdleLow(N), girdleHigh(N);
    for (int i = 0; i < N; i++) {
        float angle = 2.0f * PI * i / N;
        float x = R * cosf(angle);
        float z = R * sinf(angle);
        girdleLow[i]  = addVert(x, 0.0f, z);
        girdleHigh[i] = addVert(x, gH, z);
    }

    // Star facet vertices (on the crown, between girdle and table)
    float starR = (R + tableR) * 0.5f;
    float starY = gH + crownHeight * 0.5f;
    std::vector<uint32_t> starVerts(N);
    for (int i = 0; i < N; i++) {
        float angle = 2.0f * PI * (i + 0.5f) / N;
        float x = starR * cosf(angle);
        float z = starR * sinf(angle);
        starVerts[i] = addVert(x, starY, z);
    }

    // Table vertices
    float tableY = gH + crownHeight;
    std::vector<uint32_t> tableVerts(N);
    for (int i = 0; i < N; i++) {
        float angle = 2.0f * PI * i / N;
        float x = tableR * cosf(angle);
        float z = tableR * sinf(angle);
        tableVerts[i] = addVert(x, tableY, z);
    }

    // Table center
    uint32_t tableCenterIdx = addVert(0.0f, tableY, 0.0f);

    // ---- Pavilion facets (lower half) ----
    // Each pavilion main facet connects culet → girdleLow[i] → girdleLow[i+1]
    for (int i = 0; i < N; i++) {
        int next = (i + 1) % N;
        addTri(culetIdx, girdleLow[next], girdleLow[i]);
    }

    // ---- Girdle facets (thin band) ----
    for (int i = 0; i < N; i++) {
        int next = (i + 1) % N;
        addTri(girdleLow[i], girdleLow[next], girdleHigh[next]);
        addTri(girdleLow[i], girdleHigh[next], girdleHigh[i]);
    }

    // ---- Crown facets (upper half) ----
    // Kite facets: girdleHigh[i] → starVerts[i] → girdleHigh[i+1]
    // and girdleHigh[i] → starVerts[i-1] → starVerts[i]
    for (int i = 0; i < N; i++) {
        int next = (i + 1) % N;
        int prev = (i + N - 1) % N;
        // Lower kite triangle
        addTri(girdleHigh[i], girdleHigh[next], starVerts[i]);
        // Upper kite triangle (connecting star to table ring)
        addTri(starVerts[prev], girdleHigh[i], tableVerts[i]);
        addTri(starVerts[prev], tableVerts[i], tableVerts[prev]);
    }

    // Star facets: starVerts[i] → tableVerts[i] → tableVerts[i+1]
    for (int i = 0; i < N; i++) {
        int next = (i + 1) % N;
        addTri(starVerts[i], tableVerts[next], tableVerts[i]);
    }

    // ---- Table face (top polygon, fan from center) ----
    for (int i = 0; i < N; i++) {
        int next = (i + 1) % N;
        addTri(tableCenterIdx, tableVerts[i], tableVerts[next]);
    }
}

// ============================================================================
// Build Diamond Scene
// ============================================================================
static bool buildScene(VLRScene scene) {
    VLRResult res;

    // Materials
    float darkGray[]  = { 0.08f, 0.08f, 0.08f };
    float medGray[]   = { 0.25f, 0.25f, 0.25f };
    float diamondColor[] = { 0.999f, 0.999f, 0.999f };

    VLRMaterial floorMat = nullptr, bgMat = nullptr;
    VLRMaterial diamondMat = nullptr;

    res = vlrCreateMaterial(scene, 0, darkGray, nullptr, &floorMat);
    if (res != VLRResult_Success) return false;
    res = vlrCreateMaterial(scene, 0, medGray, nullptr, &bgMat);
    if (res != VLRResult_Success) return false;

    // Diamond: IOR 2.42, dispersion 0.12 (exaggerated for visual impact; real diamond ≈ 0.044)
    res = vlrCreateMaterialDispersive(scene, diamondColor, 2.42f, 0.12f, &diamondMat);
    if (res != VLRResult_Success) { fprintf(stderr, "Failed to create dispersive material\n"); return false; }

    float pos0[] = {0,0,0}, sc1[] = {1,1,1}, ay[] = {0,1,0};
    VLRInstance inst = nullptr;
    VLRTriangleMesh m = nullptr;

    // Large floor plane
    float floorV[] = { -10,0,-10, 10,0,-10, 10,0,10, -10,0,10 };
    uint32_t floorI[] = { 0,1,2, 0,2,3 };
    vlrCreateTriangleMesh(scene, floorV, 4, floorI, 2, floorMat, &m);
    vlrCreateInstance(scene, m, pos0, sc1, ay, 0, &inst);

    // Back wall (dark)
    float backV[] = { -10,0,-5, 10,0,-5, 10,10,-5, -10,10,-5 };
    uint32_t backI[] = { 0,1,2, 0,2,3 };
    vlrCreateTriangleMesh(scene, backV, 4, backI, 2, bgMat, &m);
    vlrCreateInstance(scene, m, pos0, sc1, ay, 0, &inst);

    // Diamond (placed at center, slightly tilted for visual appeal)
    {
        DiamondParams dp;
        dp.girdleRadius = 0.8f;
        std::vector<float> dv;
        std::vector<uint32_t> di;
        generateBrilliantCut(dp, dv, di);

        // Elevate diamond above floor and apply a slight tilt
        float tiltAngle = 12.0f * PI / 180.0f;
        float cosA = cosf(tiltAngle), sinA = sinf(tiltAngle);
        float elevateY = dp.girdleRadius * tanf(dp.pavilionAngleDeg * PI / 180.0f) + 0.05f;
        for (size_t i = 0; i < dv.size(); i += 3) {
            float x = dv[i], y = dv[i+1], z = dv[i+2];
            // Rotate around X-axis for tilt
            float ny = y * cosA - z * sinA;
            float nz = y * sinA + z * cosA;
            dv[i]   = x;
            dv[i+1] = ny + elevateY;
            dv[i+2] = nz;
        }

        VLRTriangleMesh dm = nullptr;
        vlrCreateTriangleMesh(scene, dv.data(), (uint32_t)(dv.size()/3),
                              di.data(), (uint32_t)(di.size()/3), diamondMat, &dm);
        VLRInstance dinst = nullptr;
        vlrCreateInstance(scene, dm, pos0, sc1, ay, 0, &dinst);
    }

    // Light source 1: overhead area light (key light, slightly forward)
    {
        float emission1[] = { 80.0f, 80.0f, 80.0f };
        VLRMaterial lm1 = nullptr;
        float white[] = {1,1,1};
        vlrCreateMaterial(scene, 0, white, emission1, &lm1);
        float lv[] = { -0.8f,4.0f,-0.5f, 0.8f,4.0f,-0.5f, 0.8f,4.0f,0.5f, -0.8f,4.0f,0.5f };
        uint32_t li[] = { 0,2,1, 0,3,2 };
        vlrCreateTriangleMesh(scene, lv, 4, li, 2, lm1, &m);
        VLRInstance linst = nullptr;
        vlrCreateInstance(scene, m, pos0, sc1, ay, 0, &linst);
        vlrAddAreaLight(scene, linst);
    }

    // Light source 2: side fill light (warm, from upper-left)
    {
        float emission2[] = { 40.0f, 35.0f, 30.0f };
        VLRMaterial lm2 = nullptr;
        float white[] = {1,1,1};
        vlrCreateMaterial(scene, 0, white, emission2, &lm2);
        float lv[] = { -4.0f,3.5f,0.0f, -3.0f,3.5f,0.0f, -3.0f,3.5f,1.0f, -4.0f,3.5f,1.0f };
        uint32_t li[] = { 0,2,1, 0,3,2 };
        vlrCreateTriangleMesh(scene, lv, 4, li, 2, lm2, &m);
        VLRInstance linst2 = nullptr;
        vlrCreateInstance(scene, m, pos0, sc1, ay, 0, &linst2);
        vlrAddAreaLight(scene, linst2);
    }

    // Light source 3: small bright spot from upper-right (accent for fire)
    {
        float emission3[] = { 100.0f, 100.0f, 100.0f };
        VLRMaterial lm3 = nullptr;
        float white[] = {1,1,1};
        vlrCreateMaterial(scene, 0, white, emission3, &lm3);
        float lv[] = { 2.5f,3.8f,-1.0f, 3.0f,3.8f,-1.0f, 3.0f,3.8f,-0.5f, 2.5f,3.8f,-0.5f };
        uint32_t li[] = { 0,2,1, 0,3,2 };
        vlrCreateTriangleMesh(scene, lv, 4, li, 2, lm3, &m);
        VLRInstance linst3 = nullptr;
        vlrCreateInstance(scene, m, pos0, sc1, ay, 0, &linst3);
        vlrAddAreaLight(scene, linst3);
    }

    // Camera: close-up, slightly above, looking down at diamond
    VLRCameraParams cam = {};
    cam.position[0] = 0.0f;
    cam.position[1] = 1.8f;
    cam.position[2] = 3.5f;
    cam.direction[0] = 0.0f;
    cam.direction[1] = -0.35f;
    cam.direction[2] = -1.0f;
    cam.up[0] = 0; cam.up[1] = 1; cam.up[2] = 0;
    cam.fovY = 30.0f * PI / 180.0f;
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
