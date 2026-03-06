# Wavefront 项目启动指南

> 从零开始实施 Wavefront 路径追踪的完整指南

---

## 🎯 第一步：环境准备

### 1.1 确认开发环境
```bash
# 检查 CUDA 版本
nvcc --version
# 需要: CUDA 12.5 或更高

# 检查 GPU
nvidia-smi
# 需要: RTX 20 系列或更高（支持 OptiX 7）

# 检查 OptiX
# 确保已安装 OptiX 8.0.0
```

### 1.2 创建开发分支
```powershell
# Windows PowerShell（已完成）
cd f:/project/VLR
git checkout -b wavefront-renderer
# 稍后推送: git push -u origin wavefront-renderer
```

### 1.3 确认项目可编译
```bash
# 编译当前项目
cmake --build . --config Release

# 运行测试
./HostProgram
```

---

## 📚 第二步：阅读文档（2-3 小时）

### 必读文档（按顺序）

#### 1. 架构可视化（30 分钟）
```powershell
# 打开文档
code docs_wavefront/wavefront_architecture_diagram.md
```
**重点**:
- 第 1 节：整体架构对比
- 第 2 节：数据流图
- 第 4 节：Kernel 执行流程图

#### 2. 快速参考（30 分钟）
```powershell
code docs_wavefront/wavefront_quick_reference.md
```
**重点**:
- 7 个核心 Kernel 表格
- 核心数据结构
- 代码片段

#### 3. 架构设计（1-2 小时）
```powershell
code docs_wavefront/wavefront_design.md
```
**重点章节**:
- 第 1 章：概述
- 第 2 章：当前架构分析
- 第 3 章：Wavefront 架构设计
- 第 4 章：内核设计（重点！）
- 第 11 章：详细数据结构定义

#### 4. 数据结构参考（30 分钟）
```bash
code docs/wavefront_data_structures.h
```
**重点**:
- `WavefrontPathState` 定义
- `WavefrontHitInfo` 定义
- `WavefrontWorkQueue` 定义
- 辅助函数实现

---

## 🚀 第三步：开始实施

### Week 1: 数据结构定义

#### Day 1-2: 创建 wavefront_types.h
```bash
# 创建文件
touch libVLR/shared/wavefront_types.h

# 从参考实现复制基础结构
# 参考: docs/wavefront_data_structures.h
```

**任务清单**:
- [ ] 复制文件头和命名空间
- [ ] 定义 `WavefrontPathState` 结构体
  - [ ] 添加所有成员变量
  - [ ] 添加内联方法（isActive, setTerminated 等）
  - [ ] 添加 `static_assert` 检查大小
- [ ] 定义 `WavefrontHitInfo` 结构体
  - [ ] 添加所有成员变量
  - [ ] 添加内联方法
  - [ ] 添加 `static_assert` 检查大小
- [ ] 定义 `WavefrontWorkQueue` 结构体
  - [ ] 添加成员变量
  - [ ] 实现 `enqueue()` 方法
  - [ ] 实现 `size()` 和 `reset()` 方法
- [ ] 定义 `MaterialCategory` 枚举
- [ ] 定义 `WFTracePayload` 和签名
- [ ] 编译测试

**验收标准**:
```bash
# 编译测试
cmake --build . --config Debug

# 检查结构体大小
# 在测试代码中添加:
static_assert(sizeof(WavefrontPathState) == 144);
static_assert(sizeof(WavefrontHitInfo) == 32);
```

#### Day 3: 创建 wavefront_common.h
```bash
touch libVLR/shared/wavefront_common.h
```

**任务清单**:
- [ ] 实现 `classifyMaterial()` 函数
- [ ] 实现 `computeMISWeight()` 函数
- [ ] 实现 `shouldTerminatePath()` 函数
- [ ] 实现 `computeSurfacePoint()` 函数
- [ ] 实现 `processEnvironmentHit()` 函数
- [ ] 编译测试

**参考代码**:
```cpp
// 参考: docs/wavefront_data_structures.h 第 9 节
// 参考: libVLR/GPU_kernels/path_tracing.cu 现有实现
```

#### Day 4: 更新 public_types.h
```bash
code libVLR/include/vlr/public_types.h
```

