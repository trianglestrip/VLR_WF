# Wavefront 路径追踪架构设计文档

## 1. 概述

### 1.1 目标
将 VLR 从递归式路径追踪（Recursive Path Tracing）改造为 Wavefront 路径追踪（Wavefront Path Tracing），以提高 GPU 利用率和渲染性能。

### 1.2 核心理念
**递归式路径追踪**：每个线程独立处理一条完整的光线路径，从相机出发直到路径终止。
- 优点：代码简洁，易于理解
- 缺点：线程分支发散严重，GPU 利用率低

**Wavefront 路径追踪**：将路径追踪分解为多个阶段（Stage），每个阶段处理所有活跃路径的相同操作。
- 优点：减少分支发散，提高 GPU 占用率，性能提升 1.5-3x
- 缺点：代码复杂度增加，需要更多显存

### 1.3 预期性能提升
- **简单场景**（少量材质类型）：1.5-2x
- **复杂场景**（多种材质类型）：2-3x
- **GPU 占用率**：从 40-60% 提升到 80-95%

---

## 2. 当前架构分析

### 2.1 现有渲染器
```cpp
enum VLRRenderer {
    VLRRenderer_PathTracing = 0,      // 单向路径追踪
    VLRRenderer_LightTracing,         // 光源追踪
    VLRRenderer_BPT,                  // 双向路径追踪 (LVC-BPT)
    VLRRenderer_DebugRendering,       // 调试渲染
};
```

### 2.2 当前 Path Tracing 流程
**文件**: `libVLR/GPU_kernels/path_tracing.cu`

```
RT_RG_NAME(pathTracing)  // Ray Generation
  ├─ 初始化 RNG、波长采样
  ├─ 相机采样生成初始光线
  └─ while (pathLength < MaxPathLength)
      ├─ PTPayloadSignature::trace()  // OptiX 光线追踪
      ├─ RT_CH_NAME(pathTracingIteration)  // Closest Hit
      │   ├─ 隐式光源采样 (Implicit Light Sampling)
      │   ├─ 显式光源采样 (Next Event Estimation)
      │   ├─ BSDF 采样生成下一跳方向
      │   └─ 俄罗斯轮盘赌终止
      └─ 累积贡献值
```

### 2.3 关键数据结构
```cpp
// 当前 Payload 结构
struct PTReadOnlyPayload {
    float initImportance;
    WavelengthSamples wls;
    float prevDirPDF;
    DirectionType prevSampledType;
    unsigned int pathLength : 16;
    unsigned int maxLengthTerminate : 1;
};

struct PTWriteOnlyPayload {
    Point3D nextOrigin;
    Vector3D nextDirection;
    float dirPDF;
    DirectionType sampledType;
    unsigned int terminate : 1;
};

struct PTReadWritePayload {
    KernelRNG rng;
    SampledSpectrum alpha;
    SampledSpectrum contribution;
};
```

---

## 3. Wavefront 架构设计

### 3.1 总体架构

```
┌─────────────────────────────────────────────────────────────┐
│                    Wavefront Renderer                        │
├─────────────────────────────────────────────────────────────┤
│                                                              │
│  [1] GenerateRays Kernel                                    │
│      ├─ 相机采样                                             │
│      ├─ 初始化 PathState                                     │
│      └─ 输出: 活跃路径队列                                   │
│                                                              │
│  Loop: while (有活跃路径)                                    │
│  ├─ [2] TraceRays (OptiX)                                   │
│  │      └─ 光线求交，填充命中信息                           │
│  │                                                           │
│  ├─ [3] ProcessHits Kernel                                  │
│  │      ├─ 计算表面点信息                                   │
│  │      ├─ 评估 BSDF/EDF                                    │
│  │      ├─ 隐式光源采样                                     │
│  │      └─ 分类路径（按材质/BSDF 类型）                     │
│  │                                                           │
│  ├─ [4] SampleLights Kernel                                 │
│  │      ├─ 显式光源采样（NEE）                              │
│  │      ├─ 可见性测试                                       │
│  │      └─ 累积直接光照贡献                                 │
│  │                                                           │
│  ├─ [5] SampleBSDF Kernel                                   │
│  │      ├─ BSDF 采样生成下一跳方向                          │
│  │      ├─ 俄罗斯轮盘赌终止                                 │
│  │      └─ 更新路径状态                                     │
│  │                                                           │
│  └─ [6] CompactPaths Kernel                                 │
│         ├─ Stream Compaction                                │
│         └─ 移除已终止路径                                   │
│                                                              │
│  [7] AccumulateResults Kernel                               │
│      └─ 将贡献值累加到输出缓冲区                            │
└─────────────────────────────────────────────────────────────┘
```

### 3.2 核心数据结构

#### 3.2.1 PathState - 路径状态
```cpp
// 文件: libVLR/shared/wavefront_types.h

namespace vlr::shared {

// 路径状态：存储每条路径的完整信息
struct WavefrontPathState {
    // === 光线信息 ===
    Point3D origin;                    // 光线起点
    Vector3D direction;                // 光线方向
    
    // === 路径吞吐量和贡献 ===
    SampledSpectrum throughput;        // 路径吞吐量 (alpha)
    SampledSpectrum contribution;      // 累积的辐射贡献
    
    // === 随机数和波长 ===
    KernelRNG rng;                     // 随机数生成器状态
    WavelengthSamples wls;             // 波长采样
    float initImportance;              // 初始重要性（用于俄罗斯轮盘赌）
    
    // === 路径历史信息 ===
    float prevDirPDF;                  // 前一跳的方向 PDF
    DirectionType prevSampledType;     // 前一跳的采样类型
    uint32_t pathLength;               // 路径长度
    
    // === 像素坐标 ===
    uint32_t pixelX;                   // 像素 X 坐标
    uint32_t pixelY;                   // 像素 Y 坐标
    
    // === 状态标志 ===
    uint32_t flags;                    // 状态标志位
    // flags 位定义：
    // bit 0: isActive - 路径是否活跃
    // bit 1: isTerminated - 路径是否终止
    // bit 2: maxLengthReached - 是否达到最大长度
    // bit 3: singleWlSelected - 是否选择了单一波长（色散材质）
    // bit 4-7: materialCategory - 材质类别（用于排序）
    
    CUDA_DEVICE_FUNCTION CUDA_INLINE bool isActive() const {
        return flags & 0x1;
    }
    
    CUDA_DEVICE_FUNCTION CUDA_INLINE void setTerminated() {
        flags |= 0x2;
        flags &= ~0x1;
    }
    
    CUDA_DEVICE_FUNCTION CUDA_INLINE uint32_t getMaterialCategory() const {
        return (flags >> 4) & 0xF;
    }
    
    CUDA_DEVICE_FUNCTION CUDA_INLINE void setMaterialCategory(uint32_t category) {
        flags = (flags & ~0xF0) | ((category & 0xF) << 4);
    }
};

// 命中信息：存储光线求交结果
struct WavefrontHitInfo {
    // === 几何信息 ===
    uint32_t instIndex;                // 实例索引
    uint32_t geomInstIndex;            // 几何实例索引
    uint32_t primIndex;                // 图元索引
    float u, v;                        // 重心坐标或参数化坐标
    
    // === 命中标志 ===
    uint32_t hitFlags;
    // bit 0: hasHit - 是否命中
    // bit 1: hitInfinity - 是否命中无限远（环境光）
    // bit 2: hitEmissive - 是否命中发光表面
    
    CUDA_DEVICE_FUNCTION CUDA_INLINE bool hasHit() const {
        return hitFlags & 0x1;
    }
    
    CUDA_DEVICE_FUNCTION CUDA_INLINE bool hitInfinity() const {
        return hitFlags & 0x2;
    }
    
    CUDA_DEVICE_FUNCTION CUDA_INLINE void setHasHit(bool hit) {
        if (hit) hitFlags |= 0x1;
        else hitFlags &= ~0x1;
    }
};

// 工作队列：管理活跃路径索引
struct WavefrontWorkQueue {
    uint32_t* pathIndices;             // 路径索引数组
    uint32_t* counter;                 // 原子计数器（GPU 上）
    uint32_t capacity;                 // 队列容量
    
    CUDA_DEVICE_FUNCTION CUDA_INLINE uint32_t enqueue(uint32_t pathIndex) {
        uint32_t slot = atomicAdd(counter, 1);
        if (slot < capacity) {
            pathIndices[slot] = pathIndex;
            return slot;
        }
        return 0xFFFFFFFF; // 队列满
    }
};

// 材质分类队列：按材质类型分类路径
enum MaterialCategory : uint32_t {
    MaterialCategory_Diffuse = 0,      // 漫反射材质
    MaterialCategory_Glossy,           // 光滑反射材质
    MaterialCategory_Specular,         // 镜面反射材质
    MaterialCategory_Transmissive,     // 透射材质
    MaterialCategory_Emissive,         // 发光材质
    MaterialCategory_Mixed,            // 混合材质
    NumMaterialCategories
};

} // namespace vlr::shared
```

