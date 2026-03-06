# Wavefront 架构可视化图

## 1. 整体架构对比

### 递归式 Path Tracing（当前）
```
┌─────────────────────────────────────────────────┐
│           Ray Generation Kernel                 │
│                                                 │
│  for each pixel (x, y):                        │
│    ┌─────────────────────────────────────┐    │
│    │ Thread (x, y)                       │    │
│    │                                     │    │
│    │ 1. Generate camera ray              │    │
│    │ 2. Loop (depth = 0 to MaxDepth):   │    │
│    │    ├─ Trace ray (OptiX)            │    │
│    │    ├─ Hit surface                   │    │
│    │    ├─ Evaluate BSDF/EDF             │    │
│    │    ├─ Sample light (NEE)            │    │
│    │    ├─ Sample BSDF                   │    │
│    │    ├─ Russian Roulette              │    │
│    │    └─ Generate next ray             │    │
│    │ 3. Accumulate contribution          │    │
│    └─────────────────────────────────────┘    │
│                                                 │
└─────────────────────────────────────────────────┘

问题:
❌ 线程间执行路径差异大（分支发散）
❌ 某些线程早终止，浪费 GPU 资源
❌ GPU 占用率低 (40-60%)
```

### Wavefront Path Tracing（目标）
```
┌─────────────────────────────────────────────────────────────┐
│                  Wavefront Renderer                          │
├─────────────────────────────────────────────────────────────┤
│                                                              │
│  [Kernel 1] GenerateRays                                    │
│  ┌──────────────────────────────────────────────────────┐  │
│  │ for each pixel: generate camera ray                   │  │
│  │ Initialize PathState                                  │  │
│  │ Enqueue all paths to activeQueue                      │  │
│  └──────────────────────────────────────────────────────┘  │
│                           ↓                                  │
│  Loop: while (activeQueue not empty)                        │
│  │                                                           │
│  │  [Kernel 2] TraceRays (OptiX)                           │
│  │  ┌────────────────────────────────────────────────────┐ │
│  │  │ for each path in activeQueue:                      │ │
│  │  │   Trace ray, fill HitInfo                          │ │
│  │  └────────────────────────────────────────────────────┘ │
│  │                       ↓                                  │
│  │  [Kernel 3] ProcessHits                                 │
│  │  ┌────────────────────────────────────────────────────┐ │
│  │  │ for each path in activeQueue:                      │ │
│  │  │   Compute surface point                            │ │
│  │  │   Evaluate BSDF/EDF                                │ │
│  │  │   Implicit light sampling                          │ │
│  │  │   Classify material                                │ │
│  │  │   Check termination (RR, max length)               │ │
│  │  └────────────────────────────────────────────────────┘ │
│  │                       ↓                                  │
│  │  [Kernel 4] SampleLights                                │
│  │  ┌────────────────────────────────────────────────────┐ │
│  │  │ for each path in activeQueue:                      │ │
│  │  │   Select light source                              │ │
│  │  │   Sample light position                            │ │
│  │  │   Test visibility (shadow ray)                     │ │
│  │  │   Evaluate BSDF                                    │ │
│  │  │   Compute MIS weight                               │ │
│  │  │   Accumulate direct lighting                       │ │
│  │  └────────────────────────────────────────────────────┘ │
│  │                       ↓                                  │
│  │  [Kernel 5] SampleBSDF                                  │
│  │  ┌────────────────────────────────────────────────────┐ │
│  │  │ for each path in activeQueue:                      │ │
│  │  │   Sample BSDF                                      │ │
│  │  │   Update throughput                                │ │
│  │  │   Generate next ray                                │ │
│  │  │   Enqueue to nextActiveQueue                       │ │
│  │  └────────────────────────────────────────────────────┘ │
│  │                       ↓                                  │
│  │  [Kernel 6] CompactPaths                                │
│  │  ┌────────────────────────────────────────────────────┐ │
│  │  │ Stream compaction (remove terminated paths)        │ │
│  │  │ Swap activeQueue <-> nextActiveQueue               │ │
│  │  └────────────────────────────────────────────────────┘ │
│  │                       ↓                                  │
│  └──────────────────────────────────────────────────────────┘
│                           ↓                                  │
│  [Kernel 7] AccumulateResults                               │
│  ┌──────────────────────────────────────────────────────┐  │
│  │ for each pixel: accumulate contribution               │  │
│  │ Update RNG state                                      │  │
│  └──────────────────────────────────────────────────────┘  │
│                                                              │
└─────────────────────────────────────────────────────────────┘

优势:
✅ 所有线程执行相同操作（减少分支发散）
✅ 活跃线程数量更稳定
✅ GPU 占用率高 (80-95%)
✅ 性能提升 1.5-3x
```

