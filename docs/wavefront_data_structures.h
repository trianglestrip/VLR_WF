// ============================================================================
// Wavefront Path Tracing - 数据结构定义
// 
// 这是一个参考实现文件，展示 Wavefront 模式所需的完整数据结构
// 实际实现时应该放在 libVLR/shared/wavefront_types.h
// ============================================================================

#pragma once

#include "../shared/kernel_common.h"

namespace vlr::shared {

// ============================================================================
// 1. 核心数据结构
// ============================================================================

/// 路径状态：存储单条光线路径的完整信息
/// 这是 Wavefront 架构的核心数据结构
struct alignas(16) WavefrontPathState {
    // === 光线信息 (32 bytes) ===
    Point3D origin;                    // 光线起点 (12 bytes)
    Vector3D direction;                // 光线方向 (12 bytes)
    float _padding1[2];                // 对齐填充 (8 bytes)
    
    // === 光谱和吞吐量 (64 bytes) ===
    SampledSpectrum throughput;        // 路径吞吐量/权重 (16 bytes)
    SampledSpectrum contribution;      // 累积的辐射贡献 (16 bytes)
    WavelengthSamples wls;             // 波长采样 (24 bytes)
    float initImportance;              // 初始重要性（用于 RR） (4 bytes)
    float selectWLPDF;                 // 波长选择 PDF (4 bytes)
    
    // === 随机数生成器 (16 bytes) ===
    KernelRNG rng;                     // RNG 状态 (PCG32: 16 bytes)
    
    // === 路径历史信息 (16 bytes) ===
    float prevDirPDF;                  // 前一跳的方向 PDF (4 bytes)
    DirectionType prevSampledType;     // 前一跳的采样类型 (4 bytes)
    uint32_t pathLength;               // 当前路径长度 (4 bytes)
    uint32_t _padding2;                // 对齐填充 (4 bytes)
    
    // === 像素坐标 (8 bytes) ===
    uint32_t pixelX;                   // 像素 X 坐标 (4 bytes)
    uint32_t pixelY;                   // 像素 Y 坐标 (4 bytes)
    
    // === 状态标志 (8 bytes) ===
    uint32_t flags;                    // 状态标志位 (4 bytes)
    uint32_t materialCategory;         // 材质类别 (4 bytes)
    
    // === 总大小: 144 bytes ===
    
    // Flags 位定义：
    // bit 0: isActive - 路径是否活跃
    // bit 1: isTerminated - 路径是否终止
    // bit 2: maxLengthReached - 是否达到最大长度
    // bit 3: singleWlSelected - 是否选择了单一波长
    // bit 4: hitEmissive - 是否命中发光表面
    // bit 5-7: 保留
    
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
    
    CUDA_DEVICE_FUNCTION CUDA_INLINE bool singleWlSelected() const {
        return flags & 0x8;
    }
    
    CUDA_DEVICE_FUNCTION CUDA_INLINE void setSingleWlSelected() {
        flags |= 0x8;
    }
    
    CUDA_DEVICE_FUNCTION CUDA_INLINE bool hitEmissive() const {
        return flags & 0x10;
    }
    
    CUDA_DEVICE_FUNCTION CUDA_INLINE void setHitEmissive() {
        flags |= 0x10;
    }
};

static_assert(sizeof(WavefrontPathState) == 144, "PathState size must be 144 bytes");


/// 命中信息：存储光线求交的结果
struct alignas(16) WavefrontHitInfo {
    // === 命中几何信息 (16 bytes) ===
    uint32_t instIndex;                // 实例索引 (4 bytes)
    uint32_t geomInstIndex;            // 几何实例索引 (4 bytes)
    uint32_t primIndex;                // 图元索引 (4 bytes)
    uint32_t hitFlags;                 // 命中标志位 (4 bytes)
    
    // === 参数化坐标 (16 bytes) ===
    float u, v;                        // 重心坐标或参数化坐标 (8 bytes)
    float t;                           // 光线参数 t (4 bytes)
    float _padding;                    // 对齐填充 (4 bytes)
    
    // === 总大小: 32 bytes ===
    
    // HitFlags 位定义：
    // bit 0: hasHit - 是否命中任何几何体
    // bit 1: hitInfinity - 是否命中无限远（环境光）
    // bit 2: hitEmissive - 是否命中发光表面
    // bit 3: hitTransmissive - 是否命中透射表面
    // bit 4-7: 保留
    
