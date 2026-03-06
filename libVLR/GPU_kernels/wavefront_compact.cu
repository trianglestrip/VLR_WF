// ============================================================================
// VLR Wavefront - Compact Paths Kernel
//
// 实现 Wavefront 路径压缩的三种方式：
// 1. 简单版本：队列交换（与 Context 中实现一致）
// 2. CUB Stream Compaction：使用 DeviceSelect::Flagged 筛选活跃路径
// 3. 按材质排序（可选）：使用 DeviceRadixSort::SortKeys 减少分支发散
//
// 参考：
// - CUB 文档：https://nvlabs.github.io/cub/
// - docs/wavefront_design.md - Compact 设计
//
// 作者：VLR 开发团队
// 创建日期：2026-03-07
// 环境：CUDA 13.1, OptiX 8.0.0, VS2022
// ============================================================================

#include "wavefront_compact.h"
#include "../shared/wavefront_types.h"
#include "../shared/material_types.h"

#include <cuda_runtime.h>
#include <cub/cub.cuh>

#include <algorithm>
#include <cstddef>

namespace vlr {
namespace shared {

// ============================================================================
// 内部辅助：队列计数器同步 Kernel
// ============================================================================

/// 同步队列计数器：将下一队列计数复制到当前活跃，并重置下一队列
/// counters[0] = 当前活跃路径数, counters[1] = 下一轮路径数
/// 交换后：counters[0] <- counters[1], counters[1] <- 0
__global__ void compactSyncCountersKernel(uint32_t* counters) {
    if (threadIdx.x == 0 && blockIdx.x == 0) {
        counters[0] = counters[1];
        counters[1] = 0;
    }
}


// ============================================================================
// compactPathsSimple - 简单队列交换
// ============================================================================

void compactPathsSimple(
    void** activePathIndicesPtr,
    void** nextPathIndicesPtr,
    uint32_t* d_queueCounters,
    cudaStream_t stream)
{
    if (!activePathIndicesPtr || !nextPathIndicesPtr || !d_queueCounters)
        return;

    // 1. 交换队列缓冲区指针（主机端）
    std::swap(*activePathIndicesPtr, *nextPathIndicesPtr);

    // 2. 同步计数器：将下一队列计数复制到活跃位置，重置下一队列
    compactSyncCountersKernel<<<1, 1, 0, stream>>>(d_queueCounters);
}


// ============================================================================
// CUB Stream Compaction 辅助：填充活跃标志
// ============================================================================

/// 为每条路径填充 isActive 标志，供 DeviceSelect::Flagged 使用
/// pathStateBuffer 需为 WavefrontPathState* 类型
__global__ void fillActiveFlagsKernel(
    const uint32_t* __restrict__ pathIndices,
    const WavefrontPathState* __restrict__ pathStates,
    uint8_t* __restrict__ flags,
    uint32_t numPaths)
{
    uint32_t idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= numPaths)
        return;

    uint32_t pathIndex = pathIndices[idx];
    flags[idx] = pathStates[pathIndex].isActive() ? 1 : 0;
}


// ============================================================================
// compactPathsCUB - CUB Stream Compaction
// ============================================================================

cudaError_t compactPathsCUB(
    const uint32_t* d_pathIndicesIn,
    uint32_t numPathsIn,
    const void* d_pathStateBuffer,
    uint32_t* d_pathIndicesOut,
    uint32_t* d_numSelectedOut,
    void* d_tempStorage,
    size_t& tempStorageBytes,
    cudaStream_t stream)
{
    if (!d_pathIndicesIn || !d_pathStateBuffer || !d_pathIndicesOut || !d_numSelectedOut)
        return cudaErrorInvalidValue;

    const WavefrontPathState* pathStates = static_cast<const WavefrontPathState*>(d_pathStateBuffer);

    // flags 缓冲区需单独分配，布局：tempStorage = [CUB 工作空间 | flags 缓冲区]
    const size_t flagsBytes = numPathsIn * sizeof(uint8_t);

    // 1. 查询 CUB DeviceSelect::Flagged 所需临时存储大小
    size_t cubTempBytes = 0;
    cudaError_t err = cub::DeviceSelect::Flagged(
        nullptr,
        cubTempBytes,
        d_pathIndicesIn,
        static_cast<const uint8_t*>(nullptr),  // 仅用于查询大小
        d_pathIndicesOut,
        d_numSelectedOut,
        numPathsIn,
        stream);

    if (err != cudaSuccess)
        return err;

    // 总临时存储 = CUB 工作空间 + flags 缓冲区
    const size_t totalTempBytes = cubTempBytes + flagsBytes;
    tempStorageBytes = totalTempBytes;

    // 若仅为查询大小，直接返回
    if (!d_tempStorage || totalTempBytes == 0)
        return cudaSuccess;

    if (tempStorageBytes < totalTempBytes)
        return cudaErrorInvalidValue;

    // 2. 分区：d_cubTemp 供 CUB 使用，d_flags 在末尾
    void* d_cubTemp = d_tempStorage;
    uint8_t* d_flags = static_cast<uint8_t*>(d_tempStorage) + cubTempBytes;

    // 3. Kernel 填充活跃标志（flags[i] = 1 表示 pathIndicesIn[i] 对应的路径活跃）
    constexpr uint32_t blockSize = 256;
    uint32_t numBlocks = (numPathsIn + blockSize - 1) / blockSize;
    fillActiveFlagsKernel<<<numBlocks, blockSize, 0, stream>>>(
        d_pathIndicesIn,
        pathStates,
        d_flags,
        numPathsIn);

    err = cudaGetLastError();
    if (err != cudaSuccess)
        return err;

    // 4. 调用 CUB DeviceSelect::Flagged 进行流压缩
    err = cub::DeviceSelect::Flagged(
        d_cubTemp,
        cubTempBytes,
        d_pathIndicesIn,
        d_flags,
        d_pathIndicesOut,
        d_numSelectedOut,
        numPathsIn,
        stream);

    return err;
}


// ============================================================================
// 按材质排序辅助：提取材质类别作为排序键
// ============================================================================

/// 从路径状态中提取材质类别，作为 RadixSort 的键
__global__ void fillMaterialKeysKernel(
    const uint32_t* __restrict__ pathIndices,
    const WavefrontPathState* __restrict__ pathStates,
    uint32_t* __restrict__ keys,
    uint32_t numPaths)
{
    uint32_t idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= numPaths)
        return;