#### 3.2.2 Wavefront Launch Parameters
```cpp
// 文件: libVLR/shared/wavefront_types.h

namespace vlr::shared {

struct WavefrontLaunchParameters {
    // === 继承自 PipelineLaunchParameters 的公共数据 ===
    // (材质、节点、场景数据等)
    
    // === Wavefront 特定数据 ===
    
    // 路径状态缓冲区
    WavefrontPathState* pathStateBuffer;
    WavefrontHitInfo* hitInfoBuffer;
    
    // 工作队列
    WavefrontWorkQueue activePathQueue;      // 当前活跃路径
    WavefrontWorkQueue nextActivePathQueue;  // 下一轮活跃路径
    
    // 材质分类队列（可选优化）
    WavefrontWorkQueue materialQueues[NumMaterialCategories];
    
    // 辅助缓冲区
    SurfacePoint* surfacePointBuffer;        // 表面点缓冲区
    uint32_t* rayCounters;                   // 光线计数器
    
    // 配置参数
    uint32_t maxPathLength;                  // 最大路径长度
    uint32_t maxNumPaths;                    // 最大路径数量
    uint32_t currentDepth;                   // 当前路径深度
    
    // 性能统计
    uint32_t* numActiveRays;                 // 当前活跃光线数
    uint32_t* numShadowRays;                 // 阴影光线数
    uint32_t* numTerminatedPaths;            // 终止路径数
};

} // namespace vlr::shared
```

---

## 4. 内核设计

### 4.1 Kernel 1: GenerateRays - 生成初始光线

**文件**: `libVLR/GPU_kernels/wavefront_generate_rays.cu`

**功能**：
- 为每个像素生成初始相机光线
- 初始化 PathState
- 将所有路径加入活跃队列

**伪代码**：
```cpp
CUDA_DEVICE_KERNEL void wavefrontGenerateRays() {
    uint2 launchIndex = make_uint2(optixGetLaunchIndex().x, optixGetLaunchIndex().y);
    uint32_t pathIndex = launchIndex.y * wlp.imageSize.x + launchIndex.x;
    
    // 初始化 RNG
    KernelRNG rng = wlp.rngBuffer.read(launchIndex);
    
    // 波长采样
    float selectWLPDF;
    WavelengthSamples wls = WavelengthSamples::createWithEqualOffsets(
        rng.getFloat0cTo1o(), rng.getFloat0cTo1o(), &selectWLPDF);
    
    // 相机采样
    float2 pixelSample = make_float2(
        launchIndex.x + rng.getFloat0cTo1o(),
        launchIndex.y + rng.getFloat0cTo1o());
    
    Camera camera(wlp.progSampleLensPosition);
    LensPosSample We0Sample(rng.getFloat0cTo1o(), rng.getFloat0cTo1o());
    LensPosQueryResult We0Result;
    camera.sample(We0Sample, &We0Result);
    
    IDF idf(wlp.cameraDescriptor, We0Result.surfPt, wls);
    SampledSpectrum We0 = idf.evaluateSpatialImportance();
    
    IDFSample We1Sample(pixelSample.x / wlp.imageSize.x, pixelSample.y / wlp.imageSize.y);
    IDFQueryResult We1Result;
    SampledSpectrum We1 = idf.sample(IDFQuery(), We1Sample, &We1Result);
    
    Point3D rayOrg = We0Result.surfPt.position;
    Vector3D rayDir = We0Result.surfPt.fromLocal(We1Result.dirLocal);
    SampledSpectrum throughput = (We0 * We1) * (/* ... */);
    
    // 初始化 PathState
    WavefrontPathState& pathState = wlp.pathStateBuffer[pathIndex];
    pathState.origin = rayOrg;
    pathState.direction = rayDir;
    pathState.throughput = throughput;
    pathState.contribution = SampledSpectrum::Zero();
    pathState.rng = rng;
    pathState.wls = wls;
    pathState.initImportance = throughput.importance(wls.selectedLambdaIndex());
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

**输出**：
- `pathStateBuffer`: 初始化的路径状态
- `activePathQueue`: 所有路径的索引

---

### 4.2 Kernel 2: TraceRays - 光线追踪

**实现方式**: 使用 OptiX Ray Generation Program

**文件**: `libVLR/GPU_kernels/wavefront_trace_rays.cu`

**功能**：
- 对活跃队列中的所有路径进行光线追踪
- 填充 HitInfo

**伪代码**：
```cpp
CUDA_DEVICE_KERNEL void RT_RG_NAME(wavefrontTraceRays)() {
    uint32_t workIndex = optixGetLaunchIndex().x;
    if (workIndex >= *wlp.activePathQueue.counter)
        return;
    
    uint32_t pathIndex = wlp.activePathQueue.pathIndices[workIndex];
    WavefrontPathState& pathState = wlp.pathStateBuffer[pathIndex];
    
    if (!pathState.isActive())
        return;
    
    // 准备 Payload
    WFTracePayload payload;
    payload.pathIndex = pathIndex;
    payload.wls = pathState.wls;
    
    // 发射光线
    WFTracePayloadSignature::trace(
        wlp.topGroup,
        asOptiXType(pathState.origin),
        asOptiXType(pathState.direction),
        0.0f, FLT_MAX, 0.0f,
        VisibilityGroup_Everything,
        OPTIX_RAY_FLAG_NONE,
        WFRayType::Closest, MaxNumRayTypes, WFRayType::Closest,
        payload);
}

// Closest Hit Program
CUDA_DEVICE_KERNEL void RT_CH_NAME(wavefrontClosestHit)() {
    const auto hp = HitPointParameter::get();
    
    WFTracePayload payload;
    WFTracePayloadSignature::get(&payload);
    
    uint32_t pathIndex = payload.pathIndex;
    WavefrontHitInfo& hitInfo = wlp.hitInfoBuffer[pathIndex];
    
    // 记录命中信息
    hitInfo.instIndex = optixGetInstanceId();
    hitInfo.geomInstIndex = hp.sbtr->geomInst.geomInstIndex;
    hitInfo.primIndex = hp.primIndex;
    hitInfo.u = hp.b1;
    hitInfo.v = hp.b2;
    hitInfo.hitFlags = 0x1; // hasHit = true
}

