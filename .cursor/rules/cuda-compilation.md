# CUDA/OptiX 编译规则

## 验证日期
2026-03-07

## 文件换行符规范 ⚠️ 重要

### 必须使用 CRLF
- **所有源代码文件必须使用 CRLF (Windows 风格) 换行符**
- 包括：`.cpp`, `.h`, `.cu`, `.cuh`, `.md`, `.txt`, `.cmake`, `CMakeLists.txt` 等
- **原因**：NVCC 编译器在 Windows 上对 LF 换行符的处理有问题，会导致编译失败

### Git 配置
```bash
# 在项目仓库中设置
git config core.autocrlf true
git config core.eol crlf

# 重新规范化所有文件
git add --renormalize .
```

### .editorconfig 配置
```ini
[*]
end_of_line = crlf
```

### 检查和转换
```powershell
# 检查文件换行符
file <filename>

# 批量转换为 CRLF（使用 dos2unix 工具）
unix2dos file.cpp

# 或使用 PowerShell
(Get-Content file.cpp -Raw) -replace "`n", "`r`n" | Set-Content file.cpp -NoNewline
```

---

## PTX 编译规则

### PTX 文件的特殊要求

#### 1. 最小化依赖
- PTX 编译的 `.cu` 文件应该最小化头文件依赖
- 避免包含复杂的 C++ 模板和 STL
- 创建 `*_minimal.h` 简化版本头文件用于 PTX 编译

**示例**：
```cpp
// wavefront_trace_rays.cu 使用简化头文件
#include "../shared/wavefront_types_minimal.h"  // 只包含必要类型
```

#### 2. 条件编译保护
在共享头文件中，使用条件编译跳过 PTX 不需要的复杂类型：

```cpp
// basic_types.h
#if !defined(VLR_PTX_TRACERAYS)
    // BSDF、EDF 等复杂结构体
    struct BSDF { ... };
#endif
```

#### 3. 宏定义
PTX 编译时添加特定宏：
```cmake
# CMakeLists.txt
-DVLR_PTX_TRACERAYS
-DVLR_Device
-DVLR_USE_OPTIX
-D_WIN64
```

#### 4. 数学常量
使用 `#define` 而非 `constexpr`，确保在所有编译模式下可用：
```cpp
// 正确
#define VLR_M_PI 3.14159265358979323846

// 避免（PTX 编译可能失败）
constexpr float VLR_M_PI = 3.14159265358979323846f;
```

---

## CUDA 编译规则

### 1. 头文件包含顺序
```cpp
// 推荐顺序
#include <cuda_runtime.h>        // CUDA 运行时
#include "../shared/kernel_common.h"  // 项目公共头
#include "../include/vlr/basic_types.h"  // 基础类型
#include "../shared/material_types.h"    // 材质类型
#include "../shared/geometry_types.h"    // 几何类型
```

### 2. 设备/主机函数修饰符
```cpp
// 设备和主机都可用
CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE
float myFunction(float x) { return x * 2; }

// 仅设备
CUDA_DEVICE_FUNCTION CUDA_INLINE
float deviceOnlyFunction(float x) { return x * 2; }

// 全局 kernel
extern "C" __global__
void myKernel(float* data) { ... }
```

### 3. 条件编译
```cpp
// 检测 CUDA 编译器
#if defined(__CUDACC__)
    // CUDA 特定代码
    #define MY_INLINE __forceinline__
#endif

#if !defined(__CUDACC__)
    // 主机代码
    #define MY_INLINE inline
#endif
```

**注意**：避免使用 `#else`，使用两个独立的 `#if` 块，避免 NVCC 预处理器问题。

### 4. 原子操作
```cpp
// 设备端
#if defined(__CUDACC__)
    #define atomicAdd ::atomicAdd
    #define atomicSub ::atomicSub
#endif

// 主机端
#if !defined(__CUDACC__)
    inline void atomicAdd(uint32_t* addr, uint32_t val) { *addr += val; }
    inline void atomicSub(uint32_t* addr, uint32_t val) { *addr -= val; }
#endif
```

