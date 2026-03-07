# VLR_WF 波前路径追踪性能优化

## 优化目标

提升波前路径追踪的渲染性能，减少 CPU-GPU 同步开销，优化内存访问模式。

## 性能分析

### 当前性能瓶颈

1. **CPU-GPU 同步频繁**
   - 每个深度都要从 GPU 复制活跃路径数到 CPU
   - `cudaStreamSynchronize` 造成流水线停顿

2. **内存访问模式**
   - 路径状态的非连续访问可能导致缓存未命中
   - 队列计数器的频繁读写

3. **Kernel 启动开销**
   - 小规模 kernel 启动开销占比较高
   - 可以合并部分 kernel

4. **路径压缩开销**
   - 每次迭代都进行压缩，即使路径数变化不大
   - CUB 排序的临时存储分配

## 优化方案

### 1. 减少 CPU-GPU 同步（高优先级）

**问题**：当前实现在每个深度都要同步获取活跃路径数。

**优化**：
- 使用 persistent kernel 方式，让 GPU 自主决定何时终止
- 或者使用 GPU 端的原子计数器，减少 CPU 查询频率
- 批量处理多个深度后再同步一次

**实现**：
```cpp
// 方案 A: 每 N 个深度同步一次
constexpr uint32_t SYNC_INTERVAL = 4;
for (uint32_t depth = 0; depth < wf.maxPathLength; depth += SYNC_INTERVAL) {
    // 批量处理 SYNC_INTERVAL 个深度
    // 只在批次结束时同步
}

// 方案 B: 使用 GPU 端终止条件
// 在 kernel 中检查活跃路径数，自动终止
```

### 2. 优化内存访问模式（中优先级）

**问题**：路径状态的随机访问导致缓存未命中。

**优化**：
- 使用 Structure of Arrays (SoA) 代替 Array of Structures (AoS)
- 将频繁访问的字段分离到单独的缓冲区
- 使用 `__restrict__` 提示编译器优化

**实现**：
```cpp
// 当前：AoS
struct WavefrontPathState {
    uint32_t pixelX, pixelY;
    uint32_t pathLength;
    SampledSpectrum throughput;
    // ... 更多字段
};

// 优化：SoA
struct WavefrontPathStateSoA {
    uint32_t* pixelX;
    uint32_t* pixelY;
    uint32_t* pathLength;
    SampledSpectrum* throughput;
    // ... 更多字段
};
```

### 3. 优化 Kernel 启动配置（中优先级）

**问题**：固定的 block size 可能不是最优的。

**优化**：
- 根据 GPU 架构动态调整 block size
- 使用 occupancy calculator 确定最优配置
- 考虑寄存器使用和共享内存

**实现**：
```cpp
// 使用 CUDA Occupancy API
int minGridSize, blockSize;
cudaOccupancyMaxPotentialBlockSize(&minGridSize, &blockSize, kernelFunc, 0, 0);

// 或者针对不同 kernel 使用不同的 block size
constexpr uint32_t TRACE_BLOCK_SIZE = 256;
constexpr uint32_t SHADE_BLOCK_SIZE = 128;  // 寄存器压力大的 kernel 用小 block
```

### 4. 合并 Kernel（低优先级）

**问题**：多个小 kernel 的启动开销累积。

**优化**：
- 合并 `processHits` 和 `sampleLights` 到一个 kernel
- 合并 `sampleBSDF` 和路径更新

**权衡**：
- 优点：减少启动开销，更好的数据局部性
- 缺点：增加寄存器压力，可能降低 occupancy

### 5. 优化路径压缩策略（中优先级）

**问题**：每次迭代都进行压缩，即使路径数变化不大。

**优化**：
- 只在路径数下降超过阈值时才压缩
- 使用更高效的压缩算法
- 预分配足够的临时存储

**实现**：
```cpp
// 只在路径数下降超过 20% 时才压缩
float compressionRatio = static_cast<float>(numNextPaths) / numActivePaths;
if (compressionRatio < 0.8f) {
    // 执行压缩
} else {
    // 简单交换队列
}
```

### 6. 使用 CUDA Graphs（高优先级）

**问题**：每次渲染都要重新设置和启动 kernel。

**优化**：
- 使用 CUDA Graphs 捕获渲染循环
- 减少 CPU 端的 kernel 启动开销

**实现**：
```cpp
// 首次渲染时捕获 graph
cudaGraph_t graph;
cudaGraphExec_t graphExec;

cudaStreamBeginCapture(m_stream, cudaStreamCaptureModeGlobal);
// 执行一次完整的渲染循环
executeWavefrontRender(1);
cudaStreamEndCapture(m_stream, &graph);
cudaGraphInstantiate(&graphExec, graph, nullptr, nullptr, 0);

// 后续渲染直接启动 graph
cudaGraphLaunch(graphExec, m_stream);
```

### 7. 优化 RNG（低优先级）

**问题**：PCG32 RNG 的状态更新可能有开销。

**优化**：
- 使用更快的 RNG（如 xorshift）
- 或者使用 CUDA 内置的 curand

### 8. 使用 Shared Memory（中优先级）

**问题**：频繁访问全局内存。

**优化**：
- 在 kernel 中使用 shared memory 缓存常用数据
- 例如：材质参数、光源信息

## 实施计划

### 阶段 1：快速优化（立即实施）

1. **减少同步频率**
   - 实现批量深度处理
   - 预期提升：10-20%

2. **优化路径压缩策略**
   - 添加压缩阈值判断
   - 预期提升：5-10%

3. **调整 block size**
   - 使用 occupancy calculator
   - 预期提升：5-15%

### 阶段 2：中期优化（1-2 周）

1. **实现 CUDA Graphs**
   - 捕获渲染循环
   - 预期提升：15-25%

2. **优化内存访问**
   - 部分字段改为 SoA
   - 预期提升：10-20%

### 阶段 3：长期优化（1 个月+）

1. **重构为 SoA**
   - 完全重构数据布局
   - 预期提升：20-30%

2. **合并 Kernel**
   - 减少启动开销
   - 预期提升：10-15%

## 性能测试

### 测试场景

1. **Cornell Box**（当前测试）
   - 512x512, 1024 samples
   - 基准时间：~12 秒

2. **复杂场景**
   - 更多几何体和材质
   - 测试可扩展性

### 性能指标

- **渲染时间**：总时间和每样本时间
- **吞吐量**：Mrays/s
- **GPU 利用率**：使用 Nsight Compute 分析
- **内存带宽**：使用 Nsight Systems 分析

## 预期结果

通过上述优化，预期可以实现：

- **阶段 1**：20-40% 性能提升
- **阶段 2**：累计 50-80% 性能提升
- **阶段 3**：累计 100-150% 性能提升（2-2.5x 加速）

最终目标：在 Cornell Box 场景下，512x512 @ 1024 samples 的渲染时间从 12 秒降低到 5-6 秒。