// Miss Program
CUDA_DEVICE_KERNEL void RT_MS_NAME(wavefrontMiss)() {
    WFTracePayload payload;
    WFTracePayloadSignature::get(&payload);
    
    uint32_t pathIndex = payload.pathIndex;
    WavefrontHitInfo& hitInfo = wlp.hitInfoBuffer[pathIndex];
    
    // 标记命中环境光
    hitInfo.hitFlags = 0x3; // hasHit = true, hitInfinity = true
}
```

**输出**：
- `hitInfoBuffer`: 填充的命中信息

---

### 4.3 Kernel 3: ProcessHits - 处理命中点

**文件**: `libVLR/GPU_kernels/wavefront_process_hits.cu`

**功能**：
- 计算表面点信息（法线、纹理坐标等）
- 评估材质（BSDF/EDF）
- 处理隐式光源采样
- 为下一阶段准备数据

**伪代码**：
```cpp
CUDA_DEVICE_KERNEL void wavefrontProcessHits() {
    uint32_t workIndex = blockIdx.x * blockDim.x + threadIdx.x;
    if (workIndex >= *wlp.activePathQueue.counter)
        return;
    
    uint32_t pathIndex = wlp.activePathQueue.pathIndices[workIndex];
    WavefrontPathState& pathState = wlp.pathStateBuffer[pathIndex];
    WavefrontHitInfo& hitInfo = wlp.hitInfoBuffer[pathIndex];
    
    if (!pathState.isActive() || !hitInfo.hasHit())
        return;
    
    // 处理环境光命中
    if (hitInfo.hitInfinity()) {
        processEnvironmentHit(pathState, hitInfo);
        pathState.setTerminated();
        return;
    }
    
    // 计算表面点
    SurfacePoint surfPt;
    float hypAreaPDF;
    computeSurfacePoint(hitInfo, pathState.wls, &surfPt, &hypAreaPDF);
    wlp.surfacePointBuffer[pathIndex] = surfPt;
    
    // 获取材质
    const GeometryInstance& geomInst = wlp.geomInstBuffer[hitInfo.geomInstIndex];
    const SurfaceMaterialDescriptor& matDesc = wlp.materialDescriptorBuffer[geomInst.materialIndex];
    
    // 评估 BSDF 和 EDF
    BSDF<TransportMode::Radiance> bsdf(matDesc, surfPt, pathState.wls);
    EDF edf(matDesc, surfPt, pathState.wls);
    
    // 隐式光源采样（命中发光表面）
    SampledSpectrum spEmittance = edf.evaluateEmittance();
    if (spEmittance.hasNonZero()) {
        Vector3D dirOutLocal = surfPt.shadingFrame.toLocal(-pathState.direction);
        EDFQuery feQuery(DirectionType::All(), pathState.wls);
        SampledSpectrum Le = spEmittance * edf.evaluate(feQuery, dirOutLocal);
        
        // MIS 权重计算
        float MISWeight = computeMISWeight(pathState, hitInfo, hypAreaPDF, dirOutLocal);
        
        // 累积贡献
        pathState.contribution += pathState.throughput * Le * MISWeight;
    }
    
    // 路径长度检查
    pathState.pathLength++;
    if (pathState.pathLength >= wlp.maxPathLength) {
        pathState.setTerminated();
        return;
    }
    
    // 俄罗斯轮盘赌
    float continueProb = min(
        pathState.throughput.importance(pathState.wls.selectedLambdaIndex()) / pathState.initImportance,
        1.0f);
    if (pathState.rng.getFloat0cTo1o() >= continueProb) {
        pathState.setTerminated();
        return;
    }
    pathState.throughput /= continueProb;
    
    // 材质分类（用于后续优化）
    MaterialCategory category = classifyMaterial(bsdf);
    pathState.setMaterialCategory(category);
}
```

**输出**：
- 更新 `pathStateBuffer`
- 填充 `surfacePointBuffer`
- 累积部分 `contribution`（隐式光源采样）

---

### 4.4 Kernel 4: SampleLights - 显式光源采样

**文件**: `libVLR/GPU_kernels/wavefront_sample_lights.cu`

**功能**：
- Next Event Estimation (NEE)
- 对每个活跃路径采样一个光源
- 发射阴影光线测试可见性
- 累积直接光照贡献

**伪代码**：
```cpp
CUDA_DEVICE_KERNEL void wavefrontSampleLights() {
    uint32_t workIndex = blockIdx.x * blockDim.x + threadIdx.x;
    if (workIndex >= *wlp.activePathQueue.counter)
        return;
    
    uint32_t pathIndex = wlp.activePathQueue.pathIndices[workIndex];
    WavefrontPathState& pathState = wlp.pathStateBuffer[pathIndex];
    
    if (!pathState.isActive())
        return;
    
    const SurfacePoint& surfPt = wlp.surfacePointBuffer[pathIndex];
    const WavefrontHitInfo& hitInfo = wlp.hitInfoBuffer[pathIndex];
    
    // 获取 BSDF
    const GeometryInstance& geomInst = wlp.geomInstBuffer[hitInfo.geomInstIndex];
    const SurfaceMaterialDescriptor& matDesc = wlp.materialDescriptorBuffer[geomInst.materialIndex];
    BSDF<TransportMode::Radiance> bsdf(matDesc, surfPt, pathState.wls);
    
    // 跳过 delta 材质（镜面反射等）
    if (!bsdf.hasNonDelta())
        return;
    
    // 选择光源
    float uLight = pathState.rng.getFloat0cTo1o();
    SurfaceLight light;
    float lightProb;
    float uPrim;
    selectSurfaceLight(uLight, &light, &lightProb, &uPrim);
    
    // 采样光源位置
    SurfaceLightPosSample lpSample(uPrim, 
        pathState.rng.getFloat0cTo1o(), 
        pathState.rng.getFloat0cTo1o());
    SurfaceLightPosQueryResult lpResult;
    light.sample(lpSample, surfPt.position, &lpResult);
    
    // 评估光源辐射
    const SurfaceMaterialDescriptor& lightMatDesc = wlp.materialDescriptorBuffer[lpResult.materialIndex];
    EDF ledf(lightMatDesc, lpResult.surfPt, pathState.wls);
    SampledSpectrum M = ledf.evaluateEmittance();
    
    if (!M.hasNonZero())
        return;
    
    // 可见性测试
    Vector3D shadowRayDir;
    float squaredDistance;
    float fractionalVisibility;
    if (testVisibility<WFRayType::Shadow>(
            surfPt, lpResult.surfPt, pathState.wls,
            &shadowRayDir, &squaredDistance, &fractionalVisibility)) {
        
        // 计算 BSDF 和几何项
        Vector3D shadowRayDir_sn = surfPt.toLocal(shadowRayDir);
        Normal3D geomNormalLocal = surfPt.shadingFrame.toLocal(surfPt.geometricNormal);
        BSDFQuery fsQuery(surfPt.toLocal(-pathState.direction), geomNormalLocal,
                         TransportMode::Radiance, DirectionType::All(), pathState.wls);
        
        SampledSpectrum fs = bsdf.evaluate(fsQuery, shadowRayDir_sn);
        
        // MIS 权重
        float lightPDF = lightProb * lpResult.areaPDF;
        float bsdfPDF = bsdf.evaluatePDF(fsQuery, shadowRayDir_sn) * /* ... */;
        float MISWeight = (lightPDF * lightPDF) / (lightPDF * lightPDF + bsdfPDF * bsdfPDF);
        
        // 累积贡献
        EDFQuery feQuery(DirectionType::All(), pathState.wls);
        SampledSpectrum Le = M * ledf.evaluate(feQuery, /* ... */);
        float G = fractionalVisibility * /* 几何项 */;
        
        pathState.contribution += pathState.throughput * Le * fs * G * MISWeight / lightPDF;
    }
}
```

**优化**：
- 可以将阴影光线追踪分离为独立的 kernel
- 使用 OptiX Motion Blur API 批量发射阴影光线

---

### 4.5 Kernel 5: SampleBSDF - BSDF 采样

**文件**: `libVLR/GPU_kernels/wavefront_sample_bsdf.cu`

**功能**：
- 采样 BSDF 生成下一跳方向
- 更新路径吞吐量
- 生成下一轮光线
- 将活跃路径加入下一轮队列

**伪代码**：
```cpp
CUDA_DEVICE_KERNEL void wavefrontSampleBSDF() {
    uint32_t workIndex = blockIdx.x * blockDim.x + threadIdx.x;
    if (workIndex >= *wlp.activePathQueue.counter)
        return;
    
    uint32_t pathIndex = wlp.activePathQueue.pathIndices[workIndex];
    WavefrontPathState& pathState = wlp.pathStateBuffer[pathIndex];
    
    if (!pathState.isActive())
        return;
    
    const SurfacePoint& surfPt = wlp.surfacePointBuffer[pathIndex];
    const WavefrontHitInfo& hitInfo = wlp.hitInfoBuffer[pathIndex];
    
    // 获取 BSDF
    const GeometryInstance& geomInst = wlp.geomInstBuffer[hitInfo.geomInstIndex];
    const SurfaceMaterialDescriptor& matDesc = wlp.materialDescriptorBuffer[geomInst.materialIndex];
    BSDF<TransportMode::Radiance> bsdf(matDesc, surfPt, pathState.wls);
    
    // BSDF 采样
    Vector3D dirOutLocal = surfPt.shadingFrame.toLocal(-pathState.direction);
    Normal3D geomNormalLocal = surfPt.shadingFrame.toLocal(surfPt.geometricNormal);
    BSDFQuery fsQuery(dirOutLocal, geomNormalLocal, TransportMode::Radiance,
                     DirectionType::All(), pathState.wls);
    
    BSDFSample sample(pathState.rng.getFloat0cTo1o(),
                     pathState.rng.getFloat0cTo1o(),
                     pathState.rng.getFloat0cTo1o());
    BSDFQueryResult fsResult;
    SampledSpectrum fs = bsdf.sample(fsQuery, sample, &fsResult);
    
    // 检查采样结果
    if (fs == SampledSpectrum::Zero() || fsResult.dirPDF == 0.0f) {
        pathState.setTerminated();
        return;
    }
    
    // 处理色散材质
    if (fsResult.sampledType.isDispersive() && !pathState.wls.singleIsSelected()) {
        fsResult.dirPDF /= SampledSpectrum::NumComponents();
        pathState.wls.setSingleIsSelected();
        pathState.flags |= 0x8; // singleWlSelected = true
    }
    
    // 更新吞吐量
    float cosFactor = dot(fsResult.dirLocal, geomNormalLocal);
    pathState.throughput *= fs * (abs(cosFactor) / fsResult.dirPDF);
    
    // 生成下一跳光线
    Vector3D dirIn = surfPt.fromLocal(fsResult.dirLocal);
    pathState.origin = offsetRayOrigin(surfPt.position,
        cosFactor > 0.0f ? surfPt.geometricNormal : -surfPt.geometricNormal);
    pathState.direction = dirIn;
    pathState.prevDirPDF = fsResult.dirPDF;
    pathState.prevSampledType = fsResult.sampledType;
    
    // 加入下一轮活跃队列
    wlp.nextActivePathQueue.enqueue(pathIndex);
}
```

**输出**：
- 更新 `pathStateBuffer`（新的光线起点和方向）
- `nextActivePathQueue`: 下一轮活跃路径

---

### 4.6 Kernel 6: CompactPaths - 路径压缩

**文件**: `libVLR/GPU_kernels/wavefront_compact_paths.cu`

**功能**：
- Stream Compaction：移除已终止的路径
- 交换活跃队列指针
- 重置计数器

**实现**：
可以使用 CUB 库的 `DeviceSelect::Flagged` 或自定义实现。

**伪代码**：
```cpp
CUDA_DEVICE_KERNEL void wavefrontCompactPaths() {
    // 简化版：直接交换队列指针
    // 实际实现可以使用 CUB 进行更高效的压缩
    
    // 交换队列
    WavefrontWorkQueue temp = wlp.activePathQueue;
    wlp.activePathQueue = wlp.nextActivePathQueue;
    wlp.nextActivePathQueue = temp;
    
    // 重置下一轮队列计数器
    *wlp.nextActivePathQueue.counter = 0;
}
```

**优化选项**：
- 使用 CUB 的 Stream Compaction
- 按材质类型排序路径（减少分支发散）
- 合并相似路径（Path Regeneration）

---

### 4.7 Kernel 7: AccumulateResults - 累积结果

**文件**: `libVLR/GPU_kernels/wavefront_accumulate.cu`

**功能**：
- 将所有路径的贡献值累加到输出缓冲区
- 更新 RNG 状态
- 处理 Denoiser 所需的辅助缓冲区

**伪代码**：
```cpp
CUDA_DEVICE_KERNEL void wavefrontAccumulateResults() {
    uint32_t pathIndex = blockIdx.x * blockDim.x + threadIdx.x;
    uint32_t totalPaths = wlp.imageSize.x * wlp.imageSize.y;
    
    if (pathIndex >= totalPaths)
        return;
    
    WavefrontPathState& pathState = wlp.pathStateBuffer[pathIndex];
    
    // 检查贡献值有效性
    if (!pathState.contribution.allFinite()) {
        vlrprintf("Path %u: Invalid contribution\n", pathIndex);
        return;
    }
    
    // 累积到输出缓冲区
    uint2 pixelCoord = make_uint2(pathState.pixelX, pathState.pixelY);
    
    if (wlp.numAccumFrames == 1)
        wlp.accumBuffer[pixelCoord].reset();
    
    wlp.accumBuffer[pixelCoord].add(pathState.wls, pathState.contribution);
    
    // 更新 RNG 状态
    wlp.rngBuffer.write(pixelCoord, pathState.rng);
}
```

---

## 5. Context 集成

### 5.1 在 Context 中添加 Wavefront Pipeline

**文件**: `libVLR/context.h`

```cpp
class Context : public TypeAwareClass {
    // ... 现有成员 ...
    
