// Simple glass sphere test with white background
#include <vlr/vlr.h>
#include <cstdio>
#include <cmath>
#include <vector>
#include <cstdlib>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

static const float PI = 3.14159265358979323846f;

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
            float theta = 2.0f * PI * float(lon) / float(segments);
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

            float r = fmaxf(0.0f, rgb[srcIdx + 0] * invSamples * exposure);
            float g = fmaxf(0.0f, rgb[srcIdx + 1] * invSamples * exposure);
            float b = fmaxf(0.0f, rgb[srcIdx + 2] * invSamples * exposure);

            r = powf(r < 1.0f ? r : 1.0f, 1.0f / 2.2f);
            g = powf(g < 1.0f ? g : 1.0f, 1.0f / 2.2f);
            b = powf(b < 1.0f ? b : 1.0f, 1.0f / 2.2f);

            pixels[dstIdx + 0] = (unsigned char)(r * 255.0f);
            pixels[dstIdx + 1] = (unsigned char)(g * 255.0f);
            pixels[dstIdx + 2] = (unsigned char)(b * 255.0f);
        }
    }

    stbi_write_png(filename, width, height, 3, pixels.data(), width * 3);
    printf("[Done] Image saved: %s (%u x %u)\n", filename, width, height);
}

int main(int argc, char** argv) {
    printf("=== Simple Glass Sphere Test ===\n");
    
    uint32_t width = 512;
    uint32_t height = 512;
    uint32_t numSamples = 256;
    float exposure = 2.0f;
    
    VLRContext context = nullptr;
    VLRScene scene = nullptr;
    VLRResult res = VLRResult_Success;
    
    res = vlrCreateContext(nullptr, 0, &context);
    if (res != VLRResult_Success) {
        fprintf(stderr, "Failed to create context\n");
        return 1;
    }
    
    res = vlrCreateScene(context, &scene);
    if (res != VLRResult_Success) {
        fprintf(stderr, "Failed to create scene\n");
        return 1;
    }
    
    // White background plane (large quad behind sphere)
    float bgColor[] = { 0.9f, 0.9f, 0.9f };
    float bgVerts[] = {
        -5.0f, -5.0f, -2.0f,
         5.0f, -5.0f, -2.0f,
         5.0f,  5.0f, -2.0f,
        -5.0f,  5.0f, -2.0f
    };
    uint32_t bgInds[] = { 0, 1, 2, 0, 2, 3 };
    
    VLRMaterial matBg = nullptr;
    res = vlrCreateMaterial(scene, 0, bgColor, nullptr, &matBg);
    if (res != VLRResult_Success) {
        fprintf(stderr, "Failed to create background material\n");
        goto cleanup;
    }
    
    VLRTriangleMesh meshBg = nullptr;
    res = vlrCreateTriangleMesh(scene, bgVerts, 4, bgInds, 2, matBg, &meshBg);
    if (res != VLRResult_Success) {
        fprintf(stderr, "Failed to create background mesh\n");
        goto cleanup;
    }
    
    float origin[] = { 0, 0, 0 };
    float scale[] = { 1, 1, 1 };
    float axis[] = { 0, 1, 0 };
    
    VLRInstance instBg = nullptr;
    res = vlrCreateInstance(scene, meshBg, origin, scale, axis, 0.0f, &instBg);
    if (res != VLRResult_Success) {
        fprintf(stderr, "Failed to create background instance\n");
        goto cleanup;
    }
    
    // Glass sphere
    std::vector<float> sphereVerts;
    std::vector<uint32_t> sphereInds;
    createSphere(sphereVerts, sphereInds, 0.0f, 0.0f, 0.0f, 0.5f, 64, 48);
    
    float glassColor[] = { 1.0f, 1.0f, 1.0f };
    VLRMaterial matGlass = nullptr;
    res = vlrCreateMaterialEx(scene, 6, glassColor, 0.0f, 0.0f, 1.5f, nullptr, &matGlass);
    if (res != VLRResult_Success) {
        fprintf(stderr, "Failed to create glass material\n");
        goto cleanup;
    }
    
    VLRTriangleMesh meshSphere = nullptr;
    res = vlrCreateTriangleMesh(scene, sphereVerts.data(), (uint32_t)(sphereVerts.size() / 3),
                               sphereInds.data(), (uint32_t)(sphereInds.size() / 3), 
                               matGlass, &meshSphere);
    if (res != VLRResult_Success) {
        fprintf(stderr, "Failed to create sphere mesh\n");
        goto cleanup;
    }
    
    VLRInstance instSphere = nullptr;
    res = vlrCreateInstance(scene, meshSphere, origin, scale, axis, 0.0f, &instSphere);
    if (res != VLRResult_Success) {
        fprintf(stderr, "Failed to create sphere instance\n");
        goto cleanup;
    }
    
    // Strong environment light
    float envColor[] = { 1.0f, 1.0f, 1.0f };
    res = vlrSetEnvironmentLight(scene, envColor);
    if (res != VLRResult_Success) {
        fprintf(stderr, "Failed to set environment light\n");
        goto cleanup;
    }
    
    // Camera
    VLRCameraParams camera = {};
    camera.position[0] = 0.0f;
    camera.position[1] = 0.0f;
    camera.position[2] = 3.0f;
    camera.direction[0] = 0.0f;
    camera.direction[1] = 0.0f;
    camera.direction[2] = -1.0f;
    camera.up[0] = 0.0f;
    camera.up[1] = 1.0f;
    camera.up[2] = 0.0f;
    camera.fovY = 40.0f * PI / 180.0f;
    camera.aspect = (float)width / (float)height;
    camera.lensRadius = 0.0f;
    camera.focusDistance = 1.0f;
    camera.focalLength = 0.0f;
    camera.cameraType = 0;
    
    res = vlrSetCamera(scene, &camera);
    if (res != VLRResult_Success) {
        fprintf(stderr, "Failed to set camera\n");
        goto cleanup;
    }
    
    // Render
    printf("Rendering %ux%u @ %u SPP...\n", width, height, numSamples);
    res = vlrRender(context, scene, width, height, numSamples, vlr::VLRRenderer_WavefrontPathTracing);
    if (res != VLRResult_Success) {
        fprintf(stderr, "Render failed\n");
        goto cleanup;
    }
    
    void* deviceBuffer = vlrGetOutputBuffer(context);
    if (!deviceBuffer) {
        fprintf(stderr, "Failed to get output buffer\n");
        goto cleanup;
    }
    
    size_t bufferSize = width * height * sizeof(float) * 3;
    float* outputBuffer = (float*)malloc(bufferSize);
    if (!outputBuffer) {
        fprintf(stderr, "Failed to allocate output buffer\n");
        goto cleanup;
    }
    
    cudaError_t cudaErr = cudaMemcpy(outputBuffer, deviceBuffer, bufferSize, cudaMemcpyDeviceToHost);
    if (cudaErr != cudaSuccess) {
        fprintf(stderr, "cudaMemcpy failed\n");
        free(outputBuffer);
        goto cleanup;
    }
    
    savePNG("glass_simple_test.png", width, height, outputBuffer, numSamples, exposure);
    free(outputBuffer);
    
    printf("=== Test complete ===\n");
    
cleanup:
    if (scene) vlrDestroyScene(scene);
    if (context) vlrDestroyContext(context);
    return 0;
}