### 5. 数学函数
```cpp
// 设备端使用 CUDA 内置函数
#if defined(__CUDACC__)
    float result = sqrtf(x);
    float result = fabsf(x);
    float result = sinf(x);
#endif

// 主机端使用 std::
#if !defined(__CUDACC__)
    float result = std::sqrt(x);
    float result = std::abs(x);
    float result = std::sin(x);
#endif
```

---

## OptiX 编译规则

### 1. OptiX 头文件包含
```cpp
#if defined(__CUDACC__) && defined(VLR_USE_OPTIX)
    #include <optix.h>
    #include <optix_device.h>
#endif
```

### 2. CUdeviceptr 定义
在包含 OptiX 前定义（避免类型冲突）：
```cpp
#if !defined(__CUDACC__)
    typedef unsigned long long CUdeviceptr;
#endif
```

### 3. 启动参数
```cpp
// OptiX 设备端通过 __constant__ 访问
#if defined(__CUDACC__) && defined(VLR_USE_OPTIX)
extern "C" __constant__ WavefrontLaunchParameters wlp;
#endif
```

---

## CMake 构建规则

### 1. CUDA 编译选项
```cmake
# 必须添加的选项
set(CMAKE_CUDA_FLAGS "${CMAKE_CUDA_FLAGS} --expt-relaxed-constexpr")
set(CMAKE_CUDA_FLAGS "${CMAKE_CUDA_FLAGS} -Xcompiler=/utf-8")  # 解决中文注释编码问题

# PTX 编译
-DVLR_Device
-DVLR_USE_OPTIX
-DVLR_PTX_TRACERAYS  # 用于条件编译
-D_WIN64
```

### 2. 输出目录统一
```cmake
# 所有可执行文件、库、PTX 输出到 bin/ 目录
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)

# PTX 输出
set(PTX_OUTPUT_DIR ${CMAKE_SOURCE_DIR}/bin/GPU_kernels)
```

### 3. 多配置生成器
```cmake
# 为 Visual Studio 等多配置生成器设置
foreach(OUTPUTCONFIG ${CMAKE_CONFIGURATION_TYPES})
    string(TOUPPER ${OUTPUTCONFIG} OUTPUTCONFIG)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${OUTPUTCONFIG} ${CMAKE_SOURCE_DIR}/bin)
endforeach()
```

---

## 常见编译错误及解决方案

### 1. C1019: 意外的 #else
**原因**：NVCC 预处理器对 `#else` 的处理有问题  
**解决**：使用两个独立的 `#if` 块
```cpp
// 避免
#ifdef __CUDACC__
    // ...
#else
    // ...
#endif

// 推荐
#if defined(__CUDACC__)
    // ...
#endif
#if !defined(__CUDACC__)
    // ...
#endif
```

### 2. C4819: 文件包含不能在当前代码页中表示的字符
**原因**：中文注释在非 UTF-8 编译环境下无法识别  
**解决**：添加 `-Xcompiler=/utf-8` 到 CUDA 编译选项

### 3. identifier "XXX" is undefined (PTX 编译)
**原因**：PTX 编译时包含了过多依赖，导致类型定义缺失  
**解决**：
- 创建 `*_minimal.h` 简化头文件
- 使用条件编译跳过不需要的类型定义
- 添加 `-DVLR_PTX_TRACERAYS` 宏

### 4. expected a declaration (设备函数)
**原因**：函数定义缺少 `CUDA_DEVICE_FUNCTION` 或 `CUDA_HOST_FUNCTION`  
**解决**：为所有在 `.h` 中定义的函数添加适当的修饰符

### 5. a type qualifier is not allowed on a nonmember function
**原因**：结构体成员函数的 `const` 限定符在 NVCC 中被误判为非成员函数  
**解决**：确保函数定义在结构体内部，并添加 `CUDA_HOST_FUNCTION`