**修改**:
```cpp
enum VLRRenderer {
    VLRRenderer_PathTracing = 0,
    VLRRenderer_LightTracing,
    VLRRenderer_BPT,
    VLRRenderer_WavefrontPathTracing,  // 新增这一行
    VLRRenderer_DebugRendering,
};
```

**验收**:
```bash
# 编译整个项目
cmake --build . --config Debug

# 确保无编译错误
```

#### Day 5: 扩展 context.h
```bash
code libVLR/context.h
```

**任务**:
- [ ] 在 `Context::OptiX` 结构体中添加 `WavefrontPathTracing` 子结构
- [ ] 参考 `PathTracing` 结构体的定义
- [ ] 添加必要的成员变量（见 design.md 第 5.1 节）
- [ ] 声明 `renderWavefront()` 方法

**参考**:
```cpp
// 参考: libVLR/context.h 第 166-183 行 (PathTracing 结构体)
// 参考: docs/wavefront_design.md 第 5.1 节
```

---

### Week 2-3: Context 初始化

#### Day 6-9: 实现 Context 初始化
```bash
code libVLR/context.cpp
```

**任务**:
- [ ] 在 `Context::Context()` 构造函数中添加 Wavefront Pipeline 初始化
  - [ ] 创建 Pipeline（参考 PathTracing 的实现）
  - [ ] 设置 Pipeline Options
  - [ ] 加载模块（先使用占位符 PTX）
  - [ ] 创建 Programs（先使用空实现）
  - [ ] 分配缓冲区
  - [ ] 初始化工作队列
  - [ ] 设置 Launch Parameters

**参考位置**:
```cpp
// 参考: libVLR/context.cpp 第 148-260 行
// Pipeline for Path Tracing 的实现
```

**占位符 PTX**:
```bash
# 创建空的 PTX 文件用于测试
mkdir -p libVLR/ptxes
touch libVLR/ptxes/wavefront_trace_rays.ptx
```

#### Day 10: 实现缓冲区管理
```bash
code libVLR/context.cpp
```

**任务**:
- [ ] 实现 `resizeWavefrontBuffers()` 方法
- [ ] 实现 `resetWavefrontQueues()` 方法
- [ ] 在析构函数中添加资源清理

**测试**:
```cpp
// 在 main.cpp 中测试
context->resizeWavefrontBuffers(1920, 1080);
// 检查是否有内存泄漏
```

---

### Week 4: 第一个 Kernel

#### Day 11-13: 实现 GenerateRays Kernel
```bash
# 创建文件
touch libVLR/GPU_kernels/wavefront_generate_rays.cu
code libVLR/GPU_kernels/wavefront_generate_rays.cu
```

**实现步骤**:

**1. 文件头和包含**:
```cpp
#include "../shared/wavefront_common.h"

namespace vlr {
    using namespace shared;
```

**2. 实现 Ray Generation Program**:
```cpp
CUDA_DEVICE_KERNEL void RT_RG_NAME(wavefrontGenerateRays)() {
    // 参考: libVLR/GPU_kernels/path_tracing.cu 第 30-59 行
    // 复制相机采样逻辑
    
    uint2 launchIndex = make_uint2(optixGetLaunchIndex().x, optixGetLaunchIndex().y);
    uint32_t pathIndex = launchIndex.y * wlp.imageSize.x + launchIndex.x;
    
    // 初始化 RNG
    KernelRNG rng = wlp.rngBuffer.read(launchIndex);
    
    // 波长采样
    float selectWLPDF;
    WavelengthSamples wls = WavelengthSamples::createWithEqualOffsets(
        rng.getFloat0cTo1o(), rng.getFloat0cTo1o(), &selectWLPDF);
    
    // 相机采样
    // ... (复制 path_tracing.cu 的代码)
    
    // 初始化 PathState
    WavefrontPathState& pathState = wlp.pathStateBuffer[pathIndex];
    pathState.origin = rayOrg;
    pathState.direction = rayDir;
    pathState.throughput = alpha;
    pathState.contribution = SampledSpectrum::Zero();
    pathState.rng = rng;
    pathState.wls = wls;
    pathState.initImportance = alpha.importance(wls.selectedLambdaIndex());
    pathState.prevDirPDF = We1Result.dirPDF;
    pathState.prevSampledType = We1Result.sampledType;
    pathState.pathLength = 0;
    pathState.pixelX = launchIndex.x;
    pathState.pixelY = launchIndex.y;
    pathState.flags = 0x1; // isActive = true
    
    // 加入活跃队列
    wlp.activePathQueue.enqueue(pathIndex);
}
```