---

## 2. 数据流图

```
┌─────────────┐
│   Pixels    │
│ (1920x1080) │
└──────┬──────┘
       │
       ↓ [GenerateRays]
┌──────────────────────────────────────────┐
│         PathState Buffer                 │
│  [0]: {origin, direction, throughput...} │
│  [1]: {origin, direction, throughput...} │
│  [2]: {origin, direction, throughput...} │
│  ...                                     │
│  [N]: {origin, direction, throughput...} │
└──────┬───────────────────────────────────┘
       │
       ↓ [TraceRays]
┌──────────────────────────────────────────┐
│          HitInfo Buffer                  │
│  [0]: {instIdx, primIdx, u, v...}       │
│  [1]: {instIdx, primIdx, u, v...}       │
│  ...                                     │
└──────┬───────────────────────────────────┘
       │
       ↓ [ProcessHits]
┌──────────────────────────────────────────┐
│       SurfacePoint Buffer                │
│  [0]: {position, normal, texCoord...}   │
│  [1]: {position, normal, texCoord...}   │
│  ...                                     │
└──────┬───────────────────────────────────┘
       │
       ├─→ [SampleLights] ──→ contribution
       │
       └─→ [SampleBSDF] ──→ 更新 PathState
                │
                ↓ [CompactPaths]
       ┌────────────────────┐
       │  Active Path Queue │
       │  [0, 5, 12, 18...] │ ← 只保留活跃路径
       └────────┬───────────┘
                │
                ↓ 下一次迭代
       
       最终 ↓ [AccumulateResults]
┌──────────────────────────────────────────┐
│         Accumulation Buffer              │
│  [pixel]: SpectrumStorage                │
└──────────────────────────────────────────┘
```

---

## 3. 内存布局

### PathState Buffer 布局（AoS）
```
Address     PathState[0]                PathState[1]                PathState[2]
0x0000      ┌─────────────────────┐    ┌─────────────────────┐    ┌─────────────────────┐
            │ origin (12B)        │    │ origin (12B)        │    │ origin (12B)        │
            │ direction (12B)     │    │ direction (12B)     │    │ direction (12B)     │
            │ padding (8B)        │    │ padding (8B)        │    │ padding (8B)        │
            ├─────────────────────┤    ├─────────────────────┤    ├─────────────────────┤
            │ throughput (16B)    │    │ throughput (16B)    │    │ throughput (16B)    │
            │ contribution (16B)  │    │ contribution (16B)  │    │ contribution (16B)  │
            │ wls (24B)           │    │ wls (24B)           │    │ wls (24B)           │
            │ initImportance (4B) │    │ initImportance (4B) │    │ initImportance (4B) │
            │ selectWLPDF (4B)    │    │ selectWLPDF (4B)    │    │ selectWLPDF (4B)    │
            ├─────────────────────┤    ├─────────────────────┤    ├─────────────────────┤
            │ rng (16B)           │    │ rng (16B)           │    │ rng (16B)           │
            ├─────────────────────┤    ├─────────────────────┤    ├─────────────────────┤
            │ prevDirPDF (4B)     │    │ prevDirPDF (4B)     │    │ prevDirPDF (4B)     │
            │ prevSampledType(4B) │    │ prevSampledType(4B) │    │ prevSampledType(4B) │
            │ pathLength (4B)     │    │ pathLength (4B)     │    │ pathLength (4B)     │
            │ padding (4B)        │    │ padding (4B)        │    │ padding (4B)        │
            ├─────────────────────┤    ├─────────────────────┤    ├─────────────────────┤
            │ pixelX, pixelY (8B) │    │ pixelX, pixelY (8B) │    │ pixelX, pixelY (8B) │
            ├─────────────────────┤    ├─────────────────────┤    ├─────────────────────┤
            │ flags (4B)          │    │ flags (4B)          │    │ flags (4B)          │
            │ materialCategory(4B)│    │ materialCategory(4B)│    │ materialCategory(4B)│
0x0090      └─────────────────────┘    └─────────────────────┘    └─────────────────────┘
            144 bytes                  144 bytes                  144 bytes
```

