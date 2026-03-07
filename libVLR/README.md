# libVLR - Wavefront Path Tracing Implementation

## 概述

这是VLR Wavefront渲染器的核心库实现。当前包含基础数据结构定义和公共函数库。

## 目录结构

```
libVLR/
├── shared/                          # 共享代码（主机+设备端）
│   ├── wavefront_types.h           # 核心数据结构定义
│   ├── wavefront_common.h          # 公共函数库
│   ├── kernel_common.h             # kernel公共头文件
│   └── wavefront_types_test.cu     # 测试程序
├── GPU_kernels/                     # GPU kernel实现（待实现）
│   ├── wavefront_generate_rays.cu  # (待创建) 生成光线
│   ├── wavefront_trace_rays.cu     # (待创建) 追踪光线
│   ├── wavefront_process_hits.cu   # (待创建) 处理命中
│   ├── wavefront_sample_lights.cu  # (待创建) 采样光源
│   └── wavefront_sample_bsdf.cu    # (待创建) 采样BSDF
└── include/vlr/                     # 公共API头文件
    ├── public_types.h               # 公共类型定义
    └── basic_types.h                # 基础数学类型
```

## 核心数据结构

### WavefrontPathState (144 bytes)
存储单条光线路径的完整信息：
- 光线信息（origin, direction）
- 光谱信息（throughput, contribution, wavelengths）
- RNG状态
- 路径历史
- 像素坐标
- 状态标志

### WavefrontHitInfo (32 bytes)
存储光线求交结果：
- 几何信息（instance, primitive indices）
- 参数化坐标（u, v, t）
- 命中标志

### WavefrontWorkQueue
管理活跃路径的工作队列：
- 线程安全的enqueue/dequeue操作
- 原子计数器
- 支持多队列（材质分类）

### WavefrontLaunchParameters (912 bytes)
GPU启动参数，包含：
- 场景数据（几何、材质、光源）
- 相机数据
- 缓冲区指针
- 工作队列
- 配置参数

## 编译和测试

### 环境要求
- **CUDA**: 13.1+
- **Visual Studio**: 2022
- **OptiX**: 8.0.0
- **GPU**: 支持sm_75+（RTX 2000系列或更高）

### 编译测试程序

```powershell
# 进入shared目录
cd libVLR\shared

# 使用VS2022环境编译
cmd /c "call `"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat`" && nvcc -std=c++17 -arch=sm_75 -I../../libVLR/include wavefront_types_test.cu -o wavefront_types_test.exe"

# 运行测试
.\wavefront_types_test.exe
```

### 预期输出

```
=== VLR Wavefront Data Structure Size Verification ===

WavefrontPathState:
  Size: 144 bytes (Expected: 144 bytes)
  ✓ Size is correct!

WavefrontHitInfo:
  Size: 32 bytes (Expected: 32 bytes)
  ✓ Size is correct!

...

✓ Device kernel compiled and executed successfully!
```

## 使用示例

### 包含头文件

```cpp
// 在kernel文件中
#include "shared/path_types.h"
#include "shared/render_common.h"

// 在主机端代码中
#include "vlr/public_types.h"
#include "vlr/basic_types.h"
```

### 初始化路径状态

```cuda
__global__ void generateRaysKernel() {
    uint32_t pathIndex = blockIdx.x * blockDim.x + threadIdx.x;
    
    WavefrontPathState& pathState = pathStateBuffer[pathIndex];
    
    // 初始化路径
    initializePathState(pathState, pixelX, pixelY, rng);
    
    // 生成相机光线
    generateCameraRay(pathState, screenX, screenY, camera);
    
    // 加入活跃队列
    activePathQueue.enqueue(pathIndex);
}
```

### 材质分类

```cuda
__global__ void processHitsKernel() {
    // 获取路径
    uint32_t pathIndex = activePathQueue.pathIndices[workIndex];
    WavefrontPathState& pathState = pathStateBuffer[pathIndex];
    
    // 评估BSDF
    BSDF<TransportMode::Radiance> bsdf(matDesc, surfPt, pathState.wls);
    
    // 分类材质
    MaterialCategory category = classifyMaterial(bsdf);
    pathState.materialCategory = category;
    
    // 加入对应的材质队列
    materialQueues.enqueueByCategory(pathIndex, category);
}
```

### 俄罗斯轮盘赌

```cuda
// 检查是否应该终止路径
if (shouldTerminatePath(pathState)) {
    pathState.setTerminated();
    return;
}
// 路径继续，throughput已自动调整
```

## 配置选项

### WavefrontConfig常量

```cpp
namespace WavefrontConfig {
    constexpr uint32_t DefaultMaxPathLength = 25;
    constexpr uint32_t RRStartDepth = 3;
    constexpr float RRThreshold = 0.05f;
    constexpr uint32_t BlockSize = 256;
    constexpr bool UsePathSorting = true;
    constexpr bool UseMaterialQueues = true;
    constexpr bool UseStreamCompaction = true;
}
```

可以通过修改这些常量来调整Wavefront渲染器的行为。

## 性能考虑

### 内存占用（1920x1080）
- PathState缓冲区：~288 MB
- HitInfo缓冲区：~64 MB
- SurfacePoint缓冲区：~150 MB
- 工作队列：~8 MB
- **总计**：~510 MB

### 优化策略
1. **路径排序**：按材质类型排序，减少分支发散
2. **Stream Compaction**：移除终止的路径，保持队列紧凑
3. **材质队列**：分离不同材质类型，提高SIMD效率
4. **SoA布局**：可选的Structure of Arrays布局，优化内存访问

## 调试功能

### 调试模式
```cpp
enum WavefrontDebugMode {
    WFDebug_None,
    WFDebug_PathLength,        // 可视化路径长度
    WFDebug_MaterialCategory,  // 可视化材质分类
    WFDebug_Throughput,        // 可视化吞吐量
    WFDebug_NumBounces,        // 可视化弹射次数
    ...
};
```

### 验证函数
```cuda
// 验证路径状态
if (!validatePathState(pathState)) {
    printf("Invalid path state detected!\n");
}

// 打印路径状态
printPathState(pathState, "After BSDF sampling");
```

## 已知问题

1. **编码警告C4819**：Windows中文环境下的编码警告，不影响功能
2. **占位符实现**：部分依赖类型（BSDF, EDF, IDF等）使用占位符，需要后续集成真实实现
3. **OptiX集成**：当前OptiX头文件是可选的，完整实现时需要启用

## 参考文档

- `docs/wavefront_design.md` - 完整架构设计
- `docs/wavefront_implementation_plan.md` - 实施计划
- `docs/wavefront_data_structures.h` - 参考实现
- `docs/IMPLEMENTATION_LOG.md` - 实施日志

## 版本信息

- **版本**: 1.0.0-alpha
- **创建日期**: 2026-03-07
- **状态**: 数据结构定义完成，kernel实现待开始