    struct OptiX {
        // ... 现有 Pipeline ...
        
        // 新增：Wavefront Path Tracing Pipeline
        struct WavefrontPathTracing {
            optixu::Pipeline pipeline;
            std::vector<optixu::Module> modules;
            
            // Kernel Programs
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
            
            // Wavefront 特定缓冲区
            cudau::TypedBuffer<WavefrontPathState> pathStateBuffer;
            cudau::TypedBuffer<WavefrontHitInfo> hitInfoBuffer;
            cudau::TypedBuffer<SurfacePoint> surfacePointBuffer;
            
            // 工作队列
            cudau::TypedBuffer<uint32_t> activePathIndices;
            cudau::TypedBuffer<uint32_t> nextActivePathIndices;
            cudau::TypedBuffer<uint32_t> queueCounters;
            
            // 材质分类队列（可选）
            cudau::TypedBuffer<uint32_t> materialQueueIndices[NumMaterialCategories];
            cudau::TypedBuffer<uint32_t> materialQueueCounters;
            
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
    
    // ... 现有方法 ...
    
private:
    void renderWavefront(CUstream stream, const Camera* camera,
                        uint32_t shrinkCoeff, bool firstFrame);
};
```

### 5.2 渲染主循环

**文件**: `libVLR/context.cpp`

```cpp
void Context::renderWavefront(CUstream stream, const Camera* camera,
                             uint32_t shrinkCoeff, bool firstFrame) {
    OptiX::WavefrontPathTracing& wf = m_optix.wavefrontPathTracing;
    
    uint2 imageSize = make_uint2(m_width / shrinkCoeff, m_height / shrinkCoeff);
    uint32_t numPixels = imageSize.x * imageSize.y;
    
    // === 阶段 1: 生成初始光线 ===
    optixu::Dim3 launchDim = optixu::Dim3(imageSize.x, imageSize.y, 1);
    wf.pipeline.launch(
        stream, wf.shaderBindingTable, launchDim,
        m_optix.wavefrontLaunchParamsOnDevice,
        sizeof(WavefrontLaunchParameters));
    
    // === 主循环：迭代处理路径 ===
    uint32_t maxDepth = 25; // 最大路径深度
    for (uint32_t depth = 0; depth < maxDepth; ++depth) {
        // 检查是否还有活跃路径
        uint32_t numActivePaths;
        CUDADRV_CHECK(cuMemcpyDtoHAsync(
            &numActivePaths,
            wf.queueCounters.getCUdeviceptr(),
            sizeof(uint32_t), stream));
        CUDADRV_CHECK(cuStreamSynchronize(stream));
        
        if (numActivePaths == 0)
            break; // 所有路径已终止
        
        // === 阶段 2: 光线追踪 ===
        optixu::Dim3 traceLaunchDim = optixu::Dim3(numActivePaths, 1, 1);
        wf.pipeline.launch(
            stream, wf.shaderBindingTable, traceLaunchDim,
            m_optix.wavefrontLaunchParamsOnDevice,
            sizeof(WavefrontLaunchParameters));
        
        // === 阶段 3: 处理命中点 ===
        dim3 blockDim(256);
        dim3 gridDim((numActivePaths + blockDim.x - 1) / blockDim.x);
        wf.processHitsKernel(stream, gridDim, blockDim,
                            /* kernel args */);
        
        // === 阶段 4: 显式光源采样 ===
        wf.sampleLightsKernel(stream, gridDim, blockDim,
                             /* kernel args */);
        
        // === 阶段 5: BSDF 采样 ===
        wf.sampleBSDFKernel(stream, gridDim, blockDim,
                           /* kernel args */);
        
        // === 阶段 6: 路径压缩 ===
        // 可选：使用 CUB Stream Compaction
        wf.compactPathsKernel(stream, dim3(1), dim3(1),
                             /* kernel args */);
    }
    
    // === 阶段 7: 累积结果 ===
    dim3 accumBlockDim(256);
    dim3 accumGridDim((numPixels + accumBlockDim.x - 1) / accumBlockDim.x);
    wf.accumulateResultsKernel(stream, accumGridDim, accumBlockDim,
                              /* kernel args */);
}
```

---

## 6. 内存布局优化

### 6.1 缓冲区大小估算

对于 1920x1080 分辨率：
- **PathState**: ~200 bytes/path × 2,073,600 paths = ~400 MB
- **HitInfo**: ~32 bytes/path × 2,073,600 paths = ~64 MB
- **SurfacePoint**: ~128 bytes/path × 2,073,600 paths = ~256 MB
- **工作队列**: ~4 bytes/path × 2,073,600 paths × 2 = ~16 MB
- **总计**: ~736 MB（不包括场景数据）

### 6.2 内存优化策略

#### 策略 1: 按需分配
```cpp
// 只为活跃路径分配 SurfacePoint
// 使用压缩索引映射
```

#### 策略 2: 结构体分割（SoA vs AoS）
```cpp
// 当前：Array of Structures (AoS)
struct PathState { Point3D origin; Vector3D direction; /* ... */ };
PathState paths[N];

// 优化：Structure of Arrays (SoA)
struct PathStateBuffers {
    Point3D* origins;
    Vector3D* directions;
    SampledSpectrum* throughputs;
    // ...
};
```

**优点**：
- 更好的内存合并访问（Coalesced Access）
- 减少缓存失效

**缺点**：
- 代码复杂度增加
- 需要多次内存访问

**建议**：初期使用 AoS，性能优化阶段再考虑 SoA。

#### 策略 3: 双缓冲
```cpp
// 使用两个 PathState 缓冲区交替使用
PathState* pathStateBuffers[2];
int currentBuffer = 0;

// 每次迭代后交换
currentBuffer = 1 - currentBuffer;
```

---

## 7. 路径排序与分类

### 7.1 材质分类

**目的**：减少 BSDF 评估时的分支发散

**实现**：
```cpp
enum MaterialCategory : uint32_t {
    MaterialCategory_Diffuse = 0,      // Lambert, Matte
    MaterialCategory_Glossy,           // GGX, UE4
    MaterialCategory_Specular,         // 理想镜面
    MaterialCategory_Transmissive,     // 透射材质
    MaterialCategory_Emissive,         // 发光材质
    MaterialCategory_Mixed,            // 混合材质
    NumMaterialCategories
};

// 在 ProcessHits kernel 中分类
CUDA_DEVICE_FUNCTION MaterialCategory classifyMaterial(const BSDF& bsdf) {
    if (bsdf.matches(DirectionType::Delta0D()))
        return MaterialCategory_Specular;
    if (bsdf.matches(DirectionType::Transmission()))
        return MaterialCategory_Transmissive;
    if (bsdf.matches(DirectionType::HighFreq()))
        return MaterialCategory_Glossy;
    return MaterialCategory_Diffuse;
}
```

### 7.2 路径排序 Kernel

**文件**: `libVLR/GPU_kernels/wavefront_sort_paths.cu`

**功能**：按材质类别排序路径，使相同材质的路径连续

**实现**：
- 使用 CUB 的 `DeviceRadixSort::SortPairs`
- 或使用 Thrust 的 `sort_by_key`

```cpp
// 排序键：材质类别
// 排序值：路径索引

thrust::sort_by_key(
    thrust::device,
    materialCategories,
    materialCategories + numActivePaths,
    pathIndices);
```

**性能影响**：
- 排序开销：~5-10% 额外时间
- 性能提升：BSDF 评估加速 20-30%
- **净收益**：10-20% 整体性能提升

---

## 8. OptiX 7 集成策略

### 8.1 挑战

OptiX 7 主要为递归式设计，Wavefront 需要特殊处理：

1. **Payload 限制**：OptiX 7 Payload 大小有限（32 dwords）
2. **递归深度**：Wavefront 不需要递归，但需要多次 launch
3. **Callable Programs**：BSDF/EDF 评估需要在 CUDA kernel 中调用

### 8.2 解决方案

#### 方案 1: 最小化 Payload
```cpp
// 只传递路径索引和波长
struct WFTracePayload {
    uint32_t pathIndex;      // 4 bytes
    WavelengthSamples wls;   // ~16 bytes
    // 总计: ~20 bytes (5 dwords)
};
```

#### 方案 2: 使用全局内存
```cpp
// 在 Closest Hit 中直接访问全局缓冲区
CUDA_DEVICE_KERNEL void RT_CH_NAME(wavefrontClosestHit)() {
    WFTracePayload payload;
    WFTracePayloadSignature::get(&payload);
    
    uint32_t pathIndex = payload.pathIndex;
    
    // 直接访问全局缓冲区
    WavefrontPathState& pathState = wlp.pathStateBuffer[pathIndex];
    WavefrontHitInfo& hitInfo = wlp.hitInfoBuffer[pathIndex];
    
    // 填充命中信息
    // ...
}
```

#### 方案 3: 分离 BSDF 评估
```cpp
// 不在 Closest Hit 中评估 BSDF
// 而是在独立的 CUDA kernel 中处理
// 这样可以避免 Callable Program 的开销
```

---

## 9. 高级优化

### 9.1 路径再生（Path Regeneration）

**概念**：当大量路径终止后，重新生成新路径以保持 GPU 占用率。

**实现**：
```cpp
// 在每次迭代后检查活跃路径数
if (numActivePaths < threshold) {
    // 为已终止的路径重新生成新路径
    regeneratePaths(stream, numPixels - numActivePaths);
}
```

### 9.2 自适应采样

**概念**：为高方差区域分配更多样本。

**实现**：
```cpp
// 计算每个像素的方差
computeVariance(accumBuffer, varianceBuffer);

// 根据方差生成更多路径
generateAdaptivePaths(varianceBuffer, pathStateBuffer);
```

### 9.3 材质特化 Kernel

**概念**：为不同材质类型创建特化的 kernel。

**实现**：
```cpp
// 为漫反射材质优化的 kernel
CUDA_DEVICE_KERNEL void wavefrontSampleBSDF_Diffuse() {
    // 只处理漫反射材质
    // 移除分支判断
}

// 为光滑反射材质优化的 kernel
CUDA_DEVICE_KERNEL void wavefrontSampleBSDF_Glossy() {
    // 只处理 GGX 等光滑材质
}
```

### 9.4 多流并行

**概念**：使用多个 CUDA 流并行处理不同阶段。

**实现**：
```cpp
CUstream streams[3];

// Stream 0: 当前深度的光线追踪
// Stream 1: 前一深度的 BSDF 采样
// Stream 2: 前前深度的结果累积

// 流水线并行
for (uint32_t depth = 0; depth < maxDepth; ++depth) {
    int streamIdx = depth % 3;
    
    traceRays(streams[streamIdx], depth);
    processHits(streams[(streamIdx + 1) % 3], depth - 1);
    sampleBSDF(streams[(streamIdx + 2) % 3], depth - 2);
}
```

---

## 10. 实现路线图

### 阶段 1: 基础架构（2-3 周）
- [ ] 定义 Wavefront 数据结构
  - `WavefrontPathState`
  - `WavefrontHitInfo`
  - `WavefrontWorkQueue`
  - `WavefrontLaunchParameters`

- [ ] 创建头文件
  - `libVLR/shared/wavefront_types.h`
  - `libVLR/shared/wavefront_common.h`

- [ ] 在 `public_types.h` 中添加渲染器类型
  ```cpp
  VLRRenderer_WavefrontPathTracing = 4,
  ```

### 阶段 2: 核心 Kernel 实现（3-4 周）
- [ ] 实现 GenerateRays kernel
  - `libVLR/GPU_kernels/wavefront_generate_rays.cu`
  - 相机采样逻辑
  - PathState 初始化

- [ ] 实现 TraceRays kernel
  - `libVLR/GPU_kernels/wavefront_trace_rays.cu`
  - OptiX Ray Generation Program
  - Closest Hit Program
  - Miss Program

- [ ] 实现 ProcessHits kernel
  - `libVLR/GPU_kernels/wavefront_process_hits.cu`
  - 表面点计算
  - BSDF/EDF 评估
  - 隐式光源采样

- [ ] 实现 SampleLights kernel
  - `libVLR/GPU_kernels/wavefront_sample_lights.cu`
  - 光源选择和采样
  - 阴影光线追踪
  - MIS 权重计算

- [ ] 实现 SampleBSDF kernel
  - `libVLR/GPU_kernels/wavefront_sample_bsdf.cu`
  - BSDF 采样
  - 路径延续/终止判断

- [ ] 实现 AccumulateResults kernel
  - `libVLR/GPU_kernels/wavefront_accumulate.cu`
  - 贡献值累积
  - RNG 状态更新

### 阶段 3: Context 集成（2 周）
- [ ] 在 `context.h` 中添加 WavefrontPathTracing 结构
- [ ] 在 `context.cpp` 中实现 Pipeline 初始化
  - 加载 PTX 模块
  - 创建 Programs
  - 编译 CUDA Kernels
  - 分配缓冲区

- [ ] 实现 `renderWavefront()` 方法
  - 主渲染循环
  - Kernel 调度逻辑
  - 性能计时

- [ ] 更新 `render()` 方法支持 Wavefront
  ```cpp
  void Context::render(...) {
      switch (m_renderer) {
      case VLRRenderer_PathTracing:
          renderPathTracing(stream, camera, ...);
          break;
      case VLRRenderer_WavefrontPathTracing:
          renderWavefront(stream, camera, ...);
          break;
      // ...
      }
  }
  ```

### 阶段 4: 功能完善（2-3 周）
- [ ] 支持所有 BSDF 类型
  - Lambert, Specular, GGX
  - UE4/Frostbite BRDF
  - 混合材质

- [ ] 支持所有光源类型
  - 区域光
  - 点光源
  - 环境光（IBL）

- [ ] 支持高级特性
  - 景深效果（DOF）
  - 法线贴图
  - Alpha 纹理
  - 几何实例化

- [ ] Denoiser 集成
  - 生成 Albedo/Normal 辅助缓冲区
  - 与 OptiX Denoiser 集成

### 阶段 5: 优化（2-3 周）
- [ ] 路径排序
  - 按材质类型排序
  - 使用 CUB/Thrust

- [ ] Stream Compaction
  - 使用 CUB `DeviceSelect::Flagged`
  - 优化队列管理

- [ ] 内存优化
  - SoA 布局实验
  - 减少缓冲区大小
  - 内存池复用

- [ ] 材质特化 Kernel
  - 为常见材质创建特化版本
  - 减少分支发散

- [ ] 多流并行
  - 流水线并行
  - 异步内存传输

### 阶段 6: 测试与验证（1-2 周）
- [ ] 正确性测试
  - 与原 Path Tracing 结果对比
  - 各种场景测试
  - 边界情况测试

- [ ] 性能测试
  - 不同分辨率
  - 不同场景复杂度
  - GPU 占用率分析

- [ ] 调试工具
  - 路径可视化
  - 性能计数器
  - 调试渲染模式

### 阶段 7: UI 集成（1 周）
- [ ] 更新 HostProgram
  - 在 `main.cpp` 中添加 Wavefront 选项
  - UI 控件
  - 性能显示

### 阶段 8: 文档（1 周）
- [ ] 技术文档
- [ ] API 文档
- [ ] 性能对比报告
- [ ] 更新 README.md

---

## 11. 详细数据结构定义

### 11.1 完整的 WavefrontPathState
```cpp
// 文件: libVLR/shared/wavefront_types.h

namespace vlr::shared {

struct alignas(16) WavefrontPathState {
    // === 光线信息 (32 bytes) ===
    Point3D origin;                    // 12 bytes
    Vector3D direction;                // 12 bytes
    float _padding1[2];                // 8 bytes (对齐)
    
