// ============================================================================
// VLR Wavefront - Improved Cornell Box Test Scene
//
// Reference: F:\project\OfflineRenderer\libWR\test\cornell_box_var_test.cpp
//
// Scene specifications:
// - 3×3×3 box (L=-1.5, R=1.5; B=0, T=3; N=-1.5, F=1.5)
// - sRGB to linear color conversion for walls
// - Stronger light (80, 80, 80), 1.0×1.0 at y=2.9
// - Glass sphere: center (0.0, 0.8, 0.5), radius 0.5, IOR 1.5 (glass) [center]
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
#include <fstream>
#include <sstream>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

// ----------------------------------------------------------------------------
// Tuning knobs (keep defaults here, override via INI/CLI where available)
// ----------------------------------------------------------------------------
#define VLR_CB_DEFAULT_WIDTH 512u
#define VLR_CB_DEFAULT_HEIGHT 512u
#define VLR_CB_DEFAULT_SPP 256u
#define VLR_CB_DEFAULT_MAX_DEPTH 16u
#define VLR_CB_DEFAULT_EXPOSURE 1.0f

#define VLR_CB_DEFAULT_ENV_ENABLED 1
#define VLR_CB_DEFAULT_ENV_INTENSITY 0.3f

// Area light emission (RGB)
#define VLR_CB_DEFAULT_LIGHT_EMISSION 15.0f  // Normal level for Cornell Box

// Glass model:
// 1 = SpecularTransmission (stable, clear glass)
// 0 = MicrofacetScattering (harder to sample, more sensitive)
#define VLR_CB_GLASS_USE_SPECULAR_TRANSMISSION 1
#define VLR_CB_GLASS_IOR 1.5f
#define VLR_CB_GLASS_ROUGHNESS 0.001f

#define VLR_CB_TONEMAP_ACES 1

// ============================================================================
// Helper Macros & Functions
// ============================================================================

// Error checking macro for VLR API calls
#define VLR_CHECK(call, msg) \
    do { \
        res = (call); \
        if (res != VLRResult_Success) { \
            fprintf(stderr, "[Error] %s: %d\n", (msg), res); \
            goto cleanup; \
        } \
    } while(0)

// Warning macro for non-critical failures
#define VLR_WARN(call, msg) \
    do { \
        res = (call); \
        if (res != VLRResult_Success) { \
            fprintf(stderr, "[Warning] %s: %d\n", (msg), res); \
        } \
    } while(0)

// Simplified color definition (sRGB to linear conversion)
#define DEFINE_COLOR3(name, r, g, b) float name[] = { (r), (g), (b) }

// Common transform values
static const float kIdentityOrigin[] = { 0.0f, 0.0f, 0.0f };
static const float kIdentityScale[] = { 1.0f, 1.0f, 1.0f };
static const float kIdentityAxis[] = { 0.0f, 1.0f, 0.0f };

// Material creation helpers
static inline VLRResult createMatteMaterial(
    VLRScene scene, const float* color, const float* emission, 
    VLRMaterial* outMat, const char* name) {
    VLRResult res = vlrCreateMaterial(scene, 0 /* Matte */, color, emission, outMat);
    if (res != VLRResult_Success) {
        fprintf(stderr, "[Error] %s material creation failed\n", name);
    }
    return res;
}

// Mesh creation helper
static inline VLRResult createQuadMesh(
    VLRScene scene, const float* vertices, const uint32_t* indices,
    VLRMaterial material, VLRTriangleMesh* outMesh, const char* name) {
    VLRResult res = vlrCreateTriangleMesh(scene, vertices, 4, indices, 2, material, outMesh);
    if (res != VLRResult_Success) {
        fprintf(stderr, "[Error] %s mesh creation failed\n", name);
    }
    return res;
}

// Instance creation helper
static inline VLRResult createSimpleInstance(
    VLRScene scene, VLRTriangleMesh mesh, 
    const float* origin, const float* scale, const float* axis, float angle,
    VLRInstance* outInstance, const char* name) {
    VLRResult res = vlrCreateInstance(scene, mesh, origin, scale, axis, angle, outInstance);
    if (res != VLRResult_Success) {
        fprintf(stderr, "[Error] %s instance creation failed\n", name);
    }
    return res;
}

