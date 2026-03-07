# PTX 编译简化修复方案

## 问题
`wavefront_trace_rays.cu` 编译 PTX 时，包含链导致大量不需要的类型定义（BSDF、EDF、材质等），这些类型又依赖其他头文件，导致编译失败。

## 采用的策略
**策略 1 + 策略 3 组合**：创建最小化头文件 + basic_types.h 条件编译

## 修改的文件

### 1. 新建 `libVLR/shared/wavefront_types_minimal.h`
- 仅包含 TraceRays 需要的类型
- WavefrontPathState、WavefrontHitInfo、WFTracePayload、WavefrontLaunchParameters
- 布局与 wavefront_types.h 完全一致
- 前向声明 Texture2DDescriptor，避免 texture_types.h
- 不包含 material_types.h、texture_types.h

### 2. 修改 `libVLR/include/vlr/basic_types.h`
- 添加 `#if !defined(VLR_PTX_TRACERAYS)` 保护
- PTX 编译时跳过 BSDFQuery、BSDFSample、BSDF、EDFQuery、EDF 定义

### 3. 修改 `libVLR/GPU_kernels/wavefront_trace_rays.cu`
- 使用 `#include "../shared/wavefront_types_minimal.h"` 替代 wavefront_types.h
- 在 OptiX 包含前添加 _WIN64 和 CUdeviceptr 定义

### 4. 修改 `libVLR/CMakeLists.txt`
- PTX 编译命令添加 `-DVLR_PTX_TRACERAYS` 和 `-D_WIN64`

## 额外修复（关键）
basic_types.h 中的中文注释（如 `// 占位符`）在 PTX 编译时导致 nvcc 解析错误。已改为 `/* placeholder */`。

## 验证
```bash
cd build
cmake --build . --target wavefront_trace_rays_ptx
```