**3. 编译为 PTX**:
```bash
# 在 CMakeLists.txt 中添加编译规则
# 或手动编译:
nvcc -ptx -arch=sm_86 \
  -I../shared -I../../include \
  -DVLR_Device \
  wavefront_generate_rays.cu \
  -o ../ptxes/wavefront_generate_rays.ptx
```

**4. 测试**:
```cpp
// 在 Context::renderWavefront() 中测试
wf.pipeline.launch(
    stream, wf.shaderBindingTable, 
    optixu::Dim3(imageSize.x, imageSize.y, 1),
    m_optix.wavefrontLaunchParamsOnDevice,
    sizeof(WavefrontLaunchParameters));

// 检查队列大小
uint32_t numActive;
cuMemcpyDtoH(&numActive, wf.queueCounters.getCUdeviceptr(), sizeof(uint32_t));
printf("Generated %u paths\n", numActive);
// 应该等于 imageSize.x * imageSize.y
```

---

## 🎯 第一个里程碑目标

### 目标：渲染纯色图像
**时间**: Week 1-4  
**目标**: 使用 Wavefront 渲染一个纯色图像（验证基础架构）

**步骤**:
1. ✅ 创建数据结构（Week 1）
2. ✅ 实现 Context 初始化（Week 2-3）
3. ✅ 实现 GenerateRays Kernel（Week 4）
4. ⏳ 实现简化的 AccumulateResults Kernel
   - 直接输出 throughput 作为颜色
   - 跳过光线追踪
5. ⏳ 测试渲染

**预期结果**:
- 渲染出一个图像（可能是纯白色或相机相关的颜色）
- 验证基础架构工作正常
- 验证缓冲区分配正确
- 验证队列操作正确

---

## 🔍 调试技巧

### 第一次运行可能遇到的问题

**问题 1: 编译错误**
```
错误: 'wlp' was not declared
解决: 确保在 .cu 文件中包含了正确的头文件
     确保定义了 RT_PIPELINE_LAUNCH_PARAMETERS
```

**问题 2: 链接错误**
```
错误: undefined reference to 'wavefrontGenerateRays'
解决: 确保 PTX 文件已生成
     确保 CMakeLists.txt 中添加了编译规则
```

**问题 3: 运行时错误**
```
错误: OptiX error: Invalid launch parameter
解决: 检查 Launch Parameters 是否正确上传到 GPU
     检查 Pipeline 是否正确链接
```

**问题 4: 黑屏**
```
错误: 渲染结果全黑
解决: 添加调试输出
     检查 PathState 初始化
     检查队列是否为空
```

### 调试输出示例
```cpp
// 在 GenerateRays kernel 中
if (launchIndex.x == 960 && launchIndex.y == 540) {
    printf("Center pixel: origin=(%g,%g,%g), dir=(%g,%g,%g)\n",
           pathState.origin.x, pathState.origin.y, pathState.origin.z,
           pathState.direction.x, pathState.direction.y, pathState.direction.z);
}

// 在 Context::renderWavefront() 中
uint32_t numActive;
cuMemcpyDtoH(&numActive, queueCounters.getCUdeviceptr(), sizeof(uint32_t));
printf("Depth %u: %u active paths\n", depth, numActive);
```

---

## 📋 每日开发清单

### 开始开发前
- [ ] 查看 `todo.md` 今天的任务
- [ ] 阅读相关文档章节
- [ ] 准备测试场景
- [ ] 设置调试输出

### 开发过程中
- [ ] 频繁编译测试
- [ ] 添加必要的调试输出
- [ ] 参考现有代码（path_tracing.cu）
- [ ] 参考快速参考文档

