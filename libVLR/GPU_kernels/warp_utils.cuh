// ============================================================================
// VLR Wavefront - Warp-Level 工具函数
//
// 本文件提供 warp-level 操作的工具函数，用于优化 GPU 性能。
//
// 作者：VLR 开发团队
// 创建日期：2026-03-07
// 环境：CUDA 13.1, OptiX 8.0.0, VS2022
// ============================================================================

#pragma once

#include <cuda_runtime.h>
#include <cstdint>

namespace vlr {
namespace shared {

// ============================================================================
// Warp-Level 常量
// ============================================================================

constexpr uint32_t WARP_SIZE = 32;

// ============================================================================
// Warp-Level 投票函数
// ============================================================================

/// 检查 warp 中是否所有线程的条件都为 true
CUDA_DEVICE_FUNCTION CUDA_INLINE bool warpAll(bool condition) {
    return __all_sync(0xFFFFFFFF, condition);
}

/// 检查 warp 中是否有任何线程的条件为 true
CUDA_DEVICE_FUNCTION CUDA_INLINE bool warpAny(bool condition) {
    return __any_sync(0xFFFFFFFF, condition);
}

/// 统计 warp 中有多少线程的条件为 true
CUDA_DEVICE_FUNCTION CUDA_INLINE uint32_t warpCount(bool condition) {
    return __popc(__ballot_sync(0xFFFFFFFF, condition));
}

// ============================================================================
// Warp-Level Shuffle 操作
// ============================================================================

/// Warp shuffle: 从指定 lane 广播值
template<typename T>
CUDA_DEVICE_FUNCTION CUDA_INLINE T warpBroadcast(T value, uint32_t srcLane) {
    static_assert(sizeof(T) == 4, "warpBroadcast only supports 4-byte types");
    union {
        T val;
        uint32_t u32;
    } tmp;
    tmp.val = value;
    tmp.u32 = __shfl_sync(0xFFFFFFFF, tmp.u32, srcLane);
    return tmp.val;
}

/// Warp shuffle: 从相邻 lane 获取值
template<typename T>
CUDA_DEVICE_FUNCTION CUDA_INLINE T warpShuffleDown(T value, uint32_t delta) {
    static_assert(sizeof(T) == 4, "warpShuffleDown only supports 4-byte types");
    union {
        T val;
        uint32_t u32;
    } tmp;
    tmp.val = value;
    tmp.u32 = __shfl_down_sync(0xFFFFFFFF, tmp.u32, delta);
    return tmp.val;
}

// ============================================================================
// Warp-Level Reduction 操作
// ============================================================================

/// Warp-level sum reduction
CUDA_DEVICE_FUNCTION CUDA_INLINE float warpReduceSum(float value) {
    #pragma unroll
    for (uint32_t offset = WARP_SIZE / 2; offset > 0; offset /= 2) {
        value += __shfl_down_sync(0xFFFFFFFF, value, offset);
    }
    return value;
}

/// Warp-level max reduction
CUDA_DEVICE_FUNCTION CUDA_INLINE float warpReduceMax(float value) {
    #pragma unroll
    for (uint32_t offset = WARP_SIZE / 2; offset > 0; offset /= 2) {
        float other = __shfl_down_sync(0xFFFFFFFF, value, offset);
        value = (value > other) ? value : other;
    }
    return value;
}

/// Warp-level min reduction
CUDA_DEVICE_FUNCTION CUDA_INLINE float warpReduceMin(float value) {
    #pragma unroll
    for (uint32_t offset = WARP_SIZE / 2; offset > 0; offset /= 2) {
        float other = __shfl_down_sync(0xFFFFFFFF, value, offset);
        value = (value < other) ? value : other;
    }
    return value;
}

// ============================================================================
// Block-Level Reduction 操作（使用 warp reduction）
// ============================================================================

/// Block-level sum reduction (使用 shared memory)
template<uint32_t BLOCK_SIZE>
CUDA_DEVICE_FUNCTION CUDA_INLINE float blockReduceSum(float value) {
    __shared__ float warpSums[BLOCK_SIZE / WARP_SIZE];
    
    uint32_t laneId = threadIdx.x % WARP_SIZE;
    uint32_t warpId = threadIdx.x / WARP_SIZE;
    
    // Warp-level reduction
    value = warpReduceSum(value);
    
    // 每个 warp 的第一个线程写入 shared memory
    if (laneId == 0) {
        warpSums[warpId] = value;
    }
    __syncthreads();
    
    // 最后一个 warp 对所有 warp 的结果进行 reduction
    if (warpId == 0) {
        value = (threadIdx.x < (BLOCK_SIZE / WARP_SIZE)) ? warpSums[laneId] : 0.0f;
        value = warpReduceSum(value);
    }
    
    return value;
}

// ============================================================================
// Warp-Level 活跃路径统计
// ============================================================================

/// 统计 warp 中活跃路径数量
CUDA_DEVICE_FUNCTION CUDA_INLINE uint32_t warpCountActivePaths(bool isActive) {
    return __popc(__ballot_sync(0xFFFFFFFF, isActive));
}

/// 检查整个 warp 是否都不活跃（可以提前退出）
CUDA_DEVICE_FUNCTION CUDA_INLINE bool warpAllInactive(bool isActive) {
    return !__any_sync(0xFFFFFFFF, isActive);
}

// ============================================================================
// Warp-Level 材质统计
// ============================================================================

/// 统计 warp 中使用相同材质的线程数
CUDA_DEVICE_FUNCTION CUDA_INLINE uint32_t warpCountSameMaterial(uint32_t materialId) {
    uint32_t firstMaterial = __shfl_sync(0xFFFFFFFF, materialId, 0);
    return __popc(__ballot_sync(0xFFFFFFFF, materialId == firstMaterial));
}

}  // namespace shared
}  // namespace vlr