    CUDA_DEVICE_FUNCTION CUDA_INLINE bool hasHit() const {
        return hitFlags & 0x1;
    }
    
    CUDA_DEVICE_FUNCTION CUDA_INLINE bool hitInfinity() const {
        return hitFlags & 0x2;
    }
    
    CUDA_DEVICE_FUNCTION CUDA_INLINE bool hitEmissive() const {
        return hitFlags & 0x4;
    }
    
    CUDA_DEVICE_FUNCTION CUDA_INLINE bool hitTransmissive() const {
        return hitFlags & 0x8;
    }
    
    CUDA_DEVICE_FUNCTION CUDA_INLINE void setHasHit(bool hit) {
        if (hit) hitFlags |= 0x1;
        else hitFlags &= ~0x1;
    }
    
    CUDA_DEVICE_FUNCTION CUDA_INLINE void setHitInfinity(bool inf) {
        if (inf) hitFlags |= 0x2;
        else hitFlags &= ~0x2;
    }
    
    CUDA_DEVICE_FUNCTION CUDA_INLINE void setHitEmissive(bool emissive) {
        if (emissive) hitFlags |= 0x4;
        else hitFlags &= ~0x4;
    }
    
    CUDA_DEVICE_FUNCTION CUDA_INLINE void reset() {
        instIndex = 0xFFFFFFFF;
        geomInstIndex = 0xFFFFFFFF;
        primIndex = 0xFFFFFFFF;
        hitFlags = 0;
        u = v = t = 0.0f;
    }
};

static_assert(sizeof(WavefrontHitInfo) == 32, "HitInfo size must be 32 bytes");


// ============================================================================
// 2. 工作队列管理
// ============================================================================

/// 工作队列：管理活跃路径的索引
struct WavefrontWorkQueue {
    uint32_t* pathIndices;             // 路径索引数组（GPU 内存）
    uint32_t* counter;                 // 原子计数器（GPU 内存）
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
        return 0xFFFFFFFF; // 队列满
    }
    
    CUDA_DEVICE_FUNCTION CUDA_INLINE uint32_t dequeue() {
        uint32_t slot = atomicSub(counter, 1);
        if (slot > 0 && slot <= capacity) {
            return pathIndices[slot - 1];
        }
        return 0xFFFFFFFF; // 队列空
    }
    
    CUDA_DEVICE_FUNCTION CUDA_INLINE void reset() {
        *counter = 0;
    }
};


/// 材质分类：用于路径排序和分组
enum MaterialCategory : uint32_t {
    MaterialCategory_Diffuse = 0,      // 漫反射材质（Lambert）
    MaterialCategory_Glossy,           // 光滑反射材质（GGX）
    MaterialCategory_Specular,         // 理想镜面反射
    MaterialCategory_Transmissive,     // 透射材质（玻璃等）
    MaterialCategory_Emissive,         // 发光材质
    MaterialCategory_Mixed,            // 混合材质
    NumMaterialCategories
};


/// 材质队列集合：按材质类型分类的工作队列
struct WavefrontMaterialQueues {
    WavefrontWorkQueue queues[NumMaterialCategories];
    
    CUDA_DEVICE_FUNCTION CUDA_INLINE void enqueueByCategory(
        uint32_t pathIndex, MaterialCategory category) {
        queues[category].enqueue(pathIndex);
    }
    
    CUDA_DEVICE_FUNCTION CUDA_INLINE void resetAll() {
        for (int i = 0; i < NumMaterialCategories; ++i) {
            queues[i].reset();
        }
    }
};


// ============================================================================
// 3. Launch Parameters
// ============================================================================

/// Wavefront 渲染的启动参数
/// 这个结构体会被上传到 GPU 常量内存
struct WavefrontLaunchParameters {
    // === 继承自 PipelineLaunchParameters 的公共数据 ===
    // 注意：实际实现中可能需要继承或包含 PipelineLaunchParameters
    
    // 光谱上采样数据
    DiscretizedSpectrumAlwaysSpectral::CMF DiscretizedSpectrum_xbar;
    DiscretizedSpectrumAlwaysSpectral::CMF DiscretizedSpectrum_ybar;
    DiscretizedSpectrumAlwaysSpectral::CMF DiscretizedSpectrum_zbar;
    float DiscretizedSpectrum_integralCMF;
    
