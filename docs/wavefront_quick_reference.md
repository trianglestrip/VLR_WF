# Wavefront 快速参考指南

> 快速查阅 Wavefront 实现的关键信息

---

## 📋 7 个核心 Kernel

| # | Kernel 名称 | 文件 | 功能 | 输入 | 输出 |
|---|-----------|------|------|------|------|
| 1 | **GenerateRays** | `wavefront_generate_rays.cu` | 生成初始相机光线 | 像素坐标 | PathState, activeQueue |
| 2 | **TraceRays** | `wavefront_trace_rays.cu` | 光线求交 | PathState, activeQueue | HitInfo |
| 3 | **ProcessHits** | `wavefront_process_hits.cu` | 处理命中点 | PathState, HitInfo | SurfacePoint, contribution |
| 4 | **SampleLights** | `wavefront_sample_lights.cu` | 显式光源采样 (NEE) | PathState, SurfacePoint | contribution |
| 5 | **SampleBSDF** | `wavefront_sample_bsdf.cu` | BSDF 采样 | PathState, SurfacePoint | 新光线, nextQueue |
| 6 | **CompactPaths** | `wavefront_compact.cu` | 路径压缩 | activeQueue, nextQueue | 压缩后的 activeQueue |
| 7 | **AccumulateResults** | `wavefront_accumulate.cu` | 累积结果 | PathState | accumBuffer |

---

## 🗂️ 核心数据结构

### WavefrontPathState (144 bytes)
```cpp
struct WavefrontPathState {
    // 光线
    Point3D origin;              // 12B
    Vector3D direction;          // 12B
    
    // 光谱
    SampledSpectrum throughput;  // 16B
    SampledSpectrum contribution;// 16B
    WavelengthSamples wls;       // 24B
    float initImportance;        // 4B
    
    // RNG
    KernelRNG rng;               // 16B
    
    // 历史
    float prevDirPDF;            // 4B
    DirectionType prevSampledType; // 4B
    uint32_t pathLength;         // 4B
    
    // 像素
    uint32_t pixelX, pixelY;     // 8B
    
    // 标志
    uint32_t flags;              // 4B
    uint32_t materialCategory;   // 4B
};
```

**关键方法**:
- `isActive()` - 路径是否活跃
- `setTerminated()` - 终止路径
- `getMaterialCategory()` - 获取材质类别

### WavefrontHitInfo (32 bytes)
```cpp
struct WavefrontHitInfo {
    uint32_t instIndex;          // 实例索引
    uint32_t geomInstIndex;      // 几何实例索引
    uint32_t primIndex;          // 图元索引
    uint32_t hitFlags;           // 命中标志
    float u, v;                  // 重心坐标
    float t;                     // 光线参数
};
```

**关键方法**:
- `hasHit()` - 是否命中
- `hitInfinity()` - 是否命中环境光
- `reset()` - 重置

### WavefrontWorkQueue
```cpp
struct WavefrontWorkQueue {
    uint32_t* pathIndices;       // 路径索引数组
    uint32_t* counter;           // 原子计数器
    uint32_t capacity;           // 容量
    
    uint32_t enqueue(uint32_t pathIndex);
    uint32_t size() const;
    void reset();
};
```

---

## 🔄 渲染流程

### 主循环伪代码
```cpp
void Context::renderWavefront(CUstream stream, const Camera* camera) {
    // 1. 生成初始光线
    launchGenerateRays(imageSize);
    
    // 2. 主循环
    for (depth = 0; depth < maxDepth; depth++) {
        numActive = getActivePathCount();
        if (numActive == 0) break;
        
        // 2.1 光线追踪
        launchTraceRays(numActive);
        
        // 2.2 处理命中点
        launchProcessHits(numActive);
        
        // 2.3 显式光源采样
        launchSampleLights(numActive);
        
        // 2.4 BSDF 采样
        launchSampleBSDF(numActive);
        
        // 2.5 路径压缩
        launchCompactPaths();
        
        // 交换队列
        swap(activeQueue, nextQueue);
    }
    
    // 3. 累积结果
    launchAccumulateResults(totalPixels);
}
```