### Work Queue 布局
```
Active Path Queue:
┌────────────────────────────────────────────────────┐
│ pathIndices[]: [0, 1, 2, 3, 4, 5, ..., N-1]      │ ← 路径索引数组
└────────────────────────────────────────────────────┘
┌──────────┐
│ counter: │ N                                        │ ← 原子计数器
└──────────┘

迭代后（部分路径终止）:
Next Active Path Queue:
┌────────────────────────────────────────────────────┐
│ pathIndices[]: [0, 2, 5, 7, 9, ...]              │ ← 只包含活跃路径
└────────────────────────────────────────────────────┘
┌──────────┐
│ counter: │ M  (M < N)                              │
└──────────┘
```

---

## 4. Kernel 执行流程图

### 单次迭代详细流程

```
时间 →

T0: GenerateRays (OptiX Launch)
    ┌─────────────────────────────────────────────┐
    │ Launch Dim: (1920, 1080, 1)                │
    │ Each thread: 1 pixel                       │
    │                                             │
    │ Thread (0,0) → PathState[0]                │
    │ Thread (0,1) → PathState[1]                │
    │ Thread (1,0) → PathState[1920]             │
    │ ...                                         │
    │ Thread (1919,1079) → PathState[2073599]    │
    │                                             │
    │ All paths → activeQueue                    │
    └─────────────────────────────────────────────┘
                       ↓
T1: Depth 0 Iteration
    ┌─────────────────────────────────────────────┐
    │ TraceRays (OptiX Launch)                   │
    │ Launch Dim: (2073600, 1, 1)                │
    │ Each thread: 1 path from activeQueue       │
    │                                             │
    │ Thread 0 → Trace PathState[queue[0]]       │
    │ Thread 1 → Trace PathState[queue[1]]       │
    │ ...                                         │
    │ Output: HitInfo for each path              │
    └─────────────────────────────────────────────┘
                       ↓
T2: ProcessHits (CUDA Kernel)
    ┌─────────────────────────────────────────────┐
    │ Launch: <<<gridDim, blockDim>>>            │
    │ gridDim = (2073600 + 255) / 256            │
    │ blockDim = 256                              │
    │                                             │
    │ Each thread: 1 path from activeQueue       │
    │ - Compute SurfacePoint                     │
    │ - Evaluate BSDF/EDF                        │
    │ - Implicit light sampling                  │
    │ - Check termination                        │
    │ Output: SurfacePoint, updated PathState    │
    └─────────────────────────────────────────────┘
                       ↓
T3: SampleLights (CUDA Kernel)
    ┌─────────────────────────────────────────────┐
    │ Launch: <<<gridDim, blockDim>>>            │
    │                                             │
    │ Each thread: 1 path from activeQueue       │
    │ - Select light                             │
    │ - Sample light position                    │
    │ - Test visibility (shadow ray)             │
    │ - Evaluate BSDF                            │
    │ - Compute MIS weight                       │
    │ - Accumulate contribution                  │
    └─────────────────────────────────────────────┘
                       ↓
T4: SampleBSDF (CUDA Kernel)
    ┌─────────────────────────────────────────────┐
    │ Launch: <<<gridDim, blockDim>>>            │
    │                                             │
    │ Each thread: 1 path from activeQueue       │
    │ - Sample BSDF                              │
    │ - Update throughput                        │
    │ - Generate next ray                        │
    │ - Enqueue to nextActiveQueue               │
    │ Output: Updated PathState, nextActiveQueue │
    └─────────────────────────────────────────────┘
                       ↓
T5: CompactPaths (CUDA Kernel)
    ┌─────────────────────────────────────────────┐
    │ Stream Compaction                          │
    │ Remove terminated paths                    │
    │ Swap activeQueue ↔ nextActiveQueue         │
    │                                             │
    │ Before: activeQueue = [0,1,2,3,4,5,6,7,8,9]│
    │         (paths 2,5,7 terminated)           │
    │ After:  activeQueue = [0,1,3,4,6,8,9]     │
    └─────────────────────────────────────────────┘
                       ↓
                  Depth 1 Iteration
                  (重复 T1-T5)
                       ↓
                  Depth 2 Iteration
                       ↓
                      ...
                       ↓
T_final: AccumulateResults (CUDA Kernel)
    ┌─────────────────────────────────────────────┐
    │ Launch: <<<gridDim, blockDim>>>            │
    │                                             │
    │ Each thread: 1 pixel                       │
    │ - Read PathState[pixelIndex]               │
    │ - Accumulate contribution to accumBuffer   │
    │ - Update RNG state                         │
    └─────────────────────────────────────────────┘
```