// ============================================================================
// Usage
// ============================================================================
static void printUsage(const char* prog) {
    printf("\nUsage:\n");
    printf("  %s [scene.ini] [performance.ini]\n", prog);
    printf("  %s -w <width> -h <height> -s <samples> [--max-depth <d>] [--exposure <ev>] -o <output> [--env 0|1] [--env-intensity <v>]\n", prog);
    printf("\nModes:\n");
    printf("  1. INI mode:  %s scene.ini [performance.ini]\n", prog);
    printf("     - scene.ini: [Render] Width,Height,Samples,MaxDepth,Exposure,EnableEnvironment,EnvironmentIntensity\n");
    printf("                   [Output] Filename, Format\n");
    printf("                   [Camera] PositionX/Y/Z, TargetX/Y/Z, FOV, LensRadius, FocusDistance\n");
    printf("     - performance.ini: delegated to libVLR (Optimization, KernelConfig, etc.)\n");
    printf("  2. Legacy mode: -w -h -s --max-depth --exposure -o --env --env-intensity\n");
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
    auto saturate = [](float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); };
    auto toneMapACES = [&](float x) {
        const float a = 2.51f;
        const float b = 0.03f;
        const float c = 2.43f;
        const float d = 0.59f;
        const float e = 0.14f;
        return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
    };
    auto toneMapReinhard = [](float x) { return x / (1.0f + x); };

    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            uint32_t srcIdx = (y * width + x) * 3;
            uint32_t dstIdx = (y * width + x) * 3;

            float r = fmaxf(0.0f, rgb[srcIdx + 0] * invSamples * exposure);
            float g = fmaxf(0.0f, rgb[srcIdx + 1] * invSamples * exposure);
            float b = fmaxf(0.0f, rgb[srcIdx + 2] * invSamples * exposure);
#if VLR_CB_TONEMAP_ACES == 2
            r = toneMapReinhard(r);
            g = toneMapReinhard(g);
            b = toneMapReinhard(b);
#elif VLR_CB_TONEMAP_ACES == 1
            r = toneMapACES(r);
            g = toneMapACES(g);
            b = toneMapACES(b);
#else
            r = saturate(r);
            g = saturate(g);
            b = saturate(b);
#endif

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

/// Load OBJ file (simple version, only vertices and faces)
static bool loadOBJ(const char* filename, std::vector<float>& vertices, std::vector<uint32_t>& indices,
                   float scale = 1.0f, float tx = 0.0f, float ty = 0.0f, float tz = 0.0f) {
    std::ifstream file(filename);
    if (!file.is_open()) {
        fprintf(stderr, "[Error] Cannot open OBJ file: %s\n", filename);
        return false;
    }
    
    vertices.clear();
    indices.clear();
    
    std::vector<float> tempVerts;
    std::string line;
    
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;
        
        std::istringstream iss(line);
        std::string prefix;
        iss >> prefix;
        
        if (prefix == "v") {
            float x, y, z;
            iss >> x >> y >> z;
            tempVerts.push_back(x * scale + tx);
            tempVerts.push_back(y * scale + ty);
            tempVerts.push_back(z * scale + tz);
        }
        else if (prefix == "f") {
            std::string v1, v2, v3;
            iss >> v1 >> v2 >> v3;
            
            // Parse vertex indices (format: v or v/vt or v/vt/vn)
            auto parseIndex = [](const std::string& s) -> uint32_t {
                size_t slash = s.find('/');
                std::string indexStr = (slash != std::string::npos) ? s.substr(0, slash) : s;
                return (uint32_t)(std::stoi(indexStr) - 1);  // OBJ indices are 1-based
            };
            
            indices.push_back(parseIndex(v1));
            indices.push_back(parseIndex(v2));
            indices.push_back(parseIndex(v3));
        }
    }
    
    vertices = tempVerts;
    file.close();
    
    printf("[Info] Loaded OBJ: %s (%zu vertices, %zu triangles)\n", 
           filename, vertices.size() / 3, indices.size() / 3);
    return true;
}

