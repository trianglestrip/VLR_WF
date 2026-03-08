// ============================================================================
// VLR Wavefront - Improved Cornell Box Test Scene
//
// Reference: F:\project\OfflineRenderer\libWR\test\cornell_box_var_test.cpp
//
// Scene specifications:
// - 3×3×3 box (L=-1.5, R=1.5; B=0, T=3; N=-1.5, F=1.5)
// - sRGB to linear color conversion for walls
// - Stronger light (80, 80, 80), 1.0×1.0 at y=2.9
// - Glass sphere: center (-0.6, 0.5, 0.0), radius 0.5, IOR 2.4 (diamond) [left]
// - Metal box: center (0.6, 0.5, 0.0), size 1.0, rotated 20° around Y [right]
// - Camera: position (0, 1.5, 6.0), target (0, 1.5, 0), fovY 40°
// - Checkerboard floor: 5×5 grid (position-based)
//
// Author: VLR Development Team
// Created: 2026-03-07
// Environment: CUDA 13.1, OptiX 8.0.0, VS2022
// ============================================================================

#include <vlr/vlr.h>
#include <cuda_runtime.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <vector>
#include <string>

#include "ini_parser.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

// ============================================================================
// Usage
// ============================================================================
static void printUsage(const char* prog) {
    printf("\nUsage:\n");
    printf("  %s [scene.ini] [performance.ini]\n", prog);
    printf("  %s -w <width> -h <height> -s <samples> -o <output>\n", prog);
    printf("\nModes:\n");
    printf("  1. INI mode:  %s scene.ini [performance.ini]\n", prog);
    printf("     - scene.ini: [Render] Width,Height,Samples,MaxDepth,Exposure\n");
    printf("                   [Output] Filename, Format\n");
    printf("                   [Camera] PositionX/Y/Z, TargetX/Y/Z, FOV, LensRadius, FocusDistance\n");
    printf("     - performance.ini: delegated to libVLR (Optimization, KernelConfig, etc.)\n");
    printf("  2. Legacy mode: -w -h -s -o (used when no INI provided)\n");
    printf("\nPriority: INI file > command line > defaults\n");
    printf("\nExamples:\n");
    printf("  %s config_presets/preview_scene.ini config_presets/preview_performance.ini\n", prog);
    printf("  %s -w 800 -h 600 -s 512 -o output.png\n", prog);
    printf("\n");
}

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

static const float PI = 3.14159265358979323846f;
static const float TWO_PI = 6.28318530717958647692f;

/// UV sphere: center (cx,cy,cz), radius, segments (longitude), rings (latitude)
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
                indices.push_back(second);
                indices.push_back(first + 1);
            }
            if (lat != rings - 1) {
                indices.push_back(first + 1);
                indices.push_back(second);
                indices.push_back(second + 1);
            }
        }
    }
}

/// Axis-aligned box at origin (matching OfflineRenderer createBox), then rotate and translate
static void createRotatedBox(std::vector<float>& vertices, std::vector<uint32_t>& indices,
                             float cx, float cy, float cz, float size, float rotationY) {
    float halfSize = size * 0.5f;
    float cosY = std::cos(rotationY);
    float sinY = std::sin(rotationY);

    // createBox at origin: 24 vertices (4 per face)
    float verts[] = {
        -halfSize, -halfSize, halfSize,   halfSize, -halfSize, halfSize,
        halfSize, halfSize, halfSize,     -halfSize, halfSize, halfSize,
        halfSize, -halfSize, -halfSize,   -halfSize, -halfSize, -halfSize,
        -halfSize, halfSize, -halfSize,    halfSize, halfSize, -halfSize,
        -halfSize, -halfSize, -halfSize,   -halfSize, -halfSize, halfSize,
        -halfSize, halfSize, halfSize,    -halfSize, halfSize, -halfSize,
        halfSize, -halfSize, halfSize,     halfSize, -halfSize, -halfSize,
        halfSize, halfSize, -halfSize,    halfSize, halfSize, halfSize,
        -halfSize, halfSize, halfSize,    halfSize, halfSize, halfSize,
        halfSize, halfSize, -halfSize,    -halfSize, halfSize, -halfSize,
        -halfSize, -halfSize, -halfSize,   halfSize, -halfSize, -halfSize,
        halfSize, -halfSize, halfSize,    -halfSize, -halfSize, halfSize
    };

    uint32_t faceInds[] = {
        0, 2, 1, 0, 3, 2, 4, 6, 5, 4, 7, 6,
        8, 10, 9, 8, 11, 10, 12, 14, 13, 12, 15, 14,
        16, 18, 17, 16, 19, 18, 20, 22, 21, 20, 23, 22
    };

    vertices.clear();
    indices.clear();

    for (int i = 0; i < 24; ++i) {
        float x = verts[i * 3 + 0];
        float y = verts[i * 3 + 1];
        float z = verts[i * 3 + 2];
        float newX = cosY * x - sinY * z + cx;
        float newZ = sinY * x + cosY * z + cz;
        vertices.push_back(newX);
        vertices.push_back(y + cy);
        vertices.push_back(newZ);
    }

    for (int i = 0; i < 36; ++i)
        indices.push_back(faceInds[i]);
}