    // 材质和节点数据
    const NodeProcedureSet* nodeProcedureSetBuffer;
    const SmallNodeDescriptor* smallNodeDescriptorBuffer;
    const MediumNodeDescriptor* mediumNodeDescriptorBuffer;
    const LargeNodeDescriptor* largeNodeDescriptorBuffer;
    const BSDFProcedureSet* bsdfProcedureSetBuffer;
    const EDFProcedureSet* edfProcedureSetBuffer;
    const IDFProcedureSet* idfProcedureSetBuffer;
    const SurfaceMaterialDescriptor* materialDescriptorBuffer;
    
    // 场景数据
    const GeometryInstance* geomInstBuffer;
    const Instance* instBuffer;
    OptixTraversableHandle topGroup;
    const SceneBounds* sceneBounds;
    const uint32_t* instIndices;
    DiscreteDistribution1D lightInstDist;
    uint32_t envLightInstIndex;
    
    // 相机数据
    int32_t progSampleLensPosition;
    int32_t progTestLensIntersection;
    CameraDescriptor cameraDescriptor;
    
    // === Wavefront 特定数据 ===
    
    // 路径状态缓冲区
    WavefrontPathState* pathStateBuffer;
    WavefrontHitInfo* hitInfoBuffer;
    SurfacePoint* surfacePointBuffer;
    
    // 工作队列
    WavefrontWorkQueue activePathQueue;      // 当前活跃路径队列
    WavefrontWorkQueue nextActivePathQueue;  // 下一轮活跃路径队列
    
    // 材质分类队列（可选优化）
    WavefrontMaterialQueues materialQueues;
    
    // 输出缓冲区
    optixu::NativeBlockBuffer2D<KernelRNG> rngBuffer;
    optixu::BlockBuffer2D<SpectrumStorage, 0> accumBuffer;
    DiscretizedSpectrum* accumAlbedoBuffer;  // Denoiser Albedo
    Normal3D* accumNormalBuffer;             // Denoiser Normal
    
    // 图像参数
    uint2 imageSize;                   // 图像尺寸
    uint32_t imageStrideInPixels;      // 图像步长
    uint32_t numAccumFrames;           // 累积帧数
    uint32_t limitNumAccumFrames;      // 最大累积帧数
    
    // Wavefront 配置
    uint32_t maxPathLength;            // 最大路径长度（默认 25）
    uint32_t maxNumPaths;              // 最大路径数量
    uint32_t currentDepth;             // 当前处理的路径深度
    
    // 性能统计（可选）
    uint32_t* numActiveRays;           // 当前活跃光线数
    uint32_t* numShadowRays;           // 阴影光线数
    uint32_t* numTerminatedPaths;      // 终止路径数
    
    // 调试参数
    int32_t probePixX;                 // 探测像素 X
    int32_t probePixY;                 // 探测像素 Y
    uint32_t debugMode;                // 调试模式
    
    CUDA_DEVICE_FUNCTION CUDA_INLINE void print() const {
        vlrprintf("=== Wavefront Launch Parameters ===\n");
        vlrprintf("Image Size: %ux%u\n", imageSize.x, imageSize.y);
        vlrprintf("Max Path Length: %u\n", maxPathLength);
        vlrprintf("Max Num Paths: %u\n", maxNumPaths);
        vlrprintf("Current Depth: %u\n", currentDepth);
        vlrprintf("Num Accum Frames: %u\n", numAccumFrames);
        vlrprintf("Active Path Queue Size: %u\n", activePathQueue.size());
    }
};


// ============================================================================
// 4. Payload 定义
// ============================================================================

/// Wavefront 光线追踪的 Payload
/// 设计原则：最小化 Payload 大小，只传递必要信息
struct WFTracePayload {
    uint32_t pathIndex;                // 路径索引 (4 bytes)
    WavelengthSamples wls;             // 波长采样 (24 bytes)
    // 总计: 28 bytes (7 dwords)
};

using WFTracePayloadSignature = optixu::PayloadSignature<WFTracePayload>;


/// 阴影光线 Payload（复用现有的 ShadowPayloadSignature）
// using ShadowPayloadSignature = optixu::PayloadSignature<WavelengthSamples, float>;


// ============================================================================
// 5. 光线类型
// ============================================================================

enum WFRayType {
    WFRayType_Closest = 0,             // 最近命中光线
    WFRayType_Shadow,                  // 阴影光线
    NumWFRayTypes
};


