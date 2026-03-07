// ============================================================================
// VLR Wavefront - Glass Spheres Test Scene
//
// Simple scene with multiple spheres to test material handling
// Note: Currently using Matte materials as Glass/Specular not yet implemented
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
#include <chrono>

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

// Helper: Create sphere geometry
static void createSphereGeometry(float cx, float cy, float cz, float radius, 
                                int segments, float** outVertices, uint32_t* outNumVertices,
                                uint32_t** outIndices, uint32_t* outNumTriangles) {
    int numVertices = (segments + 1) * (segments + 1);
    int numTriangles = segments * segments * 2;
    
    float* vertices = (float*)malloc(numVertices * 3 * sizeof(float));
    uint32_t* indices = (uint32_t*)malloc(numTriangles * 3 * sizeof(uint32_t));
    
    // Generate sphere vertices
    for (int lat = 0; lat <= segments; lat++) {
        float theta = lat * 3.14159265f / segments;
        float sinTheta = sinf(theta);
        float cosTheta = cosf(theta);
        
        for (int lon = 0; lon <= segments; lon++) {
            float phi = lon * 2.0f * 3.14159265f / segments;
            float sinPhi = sinf(phi);
            float cosPhi = cosf(phi);
            
            float nx = cosPhi * sinTheta;
            float ny = cosTheta;
            float nz = sinPhi * sinTheta;
            
            int idx = (lat * (segments + 1) + lon) * 3;
            vertices[idx + 0] = cx + radius * nx;
            vertices[idx + 1] = cy + radius * ny;
            vertices[idx + 2] = cz + radius * nz;
        }
    }
    
    // Generate sphere indices
    int triIdx = 0;
    for (int lat = 0; lat < segments; lat++) {
        for (int lon = 0; lon < segments; lon++) {
            int first = lat * (segments + 1) + lon;
            int second = first + segments + 1;
            
            indices[triIdx++] = first;
            indices[triIdx++] = second;
            indices[triIdx++] = first + 1;
            
            indices[triIdx++] = second;
            indices[triIdx++] = second + 1;
            indices[triIdx++] = first + 1;
        }
    }
    
    *outVertices = vertices;
    *outNumVertices = numVertices;
    *outIndices = indices;
    *outNumTriangles = numTriangles;
}