### 6. incomplete type: __half / __nv_bfloat16
**原因**：CUB 在 RDC 模式下启用了 half 精度支持  
**解决**：在包含 CUB 前定义宏
```cpp
#define CCCL_DISABLE_FP16_SUPPORT
#define CCCL_DISABLE_BF16_SUPPORT
#include <cub/cub.cuh>
```

### 7. memoryClockRate 已废弃 (CUDA 13+)
**原因**：`cudaDeviceProp::memoryClockRate` 在 CUDA 13 中废弃  
**解决**：使用 `cudaDeviceGetAttribute()`
```cpp
int memClockKHz = 0;
cudaDeviceGetAttribute(&memClockKHz, cudaDevAttrMemoryClockRate, deviceId);
```

---

## 构建目录规范

### 目录结构
```
VLR_WF/
├── bin/                    # 所有输出文件（exe、dll、ptx）
│   ├── simple_render_test.exe
│   ├── VLR.dll
│   └── GPU_kernels/
│       └── wavefront_trace_rays.ptx
├── build/                  # CMake 构建目录（不提交）
├── libVLR/                 # 库源代码
├── test/                   # 测试程序
└── docs/                   # 文档
```

### .gitignore 配置
```gitignore
# 构建目录
build/
build2/
bin/

# CMake 生成文件
CMakeCache.txt
CMakeFiles/
```

---

## 编译验证流程

### 1. 清理构建
```powershell
Remove-Item -Recurse -Force build -ErrorAction SilentlyContinue
Remove-Item -Recurse -Force bin -ErrorAction SilentlyContinue
```

### 2. 配置 CMake
```powershell
cmake -B build -G "Visual Studio 17 2022" -A x64 -DVLR_CUDA_ARCH=75
```

### 3. 构建项目
```powershell
cmake --build build --config Release
```

### 4. 验证输出
```powershell
# 检查输出文件
Test-Path bin\simple_render_test.exe
Test-Path bin\VLR.dll
Test-Path bin\GPU_kernels\wavefront_trace_rays.ptx
```

### 5. 运行测试
```powershell
cd bin
.\simple_render_test.exe -w 512 -h 512 -s 16 -o test.ppm
```

---

## 调试技巧

### 1. 查看详细编译输出
```powershell
cmake --build build --config Release --verbose
```

### 2. 只编译 PTX
```powershell
cmake --build build --target wavefront_trace_rays_ptx
```

### 3. 检查 CUDA 设备
```powershell
# 使用 deviceQuery 工具
"C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.1\extras\demo_suite\deviceQuery.exe"
```

### 4. 查看 PTX 内容
```powershell
# PTX 是文本文件，可以直接查看
Get-Content bin\GPU_kernels\wavefront_trace_rays.ptx | Select-Object -First 50
```

---

## 性能优化编译选项

### Release 构建
```cmake
# CMakeLists.txt
set(CMAKE_CUDA_FLAGS_RELEASE "-O3 -use_fast_math")
set(CMAKE_CXX_FLAGS_RELEASE "/O2 /Ob2")
```

### 计算能力
```cmake
# 根据 GPU 选择合适的 sm_XX
# RTX 2060/2070/2080: sm_75
# RTX 3060/3070/3080: sm_86
# RTX 4060/4070/4080: sm_89
set(VLR_CUDA_ARCH "75" CACHE STRING "CUDA 计算能力")
```

---

## 记住的关键点 ✅

1. ✅ **所有文件必须使用 CRLF 换行符**（最重要！）
2. ✅ PTX 编译使用简化的 `*_minimal.h` 头文件
3. ✅ 避免在条件编译中使用 `#else`，使用两个独立的 `#if`
4. ✅ 数学常量使用 `#define` 而非 `constexpr`
5. ✅ 中文注释需要 `-Xcompiler=/utf-8` 编译选项
6. ✅ 所有输出统一到 `bin/` 目录
7. ✅ CUB 使用前禁用 FP16/BF16 支持
8. ✅ 结构体成员函数需要 `CUDA_HOST_FUNCTION` 修饰符
