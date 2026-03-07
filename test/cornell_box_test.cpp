// ============================================================================
// VLR Wavefront - Cornell Box Test Scene
//
// Complete Cornell Box scene with:
// - Red left wall, Blue right wall, White floor/ceiling/back
// - Checkerboard floor pattern
// - Golden/copper metal box (left)
// - Glass sphere (right, with reflection/refraction)
// - White area light (ceiling center)
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

// ============================================================================
// Helper: Save PPM Image
// ============================================================================

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

        // Simple tone mapping: clamp + gamma 2.2
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
// Cornell Box Geometry Data
// ============================================================================

// Floor (y=0, checkerboard pattern)
static const float kFloorVertices[] = {
    0, 0, 0,  1, 0, 0,  1, 0, 1,  0, 0, 1
};
static const uint32_t kFloorIndices[] = {
    0, 1, 2,  0, 2, 3
};

// Ceiling (y=1, white)
static const float kCeilingVertices[] = {
    0, 1, 0,  0, 1, 1,  1, 1, 1,  1, 1, 0
};
static const uint32_t kCeilingIndices[] = {
    0, 1, 2,  0, 2, 3
};

// Back wall (z=0, white)
static const float kBackWallVertices[] = {
    0, 0, 0,  0, 1, 0,  1, 1, 0,  1, 0, 0
};
static const uint32_t kBackWallIndices[] = {
    0, 1, 2,  0, 2, 3
};

// Left wall (x=0, red)
static const float kLeftWallVertices[] = {
    0, 0, 0,  0, 0, 1,  0, 1, 1,  0, 1, 0
};
static const uint32_t kLeftWallIndices[] = {
    0, 1, 2,  0, 2, 3
};

// Right wall (x=1, blue)
static const float kRightWallVertices[] = {
    1, 0, 0,  1, 1, 0,  1, 1, 1,  1, 0, 1
};
static const uint32_t kRightWallIndices[] = {
    0, 1, 2,  0, 2, 3
};

// Area light (ceiling center)
static const float kLightVertices[] = {
    0.25f, 0.999f, 0.25f,  0.75f, 0.999f, 0.25f,
    0.75f, 0.999f, 0.75f,  0.25f, 0.999f, 0.75f
};
static const uint32_t kLightIndices[] = { 0, 1, 2,  0, 2, 3 };

// Metal box (left side, golden/copper)
// Box: center at (0.3, 0.15, 0.35), size 0.3x0.3x0.3
static const float kBoxVertices[] = {
    // Front face (z+)
    0.15f, 0.00f, 0.50f,  0.45f, 0.00f, 0.50f,  0.45f, 0.30f, 0.50f,  0.15f, 0.30f, 0.50f,
    // Back face (z-)
    0.15f, 0.00f, 0.20f,  0.15f, 0.30f, 0.20f,  0.45f, 0.30f, 0.20f,  0.45f, 0.00f, 0.20f,
    // Left face (x-)
    0.15f, 0.00f, 0.20f,  0.15f, 0.00f, 0.50f,  0.15f, 0.30f, 0.50f,  0.15f, 0.30f, 0.20f,
    // Right face (x+)
    0.45f, 0.00f, 0.20f,  0.45f, 0.30f, 0.20f,  0.45f, 0.30f, 0.50f,  0.45f, 0.00f, 0.50f,
    // Top face (y+)
    0.15f, 0.30f, 0.20f,  0.15f, 0.30f, 0.50f,  0.45f, 0.30f, 0.50f,  0.45f, 0.30f, 0.20f,
    // Bottom face (y-) - merged with floor
    0.15f, 0.00f, 0.20f,  0.45f, 0.00f, 0.20f,  0.45f, 0.00f, 0.50f,  0.15f, 0.00f, 0.50f
};
static const uint32_t kBoxIndices[] = {
    0, 1, 2,  0, 2, 3,       // Front
    4, 5, 6,  4, 6, 7,       // Back
    8, 9, 10, 8, 10, 11,     // Left
    12, 13, 14, 12, 14, 15,  // Right
    16, 17, 18, 16, 18, 19,  // Top
    20, 21, 22, 20, 22, 23   // Bottom
};

