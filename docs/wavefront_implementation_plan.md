# Wavefront 路径追踪实现计划

## 文档版本
- **版本**: 1.0
- **日期**: 2026-03-06
- **状态**: 规划阶段

---

## 1. 项目概览

### 1.1 目标
将 VLR 渲染器从递归式路径追踪改造为 Wavefront 路径追踪，实现：
- **性能提升**: 1.5-3x 渲染速度
- **GPU 利用率**: 从 40-60% 提升到 80-95%
- **向后兼容**: 保留现有渲染器，用户可选择

### 1.2 工作量估算
- **核心实现**: 6-8 周
- **优化调试**: 4-6 周  
- **测试文档**: 2-3 周
- **总计**: 12-17 周（3-4 个月）

### 1.3 里程碑
1. **M1 - 基础架构** (Week 1-3): 数据结构和基础框架
2. **M2 - 核心功能** (Week 4-7): 所有 Kernel 实现
3. **M3 - 集成测试** (Week 8-10): Context 集成和基础测试
4. **M4 - 优化提升** (Week 11-14): 性能优化
5. **M5 - 发布准备** (Week 15-17): 文档和发布

---

## 2. 详细任务分解

### 阶段 1: 基础架构搭建 (Week 1-3)

#### Task 1.1: 创建数据结构定义 (3 天)
**文件**: `libVLR/shared/wavefront_types.h`

- [ ] 定义 `WavefrontPathState` 结构体
  - 光线信息（origin, direction）
  - 光谱信息（throughput, contribution, wls）
  - RNG 状态
  - 路径历史（prevDirPDF, prevSampledType, pathLength）
  - 像素坐标
  - 状态标志
  - **验收标准**: 结构体大小 <= 160 bytes，内存对齐正确

- [ ] 定义 `WavefrontHitInfo` 结构体
  - 命中几何信息
  - 参数化坐标
  - 命中标志
  - **验收标准**: 结构体大小 <= 32 bytes

- [ ] 定义 `WavefrontWorkQueue` 结构体
  - 路径索引数组指针
  - 原子计数器指针
  - 队列容量
  - **验收标准**: enqueue/dequeue 操作正确

- [ ] 定义 `WavefrontLaunchParameters` 结构体
  - 继承/包含公共 Pipeline 参数
  - Wavefront 特定缓冲区指针
  - 工作队列
  - 配置参数

**交付物**:
- `libVLR/shared/wavefront_types.h`
- 单元测试（可选）

**依赖**: 无

---

#### Task 1.2: 创建公共函数库 (2 天)
**文件**: `libVLR/shared/wavefront_common.h`

- [ ] 实现材质分类函数 `classifyMaterial()`
- [ ] 实现 MIS 权重计算 `computeMISWeight()`
- [ ] 实现几何项计算 `computeGeometryTerm()`
- [ ] 实现 RR 判断函数 `shouldTerminatePath()`
- [ ] 实现表面点计算 `computeSurfacePoint()`
- [ ] 实现环境光处理 `processEnvironmentHit()`

**交付物**:
- `libVLR/shared/wavefront_common.h`

**依赖**: Task 1.1

---

#### Task 1.3: 更新公共类型定义 (1 天)
**文件**: `libVLR/include/vlr/public_types.h`

- [ ] 在 `VLRRenderer` 枚举中添加 `VLRRenderer_WavefrontPathTracing`
  ```cpp
  enum VLRRenderer {
      VLRRenderer_PathTracing = 0,
      VLRRenderer_LightTracing,
      VLRRenderer_BPT,
      VLRRenderer_WavefrontPathTracing,  // 新增
      VLRRenderer_DebugRendering,
  };
  ```

- [ ] 添加 Wavefront 配置结构体（如果需要暴露给用户）
  ```cpp
  struct VLRWavefrontConfig {
      uint32_t maxPathLength;
      bool enablePathSorting;
      bool enableMaterialQueues;
  };
  ```

**交付物**:
- 更新的 `public_types.h`

**依赖**: 无

---

#### Task 1.4: 创建 Context 扩展 (3 天)
**文件**: `libVLR/context.h`

- [ ] 在 `Context::OptiX` 中添加 `WavefrontPathTracing` 结构体
  - Pipeline 和 Module
  - Programs (RayGen, Miss, HitGroup)
  - Callable Programs
  - 缓冲区（PathState, HitInfo, SurfacePoint, Queues）
  - CUDA Kernels

- [ ] 添加 Wavefront Launch Parameters
  - `WavefrontLaunchParameters wavefrontLaunchParams`
  - `CUdeviceptr wavefrontLaunchParamsOnDevice`

- [ ] 声明 Wavefront 渲染方法
  ```cpp
  void renderWavefront(CUstream stream, const Camera* camera,
                      uint32_t shrinkCoeff, bool firstFrame);
  ```

**交付物**:
- 更新的 `context.h`

**依赖**: Task 1.1, 1.2

---

#### Task 1.5: 实现 Context 初始化 (4 天)
**文件**: `libVLR/context.cpp`

- [ ] 在 `Context::Context()` 构造函数中初始化 Wavefront Pipeline
  - 创建 Pipeline
  - 加载 PTX 模块
  - 创建 Programs
  - 链接 Pipeline
  - 创建 Shader Binding Table
  - 分配缓冲区
  - 加载 CUDA Kernels
  - 初始化 Launch Parameters

- [ ] 在 `Context::~Context()` 析构函数中清理资源
  - 销毁 Pipeline 和 Programs
  - 释放缓冲区

- [ ] 实现缓冲区管理方法
  ```cpp
  void resizeWavefrontBuffers(uint32_t width, uint32_t height);
  void resetWavefrontQueues(CUstream stream);
  ```

**交付物**:
- 更新的 `context.cpp`（初始化部分）

**依赖**: Task 1.4

---

### 阶段 2: 核心 Kernel 实现 (Week 4-7)

#### Task 2.1: GenerateRays Kernel (3 天)
**文件**: `libVLR/GPU_kernels/wavefront_generate_rays.cu`

**功能**:
- 为每个像素生成初始相机光线
- 初始化 PathState
- 将所有路径加入活跃队列

**实现步骤**:
- [ ] 创建文件和基础框架
- [ ] 实现 RNG 初始化
- [ ] 实现波长采样
- [ ] 实现相机采样（复用现有代码）
  - Perspective Camera
  - Equirectangular Camera
