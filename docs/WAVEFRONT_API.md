# VLR Wavefront API Documentation

**Version**: 1.0  
**Date**: 2026-03-07  
**Status**: Production Ready  

---

## Table of Contents

1. [Overview](#overview)
2. [Core API](#core-api)
3. [Wavefront Configuration](#wavefront-configuration)
4. [Usage Examples](#usage-examples)
5. [Performance Tuning](#performance-tuning)
6. [Troubleshooting](#troubleshooting)

---

## Overview

The VLR Wavefront API provides a high-performance path tracing renderer using the Wavefront architecture. This API enables GPU-accelerated rendering with significantly improved performance compared to traditional recursive path tracing.

### Key Features

- ✅ **Wavefront Path Tracing**: Process rays in batches for better GPU utilization
- ✅ **Path Sorting**: Sort paths by material category to reduce divergence
- ✅ **Stream Compaction**: Remove terminated paths to maintain efficiency
- ✅ **CUB Integration**: Leverage NVIDIA CUB for optimized GPU operations
- ✅ **Flexible Configuration**: Enable/disable optimizations at runtime

### Performance

- **Throughput**: Up to 150 samples/s at 512x512 resolution
- **Scalability**: Handles resolutions from 256x256 to 4K (3840x2160)
- **Memory**: ~62 MB for 512x512, ~872 MB for 2560x1440

---

## Core API

### Context Management

#### `vlrCreateContext`

```c
VLR_API VLRResult vlrCreateContext(
    void* cudaStream,
    int enableLogging,
    VLRContext* outContext
);
```

Creates a VLR rendering context.

**Parameters**:
- `cudaStream`: CUDA stream to use (can be `nullptr` for default stream)
- `enableLogging`: Enable OptiX logging (0=disabled, 1=enabled)
- `outContext`: Output context handle

**Returns**: `VLRResult_Success` on success, error code otherwise

**Example**:
```c
VLRContext context = nullptr;
VLRResult res = vlrCreateContext(nullptr, 0, &context);
if (res != VLRResult_Success) {
    fprintf(stderr, "Failed to create context: %d\n", res);
    return 1;
}
```

#### `vlrDestroyContext`

```c
VLR_API void vlrDestroyContext(VLRContext context);
```

Destroys a VLR context and frees all associated resources.

**Parameters**:
- `context`: Context handle (can be `nullptr`, no-op)

---

### Scene Management

#### `vlrCreateScene`

```c
VLR_API VLRResult vlrCreateScene(
    VLRContext context,
    VLRScene* outScene
);
```

Creates a scene for rendering.

**Parameters**:
- `context`: Parent context handle
- `outScene`: Output scene handle

**Returns**: `VLRResult_Success` on success, error code otherwise

#### `vlrDestroyScene`

```c
VLR_API void vlrDestroyScene(VLRScene scene);
```

Destroys a scene.

---

### Geometry Management

#### `vlrCreateTriangleMesh`

```c
VLR_API VLRResult vlrCreateTriangleMesh(
    VLRScene scene,
    const float* vertices,
    uint32_t numVertices,
    const uint32_t* indices,
    uint32_t numTriangles,
    VLRMaterial material,
    VLRTriangleMesh* outMesh
);
```

Creates a triangle mesh geometry.

**Parameters**:
- `scene`: Parent scene handle
- `vertices`: Vertex positions array `[x,y,z, x,y,z, ...]` (3*numVertices floats)
- `numVertices`: Number of vertices
- `indices`: Triangle indices array `[i0,i1,i2, i0,i1,i2, ...]` (3*numTriangles uint32_t)
- `numTriangles`: Number of triangles
- `material`: Material handle (must be created first)
- `outMesh`: Output mesh handle

**Returns**: `VLRResult_Success` on success, error code otherwise

**Example**:
```c
float vertices[] = { 0,0,0, 1,0,0, 1,0,1, 0,0,1 };
uint32_t indices[] = { 0,1,2, 0,2,3 };
VLRTriangleMesh mesh = nullptr;
VLRResult res = vlrCreateTriangleMesh(scene, vertices, 4, indices, 2, material, &mesh);
```

---

### Material Management

#### `vlrCreateMaterial`

```c
VLR_API VLRResult vlrCreateMaterial(
    VLRScene scene,
    uint32_t materialType,
    const float* baseColor,
    const float* emissionColor,
    VLRMaterial* outMaterial
);
```

Creates a material.

**Parameters**:
- `scene`: Parent scene handle
- `materialType`: Material type (0=Matte)
- `baseColor`: Base color RGB [0..1] (3 floats, can be `nullptr` for default gray)
- `emissionColor`: Emission color RGB (3 floats, can be `nullptr` for non-emissive)
- `outMaterial`: Output material handle

**Returns**: `VLRResult_Success` on success, error code otherwise

**Example**:
```c
// Diffuse material
float color[] = { 0.8f, 0.2f, 0.2f };
VLRMaterial matDiffuse = nullptr;
vlrCreateMaterial(scene, 0, color, nullptr, &matDiffuse);

// Emissive material (light)
float lightColor[] = { 1.0f, 1.0f, 1.0f };
float emission[] = { 15.0f, 15.0f, 15.0f };
VLRMaterial matLight = nullptr;
vlrCreateMaterial(scene, 0, lightColor, emission, &matLight);
```

---

### Instance Management

#### `vlrCreateInstance`

```c
VLR_API VLRResult vlrCreateInstance(
    VLRScene scene,
    VLRTriangleMesh mesh,
    const float* translation,
    const float* scale,
    const float* rotationAxis,
    float rotationAngle,
    VLRInstance* outInstance
);
```

Creates an instance of a mesh in the scene.

**⚠️ CRITICAL**: All meshes **must** have at least one instance created. Meshes without instances will not be included in the scene.

**Parameters**:
- `scene`: Parent scene handle
- `mesh`: Mesh to instantiate
- `translation`: Translation vector [x,y,z] (3 floats)
- `scale`: Scale factors [x,y,z] (3 floats)
- `rotationAxis`: Rotation axis (unit vector, 3 floats)
- `rotationAngle`: Rotation angle in radians
- `outInstance`: Output instance handle

**Returns**: `VLRResult_Success` on success, error code otherwise

**Example**:
```c
float origin[] = { 0, 0, 0 };
float scale[] = { 1, 1, 1 };
float axis[] = { 0, 1, 0 };

VLRInstance instance = nullptr;
vlrCreateInstance(scene, mesh, origin, scale, axis, 0.0f, &instance);
```

---

### Camera Setup

#### `vlrSetCamera`

```c
VLR_API VLRResult vlrSetCamera(
    VLRScene scene,
    const VLRCameraParams* params
);
```

Sets the camera parameters for the scene.

**Parameters**:
- `scene`: Scene handle
- `params`: Camera parameters structure

**Camera Parameters Structure**:
```c
typedef struct VLRCameraParams {
    float position[3];      // Camera position (x, y, z)
    float direction[3];     // View direction (unit vector)
    float up[3];            // Up direction (unit vector)
    float fovY;             // Vertical field of view (radians)
    float aspect;           // Aspect ratio (width/height)
    float lensRadius;       // Aperture radius (0 for pinhole)
    float focusDistance;    // Focus plane distance
    float focalLength;      // Focal length (0 to derive from FOV)
    uint32_t cameraType;    // Camera type (0=perspective)
} VLRCameraParams;
```

**Example**:
```c
VLRCameraParams camera;
memset(&camera, 0, sizeof(camera));

camera.position[0] = 0.0f;
camera.position[1] = 1.0f;
camera.position[2] = 3.0f;

camera.direction[0] = 0.0f;
camera.direction[1] = 0.0f;
camera.direction[2] = -1.0f;

camera.up[0] = 0.0f;
camera.up[1] = 1.0f;
camera.up[2] = 0.0f;

camera.fovY = 45.0f * 3.14159265f / 180.0f;  // 45 degrees
camera.aspect = (float)width / (float)height;

vlrSetCamera(scene, &camera);
```

---

### Rendering

#### `vlrRender`

```c
VLR_API VLRResult vlrRender(
    VLRContext context,
    VLRScene scene,
    uint32_t width,
    uint32_t height,
    uint32_t numSamples,
    uint32_t renderer
);
```

Renders the scene.

**Parameters**:
- `context`: Context handle
- `scene`: Scene to render
- `width`: Image width in pixels
- `height`: Image height in pixels
- `numSamples`: Number of samples per pixel
- `renderer`: Renderer type (`VLRRenderer_WavefrontPathTracing = 3`)

**Returns**: `VLRResult_Success` on success, error code otherwise

**Example**:
```c
VLRResult res = vlrRender(
    context,
    scene,
    512,  // width
    512,  // height
    64,   // samples
    VLRRenderer_WavefrontPathTracing
);
```

#### `vlrGetOutputBuffer`

```c
VLR_API void* vlrGetOutputBuffer(VLRContext context);
```

Gets the device-side output buffer pointer.

**Parameters**:
- `context`: Context handle

**Returns**: Device pointer to accumulated buffer (3 floats per pixel, RGB), or `nullptr` if not initialized

**Example**:
```c
void* deviceBuffer = vlrGetOutputBuffer(context);
float* pixels = (float*)malloc(width * height * 3 * sizeof(float));
cudaMemcpy(pixels, deviceBuffer, width * height * 3 * sizeof(float), cudaMemcpyDeviceToHost);
```

---

## Wavefront Configuration

### Path Sorting

#### `vlrContextSetWavefrontPathSorting`

```c
VLR_API VLRResult vlrContextSetWavefrontPathSorting(
    VLRContext context,
    int enable
);
```

Enables or disables path sorting by material category.

**Parameters**:
- `context`: Context handle
- `enable`: 1 to enable, 0 to disable

**Benefits**:
- Reduces warp divergence during BSDF evaluation
- Improves cache coherency
- ~10-20% performance improvement

**Cost**:
- Additional sorting overhead (~1-2% of frame time)
- Extra memory for material keys

**Recommendation**: Enable for scenes with multiple material types.

### Stream Compaction

#### `vlrContextSetWavefrontStreamCompaction`

```c
VLR_API VLRResult vlrContextSetWavefrontStreamCompaction(
    VLRContext context,
    int enable
);
```

Enables or disables stream compaction for terminated paths.

**Parameters**:
- `context`: Context handle
- `enable`: 1 to enable, 0 to disable

**Benefits**:
- Removes terminated paths from active queue
- Maintains high GPU occupancy
- ~5-10% performance improvement

**Cost**:
- Compaction overhead (~1% of frame time)

**Recommendation**: Enable for all scenes (default: enabled).

---

## Usage Examples

### Basic Rendering

```c
#include <vlr/vlr.h>
#include <cuda_runtime.h>

int main() {
    // 1. Create context
    VLRContext context = nullptr;
    vlrCreateContext(nullptr, 0, &context);
    
    // 2. Create scene
    VLRScene scene = nullptr;
    vlrCreateScene(context, &scene);
    
    // 3. Create material
    VLRMaterial material = nullptr;
    float color[] = { 0.8f, 0.8f, 0.8f };
    vlrCreateMaterial(scene, 0, color, nullptr, &material);
    
    // 4. Create geometry
    float vertices[] = { 0,0,0, 1,0,0, 1,0,1, 0,0,1 };
    uint32_t indices[] = { 0,1,2, 0,2,3 };
    VLRTriangleMesh mesh = nullptr;
    vlrCreateTriangleMesh(scene, vertices, 4, indices, 2, material, &mesh);
    
    // 5. Create instance (REQUIRED!)
    float origin[] = { 0, 0, 0 };
    float scale[] = { 1, 1, 1 };
    float axis[] = { 0, 1, 0 };
    VLRInstance instance = nullptr;
    vlrCreateInstance(scene, mesh, origin, scale, axis, 0.0f, &instance);
    
    // 6. Set camera
    VLRCameraParams camera;
    memset(&camera, 0, sizeof(camera));
    camera.position[0] = 0.5f;
    camera.position[1] = 0.5f;
    camera.position[2] = 2.0f;
    camera.direction[0] = 0.0f;
    camera.direction[1] = 0.0f;
    camera.direction[2] = -1.0f;
    camera.up[0] = 0.0f;
    camera.up[1] = 1.0f;
    camera.up[2] = 0.0f;
    camera.fovY = 45.0f * 3.14159265f / 180.0f;
    camera.aspect = 1.0f;
    vlrSetCamera(scene, &camera);
    
    // 7. Render
    vlrRender(context, scene, 512, 512, 64, VLRRenderer_WavefrontPathTracing);
    
    // 8. Get output
    void* deviceBuffer = vlrGetOutputBuffer(context);
    float* pixels = (float*)malloc(512 * 512 * 3 * sizeof(float));
    cudaMemcpy(pixels, deviceBuffer, 512 * 512 * 3 * sizeof(float), cudaMemcpyDeviceToHost);
    
    // 9. Save image (PPM format)
    FILE* fp = fopen("output.ppm", "wb");
    fprintf(fp, "P6\n512 512\n255\n");
    for (int i = 0; i < 512 * 512; i++) {
        unsigned char r = (unsigned char)(fminf(pixels[i*3+0], 1.0f) * 255.0f);
        unsigned char g = (unsigned char)(fminf(pixels[i*3+1], 1.0f) * 255.0f);
        unsigned char b = (unsigned char)(fminf(pixels[i*3+2], 1.0f) * 255.0f);
        fputc(r, fp); fputc(g, fp); fputc(b, fp);
    }
    fclose(fp);
    
    // 10. Cleanup
    free(pixels);
    vlrDestroyScene(scene);
    vlrDestroyContext(context);
    
    return 0;
}
```

### Optimized Rendering

```c
// Enable Wavefront optimizations
vlrContextSetWavefrontPathSorting(context, 1);      // Enable path sorting
vlrContextSetWavefrontStreamCompaction(context, 1); // Enable stream compaction

// Render with optimizations
vlrRender(context, scene, width, height, samples, VLRRenderer_WavefrontPathTracing);
```

### Creating Emissive Materials (Lights)

```c
// Area light material
VLRMaterial lightMaterial = nullptr;
float lightColor[] = { 1.0f, 1.0f, 1.0f };
float emission[] = { 15.0f, 15.0f, 15.0f };  // Emission intensity
vlrCreateMaterial(scene, 0, lightColor, emission, &lightMaterial);

// Create light geometry (e.g., ceiling quad)
float lightVertices[] = {
    0.25f, 0.999f, 0.25f,
    0.75f, 0.999f, 0.25f,
    0.75f, 0.999f, 0.75f,
    0.25f, 0.999f, 0.75f
};
uint32_t lightIndices[] = { 0, 1, 2, 0, 2, 3 };
VLRTriangleMesh lightMesh = nullptr;
vlrCreateTriangleMesh(scene, lightVertices, 4, lightIndices, 2, lightMaterial, &lightMesh);

// Create instance
VLRInstance lightInstance = nullptr;
vlrCreateInstance(scene, lightMesh, origin, scale, axis, 0.0f, &lightInstance);
```

---

## Performance Tuning

### Recommended Settings

| Resolution | Samples | Path Sorting | Stream Compaction | Expected Time |
|------------|---------|--------------|-------------------|---------------|
| 512x512 | 64-128 | Enabled | Enabled | 0.5-1.0s |
| 1920x1080 | 32-64 | Enabled | Enabled | 1.0-2.0s |
| 3840x2160 | 16-32 | Enabled | Enabled | 2.0-4.0s |

### Memory Requirements

| Resolution | Path Count | Approximate Memory |
|------------|------------|-------------------|
| 512x512 | 262,144 | ~62 MB |
| 1920x1080 | 2,073,600 | ~490 MB |
| 2560x1440 | 3,686,400 | ~872 MB |
| 3840x2160 | 8,294,400 | ~1.96 GB |

### Optimization Tips

1. **Enable Path Sorting**: Especially beneficial for scenes with multiple material types
2. **Enable Stream Compaction**: Always recommended (minimal overhead)
3. **Adjust Sample Count**: Lower samples for interactive preview, higher for final render
4. **Resolution Scaling**: Start with lower resolution for testing, then scale up

---

## Troubleshooting

### Common Issues

#### Issue: Render returns `-6` (VLRResult_OptiXError)

**Possible Causes**:
1. **Missing instances**: Meshes created but no instances
2. **Invalid geometry**: Degenerate triangles or NaN values
3. **Memory alignment**: CUB buffers not 16-byte aligned (fixed in v1.0)

**Solution**:
- Ensure all meshes have instances: `vlrCreateInstance(scene, mesh, ...)`
- Validate geometry data before passing to API
- Update to latest version (memory alignment fixed)

#### Issue: Low performance

**Possible Causes**:
1. Optimizations disabled
2. Too many samples for resolution
3. Very complex geometry

**Solution**:
- Enable path sorting and stream compaction
- Reduce samples for interactive rendering
- Profile with NVIDIA Nsight

#### Issue: Out of memory

**Possible Causes**:
- Resolution too high for GPU memory
- Too many paths active simultaneously

**Solution**:
- Reduce resolution
- Reduce max path length (default: 8)

---

## Result Codes

| Code | Name | Description |
|------|------|-------------|
| 0 | `VLRResult_Success` | Operation successful |
| -1 | `VLRResult_InvalidArgument` | Invalid parameter |
| -2 | `VLRResult_OutOfMemory` | Memory allocation failed |
| -3 | `VLRResult_NotImplemented` | Feature not implemented |
| -4 | `VLRResult_InternalError` | Internal error |
| -5 | `VLRResult_CUDAError` | CUDA error occurred |
| -6 | `VLRResult_OptiXError` | OptiX error occurred |

---

## Renderer Types

| Value | Name | Description |
|-------|------|-------------|
| 0 | `VLRRenderer_PathTracing` | Recursive path tracing |
| 1 | `VLRRenderer_LightTracing` | Light tracing |
| 2 | `VLRRenderer_BidirectionalPathTracing` | Bidirectional path tracing |
| 3 | `VLRRenderer_WavefrontPathTracing` | **Wavefront path tracing** ⭐ |

---

## API Workflow

```
1. vlrCreateContext()
2. vlrCreateScene()
3. For each material:
   - vlrCreateMaterial()
4. For each mesh:
   - vlrCreateTriangleMesh()
   - vlrCreateInstance()  ← REQUIRED!
5. vlrSetCamera()
6. vlrContextSetWavefrontPathSorting() (optional)
7. vlrContextSetWavefrontStreamCompaction() (optional)
8. vlrRender()
9. vlrGetOutputBuffer()
10. cudaMemcpy() to host
11. Save image
12. vlrDestroyScene()
13. vlrDestroyContext()
```

---

## See Also

- [Implementation Details](WAVEFRONT_IMPLEMENTATION.md)
- [Stage 5 Performance Report](PERFORMANCE_REPORT.md)
- [Stage 6 Test Report](STAGE6_TEST_REPORT.md)
- [Quick Reference](../docs_wavefront/wavefront_quick_reference.md)

---

**Author**: VLR Development Team  
**Last Updated**: 2026-03-07  
**Version**: 1.0
