// ============================================================================
// VLR Wavefront - Compact Paths 头文件
//
// 声明路径压缩/队列管理的公共接口。
// 提供三种实现：简单队列交换、CUB Stream Compaction、按材质排序。
//
// 作者：VLR 开发团队
// 创建日期：2026-03-07
// 环境：CUDA 13.1, OptiX 8.0.0, VS2022
// ============================================================================

#pragma once

#include <cuda_runtime.h>
#include <cstddef>

namespace vlr {
namespace shared {

// ============================================================================
// 前置声明
// ============================================================================

struct WavefrontLaunchParameters;

// ============================================================================
// 路径压缩函数
// ============================================================================

/// 简单队列交换：交换活跃队列与下一队列的缓冲区指针，并同步计数器
/// 对应 Context 中已有的实现方式。
///
/// @param activePathIndicesPtr  指向当前活跃队列索引缓冲区的指针（主机端，将被交换）
/// @param nextPathIndicesPtr   指向下一队列索引缓冲区的指针（主机端，将被交换）
/// @param d_queueCounters      设备端队列计数器 [0]=当前活跃数, [1]=下一队列数
/// @param stream               CUDA 流
void compactPathsSimple(
    void** activePathIndicesPtr,
    void** nextPathIndicesPtr,
    uint32_t* d_queueCounters,
    cudaStream_t stream = 0);


/// CUB Stream Compaction：使用 DeviceSelect::Flagged 移除已终止路径
/// 从输入路径索引中筛选 isActive() 为真的路径到输出缓冲区。
///
/// @param d_pathIndicesIn      输入路径索引（设备）
/// @param numPathsIn           输入路径数量
/// @param d_pathStateBuffer    路径状态缓冲区（用于检查 isActive）
/// @param d_pathIndicesOut     输出压缩后的路径索引（设备）
/// @param d_numSelectedOut     输出选中数量（设备，单元素）
/// @param d_tempStorage        临时存储（可选，nullptr 时返回所需大小）
/// @param tempStorageBytes     临时存储大小（输入）或所需大小（输出，当 d_tempStorage 为 nullptr）
/// @param stream               CUDA 流
cudaError_t compactPathsCUB(
    const uint32_t* d_pathIndicesIn,
    uint32_t numPathsIn,
    const void* d_pathStateBuffer,
    uint32_t* d_pathIndicesOut,
    uint32_t* d_numSelectedOut,
    void* d_tempStorage,
    size_t& tempStorageBytes,
    cudaStream_t stream = 0);


/// 按材质类别排序路径：使用 DeviceRadixSort::SortPairs 按 materialCategory 排序
/// 使相同材质的路径连续，减少后续 BSDF 评估的分支发散。
///
/// @param d_pathIndicesIn      输入路径索引（设备）
/// @param numPaths            路径数量
/// @param d_pathStateBuffer    路径状态缓冲区（用于读取 materialCategory）
/// @param d_pathIndicesOut     输出排序后的路径索引（设备）
/// @param d_tempStorage        临时存储（可选，nullptr 时返回所需大小）
/// @param tempStorageBytes     临时存储大小（输入）或所需大小（输出，当 d_tempStorage 为 nullptr）
/// @param stream               CUDA 流
cudaError_t sortPathsByMaterial(
    const uint32_t* d_pathIndicesIn,
    uint32_t numPaths,
    const void* d_pathStateBuffer,
    uint32_t* d_pathIndicesOut,
    void* d_tempStorage,
    size_t& tempStorageBytes,
    cudaStream_t stream = 0);


/// 获取 compactPathsCUB 所需临时存储大小
size_t compactPathsCUBTempStorageBytes(uint32_t numPaths);

/// 获取 sortPathsByMaterial 所需临时存储大小
size_t sortPathsByMaterialTempStorageBytes(uint32_t numPaths);

} // namespace shared
} // namespace vlr