- [ ] 实现 IDF 评估
- [ ] 初始化 PathState 所有字段
- [ ] 将路径索引加入活跃队列
- [ ] 处理 Denoiser 辅助缓冲区（第一次命中）

**测试**:
- [ ] 验证 PathState 初始化正确
- [ ] 验证队列大小等于像素数
- [ ] 验证光线方向分布正确

**交付物**:
- `wavefront_generate_rays.cu`
- 对应的 PTX 文件

**依赖**: Task 1.1-1.5

---

#### Task 2.2: TraceRays Kernel (4 天)
**文件**: `libVLR/GPU_kernels/wavefront_trace_rays.cu`

**功能**:
- 对活跃队列中的路径进行光线追踪
- 填充 HitInfo

**实现步骤**:
- [ ] 创建文件和基础框架
- [ ] 定义 `WFTracePayload` 和 `WFTracePayloadSignature`
- [ ] 实现 Ray Generation Program
  ```cpp
  CUDA_DEVICE_KERNEL void RT_RG_NAME(wavefrontTraceRays)()
  ```
  - 从活跃队列读取路径索引
  - 从 PathState 读取光线信息
  - 发射光线

- [ ] 实现 Closest Hit Program
  ```cpp
  CUDA_DEVICE_KERNEL void RT_CH_NAME(wavefrontClosestHit)()
  ```
  - 获取命中参数
  - 填充 HitInfo
  - 标记命中类型

- [ ] 实现 Miss Program
  ```cpp
  CUDA_DEVICE_KERNEL void RT_MS_NAME(wavefrontMiss)()
  ```
  - 标记命中环境光

- [ ] 实现 Any Hit Program（Alpha 测试）
  ```cpp
  CUDA_DEVICE_KERNEL void RT_AH_NAME(wavefrontAnyHitWithAlpha)()
  ```

- [ ] 实现阴影光线相关 Programs
  - Shadow Miss
  - Shadow Any Hit (default)
  - Shadow Any Hit (with alpha)

**测试**:
- [ ] 验证命中信息正确
- [ ] 验证环境光命中正确
- [ ] 验证 Alpha 测试正确

**交付物**:
- `wavefront_trace_rays.cu`
- 对应的 PTX 文件

**依赖**: Task 2.1

---

#### Task 2.3: ProcessHits Kernel (5 天)
**文件**: `libVLR/GPU_kernels/wavefront_process_hits.cu`

**功能**:
- 计算表面点信息
- 评估 BSDF/EDF
- 处理隐式光源采样
- 材质分类

**实现步骤**:
- [ ] 创建文件和基础框架
- [ ] 实现表面点计算
  - 调用几何解码程序
  - 应用法线贴图
  - 应用切线修改

- [ ] 实现 BSDF 评估
  - 获取材质描述符
  - 构造 BSDF 对象
  - 评估基础颜色

- [ ] 实现 EDF 评估
  - 构造 EDF 对象
  - 评估发射强度

- [ ] 实现隐式光源采样
  - 检查是否命中发光表面
  - 计算 MIS 权重
  - 累积贡献值

- [ ] 实现路径终止判断
  - 最大长度检查
  - 俄罗斯轮盘赌
  - 零吞吐量检查

- [ ] 实现材质分类
  - 调用 `classifyMaterial()`
  - 设置 PathState 的材质类别

- [ ] 处理 Denoiser 辅助缓冲区
  - 第一次命中的 Albedo
  - 第一次命中的 Normal

**测试**:
- [ ] 验证表面点计算正确
- [ ] 验证 BSDF 评估正确
- [ ] 验证隐式光源采样正确
- [ ] 验证材质分类正确

**交付物**:
- `wavefront_process_hits.cu`
- 对应的 PTX 文件

**依赖**: Task 2.2

---

#### Task 2.4: SampleLights Kernel (5 天)
**文件**: `libVLR/GPU_kernels/wavefront_sample_lights.cu`

**功能**:
- Next Event Estimation (显式光源采样)
- 阴影光线追踪
- MIS 权重计算
- 累积直接光照贡献

**实现步骤**:
- [ ] 创建文件和基础框架

- [ ] 实现光源选择
  - 调用 `selectSurfaceLight()`
  - 处理区域光、点光源、环境光

- [ ] 实现光源位置采样
  - 采样光源表面点
  - 计算采样 PDF

- [ ] 实现光源辐射评估
  - 构造 EDF
  - 评估发射强度和方向分布

- [ ] 实现可见性测试
  - 发射阴影光线
  - 处理 Alpha 纹理
  - 计算分数可见性

- [ ] 实现 BSDF 评估
  - 计算 BSDF 值
  - 计算 BSDF PDF

- [ ] 实现 MIS 权重计算
  - Power Heuristic (beta=2)
  - 处理 delta 材质

- [ ] 实现几何项计算
  - 距离衰减
  - 余弦项

- [ ] 累积直接光照贡献
  - contribution += throughput * Le * fs * G * MISWeight / lightPDF

**优化**:
- [ ] 跳过 delta 材质（镜面反射不需要 NEE）
- [ ] 早期退出（零辐射、零 BSDF）

**测试**:
- [ ] 验证光源采样正确
- [ ] 验证阴影光线正确
- [ ] 验证 MIS 权重正确
- [ ] 对比递归式结果

**交付物**:
- `wavefront_sample_lights.cu`
- 对应的 PTX 文件

**依赖**: Task 2.3

---

#### Task 2.5: SampleBSDF Kernel (4 天)
**文件**: `libVLR/GPU_kernels/wavefront_sample_bsdf.cu`

**功能**:
- BSDF 采样生成下一跳方向
- 更新路径吞吐量
- 生成下一轮光线
- 将活跃路径加入下一轮队列

**实现步骤**:
- [ ] 创建文件和基础框架

- [ ] 实现 BSDF 采样
  - 构造 BSDFQuery
  - 调用 BSDF::sample()
  - 处理采样失败

- [ ] 处理色散材质
  - 检查 isDispersive
  - 调整 PDF
  - 设置 singleWlSelected 标志

- [ ] 更新路径吞吐量
  - throughput *= fs * |cos| / pdf
  - 处理 Shading Normal 修正

