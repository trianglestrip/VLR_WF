// ============================================================================
// VLR 波前路径追踪 - 核心数据结构
// 
// 本文件定义了波前渲染模式的所有核心数据结构。
// 
// 作者：VLR 开发团队
// 创建日期：2026-03-07
// 环境：CUDA 13.1, OptiX 8.0.0, VS2022
// ============================================================================

#pragma once

#include "kernel_common.h"
#include "material_types.h"
#include <cstdio>

namespace vlr {
namespace shared {

// ============================================================================
// 1. 核心数据结构
// ============================================================================

/// 路径状态：存储单条光线路径的完整信息
/// 这是波前架构的核心数据结构
/// 大小：144 字节（针对内存对齐和缓存效率优化）
struct alignas(16) WavefrontPathState {
    // === 光线信息（32 字节）===
    Point3D origin;                    // 光线起点（12 字节）
    Vector3D direction;                // 光线方向（12 字节）
    float _padding1[2];                // 对齐填充（8 字节）
    
    // === 光谱和吞吐量（64 字节）===
    SampledSpectrum throughput;        // 路径吞吐量/权重（16 字节）
    SampledSpectrum contribution;      // 累积辐射贡献（16 字节）
    WavelengthSamples wls;             // 波长采样（24 字节）
    float initImportance;              // 初始重要性（用于俄罗斯轮盘赌）（4 字节）
    float selectWLPDF;                 // 波长选择概率密度（4 字节）
    
    // === 随机数生成器（16 字节）===
    KernelRNG rng;                     // RNG 状态（PCG32：16 字节）
    
    // === 路径历史（16 字节）===
    float prevDirPDF;                  // 前一次反弹方向概率密度（4 字节）
    DirectionType prevSampledType;     // 前一次反弹采样类型（4 字节）
    uint32_t pathLength;               // 当前路径长度（4 字节）
    uint32_t _padding2;                // 对齐填充（4 字节）
    
    // === 像素坐标（8 字节）===
    uint32_t pixelX;                   // 像素 X 坐标（4 字节）
    uint32_t pixelY;                   // 像素 Y 坐标（4 字节）
    
    // === 状态标志（8 字节）===
    uint32_t flags;                    // 状态标志位（4 字节）
    uint32_t materialCategory;         // 材质分类（4 字节）
    
    // === 总大小：144 字节 ===
    
    // 标志位定义：
    // bit 0: isActive - 路径是否活跃
    // bit 1: isTerminated - 路径是否终止
    // bit 2: maxLengthReached - 是否达到最大长度
    // bit 3: singleWlSelected - 是否已选择单一波长
    // bit 4: hitEmissive - 是否击中发光表面
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

static_assert(sizeof(WavefrontPathState) == 144, "PathState 大小必须为 144 字节");


/// 击中信息：存储光线相交结果
/// 大小：32 字节（针对内存带宽优化）
struct alignas(16) WavefrontHitInfo {
    // === 击中几何信息（16 字节）===
    uint32_t instIndex;                // 实例索引（4 字节）
    uint32_t geomInstIndex;            // 几何实例索引（4 字节）
    uint32_t primIndex;                // 图元索引（4 字节）
    uint32_t hitFlags;                 // 击中标志位（4 字节）
    
    // === 参数坐标（16 字节）===
    float u, v;                        // 重心坐标或参数坐标（8 字节）
    float t;                           // 光线参数 t（4 字节）
    float _padding;                    // 对齐填充（4 字节）
    
    // === 总大小：32 字节 ===
    
    // 击中标志位定义：
    // bit 0: hasHit - 是否击中任何几何体
    // bit 1: hitInfinity - 是否击中无穷远（环境）
    // bit 2: hitEmissive - 是否击中发光表面
    // bit 3: hitTransmissive - 是否击中透射表面
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

static_assert(sizeof(WavefrontHitInfo) == 32, "HitInfo 大小必须为 32 字节");


// ============================================================================
// 2. 工作队列管理
// ============================================================================

/// 工作队列：管理活跃路径的索引
/// 使用原子操作实现线程安全的入队/出队
struct WavefrontWorkQueue {
    uint32_t* pathIndices;             // 路径索引数组（GPU 内存）
    uint32_t* counter;                 // 原子计数器（GPU 内存）
    uint32_t capacity;                 // 队列容量
    
