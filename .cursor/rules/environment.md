# 开发环境配置

## 验证日期
2026-03-08

## 系统环境

### 操作系统
- **OS**: Windows 10/11 (Build 26200)
- **Shell**: PowerShell

### Visual Studio
- **版本**: Visual Studio 2022 Community
- **安装路径**: `C:\Program Files\Microsoft Visual Studio\2022\Community`
- **编译器**: MSVC (Microsoft Visual C++)
- **MSVC版本**: 14.16.27023 (动态查找)
- **cl.exe路径**: `C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.16.27023\bin\Hostx64\x64\cl.exe`
- **vcvars64.bat**: `C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat`

#### 查找cl.exe路径的方法
```powershell
# 方法1: 查找MSVC版本号
Get-ChildItem "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\" | Select-Object -First 1 -ExpandProperty Name

# 方法2: 使用vcvars64.bat初始化环境（推荐）
# 创建.bat文件执行编译命令
@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
cd /d <工作目录>
cl <编译参数>
```

### CUDA
- **版本**: CUDA 13.1 (Release 13.1.80)
- **安装路径**: `C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.1`
- **编译器**: nvcc (NVIDIA CUDA Compiler Driver)
- **环境变量**:
  - `CUDA_PATH`: `C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.1`
  - `CUDA_PATH_V13_1`: `C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.1`
  - `CUDA_PATH_V12_6`: `C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.6` (备用)

### OptiX
- **版本**: OptiX SDK 8.0.0
- **安装路径**: `C:\ProgramData\NVIDIA Corporation\OptiX SDK 8.0.0`

## 编码规则

### 构建系统
- 使用CMake作为构建系统
- 目标平台：Windows x64
- 支持CUDA 13.1和OptiX 8.0.0

### CUDA开发
- CUDA计算能力：根据目标GPU设置（建议sm_75+）
- 使用CUDA 13.1 API
- 确保代码与OptiX 8.0.0兼容

### C++标准
- 使用C++17或更高版本
- 与CUDA 13.1兼容的C++特性

### 命令行规则
- 使用Windows PowerShell命令语法
- 多个命令需要分开执行，不使用`&&`连接符（PowerShell中使用`;`或分开执行）

### 代码注释和文件格式
- **所有代码注释必须使用中文（简体）**
- **所有文件必须使用 CRLF 换行符（Windows 风格）**
- **所有文件必须使用 UTF-8 编码**
- 详细规则参见：`.cursor/rules/coding-standards.md`

### Git 提交规则
- 所有 Git 提交信息必须使用中文（简体）
- 使用 UTF-8 编码避免乱码
- 详细规则参见：`.cursor/rules/git-commit.md`

## 路径约定
- 项目根目录：`F:\project\VLR_WF`
- 文档目录：`docs/`
- 使用反斜杠`\`作为Windows路径分隔符（或正斜杠`/`在git中）

## API 参考（调试与可视化）

### 调试渲染模式 API (v1.2.0+)

| 函数 | 说明 |
|------|------|
| `vlrSetDebugMode(context, mode)` | 设置调试模式 (0-16) |
| `vlrGetDebugMode(context, &mode)` | 获取当前调试模式 |
| `vlrSetProbePixel(context, x, y)` | 设置探针像素（待完整实现） |

- **头文件**: `libVLR/include/vlr/vlr.h`
- **枚举**: `VLRDebugMode` 定义于 `libVLR/include/vlr/public_types.h`
- **详细说明**: 参见 `.cursor/rules/debug-mode.md`

## 构建与测试流程

### 完整构建
```powershell
# 配置
cmake -B build -G "Visual Studio 17 2022" -A x64 -DVLR_CUDA_ARCH=75

# 构建
cmake --build build --config Release