- [ ] 生成下一跳光线
  - 计算新的起点（offsetRayOrigin）
  - 计算新的方向
  - 更新 prevDirPDF 和 prevSampledType

- [ ] 将路径加入下一轮队列
  - 调用 nextActivePathQueue.enqueue()

**测试**:
- [ ] 验证 BSDF 采样正确
- [ ] 验证吞吐量更新正确
- [ ] 验证光线生成正确
- [ ] 验证队列操作正确

**交付物**:
- `wavefront_sample_bsdf.cu`
- 对应的 PTX 文件

**依赖**: Task 2.4

---

#### Task 2.6: AccumulateResults Kernel (2 天)
**文件**: `libVLR/GPU_kernels/wavefront_accumulate.cu`

**功能**:
- 将路径贡献累加到输出缓冲区
- 更新 RNG 状态
- 处理 Denoiser 辅助缓冲区

**实现步骤**:
- [ ] 创建文件和基础框架

- [ ] 实现贡献值累积
  - 检查贡献值有效性（allFinite）
  - 累加到 accumBuffer
  - 处理首帧重置

- [ ] 更新 RNG 状态
  - 写回 rngBuffer

- [ ] 处理 Denoiser 缓冲区
  - 累积 Albedo
  - 累积 Normal

**测试**:
- [ ] 验证累积正确
- [ ] 验证 RNG 状态保存正确
- [ ] 验证多帧累积正确

**交付物**:
- `wavefront_accumulate.cu`
- 对应的 PTX 文件

**依赖**: Task 2.5

---

#### Task 2.7: CompactPaths Kernel (3 天)
**文件**: `libVLR/GPU_kernels/wavefront_compact.cu`

**功能**:
- Stream Compaction：移除已终止路径
- 队列交换
- 计数器重置

**实现步骤**:
- [ ] 创建文件和基础框架

- [ ] 实现简单版本（队列交换）
  ```cpp
  swap(activePathQueue, nextActivePathQueue);
  nextActivePathQueue.reset();
  ```

- [ ] 实现 CUB Stream Compaction 版本
  - 使用 `cub::DeviceSelect::Flagged`
  - 定义活跃性谓词
  - 压缩路径索引数组

- [ ] 实现路径排序（可选）
  - 使用 `cub::DeviceRadixSort::SortPairs`
  - 按材质类别排序

**测试**:
- [ ] 验证队列交换正确
- [ ] 验证 Stream Compaction 正确
- [ ] 性能测试（对比简单版本和 CUB 版本）

**交付物**:
- `wavefront_compact.cu`
- 对应的 PTX 文件

**依赖**: Task 2.6

---

### 阶段 3: Context 集成 (Week 8-10)

#### Task 3.1: Pipeline 初始化实现 (4 天)
**文件**: `libVLR/context.cpp`

- [ ] 实现 Pipeline 创建
  - 设置 Pipeline Options
  - 配置 Payload 大小
  - 设置 Exception Flags

- [ ] 实现模块加载
  - 加载 Wavefront 主模块
  - 加载公共模块（材质、几何、相机等）
  - 设置编译选项

- [ ] 实现 Program 创建
  - Ray Generation Programs
  - Miss Programs
  - Hit Program Groups
  - Callable Programs（复用现有）

- [ ] 实现 Pipeline 链接
  - 设置最大递归深度（0 或 1）
  - 设置调试级别

- [ ] 实现 Shader Binding Table 创建
  - Ray Generation Records
  - Miss Records
  - Hit Group Records

**测试**:
- [ ] 验证 Pipeline 创建成功
- [ ] 验证模块加载成功
- [ ] 验证 SBT 创建正确

**交付物**:
- 完整的 Pipeline 初始化代码

**依赖**: Task 1.5, 2.1-2.7

---

#### Task 3.2: 缓冲区管理实现 (3 天)
**文件**: `libVLR/context.cpp`

- [ ] 实现缓冲区分配
  - PathState Buffer
  - HitInfo Buffer
  - SurfacePoint Buffer
  - Queue Buffers
  - Material Queue Buffers（可选）

- [ ] 实现缓冲区调整大小
  ```cpp
  void Context::resizeWavefrontBuffers(uint32_t width, uint32_t height)
  ```
  - 检查是否需要重新分配
  - 释放旧缓冲区
  - 分配新缓冲区
  - 更新 Launch Parameters

- [ ] 实现队列重置
  ```cpp
  void Context::resetWavefrontQueues(CUstream stream)
  ```
  - 重置计数器为 0
  - 清空队列内容（可选）

**测试**:
- [ ] 验证缓冲区分配成功
- [ ] 验证动态调整大小正确
- [ ] 验证队列重置正确

**交付物**:
- 缓冲区管理代码

**依赖**: Task 3.1

---

#### Task 3.3: 渲染主循环实现 (5 天)
**文件**: `libVLR/context.cpp`

- [ ] 实现 `renderWavefront()` 方法框架
  ```cpp
  void Context::renderWavefront(
      CUstream stream, const Camera* camera,
      uint32_t shrinkCoeff, bool firstFrame)
  ```

- [ ] 实现阶段 1：生成初始光线
  - 设置 Launch Dimensions
  - 调用 GenerateRays OptiX Launch
  - 同步和错误检查

- [ ] 实现主循环：迭代处理路径
  ```cpp
  for (uint32_t depth = 0; depth < maxDepth; ++depth) {
      // 检查活跃路径数
      // 调用各个 Kernel
  }
  ```

- [ ] 实现阶段 2：光线追踪
  - 调用 TraceRays OptiX Launch

- [ ] 实现阶段 3：处理命中点
  - 调用 ProcessHits CUDA Kernel
  - 计算 Grid/Block 维度

- [ ] 实现阶段 4：显式光源采样
  - 调用 SampleLights CUDA Kernel

- [ ] 实现阶段 5：BSDF 采样
  - 调用 SampleBSDF CUDA Kernel

- [ ] 实现阶段 6：路径压缩
  - 调用 CompactPaths CUDA Kernel
  - 队列交换

- [ ] 实现阶段 7：累积结果
  - 调用 AccumulateResults CUDA Kernel

- [ ] 实现活跃路径数检查
  - 从 GPU 读取计数器
  - 早期退出优化