// ============================================================================
// 6. 辅助数据结构
// ============================================================================

/// 材质评估结果：缓存 BSDF/EDF 评估结果
struct WavefrontMaterialEvaluation {
    SampledSpectrum baseColor;         // 基础颜色
    DirectionType bsdfType;            // BSDF 类型
    bool hasNonDelta;                  // 是否有非 delta 分量
    bool hasEmission;                  // 是否发光
    MaterialCategory category;         // 材质类别
};


/// 光源采样结果：缓存光源采样信息
struct WavefrontLightSample {
    SurfacePoint lightSurfPt;          // 光源表面点
    SampledSpectrum Le;                // 光源辐射
    float lightPDF;                    // 光源采样 PDF
    float bsdfPDF;                     // BSDF PDF
    float MISWeight;                   // MIS 权重
    bool isVisible;                    // 是否可见
};


/// BSDF 采样结果：缓存 BSDF 采样信息
struct WavefrontBSDFSample {
    Vector3D dirLocal;                 // 采样方向（局部坐标）
    SampledSpectrum f;                 // BSDF 值
    float pdf;                         // PDF
    DirectionType sampledType;         // 采样类型
    bool isValid;                      // 采样是否有效
};


// ============================================================================
// 7. 性能统计
// ============================================================================

/// Wavefront 性能统计
struct WavefrontPerformanceStats {
    // 路径统计
    uint32_t numInitialPaths;
    uint32_t numActivePathsPerDepth[32];  // 每个深度的活跃路径数
    uint32_t numTerminatedPathsPerDepth[32];
    
    // 光线统计
    uint32_t numPrimaryRays;
    uint32_t numSecondaryRays;
    uint32_t numShadowRays;
    uint32_t totalRaysCast;
    
    // 材质统计
    uint32_t numDiffuseInteractions;
    uint32_t numGlossyInteractions;
    uint32_t numSpecularInteractions;
    uint32_t numTransmissiveInteractions;
    
    // 时间统计（毫秒）
    float timeGenerateRays;
    float timeTraceRays;
    float timeProcessHits;
    float timeSampleLights;
    float timeSampleBSDF;
    float timeCompactPaths;
    float timeAccumulate;
    float totalTime;
    
    // 内存统计
    size_t memoryUsedBytes;
    size_t peakMemoryUsedBytes;
    
    void reset() {
        memset(this, 0, sizeof(WavefrontPerformanceStats));
    }
    
    void print() const {
        printf("=== Wavefront Performance Stats ===\n");
        printf("Initial Paths: %u\n", numInitialPaths);
        printf("Total Rays Cast: %u\n", totalRaysCast);
        printf("  Primary: %u\n", numPrimaryRays);
        printf("  Secondary: %u\n", numSecondaryRays);
        printf("  Shadow: %u\n", numShadowRays);
        printf("\nMaterial Interactions:\n");
        printf("  Diffuse: %u\n", numDiffuseInteractions);
        printf("  Glossy: %u\n", numGlossyInteractions);
        printf("  Specular: %u\n", numSpecularInteractions);
        printf("  Transmissive: %u\n", numTransmissiveInteractions);
        printf("\nTiming (ms):\n");
        printf("  Generate Rays: %.2f\n", timeGenerateRays);
        printf("  Trace Rays: %.2f\n", timeTraceRays);
        printf("  Process Hits: %.2f\n", timeProcessHits);
        printf("  Sample Lights: %.2f\n", timeSampleLights);
        printf("  Sample BSDF: %.2f\n", timeSampleBSDF);
        printf("  Compact Paths: %.2f\n", timeCompactPaths);
        printf("  Accumulate: %.2f\n", timeAccumulate);
        printf("  Total: %.2f\n", totalTime);
        printf("\nMemory:\n");
        printf("  Used: %.2f MB\n", memoryUsedBytes / (1024.0f * 1024.0f));
        printf("  Peak: %.2f MB\n", peakMemoryUsedBytes / (1024.0f * 1024.0f));
    }
};


// ============================================================================
// 8. 调试数据结构
// ============================================================================