    uint32_t pathIndex = pathIndices[idx];
    keys[idx] = pathStates[pathIndex].materialCategory;
}


// ============================================================================
// sortPathsByMaterial - 按材质类别排序
// ============================================================================

cudaError_t sortPathsByMaterial(
    const uint32_t* d_pathIndicesIn,
    uint32_t numPaths,
    const void* d_pathStateBuffer,
    uint32_t* d_pathIndicesOut,
    void* d_tempStorage,
    size_t& tempStorageBytes,
    cudaStream_t stream)
{
    if (!d_pathIndicesIn || !d_pathStateBuffer || !d_pathIndicesOut)
        return cudaErrorInvalidValue;

    if (numPaths == 0) {
        tempStorageBytes = 0;
        return cudaSuccess;
    }

    const WavefrontPathState* pathStates = static_cast<const WavefrontPathState*>(d_pathStateBuffer);

    // 排序键缓冲区：materialCategory (uint32_t)
    const size_t keysBytes = numPaths * sizeof(uint32_t);

    // 1. 查询 CUB DeviceRadixSort::SortPairs 所需临时存储大小
    size_t cubTempBytes = 0;
    cudaError_t err = cub::DeviceRadixSort::SortPairs(
        nullptr,
        cubTempBytes,
        static_cast<const uint32_t*>(nullptr),
        static_cast<uint32_t*>(nullptr),
        static_cast<const uint32_t*>(nullptr),
        static_cast<uint32_t*>(nullptr),
        numPaths,
        0, 8 * sizeof(uint32_t),  // 仅排序 materialCategory 的 8 位（0~255 足够）
        stream);

    if (err != cudaSuccess)
        return err;

    // 总临时存储 = CUB 工作空间 + 键缓冲区（输入/输出可复用）
    const size_t totalTempBytes = cubTempBytes + keysBytes;
    tempStorageBytes = totalTempBytes;

    if (!d_tempStorage || totalTempBytes == 0)
        return cudaSuccess;

    if (tempStorageBytes < totalTempBytes)
        return cudaErrorInvalidValue;

    // 2. 分区：d_cubTemp 供 CUB 使用，d_keys 在末尾（CUB 返回的 temp 大小通常已对齐）
    void* d_cubTemp = d_tempStorage;
    uint32_t* d_keys = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(d_tempStorage) + cubTempBytes);

    // 3. Kernel 提取材质类别键
    constexpr uint32_t blockSize = 256;
    uint32_t numBlocks = (numPaths + blockSize - 1) / blockSize;
    fillMaterialKeysKernel<<<numBlocks, blockSize, 0, stream>>>(
        d_pathIndicesIn,
        pathStates,
        d_keys,
        numPaths);

    err = cudaGetLastError();
    if (err != cudaSuccess)
        return err;

    // 4. 调用 CUB DeviceRadixSort::SortPairs 按材质类别排序
    // 键：materialCategory，值：pathIndex
    err = cub::DeviceRadixSort::SortPairs(
        d_cubTemp,
        cubTempBytes,
        d_keys,
        d_keys,  // 键输出可覆盖输入（我们不需要保留）
        d_pathIndicesIn,
        d_pathIndicesOut,
        numPaths,
        0,
        sizeof(uint32_t) * 8,  // 排序完整 32 位键
        stream);

    return err;
}


// ============================================================================
// 临时存储大小查询
// ============================================================================

size_t compactPathsCUBTempStorageBytes(uint32_t numPaths) {
    size_t cubTempBytes = 0;
    const size_t flagsBytes = numPaths * sizeof(uint8_t);

    cub::DeviceSelect::Flagged(
        nullptr,
        cubTempBytes,
        static_cast<const uint32_t*>(nullptr),
        static_cast<const uint8_t*>(nullptr),
        static_cast<uint32_t*>(nullptr),
        static_cast<uint32_t*>(nullptr),
        numPaths,
        0);

    return cubTempBytes + flagsBytes;
}


size_t sortPathsByMaterialTempStorageBytes(uint32_t numPaths) {
    if (numPaths == 0)
        return 0;

    size_t cubTempBytes = 0;
    cub::DeviceRadixSort::SortPairs(
        nullptr,
        cubTempBytes,
        static_cast<const uint32_t*>(nullptr),
        static_cast<uint32_t*>(nullptr),
        static_cast<const uint32_t*>(nullptr),
        static_cast<uint32_t*>(nullptr),
        numPaths,
        0,
        sizeof(uint32_t) * 8,  // 排序完整 32 位键
        0);

    return cubTempBytes + numPaths * sizeof(uint32_t);
}

} // namespace shared
} // namespace vlr
<｜tool▁calls▁begin｜><｜tool▁call▁begin｜>
WebSearch