- [ ] 实现性能计时（可选）
  - CUDA Events
  - 记录各阶段时间

**测试**:
- [ ] 简单场景渲染测试（Cornell Box）
- [ ] 验证主循环逻辑正确
- [ ] 验证早期退出正确

**交付物**:
- 完整的 `renderWavefront()` 实现

**依赖**: Task 3.2, 2.1-2.7

---

#### Task 3.4: 渲染器切换实现 (2 天)
**文件**: `libVLR/context.cpp`, `HostProgram/main.cpp`

- [ ] 更新 `Context::render()` 方法
  ```cpp
  void Context::render(...) {
      switch (m_renderer) {
      case VLRRenderer_PathTracing:
          renderPathTracing(stream, camera, ...);
          break;
      case VLRRenderer_WavefrontPathTracing:
          renderWavefront(stream, camera, ...);
          break;
      // ...
      }
  }
  ```

- [ ] 更新 HostProgram UI
  - 在渲染器下拉菜单中添加 "Wavefront Path Tracing"
  - 添加 Wavefront 配置选项（可选）

**测试**:
- [ ] 验证渲染器切换正确
- [ ] 验证 UI 显示正确

**交付物**:
- 更新的 `context.cpp` 和 `main.cpp`

**依赖**: Task 3.3

---

### 阶段 4: 功能完善 (Week 11-12)

#### Task 4.1: 支持所有 BSDF 类型 (4 天)

**测试每种材质**:
- [ ] Lambert (Matte)
- [ ] Ideal Specular Reflection
- [ ] Ideal Specular Transmission
- [ ] Microfacet GGX Reflection
- [ ] Microfacet GGX Transmission
- [ ] Fresnel-blended Lambertian
- [ ] UE4-like BRDF
- [ ] Frostbite-like BRDF
- [ ] Mixed BSDF

**验证**:
- [ ] 视觉对比（与递归式）
- [ ] 数值对比（像素级误差）

**交付物**:
- 测试报告
- Bug 修复

**依赖**: Task 3.4

---

#### Task 4.2: 支持所有光源类型 (3 天)

**测试每种光源**:
- [ ] 区域光（Polygonal Light）
- [ ] 点光源（Point Light）
- [ ] 环境光（IBL）
- [ ] 多光源场景

**验证**:
- [ ] 光源采样正确
- [ ] MIS 权重正确
- [ ] 阴影正确

**交付物**:
- 测试报告
- Bug 修复

**依赖**: Task 4.1

---

#### Task 4.3: 支持高级特性 (5 天)

- [ ] 景深效果（Depth of Field）
  - 透镜采样
  - 焦平面计算

- [ ] 法线贴图（Normal Mapping）
  - 验证 applyBumpMapping 正确

- [ ] Alpha 纹理（Alpha Texture）
  - 验证 Alpha 测试正确
  - 验证阴影光线 Alpha 处理

- [ ] 几何实例化（Geometry Instancing）
  - 验证实例变换正确
  - 验证多实例场景

- [ ] 光谱渲染（Spectral Rendering）
  - 验证波长采样正确
  - 验证色散材质

**测试**:
- [ ] 每个特性的独立测试
- [ ] 组合特性测试

**交付物**:
- 功能完整的 Wavefront 渲染器

**依赖**: Task 4.2

---

### 阶段 5: 性能优化 (Week 13-14)

#### Task 5.1: 路径排序优化 (3 天)

**实现**:
- [ ] 集成 CUB RadixSort
  ```cpp
  #include <cub/cub.cuh>
  
  cub::DeviceRadixSort::SortPairs(
      d_temp_storage, temp_storage_bytes,
      d_keys_in, d_keys_out,
      d_values_in, d_values_out,
      num_items);
  ```

- [ ] 实现排序 Kernel
  - 提取材质类别作为 key
  - 路径索引作为 value
  - 排序后更新队列

- [ ] 性能测试
  - 测量排序开销
  - 测量 BSDF 评估加速
  - 计算净收益

**预期收益**: 10-20% 性能提升

**交付物**:
- 路径排序实现
- 性能对比报告

**依赖**: Task 4.3

---

#### Task 5.2: Stream Compaction 优化 (2 天)

**实现**:
- [ ] 集成 CUB DeviceSelect
  ```cpp
  cub::DeviceSelect::Flagged(
      d_temp_storage, temp_storage_bytes,
      d_in, d_flags, d_out, d_num_selected_out,
      num_items);
  ```

- [ ] 实现活跃性标志提取
- [ ] 性能对比（简单交换 vs CUB）

**预期收益**: 5-10% 性能提升

**交付物**:
- Stream Compaction 实现
- 性能对比报告

**依赖**: Task 5.1

---

#### Task 5.3: 内存访问优化 (3 天)

**实验 SoA 布局**:
- [ ] 实现 SoA 版本的 PathState
- [ ] 修改 Kernel 访问模式
- [ ] 性能对比（AoS vs SoA）

**内存合并优化**:
- [ ] 分析内存访问模式
- [ ] 优化数据布局
- [ ] 使用 Nsight Compute 分析

**预期收益**: 5-15% 性能提升（取决于场景）

**交付物**:
- 内存优化实现（如果有收益）
- 性能分析报告

**依赖**: Task 5.2

---

#### Task 5.4: 材质特化 Kernel (4 天)

**实现**:
- [ ] 创建漫反射特化 Kernel
  ```cpp
  CUDA_DEVICE_KERNEL void wavefrontSampleBSDF_Diffuse()
  ```
  - 移除材质类型判断
  - 优化 Lambert BRDF 采样

- [ ] 创建光滑反射特化 Kernel
  ```cpp
  CUDA_DEVICE_KERNEL void wavefrontSampleBSDF_Glossy()
  ```
  - 优化 GGX BRDF 采样

- [ ] 创建镜面反射特化 Kernel
  ```cpp
  CUDA_DEVICE_KERNEL void wavefrontSampleBSDF_Specular()
  ```
  - 完美镜面反射，无需采样

- [ ] 实现动态 Kernel 选择
  - 根据材质队列大小选择
  - 负载均衡

**预期收益**: 15-25% 性能提升

**交付物**:
- 材质特化 Kernel
- 性能对比报告

**依赖**: Task 5.3

---