---

## 🎨 材质分类

```cpp
enum MaterialCategory {
    Diffuse = 0,      // Lambert, Matte
    Glossy,           // GGX, UE4 BRDF
    Specular,         // 理想镜面
    Transmissive,     // 玻璃、透射
    Emissive,         // 发光材质
    Mixed,            // 混合材质
};

MaterialCategory classifyMaterial(const BSDF& bsdf) {
    if (bsdf.matches(Delta0D | Reflection))
        return Specular;
    if (bsdf.matches(Transmission))
        return Transmissive;
    if (bsdf.matches(HighFreq | Reflection))
        return Glossy;
    return Diffuse;
}
```

---

## 📊 内存布局

### 1920x1080 分辨率内存估算

| 缓冲区 | 大小/路径 | 总大小 |
|-------|----------|--------|
| PathState | 144 B | 299 MB |
| HitInfo | 32 B | 65 MB |
| SurfacePoint | 128 B | 265 MB |
| 工作队列 | 8 B | 16 MB |
| **总计** | | **645 MB** |

### 优化策略
1. **按需分配**: 只为活跃路径分配 SurfacePoint
2. **SoA 布局**: 提高内存合并访问
3. **双缓冲**: 减少内存拷贝
4. **分块渲染**: 降低内存峰值

---

## ⚡ 性能优化技巧

### 1. 路径排序
```cpp
// 按材质类别排序，减少分支发散
cub::DeviceRadixSort::SortPairs(
    materialCategories, pathIndices, numPaths);
```
**收益**: 10-20% 性能提升

### 2. Stream Compaction
```cpp
// 移除已终止路径
cub::DeviceSelect::Flagged(
    pathIndices, activeFlags, outputIndices, numActive);
```
**收益**: 5-10% 性能提升

### 3. 材质特化 Kernel
```cpp
// 为漫反射材质优化
__global__ void sampleBSDF_Diffuse() {
    // 无分支，直接 Lambert 采样
}
```
**收益**: 15-25% 性能提升

### 4. 多流并行
```cpp
// 流水线并行
traceRays(stream0, depth);
processHits(stream1, depth-1);
sampleBSDF(stream2, depth-2);
```
**收益**: 10-15% 性能提升

---

## 🐛 调试技巧

### 常见问题诊断

**问题**: 渲染结果全黑
```cpp
// 检查清单:
1. PathState 初始化是否正确？
2. 活跃队列是否为空？
3. 光线方向是否正确？
4. 使用 WFDebug_PathLength 模式检查
```

**问题**: 结果与递归式不一致
```cpp
// 调试步骤:
1. 逐个 Kernel 验证
2. 对比中间结果（contribution, throughput）
3. 检查 MIS 权重计算
4. 检查吞吐量更新公式
```

**问题**: 性能未达预期
```cpp
// 分析工具:
1. Nsight Systems: 整体性能分析
2. Nsight Compute: Kernel 级分析
3. 检查 GPU 占用率
4. 检查内存带宽利用率
```

### 调试渲染模式
```cpp
enum WavefrontDebugMode {
    WFDebug_PathLength,        // 路径长度（颜色深浅）
    WFDebug_MaterialCategory,  // 材质类别（不同颜色）
    WFDebug_Throughput,        // 吞吐量（亮度）
    WFDebug_ActivePaths,       // 活跃路径（黑白）
};

// 使用:
context->setWavefrontDebugMode(WFDebug_PathLength);
```

---

## 📐 关键公式

### MIS 权重（Power Heuristic, β=2）
```cpp
float MISWeight(float pdf1, float pdf2) {
    return (pdf1 * pdf1) / (pdf1 * pdf1 + pdf2 * pdf2);
}
```