/// Wavefront 调试模式
enum WavefrontDebugMode : uint32_t {
    WFDebug_None = 0,
    WFDebug_PathLength,                // 可视化路径长度
    WFDebug_MaterialCategory,          // 可视化材质分类
    WFDebug_Throughput,                // 可视化路径吞吐量
    WFDebug_NumBounces,                // 可视化弹射次数
    WFDebug_ActivePaths,               // 可视化活跃路径分布
    WFDebug_RayDensity,                // 可视化光线密度
    WFDebug_TerminationReason,         // 可视化终止原因
};


/// 路径终止原因
enum PathTerminationReason : uint32_t {
    TerminationReason_None = 0,
    TerminationReason_MaxLength,       // 达到最大长度
    TerminationReason_RussianRoulette, // 俄罗斯轮盘赌
    TerminationReason_ZeroThroughput,  // 吞吐量为零
    TerminationReason_Absorption,      // 被吸收
    TerminationReason_EscapeScene,     // 逃离场景
};


// ============================================================================
// 9. 辅助函数
// ============================================================================

#if defined(VLR_Device) || defined(OPTIXU_Platform_CodeCompletion)

/// 根据 BSDF 特性分类材质
CUDA_DEVICE_FUNCTION CUDA_INLINE MaterialCategory classifyMaterial(
    const BSDF<TransportMode::Radiance>& bsdf) {
    
    // 检查是否为理想镜面反射
    if (bsdf.matches(DirectionType::Delta0D() | DirectionType::Reflection()))
        return MaterialCategory_Specular;
    
    // 检查是否为透射材质
    if (bsdf.matches(DirectionType::Transmission()))
        return MaterialCategory_Transmissive;
    
    // 检查是否为光滑反射（高频）
    if (bsdf.matches(DirectionType::HighFreq() | DirectionType::Reflection()))
        return MaterialCategory_Glossy;
    
    // 检查是否为混合材质
    if (bsdf.matches(DirectionType::Reflection()) && 
        bsdf.matches(DirectionType::Transmission()))
        return MaterialCategory_Mixed;
    
    // 默认为漫反射
    return MaterialCategory_Diffuse;
}


/// 计算 MIS 权重（Power Heuristic, beta=2）
CUDA_DEVICE_FUNCTION CUDA_INLINE float computeMISWeight(
    float pdf1, float pdf2) {
    
    if (isinf(pdf1) || isinf(pdf2))
        return 1.0f;
    
    float pdf1Sq = pdf1 * pdf1;
    float pdf2Sq = pdf2 * pdf2;
    return pdf1Sq / (pdf1Sq + pdf2Sq);
}


/// 计算几何项 G(x <-> y)
CUDA_DEVICE_FUNCTION CUDA_INLINE float computeGeometryTerm(
    const SurfacePoint& sp1,
    const SurfacePoint& sp2,
    const Vector3D& direction,
    float squaredDistance) {
    
    if (sp1.atInfinity || sp2.atInfinity)
        return 1.0f;
    
    float cos1 = absDot(direction, sp1.geometricNormal);
    float cos2 = absDot(-direction, sp2.geometricNormal);
    
    return (cos1 * cos2) / squaredDistance;
}


/// 检查路径是否应该终止（俄罗斯轮盘赌）
CUDA_DEVICE_FUNCTION CUDA_INLINE bool shouldTerminatePath(
    WavefrontPathState& pathState,
    float rrThreshold = 0.05f) {
    
    // 前几次弹射不使用 RR
    if (pathState.pathLength < 3)
        return false;
    
    // 计算继续概率
    float importance = pathState.throughput.importance(pathState.wls.selectedLambdaIndex());
    float continueProb = min(importance / pathState.initImportance, 1.0f);
    
    // 重要性太低，强制终止
    if (continueProb < rrThreshold)
        return true;
    
    // 俄罗斯轮盘赌
    if (pathState.rng.getFloat0cTo1o() >= continueProb)
        return true;
    
    // 路径继续，调整吞吐量
    pathState.throughput /= continueProb;
    return false;
}


