# 构建问题排查指南

## 验证日期
2026-03-08

## 常见构建错误及解决方案

### 1. 换行符问题 ⚠️ 最常见

#### 症状
- NVCC 编译时出现奇怪的语法错误
- 预处理器错误（C1019: 意外的 #else）
- 类型未定义错误（明明已经定义了）

#### 原因
文件使用了 LF 换行符而非 CRLF

#### 解决方案
```powershell
# 1. 设置 Git 配置
git config core.autocrlf true
git config core.eol crlf

# 2. 重新规范化所有文件
git add --renormalize .

# 3. 重新签出文件（如果需要）
git rm --cached -r .
git reset --hard
```

#### 预防措施
- 确保 `.editorconfig` 设置了 `end_of_line = crlf`
- 在编辑器中设置默认换行符为 CRLF
- 每次从其他系统复制文件后检查换行符

---

### 2. PTX 编译失败

#### 症状
- `identifier "XXX" is undefined`
- `expected a declaration`
- 大量类型未定义错误

#### 原因
PTX 编译包含了过多复杂依赖

#### 解决方案
1. 创建简化的 `*_minimal.h` 头文件
2. 使用条件编译跳过复杂类型：
```cpp
#if !defined(VLR_PTX_TRACERAYS)
    // 复杂类型定义
#endif
```
3. 在 CMake 中添加 `-DVLR_PTX_TRACERAYS`

---

### 3. 中文注释编译警告/错误

#### 症状
- C4819: 文件包含不能在当前代码页中表示的字符

#### 解决方案
在 CMakeLists.txt 中添加：
```cmake
set(CMAKE_CUDA_FLAGS "${CMAKE_CUDA_FLAGS} -Xcompiler=/utf-8")
```

---

### 4. CUB 编译错误

#### 症状
- `incomplete type: __half`
- `incomplete type: __nv_bfloat16`

#### 解决方案
在包含 CUB 前禁用 FP16/BF16：
```cpp
#define CCCL_DISABLE_FP16_SUPPORT
#define CCCL_DISABLE_BF16_SUPPORT
#include <cub/cub.cuh>
```

---

### 5. OptiX API 不匹配

#### 症状
- `optixTrace` 参数数量不对
- `optixModuleCreateFromPTX` 未定义

#### 解决方案
- OptiX 8 使用 `optixModuleCreate`（不是 `optixModuleCreateFromPTX`）
- `optixTrace` 需要 `float3` 类型的 origin 和 direction
- 包含 `optix_function_table_definition.h` 定义 `g_optixFunctionTable`

---

### 6. 链接错误

#### 症状
- LNK2019: 未解析的外部符号
- LNK4098: LIBCMT 与默认库冲突

#### 解决方案
1. 确保所有 `.cu` 文件都在 CMakeLists.txt 中列出
2. 检查函数声明和定义是否匹配
3. 对于 LIBCMT 冲突，在 CMake 中设置：
```cmake
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
```

---

### 7. 全局变量链接错误

#### 症状
- 多个编译单元中定义了相同的全局变量
- `wlp` 未定义

#### 解决方案
- 避免在头文件中定义全局变量
- 使用 `extern` 声明 + 单个 `.cu` 文件定义
- 或改为函数参数传递

---

## 构建流程最佳实践

### 完整构建流程
```powershell
# 1. 清理（如果需要）
Remove-Item -Recurse -Force build, bin -ErrorAction SilentlyContinue

# 2. 检查换行符（重要！）
git config core.autocrlf
git config core.eol
# 应该是: autocrlf=true, eol=crlf

# 3. 配置
cmake -B build -G "Visual Studio 17 2022" -A x64 -DVLR_CUDA_ARCH=75

# 4. 构建
cmake --build build --config Release

# 5. 验证输出
Test-Path bin\simple_render_test.exe
Test-Path bin\VLR.dll
Test-Path bin\GPU_kernels\wavefront_trace_rays.ptx

# 6. 运行测试
cd bin
.\simple_render_test.exe

# 7. 调试模式测试（可选）
# 修改 cornell_box_improved_test.cpp 添加 vlrSetDebugMode(context, mode)
# 渲染后保存为 debug_mode_XX.png
```

### 增量构建
```powershell
# 只重新编译修改的文件
cmake --build build --config Release

# 只编译特定目标
cmake --build build --target VLR --config Release
cmake --build build --target wavefront_trace_rays_ptx
```