### 俄罗斯轮盘赌
```cpp
float continueProb = min(
    throughput.importance() / initImportance, 
    1.0f);

if (rng.getFloat0cTo1o() >= continueProb)
    terminate();
else
    throughput /= continueProb;
```

### 几何项
```cpp
float G = |cos(θ_x)| * |cos(θ_y)| / distance²
```

### BSDF 吞吐量更新
```cpp
throughput *= fs * |cos(θ)| / pdf
```

---

## 🔧 开发工具

### 编译命令
```bash
# 编译 PTX
nvcc -ptx -arch=sm_86 \
  -I../shared -I../../include \
  -DVLR_Device \
  wavefront_generate_rays.cu \
  -o wavefront_generate_rays.ptx

# 编译整个项目
cmake --build . --config Release
```

### 调试命令
```bash
# Nsight Systems
nsys profile --trace=cuda,nvtx ./HostProgram

# Nsight Compute
ncu --set full --target-processes all ./HostProgram

# CUDA-MEMCHECK
cuda-memcheck --tool memcheck ./HostProgram
```

### 性能分析
```bash
# 查看 GPU 占用率
nvidia-smi dmon -s u

# 查看内存使用
nvidia-smi dmon -s m
```

---

## 📊 性能基准

### 预期性能（RTX 3080, 1920x1080）

| 场景 | 递归式 (ms) | Wavefront (ms) | 加速比 |
|-----|-----------|---------------|--------|
| Cornell Box | 50 | 25 | 2.0x |
| Glass Spheres | 120 | 50 | 2.4x |
| Complex Scene | 200 | 70 | 2.9x |
| Rungholt | 500 | 180 | 2.8x |

### GPU 占用率

| 场景 | 递归式 | Wavefront |
|-----|--------|-----------|
| 简单场景 | 45% | 80% |
| 复杂场景 | 55% | 90% |

---

## 🎯 快速检查清单

### 开始新 Kernel 前
- [ ] 阅读对应的设计文档章节
- [ ] 理解输入输出数据结构
- [ ] 准备测试场景
- [ ] 设置调试输出

### 完成 Kernel 后
- [ ] 编译无错误和警告
- [ ] 单独测试 Kernel
- [ ] 集成测试
- [ ] 性能初步测量
- [ ] 代码审查

### 集成到 Context 前
- [ ] 所有 Kernel 独立测试通过
- [ ] 缓冲区正确分配
- [ ] Launch Parameters 正确设置
- [ ] SBT 正确创建

### 发布前
- [ ] 所有测试通过
- [ ] 性能达标
- [ ] 文档完整
- [ ] 无已知严重 Bug
- [ ] 代码审查通过

---

## 💡 代码片段

### 从队列读取路径
```cpp
uint32_t workIndex = blockIdx.x * blockDim.x + threadIdx.x;
if (workIndex >= wlp.activePathQueue.size())
    return;

uint32_t pathIndex = wlp.activePathQueue.pathIndices[workIndex];
WavefrontPathState& pathState = wlp.pathStateBuffer[pathIndex];

if (!pathState.isActive())
    return;
```

### 发射光线（OptiX）
```cpp
WFTracePayload payload;
payload.pathIndex = pathIndex;
payload.wls = pathState.wls;

WFTracePayloadSignature::trace(
    wlp.topGroup,
    asOptiXType(pathState.origin),
    asOptiXType(pathState.direction),
    0.0f, FLT_MAX, 0.0f,
    VisibilityGroup_Everything,
    OPTIX_RAY_FLAG_NONE,
    WFRayType::Closest, NumWFRayTypes, WFRayType::Closest,
    payload);
```

### 计算表面点
```cpp
SurfacePoint surfPt;
float hypAreaPDF;
computeSurfacePoint(hitInfo, pathState.wls, &surfPt, &hypAreaPDF);
wlp.surfacePointBuffer[pathIndex] = surfPt;
```

