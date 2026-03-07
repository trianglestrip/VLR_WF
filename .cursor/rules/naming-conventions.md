# 命名规范

## 验证日期
2026-03-07

## 命名原则

### 避免冗余前缀
- **当前分支就是 wavefront，不需要在文件名、函数名、打印信息中重复 "wavefront" 前缀**
- 保持简洁明了的命名

---

## 文件命名

### GPU Kernel 文件
```
# 避免（冗余）
wavefront_generate_rays.cu
wavefront_trace_rays.cu
wavefront_process_hits.cu

# 推荐（简洁）
generate_rays.cu
trace_rays.cu
process_hits.cu
```

### 头文件
```
# 避免（冗余）
wavefront_types.h
wavefront_common.h

# 推荐（简洁）
path_types.h
render_common.h
```

---

## 函数命名

### Kernel 函数
```cpp
// 避免（冗余）
extern "C" __global__ void wavefrontGenerateRays(...);
extern "C" __global__ void wavefrontProcessHits(...);

// 推荐（简洁）
extern "C" __global__ void generateRays(...);
extern "C" __global__ void processHits(...);
```

### 启动函数
```cpp
// 避免（冗余）
void Context::launchWavefrontGenerateRays();
void Context::executeWavefrontRender();

// 推荐（简洁）
void Context::launchGenerateRays();
void Context::executeRender();
```

---

## 数据结构命名

### 结构体
```cpp
// 避免（冗余）
struct WavefrontPathState { ... };
struct WavefrontHitInfo { ... };
struct WavefrontWorkQueue { ... };

// 推荐（简洁）
struct PathState { ... };
struct HitInfo { ... };
struct WorkQueue { ... };
```

### 枚举
```cpp
// 避免（冗余）
enum WavefrontDebugMode { ... };

// 推荐（简洁）
enum DebugMode { ... };
```

---

## 运行时输出

### 打印信息
```cpp
// 避免（冗余 + 中文）
printf("[Wavefront] 开始初始化管线...\n");
printf("[Wavefront] 构建加速结构...\n");

// 推荐（简洁 + 英文）
printf("[VLR] Initializing pipeline...\n");
printf("[VLR] Building acceleration structure...\n");
```

### 错误消息
```cpp
// 避免（中文）
throw std::runtime_error("OptiX 上下文未初始化");
fprintf(stderr, "[错误] 创建 Context 失败\n");

// 推荐（英文）
throw std::runtime_error("OptiX context not initialized");
fprintf(stderr, "[Error] Failed to create context\n");
```

### 日志前缀
```
[VLR]       - 一般信息
[Error]     - 错误
[Warning]   - 警告
[Debug]     - 调试信息
```

---

## 变量命名

### 局部变量
```cpp
// 保持简洁
PathState pathState;
HitInfo hitInfo;
WorkQueue activeQueue;

// 不需要前缀
// WavefrontPathState wfPathState;  // ❌ 冗余
```

### 成员变量
```cpp
// Context 成员
struct WavefrontData {
    // 避免（冗余）
    cudau::Buffer<WavefrontPathState>* wavefrontPathStateBuffer;
    
    // 推荐（简洁）
    cudau::Buffer<PathState>* pathStateBuffer;
};
```

---

## 命名空间

### 保持现有结构
```cpp
namespace vlr {
    namespace shared {
        // 共享类型定义
    }
    namespace optixu {
        // OptiX 工具类
    }
    namespace cudau {
        // CUDA 工具类
    }
}
```

---

## 特殊情况

### 需要保留前缀的场景
1. **区分不同渲染器**：如果将来有多个渲染器实现
   ```cpp
   enum RendererType {
       Renderer_PathTracing,
       Renderer_Wavefront,      // 这里保留以区分
       Renderer_Bidirectional
   };
   ```

2. **外部 API**：C API 需要前缀避免命名冲突
   ```cpp
   // C API 保留 VLR 前缀
   VLRResult vlrCreateContext(...);
   VLRResult vlrRender(...);
   ```

3. **宏定义**：避免全局命名冲突
   ```cpp
   #define VLR_M_PI 3.14159265358979323846
   #define VLR_MAX_PATH_LENGTH 64
   ```

---

## 记住的要点

1. ✅ 文件名、函数名、结构体名保持简洁，不重复 "wavefront"
2. ✅ 运行时输出（printf、throw、assert）必须使用英文
3. ✅ 代码注释可以使用中文
4. ✅ C API 和宏定义保留 VLR 前缀
5. ✅ 日志前缀使用 `[VLR]` 而非 `[Wavefront]`
