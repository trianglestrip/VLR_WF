// ============================================================================
// VLR Wavefront - Shared Memory 缓存工具
//
// 本文件提供 shared memory 缓存的工具函数，用于优化 GPU 性能。
//
// 作者：VLR 开发团队
// 创建日期：2026-03-07
// 环境：CUDA 13.1, OptiX 8.0.0, VS2022
// ============================================================================

#pragma once

#include "../shared/material_types.h"
#include "../shared/light_types.h"
#include <cuda_runtime.h>

namespace vlr {
namespace shared {

// ============================================================================
// Shared Memory 材质缓存
// ============================================================================

/// 缓存材质描述符到 shared memory
/// 适用于材质数量较少的场景
template<uint32_t MAX_MATERIALS = 16>
struct MaterialCache {
    SurfaceMaterialDescriptor materials[MAX_MATERIALS];
    uint32_t numMaterials;
    
    /// 初始化缓存（由 block 的第一个线程调用）
    CUDA_DEVICE_FUNCTION CUDA_INLINE void initialize(
        const SurfaceMaterialDescriptor* globalMaterials,
        uint32_t count) {
        
        if (threadIdx.x == 0) {
            numMaterials = (count < MAX_MATERIALS) ? count : MAX_MATERIALS;
            for (uint32_t i = 0; i < numMaterials; ++i) {
                materials[i] = globalMaterials[i];
            }
        }
        __syncthreads();
    }
    
    /// 获取材质（如果在缓存中）
    CUDA_DEVICE_FUNCTION CUDA_INLINE const SurfaceMaterialDescriptor* get(
        uint32_t materialIndex,
        const SurfaceMaterialDescriptor* globalMaterials) const {
        
        if (materialIndex < numMaterials) {
            return &materials[materialIndex];
        }
        return &globalMaterials[materialIndex];
    }
};

// ============================================================================
// Shared Memory 光源缓存
// ============================================================================

/// 缓存光源信息到 shared memory
template<uint32_t MAX_LIGHTS = 8>
struct LightCache {
    GeometryInstance lights[MAX_LIGHTS];
    uint32_t numLights;
    
    /// 初始化缓存（由 block 的第一个线程调用）
    CUDA_DEVICE_FUNCTION CUDA_INLINE void initialize(
        const GeometryInstance* globalLights,
        uint32_t count) {
        
        if (threadIdx.x == 0) {
            numLights = (count < MAX_LIGHTS) ? count : MAX_LIGHTS;
            for (uint32_t i = 0; i < numLights; ++i) {
                lights[i] = globalLights[i];
            }
        }
        __syncthreads();
    }
    
    /// 获取光源（如果在缓存中）
    CUDA_DEVICE_FUNCTION CUDA_INLINE const GeometryInstance* get(
        uint32_t lightIndex,
        const GeometryInstance* globalLights) const {
        
        if (lightIndex < numLights) {
            return &lights[lightIndex];
        }
        return &globalLights[lightIndex];
    }
};

// ============================================================================
// Warp-Level 协作加载
// ============================================================================

/// 使用 warp 协作加载数据到 shared memory
/// 每个线程加载一部分数据，提高加载效率
template<typename T>
CUDA_DEVICE_FUNCTION CUDA_INLINE void warpCooperativeLoad(
    T* sharedDest,
    const T* globalSrc,
    uint32_t count) {
    
    uint32_t laneId = threadIdx.x % 32;
    uint32_t numWarps = blockDim.x / 32;
    uint32_t warpId = threadIdx.x / 32;
    
    // 每个 warp 加载一部分数据
    for (uint32_t i = warpId * 32 + laneId; i < count; i += numWarps * 32) {
        sharedDest[i] = globalSrc[i];
    }
    __syncthreads();
}

}  // namespace shared
}  // namespace vlr
