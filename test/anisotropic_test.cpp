// ============================================================================
// VLR Anisotropic MicrofacetReflection Test
//
// Renders two metal spheres side by side:
// - Left:  Isotropic conductor (anisotropy=0) - circular highlight
// - Right: Anisotropic conductor (anisotropy=0.8) - stretched/brushed highlight
//
// Verifies vlrCreateMaterialConductorAniso and anisotropic GGX BSDF.
// ============================================================================

#include <vlr/vlr.h>
#include <cuda_runtime.h>
#include <cstdio>
#include <cstdlib>
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
            vertices.push_back(cx + radius * sinPhi * cosTheta);
            vertices.push_back(cy + radius * cosPhi);
            vertices.push_back(cz + radius * sinPhi * sinTheta);
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
        printf("[Done] Saved: %s (%u x %u)\n", filename, width, height);
    } else {
        fprintf(stderr, "[Error] Failed to save: %s\n", filename);
    }
}

int main(int argc, char** argv) {
    printf("=== VLR Anisotropic Conductor Test ===\n\n");

    // All variable declarations at function start (before any goto cleanup)
    VLRContext context = nullptr;
    VLRScene scene = nullptr;
    VLRResult res = VLRResult_Success;
    VLRMaterial matIso = nullptr;
    VLRMaterial matAniso = nullptr;
    VLRMaterial matFloor = nullptr;
    VLRMaterial matLight = nullptr;
    VLRTriangleMesh meshIso = nullptr, meshAniso = nullptr, meshFloor = nullptr, meshLight = nullptr;
    VLRInstance instIso = nullptr, instAniso = nullptr, instFloor = nullptr, instLight = nullptr;

    uint32_t width = 512;
    uint32_t height = 512;
    uint32_t numSamples = 256;
    float exposure = 1.2f;
    const char* outputFile = "anisotropic_test.png";

    float etaGold[] = {0.143f, 0.374f, 1.442f};
    float kappaGold[] = {3.984f, 2.386f, 1.603f};
    float whiteColor[] = {0.75f, 0.75f, 0.75f};
    float lightEmission[] = {80.0f, 80.0f, 80.0f};

    float kFloorVerts[] = {
        -2.0f, 0.0f, -2.0f,  -2.0f, 0.0f, 2.0f,  2.0f, 0.0f, 2.0f,  2.0f, 0.0f, -2.0f
    };
    uint32_t kFloorInds[] = { 0, 1, 2, 0, 2, 3 };
    float kLightVerts[] = {
        -0.4f, 1.9f, -0.4f,  -0.4f, 1.9f, 0.4f,  0.4f, 1.9f, 0.4f,  0.4f, 1.9f, -0.4f
    };
    uint32_t kLightInds[] = { 0, 1, 2, 0, 2, 3 };

    std::vector<float> sphereVerts;
    std::vector<uint32_t> sphereInds;

    float origin[] = {0.0f, 0.0f, 0.0f};
    float scale[] = {1.0f, 1.0f, 1.0f};
    float axis[] = {0.0f, 1.0f, 0.0f};

    VLRCameraParams camera = {};
    size_t bufferSize = 0;
    float* outputBuffer = nullptr;
    void* deviceBuffer = nullptr;
    cudaError_t cudaErr = cudaSuccess;

    if (argc >= 2) outputFile = argv[1];
    if (argc >= 3) numSamples = (uint32_t)atoi(argv[2]);

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

    // Materials
    res = vlrCreateMaterial(scene, 0, whiteColor, nullptr, &matFloor);
    if (res != VLRResult_Success) { fprintf(stderr, "[Error] Floor material\n"); goto cleanup; }

    res = vlrCreateMaterial(scene, 0, whiteColor, lightEmission, &matLight);
    if (res != VLRResult_Success) { fprintf(stderr, "[Error] Light material\n"); goto cleanup; }

    res = vlrCreateMaterialConductor(scene, etaGold, kappaGold, 0.2f, &matIso);
    if (res != VLRResult_Success) { fprintf(stderr, "[Error] Isotropic conductor\n"); goto cleanup; }
    printf("[OK] Isotropic conductor (anisotropy=0)\n");

    res = vlrCreateMaterialConductorAniso(scene, etaGold, kappaGold, 0.2f, 0.8f, &matAniso);
    if (res != VLRResult_Success) { fprintf(stderr, "[Error] Anisotropic conductor\n"); goto cleanup; }
    printf("[OK] Anisotropic conductor (anisotropy=0.8)\n");

    // Geometry: floor, light, two spheres
    createSphere(sphereVerts, sphereInds, -0.5f, 0.5f, 0.0f, 0.4f);
    res = vlrCreateTriangleMesh(scene, sphereVerts.data(), (uint32_t)(sphereVerts.size() / 3),
                               sphereInds.data(), (uint32_t)(sphereInds.size() / 3), matIso, &meshIso);
    if (res != VLRResult_Success) { fprintf(stderr, "[Error] Iso sphere mesh\n"); goto cleanup; }

    createSphere(sphereVerts, sphereInds, 0.5f, 0.5f, 0.0f, 0.4f);
    res = vlrCreateTriangleMesh(scene, sphereVerts.data(), (uint32_t)(sphereVerts.size() / 3),
                               sphereInds.data(), (uint32_t)(sphereInds.size() / 3), matAniso, &meshAniso);
    if (res != VLRResult_Success) { fprintf(stderr, "[Error] Aniso sphere mesh\n"); goto cleanup; }

    res = vlrCreateTriangleMesh(scene, kFloorVerts, 4, kFloorInds, 2, matFloor, &meshFloor);
    if (res != VLRResult_Success) { fprintf(stderr, "[Error] Floor mesh\n"); goto cleanup; }

    res = vlrCreateTriangleMesh(scene, kLightVerts, 4, kLightInds, 2, matLight, &meshLight);
    if (res != VLRResult_Success) { fprintf(stderr, "[Error] Light mesh\n"); goto cleanup; }

    res = vlrCreateInstance(scene, meshFloor, origin, scale, axis, 0.0f, &instFloor);
    if (res != VLRResult_Success) { fprintf(stderr, "[Error] Floor instance\n"); goto cleanup; }

    res = vlrCreateInstance(scene, meshLight, origin, scale, axis, 0.0f, &instLight);
    if (res != VLRResult_Success) { fprintf(stderr, "[Error] Light instance\n"); goto cleanup; }

    res = vlrCreateInstance(scene, meshIso, origin, scale, axis, 0.0f, &instIso);
    if (res != VLRResult_Success) { fprintf(stderr, "[Error] Iso sphere instance\n"); goto cleanup; }

    res = vlrCreateInstance(scene, meshAniso, origin, scale, axis, 0.0f, &instAniso);
    if (res != VLRResult_Success) { fprintf(stderr, "[Error] Aniso sphere instance\n"); goto cleanup; }

    res = vlrAddAreaLight(scene, instLight);
    if (res != VLRResult_Success) { fprintf(stderr, "[Error] Add area light\n"); goto cleanup; }

    camera.position[0] = 0.0f;
    camera.position[1] = 1.0f;
    camera.position[2] = 2.5f;
    camera.direction[0] = 0.0f;
    camera.direction[1] = -0.3f;
    camera.direction[2] = -0.95f;
    camera.up[0] = 0.0f;
    camera.up[1] = 1.0f;
    camera.up[2] = 0.0f;
    camera.fovY = 45.0f * PI / 180.0f;
    camera.aspect = (float)width / (float)height;
    camera.lensRadius = 0.0f;
    camera.focusDistance = 1.0f;
    camera.focalLength = 0.0f;
    camera.cameraType = 0;

    res = vlrSetCamera(scene, &camera);
    if (res != VLRResult_Success) { fprintf(stderr, "[Error] Set camera\n"); goto cleanup; }

    bufferSize = width * height * sizeof(float) * 3;
    outputBuffer = (float*)malloc(bufferSize);
    if (!outputBuffer) { fprintf(stderr, "[Error] Allocate buffer\n"); goto cleanup; }

    printf("[Render] %u x %u, %u spp...\n", width, height, numSamples);
    res = vlrRender(context, scene, width, height, numSamples, vlr::VLRRenderer_WavefrontPathTracing);
    if (res != VLRResult_Success) {
        fprintf(stderr, "[Error] Render failed: %d\n", res);
        free(outputBuffer);
        goto cleanup;
    }

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

    savePNG(outputFile, width, height, outputBuffer, numSamples, exposure);
    free(outputBuffer);

    printf("\n=== Anisotropic test complete ===\n");
    printf("Left sphere: isotropic (circular highlight)\n");
    printf("Right sphere: anisotropic (stretched/brushed highlight)\n");
    printf("Output: %s\n", outputFile);

cleanup:
    vlrDestroyScene(scene);
    vlrDestroyContext(context);
    return (res == VLRResult_Success) ? 0 : 1;
}