### 开发完成后
- [ ] 移除调试输出（或使用条件编译）
- [ ] 编译 Release 版本
- [ ] 运行测试场景
- [ ] 更新 `todo.md` 任务状态
- [ ] 提交代码

---

## 🎨 代码风格指南

### 命名规范
```cpp
// Kernel 名称
RT_RG_NAME(wavefrontGenerateRays)  // OptiX Ray Gen
RT_CH_NAME(wavefrontClosestHit)    // OptiX Closest Hit
RT_MS_NAME(wavefrontMiss)          // OptiX Miss
__global__ void wavefrontProcessHits()  // CUDA Kernel

// 变量名称
WavefrontPathState pathState;      // 完整名称
WavefrontHitInfo hitInfo;
SurfacePoint surfPt;               // 缩写（遵循现有风格）

// 缓冲区
pathStateBuffer
hitInfoBuffer
activePathQueue
```

### 注释规范
```cpp
// 好的注释（解释"为什么"）
// 使用 Power Heuristic (β=2) 计算 MIS 权重，
// 因为它在大多数情况下比 Balance Heuristic 更稳定
float MISWeight = (pdf1 * pdf1) / (pdf1 * pdf1 + pdf2 * pdf2);

// 不好的注释（只描述"是什么"）
// 计算 MIS 权重
float MISWeight = (pdf1 * pdf1) / (pdf1 * pdf1 + pdf2 * pdf2);

// 关键算法需要注释
// 复杂的数学公式需要注释
// 非显而易见的优化需要注释
// 显而易见的代码不需要注释
```

### 代码格式
```cpp
// 遵循现有代码风格
// 缩进: 4 空格
// 大括号: K&R 风格
// 行宽: 尽量 < 100 字符

// 示例
CUDA_DEVICE_KERNEL void wavefrontProcessHits() {
    uint32_t workIndex = blockIdx.x * blockDim.x + threadIdx.x;
    if (workIndex >= wlp.activePathQueue.size())
        return;
    
    uint32_t pathIndex = wlp.activePathQueue.pathIndices[workIndex];
    WavefrontPathState& pathState = wlp.pathStateBuffer[pathIndex];
    
    if (!pathState.isActive())
        return;
    
    // ...
}
```

---

## 🧪 测试策略

### 单元测试（每个 Kernel）
```cpp
// 测试 GenerateRays
void testGenerateRays() {
    // 1. 初始化 Context
    // 2. 调用 GenerateRays
    // 3. 检查 PathState 初始化
    // 4. 检查队列大小
    
    assert(numActive == width * height);
    assert(pathState[0].isActive());
    assert(pathState[0].pathLength == 0);
}
```

### 集成测试（多个 Kernel）
```cpp
// 测试 GenerateRays + TraceRays
void testGenerateAndTrace() {
    // 1. GenerateRays
    // 2. TraceRays
    // 3. 检查 HitInfo
    
    assert(hitInfo[0].hasHit());
}
```

### 端到端测试（完整渲染）
```cpp
// 测试完整渲染流程
void testFullRender() {
    // 1. 设置简单场景（Cornell Box）
    // 2. 调用 renderWavefront()
    // 3. 对比递归式结果
    
    float error = computeL2Error(wavefrontBuffer, recursiveBuffer);
    assert(error < 0.01f);  // 1% 误差
}
```

---

## 📊 进度跟踪

### 每周报告模板
```markdown
# Week X 进度报告

## 本周完成
- [x] Task X.X: XXX
- [x] Task X.X: XXX

## 遇到的问题
- 问题 1: XXX
  - 解决方案: XXX
- 问题 2: XXX
  - 解决方案: XXX

## 下周计划
- [ ] Task X.X: XXX
- [ ] Task X.X: XXX

## 风险和阻塞
- 无 / 有: XXX

## 性能数据（如果有）
- Kernel X: XX ms
- 加速比: X.Xx
```

### 每日记录模板
```markdown
# 2026-03-XX 开发日志

## 今日任务
- [ ] Task X.X.X: XXX

## 完成情况
- 完成: XXX
- 进行中: XXX
- 遇到问题: XXX

## 明日计划
- Task X.X.X: XXX

## 笔记
- XXX
```

---

## 🎓 学习资源