---

## 5. 线程执行对比

### 递归式（Warp 内线程执行差异大）
```
Warp 0 (32 threads):
Thread 0:  ████████████████████████████ (28 bounces)
Thread 1:  ████████ (8 bounces)
Thread 2:  ████████████ (12 bounces)
Thread 3:  ████ (4 bounces)
Thread 4:  ████████████████ (16 bounces)
...
Thread 31: ████████████████████ (20 bounces)

问题: 
- Thread 1 在第 8 次弹射后终止，但必须等待 Thread 0 完成
- 大量线程空闲，GPU 利用率低
- 分支发散严重
```

### Wavefront（Warp 内线程执行一致）
```
Depth 0 (所有线程都执行):
Warp 0:  ████████████████████████████████ (32 threads active)
Warp 1:  ████████████████████████████████ (32 threads active)
...

Depth 1 (部分路径终止):
Warp 0:  ████████████████████████████████ (32 threads active)
Warp 1:  ████████████████████████████░░░░ (28 threads active)
...

Depth 5 (更多路径终止):
Warp 0:  ████████████████████████░░░░░░░░ (24 threads active)
Warp 1:  ████████████████░░░░░░░░░░░░░░░░ (16 threads active)
...

优势:
- 每个 Warp 内线程执行相同操作
- 分支发散最小化
- GPU 利用率高
- 通过 Stream Compaction 保持高效
```

---

## 6. 内存访问模式

### 递归式（随机访问）
```
Thread 0 访问:
  材质 A → BSDF 评估 → 光源 X → 材质 B → BSDF 评估 → ...
  
Thread 1 访问:
  材质 C → BSDF 评估 → 光源 Y → 材质 A → BSDF 评估 → ...
  
Thread 2 访问:
  材质 B → BSDF 评估 → 光源 X → 材质 C → BSDF 评估 → ...

问题: 
- 内存访问模式不规律
- 缓存命中率低
- 材质数据重复加载
```

### Wavefront（规律访问）
```
ProcessHits Kernel:
  所有线程同时:
    Thread 0-31:   读取 PathState[0-31]
    Thread 32-63:  读取 PathState[32-63]
    ...
    → 合并内存访问 (Coalesced)
    
SampleBSDF Kernel (排序后):
  材质 A 的路径:
    Thread 0-1023: 处理材质 A → 相同的 BSDF 代码路径
  
  材质 B 的路径:
    Thread 1024-2047: 处理材质 B → 相同的 BSDF 代码路径

优势:
- 内存访问规律
- 缓存命中率高
- 无分支发散
```

---

## 7. 路径生命周期

```
Pixel (100, 200) 的路径生命周期:

T0: GenerateRays
    ↓
    PathState[100*1920 + 200] 初始化
    {
      origin: Camera position
      direction: (0.1, 0.5, -0.9)
      throughput: (1.0, 1.0, 1.0)
      contribution: (0, 0, 0)
      pathLength: 0
      flags: 0x1 (isActive)
    }
    ↓
    Enqueue to activeQueue[192100]

Depth 0:
    TraceRays → HitInfo: {inst:5, prim:123, u:0.3, v:0.7}
    ↓
    ProcessHits → 
    {
      命中漫反射墙
      隐式光源采样: contribution += 0
      pathLength: 1
      flags: 0x1 (still active)
    }
    ↓
    SampleLights → contribution += (0.2, 0.2, 0.2)
    ↓
    SampleBSDF → 
    {
      新方向: (0.3, 0.8, 0.5)
      throughput: (0.8, 0.8, 0.8)
    }
    ↓
    Enqueue to nextActiveQueue[150000]

Depth 1:
    TraceRays → HitInfo: {inst:3, prim:45, u:0.5, v:0.5}
    ↓
    ProcessHits →
    {
      命中发光天花板
      隐式光源采样: contribution += (1.5, 1.5, 1.5)
      pathLength: 2
      flags: 0x2 (terminated)
    }
    ↓
    (不再加入 nextActiveQueue)

Final:
    AccumulateResults →
    accumBuffer[100, 200] += contribution
    = (0.2, 0.2, 0.2) + (1.5, 1.5, 1.5)
    = (1.7, 1.7, 1.7)
```