    // === 光谱信息 (64 bytes) ===
    SampledSpectrum throughput;        // 16 bytes (4 floats)
    SampledSpectrum contribution;      // 16 bytes (4 floats)
    WavelengthSamples wls;             // 24 bytes
    float initImportance;              // 4 bytes
    float selectWLPDF;                 // 4 bytes
    
    // === 随机数生成器 (16 bytes) ===
    KernelRNG rng;                     // 16 bytes (PCG32)
    
    // === 路径历史 (16 bytes) ===
    float prevDirPDF;                  // 4 bytes
    DirectionType prevSampledType;     // 4 bytes
    uint32_t pathLength;               // 4 bytes
    uint32_t _padding2;                // 4 bytes
    
    // === 像素坐标 (8 bytes) ===
    uint32_t pixelX;                   // 4 bytes
    uint32_t pixelY;                   // 4 bytes
    
    // === 状态标志 (8 bytes) ===
    uint32_t flags;                    // 4 bytes
    uint32_t materialCategory;         // 4 bytes
    
    // === 总大小: 144 bytes ===
    
    // 内联辅助方法
    CUDA_DEVICE_FUNCTION CUDA_INLINE bool isActive() const {
        return flags & 0x1;
    }
    
    CUDA_DEVICE_FUNCTION CUDA_INLINE bool isTerminated() const {
        return flags & 0x2;
    }
    