### 推荐学习顺序

#### 阶段 1: CUDA 基础（如果不熟悉）
- CUDA C++ Programming Guide
- CUDA Best Practices Guide
- 重点：线程层次、内存层次、同步

#### 阶段 2: OptiX 7 基础
- OptiX 7 Programming Guide
- OptiX 7 Samples
- 重点：Pipeline、Program、Payload、SBT

#### 阶段 3: 光线追踪理论
- PBRT Book (Chapter 13-16)
- 重点：Path Tracing、MIS、BSDF

#### 阶段 4: Wavefront 架构
- [Laine2013] 论文
- PBRT-v4 源码
- 本项目文档

### 代码阅读顺序

#### 1. 理解现有实现
```bash
# 按顺序阅读
1. libVLR/shared/kernel_common.h         # 基础类型
2. libVLR/shared/renderer_common.h       # BSDF/EDF 类
3. libVLR/shared/light_transport_common.h # Payload 定义
4. libVLR/GPU_kernels/path_tracing.cu    # 当前实现
```

#### 2. 理解 Context 管理
```bash
5. libVLR/context.h                      # Context 类定义
6. libVLR/context.cpp (行 108-260)       # Pipeline 初始化
7. libVLR/context.cpp (行 1400-1591)     # 渲染主循环
```

#### 3. 理解材质系统
```bash
8. libVLR/materials.h                    # 材质类定义
9. libVLR/GPU_kernels/materials.cu       # 材质实现
```

---

## 🔧 开发环境设置

### Visual Studio Code 配置
```json
// .vscode/settings.json
{
    "C_Cpp.default.includePath": [
        "${workspaceFolder}/libVLR/include",
        "${workspaceFolder}/libVLR/shared",
        "C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v12.5/include",
        "C:/ProgramData/NVIDIA Corporation/OptiX SDK 8.0.0/include"
    ],
    "C_Cpp.default.defines": [
        "VLR_Device",
        "VLR_ENABLE_WAVEFRONT"
    ],
    "files.associations": {
        "*.cu": "cuda-cpp",
        "*.cuh": "cuda-cpp"
    }
}
```

### CMake 配置
```cmake
# 在 libVLR/CMakeLists.txt 中添加
option(VLR_ENABLE_WAVEFRONT "Enable Wavefront Path Tracing" ON)

if(VLR_ENABLE_WAVEFRONT)
    message(STATUS "Wavefront Path Tracing: ENABLED")
    add_definitions(-DVLR_ENABLE_WAVEFRONT)
    
    # 添加 Wavefront 源文件
    # ...
endif()
```

---

## 📞 获取帮助

### 文档查询表

| 我想... | 查阅文档 | 章节 |
|--------|---------|------|
| 了解整体架构 | `wavefront_architecture_diagram.md` | 第 1 节 |
| 理解数据流 | `wavefront_architecture_diagram.md` | 第 2 节 |
| 实现某个 Kernel | `wavefront_design.md` | 第 4 节 |
| 查看代码示例 | `wavefront_quick_reference.md` | 第 11 节 |
| 了解数据结构 | `wavefront_data_structures.h` | 全文 |
| 查看任务清单 | `wavefront_implementation_plan.md` | 第 2 节 |
| 性能优化 | `wavefront_quick_reference.md` | 第 5 节 |
| 调试问题 | `wavefront_quick_reference.md` | 第 6 节 |
| 了解风险 | `wavefront_implementation_plan.md` | 第 3 节 |

### 问题类型和解决方案

| 问题类型 | 首先查阅 | 然后查阅 |
|---------|---------|---------|
| 编译错误 | 错误信息 | `wavefront_quick_reference.md` |
| 运行时错误 | `wavefront_quick_reference.md` 第 15 节 | cuda-memcheck |
| 性能问题 | `wavefront_quick_reference.md` 第 5 节 | Nsight 工具 |
| 架构问题 | `wavefront_design.md` | PBRT-v4 源码 |
| 任务不清楚 | `wavefront_implementation_plan.md` | `todo.md` |

---

## ✅ 第一周任务详细指南

### Day 1: 创建 wavefront_types.h

**时间**: 4-6 小时