# 验证输出
Test-Path bin\simple_render_test.exe
Test-Path bin\VLR.dll
```

### 测试程序
- **基础渲染**: `cornell_box_improved_test.exe`
- **材质测试**: `material_test.exe`, `anisotropic_test.exe`, `disney_brdf_test.exe` 等
- **调试模式**: 修改测试程序调用 `vlrSetDebugMode()` 后渲染，保存为 PNG

### 调试模式测试
1. 在 `cornell_box_improved_test.cpp` 中，`vlrRender()` 前添加 `vlrSetDebugMode(context, mode)`
2. 使用 `numSamples=1`（调试模式自动单次采样）
3. 循环 mode 0-16 可批量导出所有调试视图

## TaskFlow 并行框架

### 基本信息
- **版本**: TaskFlow (Header-Only)
- **位置**: `external/taskflow/`（libVLR 和 viewer 各有一份副本）
- **用途**: CPU 端任务并行与 DAG 编排，用于加速场景准备阶段

### 使用方式
```cpp
#ifdef _WIN32
#undef min
#undef max
#endif
#include <taskflow/taskflow.hpp>
#include <taskflow/algorithm/for_each.hpp>  // 必须显式包含
```

### 已应用的并行优化
| 模块 | 优化内容 |
|------|----------|
| `Scene::buildGeometryAccelerationStructures()` | CPU 数据准备 parallel_for + 多 CUDA stream GAS 构建 |
| `Scene::computeSceneBounds()` | 每个 Instance 独立计算局部 AABB，reduce 合并 |
| `Scene::aggregateMeshData()` | prefix sum + parallel_for 拷贝各 mesh 数据到全局数组 |
| `Scene::prepareSceneParallel()` | TaskFlow DAG 编排：GAS ∥ Bounds ∥ Aggregate → Upload → IAS |
| `SceneLoader::buildFromPbrt()` | PBRT 材质并行创建（加锁保护 VLR API） |

### 性能统计宏
- **头文件**: `libVLR/vlr_profile.h`
- **CMake 开关**: `cmake -DVLR_PROFILE_SCENE_PREPARE=ON`（默认开启）
- **宏定义**:
  - `VLR_PROFILE_BEGIN(var)` — 记录起始时间点
  - `VLR_PROFILE_END(var, outMs)` — 耗时赋值到已有 double 变量
  - `VLR_PROFILE_END_NEW(var, newMs)` — 声明新 double 变量并赋值耗时
- **输出位置**:
  - `Scene::prepareSceneParallel()` — 各阶段耗时 + 并行加速比
  - `Context::renderWavefront()` — GPU 渲染耗时 + 端到端总时间

## 第三方库管理

### 已集成的库

#### tinyexr (v2.7MB)
- **位置**: `external/tinyexr/`
- **用途**: 加载EXR格式的HDR环境贴图
- **依赖**: miniz（zlib压缩）、nanozlib
- **集成方式**: 单头文件库，在`libVLR/tinyexr_impl.cpp`中定义`TINYEXR_IMPLEMENTATION`
- **配置**: 使用`TINYEXR_USE_STB_ZLIB=1`避免miniz编译问题

#### stb_image / stb_image_write
- **位置**: `external/stb/`
- **用途**: 
  - `stb_image.h`: 加载HDR格式图像，提供zlib解压缩
  - `stb_image_write.h`: 提供zlib压缩支持
- **集成方式**: 在`libVLR/tinyexr_impl.cpp`中统一定义`STB_IMAGE_IMPLEMENTATION`和`STB_IMAGE_WRITE_IMPLEMENTATION`

### 单头文件库集成规范

**重要原则**：避免多重定义错误

1. **创建专用实现文件**：`tinyexr_impl.cpp`
   - 在此文件中定义所有`*_IMPLEMENTATION`宏
   - 配置库的编译选项（如`TINYEXR_USE_STB_ZLIB`）
   - 禁用不需要的功能（如`STBI_NO_JPEG`）

2. **其他文件只包含头文件**
   - 不定义`*_IMPLEMENTATION`宏
   - 只声明接口

3. **CMake配置**
   - 将实现文件添加到`LIBVLR_CPP_SOURCES`
   - 添加必要的`include_directories`

### 第三方库清理规范

从Git仓库克隆第三方库后，应删除：
- `.git/` 目录
- `test/`, `tests/`, `examples/` 目录
- 构建配置文件（`.yml`, `.bat`, `.lua`, `CMakeLists.txt`）
- 示例图片和数据文件
- 文档文件（`*.md`，但保留`LICENSE`）

## 调试模式

- **17 种调试可视化**: BaseColor, GeometricNormal, ShadingNormal, Depth, UV, Tangent, Bitangent, Roughness, Metallic, MaterialID, InstanceID, PrimitiveID, DirectLighting, IndirectLighting, DenoiserAlbedo, DenoiserNormal
- **实现文件**: `libVLR/GPU_kernels/debug_rendering.cu`
- **性能**: 512×512 单次采样 < 100ms
- **详细文档**: `docs/DEBUG_MODE_IMPLEMENTATION.md`, `.cursor/rules/debug-mode.md`

## 开发最佳实践

1. **新增调试模式**: 在 `public_types.h` 添加枚举 → `debug_rendering.cu` 实现 → `kernel_launch.cu` 集成
2. **新增 API**: 在 `vlr.h` 声明 → `vlr.cpp` 实现 → `context.h/cpp` 添加状态
3. **修改头文件后**: 执行 `cmake --build build --target VLR --config Release` 或完整重建
4. **调试 GPU Kernel**: 使用 `cuda-memcheck` 或 Nsight Compute 分析
5. **参考变更**: 查看 `CHANGELOG.md` 和 `docs/TODO.md` 了解最新功能状态

## 开发注意事项
1. 确保CUDA和OptiX环境变量已正确设置
2. 使用Visual Studio 2022的开发者命令提示符进行编译
3. CMake配置时需要指定CUDA和OptiX路径
4. 所有GPU代码应使用`.cu`扩展名
5. OptiX程序应使用`.cu`文件并通过OptiX编译器编译
6. 集成新的单头文件库时，创建专用的`*_impl.cpp`文件
7. 调试模式开发时，修改 `debug_rendering.cu` 后需重新编译 `VLR` 目标