### BSDF 采样
```cpp
BSDF<TransportMode::Radiance> bsdf(matDesc, surfPt, wls);

Vector3D dirOutLocal = surfPt.toLocal(-pathState.direction);
Normal3D geomNormalLocal = surfPt.toLocal(surfPt.geometricNormal);
BSDFQuery query(dirOutLocal, geomNormalLocal, TransportMode::Radiance,
                DirectionType::All(), wls);

BSDFSample sample(rng.getFloat0cTo1o(), rng.getFloat0cTo1o(), rng.getFloat0cTo1o());
BSDFQueryResult result;
SampledSpectrum fs = bsdf.sample(query, sample, &result);

if (fs != SampledSpectrum::Zero() && result.dirPDF > 0.0f) {
    float cosFactor = dot(result.dirLocal, geomNormalLocal);
    pathState.throughput *= fs * abs(cosFactor) / result.dirPDF;
    
    Vector3D dirIn = surfPt.fromLocal(result.dirLocal);
    pathState.origin = offsetRayOrigin(surfPt.position,
        cosFactor > 0 ? surfPt.geometricNormal : -surfPt.geometricNormal);
    pathState.direction = dirIn;
}
```

### 光源采样（NEE）
```cpp
// 选择光源
float uLight = rng.getFloat0cTo1o();
SurfaceLight light;
float lightProb;
float uPrim;
selectSurfaceLight(uLight, &light, &lightProb, &uPrim);

// 采样光源位置
SurfaceLightPosSample lpSample(uPrim, rng.getFloat0cTo1o(), rng.getFloat0cTo1o());
SurfaceLightPosQueryResult lpResult;
light.sample(lpSample, surfPt.position, &lpResult);

// 评估光源辐射
const SurfaceMaterialDescriptor& lightMatDesc = wlp.materialDescriptorBuffer[lpResult.materialIndex];
EDF ledf(lightMatDesc, lpResult.surfPt, wls);
SampledSpectrum M = ledf.evaluateEmittance();

// 可见性测试
Vector3D shadowRayDir;
float squaredDistance;
float fractionalVisibility;
if (M.hasNonZero() && testVisibility<WFRayType::Shadow>(
        surfPt, lpResult.surfPt, wls,
        &shadowRayDir, &squaredDistance, &fractionalVisibility)) {
    
    // BSDF 评估
    Vector3D shadowRayDir_sn = surfPt.toLocal(shadowRayDir);
    SampledSpectrum fs = bsdf.evaluate(query, shadowRayDir_sn);
    
    // MIS 权重
    float lightPDF = lightProb * lpResult.areaPDF;
    float bsdfPDF = bsdf.evaluatePDF(query, shadowRayDir_sn) * /* ... */;
    float MISWeight = computeMISWeight(lightPDF, bsdfPDF);
    
    // 累积贡献
    EDFQuery feQuery(DirectionType::All(), wls);
    SampledSpectrum Le = M * ledf.evaluate(feQuery, /* ... */);
    float G = fractionalVisibility * /* 几何项 */;
    
    pathState.contribution += pathState.throughput * Le * fs * G * MISWeight / lightPDF;
}
```

### 累积结果
```cpp
uint32_t pathIndex = blockIdx.x * blockDim.x + threadIdx.x;
if (pathIndex >= totalPaths)
    return;

WavefrontPathState& pathState = wlp.pathStateBuffer[pathIndex];

if (!pathState.contribution.allFinite())
    return;

uint2 pixelCoord = make_uint2(pathState.pixelX, pathState.pixelY);

if (wlp.numAccumFrames == 1)
    wlp.accumBuffer[pixelCoord].reset();

wlp.accumBuffer[pixelCoord].add(pathState.wls, pathState.contribution);
wlp.rngBuffer.write(pixelCoord, pathState.rng);
```

---

## 🔍 常用查询

### 查找 BSDF 相关代码
```bash
rg "BSDF" libVLR/shared/renderer_common.h
rg "BSDFQuery" libVLR/shared/kernel_common.h
```

### 查找光源采样代码
```bash
rg "selectSurfaceLight" libVLR/shared/light_transport_common.h
rg "SurfaceLight" libVLR/shared/kernel_common.h
```