/// 计算表面点信息（从 HitInfo）
CUDA_DEVICE_FUNCTION CUDA_INLINE void computeSurfacePoint(
    const WavefrontHitInfo& hitInfo,
    const WavelengthSamples& wls,
    SurfacePoint* surfPt,
    float* hypAreaPDF) {
    
    const GeometryInstance& geomInst = wlp.geomInstBuffer[hitInfo.geomInstIndex];
    
    // 调用几何解码程序
    ProgSigDecodeHitPoint decodeHitPoint(geomInst.progDecodeHitPoint);
    decodeHitPoint(
        hitInfo.instIndex,
        hitInfo.geomInstIndex,
        hitInfo.primIndex,
        hitInfo.u, hitInfo.v,
        surfPt);
    
    // 应用法线贴图
    Normal3D localNormal = calcNode(geomInst.nodeNormal, Normal3D(0.0f, 0.0f, 1.0f), *surfPt, wls);
    applyBumpMapping(localNormal, surfPt);
    
    // 应用切线修改
    Vector3D newTangent = calcNode(geomInst.nodeTangent, surfPt->shadingFrame.x, *surfPt, wls);
    modifyTangent(newTangent, surfPt);
    
    // 计算面积 PDF（用于光源采样）
    if (geomInst.geomType == GeometryType_TriangleMesh) {
        const Triangle& tri = geomInst.asTriMesh.triangleBuffer[hitInfo.primIndex];
        *hypAreaPDF = 1.0f / tri.area;
    } else {
        *hypAreaPDF = 1.0f;
    }
}


/// 处理环境光命中
CUDA_DEVICE_FUNCTION CUDA_INLINE void processEnvironmentHit(
    WavefrontPathState& pathState,
    const WavefrontHitInfo& hitInfo) {
    
    const Instance& inst = wlp.instBuffer[wlp.envLightInstIndex];
    const GeometryInstance& geomInst = wlp.geomInstBuffer[inst.geomInstIndices[0]];
    
    if (geomInst.importance == 0)
        return;
    
    // 计算环境光方向
    Vector3D direction = pathState.direction;
    float phi, theta;
    direction.toPolarYUp(&theta, &phi);
    
    // 构造表面点
    SurfacePoint surfPt;
    surfPt.position = Point3D(direction.x, direction.y, direction.z);
    surfPt.atInfinity = true;
    surfPt.geometricNormal = -direction;
    
    float sinPhi, cosPhi;
    sincos(phi, &sinPhi, &cosPhi);
    Vector3D texCoord0Dir = normalize(Vector3D(-cosPhi, 0.0f, -sinPhi));
    surfPt.shadingFrame = ReferenceFrame(texCoord0Dir, -direction);
    
    phi += inst.rotationPhi;
    phi = phi - floor(phi / (2 * VLR_M_PI)) * 2 * VLR_M_PI;
    surfPt.texCoord = TexCoord2D(phi / (2 * VLR_M_PI), theta / VLR_M_PI);
    
    // 评估环境光
    const SurfaceMaterialDescriptor& matDesc = wlp.materialDescriptorBuffer[geomInst.materialIndex];
    EDF edf(matDesc, surfPt, pathState.wls);
    
    Vector3D dirOutLocal = surfPt.shadingFrame.toLocal(-direction);
    SampledSpectrum spEmittance = edf.evaluateEmittance();
    
    if (spEmittance.hasNonZero()) {
        EDFQuery feQuery(DirectionType::All(), pathState.wls);
        SampledSpectrum Le = spEmittance * edf.evaluate(feQuery, dirOutLocal);
        
        // MIS 权重计算
        float MISWeight = 1.0f;
        if (!pathState.prevSampledType.isDelta() && pathState.pathLength > 1) {
            float uvPDF = geomInst.asInfSphere.importanceMap.evaluatePDF(
                phi / (2 * VLR_M_PI), theta / VLR_M_PI);
            float hypAreaPDF = uvPDF / (2 * VLR_M_PI * VLR_M_PI * sin(theta));
            
            float instProb = inst.lightGeomInstDistribution.integral() / wlp.lightInstDist.integral();
            float geomInstProb = geomInst.importance / inst.lightGeomInstDistribution.integral();
            
            float bsdfPDF = pathState.prevDirPDF;
            float lightPDF = instProb * geomInstProb * hypAreaPDF / abs(dirOutLocal.z);
            
            MISWeight = computeMISWeight(bsdfPDF, lightPDF);
        }
        
        // 累积贡献
        pathState.contribution += pathState.throughput * Le * MISWeight;
    }
}

#endif // VLR_Device


// ============================================================================
// 10. 配置常量
// ============================================================================

namespace WavefrontConfig {
    // 路径配置
    constexpr uint32_t DefaultMaxPathLength = 25;
    constexpr uint32_t MinPathLength = 1;
    constexpr uint32_t MaxPathLength = 64;
    