    CUDA_DEVICE_FUNCTION CUDA_INLINE void setActive(bool active) {
        if (active) flags |= 0x1;
        else flags &= ~0x1;
    }
    
    CUDA_DEVICE_FUNCTION CUDA_INLINE void setTerminated() {
        flags |= 0x2;
        flags &= ~0x1;
    }
    
    CUDA_DEVICE_FUNCTION CUDA_INLINE bool maxLengthReached() const {
        return flags & 0x4;
    }
    
    CUDA_DEVICE_FUNCTION CUDA_INLINE void setMaxLengthReached() {
        flags |= 0x4;
    }
};

static_assert(sizeof(WavefrontPathState) == 144, "PathState size mismatch");

} // namespace vlr::shared
```

### 11.2 完整的 WavefrontHitInfo
```cpp
namespace vlr::shared {

struct alignas(16) WavefrontHitInfo {
    // === 命中几何信息 (16 bytes) ===
    uint32_t instIndex;                // 4 bytes
    uint32_t geomInstIndex;            // 4 bytes
    uint32_t primIndex;                // 4 bytes
    uint32_t hitFlags;                 // 4 bytes
    
    // === 参数化坐标 (16 bytes) ===
    float u, v;                        // 8 bytes (重心坐标)
    float t;                           // 4 bytes (光线参数)
    float _padding;                    // 4 bytes
    
    // === 总大小: 32 bytes ===
    
    CUDA_DEVICE_FUNCTION CUDA_INLINE bool hasHit() const {
        return hitFlags & 0x1;
    }
    
    CUDA_DEVICE_FUNCTION CUDA_INLINE bool hitInfinity() const {
        return hitFlags & 0x2;
    }
    
    CUDA_DEVICE_FUNCTION CUDA_INLINE bool hitEmissive() const {
        return hitFlags & 0x4;
    }
    
    CUDA_DEVICE_FUNCTION CUDA_INLINE void setHasHit(bool hit) {
        if (hit) hitFlags |= 0x1;
        else hitFlags &= ~0x1;
    }
    
    CUDA_DEVICE_FUNCTION CUDA_INLINE void setHitInfinity(bool inf) {
        if (inf) hitFlags |= 0x2;
        else hitFlags &= ~0x2;
    }
    
    CUDA_DEVICE_FUNCTION CUDA_INLINE void reset() {
        instIndex = 0xFFFFFFFF;
        geomInstIndex = 0xFFFFFFFF;
        primIndex = 0xFFFFFFFF;
        hitFlags = 0;
        u = v = t = 0.0f;
    }
};

static_assert(sizeof(WavefrontHitInfo) == 32, "HitInfo size mismatch");

} // namespace vlr::shared
```

### 11.3 工作队列管理
```cpp
namespace vlr::shared {

struct WavefrontWorkQueue {
    uint32_t* pathIndices;             // 路径索引数组（GPU）
    uint32_t* counter;                 // 原子计数器（GPU）
    uint32_t capacity;                 // 队列容量
    