    CUDA_DEVICE_FUNCTION CUDA_INLINE uint32_t size() const {
        return *counter;
    }
    
    CUDA_DEVICE_FUNCTION CUDA_INLINE uint32_t enqueue(uint32_t pathIndex) {
#ifdef __CUDACC__
        uint32_t slot = atomicAdd(counter, 1u);
#else
        atomicAdd(counter, 1u);
        uint32_t slot = (*counter) - 1;
#endif
        if (slot < capacity) {
            pathIndices[slot] = pathIndex;
            return slot;
        }
        return 0xFFFFFFFF; // 队列已满
    }
    
    CUDA_DEVICE_FUNCTION CUDA_INLINE uint32_t dequeue() {
        uint32_t slot = atomicSub(counter, 1);
        if (slot > 0 && slot <= capacity) {
            return pathIndices[slot - 1];
        }
        return 0xFFFFFFFF; // 队列为空
    }
    
    CUDA_DEVICE_FUNCTION CUDA_INLINE void reset() {
        *counter = 0;
    }
};


/// 材质队列集：按材质类型分类的工作队列
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
// 3. 启动参数
// ============================================================================

/// 波前渲染启动参数
/// 此结构体上传到 GPU 常量内存
struct WavefrontLaunchParameters {
    // === 从 PipelineLaunchParameters 继承的公共数据 ===
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
    // 顶点数据（用于三角形网格 decodeHitPoint，可为 nullptr 时几何解码不可用）
    const Point3D* vertexPositions;
    const Normal3D* vertexNormals;
    const TexCoord2D* vertexTexCoords;
    uint64_t topGroup;  // OptixTraversableHandle（OptiX 可用时定义）
    const SceneBounds* sceneBounds;
    const uint32_t* instIndices;
    DiscreteDistribution1D lightInstDist;
    uint32_t envLightInstIndex;
    
    // 相机数据
    int32_t progSampleLensPosition;
    int32_t progTestLensIntersection;
    CameraDescriptor cameraDescriptor;
    
    // === 波前特定数据 ===
    
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
    DiscretizedSpectrum* accumAlbedoBuffer;  // 降噪器反照率
    Normal3D* accumNormalBuffer;             // 降噪器法线
    
    // 图像参数
    uint2 imageSize;                   // 图像尺寸
    uint32_t imageStrideInPixels;      // 图像步长
    uint32_t numAccumFrames;           // 累积帧数
    uint32_t limitNumAccumFrames;      // 最大累积帧数
    