### 阶段 6: 测试与验证 (Week 15-16)

#### Task 6.1: 正确性测试 (4 天)

**测试场景**:
- [ ] Cornell Box（基准场景）
  - 漫反射材质
  - 区域光源
  - 简单几何

- [ ] 玻璃球场景
  - 透射材质
  - 焦散效果
  - 色散

- [ ] 复杂材质场景
  - UE4 BRDF
  - 法线贴图
  - 多种材质混合

- [ ] 大规模场景
  - Rungholt 模型
  - 环境光照
  - 几何实例化

**验证方法**:
- [ ] 像素级对比（L2 误差）
- [ ] 统计误差分析（均值、方差）
- [ ] 视觉质量评估（SSIM、PSNR）

**验收标准**:
- L2 误差 < 1%
- SSIM > 0.99
- 无明显视觉差异

**交付物**:
- 测试报告
- 对比图像
- Bug 修复

**依赖**: Task 5.4

---

#### Task 6.2: 性能测试 (3 天)

**测试维度**:
- [ ] 不同分辨率
  - 720p, 1080p, 1440p, 4K
  - 测量渲染时间和 FPS

- [ ] 不同场景复杂度
  - 简单场景（< 1K 三角形）
  - 中等场景（10K-100K 三角形）
  - 复杂场景（> 1M 三角形）

- [ ] 不同材质组合
  - 纯漫反射
  - 混合材质
  - 复杂材质

- [ ] 不同路径长度
  - MaxPathLength = 5, 10, 15, 25, 50

**性能指标**:
- [ ] 渲染时间（ms/frame）
- [ ] 加速比（vs 递归式）
- [ ] GPU 占用率（使用 Nsight Systems）
- [ ] 内存带宽利用率
- [ ] SM 利用率

**工具**:
- CUDA Events（时间测量）
- Nsight Systems（整体性能分析）
- Nsight Compute（Kernel 级分析）

**交付物**:
- 性能测试报告
- 性能对比图表
- 优化建议

**依赖**: Task 6.1

---

#### Task 6.3: 边界情况测试 (2 天)

**测试用例**:
- [ ] 空场景（只有环境光）
- [ ] 纯发光场景（无间接光照）
- [ ] 极长路径（MaxPathLength = 100）
- [ ] 极短路径（MaxPathLength = 1）
- [ ] 高分辨率（8K）
- [ ] 低分辨率（256x256）
- [ ] 复杂材质组合
- [ ] 大量透明物体
- [ ] 色散材质

**验证**:
- [ ] 无崩溃
- [ ] 无内存泄漏
- [ ] 结果正确

**交付物**:
- 边界测试报告
- Bug 修复

**依赖**: Task 6.2

---

### 阶段 7: 调试工具 (Week 17)

#### Task 7.1: 调试渲染模式 (3 天)

**实现调试模式**:
- [ ] 路径长度可视化
  ```cpp
  WFDebug_PathLength: 颜色 = pathLength / maxPathLength
  ```

- [ ] 材质类别可视化
  ```cpp
  WFDebug_MaterialCategory: 不同颜色表示不同材质
  ```

- [ ] 吞吐量可视化
  ```cpp
  WFDebug_Throughput: 颜色 = throughput.luminance()
  ```

- [ ] 活跃路径分布
  ```cpp
  WFDebug_ActivePaths: 显示哪些像素的路径仍然活跃
  ```

**实现**:
- [ ] 在 `public_types.h` 中添加枚举
- [ ] 在 AccumulateResults kernel 中实现
- [ ] 在 UI 中添加控件

**交付物**:
- 调试渲染模式实现
- 调试截图

**依赖**: Task 6.3

---

#### Task 7.2: 性能分析工具 (2 天)

**实现**:
- [ ] 性能统计收集
  - 每个深度的活跃路径数
  - 光线数量统计
  - 材质交互统计

- [ ] 实时性能显示
  - 在 UI 中显示统计信息
  - 实时更新

- [ ] 性能日志导出
  - CSV 格式
  - 用于后续分析

**交付物**:
- 性能分析工具
- 使用文档

**依赖**: Task 7.1

---

### 阶段 8: 文档与发布 (Week 18)

#### Task 8.1: 技术文档 (2 天)

**文档内容**:
- [ ] 架构设计文档（已完成）
- [ ] API 文档
  - Wavefront 相关的 API
  - 配置选项
  - 使用示例

- [ ] 实现细节文档
  - 数据结构详解
  - Kernel 实现细节
  - 优化技巧

**交付物**:
- `docs/wavefront_api.md`
- `docs/wavefront_implementation.md`

**依赖**: Task 7.2

---

#### Task 8.2: 更新 README (1 天)

**更新内容**:
- [ ] 在 Features 中添加 Wavefront Path Tracing
- [ ] 更新性能数据
- [ ] 添加使用示例
- [ ] 添加性能对比图表

**交付物**:
- 更新的 `README.md`

**依赖**: Task 8.1

---

#### Task 8.3: 创建示例场景 (2 天)

**示例场景**:
- [ ] Wavefront 性能展示场景
  - 复杂材质组合
  - 展示性能优势

- [ ] 对比场景
  - 递归式 vs Wavefront
  - 并排渲染对比

**交付物**:
- 示例场景文件
- 渲染结果图像

**依赖**: Task 8.2

---

## 3. 风险管理

### 3.1 技术风险

| 风险 | 概率 | 影响 | 缓解策略 |
|-----|------|------|---------|
| OptiX 7 Payload 限制 | 中 | 高 | 最小化 Payload，使用全局内存 |
| 内存不足（高分辨率） | 高 | 中 | 实现分块渲染，动态调整缓冲区 |
| 性能未达预期 | 中 | 高 | 渐进式优化，多轮迭代 |
| Callable Program 开销 | 中 | 中 | 考虑内联或特化 Kernel |
| 复杂材质支持困难 | 低 | 中 | 参考 PBRT-v4 实现 |

### 3.2 项目风险

| 风险 | 概率 | 影响 | 缓解策略 |
|-----|------|------|---------|
| 开发时间超期 | 中 | 中 | 分阶段交付，优先核心功能 |
| Bug 修复耗时 | 高 | 中 | 充分测试，早期发现问题 |
| 维护成本高 | 中 | 低 | 良好的代码结构和文档 |