### 查找 Payload 定义
```bash
rg "Payload" libVLR/shared/light_transport_common.h
```

### 查找现有 Path Tracing 实现
```bash
rg "RT_RG_NAME\(pathTracing\)" libVLR/GPU_kernels/path_tracing.cu
```

---

## 📝 命名规范

### Kernel 命名
```cpp
// OptiX Programs
RT_RG_NAME(wavefrontGenerateRays)
RT_RG_NAME(wavefrontTraceRays)
RT_CH_NAME(wavefrontClosestHit)
RT_MS_NAME(wavefrontMiss)
RT_AH_NAME(wavefrontAnyHitWithAlpha)

// CUDA Kernels
__global__ void wavefrontProcessHits()
__global__ void wavefrontSampleLights()
__global__ void wavefrontSampleBSDF()
__global__ void wavefrontCompactPaths()
__global__ void wavefrontAccumulateResults()
```

### 变量命名
```cpp
// 缩写
wlp - WavefrontLaunchParameters
wf  - WavefrontPathTracing (Context 成员)
ps  - PathState
hi  - HitInfo
sp  - SurfacePoint

// 队列
activePathQueue
nextActivePathQueue
materialQueues[category]

// 缓冲区
pathStateBuffer
hitInfoBuffer
surfacePointBuffer
```

---

## 🎓 学习资源

### 必读论文
1. **[Laine2013]** - Wavefront 架构基础
2. **[Pharr2023]** - PBRT-v4 实现细节
3. **[Novák2010]** - GPU 光线追踪效率

### 推荐阅读
- OptiX 7 Best Practices Guide
- CUDA C++ Best Practices Guide
- GPU Gems 3: Chapter 39 (Parallel Prefix Sum)

### 参考实现
- **PBRT-v4**: `src/pbrt/gpu/pathintegrator.cpp`
- **OptiX Samples**: `SDK/optixPathTracer`

---

## 🚀 快速启动命令

### 创建开发分支
```bash
git checkout -b feature/wavefront-path-tracing
```

### 创建必要目录
```bash
mkdir -p libVLR/shared
mkdir -p libVLR/GPU_kernels
mkdir -p docs
```

### 创建第一个文件
```bash
# 从模板复制
cp docs/wavefront_data_structures.h libVLR/shared/wavefront_types.h

# 或从头创建
touch libVLR/shared/wavefront_types.h
```

### 编译测试
```bash
cmake --build . --config Debug
./HostProgram
```

---

## 📞 获取帮助

### 遇到问题时
1. 查阅 `docs/wavefront_design.md` - 架构设计
2. 查阅 `docs/wavefront_implementation_plan.md` - 实现细节
3. 查阅本文档 - 快速参考
4. 参考 PBRT-v4 源码
5. 查阅 OptiX 文档

### 性能问题
1. 使用 Nsight Systems 分析
2. 使用 Nsight Compute 分析 Kernel
3. 检查内存访问模式
4. 检查分支发散

### Bug 调试
1. 使用调试渲染模式
2. 使用 cuda-memcheck
3. 添加 printf 调试（小心性能影响）
4. 对比递归式中间结果

---

## 📌 重要提醒

### ⚠️ 注意事项
- Payload 大小限制：<= 32 dwords (128 bytes)
- 内存对齐：使用 `alignas(16)`
- 原子操作：使用 `atomicAdd` 等
- 队列容量：检查是否溢出
- 有效性检查：`allFinite()`, `hasNonZero()`

### ✅ 最佳实践
- 最小化 Payload，使用全局内存
- 早期退出优化（零吞吐量、零 PDF）
- 合并内存访问（连续线程访问连续内存）
- 减少分支发散（路径排序）
- 使用 `__restrict__` 指针
- 使用 `__launch_bounds__` 优化寄存器

---

**版本**: 1.0  
**日期**: 2026-03-06  
**维护**: VLR 开发团队