// ============================================================================
// Cornell Box 3×3×3 Geometry
// ============================================================================

// L=-1.5, R=1.5, B=0, T=3, N=-1.5, F=1.5
// All normals point inward (into box interior)

// Floor (y=B=0): normal +Y
static const float kFloorVertices[] = {
    -1.5f, 0.0f, 1.5f,  -1.5f, 0.0f, -1.5f,  1.5f, 0.0f, -1.5f,  1.5f, 0.0f, 1.5f
};
static const uint32_t kFloorIndices[] = { 0, 1, 2,  0, 2, 3 };

// Ceiling (y=T=3): normal -Y
static const float kCeilingVertices[] = {
    -1.5f, 3.0f, -1.5f,  -1.5f, 3.0f, 1.5f,  1.5f, 3.0f, 1.5f,  1.5f, 3.0f, -1.5f
};
static const uint32_t kCeilingIndices[] = { 0, 1, 2,  0, 2, 3 };

// Back wall (z=N=-1.5): normal +Z
static const float kBackWallVertices[] = {
    -1.5f, 0.0f, -1.5f,  1.5f, 0.0f, -1.5f,  1.5f, 3.0f, -1.5f,  -1.5f, 3.0f, -1.5f
};
static const uint32_t kBackWallIndices[] = { 0, 1, 2,  0, 2, 3 };

// Left wall (x=L=-1.5): normal +X
static const float kLeftWallVertices[] = {
    -1.5f, 0.0f, 1.5f,  -1.5f, 0.0f, -1.5f,  -1.5f, 3.0f, -1.5f,  -1.5f, 3.0f, 1.5f
};
static const uint32_t kLeftWallIndices[] = { 0, 1, 2,  0, 2, 3 };

// Right wall (x=R=1.5): normal -X
static const float kRightWallVertices[] = {
    1.5f, 0.0f, -1.5f,  1.5f, 0.0f, 1.5f,  1.5f, 3.0f, 1.5f,  1.5f, 3.0f, -1.5f
};
static const uint32_t kRightWallIndices[] = { 0, 1, 2,  0, 2, 3 };

// Area light: y=2.9, size 1.0×1.0 (from -0.5 to 0.5 in x and z)
static const float kLightVertices[] = {
    -0.5f, 2.9f, -0.5f,  0.5f, 2.9f, -0.5f,  0.5f, 2.9f, 0.5f,  -0.5f, 2.9f, 0.5f
};
static const uint32_t kLightIndices[] = { 0, 1, 2,  0, 2, 3 };

// ============================================================================
// Main
// ============================================================================