---

## 4. 资源需求

### 4.1 硬件资源
- **开发机器**: RTX 3080 或更高（10GB+ 显存）
- **测试机器**: 多种 GPU（RTX 20/30/40 系列）

### 4.2 软件资源
- CUDA 12.5
- OptiX 8.0.0
- Nsight Systems / Nsight Compute
- CUB / Thrust 库

### 4.3 参考资料
- PBRT-v4 源代码
- Laine2013 论文
- OptiX 7 编程指南
- CUDA 最佳实践指南

---

## 5. 质量保证

### 5.1 代码质量标准
- [ ] 遵循现有代码风格
- [ ] 适当的错误处理
- [ ] 关键部分添加注释（解释"为什么"，而非"是什么"）
- [ ] 无编译警告

### 5.2 测试覆盖
- [ ] 单元测试（数据结构）
- [ ] 集成测试（Kernel）
- [ ] 端到端测试（完整渲染）
- [ ] 性能回归测试

### 5.3 性能基准
- [ ] 建立性能基准线（递归式）
- [ ] 每次优化后测量性能
- [ ] 防止性能回退

---

## 6. 交付检查清单

### 6.1 代码交付
- [ ] 所有源文件已提交
- [ ] 编译无错误和警告
- [ ] 通过所有测试
- [ ] 代码审查完成

### 6.2 文档交付
- [ ] 架构设计文档
- [ ] API 文档
- [ ] 实现细节文档
- [ ] 用户指南
- [ ] 性能测试报告

### 6.3 测试交付
- [ ] 正确性测试报告
- [ ] 性能测试报告
- [ ] 对比图像
- [ ] 测试场景文件

---

## 7. 后续优化方向

### 7.1 短期优化（3-6 个月）
- [ ] 多流并行（Pipeline Parallelism）
- [ ] 自适应采样（Adaptive Sampling）
- [ ] 路径再生（Path Regeneration）
- [ ] 重要性采样改进

### 7.2 中期优化（6-12 个月）
- [ ] ReSTIR 集成（Reservoir Sampling）
- [ ] 神经降噪器集成
- [ ] 光子映射集成
- [ ] 体积渲染支持

### 7.3 长期优化（1-2 年）
- [ ] 多 GPU 支持
- [ ] 分布式渲染
- [ ] 实时渲染模式
- [ ] 硬件光线追踪优化（RTX）

---

## 8. 成功标准

### 8.1 功能标准
- ✅ 所有现有 Path Tracing 功能正常工作
- ✅ 渲染结果与递归式一致（误差 < 1%）
- ✅ 支持所有材质和光源类型
- ✅ UI 集成完整

### 8.2 性能标准
- ✅ 简单场景加速 >= 1.5x
- ✅ 复杂场景加速 >= 2.0x
- ✅ GPU 占用率 >= 75%
- ✅ 内存开销 < 1GB（1080p）

### 8.3 质量标准
- ✅ 无已知 Bug
- ✅ 代码可维护性良好
- ✅ 文档完整
- ✅ 通过所有测试

---

## 9. 进度跟踪

### 9.1 每周检查点
- **Week 1**: Task 1.1-1.3 完成
- **Week 2**: Task 1.4-1.5 完成
- **Week 3**: 阶段 1 完成，开始阶段 2
- **Week 4**: Task 2.1-2.2 完成
- **Week 5**: Task 2.3-2.4 完成
- **Week 6**: Task 2.5-2.6 完成
- **Week 7**: Task 2.7 完成，阶段 2 完成
- **Week 8**: Task 3.1-3.2 完成
- **Week 9**: Task 3.3 完成
- **Week 10**: Task 3.4 完成，阶段 3 完成，首次完整渲染
- **Week 11**: Task 4.1-4.2 完成
- **Week 12**: Task 4.3 完成，阶段 4 完成
- **Week 13**: Task 5.1-5.2 完成
- **Week 14**: Task 5.3-5.4 完成，阶段 5 完成
- **Week 15**: Task 6.1 完成
- **Week 16**: Task 6.2-6.3 完成，阶段 6 完成
- **Week 17**: Task 7.1-7.2 完成，阶段 7 完成
- **Week 18**: Task 8.1-8.3 完成，项目完成

### 9.2 关键里程碑
- **M1 (Week 3)**: 基础架构完成
- **M2 (Week 7)**: 所有 Kernel 实现完成
- **M3 (Week 10)**: 首次完整渲染成功 🎉
- **M4 (Week 14)**: 性能优化完成
- **M5 (Week 18)**: 项目发布 🚀

---

## 10. 开发环境配置

### 10.1 编译配置

**CMakeLists.txt 更新**:
```cmake
# Wavefront Path Tracing 选项
option(VLR_ENABLE_WAVEFRONT "Enable Wavefront Path Tracing" ON)

if(VLR_ENABLE_WAVEFRONT)
    add_definitions(-DVLR_ENABLE_WAVEFRONT)
    
    # Wavefront CUDA 源文件
    set(WAVEFRONT_SOURCES
        GPU_kernels/wavefront_generate_rays.cu
        GPU_kernels/wavefront_trace_rays.cu
        GPU_kernels/wavefront_process_hits.cu
        GPU_kernels/wavefront_sample_lights.cu
        GPU_kernels/wavefront_sample_bsdf.cu
        GPU_kernels/wavefront_accumulate.cu
        GPU_kernels/wavefront_compact.cu
        GPU_kernels/wavefront_utils.cu
    )
    
    # 编译为 PTX
    foreach(WAVEFRONT_SRC ${WAVEFRONT_SOURCES})
        # ... PTX 编译配置 ...
    endforeach()
endif()
```

### 10.2 调试配置

**预处理器定义**:
```cpp
// 启用 Wavefront 调试输出
#define VLR_WAVEFRONT_DEBUG 1

// 启用性能统计
#define VLR_WAVEFRONT_PERF_STATS 1

// 启用验证检查
#define VLR_WAVEFRONT_VALIDATION 0  // Release 时关闭
```

---

## 11. 代码审查检查清单

### 11.1 架构审查
- [ ] 数据结构设计合理
- [ ] 内存布局优化
- [ ] 模块划分清晰
- [ ] 接口设计良好