/// UV sphere: center (cx,cy,cz), radius, segments (longitude), rings (latitude)
/// Also outputs per-vertex normals for smooth shading.
static void createSphere(std::vector<float>& vertices, std::vector<uint32_t>& indices,
                        float cx, float cy, float cz, float radius,
                        int segments = 64, int rings = 48,
                        std::vector<float>* outNormals = nullptr) {
    vertices.clear();
    indices.clear();
    if (outNormals) outNormals->clear();

    for (int lat = 0; lat <= rings; ++lat) {
        float phi = PI * float(lat) / float(rings);
        float sinPhi = std::sin(phi);
        float cosPhi = std::cos(phi);

        for (int lon = 0; lon <= segments; ++lon) {
            float theta = TWO_PI * float(lon) / float(segments);
            float sinTheta = std::sin(theta);
            float cosTheta = std::cos(theta);

            float nx = sinPhi * cosTheta;
            float ny = cosPhi;
            float nz = sinPhi * sinTheta;

            vertices.push_back(cx + radius * nx);
            vertices.push_back(cy + radius * ny);
            vertices.push_back(cz + radius * nz);

            if (outNormals) {
                outNormals->push_back(nx);
                outNormals->push_back(ny);
                outNormals->push_back(nz);
            }
        }
    }

    for (int lat = 0; lat < rings; ++lat) {
        for (int lon = 0; lon < segments; ++lon) {
            uint32_t first = lat * (segments + 1) + lon;
            uint32_t second = first + segments + 1;

            // 反转卷绕顺序，使法线指向外部（对玻璃球至关重要）
            if (lat != 0) {
                indices.push_back(first);
                indices.push_back(first + 1);     // 反转
                indices.push_back(second);
            }
            if (lat != rings - 1) {
                indices.push_back(first + 1);
                indices.push_back(second + 1);    // 反转
                indices.push_back(second);
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
        0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7,
        8, 9, 10, 8, 10, 11, 12, 13, 14, 12, 14, 15,
        16, 17, 18, 16, 18, 19, 20, 21, 22, 20, 22, 23
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

// Front wall (z=F=1.5): normal -Z (facing camera)
static const float kFrontWallVertices[] = {
    1.5f, 0.0f, 1.5f,  -1.5f, 0.0f, 1.5f,  -1.5f, 3.0f, 1.5f,  1.5f, 3.0f, 1.5f
};
static const uint32_t kFrontWallIndices[] = { 0, 1, 2,  0, 2, 3 };

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
    uint32_t width = VLR_CB_DEFAULT_WIDTH;
    uint32_t height = VLR_CB_DEFAULT_HEIGHT;
    uint32_t numSamples = VLR_CB_DEFAULT_SPP;
    uint32_t maxDepth = VLR_CB_DEFAULT_MAX_DEPTH;
    float exposure = VLR_CB_DEFAULT_EXPOSURE;
    std::string outputFile = "cornell_box_improved.png";
    std::string outputFormat = "png";

    float camPosX = 0.0f, camPosY = 1.5f, camPosZ = 6.0f;
    float camTargetX = 0.0f, camTargetY = 1.5f, camTargetZ = 0.0f;
    float camFOV = 40.0f, lensRadius = 0.0f, focusDistance = 1.0f;
    
    // Denoiser config (disabled by default)
    bool denoiserEnabled = false;
    bool denoiserUseAlbedo = true;
    bool denoiserUseNormal = true;
    float denoiserHDRIntensity = 1.0f;
    bool enableEnvironmentLight = (VLR_CB_DEFAULT_ENV_ENABLED != 0);
    float environmentIntensity = VLR_CB_DEFAULT_ENV_INTENSITY;

    bool useIniScene = false;
    const char* perfConfigFile = nullptr;

    // 1. Load scene.ini if provided (argc >= 2, argv[1] not a flag)
    if (argc >= 2 && argv[1][0] != '-') {
        INIParser parser;
        if (parser.load(argv[1])) {
            useIniScene = true;
            width = (uint32_t)parser.getInt("Render", "Width", (int)VLR_CB_DEFAULT_WIDTH);
            height = (uint32_t)parser.getInt("Render", "Height", (int)VLR_CB_DEFAULT_HEIGHT);
            numSamples = (uint32_t)parser.getInt("Render", "Samples", (int)VLR_CB_DEFAULT_SPP);
            maxDepth = (uint32_t)parser.getInt("Render", "MaxDepth", (int)VLR_CB_DEFAULT_MAX_DEPTH);
            exposure = parser.getFloat("Render", "Exposure", VLR_CB_DEFAULT_EXPOSURE);
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
            enableEnvironmentLight = parser.getInt("Render", "EnableEnvironment", VLR_CB_DEFAULT_ENV_ENABLED) != 0;
            environmentIntensity = parser.getFloat("Render", "EnvironmentIntensity", VLR_CB_DEFAULT_ENV_INTENSITY);
            
            // Read denoiser config
            denoiserEnabled = parser.getInt("Denoiser", "Enabled", 0) != 0;
            denoiserUseAlbedo = parser.getInt("Denoiser", "UseAlbedo", 1) != 0;
            denoiserUseNormal = parser.getInt("Denoiser", "UseNormal", 1) != 0;
            denoiserHDRIntensity = parser.getFloat("Denoiser", "HDRIntensity", 1.0f);
        } else {
            fprintf(stderr, "[Warning] Failed to load scene config: %s, using defaults\n", argv[1]);
        }
        if (argc >= 3 && argv[2][0] != '-') {
            perfConfigFile = argv[2];
        }
    }

    // 2. Command line: flags override defaults/INI values.
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-w") == 0 && i + 1 < argc) {
            width = (uint32_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "-h") == 0 && i + 1 < argc) {
            height = (uint32_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            numSamples = (uint32_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--max-depth") == 0 && i + 1 < argc) {
            maxDepth = (uint32_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--exposure") == 0 && i + 1 < argc) {
            exposure = (float)atof(argv[++i]);
        } else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            outputFile = argv[++i];
        } else if (strcmp(argv[i], "--env") == 0 && i + 1 < argc) {
            enableEnvironmentLight = atoi(argv[++i]) != 0;
        } else if (strcmp(argv[i], "--env-intensity") == 0 && i + 1 < argc) {
            environmentIntensity = (float)atof(argv[++i]);
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
    if (denoiserEnabled) {
        printf("Denoiser: Enabled (Albedo: %s, Normal: %s, HDR: %.2f)\n", 
               denoiserUseAlbedo ? "ON" : "OFF",
               denoiserUseNormal ? "ON" : "OFF",
               denoiserHDRIntensity);
    } else {
        printf("Denoiser: Disabled\n");
    }
    printf("Environment: %s (Intensity: %.3f)\n",
           enableEnvironmentLight ? "Enabled" : "Disabled",
           environmentIntensity);
    if (useIniScene) printf("Scene config: %s\n", argv[1]);
    if (perfConfigFile) printf("Performance config: %s (delegated to libVLR)\n", perfConfigFile);
    fflush(stdout);

    VLRContext context = nullptr;
    VLRScene scene = nullptr;
    VLRResult res = VLRResult_Success;
    VLRCameraParams camera = {};
    std::vector<float> sphereVerts, boxVerts, sphereNormals;
    std::vector<uint32_t> sphereInds, boxInds;
    
    // Materials (sRGB to linear: white=0.522, red/blue=0.522/0.0508, black=0.0508)
    DEFINE_COLOR3(whiteColor, 0.522f, 0.522f, 0.522f);
    DEFINE_COLOR3(redColor, 0.522f, 0.0508f, 0.0508f);
    DEFINE_COLOR3(blueColor, 0.0508f, 0.0508f, 0.522f);
    DEFINE_COLOR3(blackColor, 0.0508f, 0.0508f, 0.0508f);
    DEFINE_COLOR3(lightEmission, VLR_CB_DEFAULT_LIGHT_EMISSION, VLR_CB_DEFAULT_LIGHT_EMISSION, VLR_CB_DEFAULT_LIGHT_EMISSION);
    DEFINE_COLOR3(glassColor, 0.999f, 0.999f, 0.999f);
    DEFINE_COLOR3(etaGold, 0.143f, 0.374f, 1.442f);
    DEFINE_COLOR3(kappaGold, 3.984f, 2.386f, 1.603f);
    DEFINE_COLOR3(envColor, environmentIntensity, environmentIntensity, environmentIntensity);
    DEFINE_COLOR3(pointLightPos, 0.0f, 2.5f, 0.5f);
    DEFINE_COLOR3(pointLightIntensity, 10.0f, 10.0f, 10.0f);  // Normal point light intensity
    
    VLRMaterial matWhite = nullptr, matRed = nullptr, matBlue = nullptr;
    VLRMaterial matFloor = nullptr, matLight = nullptr;
    VLRMaterial matGlass = nullptr, matGold = nullptr;
    
    // Geometry
    VLRTriangleMesh meshFloor = nullptr, meshCeiling = nullptr, meshBackWall = nullptr;
    VLRTriangleMesh meshLeftWall = nullptr, meshRightWall = nullptr, meshFrontWall = nullptr;
    VLRTriangleMesh meshLight = nullptr, meshSphere = nullptr, meshBox = nullptr;
    
    // Instances
    VLRInstance instFloor = nullptr, instCeiling = nullptr, instBackWall = nullptr;
    VLRInstance instLeftWall = nullptr, instRightWall = nullptr;
    VLRInstance instLight = nullptr, instSphere = nullptr, instBox = nullptr;
    
    // Camera
    float dx, dy, dz, len;
    
    // Render
    size_t bufferSize = 0;
    float* outputBuffer = nullptr;
    void* deviceBuffer = nullptr;
    cudaError_t cudaErr = cudaSuccess;

    res = vlrCreateContext(nullptr, 0, &context);
    if (res != VLRResult_Success || !context) {
        fprintf(stderr, "[Error] Failed to create Context: %d\n", res);
        return 1;
    }

    VLR_WARN(vlrSetDenoiserConfig(context, denoiserEnabled, denoiserUseAlbedo, denoiserUseNormal, denoiserHDRIntensity),
             "Set denoiser config");

    if (perfConfigFile) {
        VLR_WARN(vlrLoadPerformanceConfig(context, perfConfigFile), "Load performance config");
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

    VLR_CHECK(createMatteMaterial(scene, whiteColor, nullptr, &matWhite, "White"), "White material");
    VLR_CHECK(createMatteMaterial(scene, redColor, nullptr, &matRed, "Red"), "Red material");
    VLR_CHECK(createMatteMaterial(scene, blueColor, nullptr, &matBlue, "Blue"), "Blue material");
    VLR_CHECK(vlrCreateMaterialCheckerboard(scene, blackColor, whiteColor, 20, 1.5f, &matFloor), "Floor material");
    VLR_CHECK(createMatteMaterial(scene, whiteColor, lightEmission, &matLight, "Light"), "Light material");

    // Glass sphere: use SpecularTransmission (BSDFType = 6)
#if VLR_CB_GLASS_USE_SPECULAR_TRANSMISSION
    printf("[Test] Creating glass material: BSDFType=6 (SpecularTransmission), IOR=%.2f, Color=(%.2f,%.2f,%.2f)\n",
           VLR_CB_GLASS_IOR, glassColor[0], glassColor[1], glassColor[2]);
    VLR_CHECK(vlrCreateMaterialEx(scene, 6 /* BSDFType_SpecularTransmission */, glassColor, 0.0f, 0.0f, 
                                   VLR_CB_GLASS_IOR, nullptr, &matGlass), "Glass material (SpecularTransmission)");
#else
    VLR_CHECK(vlrCreateMaterialMicrofacetScattering(scene, VLR_CB_GLASS_IOR, VLR_CB_GLASS_ROUGHNESS, &matGlass),
              "Glass material (MicrofacetScattering)");
#endif

    // Gold metal box: MicrofacetReflection, roughness 0.2
    VLR_CHECK(vlrCreateMaterialConductor(scene, etaGold, kappaGold, 0.2f, &matGold), "Gold material");

    // ========================================================================
    // Geometry
    // ========================================================================

    VLR_CHECK(createQuadMesh(scene, kFloorVertices, kFloorIndices, matFloor, &meshFloor, "Floor"), "Floor mesh");
    VLR_CHECK(createQuadMesh(scene, kCeilingVertices, kCeilingIndices, matWhite, &meshCeiling, "Ceiling"), "Ceiling mesh");
    VLR_CHECK(createQuadMesh(scene, kBackWallVertices, kBackWallIndices, matWhite, &meshBackWall, "BackWall"), "BackWall mesh");
    VLR_CHECK(createQuadMesh(scene, kLeftWallVertices, kLeftWallIndices, matBlue, &meshLeftWall, "LeftWall"), "LeftWall mesh");
    VLR_CHECK(createQuadMesh(scene, kRightWallVertices, kRightWallIndices, matRed, &meshRightWall, "RightWall"), "RightWall mesh");
    VLR_CHECK(createQuadMesh(scene, kFrontWallVertices, kFrontWallIndices, matWhite, &meshFrontWall, "FrontWall"), "FrontWall mesh");
    VLR_CHECK(createQuadMesh(scene, kLightVertices, kLightIndices, matLight, &meshLight, "Light"), "Light mesh");

    // Glass sphere on the left, gold box on the right (standard Cornell box layout)
    // Positions chosen to avoid geometric overlap after box rotation
    createSphere(sphereVerts, sphereInds, -0.6f, 0.5f, 0.0f, 0.5f, 64, 48, &sphereNormals);
    
    createRotatedBox(boxVerts, boxInds, 0.6f, 0.5f, 0.0f, 1.0f, 20.0f * PI / 180.0f);

    VLR_CHECK(vlrCreateTriangleMeshWithNormals(scene, sphereVerts.data(), (uint32_t)(sphereVerts.size() / 3),
                                    sphereNormals.data(), (uint32_t)(sphereNormals.size() / 3),
                                    sphereInds.data(), (uint32_t)(sphereInds.size() / 3), matGlass, &meshSphere),
              "Sphere mesh");
    VLR_CHECK(vlrCreateTriangleMesh(scene, boxVerts.data(), (uint32_t)(boxVerts.size() / 3),
                                    boxInds.data(), (uint32_t)(boxInds.size() / 3), matGold, &meshBox),
              "Box mesh");

    // ========================================================================
    // Instances
    // ========================================================================

    VLR_CHECK(createSimpleInstance(scene, meshFloor, kIdentityOrigin, kIdentityScale, kIdentityAxis, 0.0f, &instFloor, "Floor"), "Floor instance");
    VLR_CHECK(createSimpleInstance(scene, meshCeiling, kIdentityOrigin, kIdentityScale, kIdentityAxis, 0.0f, &instCeiling, "Ceiling"), "Ceiling instance");
    VLR_CHECK(createSimpleInstance(scene, meshBackWall, kIdentityOrigin, kIdentityScale, kIdentityAxis, 0.0f, &instBackWall, "BackWall"), "BackWall instance");
    VLR_CHECK(createSimpleInstance(scene, meshLeftWall, kIdentityOrigin, kIdentityScale, kIdentityAxis, 0.0f, &instLeftWall, "LeftWall"), "LeftWall instance");
    VLR_CHECK(createSimpleInstance(scene, meshRightWall, kIdentityOrigin, kIdentityScale, kIdentityAxis, 0.0f, &instRightWall, "RightWall"), "RightWall instance");
    VLR_CHECK(createSimpleInstance(scene, meshLight, kIdentityOrigin, kIdentityScale, kIdentityAxis, 0.0f, &instLight, "Light"), "Light instance");
    VLR_CHECK(createSimpleInstance(scene, meshSphere, kIdentityOrigin, kIdentityScale, kIdentityAxis, 0.0f, &instSphere, "Sphere"), "Sphere instance");
    VLR_CHECK(createSimpleInstance(scene, meshBox, kIdentityOrigin, kIdentityScale, kIdentityAxis, 0.0f, &instBox, "Box"), "Box instance");

    VLR_CHECK(vlrAddAreaLight(scene, instLight), "Add area light");

    if (enableEnvironmentLight) {
        VLR_CHECK(vlrSetEnvironmentLight(scene, envColor), "Set environment light");
    }

    // Add point light for better glass illumination
    VLR_CHECK(vlrAddPointLight(scene, pointLightPos, pointLightIntensity), "Add point light");

    // ========================================================================
    // Camera: from config or defaults
    // ========================================================================
    dx = camTargetX - camPosX;
    dy = camTargetY - camPosY;
    dz = camTargetZ - camPosZ;
    len = std::sqrt(dx * dx + dy * dy + dz * dz);
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

    VLR_CHECK(vlrSetCamera(scene, &camera), "Set camera");

    // ========================================================================
    // Environment Light (optional, for ambient lighting)
    // ========================================================================
    // 暂时禁用环境光，测试区域光效果
    // res = vlrSetEnvironmentLightFromImage(scene, "resources/environments/WhiteOne.exr", 0.0f);
    // if (res != VLRResult_Success) {
    //     fprintf(stderr, "[Warning] Failed to load environment map, using constant color fallback\n");
    //     float envColor[] = { 0.05f, 0.05f, 0.05f };
    //     res = vlrSetEnvironmentLight(scene, envColor);
    // } else {
    //     printf("[Info] Environment light loaded from EXR\n");
    // }

    // ========================================================================
    // Render
    // ========================================================================

    bufferSize = width * height * sizeof(float) * 3;
    outputBuffer = (float*)malloc(bufferSize);
    if (!outputBuffer) {
        fprintf(stderr, "[Error] Allocate output buffer\n");
        goto cleanup;
    }

    VLR_CHECK(vlrRender(context, scene, width, height, numSamples, vlr::VLRRenderer_WavefrontPathTracing), "Render");

    deviceBuffer = vlrGetOutputBuffer(context);
    if (!deviceBuffer) {
        fprintf(stderr, "[Error] Get output buffer\n");
        free(outputBuffer);
        goto cleanup;
    }

    cudaErr = cudaMemcpy(outputBuffer, deviceBuffer, bufferSize, cudaMemcpyDeviceToHost);
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
