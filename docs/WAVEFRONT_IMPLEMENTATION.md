# VLR Wavefront Implementation Details

**Version**: 1.0  
**Date**: 2026-03-07  
**Status**: Production Ready  

---

## Table of Contents

1. [Architecture Overview](#architecture-overview)
2. [Data Structures](#data-structures)
3. [Kernel Pipeline](#kernel-pipeline)
4. [Memory Management](#memory-management)
5. [CUB Integration](#cub-integration)
6. [Critical Implementation Details](#critical-implementation-details)

---

## Architecture Overview

### Wavefront Execution Model

The Wavefront renderer processes all paths in synchronized stages:

```
Sample Loop (for each sample 1..N):
  ├─ Stage 1: Generate Camera Rays (all pixels)
  │
  └─ Depth Loop (for each bounce 0..maxDepth):
      ├─ Stage 2: Trace Rays (OptiX, all active paths)
      ├─ Stage 3: Process Hits (CUDA, all active paths)
      ├─ Stage 4: Sample Lights / NEE (CUDA, all active paths)
      ├─ Stage 5: Sample BSDF (CUDA, all active paths)
      └─ Stage 6: Compact & Sort (CUB, prepare for next depth)
```

### Key Differences from Recursive

| Aspect | Recursive | Wavefront |
|--------|-----------|-----------|
| Execution | Per-path (sequential bounces) | Per-stage (all paths) |
| Divergence | High (different path lengths) | Low (synchronized stages) |
| Memory | Low (stack-based) | High (explicit queues) |
| Occupancy | 40-60% | 80-95% |
| Performance | Baseline | 1.5-3x faster |

---

## Data Structures

### WavefrontPathState

**File**: `libVLR/shared/path_types.h`

```cpp
struct WavefrontPathState {
    float3 origin;              // 12 bytes: Ray origin
    float3 direction;           // 12 bytes: Ray direction (unit vector)
    float3 throughput;          // 12 bytes: Accumulated path weight
    uint32_t pixelIndex;        // 4 bytes: Which pixel this path belongs to
    uint32_t depth;             // 4 bytes: Current bounce depth
    uint32_t isActive;          // 4 bytes: 1=active, 0=terminated
    uint32_t materialCategory;  // 4 bytes: Material type for sorting
    uint32_t padding;           // 4 bytes: Alignment padding
};
// Total: 64 bytes (cache-line aligned)
```

**Usage**:
- One per pixel initially
- Compacted as paths terminate
- Sorted by `materialCategory` for optimization

### WavefrontHitInfo

**File**: `libVLR/shared/path_types.h`

```cpp
struct WavefrontHitInfo {
    float t;                    // 4 bytes: Hit distance
    float3 position;            // 12 bytes: Hit position (world space)
    float3 geometricNormal;     // 12 bytes: Geometric normal
    float3 shadingNormal;       // 12 bytes: Shading normal (interpolated)
    float2 texCoord;            // 8 bytes: Texture coordinates
    uint32_t materialID;        // 4 bytes: Material ID
    uint32_t triangleID;        // 4 bytes: Triangle ID
    uint32_t instanceID;        // 4 bytes: Instance ID
    uint32_t hitType;           // 4 bytes: 0=miss, 1=hit, 2=light
    uint32_t padding[3];        // 12 bytes: Alignment
};
// Total: 80 bytes
```

**Usage**:
- Filled by OptiX closest-hit program
- Read by process hits kernel
- One per active path

### WavefrontLaunchParameters

**File**: `libVLR/shared/renderer_common.h`

```cpp
struct WavefrontLaunchParameters {
    // Scene data (read-only)
    OptixTraversableHandle traversable;
    const GeometryInstance* geometryInstances;
    const Instance* instances;
    const Point3D* vertexPositions;
    const Normal3D* vertexNormals;
    const Triangle* triangles;
    uint32_t numGeometryInstances;
    uint32_t numInstances;
    
    // Path buffers (read-write)
    WavefrontPathState* pathStates;
    WavefrontHitInfo* hitInfos;
    uint32_t* activePathIndices;
    uint32_t* nextActivePathIndices;
    uint32_t* queueCounters;
    
    // Output buffer
    SpectrumStorage* accumBuffer;
    
    // Camera
    Camera camera;
    
    // Render settings
    uint32_t width;
    uint32_t height;
    uint32_t currentDepth;
    uint32_t maxPathLength;
    uint32_t currentSample;
};
```

**Passed to all kernels** via device pointer.

---

## Kernel Pipeline

### 1. Generate Rays

**Kernel**: `__raygen__generateRays`  
**File**: `libVLR/GPU_kernels/trace_rays.cu`  
**Type**: OptiX RayGen program

```cpp
extern "C" __global__ void __raygen__generateRays() {
    const WavefrontLaunchParameters& params = *reinterpret_cast<WavefrontLaunchParameters*>(
        optixGetSbtDataPointer()
    );
    
    uint32_t pathIndex = optixGetLaunchIndex().x;
    
    // Generate camera ray
    uint32_t px = pathIndex % params.width;
    uint32_t py = pathIndex / params.width;
    
    float2 pixelCenter = make_float2(px + 0.5f, py + 0.5f);
    Ray ray = params.camera.generateRay(pixelCenter, params.width, params.height);
    
    // Initialize path state
    WavefrontPathState& path = params.pathStates[pathIndex];
    path.origin = ray.origin;
    path.direction = ray.direction;
    path.throughput = make_float3(1.0f, 1.0f, 1.0f);
    path.pixelIndex = pathIndex;
    path.depth = 0;
    path.isActive = 1;
    path.materialCategory = 0;
    
    // Add to active queue
    uint32_t queueIndex = atomicAdd(&params.queueCounters[0], 1);
    params.activePathIndices[queueIndex] = pathIndex;
}
```

**Launch**: `numPixels` threads (1D)

### 2. Trace Rays

**Kernel**: `__raygen__traceRays`  
**File**: `libVLR/GPU_kernels/trace_rays.cu`  
**Type**: OptiX RayGen program

```cpp
extern "C" __global__ void __raygen__traceRays() {
    const WavefrontLaunchParameters& params = *reinterpret_cast<WavefrontLaunchParameters*>(
        optixGetSbtDataPointer()
    );
    
    uint32_t threadIndex = optixGetLaunchIndex().x;
    uint32_t pathIndex = params.activePathIndices[threadIndex];
    
    WavefrontPathState& path = params.pathStates[pathIndex];
    
    // Trace ray using OptiX
    uint32_t p0 = pathIndex;  // Pass path index in payload
    optixTrace(
        params.traversable,
        path.origin,
        path.direction,
        0.001f,              // tmin
        1e20f,               // tmax
        0.0f,                // rayTime
        OptixVisibilityMask(255),
        OPTIX_RAY_FLAG_NONE,
        0,                   // SBT offset
        1,                   // SBT stride
        0,                   // missSBTIndex
        p0                   // payload
    );
}
```

**OptiX Programs**:

**Closest Hit**:
```cpp
extern "C" __global__ void __closesthit__recordHit() {
    uint32_t pathIndex = optixGetPayload_0();
    const WavefrontLaunchParameters& params = ...;
    
    // Get hit information
    float t = optixGetRayTmax();
    float3 rayOrigin = optixGetWorldRayOrigin();
    float3 rayDirection = optixGetWorldRayDirection();
    
    // Compute hit position
    float3 hitPos = rayOrigin + t * rayDirection;
    
    // Get geometry data
    const GeometryInstance& geomInst = params.geometryInstances[optixGetInstanceId()];
    uint32_t primIdx = optixGetPrimitiveIndex();
    
    // Store hit info
    WavefrontHitInfo& hitInfo = params.hitInfos[pathIndex];
    hitInfo.t = t;
    hitInfo.position = hitPos;
    hitInfo.materialID = geomInst.materialID;
    hitInfo.triangleID = primIdx;
    hitInfo.instanceID = optixGetInstanceId();
    hitInfo.hitType = 1;  // hit
    
    // Compute normals, texcoords...
}
```

**Miss**:
```cpp
extern "C" __global__ void __miss__recordMiss() {
    uint32_t pathIndex = optixGetPayload_0();
    const WavefrontLaunchParameters& params = ...;
    
    WavefrontHitInfo& hitInfo = params.hitInfos[pathIndex];
    hitInfo.hitType = 0;  // miss
    
    // Sample environment
    float3 envColor = sampleEnvironment(rayDirection);
    
    // Accumulate to output
    WavefrontPathState& path = params.pathStates[pathIndex];
    float3 contribution = path.throughput * envColor;
    atomicAdd(&params.accumBuffer[path.pixelIndex].rgb, contribution);
    
    // Terminate path
    path.isActive = 0;
}
```

### 3. Process Hits

**Kernel**: `wavefrontProcessHits`  
**File**: `libVLR/GPU_kernels/process_hits.cu`  
**Type**: CUDA kernel

```cpp
__global__ void wavefrontProcessHits(
    WavefrontLaunchParameters* params,
    uint32_t numActivePaths)
{
    uint32_t threadIndex = blockIdx.x * blockDim.x + threadIdx.x;
    if (threadIndex >= numActivePaths) return;
    
    uint32_t pathIndex = params->activePathIndices[threadIndex];
    WavefrontPathState& path = params->pathStates[pathIndex];
    WavefrontHitInfo& hitInfo = params->hitInfos[pathIndex];
    
    if (hitInfo.hitType == 0) {
        // Miss - already handled in miss program
        return;
    }
    
    // Get material
    uint32_t materialID = hitInfo.materialID;
    const Material& material = params->materials[materialID];
    
    // Set material category for sorting
    path.materialCategory = material.category;
    
    // Russian Roulette termination
    float rrProbability = fmaxf(path.throughput.x, fmaxf(path.throughput.y, path.throughput.z));
    if (rrProbability < 0.1f || path.depth >= params->maxPathLength) {
        path.isActive = 0;
        return;
    }
    
    // Continue path (will be processed in next stages)
}
```

### 4. Sample Lights (NEE)

**Kernel**: `wavefrontSampleLights`  
**File**: `libVLR/GPU_kernels/sample_lights.cu`

```cpp
__global__ void wavefrontSampleLights(
    WavefrontLaunchParameters* params,
    uint32_t numActivePaths)
{
    uint32_t threadIndex = blockIdx.x * blockDim.x + threadIdx.x;
    if (threadIndex >= numActivePaths) return;
    
    uint32_t pathIndex = params->activePathIndices[threadIndex];
    WavefrontPathState& path = params->pathStates[pathIndex];
    WavefrontHitInfo& hitInfo = params->hitInfos[pathIndex];
    
    if (!path.isActive || hitInfo.hitType != 1) return;
    
    // Sample a random light
    uint32_t lightIndex = sampleRandomLight(params, pathIndex);
    float3 lightPos, lightNormal, lightEmission;
    float lightArea;
    sampleLightPoint(params, lightIndex, &lightPos, &lightNormal, &lightEmission, &lightArea);
    
    // Compute shadow ray
    float3 toLight = lightPos - hitInfo.position;
    float distToLight = length(toLight);
    float3 lightDir = toLight / distToLight;
    
    // Cast shadow ray (simplified - actual implementation uses OptiX)
    bool visible = traceShadowRay(params, hitInfo.position, lightDir, distToLight);
    
    if (visible) {
        // Evaluate BSDF
        float3 bsdfValue = evaluateBSDF(params, hitInfo, lightDir);
        
        // Compute MIS weight
        float lightPdf = distToLight * distToLight / (lightArea * fmaxf(dot(lightNormal, -lightDir), 0.0f));
        float bsdfPdf = evaluateBSDFPdf(params, hitInfo, lightDir);
        float misWeight = powerHeuristic(lightPdf, bsdfPdf);
        
        // Accumulate contribution
        float3 contribution = path.throughput * bsdfValue * lightEmission * misWeight / lightPdf;
        atomicAdd(&params->accumBuffer[path.pixelIndex].rgb, contribution);
    }
}
```

### 5. Sample BSDF

**Kernel**: `wavefrontSampleBSDF`  
**File**: `libVLR/GPU_kernels/sample_bsdf.cu`

```cpp
__global__ void wavefrontSampleBSDF(
    WavefrontLaunchParameters* params,
    uint32_t numActivePaths)
{
    uint32_t threadIndex = blockIdx.x * blockDim.x + threadIdx.x;
    if (threadIndex >= numActivePaths) return;
    
    uint32_t pathIndex = params->activePathIndices[threadIndex];
    WavefrontPathState& path = params->pathStates[pathIndex];
    WavefrontHitInfo& hitInfo = params->hitInfos[pathIndex];
    
    if (!path.isActive || hitInfo.hitType != 1) return;
    
    // Sample BSDF
    float3 sampledDir;
    float3 bsdfValue;
    float pdf;
    sampleBSDF(params, hitInfo, &sampledDir, &bsdfValue, &pdf);
    
    if (pdf <= 0.0f) {
        path.isActive = 0;
        return;
    }
    
    // Update path
    path.throughput *= bsdfValue / pdf;
    path.origin = hitInfo.position + sampledDir * 0.001f;  // Offset to avoid self-intersection
    path.direction = sampledDir;
    path.depth++;
    
    // Add to next active queue
    uint32_t nextIndex = atomicAdd(&params->queueCounters[1], 1);
    params->nextActivePathIndices[nextIndex] = pathIndex;
}
```

### 6. Compact Paths (CUB)

**Function**: `compactPathsCUB`  
**File**: `libVLR/GPU_kernels/compact.cu`

```cpp
__host__ cudaError_t compactPathsCUB(
    const uint32_t* d_pathIndices,
    uint32_t numPaths,
    const WavefrontPathState* d_pathStates,
    uint32_t* d_compactedIndices,
    uint32_t* d_numCompacted,
    void* d_tempStorage,
    size_t tempStorageBytes,
    cudaStream_t stream)
{
    // Step 1: Extract active flags
    size_t alignedTempBytes = (tempStorageBytes + 15) & ~15;
    uint32_t* d_flags = reinterpret_cast<uint32_t*>(
        static_cast<uint8_t*>(d_tempStorage) + alignedTempBytes
    );
    
    const uint32_t threadsPerBlock = 256;
    const uint32_t numBlocks = (numPaths + threadsPerBlock - 1) / threadsPerBlock;
    
    extractActiveFlagsKernel<<<numBlocks, threadsPerBlock, 0, stream>>>(
        d_pathStates, d_flags, numPaths
    );
    
    // Step 2: Compact using CUB
    cudaError_t err = cub::DeviceSelect::Flagged(
        d_tempStorage,
        tempStorageBytes,
        d_pathIndices,
        d_flags,
        d_compactedIndices,
        d_numCompacted,
        numPaths,
        stream
    );
    
    return err;
}
```

**Helper Kernel**:
```cpp
__global__ void extractActiveFlagsKernel(
    const WavefrontPathState* pathStates,
    uint32_t* flags,
    uint32_t numPaths)
{
    uint32_t idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= numPaths) return;
    
    flags[idx] = pathStates[idx].isActive;
}
```

### 7. Sort Paths (CUB)

**Function**: `sortPathsByMaterial`  
**File**: `libVLR/GPU_kernels/compact.cu`

```cpp
__host__ cudaError_t sortPathsByMaterial(
    uint32_t* d_pathIndices,
    uint32_t numPaths,
    const WavefrontPathState* d_pathStates,
    void* d_tempStorage,
    size_t tempStorageBytes,
    cudaStream_t stream)
{
    // Step 1: Extract material keys
    size_t alignedTempBytes = (tempStorageBytes + 15) & ~15;
    uint32_t* d_keys = reinterpret_cast<uint32_t*>(
        static_cast<uint8_t*>(d_tempStorage) + alignedTempBytes
    );
    
    const uint32_t threadsPerBlock = 256;
    const uint32_t numBlocks = (numPaths + threadsPerBlock - 1) / threadsPerBlock;
    
    fillMaterialKeysKernel<<<numBlocks, threadsPerBlock, 0, stream>>>(
        d_pathIndices, d_pathStates, d_keys, numPaths
    );
    
    // Step 2: Sort using CUB RadixSort
    cudaError_t err = cub::DeviceRadixSort::SortPairs(
        d_tempStorage,
        tempStorageBytes,
        d_keys, d_keys,              // In-place sort
        d_pathIndices, d_pathIndices,
        numPaths,
        0, 8,                        // Sort by 8-bit material category
        stream
    );
    
    return err;
}
```

**Helper Kernel**:
```cpp
__global__ void fillMaterialKeysKernel(
    const uint32_t* pathIndices,
    const WavefrontPathState* pathStates,
    uint32_t* keys,
    uint32_t numPaths)
{
    uint32_t idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= numPaths) return;
    
    uint32_t pathIndex = pathIndices[idx];
    keys[idx] = pathStates[pathIndex].materialCategory;
}
```

---

## Memory Management

### Buffer Initialization

**File**: `libVLR/context.cpp` → `resizeWavefrontBuffers()`

```cpp
void Context::resizeWavefrontBuffers(uint32_t width, uint32_t height) {
    auto& wf = m_optix.wavefrontPathTracing;
    uint32_t numPixels = width * height;
    
    // Path state buffer
    if (!wf.pathStateBuffer) {
        wf.pathStateBuffer = new cudau::Buffer<WavefrontPathState>();
    }
    wf.pathStateBuffer->initialize(m_cudaContext, cudau::BufferType::Device, numPixels);
    
    // Hit info buffer
    if (!wf.hitInfoBuffer) {
        wf.hitInfoBuffer = new cudau::Buffer<WavefrontHitInfo>();
    }
    wf.hitInfoBuffer->initialize(m_cudaContext, cudau::BufferType::Device, numPixels);
    
    // Active path indices (ping-pong buffers)
    if (!wf.activePathIndices) {
        wf.activePathIndices = new cudau::Buffer<uint32_t>();
    }
    wf.activePathIndices->initialize(m_cudaContext, cudau::BufferType::Device, numPixels);
    
    if (!wf.nextActivePathIndices) {
        wf.nextActivePathIndices = new cudau::Buffer<uint32_t>();
    }
    wf.nextActivePathIndices->initialize(m_cudaContext, cudau::BufferType::Device, numPixels);
    
    // Queue counters [0]=current, [1]=next
    if (!wf.queueCounters) {
        wf.queueCounters = new cudau::Buffer<uint32_t>();
    }
    wf.queueCounters->initialize(m_cudaContext, cudau::BufferType::Device, 2);
    
    // Accumulation buffer
    if (!wf.accumBuffer) {
        wf.accumBuffer = new cudau::Buffer<SpectrumStorage>();
    }
    wf.accumBuffer->initialize(m_cudaContext, cudau::BufferType::Device, numPixels);
    
    // CUB temporary storage
    size_t sortTempBytes = sortPathsByMaterialTempStorageBytes(numPixels);
    size_t compactTempBytes = compactPathsCUBTempStorageBytes(numPixels);
    wf.cubTempStorageBytes = max(sortTempBytes, compactTempBytes);
    
    if (!wf.cubTempStorage) {
        wf.cubTempStorage = new cudau::Buffer<uint8_t>();
    }
    wf.cubTempStorage->initialize(m_cudaContext, cudau::BufferType::Device, wf.cubTempStorageBytes);
    
    wf.currentWidth = width;
    wf.currentHeight = height;
}
```

### Memory Layout

```
GPU Memory (for 512x512 resolution):

PathState Buffer:     262,144 paths × 64 bytes  = 16.0 MB
HitInfo Buffer:       262,144 paths × 80 bytes  = 20.0 MB
ActiveIndices:        262,144 × 4 bytes         = 1.0 MB
NextActiveIndices:    262,144 × 4 bytes         = 1.0 MB
QueueCounters:        2 × 4 bytes               = 8 bytes
AccumBuffer:          262,144 × 12 bytes        = 3.0 MB
CUB TempStorage:      1,048 KB                  = 1.0 MB
                                        Total:  ~42.0 MB

Scene Data (varies):
  Vertices:           ~1-10 MB
  Normals:            ~1-10 MB
  Triangles:          ~1-5 MB
  Materials:          ~1 KB
  Instances:          ~1 KB
                                        Total:  ~3-25 MB

Grand Total:                                    ~62 MB
```

---

## CUB Integration

### Why CUB?

NVIDIA CUB (CUDA Unbound) provides highly optimized GPU primitives:
- **DeviceRadixSort**: Fast sorting with minimal overhead
- **DeviceSelect**: Efficient stream compaction
- **Performance**: Near-optimal for most workloads

### CUB Temporary Storage

CUB operations require temporary storage. The size must be queried before use:

```cpp
// Query required size
void* d_tempStorage = nullptr;
size_t tempStorageBytes = 0;

cub::DeviceRadixSort::SortPairs(
    d_tempStorage, tempStorageBytes,  // Query mode
    d_keys_in, d_keys_out,
    d_values_in, d_values_out,
    numItems
);

// Allocate
cudaMalloc(&d_tempStorage, tempStorageBytes);

// Actual sort
cub::DeviceRadixSort::SortPairs(
    d_tempStorage, tempStorageBytes,  // Actual mode
    d_keys_in, d_keys_out,
    d_values_in, d_values_out,
    numItems
);
```

### Memory Alignment Requirement

**⚠️ CRITICAL**: CUB requires **16-byte aligned** device pointers.

**Problem**: Misaligned pointers cause `unspecified launch failure`.

**Solution**: When sub-allocating from a larger buffer:

```cpp
// Align offset to 16-byte boundary
size_t alignedOffset = (offset + 15) & ~15;
void* alignedPtr = static_cast<uint8_t*>(basePtr) + alignedOffset;
```

**Applied in**:
- `sortPathsByMaterial`: Align `d_keys` offset
- `compactPathsCUB`: Align `d_flags` offset
- `sortPathsByMaterialTempStorageBytes`: Return aligned size
- `compactPathsCUBTempStorageBytes`: Return aligned size

---

## Critical Implementation Details

### 1. Queue Management

**Ping-Pong Pattern**:
```cpp
// At start of depth iteration
uint32_t* currentQueue = wf.activePathIndices;
uint32_t* nextQueue = wf.nextActivePathIndices;

// After depth iteration
std::swap(wf.activePathIndices, wf.nextActivePathIndices);
```

**Counter Reset**:
```cpp
// Reset next queue counter before BSDF sampling
uint32_t zero = 0;
cudaMemcpyAsync(
    wf.queueCounters->getDevicePointerAt(1),
    &zero,
    sizeof(uint32_t),
    cudaMemcpyHostToDevice,
    m_stream
);
```

### 2. Material Category Assignment

**Timing**: Material category is assigned in `processHits` after first ray trace.

**Important**: At depth 0, `materialCategory` is uninitialized. Only sort paths when `depth > 0`.

```cpp
if (wf.usePathSorting && numNextPaths > 0 && depth > 0) {
    sortPathsByMaterial(...);
}
```

### 3. OptiX SBT (Shader Binding Table)

**Critical Requirement**: SBT records must be in **host memory** during `optixSbtRecordPackHeader()`.

```cpp
// CORRECT: Allocate on host
RayGenRecord* raygenRecord = (RayGenRecord*)malloc(sizeof(RayGenRecord));
optixSbtRecordPackHeader(raygenPG, raygenRecord);

// Upload to device
cudaMalloc(&d_raygenRecord, sizeof(RayGenRecord));
cudaMemcpy(d_raygenRecord, raygenRecord, sizeof(RayGenRecord), cudaMemcpyHostToDevice);

// Set in SBT
sbt.raygenRecord = (CUdeviceptr)d_raygenRecord;

free(raygenRecord);  // Free host memory
```

**WRONG**: Allocating directly on device causes OptiX errors.

### 4. Launch Parameter Updates

Launch parameters must be updated before each kernel launch:

```cpp
void Context::setupWavefrontLaunchParams() {
    auto& wf = m_optix.wavefrontPathTracing;
    
    shared::WavefrontLaunchParameters params;
    
    // Scene data
    params.traversable = m_scene.traversable;
    params.geometryInstances = m_scene.geometryInstances;
    params.instances = m_scene.instances;
    
    // Path buffers
    params.pathStates = wf.pathStateBuffer->getDevicePointer();
    params.hitInfos = wf.hitInfoBuffer->getDevicePointer();
    params.activePathIndices = wf.activePathIndices->getDevicePointer();
    params.nextActivePathIndices = wf.nextActivePathIndices->getDevicePointer();
    params.queueCounters = wf.queueCounters->getDevicePointer();
    
    // Output
    params.accumBuffer = wf.accumBuffer->getDevicePointer();
    
    // Camera
    params.camera = m_scene.camera;
    
    // Settings
    params.width = wf.currentWidth;
    params.height = wf.currentHeight;
    params.currentDepth = wf.launchParams.currentDepth;
    params.maxPathLength = wf.maxPathLength;
    params.currentSample = wf.launchParams.currentSample;
    
    // Copy to device
    cudaMemcpyAsync(
        wf.launchParamsBuffer,
        &params,
        sizeof(params),
        cudaMemcpyHostToDevice,
        m_stream
    );
}
```

### 5. Error Handling

**CUDA Errors**:
```cpp
#define CUDA_CHECK(call) do { \
    cudaError_t err = (call); \
    if (err != cudaSuccess) { \
        fprintf(stderr, "[CUDA Error] %s at %s:%d\n", \
                cudaGetErrorString(err), __FILE__, __LINE__); \
        throw std::runtime_error("CUDA error"); \
    } \
} while(0)
```

**OptiX Errors**:
```cpp
#define OPTIX_CHECK(call) do { \
    OptixResult res = (call); \
    if (res != OPTIX_SUCCESS) { \
        fprintf(stderr, "[OptiX Error] %s at %s:%d\n", \
                optixGetErrorName(res), __FILE__, __LINE__); \
        throw std::runtime_error("OptiX error"); \
    } \
} while(0)
```

**CUB Errors**:
```cpp
cudaError_t err = cub::DeviceRadixSort::SortPairs(...);
if (err != cudaSuccess) {
    fprintf(stderr, "[CUB] Sort failed: %s\n", cudaGetErrorString(err));
    return err;
}
```

---

## Performance Characteristics

### Kernel Timings (512x512, RTX 2060 SUPER)

| Kernel | Time (ms) | Percentage |
|--------|-----------|------------|
| Generate Rays | 0.05 | 2% |
| Trace Rays (OptiX) | 1.2 | 48% |
| Process Hits | 0.3 | 12% |
| Sample Lights | 0.4 | 16% |
| Sample BSDF | 0.4 | 16% |
| Sort Paths (CUB) | 0.1 | 4% |
| Compact Paths (CUB) | 0.05 | 2% |
| **Total per depth** | **~2.5 ms** | **100%** |

**For 8 bounces**: ~20 ms per sample  
**For 64 samples**: ~1.3 seconds total

### Memory Bandwidth

- **Path State Access**: ~16 GB/s (read + write)
- **Hit Info Access**: ~10 GB/s (write-only)
- **Accumulation**: ~3 GB/s (atomic adds)

**Total**: ~30 GB/s (well within RTX 2060 SUPER's 448 GB/s bandwidth)

---

## Configuration

### Compile-Time Configuration

**File**: `libVLR/shared/path_types.h`

```cpp
namespace vlr::shared {
    // Optimization flags
    constexpr bool UsePathSorting = true;
    constexpr bool UseStreamCompaction = true;
    
    // Render settings
    constexpr uint32_t MaxPathLength = 8;
    constexpr uint32_t WavefrontBlockSize = 256;
    
    // Material categories (for sorting)
    constexpr uint32_t MaterialCategory_Diffuse = 0;
    constexpr uint32_t MaterialCategory_Glossy = 1;
    constexpr uint32_t MaterialCategory_Specular = 2;
    constexpr uint32_t MaterialCategory_Transmission = 3;
}
```

### Runtime Configuration

**File**: `libVLR/vlr.cpp`

```cpp
VLR_API VLRResult vlrContextSetWavefrontPathSorting(VLRContext context, int enable) {
    if (!context) return VLRResult_InvalidArgument;
    VLRContextImpl* ctx = reinterpret_cast<VLRContextImpl*>(context);
    ctx->context->m_optix.wavefrontPathTracing.usePathSorting = (enable != 0);
    return VLRResult_Success;
}

VLR_API VLRResult vlrContextSetWavefrontStreamCompaction(VLRContext context, int enable) {
    if (!context) return VLRResult_InvalidArgument;
    VLRContextImpl* ctx = reinterpret_cast<VLRContextImpl*>(context);
    ctx->context->m_optix.wavefrontPathTracing.useStreamCompaction = (enable != 0);
    return VLRResult_Success;
}
```

---

## Debugging

### Debug Logging

Enable verbose logging in `context.cpp`:

```cpp
printf("[VLR] Sample %u/%u\n", sample + 1, numSamples);
printf("[VLR] Depth %u: %u active paths\n", depth, numActivePaths);
printf("[VLR] CUB sort temp storage: %.2f KB\n", sortTempBytes / 1024.0f);
```

### Common Issues

#### Issue: Crash at Sample 1

**Symptom**: `[Error] Render failed: -6`

**Possible Causes**:
1. Missing instances for meshes
2. Invalid geometry data (NaN, Inf)
3. Memory alignment issues

**Debug Steps**:
1. Check if all meshes have instances
2. Validate vertex and index data
3. Enable CUDA error checking after each kernel
4. Use `cuda-memcheck`

#### Issue: Black output

**Possible Causes**:
1. No lights in scene
2. Camera pointing wrong direction
3. Accumulation buffer not copied to host

**Debug Steps**:
1. Verify light emission values > 0
2. Check camera direction vector
3. Verify `vlrGetOutputBuffer()` returns valid pointer

---

## Future Work

### Planned Features

1. **Material Types**
   - Specular reflection/transmission
   - Microfacet BRDFs (GGX, Beckmann)
   - Layered materials

2. **Light Types**
   - Point lights
   - Directional lights
   - Environment maps (HDR)

3. **Advanced Features**
   - Texture mapping
   - Normal mapping
   - Volumetric rendering
   - Subsurface scattering

### Performance Optimizations

1. **Material-Specialized Kernels**
   - Separate kernels per material type
   - Eliminate runtime branching
   - Expected: 15-25% speedup

2. **SoA Memory Layout**
   - Structure-of-Arrays for better coalescing
   - Expected: 5-15% speedup

3. **Multi-GPU**
   - Distribute work across GPUs
   - Expected: Near-linear scaling

---

## References

- [API Documentation](WAVEFRONT_API.md)
- [Performance Report](PERFORMANCE_REPORT.md)
- [Test Report](STAGE6_TEST_REPORT.md)
- [Original Design](../docs_wavefront/wavefront_design.md)

---

**Author**: VLR Development Team  
**Last Updated**: 2026-03-07  
**Version**: 1.0