---

## 8. 队列管理示意图

### 初始状态（所有路径活跃）
```
activeQueue:
┌───┬───┬───┬───┬───┬───┬───┬───┬───┬───┐
│ 0 │ 1 │ 2 │ 3 │ 4 │ 5 │ 6 │ 7 │ 8 │ 9 │
└───┴───┴───┴───┴───┴───┴───┴───┴───┴───┘
counter: 10
```

### Depth 0 后（部分路径终止）
```
nextActiveQueue (SampleBSDF 填充):
┌───┬───┬───┬───┬───┬───┬───┐
│ 0 │ 1 │ 3 │ 4 │ 6 │ 8 │ 9 │  (路径 2, 5, 7 终止)
└───┴───┴───┴───┴───┴───┴───┘
counter: 7
```

### CompactPaths 后（交换队列）
```
activeQueue (交换后):
┌───┬───┬───┬───┬───┬───┬───┐
│ 0 │ 1 │ 3 │ 4 │ 6 │ 8 │ 9 │
└───┴───┴───┴───┴───┴───┴───┘
counter: 7

nextActiveQueue (重置):
┌───┬───┬───┬───┬───┬───┬───┬───┬───┬───┐
│   │   │   │   │   │   │   │   │   │   │
└───┴───┴───┴───┴───┴───┴───┴───┴───┴───┘
counter: 0 (重置)
```

---

## 9. 材质分类队列（可选优化）

### 未分类（所有路径混合）
```
activeQueue: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9]
材质类型:     [D, G, S, D, T, D, G, S, D, G]
             (D=Diffuse, G=Glossy, S=Specular, T=Transmissive)

SampleBSDF 执行:
  Thread 0: 处理 Diffuse  → 执行分支 A
  Thread 1: 处理 Glossy   → 执行分支 B
  Thread 2: 处理 Specular → 执行分支 C
  Thread 3: 处理 Diffuse  → 执行分支 A
  ...
  
分支发散严重！
```

### 分类后（按材质类型分组）
```
排序后:
activeQueue: [0, 3, 5, 8, 1, 6, 9, 2, 7, 4]
材质类型:     [D, D, D, D, G, G, G, S, S, T]

或使用材质队列:
diffuseQueue:      [0, 3, 5, 8]
glossyQueue:       [1, 6, 9]
specularQueue:     [2, 7]
transmissiveQueue: [4]

SampleBSDF 执行:
  处理 diffuseQueue:
    Thread 0-3: 全部处理 Diffuse → 相同分支
  
  处理 glossyQueue:
    Thread 0-2: 全部处理 Glossy → 相同分支
  
  处理 specularQueue:
    Thread 0-1: 全部处理 Specular → 相同分支

无分支发散！性能提升 15-25%
```

---

## 10. OptiX Pipeline 结构

```
┌─────────────────────────────────────────────────────────────┐
│              Wavefront OptiX Pipeline                        │
├─────────────────────────────────────────────────────────────┤
│                                                              │
│  Ray Generation Programs:                                   │
│  ├─ wavefrontGenerateRays                                   │
│  └─ wavefrontTraceRays                                      │
│                                                              │
│  Miss Programs:                                             │
│  ├─ wavefrontMiss (RayType: Closest)                       │
│  └─ (empty) (RayType: Shadow)                              │
│                                                              │
│  Hit Program Groups:                                        │
│  ├─ Closest Hit:                                            │
│  │   ├─ wavefrontClosestHit (default)                      │
│  │   └─ wavefrontClosestHit (with alpha)                   │
│  │                                                           │
│  ├─ Any Hit:                                                │
│  │   ├─ (none) (default)                                   │
│  │   └─ wavefrontAnyHitWithAlpha (with alpha)              │
│  │                                                           │
│  └─ Shadow Hit:                                             │
│      ├─ shadowAnyHitDefault                                 │
│      └─ shadowAnyHitWithAlpha                               │
│                                                              │
│  Callable Programs: (复用现有)                              │
│  ├─ BSDF Callables (setupBSDF, sample, evaluate, ...)     │
│  ├─ EDF Callables (setupEDF, sample, evaluate, ...)       │
│  ├─ IDF Callables (setupIDF, sample, ...)                 │
│  ├─ Node Callables (shader node evaluation)                │
│  └─ Geometry Callables (decode hit point, ...)             │
│                                                              │
└─────────────────────────────────────────────────────────────┘
```

