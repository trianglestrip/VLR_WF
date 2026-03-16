// ============================================================================
// VLR 波前路径追踪 - 核心共享数据结构
//
// 本文件定义 path_types.h 和 path_types_minimal.h 共用的数据结构。
// 仅依赖 basic_types.h，可安全用于 PTX 编译。
// ============================================================================

#pragma once

#include "vlr/basic_types.h"
#include <cstdint>

namespace vlr {
namespace shared {

// ============================================================================
// 1. WavefrontPathState
// ============================================================================
#ifndef VLR_WAVEFRONT_PATH_STATE_MINIMAL_DEFINED
#define VLR_WAVEFRONT_PATH_STATE_MINIMAL_DEFINED
struct alignas(16) WavefrontPathState {
    // === 光线信息（32 字节）===
    Point3D origin;
    Vector3D direction;
    float _padding1[2];
    
    // === 光谱和吞吐量（64 字节）===
    SampledSpectrum throughput;
    SampledSpectrum contribution;
    WavelengthSamples wls;
    float initImportance;
    float selectWLPDF;
    
    // === 随机数生成器（16 字节）===
    KernelRNG rng;
    
    // === 路径历史（16 字节）===
    float prevDirPDF;
    DirectionType prevSampledType;
    uint32_t pathLength;
    uint32_t _padding2;
    
    // === 像素坐标（8 字节）===
    uint32_t pixelX;
    uint32_t pixelY;
    
    // === 状态标志（8 字节）===
    uint32_t flags;
    uint32_t materialCategory;
    
    // 标志位定义：
    // bit 0: isActive
    // bit 1: isTerminated
    // bit 2: maxLengthReached
    // bit 3: singleWlSelected
    // bit 4: hitEmissive
    
    CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE bool isActive() const {
        return flags & 0x1;
    }
    CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE bool isTerminated() const {
        return flags & 0x2;
    }
    CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE void setActive(bool active) {
        if (active) flags |= 0x1;
        else flags &= ~0x1;
    }
    CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE void setTerminated() {
        flags |= 0x2;
        flags &= ~0x1;
    }
    CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE bool maxLengthReached() const {
        return flags & 0x4;
    }
    CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE void setMaxLengthReached() {
        flags |= 0x4;
    }
    CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE bool singleWlSelected() const {
        return flags & 0x8;
    }
    CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE void setSingleWlSelected() {
        flags |= 0x8;
    }
    CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE bool hitEmissive() const {
        return flags & 0x10;
    }
    CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE void setHitEmissive() {
        flags |= 0x10;
    }
    CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE void reset() {
        flags = 0;
        pathLength = 0;
        materialCategory = 0;
    }
};

#if !defined(__CUDACC__)
static_assert(sizeof(WavefrontPathState) == 144, "PathState size must be 144 bytes");
#endif
#endif

// ============================================================================
// 2. WavefrontHitInfo
// ============================================================================
#ifndef VLR_WAVEFRONT_HIT_INFO_MINIMAL_DEFINED
#define VLR_WAVEFRONT_HIT_INFO_MINIMAL_DEFINED
struct alignas(16) WavefrontHitInfo {
    uint32_t instIndex;
    uint32_t geomInstIndex;
    uint32_t primIndex;
    uint32_t hitFlags;
    float u, v;
    float t;
    float _padding;

    CUDA_DEVICE_FUNCTION CUDA_INLINE bool hasHit() const {
        return hitFlags & 0x1;
    }
    CUDA_DEVICE_FUNCTION CUDA_INLINE bool hitInfinity() const {
        return hitFlags & 0x2;
    }
    CUDA_DEVICE_FUNCTION CUDA_INLINE bool hitEmissive() const {
        return hitFlags & 0x4;
    }
    CUDA_DEVICE_FUNCTION CUDA_INLINE bool hitTransmissive() const {
        return hitFlags & 0x8;
    }
    CUDA_DEVICE_FUNCTION CUDA_INLINE void setHasHit(bool hit) {
        if (hit) hitFlags |= 0x1;
        else hitFlags &= ~0x1;
    }
    CUDA_DEVICE_FUNCTION CUDA_INLINE void setHitInfinity(bool inf) {
        if (inf) hitFlags |= 0x2;
        else hitFlags &= ~0x2;
    }
    CUDA_DEVICE_FUNCTION CUDA_INLINE void setHitEmissive(bool emissive) {
        if (emissive) hitFlags |= 0x4;
        else hitFlags &= ~0x4;
    }
    CUDA_DEVICE_FUNCTION CUDA_INLINE void reset() {
        instIndex = 0xFFFFFFFF;
        geomInstIndex = 0xFFFFFFFF;
        primIndex = 0xFFFFFFFF;
        hitFlags = 0;
        u = v = t = 0.0f;
    }
};

#if !defined(__CUDACC__)
static_assert(sizeof(WavefrontHitInfo) == 32, "HitInfo size must be 32 bytes");
#endif
#endif

// ============================================================================
// 3. WavefrontWorkQueue
// ============================================================================
#ifndef VLR_WAVEFRONT_WORK_QUEUE_MINIMAL_DEFINED
#define VLR_WAVEFRONT_WORK_QUEUE_MINIMAL_DEFINED
struct WavefrontWorkQueue {
    uint32_t* pathIndices;
    uint32_t* counter;
    uint32_t capacity;