**步骤**:
1. 创建文件
   ```powershell
   # Windows PowerShell
   New-Item libVLR/shared/wavefront_types.h -ItemType File
   ```

2. 添加文件头
   ```cpp
   #pragma once
   
   #include "kernel_common.h"
   
   namespace vlr::shared {
   ```

3. 复制 `WavefrontPathState` 定义
   - 从 `docs/wavefront_data_structures.h` 复制
   - 确保所有成员变量都有
   - 确保内联方法都有

4. 复制 `WavefrontHitInfo` 定义

5. 复制 `WavefrontWorkQueue` 定义

6. 复制其他必要的定义

7. 编译测试
   ```bash
   cmake --build . --config Debug
   ```

8. 提交代码
   ```bash
   git add libVLR/shared/wavefront_types.h
   git commit -m "[Wavefront] feat: add core data structures"
   ```

**验收**:
- ✅ 编译通过
- ✅ 结构体大小正确
- ✅ 代码已提交

---

### Day 2: 创建 wavefront_common.h

**时间**: 4-6 小时

**步骤**:
1. 创建文件
2. 实现 `classifyMaterial()` 函数
   - 参考 `docs/wavefront_data_structures.h`
   - 参考 `libVLR/shared/renderer_common.h` 的 BSDF 类
3. 实现 `computeMISWeight()` 函数
4. 实现其他辅助函数
5. 编译测试
6. 提交代码

---

### Day 3: 更新 public_types.h

**时间**: 2-3 小时

**步骤**:
1. 打开 `libVLR/include/vlr/public_types.h`
2. 找到 `VLRRenderer` 枚举
3. 添加 `VLRRenderer_WavefrontPathTracing`
4. 编译整个项目
5. 确保无错误
6. 提交代码

---

### Day 4-5: 扩展 context.h 和初步实现

**时间**: 8-12 小时

**步骤**:
1. 在 `context.h` 中添加 `WavefrontPathTracing` 结构体
2. 在 `context.cpp` 构造函数中添加初始化代码
3. 创建占位符 PTX 文件
4. 测试编译
5. 提交代码

---

## 🎉 第一周结束时

### 应该完成的内容
- ✅ `libVLR/shared/wavefront_types.h` 创建完成
- ✅ `libVLR/shared/wavefront_common.h` 创建完成
- ✅ `libVLR/include/vlr/public_types.h` 更新完成
- ✅ `libVLR/context.h` 扩展完成
- ✅ `libVLR/context.cpp` 初步实现完成
- ✅ 项目可以编译
- ✅ 基础架构就绪

### 庆祝 🎊
你已经完成了最重要的第一步！基础架构已经搭建好，接下来就是实现各个 Kernel 了。

---

## 🚀 继续前进

### 第二周开始
查看 `wavefront_implementation_plan.md` 的 **Task 1.5** 和 **Task 2.1**，继续实现：
- Context 初始化的完整实现
- 第一个 Kernel: GenerateRays

### 保持动力
- 每完成一个任务，更新 `todo.md`
- 看到进度条增长会很有成就感
- 遇到困难是正常的，参考文档和现有代码
- 记住：这是一个 3-4 个月的项目，不要着急

### 寻求帮助
- 查阅文档
- 参考 PBRT-v4 源码
- 使用调试工具
- 记录问题和解决方案

---

## 🎯 最终目标

### 项目完成时，你将拥有
- ✅ 一个高性能的 Wavefront 路径追踪渲染器
- ✅ 1.5-3x 的性能提升
- ✅ 完整的文档和测试
- ✅ 宝贵的 GPU 编程经验
- ✅ 对光线追踪的深入理解

### 这个项目将帮助你
- 📈 提升 CUDA/OptiX 编程能力
- 🧠 深入理解光线追踪算法
- 🚀 掌握 GPU 性能优化技巧
- 📚 学习大型项目的架构设计

---

**准备好了吗？让我们开始吧！** 🚀

**第一步**: 创建 `libVLR/shared/wavefront_types.h`  
**参考**: `docs/wavefront_data_structures.h`  
**加油！** 💪

---

**文档版本**: 1.0  
**创建日期**: 2026-03-06  
**维护者**: VLR 开发团队