---

## 11. Context 类结构

```cpp
class Context {
    struct OptiX {
        // 现有 Pipelines
        struct PathTracing { ... } pathTracing;
        struct LightTracing { ... } lightTracing;
        struct LVCBPT { ... } lvcbpt;
        struct DebugRendering { ... } debugRendering;
        
        // 新增: Wavefront Pipeline
        struct WavefrontPathTracing {
            // OptiX 资源
            optixu::Pipeline pipeline;
            std::vector<optixu::Module> modules;
            
            // Programs
            optixu::Program generateRays;
            optixu::Program traceRaysRayGen;
            optixu::Program traceRaysMiss;
            optixu::HitProgramGroup traceRaysHitGroupDefault;
            optixu::HitProgramGroup traceRaysHitGroupWithAlpha;
            optixu::HitProgramGroup shadowHitGroupDefault;
            optixu::HitProgramGroup shadowHitGroupWithAlpha;
            optixu::HitProgramGroup emptyHitGroup;
            std::vector<optixu::CallableProgramGroup> callablePrograms;
            
            cudau::Buffer shaderBindingTable;
            cudau::Buffer hitGroupShaderBindingTable;
            
            // Wavefront 缓冲区
            cudau::TypedBuffer<WavefrontPathState> pathStateBuffer;
            cudau::TypedBuffer<WavefrontHitInfo> hitInfoBuffer;
            cudau::TypedBuffer<SurfacePoint> surfacePointBuffer;
            
            // 工作队列
            cudau::TypedBuffer<uint32_t> activePathIndices;
            cudau::TypedBuffer<uint32_t> nextActivePathIndices;
            cudau::TypedBuffer<uint32_t> queueCounters;
            
            // CUDA Kernels
            cudau::Kernel processHitsKernel;
            cudau::Kernel sampleLightsKernel;
            cudau::Kernel sampleBSDFKernel;
            cudau::Kernel compactPathsKernel;
            cudau::Kernel accumulateResultsKernel;
        } wavefrontPathTracing;
        
        // Launch Parameters
        WavefrontLaunchParameters wavefrontLaunchParams;
        CUdeviceptr wavefrontLaunchParamsOnDevice;
    } m_optix;
    
    // 渲染方法
    void render(CUstream stream, const Camera* camera, ...);
    void renderPathTracing(...);      // 现有
    void renderLightTracing(...);     // 现有
    void renderLVCBPT(...);           // 现有
    void renderWavefront(...);        // 新增
};
```

---

## 12. Payload 设计

### 最小化 Payload（推荐）
```cpp
// 只传递路径索引和波长
struct WFTracePayload {
    uint32_t pathIndex;      // 4 bytes
    WavelengthSamples wls;   // 24 bytes
    // 总计: 28 bytes (7 dwords)
};

// 在 Closest Hit 中:
CUDA_DEVICE_KERNEL void RT_CH_NAME(wavefrontClosestHit)() {
    WFTracePayload payload;
    WFTracePayloadSignature::get(&payload);
    
    // 直接访问全局缓冲区
    WavefrontPathState& pathState = wlp.pathStateBuffer[payload.pathIndex];
    WavefrontHitInfo& hitInfo = wlp.hitInfoBuffer[payload.pathIndex];
    
    // 填充 HitInfo
    hitInfo.instIndex = optixGetInstanceId();
    hitInfo.geomInstIndex = hp.sbtr->geomInst.geomInstIndex;
    hitInfo.primIndex = hp.primIndex;
    hitInfo.u = hp.b1;
    hitInfo.v = hp.b2;
    hitInfo.setHasHit(true);
}
```

---

## 13. 性能分析图

### GPU 占用率对比
```
递归式 Path Tracing:
GPU Utilization:
100% ┤
 90% ┤
 80% ┤
 70% ┤
 60% ┤     ████
 50% ┤   ██    ██
 40% ┤ ██        ██
 30% ┤█            ██
 20% ┤              ██
 10% ┤                ██
  0% ┴──────────────────────→ Time
     开始  中期  后期  结束
     
平均占用率: ~50%

Wavefront Path Tracing:
GPU Utilization:
100% ┤
 90% ┤ ████████████████
 80% ┤█                ██
 70% ┤                  ██
 60% ┤                    ██
 50% ┤                      ██
 40% ┤                        ██
 30% ┤                          ██
 20% ┤                            ██
 10% ┤                              ██
  0% ┴────────────────────────────────→ Time
     开始  Depth 0-5  Depth 6-10  结束
     
平均占用率: ~85%
```

