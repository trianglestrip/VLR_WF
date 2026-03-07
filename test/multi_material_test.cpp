// ============================================================================
// VLR Wavefront - Multi Material Test Scene
//
// Simple scene with multiple colored boxes to test material handling
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

#define CHECK_VLR(call) do { \
    VLRResult result = (call); \
    if (result != VLRResult_Success) { \
        fprintf(stderr, "[Error] %s failed: %d\n", #call, result); \
        goto cleanup; \
    } \
} while(0)

// Helper: Save PPM image
static void savePPM(const char* filename, const float* pixels, uint32_t width, uint32_t height) {
    FILE* fp = fopen(filename, "wb");
    if (!fp) {
        fprintf(stderr, "[Error] Failed to open output file: %s\n", filename);
        return;
    }
    
    fprintf(fp, "P6\n%u %u\n255\n", width, height);
    for (uint32_t i = 0; i < width * height; i++) {
        float r = pixels[i * 3 + 0];
        float g = pixels[i * 3 + 1];
        float b = pixels[i * 3 + 2];
        
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

int main(int argc, char** argv) {
    printf("=== VLR Multi Material Test START ===\n");
    
    // Parse command line arguments
    uint32_t width = 512;
    uint32_t height = 512;
    uint32_t numSamples = 64;
    const char* outputFile = "multi_material.ppm";
    
    for (int i = 1; i < argc; i++) {
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
    
    printf("=== VLR Multi Material Test ===\n");
    printf("Resolution: %u x %u, Samples: %u, Output: %s\n", width, height, numSamples, outputFile);
    fflush(stdout);
    
    // ========================================================================
    // Step 1: Create Context and Scene
    // ========================================================================
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
    
    // ========================================================================
    // Step 2: Create Materials
    // ========================================================================
    printf("[Step 2] Creating materials...\n");
    fflush(stdout);
    
    // Floor - white matte
    VLRMaterial matFloor = nullptr;
    float floorColor[] = { 0.8f, 0.8f, 0.8f };
    CHECK_VLR(vlrCreateMaterial(scene, 0, floorColor, nullptr, &matFloor));
    
    // Box 1 - red
    VLRMaterial matBox1 = nullptr;
    float box1Color[] = { 0.8f, 0.2f, 0.2f };
    CHECK_VLR(vlrCreateMaterial(scene, 0, box1Color, nullptr, &matBox1));
    
    // Box 2 - green
    VLRMaterial matBox2 = nullptr;
    float box2Color[] = { 0.2f, 0.8f, 0.2f };
    CHECK_VLR(vlrCreateMaterial(scene, 0, box2Color, nullptr, &matBox2));
    
    // Box 3 - blue
    VLRMaterial matBox3 = nullptr;
    float box3Color[] = { 0.2f, 0.2f, 0.8f };
    CHECK_VLR(vlrCreateMaterial(scene, 0, box3Color, nullptr, &matBox3));
    
    // Area light
    VLRMaterial matLight = nullptr;
    float lightColor[] = { 0.8f, 0.8f, 0.8f };
    float lightEmission[] = { 15.0f, 15.0f, 15.0f };
    CHECK_VLR(vlrCreateMaterial(scene, 0, lightColor, lightEmission, &matLight));
    
    // ========================================================================
    // Step 3: Create Geometry
    // ========================================================================
    printf("[Step 3] Creating geometry...\n");
    fflush(stdout);
    
    // Floor plane (large)
    float floorVertices[] = {
        -5.0f, 0.0f, -5.0f,
         5.0f, 0.0f, -5.0f,
         5.0f, 0.0f,  5.0f,
        -5.0f, 0.0f,  5.0f
    };
    uint32_t floorIndices[] = { 0, 1, 2, 0, 2, 3 };
    
    VLRTriangleMesh floorMesh = nullptr;
    CHECK_VLR(vlrCreateTriangleMesh(scene, floorVertices, 4, floorIndices, 2, matFloor, &floorMesh));
    
    // Box 1 (left, red) - simple cube
    float box1Vertices[] = {
        // Front face
        -2.0f, 0.0f, -0.5f,  -1.0f, 0.0f, -0.5f,  -1.0f, 1.0f, -0.5f,  -2.0f, 1.0f, -0.5f,
        // Back face
        -2.0f, 0.0f, 0.5f,  -2.0f, 1.0f, 0.5f,  -1.0f, 1.0f, 0.5f,  -1.0f, 0.0f, 0.5f,
        // Left face
        -2.0f, 0.0f, -0.5f,  -2.0f, 1.0f, -0.5f,  -2.0f, 1.0f, 0.5f,  -2.0f, 0.0f, 0.5f,
        // Right face
        -1.0f, 0.0f, -0.5f,  -1.0f, 0.0f, 0.5f,  -1.0f, 1.0f, 0.5f,  -1.0f, 1.0f, -0.5f,
        // Top face
        -2.0f, 1.0f, -0.5f,  -1.0f, 1.0f, -0.5f,  -1.0f, 1.0f, 0.5f,  -2.0f, 1.0f, 0.5f,
        // Bottom face
        -2.0f, 0.0f, -0.5f,  -2.0f, 0.0f, 0.5f,  -1.0f, 0.0f, 0.5f,  -1.0f, 0.0f, -0.5f
    };
    uint32_t box1Indices[] = {
        0, 1, 2,  0, 2, 3,    // Front
        4, 5, 6,  4, 6, 7,    // Back
        8, 9, 10,  8, 10, 11, // Left
        12, 13, 14,  12, 14, 15, // Right
        16, 17, 18,  16, 18, 19, // Top
        20, 21, 22,  20, 22, 23  // Bottom
    };
    
    VLRTriangleMesh box1Mesh = nullptr;
    CHECK_VLR(vlrCreateTriangleMesh(scene, box1Vertices, 24, box1Indices, 12, matBox1, &box1Mesh));
    
    // Box 2 (center, green)
    float box2Vertices[] = {
        // Front face
        -0.5f, 0.0f, -0.5f,  0.5f, 0.0f, -0.5f,  0.5f, 0.8f, -0.5f,  -0.5f, 0.8f, -0.5f,
        // Back face
        -0.5f, 0.0f, 0.5f,  -0.5f, 0.8f, 0.5f,  0.5f, 0.8f, 0.5f,  0.5f, 0.0f, 0.5f,
        // Left face
        -0.5f, 0.0f, -0.5f,  -0.5f, 0.8f, -0.5f,  -0.5f, 0.8f, 0.5f,  -0.5f, 0.0f, 0.5f,
        // Right face
        0.5f, 0.0f, -0.5f,  0.5f, 0.0f, 0.5f,  0.5f, 0.8f, 0.5f,  0.5f, 0.8f, -0.5f,
        // Top face
        -0.5f, 0.8f, -0.5f,  0.5f, 0.8f, -0.5f,  0.5f, 0.8f, 0.5f,  -0.5f, 0.8f, 0.5f,
        // Bottom face
        -0.5f, 0.0f, -0.5f,  -0.5f, 0.0f, 0.5f,  0.5f, 0.0f, 0.5f,  0.5f, 0.0f, -0.5f
    };
    uint32_t box2Indices[] = {
        0, 1, 2,  0, 2, 3,
        4, 5, 6,  4, 6, 7,
        8, 9, 10,  8, 10, 11,
        12, 13, 14,  12, 14, 15,
        16, 17, 18,  16, 18, 19,
        20, 21, 22,  20, 22, 23
    };
    
    VLRTriangleMesh box2Mesh = nullptr;
    CHECK_VLR(vlrCreateTriangleMesh(scene, box2Vertices, 24, box2Indices, 12, matBox2, &box2Mesh));
    
    // Box 3 (right, blue)
    float box3Vertices[] = {
        // Front face
        1.0f, 0.0f, -0.5f,  2.0f, 0.0f, -0.5f,  2.0f, 0.6f, -0.5f,  1.0f, 0.6f, -0.5f,
        // Back face
        1.0f, 0.0f, 0.5f,  1.0f, 0.6f, 0.5f,  2.0f, 0.6f, 0.5f,  2.0f, 0.0f, 0.5f,
        // Left face
        1.0f, 0.0f, -0.5f,  1.0f, 0.6f, -0.5f,  1.0f, 0.6f, 0.5f,  1.0f, 0.0f, 0.5f,
        // Right face
        2.0f, 0.0f, -0.5f,  2.0f, 0.0f, 0.5f,  2.0f, 0.6f, 0.5f,  2.0f, 0.6f, -0.5f,
        // Top face
        1.0f, 0.6f, -0.5f,  2.0f, 0.6f, -0.5f,  2.0f, 0.6f, 0.5f,  1.0f, 0.6f, 0.5f,
        // Bottom face
        1.0f, 0.0f, -0.5f,  1.0f, 0.0f, 0.5f,  2.0f, 0.0f, 0.5f,  2.0f, 0.0f, -0.5f
    };
    uint32_t box3Indices[] = {
        0, 1, 2,  0, 2, 3,
        4, 5, 6,  4, 6, 7,
        8, 9, 10,  8, 10, 11,
        12, 13, 14,  12, 14, 15,
        16, 17, 18,  16, 18, 19,
        20, 21, 22,  20, 22, 23
    };
    
    VLRTriangleMesh box3Mesh = nullptr;
    CHECK_VLR(vlrCreateTriangleMesh(scene, box3Vertices, 24, box3Indices, 12, matBox3, &box3Mesh));
    
    // Area light (ceiling)
    float lightVertices[] = {
        -1.5f, 4.5f, -1.5f,
         1.5f, 4.5f, -1.5f,
         1.5f, 4.5f,  1.5f,
        -1.5f, 4.5f,  1.5f
    };
    uint32_t lightIndices[] = { 0, 1, 2, 0, 2, 3 };
    
    VLRTriangleMesh lightMesh = nullptr;
    CHECK_VLR(vlrCreateTriangleMesh(scene, lightVertices, 4, lightIndices, 2, matLight, &lightMesh));
    
    // ========================================================================
    // Step 4: Create Instances
    // ========================================================================
    printf("[Step 4] Creating instances...\n");
    fflush(stdout);
    
    float origin[] = { 0, 0, 0 };
    float scale[] = { 1, 1, 1 };
    float axis[] = { 0, 1, 0 };
    
    VLRInstance instFloor = nullptr;
    CHECK_VLR(vlrCreateInstance(scene, floorMesh, origin, scale, axis, 0.0f, &instFloor));
    
    VLRInstance instBox1 = nullptr;
    CHECK_VLR(vlrCreateInstance(scene, box1Mesh, origin, scale, axis, 0.0f, &instBox1));
    
    VLRInstance instBox2 = nullptr;
    CHECK_VLR(vlrCreateInstance(scene, box2Mesh, origin, scale, axis, 0.0f, &instBox2));
    
    VLRInstance instBox3 = nullptr;
    CHECK_VLR(vlrCreateInstance(scene, box3Mesh, origin, scale, axis, 0.0f, &instBox3));
    
    VLRInstance instLight = nullptr;
    CHECK_VLR(vlrCreateInstance(scene, lightMesh, origin, scale, axis, 0.0f, &instLight));
    
    // ========================================================================
    // Step 5: Set up camera
    // ========================================================================
    printf("[Step 5] Setting up camera...\n");
    fflush(stdout);
    
    VLRCameraParams camera;
    memset(&camera, 0, sizeof(camera));
    
    camera.position[0] = 0.0f;
    camera.position[1] = 2.0f;
    camera.position[2] = 5.0f;
    
    float center[3] = { 0.0f, 0.5f, 0.0f };
    float dir[3] = {
        center[0] - camera.position[0],
        center[1] - camera.position[1],
        center[2] - camera.position[2]
    };
    float dirLen = sqrtf(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
    camera.direction[0] = dir[0] / dirLen;
    camera.direction[1] = dir[1] / dirLen;
    camera.direction[2] = dir[2] / dirLen;
    
    camera.up[0] = 0.0f;
    camera.up[1] = 1.0f;
    camera.up[2] = 0.0f;
    
    camera.fovY = 45.0f * 3.14159265f / 180.0f;
    camera.aspect = (float)width / (float)height;
    camera.lensRadius = 0.0f;
    camera.focusDistance = 1.0f;
    camera.focalLength = 0.0f;
    camera.cameraType = 0;
    
    CHECK_VLR(vlrSetCamera(scene, &camera));
    
    // ========================================================================
    // Step 6: Render
    // ========================================================================
    printf("[Render] Starting render...\n");
    fflush(stdout);
    
    float* pixels = (float*)malloc(width * height * 3 * sizeof(float));
    if (!pixels) {
        fprintf(stderr, "[Error] Failed to allocate pixel buffer\n");
        goto cleanup;
    }
    
    res = vlrRender(context, scene, width, height, numSamples, VLRRenderer_WavefrontPathTracing);
    
    if (res != VLRResult_Success) {
        fprintf(stderr, "[Error] Render failed: %d\n", res);
        free(pixels);
        goto cleanup;
    }
    
    // Get output buffer from device
    void* deviceBuffer = vlrGetOutputBuffer(context);
    if (!deviceBuffer) {
        fprintf(stderr, "[Error] Failed to get output buffer\n");
        free(pixels);
        goto cleanup;
    }
    
    // Copy from device to host
    cudaError_t cudaErr = cudaMemcpy(pixels, deviceBuffer, width * height * 3 * sizeof(float), cudaMemcpyDeviceToHost);
    if (cudaErr != cudaSuccess) {
        fprintf(stderr, "[Error] cudaMemcpy failed: %s\n", cudaGetErrorString(cudaErr));
        free(pixels);
        goto cleanup;
    }
    
    printf("[Render] Done\n");
    fflush(stdout);
    
    // Save image
    savePPM(outputFile, pixels, width, height);
    free(pixels);
    
    // Cleanup
    vlrDestroyScene(scene);
    vlrDestroyContext(context);
    
    printf("=== Test complete ===\n");
    return 0;

cleanup:
    if (scene) vlrDestroyScene(scene);
    if (context) vlrDestroyContext(context);
    return 1;
}