    // 俄罗斯轮盘赌配置
    constexpr uint32_t RRStartDepth = 3;      // 开始使用 RR 的深度
    constexpr float RRThreshold = 0.05f;      // RR 阈值
    
    // 队列配置
    constexpr uint32_t DefaultQueueCapacity = 1920 * 1080;
    constexpr uint32_t MaxQueueCapacity = 3840 * 2160;
    
    // 性能配置
    constexpr uint32_t BlockSize = 256;       // CUDA block 大小
    constexpr uint32_t WarpSize = 32;         // Warp 大小
    
    // 内存配置
    constexpr bool UsePathSorting = true;     // 是否使用路径排序
    constexpr bool UseMaterialQueues = true;  // 是否使用材质队列
    constexpr bool UseStreamCompaction = true; // 是否使用 Stream Compaction
    
    // 调试配置
    constexpr bool EnablePerfStats = true;    // 启用性能统计
    constexpr bool EnableValidation = false;  // 启用验证检查
}


// ============================================================================
// 11. 内存布局优化（可选 SoA 版本）
// ============================================================================

/// Structure of Arrays 版本的 PathState（用于内存优化）
struct WavefrontPathStateBuffers_SoA {
    // 光线信息
    Point3D* origins;
    Vector3D* directions;
    
    // 光谱信息
    SampledSpectrum* throughputs;
    SampledSpectrum* contributions;
    WavelengthSamples* wavelengthSamples;
    float* initImportances;
    
    // RNG
    KernelRNG* rngs;
    
    // 路径历史
    float* prevDirPDFs;
    DirectionType* prevSampledTypes;
    uint32_t* pathLengths;
    
    // 像素坐标
    uint32_t* pixelXs;
    uint32_t* pixelYs;
    
    // 标志
    uint32_t* flags;
    uint32_t* materialCategories;
    
    // 访问接口
    CUDA_DEVICE_FUNCTION CUDA_INLINE void load(
        uint32_t index, WavefrontPathState* state) const {
        state->origin = origins[index];
        state->direction = directions[index];
        state->throughput = throughputs[index];
        state->contribution = contributions[index];
        state->wls = wavelengthSamples[index];
        state->initImportance = initImportances[index];
        state->rng = rngs[index];
        state->prevDirPDF = prevDirPDFs[index];
        state->prevSampledType = prevSampledTypes[index];
        state->pathLength = pathLengths[index];
        state->pixelX = pixelXs[index];
        state->pixelY = pixelYs[index];
        state->flags = flags[index];
        state->materialCategory = materialCategories[index];
    }
    
    CUDA_DEVICE_FUNCTION CUDA_INLINE void store(
        uint32_t index, const WavefrontPathState& state) {
        origins[index] = state.origin;
        directions[index] = state.direction;
        throughputs[index] = state.throughput;
        contributions[index] = state.contribution;
        wavelengthSamples[index] = state.wls;
        initImportances[index] = state.initImportance;
        rngs[index] = state.rng;
        prevDirPDFs[index] = state.prevDirPDF;
        prevSampledTypes[index] = state.prevSampledType;
        pathLengths[index] = state.pathLength;
        pixelXs[index] = state.pixelX;
        pixelYs[index] = state.pixelY;
        flags[index] = state.flags;
        materialCategories[index] = state.materialCategory;
    }
};


// ============================================================================
// 12. Stream Compaction 辅助
// ============================================================================

/// 路径活跃性谓词（用于 Stream Compaction）
struct PathIsActivePredicate {
    WavefrontPathState* pathStates;
    
    CUDA_DEVICE_FUNCTION CUDA_INLINE bool operator()(uint32_t pathIndex) const {
        return pathStates[pathIndex].isActive();
    }
};


/// 材质类别谓词（用于路径排序）
struct MaterialCategoryComparator {
    WavefrontPathState* pathStates;
    
    CUDA_DEVICE_FUNCTION CUDA_INLINE bool operator()(
        uint32_t pathIndex1, uint32_t pathIndex2) const {
        return pathStates[pathIndex1].materialCategory < 
               pathStates[pathIndex2].materialCategory;
    }
};

} // namespace vlr::shared


// ============================================================================
// 13. 使用示例
// ============================================================================

#if 0 // 示例代码，不编译

// === 主机端代码示例 ===