    // 波前配置
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
        printf("=== Wavefront Launch Parameters ===\n");
        printf("Image Size: %ux%u\n", imageSize.x, imageSize.y);
        printf("Max Path Length: %u\n", maxPathLength);
        printf("Max Num Paths: %u\n", maxNumPaths);
        printf("Current Depth: %u\n", currentDepth);
        printf("Num Accum Frames: %u\n", numAccumFrames);
        printf("Active Path Queue Size: %u\n", activePathQueue.size());
    }
};


// ============================================================================
// 4. 载荷定义
// ============================================================================

/// 波前光线追踪载荷
/// 设计原则：最小化载荷大小，仅传递必要信息
struct WFTracePayload {
    uint32_t pathIndex;                // 路径索引（4 字节）
    WavelengthSamples wls;             // 波长采样（24 字节）
    // 总计：28 字节（7 个双字）
};

using WFTracePayloadSignature = optixu::PayloadSignature<WFTracePayload>;


/// 阴影光线载荷（重用现有的 ShadowPayloadSignature）
// using ShadowPayloadSignature = optixu::PayloadSignature<WavelengthSamples, float>;


// ============================================================================
// 5. 光线类型
// ============================================================================

enum WFRayType {
    WFRayType_Closest = 0,             // 最近击中光线
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
    bool hasEmission;                  // 是否有发光
    MaterialCategory category;         // 材质分类
};


/// 光源采样结果：缓存光源采样信息
struct WavefrontLightSample {
    SurfacePoint lightSurfPt;          // 光源表面点
    SampledSpectrum Le;                // 光源发射
    float lightPDF;                    // 光源采样概率密度
    float bsdfPDF;                     // BSDF 概率密度
    float MISWeight;                   // MIS 权重
    bool isVisible;                    // 是否可见
};


/// BSDF 采样结果：缓存 BSDF 采样信息
struct WavefrontBSDFSample {
    Vector3D dirLocal;                 // 采样方向（局部坐标）
    SampledSpectrum f;                 // BSDF 值
    float pdf;                         // 概率密度
    DirectionType sampledType;         // 采样类型
    bool isValid;                      // 采样是否有效
};


// ============================================================================
// 7. 性能统计
// ============================================================================

/// 波前性能统计
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

/// 波前调试模式
enum WavefrontDebugMode : uint32_t {
    WFDebug_None = 0,
    WFDebug_PathLength,                // 可视化路径长度
    WFDebug_MaterialCategory,          // 可视化材质分类
    WFDebug_Throughput,                // 可视化路径吞吐量
    WFDebug_NumBounces,                // 可视化反弹次数
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
// 9. 配置常量
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
    constexpr uint32_t BlockSize = 256;       // CUDA 块大小
    constexpr uint32_t WarpSize = 32;         // Warp 大小
    
    // 内存配置
    constexpr bool UsePathSorting = true;     // 使用路径排序
    constexpr bool UseMaterialQueues = true;  // 使用材质队列
    constexpr bool UseStreamCompaction = true; // 使用流压缩
    
    // 调试配置
    constexpr bool EnablePerfStats = true;    // 启用性能统计
    constexpr bool EnableValidation = false;  // 启用校验检查
}


// ============================================================================
// 10. 内存布局优化（可选 SoA 版本）
// ============================================================================

/// PathState 的结构数组（SoA）版本（用于内存优化）
struct WavefrontPathStateBuffers_SoA {
    // 光线信息
    Point3D* origins;
    Vector3D* directions;
    
    // 光谱信息
    SampledSpectrum* throughputs;
    SampledSpectrum* contributions;
    WavelengthSamples* wavelengthSamples;
    float* initImportances;
    
    // 随机数生成器
    KernelRNG* rngs;
    
    // 路径历史
    float* prevDirPDFs;
    DirectionType* prevSampledTypes;
    uint32_t* pathLengths;
    
    // 像素坐标
    uint32_t* pixelXs;
    uint32_t* pixelYs;
    
    // 标志位
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
// 11. 流压缩辅助
// ============================================================================

/// 路径活跃谓词（用于流压缩）
struct PathIsActivePredicate {
    WavefrontPathState* pathStates;
    
    CUDA_DEVICE_FUNCTION CUDA_INLINE bool operator()(uint32_t pathIndex) const {
        return pathStates[pathIndex].isActive();
    }
};


/// 材质分类比较器（用于路径排序）
struct MaterialCategoryComparator {
    WavefrontPathState* pathStates;
    
    CUDA_DEVICE_FUNCTION CUDA_INLINE bool operator()(
        uint32_t pathIndex1, uint32_t pathIndex2) const {
        return pathStates[pathIndex1].materialCategory < 
               pathStates[pathIndex2].materialCategory;
    }
};


// ============================================================================
// 12. 版本信息
// ============================================================================

namespace WavefrontVersion {
    constexpr uint32_t Major = 1;
    constexpr uint32_t Minor = 0;
    constexpr uint32_t Patch = 0;
    constexpr const char* String = "1.0.0-alpha";
}

} // namespace shared
} // namespace vlr