### 活跃路径数量变化
```
Number of Active Paths:

2.0M ┤█
     ┤█
1.5M ┤█
     ┤█
1.0M ┤█████
     ┤     ████
0.5M ┤         ████
     ┤             ████
   0 ┴─────────────────────→ Depth
     0   5   10   15   20   25

说明:
- Depth 0: 所有路径活跃 (2073600)
- Depth 5: ~50% 路径活跃
- Depth 10: ~25% 路径活跃
- Depth 20: ~5% 路径活跃
```

---

## 14. 调试可视化

### PathLength 可视化
```
颜色映射: 路径长度 → 颜色
  0-5 bounces:   蓝色 (短路径)
  6-10 bounces:  绿色
  11-15 bounces: 黄色
  16-20 bounces: 橙色
  21-25 bounces: 红色 (长路径)

用途: 识别复杂光照区域
```

### MaterialCategory 可视化
```
颜色映射: 材质类别 → 颜色
  Diffuse:      红色
  Glossy:       绿色
  Specular:     蓝色
  Transmissive: 青色
  Mixed:        黄色

用途: 验证材质分类正确
```

### Throughput 可视化
```
颜色映射: 吞吐量亮度 → 灰度
  High throughput:  白色 (重要路径)
  Low throughput:   黑色 (不重要路径)

用途: 识别重要路径分布
```

---

## 15. 错误处理

### 常见错误和解决方案

**错误 1: 队列溢出**
```cpp
// 检测
if (queueCounter >= capacity) {
    printf("ERROR: Queue overflow!\n");
}

// 解决
- 增加队列容量
- 实现动态队列扩展
- 分块渲染
```

**错误 2: 无效贡献值**
```cpp
// 检测
if (!contribution.allFinite()) {
    printf("ERROR: Invalid contribution at pixel (%u, %u)\n", 
           pixelX, pixelY);
}

// 解决
- 检查 BSDF 采样
- 检查 PDF 计算
- 检查除零错误
```

**错误 3: 内存不足**
```cpp
// 检测
cudaError_t err = cudaMalloc(...);
if (err == cudaErrorMemoryAllocation) {
    printf("ERROR: Out of memory!\n");
}

// 解决
- 降低分辨率
- 分块渲染
- 减少缓冲区大小
```

---

## 16. 性能调优清单

### Kernel 级优化
- [ ] 使用 `__launch_bounds__` 限制寄存器
- [ ] 使用 `__restrict__` 指针
- [ ] 使用 `__forceinline__` 关键函数
- [ ] 避免动态分支（`if` 改为 `select`）
- [ ] 使用内建函数（`__sinf`, `__cosf`, ...）
- [ ] 合并内存访问（连续线程访问连续内存）
- [ ] 使用共享内存缓存常用数据

### 算法级优化
- [ ] 路径排序（按材质类型）
- [ ] Stream Compaction（移除终止路径）
- [ ] 材质特化 Kernel
- [ ] 早期退出（零吞吐量、零 PDF）
- [ ] 自适应采样
- [ ] 重要性采样改进

### 系统级优化
- [ ] 多流并行
- [ ] 异步内存传输
- [ ] 分块渲染（降低内存峰值）
- [ ] 动态负载均衡

---

## 17. 测试场景推荐

### 基础测试
1. **Cornell Box** - 基准场景
   - 纯漫反射
   - 简单几何
   - 1 个区域光

2. **Single Sphere** - 材质测试
   - 测试单一材质类型
   - 简单几何
   - 环境光

### 功能测试
3. **Glass Spheres** - 透射测试
   - 玻璃材质
   - 焦散效果
   - 色散

4. **Mixed Materials** - 复杂材质
   - UE4 BRDF
   - 法线贴图
   - Alpha 纹理

### 性能测试
5. **Rungholt** - 大规模场景
   - 600 万三角形
   - 环境光
   - 几何实例化

6. **Stress Test** - 压力测试
   - 高分辨率（4K）
   - 长路径（MaxPathLength=50）
   - 复杂材质组合

---