### 11.2 实现审查
- [ ] 算法正确性
- [ ] 边界条件处理
- [ ] 错误处理完善
- [ ] 资源管理正确（无泄漏）

### 11.3 性能审查
- [ ] 无明显性能瓶颈
- [ ] 内存访问模式优化
- [ ] 分支发散最小化
- [ ] 寄存器使用合理

### 11.4 可维护性审查
- [ ] 代码可读性好
- [ ] 注释充分（关键部分）
- [ ] 命名规范一致
- [ ] 易于扩展

---

## 12. 发布准备

### 12.1 发布内容
- [ ] 源代码
- [ ] 编译好的库（可选）
- [ ] 示例程序
- [ ] 文档
- [ ] 测试场景

### 12.2 发布说明
```markdown
# VLR v2.0 - Wavefront Path Tracing

## 新特性
- 全新的 Wavefront 路径追踪渲染器
- 性能提升 1.5-3x
- GPU 利用率提升到 80-95%
- 保留所有现有功能

## 使用方法
context->setRenderer(VLRRenderer_WavefrontPathTracing);

## 性能对比
- Cornell Box (1080p): 2.1x faster
- Complex Scene (1080p): 2.8x faster

## 已知限制
- 高分辨率（>4K）需要更多显存
- 某些极端场景可能性能提升有限

## 向后兼容性
- 完全向后兼容
- 现有代码无需修改
- 可以在运行时切换渲染器
```

### 12.3 发布检查清单
- [ ] 所有测试通过
- [ ] 文档完整
- [ ] 示例可运行
- [ ] 性能达标
- [ ] 无已知严重 Bug
- [ ] 代码审查通过
- [ ] 发布说明准备好

---

## 13. 维护计划

### 13.1 短期维护（3 个月）
- 修复用户报告的 Bug
- 小幅性能优化
- 文档改进

### 13.2 中期维护（6-12 个月）
- 添加新特性
- 重大性能优化
- 支持新的 GPU 架构

### 13.3 长期维护（1-2 年）
- 架构升级
- 集成新技术
- 社区贡献管理

---

## 附录 A: 文件清单

### A.1 新增文件

**头文件**:
- `libVLR/shared/wavefront_types.h` - 数据结构定义
- `libVLR/shared/wavefront_common.h` - 公共函数
- `docs/wavefront_data_structures.h` - 参考实现

**CUDA 源文件**:
- `libVLR/GPU_kernels/wavefront_generate_rays.cu`
- `libVLR/GPU_kernels/wavefront_trace_rays.cu`
- `libVLR/GPU_kernels/wavefront_process_hits.cu`
- `libVLR/GPU_kernels/wavefront_sample_lights.cu`
- `libVLR/GPU_kernels/wavefront_sample_bsdf.cu`
- `libVLR/GPU_kernels/wavefront_accumulate.cu`
- `libVLR/GPU_kernels/wavefront_compact.cu`
- `libVLR/GPU_kernels/wavefront_utils.cu`

**文档文件**:
- `docs/wavefront_design.md` - 架构设计
- `docs/wavefront_implementation_plan.md` - 实现计划（本文件）
- `docs/wavefront_api.md` - API 文档
- `docs/wavefront_performance.md` - 性能报告

### A.2 修改文件

**核心文件**:
- `libVLR/include/vlr/public_types.h` - 添加渲染器类型
- `libVLR/context.h` - 添加 Wavefront Pipeline
- `libVLR/context.cpp` - 实现 Wavefront 渲染
- `HostProgram/main.cpp` - UI 集成

**构建文件**:
- `libVLR/CMakeLists.txt` - 添加 Wavefront 源文件
- `libVLR/libVLR.vcxproj` - Visual Studio 项目文件

**文档文件**:
- `README.md` - 更新特性列表
- `todo.md` - 更新任务状态

---

## 附录 B: 性能优化检查清单

### B.1 内存优化
- [ ] 最小化 Payload 大小
- [ ] 使用合并内存访问（Coalesced Access）
- [ ] 考虑 SoA vs AoS 布局
- [ ] 减少全局内存访问
- [ ] 使用共享内存（Shared Memory）缓存

### B.2 计算优化
- [ ] 减少分支发散
- [ ] 使用 warp-level primitives
- [ ] 优化数学运算（使用内建函数）
- [ ] 避免不必要的计算
- [ ] 使用 LUT（Look-Up Table）

### B.3 并行优化
- [ ] 路径排序（按材质类型）
- [ ] Stream Compaction
- [ ] 材质特化 Kernel
- [ ] 多流并行
- [ ] 异步内存传输

### B.4 算法优化
- [ ] 重要性采样改进
- [ ] MIS 策略优化
- [ ] 俄罗斯轮盘赌阈值调优
- [ ] 自适应采样

---

## 附录 C: 调试技巧

### C.1 常见问题

**问题 1: 渲染结果全黑**
- 检查 PathState 初始化
- 检查工作队列是否为空
- 检查光线方向是否正确
- 使用调试渲染模式

**问题 2: 渲染结果与递归式不一致**
- 逐个 Kernel 验证
- 对比中间结果
- 检查 MIS 权重计算
- 检查吞吐量更新

**问题 3: 性能未达预期**
- 使用 Nsight Compute 分析
- 检查 GPU 占用率
- 检查内存带宽利用率
- 检查分支发散

**问题 4: 内存不足**
- 减少缓冲区大小
- 实现分块渲染
- 使用更高效的数据结构

### C.2 调试工具使用

**Nsight Systems**:
```bash
nsys profile --trace=cuda,nvtx ./HostProgram
```

**Nsight Compute**:
```bash
ncu --set full --target-processes all ./HostProgram
```

**CUDA-MEMCHECK**:
```bash
cuda-memcheck --tool memcheck ./HostProgram
```

---

## 附录 D: 性能测试场景

### D.1 基准场景

**Cornell Box**:
- 分辨率: 1024x1024
- 三角形数: 32
- 材质: 纯漫反射
- 光源: 1 个区域光
- 预期加速: 1.8-2.0x

**Glass Spheres**:
- 分辨率: 1920x1080
- 三角形数: ~5K
- 材质: 玻璃（透射）
- 光源: 环境光
- 预期加速: 2.0-2.5x