int main(int argc, char** argv) {
    printf("=== VLR Improved Cornell Box Test START ===\n");
    fflush(stdout);

    // Defaults (priority: INI file > command line > these values)
    uint32_t width = 512;
    uint32_t height = 512;
    uint32_t numSamples = 1024;
    uint32_t maxDepth = 8;
    float exposure = 1.0f;
    std::string outputFile = "cornell_box_improved.png";
    std::string outputFormat = "png";

    float camPosX = 0.0f, camPosY = 1.5f, camPosZ = 6.0f;
    float camTargetX = 0.0f, camTargetY = 1.5f, camTargetZ = 0.0f;
    float camFOV = 40.0f, lensRadius = 0.0f, focusDistance = 1.0f;

    bool useIniScene = false;
    const char* perfConfigFile = nullptr;

    // 1. Load scene.ini if provided (argc >= 2, argv[1] not a flag)
    if (argc >= 2 && argv[1][0] != '-') {
        INIParser parser;
        if (parser.load(argv[1])) {
            useIniScene = true;
            width = (uint32_t)parser.getInt("Render", "Width", 512);
            height = (uint32_t)parser.getInt("Render", "Height", 512);
            numSamples = (uint32_t)parser.getInt("Render", "Samples", 1024);
            maxDepth = (uint32_t)parser.getInt("Render", "MaxDepth", 8);
            exposure = parser.getFloat("Render", "Exposure", 1.0f);
            outputFile = parser.getString("Output", "Filename", "cornell_box_improved.png");
            outputFormat = parser.getString("Output", "Format", "png");
            camPosX = parser.getFloat("Camera", "PositionX", 0.0f);
            camPosY = parser.getFloat("Camera", "PositionY", 1.5f);
            camPosZ = parser.getFloat("Camera", "PositionZ", 6.0f);
            camTargetX = parser.getFloat("Camera", "TargetX", 0.0f);
            camTargetY = parser.getFloat("Camera", "TargetY", 1.5f);
            camTargetZ = parser.getFloat("Camera", "TargetZ", 0.0f);
            camFOV = parser.getFloat("Camera", "FOV", 40.0f);
            lensRadius = parser.getFloat("Camera", "LensRadius", 0.0f);
            focusDistance = parser.getFloat("Camera", "FocusDistance", 1.0f);
        } else {
            fprintf(stderr, "[Warning] Failed to load scene config: %s, using defaults\n", argv[1]);
        }
        if (argc >= 3 && argv[2][0] != '-') {
            perfConfigFile = argv[2];
        }
    }

    // 2. Command line: legacy mode (when no INI) or --help
    if (!useIniScene) {
        for (int i = 1; i < argc; ++i) {
            if (strcmp(argv[i], "-w") == 0 && i + 1 < argc) {
                width = (uint32_t)atoi(argv[++i]);
            } else if (strcmp(argv[i], "-h") == 0 && i + 1 < argc) {
                height = (uint32_t)atoi(argv[++i]);
            } else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
                numSamples = (uint32_t)atoi(argv[++i]);
            } else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
                outputFile = argv[++i];
            }
        }
    }
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-?") == 0 || strcmp(argv[i], "--help") == 0) {
            printUsage(argv[0]);
            return 0;
        }
    }

    printf("Resolution: %u x %u, Samples: %u, MaxDepth: %u, Exposure: %.2f\n", width, height, numSamples, maxDepth, exposure);
    printf("Output: %s (%s)\n", outputFile.c_str(), outputFormat.c_str());
    if (useIniScene) printf("Scene config: %s\n", argv[1]);
    if (perfConfigFile) printf("Performance config: %s (delegated to libVLR)\n", perfConfigFile);
    fflush(stdout);

    VLRContext context = nullptr;
    VLRScene scene = nullptr;
    VLRResult res = VLRResult_Success;
    VLRCameraParams camera = {};
    std::vector<float> sphereVerts, boxVerts;
    std::vector<uint32_t> sphereInds, boxInds;

    res = vlrCreateContext(nullptr, 0, &context);
    if (res != VLRResult_Success || !context) {
        fprintf(stderr, "[Error] Failed to create Context: %d\n", res);
        return 1;
    }

    // Load performance config (delegated to libVLR)
    if (perfConfigFile) {
        res = vlrLoadPerformanceConfig(context, perfConfigFile);
        if (res != VLRResult_Success) {
            fprintf(stderr, "[Warning] Failed to load performance config: %s, using defaults\n", perfConfigFile);
        }
    }

    res = vlrCreateScene(context, &scene);
    if (res != VLRResult_Success || !scene) {
        fprintf(stderr, "[Error] Failed to create Scene: %d\n", res);
        vlrDestroyContext(context);
        return 1;
    }

    // ========================================================================
    // Materials (sRGB to linear)
    // ========================================================================
    // white: sRGB 0.75 -> linear ~0.522
    // red:   sRGB red -> linear (0.522, 0.0508, 0.0508)
    // blue:  sRGB blue -> linear (0.0508, 0.0508, 0.522)
    // black: sRGB 0.25 -> linear ~0.0508

    float whiteColor[] = { 0.522f, 0.522f, 0.522f };
    float redColor[] = { 0.522f, 0.0508f, 0.0508f };
    float blueColor[] = { 0.0508f, 0.0508f, 0.522f };
    float blackColor[] = { 0.0508f, 0.0508f, 0.0508f };

    VLRMaterial matWhite = nullptr;
    res = vlrCreateMaterial(scene, 0 /* Matte */, whiteColor, nullptr, &matWhite);
    if (res != VLRResult_Success) { fprintf(stderr, "[Error] White material\n"); goto cleanup; }

    VLRMaterial matRed = nullptr;
    res = vlrCreateMaterial(scene, 0 /* Matte */, redColor, nullptr, &matRed);
    if (res != VLRResult_Success) { fprintf(stderr, "[Error] Red material\n"); goto cleanup; }

    VLRMaterial matBlue = nullptr;
    res = vlrCreateMaterial(scene, 0 /* Matte */, blueColor, nullptr, &matBlue);
    if (res != VLRResult_Success) { fprintf(stderr, "[Error] Blue material\n"); goto cleanup; }

    VLRMaterial matFloor = nullptr;
    res = vlrCreateMaterialCheckerboard(scene, blackColor, whiteColor, 20, 1.5f, &matFloor);
    if (res != VLRResult_Success) { fprintf(stderr, "[Error] Floor material\n"); goto cleanup; }

    VLRMaterial matLight = nullptr;
    float lightEmission[] = { 30.0f, 30.0f, 30.0f };  // 完全对标 VLR
    res = vlrCreateMaterial(scene, 0 /* Matte */, whiteColor, lightEmission, &matLight);
    if (res != VLRResult_Success) { fprintf(stderr, "[Error] Light material\n"); goto cleanup; }

    // Glass sphere: SpecularTransmission, IOR 1.5 (standard glass)
    VLRMaterial matGlass = nullptr;
    float glassColor[] = { 0.999f, 0.999f, 0.999f };  // Minimal absorption
    res = vlrCreateMaterialEx(scene, 4 /* SpecularTransmission */, glassColor, 0.0f, 0.0f, 1.5f, nullptr, &matGlass);
    if (res != VLRResult_Success) { fprintf(stderr, "[Error] Glass material\n"); goto cleanup; }

    // Gold metal box: MicrofacetReflection, roughness 0.15
    VLRMaterial matGold = nullptr;
    float etaGold[] = { 0.143f, 0.374f, 1.442f };
    float kappaGold[] = { 3.984f, 2.386f, 1.603f };
    res = vlrCreateMaterialConductor(scene, etaGold, kappaGold, 0.15f, &matGold);
    if (res != VLRResult_Success) { fprintf(stderr, "[Error] Gold material\n"); goto cleanup; }

    // ========================================================================
    // Geometry
    // ========================================================================

    VLRTriangleMesh meshFloor = nullptr;
    res = vlrCreateTriangleMesh(scene, kFloorVertices, 4, kFloorIndices, 2, matFloor, &meshFloor);
    if (res != VLRResult_Success) { fprintf(stderr, "[Error] Floor mesh\n"); goto cleanup; }

    VLRTriangleMesh meshCeiling = nullptr;
    res = vlrCreateTriangleMesh(scene, kCeilingVertices, 4, kCeilingIndices, 2, matWhite, &meshCeiling);
    if (res != VLRResult_Success) { fprintf(stderr, "[Error] Ceiling mesh\n"); goto cleanup; }

    VLRTriangleMesh meshBackWall = nullptr;
    res = vlrCreateTriangleMesh(scene, kBackWallVertices, 4, kBackWallIndices, 2, matWhite, &meshBackWall);
    if (res != VLRResult_Success) { fprintf(stderr, "[Error] Back wall mesh\n"); goto cleanup; }

    VLRTriangleMesh meshLeftWall = nullptr;
    res = vlrCreateTriangleMesh(scene, kLeftWallVertices, 4, kLeftWallIndices, 2, matRed, &meshLeftWall);
    if (res != VLRResult_Success) { fprintf(stderr, "[Error] Left wall mesh\n"); goto cleanup; }

    VLRTriangleMesh meshRightWall = nullptr;
    res = vlrCreateTriangleMesh(scene, kRightWallVertices, 4, kRightWallIndices, 2, matBlue, &meshRightWall);
    if (res != VLRResult_Success) { fprintf(stderr, "[Error] Right wall mesh\n"); goto cleanup; }

    VLRTriangleMesh meshLight = nullptr;
    res = vlrCreateTriangleMesh(scene, kLightVertices, 4, kLightIndices, 2, matLight, &meshLight);
    if (res != VLRResult_Success) { fprintf(stderr, "[Error] Light mesh\n"); goto cleanup; }

    createSphere(sphereVerts, sphereInds, 0.6f, 0.5f, 0.0f, 0.5f, 64, 48);  // glass sphere on right
    createRotatedBox(boxVerts, boxInds, -0.6f, 0.5f, 0.0f, 1.0f, 20.0f * PI / 180.0f);  // metal box on left

    VLRTriangleMesh meshSphere = nullptr;
    res = vlrCreateTriangleMesh(scene, sphereVerts.data(), (uint32_t)(sphereVerts.size() / 3),
                               sphereInds.data(), (uint32_t)(sphereInds.size() / 3), matGlass, &meshSphere);
    if (res != VLRResult_Success) { fprintf(stderr, "[Error] Sphere mesh\n"); goto cleanup; }

    VLRTriangleMesh meshBox = nullptr;
    res = vlrCreateTriangleMesh(scene, boxVerts.data(), (uint32_t)(boxVerts.size() / 3),
                               boxInds.data(), (uint32_t)(boxInds.size() / 3), matGold, &meshBox);
    if (res != VLRResult_Success) { fprintf(stderr, "[Error] Box mesh\n"); goto cleanup; }

    // ========================================================================
    // Instances
    // ========================================================================

    float origin[] = { 0, 0, 0 };
    float scale[] = { 1, 1, 1 };
    float axis[] = { 0, 1, 0 };

    VLRInstance instFloor = nullptr;
    res = vlrCreateInstance(scene, meshFloor, origin, scale, axis, 0.0f, &instFloor);
    if (res != VLRResult_Success) { fprintf(stderr, "[Error] Floor instance\n"); goto cleanup; }

    VLRInstance instCeiling = nullptr;
    res = vlrCreateInstance(scene, meshCeiling, origin, scale, axis, 0.0f, &instCeiling);
    if (res != VLRResult_Success) { fprintf(stderr, "[Error] Ceiling instance\n"); goto cleanup; }

    VLRInstance instBackWall = nullptr;
    res = vlrCreateInstance(scene, meshBackWall, origin, scale, axis, 0.0f, &instBackWall);
    if (res != VLRResult_Success) { fprintf(stderr, "[Error] Back wall instance\n"); goto cleanup; }

    VLRInstance instLeftWall = nullptr;
    res = vlrCreateInstance(scene, meshLeftWall, origin, scale, axis, 0.0f, &instLeftWall);
    if (res != VLRResult_Success) { fprintf(stderr, "[Error] Left wall instance\n"); goto cleanup; }

    VLRInstance instRightWall = nullptr;
    res = vlrCreateInstance(scene, meshRightWall, origin, scale, axis, 0.0f, &instRightWall);
    if (res != VLRResult_Success) { fprintf(stderr, "[Error] Right wall instance\n"); goto cleanup; }

    VLRInstance instLight = nullptr;
    res = vlrCreateInstance(scene, meshLight, origin, scale, axis, 0.0f, &instLight);
    if (res != VLRResult_Success) { fprintf(stderr, "[Error] Light instance\n"); goto cleanup; }

    VLRInstance instSphere = nullptr;
    res = vlrCreateInstance(scene, meshSphere, origin, scale, axis, 0.0f, &instSphere);
    if (res != VLRResult_Success) { fprintf(stderr, "[Error] Sphere instance\n"); goto cleanup; }

    VLRInstance instBox = nullptr;
    res = vlrCreateInstance(scene, meshBox, origin, scale, axis, 0.0f, &instBox);
    if (res != VLRResult_Success) { fprintf(stderr, "[Error] Box instance\n"); goto cleanup; }

    res = vlrAddAreaLight(scene, instLight);
    if (res != VLRResult_Success) { fprintf(stderr, "[Error] Add area light\n"); goto cleanup; }

    // 注释掉方向光 - Cornell Box 只需要顶部面光源
    // float dirLightDir[] = { -0.3f, -0.8f, -0.2f };
    // float dirLightRadiance[] = { 2.0f, 2.0f, 2.0f };
    // res = vlrAddDirectionalLight(scene, dirLightDir, dirLightRadiance);
    // if (res != VLRResult_Success) { fprintf(stderr, "[Error] Add directional light\n"); goto cleanup; }

    // ========================================================================
    // Camera: from config or defaults
    // ========================================================================
    float dx = camTargetX - camPosX;
    float dy = camTargetY - camPosY;
    float dz = camTargetZ - camPosZ;
    float len = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (len < 1e-6f) len = 1.0f;
    camera.position[0] = camPosX;
    camera.position[1] = camPosY;
    camera.position[2] = camPosZ;
    camera.direction[0] = dx / len;
    camera.direction[1] = dy / len;
    camera.direction[2] = dz / len;
    camera.up[0] = 0.0f;
    camera.up[1] = 1.0f;
    camera.up[2] = 0.0f;
    camera.fovY = camFOV * PI / 180.0f;
    camera.aspect = (float)width / (float)height;
    camera.lensRadius = lensRadius;
    camera.focusDistance = focusDistance;
    camera.focalLength = 0.0f;
    camera.cameraType = 0;

    res = vlrSetCamera(scene, &camera);
    if (res != VLRResult_Success) { fprintf(stderr, "[Error] Set camera\n"); goto cleanup; }

    // ========================================================================
    // Environment Light (optional, for ambient lighting)
    // ========================================================================
    // Environment Light (IBL) - WhiteOne.exr
    res = vlrSetEnvironmentLightFromImage(scene, "resources/environments/WhiteOne.exr", 0.0f);
    if (res != VLRResult_Success) {
        fprintf(stderr, "[Warning] Failed to load environment map, using constant color fallback\n");
        float envColor[] = { 0.05f, 0.05f, 0.05f };
        res = vlrSetEnvironmentLight(scene, envColor);
    } else {
        printf("[Info] Environment light loaded from EXR\n");
    }

    // ========================================================================
    // Render
    // ========================================================================

    size_t bufferSize = width * height * sizeof(float) * 3;
    float* outputBuffer = (float*)malloc(bufferSize);
    if (!outputBuffer) {
        fprintf(stderr, "[Error] Allocate output buffer\n");
        goto cleanup;
    }

    res = vlrRender(context, scene, width, height, numSamples, vlr::VLRRenderer_WavefrontPathTracing);
    if (res != VLRResult_Success) {
        fprintf(stderr, "[Error] Render failed: %d\n", res);
        free(outputBuffer);
        goto cleanup;
    }

    void* deviceBuffer = vlrGetOutputBuffer(context);
    if (!deviceBuffer) {
        fprintf(stderr, "[Error] Get output buffer\n");
        free(outputBuffer);
        goto cleanup;
    }

    cudaError_t cudaErr = cudaMemcpy(outputBuffer, deviceBuffer, bufferSize, cudaMemcpyDeviceToHost);
    if (cudaErr != cudaSuccess) {
        fprintf(stderr, "[Error] cudaMemcpy: %s\n", cudaGetErrorString(cudaErr));
        free(outputBuffer);
        goto cleanup;
    }

    savePNG(outputFile.c_str(), width, height, outputBuffer, numSamples, exposure);
    free(outputBuffer);

    printf("=== Test complete ===\n");

cleanup:
    vlrDestroyScene(scene);
    vlrDestroyContext(context);
    return (res == VLRResult_Success) ? 0 : 1;
}