    CUDA_DEVICE_FUNCTION CUDA_INLINE uint32_t size() {
        return *counter;
    }
    CUDA_DEVICE_FUNCTION CUDA_INLINE uint32_t enqueue(uint32_t pathIndex) {
        uint32_t slot = atomicAdd(counter, 1u);
        if (slot < capacity) {
            pathIndices[slot] = pathIndex;
            return slot;
        }
        return 0xFFFFFFFF;
    }
    CUDA_DEVICE_FUNCTION CUDA_INLINE uint32_t dequeue() {
#ifdef __CUDACC__
        uint32_t slot = atomicAdd(counter, 0xFFFFFFFFu);
#else
        uint32_t oldVal = *counter;
        *counter = (oldVal > 0) ? (oldVal - 1) : 0;
        uint32_t slot = oldVal;
#endif
        if (slot > 0 && slot <= capacity) {
            return pathIndices[slot - 1];
        }
        return 0xFFFFFFFF;
    }
    CUDA_DEVICE_FUNCTION CUDA_INLINE void reset() {
        *counter = 0;
    }
};
#endif

// ============================================================================
// 4. WavefrontMaterialQueues
// ============================================================================
#ifndef VLR_NUM_MATERIAL_CATEGORIES_DEFINED
#define VLR_NUM_MATERIAL_CATEGORIES_DEFINED
constexpr uint32_t NumMaterialCategories = 6;
#endif

#ifndef VLR_WAVEFRONT_MATERIAL_QUEUES_MINIMAL_DEFINED
#define VLR_WAVEFRONT_MATERIAL_QUEUES_MINIMAL_DEFINED
struct WavefrontMaterialQueues {
    WavefrontWorkQueue queues[NumMaterialCategories];

    CUDA_DEVICE_FUNCTION CUDA_INLINE void enqueueByCategory(
        uint32_t pathIndex, uint32_t category) {
        queues[category].enqueue(pathIndex);
    }
    
    CUDA_DEVICE_FUNCTION CUDA_INLINE void resetAll() {
        for (int i = 0; i < NumMaterialCategories; ++i) {
            queues[i].reset();
        }
    }
};
#endif

// ============================================================================
// 5. WFTracePayload
// ============================================================================
#ifndef VLR_WF_TRACE_PAYLOAD_MINIMAL_DEFINED
#define VLR_WF_TRACE_PAYLOAD_MINIMAL_DEFINED
struct WFTracePayload {
    uint32_t pathIndex;
    WavelengthSamples wls;
};
#endif

// ============================================================================
// 6. LightPathVertex / LightPathState (LVC-BPT)
// ============================================================================
#ifndef VLR_LIGHT_PATH_VERTEX_DEFINED
#define VLR_LIGHT_PATH_VERTEX_DEFINED
struct alignas(16) LightPathVertex {
    Point3D position;
    Normal3D geometricNormal;
    ReferenceFrame shadingFrame;
    SampledSpectrum flux;
    Vector3D dirInLocal;
    uint32_t materialIndex;
    uint32_t flags;
    uint32_t pathLength;
    float _padding;

    CUDA_DEVICE_FUNCTION CUDA_INLINE bool isDeltaSampled() const { return flags & 0x1; }
    CUDA_DEVICE_FUNCTION CUDA_INLINE bool isPrevDeltaSampled() const { return flags & 0x2; }
    CUDA_DEVICE_FUNCTION CUDA_INLINE bool isWlSelected() const { return flags & 0x4; }
    CUDA_DEVICE_FUNCTION CUDA_INLINE bool isPoint() const { return flags & 0x8; }
};

struct alignas(16) LightPathState {
    Point3D origin;
    Vector3D direction;
    SampledSpectrum flux;
    WavelengthSamples wls;
    KernelRNG rng;
    uint32_t pathLength;
    uint32_t flags;

    CUDA_DEVICE_FUNCTION CUDA_INLINE bool isActive() const { return flags & 0x1; }
    CUDA_DEVICE_FUNCTION CUDA_INLINE void setActive(bool a) { if(a) flags|=0x1; else flags&=~0x1; }
    CUDA_DEVICE_FUNCTION CUDA_INLINE void setTerminated() { flags |= 0x2; flags &= ~0x1; }
    CUDA_DEVICE_FUNCTION CUDA_INLINE bool isTerminated() const { return flags & 0x2; }
    CUDA_DEVICE_FUNCTION CUDA_INLINE bool singleWlSelected() const { return flags & 0x4; }
    CUDA_DEVICE_FUNCTION CUDA_INLINE void setSingleWlSelected() { flags |= 0x4; }
};
#endif

// ============================================================================
// 7. ShadowRayRequest
// ============================================================================
#ifndef VLR_SHADOW_RAY_REQUEST_DEFINED
#define VLR_SHADOW_RAY_REQUEST_DEFINED
struct ShadowRayRequest {
    Point3D origin;
    Vector3D direction;
    float tMax;
    uint32_t pathIndex;
    SampledSpectrum contribution;
};
#endif

// ============================================================================
// 8. WavefrontSBTData (forward declared WavefrontLaunchParameters)
// ============================================================================
struct WavefrontLaunchParameters;

#ifndef VLR_WAVEFRONT_SBT_DATA_MINIMAL_DEFINED
#define VLR_WAVEFRONT_SBT_DATA_MINIMAL_DEFINED
struct WavefrontSBTData {
    const WavefrontLaunchParameters* params;
};
#endif

} // namespace shared
} // namespace vlr
