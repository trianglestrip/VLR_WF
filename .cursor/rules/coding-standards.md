# 编码规范

## 验证日期
2026-03-07

## 代码注释规范

### 注释语言
- **所有代码注释必须使用中文（简体）**
- 包括但不限于：
  - 单行注释 (`//`)
  - 多行注释 (`/* */`)
  - 文档注释 (Doxygen 风格)
  - TODO/FIXME/NOTE 等标记
  - 函数、类、结构体的说明注释

### 运行时输出语言 ⚠️ 重要
- **所有运行时输出必须使用英文**
- 包括但不限于：
  - `printf`、`fprintf` 等打印语句
  - `assert` 断言消息
  - 异常消息 (`throw std::runtime_error(...)`)
  - 日志输出
  - 错误提示
- **原因**：避免编码问题导致乱码或崩溃，特别是在 Windows 控制台和 CUDA/OptiX 运行时中

### 示例对比

#### 正确示例
```cpp
// 初始化波前路径追踪管线
void Context::initializeWavefrontPipeline() {
    printf("[Wavefront] Initializing pipeline...\n");
    
    if (!m_optix.context) {
        throw std::runtime_error("OptiX context not initialized");
    }
}
```

#### 错误示例（避免）
```cpp
// 初始化波前路径追踪管线
void Context::initializeWavefrontPipeline() {
    printf("[Wavefront] 开始初始化管线...\n");  // ❌ 运行时输出不要用中文
    
    if (!m_optix.context) {
        throw std::runtime_error("OptiX 上下文未初始化");  // ❌ 异常消息不要用中文
    }
}
```

### 示例

#### C++/CUDA 注释
```cpp
// 好的示例：使用中文注释
struct WavefrontPathState {
    // 当前路径的吞吐量
    float3 throughput;
    
    // 光线追踪深度
    uint32_t depth;
};

// 避免：使用英文注释
// Current path throughput
```

#### 文档注释
```cpp
/**
 * @brief 初始化波前路径追踪状态
 * @param pathIndex 路径索引
 * @return 初始化后的路径状态
 */
CUDA_DEVICE_FUNCTION WavefrontPathState initPathState(uint32_t pathIndex);
```

## 文件编码规范 ⚠️ 极其重要

### 换行符 - 必须 CRLF
- **所有文件必须使用 CRLF (Windows 风格) 换行符**
- **绝对不能使用 LF (Unix 风格) 换行符**
- 适用于所有文本文件：`.cpp`, `.h`, `.cu`, `.cuh`, `.md`, `.txt`, `.cmake`, `CMakeLists.txt` 等
- **原因**：NVCC 编译器在 Windows 上对 LF 换行符处理有严重问题，会导致编译失败

### 字符编码
- **所有文件必须使用 UTF-8 编码（带 BOM 或不带 BOM 均可）**
- 确保中文注释正确显示
- NVCC 编译时需要 `-Xcompiler=/utf-8` 选项

### Git 配置（项目级别）
```bash
# 在项目仓库中设置（不要用 --global）
git config core.autocrlf true
git config core.eol crlf

# 重新规范化所有文件为 CRLF
git add --renormalize .
```

### 验证换行符
```powershell
# 检查文件换行符类型
file <filename>

# 或使用 Git
git ls-files --eol
```

### 编辑器配置
在 `.editorconfig` 或编辑器设置中配置：
```
[*]
charset = utf-8
end_of_line = crlf
insert_final_newline = true
trim_trailing_whitespace = true

[*.{cpp,h,cu,cuh}]
indent_style = space
indent_size = 4
```

## 代码审查检查项

每次修改代码时，必须确保：
1. ✅ 所有新增注释使用中文
2. ✅ 文件使用 CRLF 换行符
3. ✅ 文件编码为 UTF-8
4. ✅ 没有英文注释混入（除非是特殊情况如引用外部文档）

## 特殊情况

### 可以保留英文的场景
- 第三方库的原始注释（不修改）
- 外部 API 文档的直接引用
- 代码中的英文标识符和关键字（这些不是注释）

### 技术术语
- 专业术语可以保留英文或使用中英文混合
- 示例：`// 初始化 OptiX context` 或 `// 初始化光线追踪上下文`
