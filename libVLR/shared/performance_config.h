// ============================================================================
// VLR Wavefront - 性能配置
//
// 本文件定义性能优化相关的配置选项和常量。
//
// 作者：VLR 开发团队
// 创建日期：2026-03-07
// 环境：CUDA 13.1, OptiX 8.0.0, VS2022
// ============================================================================

#pragma once

#include <cstdint>

namespace vlr {
namespace shared {

/// 性能优化配置
struct PerformanceConfig {
    // ========================================================================
    // 同步优化
    // ========================================================================
    
    /// CPU-GPU 同步间隔（深度数）
    /// 较大的值减少同步开销，但可能导致不必要的计算
    /// 推荐值：4-8
    static constexpr uint32_t SyncInterval = 4;
    
    // ========================================================================
    // 路径压缩优化
    // ========================================================================
    
    /// 路径压缩阈值（0.0-1.0）
    /// 只在路径数下降超过此阈值时才执行压缩
    /// 推荐值：0.70-0.80（即路径数下降 20-30%）
    static constexpr float CompressionThreshold = 0.75f;
    
    /// 最小压缩路径数
    /// 当活跃路径数低于此值时，不执行压缩（开销大于收益）
    /// 推荐值：1024-4096
    static constexpr uint32_t MinPathsForCompression = 2048;
    
    // ========================================================================
    // Kernel 启动配置
    // ========================================================================
    
    /// GenerateRays kernel 的 2D block 尺寸
    static constexpr uint32_t GenerateRaysBlockWidth = 16;
    static constexpr uint32_t GenerateRaysBlockHeight = 16;
    
    /// ProcessHits kernel 的 block size
    /// 较小的值适合寄存器压力大的 kernel
    static constexpr uint32_t ProcessHitsBlockSize = 128;
    
    /// SampleLights kernel 的 block size
    /// 较大的值适合计算密集型 kernel
    static constexpr uint32_t SampleLightsBlockSize = 256;
    
    /// SampleBSDF kernel 的 block size
    /// 中等值平衡寄存器使用和 occupancy
    static constexpr uint32_t SampleBSDFBlockSize = 192;
    
    /// Accumulate kernel 的 block size
    static constexpr uint32_t AccumulateBlockSize = 256;
    
    // ========================================================================
    // 内存访问优化
    // ========================================================================
    
    /// 是否使用 __restrict__ 提示优化内存访问
    static constexpr bool UseRestrictPointers = true;
    
    /// 是否使用 shared memory 缓存材质数据
    /// 注意：需要足够的 shared memory
    static constexpr bool UseMaterialCache = false;  // 暂时禁用，需要更多测试
    
    // ========================================================================
    // 高级优化
    // ========================================================================
    
    /// 是否使用 CUDA Graphs
    /// 注意：对于动态工作负载，效果可能有限
    static constexpr bool UseCudaGraphs = false;  // 暂时禁用
    
    /// 是否使用 warp-level 操作优化
    static constexpr bool UseWarpOptimizations = true;
    
    /// 是否启用预取优化
    static constexpr bool UsePrefetching = false;  // 暂时禁用，需要更多测试
};

}  // namespace shared
}  // namespace vlr