    CUDA_DEVICE_FUNCTION CUDA_INLINE uint32_t size() const {
        return *counter;
    }
    
    CUDA_DEVICE_FUNCTION CUDA_INLINE uint32_t enqueue(uint32_t pathIndex) {
        uint32_t slot = atomicAdd(counter, 1);
        if (slot < capacity) {
            pathIndices[slot] = pathIndex;
            return slot;
        }
        return 0xFFFFFFFF;
    }
    
    CUDA_DEVICE_FUNCTION CUDA_INLINE void reset() {
        *counter = 0;
    }
};

// 主机端管理类
class WavefrontWorkQueueManager {
    cudau::TypedBuffer<uint32_t> m_pathIndices;
    cudau::TypedBuffer<uint32_t> m_counter;
    uint32_t m_capacity;
    
public:
    void initialize(CUcontext cuContext, uint32_t capacity) {
        m_capacity = capacity;
        m_pathIndices.initialize(cuContext, cudau::BufferType::Device, capacity);
        m_counter.initialize(cuContext, cudau::BufferType::Device, 1);
        reset();
    }
    
    void finalize() {
        m_pathIndices.finalize();
        m_counter.finalize();
    }
    
    void reset(CUstream stream = 0) {
        uint32_t zero = 0;
        CUDADRV_CHECK(cuMemcpyHtoDAsync(
            m_counter.getCUdeviceptr(), &zero, sizeof(uint32_t), stream));
    }
    
    uint32_t getSize(CUstream stream) {
        uint32_t size;
        CUDADRV_CHECK(cuMemcpyDtoHAsync(
            &size, m_counter.getCUdeviceptr(), sizeof(uint32_t), stream));
        CUDADRV_CHECK(cuStreamSynchronize(stream));
        return size;
    }
    