## 18. 常用命令速查

### Git 命令
```bash
# 创建分支
git checkout -b feature/wavefront-path-tracing

# 提交更改
git add libVLR/shared/wavefront_types.h
git commit -m "Add Wavefront data structures"

# 查看状态
git status
git diff
```

### 编译命令
```bash
# 完整编译
cmake --build . --config Release

# 只编译 libVLR
cmake --build . --target libVLR --config Release

# 只编译 PTX
cmake --build . --target vlr_ptx --config Release
```

### 运行测试
```bash
# 运行 HostProgram
./HostProgram

# 运行特定场景
./HostProgram --scene cornell_box.txt

# 运行性能测试
./HostProgram --benchmark --renderer wavefront
```

---

## 19. 关键文件快速定位

| 需要查找 | 文件路径 |
|---------|---------|
| 当前 Path Tracing 实现 | `libVLR/GPU_kernels/path_tracing.cu` |
| Payload 定义 | `libVLR/shared/light_transport_common.h` |
| BSDF 类定义 | `libVLR/shared/renderer_common.h` |
| 材质描述符 | `libVLR/materials.h` |
| 场景数据结构 | `libVLR/shared/kernel_common.h` |
| Context 类 | `libVLR/context.h` |
| 渲染主循环 | `libVLR/context.cpp` (render 方法) |
| UI 代码 | `HostProgram/main.cpp` |

---

## 20. 快速问题解决

### Q: 如何查看活跃路径数？
```cpp
uint32_t numActive;
cuMemcpyDtoH(&numActive, queueCounters.getCUdeviceptr(), sizeof(uint32_t));
printf("Active paths: %u\n", numActive);
```

### Q: 如何调试单个像素？
```cpp
// 在 Launch Parameters 中设置
wlp.probePixX = 960;  // 中心像素
wlp.probePixY = 540;

// 在 Kernel 中检查
if (pathState.pixelX == wlp.probePixX && 
    pathState.pixelY == wlp.probePixY) {
    printf("Probe pixel: depth=%u, throughput=(%g,%g,%g)\n",
           pathState.pathLength,
           pathState.throughput[0],
           pathState.throughput[1],
           pathState.throughput[2]);
}
```

### Q: 如何测量 Kernel 时间？
```cpp
cudaEvent_t start, stop;
cudaEventCreate(&start);
cudaEventCreate(&stop);

cudaEventRecord(start, stream);
// Launch kernel
cudaEventRecord(stop, stream);
cudaEventSynchronize(stop);

float milliseconds = 0;
cudaEventElapsedTime(&milliseconds, start, stop);
printf("Kernel time: %.2f ms\n", milliseconds);
```

### Q: 如何对比两个渲染结果？
```cpp
float computeL2Error(
    const SpectrumStorage* buffer1,
    const SpectrumStorage* buffer2,
    uint32_t width, uint32_t height) {
    
    float sumSquaredError = 0.0f;
    for (uint32_t i = 0; i < width * height; ++i) {
        RGB rgb1 = buffer1[i].toRGB();
        RGB rgb2 = buffer2[i].toRGB();
        
        float dr = rgb1.r - rgb2.r;
        float dg = rgb1.g - rgb2.g;
        float db = rgb1.b - rgb2.b;
        
        sumSquaredError += dr*dr + dg*dg + db*db;
    }
    
    return sqrt(sumSquaredError / (width * height * 3));
}
```

---

## 21. 优先级指南

### 必须实现（P0）
- ✅ 7 个核心 Kernel
- ✅ Context 集成
- ✅ 基础材质支持（Diffuse, Glossy, Specular）
- ✅ 基础光源支持（Area, IBL）
- ✅ 正确性验证

### 应该实现（P1）
- ✅ 所有材质类型
- ✅ 所有光源类型
- ✅ 路径排序优化
- ✅ Stream Compaction
- ✅ 性能测试

### 可以实现（P2）
- ⭕ 材质特化 Kernel
- ⭕ 多流并行
- ⭕ SoA 内存布局
- ⭕ 调试渲染模式
- ⭕ 性能统计工具

### 未来实现（P3）
- ⭕ 自适应采样
- ⭕ 路径再生
- ⭕ ReSTIR 集成
- ⭕ 多 GPU 支持

---

**最后更新**: 2026-03-06  
**用途**: 开发时快速查阅关键信息  
**维护**: 随项目进展更新