---

## 调试编译问题的步骤

### 1. 确认环境
```powershell
# CUDA 版本
nvcc --version

# OptiX 路径
Test-Path "C:\ProgramData\NVIDIA Corporation\OptiX SDK 8.0.0"

# Visual Studio
where cl.exe
```

### 2. 检查文件换行符
```powershell
# 查看文件十六进制
Format-Hex file.cpp | Select-Object -First 20

# 0D 0A = CRLF (正确)
# 0A = LF (错误)
```

### 3. 逐个编译
```powershell
# 单独编译 PTX
cmake --build build --target wavefront_trace_rays_ptx

# 单独编译库
cmake --build build --target VLR --config Release
```

### 4. 查看详细错误
```powershell
# 详细输出
cmake --build build --config Release --verbose > build_log.txt 2>&1

# 查看错误
Get-Content build_log.txt | Select-String "error"
```

---

### 8. 单头文件库多重定义错误

#### 症状
- LNK2019: 无法解析的外部符号 `stbi_loadf`, `stbi_zlib_compress`, `mz_compress` 等
- LNK2005: 符号已经在其他对象中定义
- C2086: 重定义错误

#### 原因
单头文件库（如`stb_image.h`, `tinyexr.h`）的`*_IMPLEMENTATION`宏在多个编译单元中定义

#### 解决方案
1. **创建专用实现文件**（如`tinyexr_impl.cpp`）：
   ```cpp
   // tinyexr_impl.cpp - 统一管理所有单头文件库的实现
   
   // 定义 STB_IMAGE 实现（提供 zlib 和 HDR 加载）
   #define STB_IMAGE_IMPLEMENTATION
   #define STBI_NO_JPEG
   #define STBI_NO_PNG
   #define STBI_NO_BMP
   // ... 其他 STBI_NO_* 宏
   #include "stb/stb_image.h"
   
   // 定义 STB_IMAGE_WRITE 实现（提供 zlib 压缩）
   #define STB_IMAGE_WRITE_IMPLEMENTATION
   #define STBIW_ZLIB_COMPRESS stbi_zlib_compress
   #include "stb/stb_image_write.h"
   
   // 定义 tinyexr 实现（使用 STB zlib）
   #define TINYEXR_USE_MINIZ 0
   #define TINYEXR_USE_STB_ZLIB 1
   #define TINYEXR_IMPLEMENTATION
   #pragma warning(disable: 4717) // 递归警告
   #include "tinyexr/tinyexr.h"
   ```

2. **其他文件只包含头文件**：
   ```cpp
   // image_loader.cpp - 不定义 IMPLEMENTATION
   #include "stb/stb_image.h"
   #include "tinyexr/tinyexr.h"
   // 使用库函数...
   ```

3. **CMake配置**：
   ```cmake
   set(LIBVLR_CPP_SOURCES
       # ... 其他源文件
       libVLR/tinyexr_impl.cpp  # 专用实现文件
       libVLR/image_loader.cpp
   )
   ```

#### 关键点
- ✅ `*_IMPLEMENTATION`宏只在一个`.cpp`文件中定义
- ✅ 使用`TINYEXR_USE_STB_ZLIB=1`避免miniz编译问题
- ✅ 禁用不需要的stb_image格式以减少编译时间
- ✅ 使用`#pragma warning(disable: 4717)`忽略stb_zlib递归警告

---

## 记住的教训

1. ✅ **换行符必须是 CRLF**（最重要，忘记这个会浪费大量时间）
2. ✅ 中文注释需要 UTF-8 编码 + `-Xcompiler=/utf-8`
3. ✅ PTX 编译需要最小化依赖
4. ✅ 避免使用 `#else`，用两个 `#if` 代替
5. ✅ 所有输出统一到 `bin/` 目录
6. ✅ build/ 和 bin/ 目录不提交到 Git
7. ✅ 每次修改头文件后，清理重新构建
8. ✅ 使用 Task 并行执行独立的构建任务
9. ✅ **单头文件库的`*_IMPLEMENTATION`宏只在一个专用`.cpp`文件中定义**
10. ✅ 集成tinyexr时使用`TINYEXR_USE_STB_ZLIB=1`避免miniz问题
11. ✅ 从Git克隆第三方库后，删除`.git/`、`test/`、`examples/`、`*.md`等冗余文件
12. ✅ 调试模式修改 `debug_rendering.cu` 后需重新编译 VLR 目标