void Context::initializeWavefrontBuffers(uint32_t width, uint32_t height) {
    OptiX::WavefrontPathTracing& wf = m_optix.wavefrontPathTracing;
    
    uint32_t numPixels = width * height;
    
    // 分配缓冲区
    wf.pathStateBuffer.initialize(m_cuContext, cudau::BufferType::Device, numPixels);
    wf.hitInfoBuffer.initialize(m_cuContext, cudau::BufferType::Device, numPixels);
    wf.surfacePointBuffer.initialize(m_cuContext, cudau::BufferType::Device, numPixels);
    
    // 初始化工作队列
    wf.activePathIndices.initialize(m_cuContext, cudau::BufferType::Device, numPixels);
    wf.nextActivePathIndices.initialize(m_cuContext, cudau::BufferType::Device, numPixels);
    wf.queueCounters.initialize(m_cuContext, cudau::BufferType::Device, 2);
    
    // 设置 Launch Parameters
    m_optix.wavefrontLaunchParams.pathStateBuffer = wf.pathStateBuffer.getDevicePointer();
    m_optix.wavefrontLaunchParams.hitInfoBuffer = wf.hitInfoBuffer.getDevicePointer();
    m_optix.wavefrontLaunchParams.surfacePointBuffer = wf.surfacePointBuffer.getDevicePointer();
    
    m_optix.wavefrontLaunchParams.activePathQueue.pathIndices = wf.activePathIndices.getDevicePointer();
    m_optix.wavefrontLaunchParams.activePathQueue.counter = wf.queueCounters.getDevicePointerAt(0);
    m_optix.wavefrontLaunchParams.activePathQueue.capacity = numPixels;
    
    m_optix.wavefrontLaunchParams.nextActivePathQueue.pathIndices = wf.nextActivePathIndices.getDevicePointer();
    m_optix.wavefrontLaunchParams.nextActivePathQueue.counter = wf.queueCounters.getDevicePointerAt(1);
    m_optix.wavefrontLaunchParams.nextActivePathQueue.capacity = numPixels;
    
    m_optix.wavefrontLaunchParams.maxPathLength = WavefrontConfig::DefaultMaxPathLength;
    m_optix.wavefrontLaunchParams.maxNumPaths = numPixels;
}


// === 设备端代码示例 ===

// Kernel: Generate Rays
CUDA_DEVICE_KERNEL void RT_RG_NAME(wavefrontGenerateRays)() {
    uint2 launchIndex = make_uint2(optixGetLaunchIndex().x, optixGetLaunchIndex().y);
    uint32_t pathIndex = launchIndex.y * wlp.imageSize.x + launchIndex.x;
    
    // 初始化路径状态
    WavefrontPathState& pathState = wlp.pathStateBuffer[pathIndex];
    
    // ... 相机采样和光线生成 ...
    
    pathState.setActive(true);
    
    // 加入活跃队列
    wlp.activePathQueue.enqueue(pathIndex);
}


// Kernel: Process Hits
CUDA_DEVICE_KERNEL void wavefrontProcessHits() {
    uint32_t workIndex = blockIdx.x * blockDim.x + threadIdx.x;
    if (workIndex >= wlp.activePathQueue.size())
        return;
    
    uint32_t pathIndex = wlp.activePathQueue.pathIndices[workIndex];
    WavefrontPathState& pathState = wlp.pathStateBuffer[pathIndex];
    
    if (!pathState.isActive())
        return;
    
    const WavefrontHitInfo& hitInfo = wlp.hitInfoBuffer[pathIndex];
    
    if (!hitInfo.hasHit()) {
        pathState.setTerminated();
        return;
    }
    
    // 处理环境光
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
    
    // ... BSDF/EDF 评估和隐式光源采样 ...
    
    // 路径长度检查
    pathState.pathLength++;
    if (pathState.pathLength >= wlp.maxPathLength) {
        pathState.setMaxLengthReached();
        pathState.setTerminated();
        return;
    }
    
    // 俄罗斯轮盘赌
    if (shouldTerminatePath(pathState)) {
        pathState.setTerminated();
        return;
    }
}

#endif // 示例代码


// ============================================================================
// 14. 版本信息
// ============================================================================

namespace WavefrontVersion {
    constexpr uint32_t Major = 1;
    constexpr uint32_t Minor = 0;
    constexpr uint32_t Patch = 0;
    constexpr const char* String = "1.0.0-alpha";
}

} // namespace vlr::shared