    WavefrontWorkQueue getDeviceQueue() const {
        WavefrontWorkQueue queue;
        queue.pathIndices = m_pathIndices.getDevicePointer();
        queue.counter = m_counter.getDevicePointer();
        queue.capacity = m_capacity;
        return queue;
    }
};

} // namespace vlr::shared
```

---

## 12. Pipeline 初始化代码

### 12.1 Context 构造函数中的初始化

**文件**: `libVLR/context.cpp`

```cpp
Context::Context(CUcontext cuContext, bool logging, uint32_t maxCallableDepth) {
    // ... 现有初始化代码 ...
    
    // === 初始化 Wavefront Path Tracing Pipeline ===
    {
        OptiX::WavefrontPathTracing& wf = m_optix.wavefrontPathTracing;
        
        // 创建 Pipeline
        wf.pipeline = m_optix.context.createPipeline();
        wf.pipeline.setPipelineOptions(
            std::max(shared::WFTracePayloadSignature::numDwords,
                    shared::ShadowPayloadSignature::numDwords),
            0, // 不需要 attribute
            "wlp", sizeof(shared::WavefrontLaunchParameters),
            OPTIX_TRAVERSABLE_GRAPH_FLAG_ALLOW_SINGLE_LEVEL_INSTANCING,
            OPTIX_EXCEPTION_FLAG_STACK_OVERFLOW,
            OPTIX_PRIMITIVE_TYPE_FLAGS_TRIANGLE);
        wf.pipeline.setNumMissRayTypes(shared::WFRayType::NumTypes);
        
        // 加载模块
        wf.modules.resize(NumOptiXModules);
        wf.modules[OptiXModule_LightTransport] = wf.pipeline.createModuleFromPTXString(
            readTxtFile(exeDir / "libvlr/ptxes/wavefront_trace_rays.ptx"),
            OPTIX_COMPILE_DEFAULT_MAX_REGISTER_COUNT,
            OPTIX_COMPILE_OPTIMIZATION_LEVEL_3,
            OPTIX_COMPILE_DEBUG_LEVEL_NONE);
        
        // 加载公共模块（材质、几何等）
        for (int i = 0; i < static_cast<int>(lengthof(commonModulePaths)); ++i) {
            wf.modules[OptiXModule_ShaderNode + i] = wf.pipeline.createModuleFromPTXString(
                readTxtFile(exeDir / commonModulePaths[i]),
                OPTIX_COMPILE_DEFAULT_MAX_REGISTER_COUNT,
                OPTIX_COMPILE_OPTIMIZATION_LEVEL_3,
                OPTIX_COMPILE_DEBUG_LEVEL_NONE);
        }
        
        // 创建 Programs
        wf.generateRays = wf.pipeline.createRayGenProgram(
            wf.modules[OptiXModule_LightTransport], RT_RG_NAME_STR("wavefrontGenerateRays"));
        
        wf.traceRaysRayGen = wf.pipeline.createRayGenProgram(
            wf.modules[OptiXModule_LightTransport], RT_RG_NAME_STR("wavefrontTraceRays"));
        
        wf.traceRaysMiss = wf.pipeline.createMissProgram(
            wf.modules[OptiXModule_LightTransport], RT_MS_NAME_STR("wavefrontMiss"));
        
        wf.traceRaysHitGroupDefault = wf.pipeline.createHitProgramGroupForTriangleIS(
            wf.modules[OptiXModule_LightTransport], RT_CH_NAME_STR("wavefrontClosestHit"),
            optixu::Module(), nullptr);
        
        wf.traceRaysHitGroupWithAlpha = wf.pipeline.createHitProgramGroupForTriangleIS(
            wf.modules[OptiXModule_LightTransport], RT_CH_NAME_STR("wavefrontClosestHit"),
            wf.modules[OptiXModule_LightTransport], RT_AH_NAME_STR("wavefrontAnyHitWithAlpha"));
        
        wf.shadowHitGroupDefault = wf.pipeline.createHitProgramGroupForTriangleIS(
            optixu::Module(), nullptr,
            wf.modules[OptiXModule_LightTransport], RT_AH_NAME_STR("shadowAnyHitDefault"));
        
        wf.shadowHitGroupWithAlpha = wf.pipeline.createHitProgramGroupForTriangleIS(
            optixu::Module(), nullptr,
            wf.modules[OptiXModule_LightTransport], RT_AH_NAME_STR("shadowAnyHitWithAlpha"));
        
        wf.emptyHitGroup = wf.pipeline.createEmptyHitProgramGroup();
        
        // 设置 Ray Generation Program
        wf.pipeline.setRayGenerationProgram(wf.generateRays);
        wf.pipeline.setMissProgram(shared::WFRayType::Closest, wf.traceRaysMiss);
        wf.pipeline.setMissProgram(shared::WFRayType::Shadow, optixu::Program());
        
        // 创建 Callable Programs（复用现有的 BSDF/EDF）
        // ...
        
        // 链接 Pipeline
        wf.pipeline.link(2, VLR_DEBUG_SELECT(OPTIX_COMPILE_DEBUG_LEVEL_FULL, OPTIX_COMPILE_DEBUG_LEVEL_NONE));
        
        // 创建 Shader Binding Table
        // ...
        
        // 分配 Wavefront 缓冲区
        uint32_t maxNumPaths = 1920 * 1080; // 默认分辨率
        wf.pathStateBuffer.initialize(m_cuContext, cudau::BufferType::Device, maxNumPaths);
        wf.hitInfoBuffer.initialize(m_cuContext, cudau::BufferType::Device, maxNumPaths);
        wf.surfacePointBuffer.initialize(m_cuContext, cudau::BufferType::Device, maxNumPaths);
        
        wf.activePathIndices.initialize(m_cuContext, cudau::BufferType::Device, maxNumPaths);
        wf.nextActivePathIndices.initialize(m_cuContext, cudau::BufferType::Device, maxNumPaths);
        wf.queueCounters.initialize(m_cuContext, cudau::BufferType::Device, 2);
        
        // 材质分类队列（可选）
        for (int i = 0; i < NumMaterialCategories; ++i) {
            wf.materialQueueIndices[i].initialize(m_cuContext, cudau::BufferType::Device, maxNumPaths);
        }
        wf.materialQueueCounters.initialize(m_cuContext, cudau::BufferType::Device, NumMaterialCategories);
        
        // 加载 CUDA Kernels
        CUmodule wavefrontModule;
        CUDADRV_CHECK(cuModuleLoad(&wavefrontModule,
            (exeDir / "libvlr/ptxes/wavefront_kernels.ptx").string().c_str()));
        
        wf.processHitsKernel.initialize(wavefrontModule, "wavefrontProcessHits", 0);
        wf.sampleLightsKernel.initialize(wavefrontModule, "wavefrontSampleLights", 0);
        wf.sampleBSDFKernel.initialize(wavefrontModule, "wavefrontSampleBSDF", 0);
        wf.compactPathsKernel.initialize(wavefrontModule, "wavefrontCompactPaths", 0);
        wf.accumulateResultsKernel.initialize(wavefrontModule, "wavefrontAccumulateResults", 0);
        
        // 初始化 Launch Parameters
        m_optix.wavefrontLaunchParams = {};
        CUDADRV_CHECK(cuMemAlloc(&m_optix.wavefrontLaunchParamsOnDevice,
                                sizeof(shared::WavefrontLaunchParameters)));
    }
}
```

---

## 13. CMakeLists.txt 更新

**文件**: `libVLR/CMakeLists.txt`

需要添加新的 CUDA 源文件：

```cmake
# Wavefront Path Tracing kernels
set(WAVEFRONT_KERNELS
    GPU_kernels/wavefront_generate_rays.cu
    GPU_kernels/wavefront_trace_rays.cu
    GPU_kernels/wavefront_process_hits.cu
    GPU_kernels/wavefront_sample_lights.cu
    GPU_kernels/wavefront_sample_bsdf.cu
    GPU_kernels/wavefront_accumulate.cu
    GPU_kernels/wavefront_utils.cu
)

# 编译 Wavefront kernels 为 PTX
foreach(KERNEL_FILE ${WAVEFRONT_KERNELS})
    # ... PTX 编译配置 ...
endforeach()
```

---

## 14. 调试策略

### 14.1 调试渲染模式

添加 Wavefront 特定的调试模式：

```cpp
enum VLRWavefrontDebugMode {
    WFDebug_PathLength = 0,        // 可视化路径长度
    WFDebug_MaterialCategory,      // 可视化材质分类
    WFDebug_Throughput,            // 可视化路径吞吐量
    WFDebug_NumBounces,            // 可视化弹射次数
    WFDebug_ActivePaths,           // 可视化活跃路径分布
};
```

### 14.2 性能分析

```cpp
struct WavefrontPerformanceStats {
    uint32_t numInitialPaths;
    uint32_t numActivePathsPerDepth[MaxPathLength];
    uint32_t numShadowRays;
    uint32_t numBSDFSamples;
    float kernelTimes[NumWavefrontKernels];
    float totalTime;
};
```

### 14.3 验证工具

```cpp
// 对比 Wavefront 和递归式结果
void validateWavefrontResults(
    const SpectrumStorage* wavefrontBuffer,
    const SpectrumStorage* recursiveBuffer,
    uint32_t width, uint32_t height,
    float tolerance = 0.01f);
```

---

## 15. 性能预测

### 15.1 理论分析

**递归式 Path Tracing**：
- 线程利用率：40-60%（分支发散严重）
- 内存访问：随机访问模式
- 寄存器使用：高（递归调用栈）

**Wavefront Path Tracing**：
- 线程利用率：80-95%（同质化工作负载）
- 内存访问：更规律的访问模式
- 寄存器使用：中等（无递归）

### 15.2 预期性能提升

| 场景类型 | 材质复杂度 | 预期加速比 | GPU 占用率提升 |
|---------|-----------|-----------|--------------|
| Cornell Box | 低 | 1.5x | 40% → 75% |
| 室内场景 | 中 | 2.0x | 50% → 85% |
| 复杂场景 | 高 | 2.5-3.0x | 45% → 90% |

### 15.3 内存开销

| 分辨率 | PathState | HitInfo | SurfacePoint | 队列 | 总计 |
|-------|-----------|---------|--------------|------|------|
| 1280x720 | 133 MB | 29 MB | 118 MB | 7 MB | 287 MB |
| 1920x1080 | 299 MB | 65 MB | 265 MB | 16 MB | 645 MB |
| 3840x2160 | 1196 MB | 262 MB | 1060 MB | 66 MB | 2584 MB |

---

## 16. 风险与挑战

### 16.1 技术风险

1. **OptiX 7 限制**
   - Payload 大小限制
   - 递归深度限制（虽然 Wavefront 不需要递归）
   - Callable Program 性能开销

2. **内存压力**
   - 高分辨率下显存占用大
   - 可能需要分块渲染（Tiled Rendering）

3. **复杂性增加**
   - 代码量增加 2-3 倍
   - 调试难度提高
   - 维护成本增加

### 16.2 缓解策略

1. **最小化 Payload**
   - 只传递路径索引
   - 使用全局内存存储状态

2. **分块渲染**
   - 将图像分为多个 tile
   - 每次只处理一个 tile 的路径

3. **渐进式实现**
   - 先实现基础功能
   - 逐步添加优化
   - 保持与递归式版本的兼容性

---

## 17. 测试计划

### 17.1 单元测试

- [ ] PathState 初始化测试
- [ ] 工作队列操作测试
- [ ] 材质分类测试
- [ ] Stream Compaction 测试

### 17.2 集成测试

- [ ] Cornell Box 场景
- [ ] 简单几何体（球体、立方体）
- [ ] 复杂网格模型
- [ ] 各种材质类型
- [ ] 环境光照

### 17.3 性能测试

- [ ] 不同分辨率下的性能
- [ ] 不同场景复杂度
- [ ] 不同材质组合
- [ ] GPU 占用率分析
- [ ] 内存带宽分析

### 17.4 正确性验证

- [ ] 像素级对比（与递归式）
- [ ] 统计误差分析
- [ ] 视觉质量评估
- [ ] 边界情况测试

---

## 18. 参考实现

### 18.1 PBRT-v4 Wavefront

PBRT-v4 是最完整的 Wavefront 参考实现：
- 文件: `src/pbrt/gpu/pathintegrator.cpp`
- 使用多个 CUDA kernel 分阶段处理
- 完整的材质排序和 Stream Compaction

### 18.2 关键论文

1. **[Laine2013]** "Megakernels Considered Harmful: Wavefront Path Tracing on GPUs"
   - Wavefront 架构的奠基论文
   - 详细的性能分析

2. **[Pharr2023]** "Physically Based Rendering: From Theory To Implementation (4th Edition)"
   - PBRT-v4 的完整实现
   - 第 15 章详细介绍 Wavefront

3. **[Novák2010]** "Understanding the Efficiency of Ray Traversal on GPUs"
   - GPU 光线追踪性能分析

---

## 19. 下一步行动

### 立即开始（本周）
1. 创建 `libVLR/shared/wavefront_types.h`
2. 定义核心数据结构
3. 在 `public_types.h` 中添加 `VLRRenderer_WavefrontPathTracing`
4. 创建第一个 kernel: `wavefront_generate_rays.cu`

### 短期目标（2 周内）
1. 实现所有 7 个核心 kernel
2. 基础的 Pipeline 集成
3. 简单场景测试（Cornell Box）

### 中期目标（1 个月内）
1. 支持所有材质和光源类型
2. 基础优化（路径排序、Stream Compaction）
3. 性能测试和对比

### 长期目标（2-3 个月内）
1. 高级优化（材质特化、多流并行）
2. 完整的测试覆盖
3. 文档和示例
4. 发布和推广

---

## 20. 总结

Wavefront 路径追踪是一个复杂但值得的改进：
- **性能提升**：1.5-3x 渲染速度
- **GPU 利用率**：从 40-60% 提升到 80-95%
- **可扩展性**：更容易添加新的优化

**关键成功因素**：
1. 仔细的数据结构设计
2. 高效的内存管理
3. 渐进式实现和测试
4. 持续的性能分析和优化

**预计工作量**：
- **核心实现**：6-8 周
- **优化和测试**：4-6 周
- **文档和发布**：2 周
- **总计**：3-4 个月