int main(int argc, char** argv) {
    printf("=== VLR Glass Spheres Test START ===\n");
    
    // Parse command line arguments
    uint32_t width = 512;
    uint32_t height = 512;
    uint32_t numSamples = 64;
    const char* outputFile = "glass_spheres.ppm";
    
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
    
    printf("=== VLR Glass Spheres Test ===\n");
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
    
    // Back wall - light blue
    VLRMaterial matWall = nullptr;
    float wallColor[] = { 0.5f, 0.7f, 0.9f };
    CHECK_VLR(vlrCreateMaterial(scene, 0, wallColor, nullptr, &matWall));
    
    // Sphere 1 - white (simulating clear glass)
    VLRMaterial matSphere1 = nullptr;
    float sphere1Color[] = { 0.95f, 0.95f, 0.95f };
    CHECK_VLR(vlrCreateMaterial(scene, 0, sphere1Color, nullptr, &matSphere1));
    
    // Sphere 2 - red tint
    VLRMaterial matSphere2 = nullptr;
    float sphere2Color[] = { 0.95f, 0.3f, 0.3f };
    CHECK_VLR(vlrCreateMaterial(scene, 0, sphere2Color, nullptr, &matSphere2));
    
    // Sphere 3 - green tint
    VLRMaterial matSphere3 = nullptr;
    float sphere3Color[] = { 0.3f, 0.95f, 0.3f };
    CHECK_VLR(vlrCreateMaterial(scene, 0, sphere3Color, nullptr, &matSphere3));
    
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
    
    // Floor plane
    float floorVertices[] = {
        -5.0f, 0.0f, -5.0f,
         5.0f, 0.0f, -5.0f,
         5.0f, 0.0f,  5.0f,
        -5.0f, 0.0f,  5.0f
    };
    uint32_t floorIndices[] = { 0, 1, 2, 0, 2, 3 };
    
    VLRTriangleMesh floorMesh = nullptr;
    CHECK_VLR(vlrCreateTriangleMesh(scene, floorVertices, 4, floorIndices, 2, matFloor, &floorMesh));
    
    // Back wall
    float backWallVertices[] = {
        -5.0f, 0.0f, -5.0f,
         5.0f, 0.0f, -5.0f,
         5.0f, 5.0f, -5.0f,
        -5.0f, 5.0f, -5.0f
    };
    uint32_t backWallIndices[] = { 0, 1, 2, 0, 2, 3 };
    
    VLRTriangleMesh backWallMesh = nullptr;
    CHECK_VLR(vlrCreateTriangleMesh(scene, backWallVertices, 4, backWallIndices, 2, matWall, &backWallMesh));
    
    // Create three spheres
    float* sphere1Verts = nullptr;
    uint32_t sphere1NumVerts = 0;
    uint32_t* sphere1Indices = nullptr;
    uint32_t sphere1NumTris = 0;
    createSphereGeometry(-1.5f, 0.8f, 0.0f, 0.8f, 24, &sphere1Verts, &sphere1NumVerts, &sphere1Indices, &sphere1NumTris);
    printf("[Debug] Sphere1: %u vertices, %u triangles\n", sphere1NumVerts, sphere1NumTris);
    fflush(stdout);
    
    VLRTriangleMesh sphere1Mesh = nullptr;
    CHECK_VLR(vlrCreateTriangleMesh(scene, sphere1Verts, sphere1NumVerts, sphere1Indices, sphere1NumTris, matSphere1, &sphere1Mesh));
    
    float* sphere2Verts = nullptr;
    uint32_t sphere2NumVerts = 0;
    uint32_t* sphere2Indices = nullptr;
    uint32_t sphere2NumTris = 0;
    createSphereGeometry(0.0f, 0.6f, 1.0f, 0.6f, 24, &sphere2Verts, &sphere2NumVerts, &sphere2Indices, &sphere2NumTris);
    
    VLRTriangleMesh sphere2Mesh = nullptr;
    CHECK_VLR(vlrCreateTriangleMesh(scene, sphere2Verts, sphere2NumVerts, sphere2Indices, sphere2NumTris, matSphere2, &sphere2Mesh));
    
    float* sphere3Verts = nullptr;
    uint32_t sphere3NumVerts = 0;
    uint32_t* sphere3Indices = nullptr;
    uint32_t sphere3NumTris = 0;
    createSphereGeometry(1.5f, 0.7f, -0.5f, 0.7f, 24, &sphere3Verts, &sphere3NumVerts, &sphere3Indices, &sphere3NumTris);
    
    VLRTriangleMesh sphere3Mesh = nullptr;
    CHECK_VLR(vlrCreateTriangleMesh(scene, sphere3Verts, sphere3NumVerts, sphere3Indices, sphere3NumTris, matSphere3, &sphere3Mesh));
    
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
    
    VLRInstance instBackWall = nullptr;
    CHECK_VLR(vlrCreateInstance(scene, backWallMesh, origin, scale, axis, 0.0f, &instBackWall));
    
    VLRInstance instSphere1 = nullptr;
    CHECK_VLR(vlrCreateInstance(scene, sphere1Mesh, origin, scale, axis, 0.0f, &instSphere1));
    
    VLRInstance instSphere2 = nullptr;
    CHECK_VLR(vlrCreateInstance(scene, sphere2Mesh, origin, scale, axis, 0.0f, &instSphere2));
    
    VLRInstance instSphere3 = nullptr;
    CHECK_VLR(vlrCreateInstance(scene, sphere3Mesh, origin, scale, axis, 0.0f, &instSphere3));
    
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
    camera.position[1] = 2.5f;
    camera.position[2] = 6.0f;
    
    float center[3] = { 0.0f, 1.5f, 0.0f };
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
    
    // Copy from device to host (assuming SpectrumStorage is 3 floats)
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
    
    // Free sphere geometry
    free(sphere1Verts);
    free(sphere1Indices);
    free(sphere2Verts);
    free(sphere2Indices);
    free(sphere3Verts);
    free(sphere3Indices);
    
    // Cleanup
    vlrDestroyScene(scene);
    vlrDestroyContext(context);
    
    printf("=== Test complete ===\n");
    return 0;

cleanup:
    if (sphere1Verts) free(sphere1Verts);
    if (sphere1Indices) free(sphere1Indices);
    if (sphere2Verts) free(sphere2Verts);
    if (sphere2Indices) free(sphere2Indices);
    if (sphere3Verts) free(sphere3Verts);
    if (sphere3Indices) free(sphere3Indices);
    if (scene) vlrDestroyScene(scene);
    if (context) vlrDestroyContext(context);
    return 1;
}