**Rungholt**:
- 分辨率: 1920x1080
- 三角形数: ~6M
- 材质: 混合（漫反射、光滑反射）
- 光源: 环境光
- 预期加速: 2.5-3.0x

### D.2 压力测试场景

**High Resolution**:
- 分辨率: 3840x2160 (4K)
- 测试内存管理

**Long Paths**:
- MaxPathLength: 100
- 测试路径管理

**Complex Materials**:
- 所有材质类型混合
- 测试材质分类和排序

---

## 附录 E: 代码模板

### E.1 Kernel 模板

```cpp
// ============================================================================
// Wavefront [Kernel Name] Kernel
// 
// 功能: [描述]
// 输入: [列出输入]
// 输出: [列出输出]
// ============================================================================

#include "../shared/wavefront_common.h"

namespace vlr {
    using namespace shared;

    CUDA_DEVICE_KERNEL void wavefront[KernelName]() {
        // 获取工作索引
        uint32_t workIndex = blockIdx.x * blockDim.x + threadIdx.x;
        if (workIndex >= wlp.activePathQueue.size())
            return;
        
        // 获取路径索引
        uint32_t pathIndex = wlp.activePathQueue.pathIndices[workIndex];
        WavefrontPathState& pathState = wlp.pathStateBuffer[pathIndex];
        
        // 检查路径是否活跃
        if (!pathState.isActive())
            return;
        
        // === Kernel 主要逻辑 ===
        
        // ...
        
        // === 更新路径状态 ===
        
        // ...
    }

} // namespace vlr
```

### E.2 OptiX Program 模板

```cpp
// ============================================================================
// Wavefront [Program Name]
// 
// 类型: [Ray Generation / Closest Hit / Miss / Any Hit]
// 功能: [描述]
// ============================================================================

namespace vlr {
    using namespace shared;

    CUDA_DEVICE_KERNEL void RT_[TYPE]_NAME(wavefront[ProgramName])() {
        // 获取 Payload
        [PayloadType] payload;
        [PayloadSignature]::get(&payload);
        
        // === Program 主要逻辑 ===
        
        // ...
        
        // 设置 Payload（如果需要）
        [PayloadSignature]::set(&payload);
    }

} // namespace vlr
```

---

## 附录 F: 参考资源链接

### F.1 论文
- [Laine2013] Megakernels Considered Harmful: Wavefront Path Tracing on GPUs
  - https://research.nvidia.com/publication/2013-07_megakernels-considered-harmful-wavefront-path-tracing-gpus

- [Pharr2023] Physically Based Rendering (4th Edition)
  - https://pbr-book.org/

- [Novák2010] Understanding the Efficiency of Ray Traversal on GPUs
  - https://www.cgg.cvut.cz/~havran/ARTICLES/cgf2010.pdf

### F.2 开源实现
- PBRT-v4 (Wavefront 参考实现)
  - https://github.com/mmp/pbrt-v4

- OptiX 7 Samples
  - https://github.com/NVIDIA/OptiX_Apps

### F.3 工具文档
- CUDA C++ Programming Guide
  - https://docs.nvidia.com/cuda/cuda-c-programming-guide/

- OptiX 7 Programming Guide
  - https://raytracing-docs.nvidia.com/optix7/guide/

- CUB Documentation
  - https://nvlabs.github.io/cub/

- Nsight Systems User Guide
  - https://docs.nvidia.com/nsight-systems/

- Nsight Compute User Guide
  - https://docs.nvidia.com/nsight-compute/

---

## 附录 G: 常见问题 FAQ

### Q1: 为什么选择 Wavefront 而不是 Megakernel？
**A**: Wavefront 将不同操作分离到不同 kernel，减少分支发散，提高 GPU 利用率。Megakernel 虽然简单，但在复杂场景下性能较差。

### Q2: Wavefront 的内存开销有多大？
**A**: 对于 1080p，大约需要 600-800 MB 额外显存。可以通过分块渲染减少开销。

### Q3: 是否会移除递归式 Path Tracing？
**A**: 不会。Wavefront 作为新选项添加，用户可以选择使用哪种渲染器。

### Q4: Wavefront 是否支持所有现有特性？
**A**: 是的。Wavefront 将支持所有现有的材质、光源、相机类型和特性。

### Q5: 如何在我的项目中使用 Wavefront？
**A**: 
```cpp
context->setRenderer(VLRRenderer_WavefrontPathTracing);
context->render(stream, camera, enableDenoiser, 1, firstFrame, &numAccumFrames);
```

### Q6: Wavefront 是否支持双向路径追踪（BPT）？
**A**: 当前版本只支持单向路径追踪。BPT 的 Wavefront 版本是未来的优化方向。

### Q7: 性能提升在所有场景下都一致吗？
**A**: 不一致。复杂场景（多种材质）提升更明显，简单场景提升较小。

### Q8: 如何调试 Wavefront 渲染问题？
**A**: 使用内置的调试渲染模式，可视化路径长度、材质类别、吞吐量等信息。

---

## 附录 H: 术语表

| 术语 | 英文 | 解释 |
|-----|------|------|
| 递归式路径追踪 | Recursive Path Tracing | 每个线程独立处理一条完整路径 |
| Wavefront 路径追踪 | Wavefront Path Tracing | 分阶段处理所有路径的相同操作 |
| 路径状态 | Path State | 存储单条路径的完整信息 |
| 工作队列 | Work Queue | 管理活跃路径索引的队列 |
| Stream Compaction | Stream Compaction | 移除无效元素，压缩数组 |
| 分支发散 | Branch Divergence | Warp 内线程执行不同分支 |
| 合并访问 | Coalesced Access | 连续线程访问连续内存 |
| 俄罗斯轮盘赌 | Russian Roulette | 随机终止低贡献路径 |
| MIS | Multiple Importance Sampling | 多重重要性采样 |
| NEE | Next Event Estimation | 显式光源采样 |
| BSDF | Bidirectional Scattering Distribution Function | 双向散射分布函数 |
| EDF | Emission Distribution Function | 发射分布函数 |
| IDF | Importance Distribution Function | 重要性分布函数 |

---

## 版本历史

- **v1.0** (2026-03-06): 初始版本
  - 完整的实现计划
  - 详细的任务分解
  - 风险管理和质量保证

---

**文档维护者**: VLR 开发团队  
**最后更新**: 2026-03-06