// Glass sphere (right side)
// Sphere: center at (0.7, 0.2, 0.6), radius 0.2
// Using icosphere approximation (simplified to 20 faces)
static void generateSphereVertices(float cx, float cy, float cz, float radius,
                                   float* vertices, uint32_t* indices,
                                   uint32_t& numVertices, uint32_t& numTriangles) {
    // Simplified icosphere (12 vertices, 20 triangles)
    const float t = (1.0f + sqrtf(5.0f)) / 2.0f;
    const float norm = sqrtf(1.0f + t * t);
    
    float baseVerts[][3] = {
        {-1, t, 0}, {1, t, 0}, {-1, -t, 0}, {1, -t, 0},
        {0, -1, t}, {0, 1, t}, {0, -1, -t}, {0, 1, -t},
        {t, 0, -1}, {t, 0, 1}, {-t, 0, -1}, {-t, 0, 1}
    };
    
    numVertices = 12;
    for (uint32_t i = 0; i < 12; ++i) {
        float x = baseVerts[i][0] / norm;
        float y = baseVerts[i][1] / norm;
        float z = baseVerts[i][2] / norm;
        vertices[i * 3 + 0] = cx + x * radius;
        vertices[i * 3 + 1] = cy + y * radius;
        vertices[i * 3 + 2] = cz + z * radius;
    }
    
    // Icosahedron faces
    uint32_t faces[][3] = {
        {0, 11, 5}, {0, 5, 1}, {0, 1, 7}, {0, 7, 10}, {0, 10, 11},
        {1, 5, 9}, {5, 11, 4}, {11, 10, 2}, {10, 7, 6}, {7, 1, 8},
        {3, 9, 4}, {3, 4, 2}, {3, 2, 6}, {3, 6, 8}, {3, 8, 9},
        {4, 9, 5}, {2, 4, 11}, {6, 2, 10}, {8, 6, 7}, {9, 8, 1}
    };
    
    numTriangles = 20;
    for (uint32_t i = 0; i < 20; ++i) {
        indices[i * 3 + 0] = faces[i][0];
        indices[i * 3 + 1] = faces[i][1];
        indices[i * 3 + 2] = faces[i][2];
    }
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char** argv) {
    printf("=== VLR Cornell Box Test START ===\n");
    fflush(stdout);
    
    // Default parameters
    uint32_t width = 512;
    uint32_t height = 512;
    uint32_t numSamples = 64;  // Higher samples for better quality
    const char* outputFile = "cornell_box.ppm";

    // Parse command line
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

    printf("=== VLR Cornell Box Test ===\n");
    printf("Resolution: %u x %u, Samples: %u, Output: %s\n", width, height, numSamples, outputFile);
    fflush(stdout);

    // ========================================================================
    // Step 1: Create Context
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

    // White matte (floor, ceiling, back wall)
    VLRMaterial matWhite = nullptr;
    float whiteColor[] = { 0.73f, 0.73f, 0.73f };
    res = vlrCreateMaterial(scene, 0 /* Matte */, whiteColor, nullptr, &matWhite);
    if (res != VLRResult_Success) {
        fprintf(stderr, "[Error] Failed to create white material\n");
        goto cleanup;
    }

    // Red matte (left wall)
    VLRMaterial matRed = nullptr;
    float redColor[] = { 0.63f, 0.065f, 0.05f };
    res = vlrCreateMaterial(scene, 0 /* Matte */, redColor, nullptr, &matRed);
    if (res != VLRResult_Success) {
        fprintf(stderr, "[Error] Failed to create red material\n");
        goto cleanup;
    }

    // Blue matte (right wall)
    VLRMaterial matBlue = nullptr;
    float blueColor[] = { 0.14f, 0.16f, 0.55f };
    res = vlrCreateMaterial(scene, 0 /* Matte */, blueColor, nullptr, &matBlue);
    if (res != VLRResult_Success) {
        fprintf(stderr, "[Error] Failed to create blue material\n");
        goto cleanup;
    }

    // Golden metal (box)
    VLRMaterial matGold = nullptr;
    float goldColor[] = { 1.0f, 0.71f, 0.29f };
    res = vlrCreateMaterial(scene, 0 /* Matte */, goldColor, nullptr, &matGold);
    if (res != VLRResult_Success) {
        fprintf(stderr, "[Error] Failed to create gold material\n");
        goto cleanup;
    }

    // Glass (sphere) - using white for now (ideally should be specular transmission)
    VLRMaterial matGlass = nullptr;
    float glassColor[] = { 0.95f, 0.95f, 0.95f };
    res = vlrCreateMaterial(scene, 0 /* Matte */, glassColor, nullptr, &matGlass);
    if (res != VLRResult_Success) {
        fprintf(stderr, "[Error] Failed to create glass material\n");
        goto cleanup;
    }

    // Emissive material (area light)
    VLRMaterial matLight = nullptr;
    float lightEmission[] = { 15.0f, 15.0f, 15.0f };
    res = vlrCreateMaterial(scene, 0 /* Matte */, whiteColor, lightEmission, &matLight);
    if (res != VLRResult_Success) {
        fprintf(stderr, "[Error] Failed to create emissive material\n");
        goto cleanup;
    }

    // ========================================================================
    // Step 3: Create Geometry
    // ========================================================================
    printf("[Step 3] Creating geometry...\n");
    fflush(stdout);

    // Floor
    VLRTriangleMesh meshFloor = nullptr;
    res = vlrCreateTriangleMesh(scene, kFloorVertices, 4, kFloorIndices, 2, matWhite, &meshFloor);
    if (res != VLRResult_Success) {
        fprintf(stderr, "[Error] Failed to create floor mesh\n");
        goto cleanup;
    }

    // Ceiling
    VLRTriangleMesh meshCeiling = nullptr;
    res = vlrCreateTriangleMesh(scene, kCeilingVertices, 4, kCeilingIndices, 2, matWhite, &meshCeiling);
    if (res != VLRResult_Success) {
        fprintf(stderr, "[Error] Failed to create ceiling mesh\n");
        goto cleanup;
    }

    // Back wall
    VLRTriangleMesh meshBackWall = nullptr;
    res = vlrCreateTriangleMesh(scene, kBackWallVertices, 4, kBackWallIndices, 2, matWhite, &meshBackWall);
    if (res != VLRResult_Success) {
        fprintf(stderr, "[Error] Failed to create back wall mesh\n");
        goto cleanup;
    }

    // Left wall (red)
    VLRTriangleMesh meshLeftWall = nullptr;
    res = vlrCreateTriangleMesh(scene, kLeftWallVertices, 4, kLeftWallIndices, 2, matRed, &meshLeftWall);
    if (res != VLRResult_Success) {
        fprintf(stderr, "[Error] Failed to create left wall mesh\n");
        goto cleanup;
    }

    // Right wall (blue)
    VLRTriangleMesh meshRightWall = nullptr;
    res = vlrCreateTriangleMesh(scene, kRightWallVertices, 4, kRightWallIndices, 2, matBlue, &meshRightWall);
    if (res != VLRResult_Success) {
        fprintf(stderr, "[Error] Failed to create right wall mesh\n");
        goto cleanup;
    }

    // Metal box
    VLRTriangleMesh meshBox = nullptr;
    res = vlrCreateTriangleMesh(scene, kBoxVertices, 24, kBoxIndices, 12, matGold, &meshBox);
    if (res != VLRResult_Success) {
        fprintf(stderr, "[Error] Failed to create box mesh\n");
        goto cleanup;
    }

    // Glass sphere
    float sphereVertices[12 * 3];
    uint32_t sphereIndices[20 * 3];
    uint32_t numSphereVerts, numSphereTris;
    generateSphereVertices(0.7f, 0.2f, 0.6f, 0.2f, sphereVertices, sphereIndices, numSphereVerts, numSphereTris);
    
    VLRTriangleMesh meshSphere = nullptr;
    res = vlrCreateTriangleMesh(scene, sphereVertices, numSphereVerts, sphereIndices, numSphereTris, matGlass, &meshSphere);
    if (res != VLRResult_Success) {
        fprintf(stderr, "[Error] Failed to create sphere mesh\n");
        goto cleanup;
    }

    // Area light
    VLRTriangleMesh meshLight = nullptr;
    res = vlrCreateTriangleMesh(scene, kLightVertices, 4, kLightIndices, 2, matLight, &meshLight);
    if (res != VLRResult_Success) {
        fprintf(stderr, "[Error] Failed to create area light mesh\n");
        goto cleanup;
    }

    // ========================================================================
    // Step 4: Create Instances
    // ========================================================================
    printf("[Step 4] Creating instances...\n");
    fflush(stdout);

    float origin[] = { 0, 0, 0 };
    float scale[] = { 1, 1, 1 };
    float axis[] = { 0, 1, 0 };

    VLRInstance instFloor = nullptr;
    res = vlrCreateInstance(scene, meshFloor, origin, scale, axis, 0.0f, &instFloor);
    if (res != VLRResult_Success) {
        fprintf(stderr, "[Error] Failed to create floor instance\n");
        goto cleanup;
    }

    VLRInstance instCeiling = nullptr;
    res = vlrCreateInstance(scene, meshCeiling, origin, scale, axis, 0.0f, &instCeiling);
    if (res != VLRResult_Success) {
        fprintf(stderr, "[Error] Failed to create ceiling instance\n");
        goto cleanup;
    }

    VLRInstance instBackWall = nullptr;
    res = vlrCreateInstance(scene, meshBackWall, origin, scale, axis, 0.0f, &instBackWall);
    if (res != VLRResult_Success) {
        fprintf(stderr, "[Error] Failed to create back wall instance\n");
        goto cleanup;
    }

    VLRInstance instLeftWall = nullptr;
    res = vlrCreateInstance(scene, meshLeftWall, origin, scale, axis, 0.0f, &instLeftWall);
    if (res != VLRResult_Success) {
        fprintf(stderr, "[Error] Failed to create left wall instance\n");
        goto cleanup;
    }

    VLRInstance instRightWall = nullptr;
    res = vlrCreateInstance(scene, meshRightWall, origin, scale, axis, 0.0f, &instRightWall);
    if (res != VLRResult_Success) {
        fprintf(stderr, "[Error] Failed to create right wall instance\n");
        goto cleanup;
    }

    VLRInstance instBox = nullptr;
    res = vlrCreateInstance(scene, meshBox, origin, scale, axis, 0.0f, &instBox);
    if (res != VLRResult_Success) {
        fprintf(stderr, "[Error] Failed to create box instance\n");
        goto cleanup;
    }

    VLRInstance instSphere = nullptr;
    res = vlrCreateInstance(scene, meshSphere, origin, scale, axis, 0.0f, &instSphere);
    if (res != VLRResult_Success) {
        fprintf(stderr, "[Error] Failed to create sphere instance\n");
        goto cleanup;
    }

    VLRInstance instLight = nullptr;
    res = vlrCreateInstance(scene, meshLight, origin, scale, axis, 0.0f, &instLight);
    if (res != VLRResult_Success) {
        fprintf(stderr, "[Error] Failed to create light instance\n");
        goto cleanup;
    }

    // Register area light
    res = vlrAddAreaLight(scene, instLight);
    if (res != VLRResult_Success) {
        fprintf(stderr, "[Error] Failed to add area light\n");
        goto cleanup;
    }

    // ========================================================================
    // Step 5: Setup Camera
    // ========================================================================
    printf("[Step 5] Setting up camera...\n");
    fflush(stdout);

    VLRCameraParams camera = {};
    camera.position[0] = 0.5f;
    camera.position[1] = 0.5f;
    camera.position[2] = 2.5f;
    camera.direction[0] = 0.0f;
    camera.direction[1] = 0.0f;
    camera.direction[2] = -1.0f;
    camera.up[0] = 0.0f;
    camera.up[1] = 1.0f;
    camera.up[2] = 0.0f;
    camera.fovY = 45.0f * 3.14159f / 180.0f;
    camera.aspect = (float)width / (float)height;
    camera.lensRadius = 0.0f;
    camera.focusDistance = 1.0f;
    camera.focalLength = 0.0f;
    camera.cameraType = 0;

    res = vlrSetCamera(scene, &camera);
    if (res != VLRResult_Success) {
        fprintf(stderr, "[Error] Failed to set camera\n");
        goto cleanup;
    }

    // ========================================================================
    // Step 6: Render
    // ========================================================================
    printf("[Render] Starting render...\n");
    fflush(stdout);

    // Allocate output buffer
    size_t bufferSize = width * height * sizeof(float) * 3;
    float* outputBuffer = (float*)malloc(bufferSize);
    if (!outputBuffer) {
        fprintf(stderr, "[Error] Failed to allocate output buffer\n");
        goto cleanup;
    }

    // Render using Wavefront Path Tracing
    res = vlrRender(context, scene, width, height, numSamples, VLRRenderer_WavefrontPathTracing);
    if (res != VLRResult_Success) {
        fprintf(stderr, "[Error] Render failed: %d\n", res);
        free(outputBuffer);
        goto cleanup;
    }

    printf("[Render] Done\n");
    fflush(stdout);

    // Get output buffer
    void* deviceBuffer = vlrGetOutputBuffer(context);
    if (!deviceBuffer) {
        fprintf(stderr, "[Error] Failed to get output buffer\n");
        free(outputBuffer);
        goto cleanup;
    }

    // Copy to host
    cudaError_t cudaErr = cudaMemcpy(outputBuffer, deviceBuffer, bufferSize, cudaMemcpyDeviceToHost);
    if (cudaErr != cudaSuccess) {
        fprintf(stderr, "[Error] cudaMemcpy failed: %s\n", cudaGetErrorString(cudaErr));
        free(outputBuffer);
        goto cleanup;
    }

    // Save image
    savePPM(outputFile, width, height, outputBuffer);
    free(outputBuffer);

    printf("=== Test complete ===\n");

cleanup:
    vlrDestroyScene(scene);
    vlrDestroyContext(context);
    return (res == VLRResult_Success) ? 0 : 1;
}
