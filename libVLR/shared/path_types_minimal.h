// ============================================================================
// VLR Wavefront TraceRays - 最小化类型定义
//
// 本文件仅包含 TraceRays OptiX 程序所需的最小类型集合。
// 用于 PTX 编译，避免 material_types、texture_types 等重型依赖。
//
// 作者：VLR 开发团队
// 创建日期：2026-03-07
// ============================================================================

#pragma once

#include "vlr/basic_types.h"
#include <cstdint>

namespace vlr {
namespace shared {

// ============================================================================
// 环境光重要性贴图（完整定义，与 env_importance.h 保持布局一致）
// ============================================================================
#ifndef VLR_ENVIRONMENT_IMPORTANCE_MAP_DEFINED
#define VLR_ENVIRONMENT_IMPORTANCE_MAP_DEFINED
struct EnvironmentImportanceMap {
    const float* cdfTheta;
    const float* cdfPhi;
    uint32_t thetaRes;
    uint32_t phiRes;
    float totalLuminance;

    CUDA_DEVICE_FUNCTION CUDA_INLINE bool isValid() const {
        return cdfTheta != nullptr && cdfPhi != nullptr &&
               thetaRes > 0 && phiRes > 0 && totalLuminance > 1e-10f;
    }

    CUDA_DEVICE_FUNCTION CUDA_INLINE void sample(
        float u0, float u1,
        float* outTheta, float* outPhi,
        float* outPDF) const {

        if (!isValid()) {
            *outTheta = u0 * VLR_M_PI;
            *outPhi = u1 * VLR_M_2PI;
            *outPDF = 1.0f / (VLR_M_2PI * VLR_M_PI);
            return;
        }

        float uTheta = u0 * cdfTheta[thetaRes];
        uint32_t thetaIdx = 0;
        for (uint32_t i = 1; i <= thetaRes; ++i) {
            if (uTheta < cdfTheta[i]) {
                thetaIdx = i - 1;
                break;
            }
            thetaIdx = i - 1;
        }
        if (thetaIdx >= thetaRes) thetaIdx = thetaRes - 1;

        const float* rowCdf = cdfPhi + thetaIdx * (phiRes + 1);
        float rowSum = rowCdf[phiRes];
        float uPhi = (rowSum > 1e-10f) ? (u1 * rowSum) : 0.0f;
        uint32_t phiIdx = 0;
        for (uint32_t i = 1; i <= phiRes; ++i) {
            if (uPhi < rowCdf[i]) {
                phiIdx = i - 1;
                break;
            }
            phiIdx = i - 1;
        }
        if (phiIdx >= phiRes) phiIdx = phiRes - 1;

        float dTheta = VLR_M_PI / thetaRes;
        float dPhi = VLR_M_2PI / phiRes;
        float fracTheta = (cdfTheta[thetaIdx + 1] > cdfTheta[thetaIdx])
            ? (uTheta - cdfTheta[thetaIdx]) / (cdfTheta[thetaIdx + 1] - cdfTheta[thetaIdx])
            : 0.5f;
        float fracPhi = (rowSum > 1e-10f && rowCdf[phiIdx + 1] > rowCdf[phiIdx])
            ? (uPhi - rowCdf[phiIdx]) / (rowCdf[phiIdx + 1] - rowCdf[phiIdx])
            : 0.5f;
        fracTheta = (fracTheta < 0.0f) ? 0.0f : ((fracTheta > 1.0f) ? 1.0f : fracTheta);
        fracPhi = (fracPhi < 0.0f) ? 0.0f : ((fracPhi > 1.0f) ? 1.0f : fracPhi);

        *outTheta = (thetaIdx + fracTheta) * dTheta;
        *outPhi = (phiIdx + fracPhi) * dPhi;

        float sinTheta = sinf(*outTheta);
        float sinThetaSafe = (sinTheta > 1e-6f) ? sinTheta : 1e-6f;
        float cellSolidAngle = sinThetaSafe * dTheta * dPhi;
        float marginalProb = (cdfTheta[thetaRes] > 1e-10f)
            ? (cdfTheta[thetaIdx + 1] - cdfTheta[thetaIdx]) / cdfTheta[thetaRes]
            : 1.0f / thetaRes;
        float condProb = (rowSum > 1e-10f)
            ? (rowCdf[phiIdx + 1] - rowCdf[phiIdx]) / rowSum
            : 1.0f / phiRes;
        float cellProb = marginalProb * condProb;
        *outPDF = (cellSolidAngle > 1e-12f)
            ? (cellProb / cellSolidAngle)
            : (1.0f / (VLR_M_2PI * VLR_M_PI * sinThetaSafe));
    }

    CUDA_DEVICE_FUNCTION CUDA_INLINE float evaluatePDF(float u, float v) const {
        if (!isValid())
            return 1.0f / (VLR_M_2PI * VLR_M_PI);

        float theta = v * VLR_M_PI;
        float sinTheta = sinf(theta);
        float sinThetaSafe = (sinTheta > 1e-6f) ? sinTheta : 1e-6f;

        uint32_t thetaIdx = static_cast<uint32_t>(v * thetaRes);
        uint32_t phiIdx = static_cast<uint32_t>(u * phiRes);
        if (thetaIdx >= thetaRes) thetaIdx = thetaRes - 1;
        if (phiIdx >= phiRes) phiIdx = phiRes - 1;

        const float* rowCdf = cdfPhi + thetaIdx * (phiRes + 1);
        float rowSum = rowCdf[phiRes];
        float dTheta = VLR_M_PI / thetaRes;
        float dPhi = VLR_M_2PI / phiRes;
        float cellSolidAngle = sinThetaSafe * dTheta * dPhi;

        float marginalProb = (cdfTheta[thetaRes] > 1e-10f)
            ? (cdfTheta[thetaIdx + 1] - cdfTheta[thetaIdx]) / cdfTheta[thetaRes]
            : 1.0f / thetaRes;
        float condProb = (rowSum > 1e-10f)
            ? (rowCdf[phiIdx + 1] - rowCdf[phiIdx]) / rowSum
            : 1.0f / phiRes;
        float cellProb = marginalProb * condProb;

        if (cellSolidAngle < 1e-12f)
            return 1.0f / (VLR_M_2PI * VLR_M_PI * sinThetaSafe);
        return cellProb / cellSolidAngle;
    }
};
#endif

using ::vlr::Point3D;
using ::vlr::Vector3D;
using ::vlr::Normal3D;
using ::vlr::TexCoord2D;
using ::vlr::ReferenceFrame;
using ::vlr::SampledSpectrum;
using ::vlr::DiscretizedSpectrum;
using ::vlr::WavelengthSamples;
using ::vlr::KernelRNG;
using ::vlr::DirectionType;
using ::vlr::SurfacePoint;
using ::vlr::CameraDescriptor;
using ::vlr::NodeProcedureSet;
using ::vlr::SmallNodeDescriptor;
using ::vlr::MediumNodeDescriptor;
using ::vlr::LargeNodeDescriptor;
using ::vlr::BSDFProcedureSet;
using ::vlr::EDFProcedureSet;
using ::vlr::IDFProcedureSet;
using ::vlr::SurfaceMaterialDescriptor;
using ::vlr::GeometryInstance;
using ::vlr::Instance;
using ::vlr::DiscreteDistribution1D;
using ::vlr::SceneBounds;
using ::vlr::SpectrumStorage;
using ::vlr::NativeBlockBuffer2D;
using ::vlr::BlockBuffer2D;

// 前向声明，避免包含 texture_types.h
struct Texture2DDescriptor;
struct PathTexturedMaterialParams;
struct MaterialTextureParams;

// 材质类别数量（与 kernel_common.h 保持一致，用于 WavefrontMaterialQueues 布局）
#ifndef VLR_NUM_MATERIAL_CATEGORIES_DEFINED
#define VLR_NUM_MATERIAL_CATEGORIES_DEFINED
constexpr uint32_t NumMaterialCategories = 6;
#endif

// ============================================================================
// 1. WavefrontPathState（TraceRays 仅需 origin、direction、wls、isActive）
// 布局必须与 path_types.h 完全一致（144 字节），因 pathStateBuffer 共享
// ============================================================================
#ifndef VLR_WAVEFRONT_PATH_STATE_MINIMAL_DEFINED
#define VLR_WAVEFRONT_PATH_STATE_MINIMAL_DEFINED
struct alignas(16) WavefrontPathState {
    Point3D origin;
    Vector3D direction;
    float _padding1[2];

    SampledSpectrum throughput;
    SampledSpectrum contribution;
    WavelengthSamples wls;
    float initImportance;
    float selectWLPDF;

    KernelRNG rng;
    float prevDirPDF;
    DirectionType prevSampledType;
    uint32_t pathLength;
    uint32_t _padding2;

    uint32_t pixelX;
    uint32_t pixelY;
    uint32_t flags;
    uint32_t materialCategory;

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
    CUDA_DEVICE_FUNCTION CUDA_INLINE void reset() {
        flags = 0;
        pathLength = 0;
        materialCategory = 0;
    }
};

static_assert(sizeof(WavefrontPathState) == 144, "PathState layout must match path_types.h");
#endif

// ============================================================================
// 2. WavefrontHitInfo
// ============================================================================
#ifndef VLR_WAVEFRONT_HIT_INFO_MINIMAL_DEFINED
#define VLR_WAVEFRONT_HIT_INFO_MINIMAL_DEFINED
struct alignas(16) WavefrontHitInfo {
    uint32_t instIndex;
    uint32_t geomInstIndex;
    uint32_t primIndex;
    uint32_t hitFlags;
    float u, v;
    float t;
    float _padding;

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

static_assert(sizeof(WavefrontHitInfo) == 32, "HitInfo layout must match path_types.h");
#endif

// ============================================================================
// 3. WavefrontWorkQueue
// ============================================================================
#ifndef VLR_WAVEFRONT_WORK_QUEUE_MINIMAL_DEFINED
#define VLR_WAVEFRONT_WORK_QUEUE_MINIMAL_DEFINED
struct WavefrontWorkQueue {
    uint32_t* pathIndices;
    uint32_t* counter;
    uint32_t capacity;

    CUDA_DEVICE_FUNCTION CUDA_INLINE uint32_t size() {
        return *counter;
    }
    CUDA_DEVICE_FUNCTION CUDA_INLINE uint32_t enqueue(uint32_t pathIndex) {
        uint32_t slot = atomicAdd(counter, 1u);
        if (slot < capacity) {
            pathIndices[slot] = pathIndex;
            return slot;
        }
        return 0xFFFFFFFF;
    }
    CUDA_DEVICE_FUNCTION CUDA_INLINE uint32_t dequeue() {
#ifdef __CUDACC__
        uint32_t slot = atomicAdd(counter, 0xFFFFFFFFu);
#else
        uint32_t oldVal = *counter;
        *counter = (oldVal > 0) ? (oldVal - 1) : 0;
        uint32_t slot = oldVal;
#endif
        if (slot > 0 && slot <= capacity) {
            return pathIndices[slot - 1];
        }
        return 0xFFFFFFFF;
    }
    CUDA_DEVICE_FUNCTION CUDA_INLINE void reset() {
        *counter = 0;
    }
};
#endif

// ============================================================================
// 4. WavefrontMaterialQueues（布局匹配，用于 WavefrontLaunchParameters）
// ============================================================================
#ifndef VLR_WAVEFRONT_MATERIAL_QUEUES_MINIMAL_DEFINED
#define VLR_WAVEFRONT_MATERIAL_QUEUES_MINIMAL_DEFINED
struct WavefrontMaterialQueues {
    WavefrontWorkQueue queues[NumMaterialCategories];

    CUDA_DEVICE_FUNCTION CUDA_INLINE void enqueueByCategory(
        uint32_t pathIndex, uint32_t category) {
        queues[category].enqueue(pathIndex);
    }
    
    CUDA_DEVICE_FUNCTION CUDA_INLINE void resetAll() {
        for (int i = 0; i < NumMaterialCategories; ++i) {
            queues[i].reset();
        }
    }
};
#endif

// ============================================================================
// 5. WFTracePayload
// ============================================================================
#ifndef VLR_WF_TRACE_PAYLOAD_MINIMAL_DEFINED
#define VLR_WF_TRACE_PAYLOAD_MINIMAL_DEFINED
struct WFTracePayload {
    uint32_t pathIndex;
    WavelengthSamples wls;
};
#endif

// ============================================================================
// 5.5 LVC-BPT 数据结构（Light Vertex Cache Bidirectional Path Tracing）
// ============================================================================
#ifndef VLR_LIGHT_PATH_VERTEX_DEFINED
#define VLR_LIGHT_PATH_VERTEX_DEFINED
/// 光路顶点：存储光源子路径上的表面信息，用于与视线路径做 vertex connection
struct alignas(16) LightPathVertex {
    Point3D position;
    Normal3D geometricNormal;
    ReferenceFrame shadingFrame;
    SampledSpectrum flux;
    Vector3D dirInLocal;
    uint32_t materialIndex;
    uint32_t flags;
    uint32_t pathLength;
    float _padding;

    CUDA_DEVICE_FUNCTION CUDA_INLINE bool isDeltaSampled() const { return flags & 0x1; }
    CUDA_DEVICE_FUNCTION CUDA_INLINE bool isPrevDeltaSampled() const { return flags & 0x2; }
    CUDA_DEVICE_FUNCTION CUDA_INLINE bool isWlSelected() const { return flags & 0x4; }
    CUDA_DEVICE_FUNCTION CUDA_INLINE bool isPoint() const { return flags & 0x8; }
};

/// 光路追踪状态（wavefront 架构的光路状态）
struct alignas(16) LightPathState {
    Point3D origin;
    Vector3D direction;
    SampledSpectrum flux;
    WavelengthSamples wls;
    KernelRNG rng;
    uint32_t pathLength;
    uint32_t flags;

    CUDA_DEVICE_FUNCTION CUDA_INLINE bool isActive() const { return flags & 0x1; }
    CUDA_DEVICE_FUNCTION CUDA_INLINE void setActive(bool a) { if(a) flags|=0x1; else flags&=~0x1; }
    CUDA_DEVICE_FUNCTION CUDA_INLINE void setTerminated() { flags |= 0x2; flags &= ~0x1; }
    CUDA_DEVICE_FUNCTION CUDA_INLINE bool isTerminated() const { return flags & 0x2; }
    CUDA_DEVICE_FUNCTION CUDA_INLINE bool singleWlSelected() const { return flags & 0x4; }
    CUDA_DEVICE_FUNCTION CUDA_INLINE void setSingleWlSelected() { flags |= 0x4; }
};
#endif

// ============================================================================
// 6. WavefrontLaunchParameters（TraceRays 用到的字段，布局与 path_types.h 完全一致）
// ============================================================================
#ifndef VLR_WAVEFRONT_LAUNCH_PARAMETERS_MINIMAL_DEFINED
#define VLR_WAVEFRONT_LAUNCH_PARAMETERS_MINIMAL_DEFINED
struct WavefrontLaunchParameters {
    DiscretizedSpectrumAlwaysSpectral::CMF DiscretizedSpectrum_xbar;
    DiscretizedSpectrumAlwaysSpectral::CMF DiscretizedSpectrum_ybar;
    DiscretizedSpectrumAlwaysSpectral::CMF DiscretizedSpectrum_zbar;
    float DiscretizedSpectrum_integralCMF;

    const Texture2DDescriptor* textureDescriptorBuffer;
    const uint32_t* materialNormalMapIndices;
    const uint32_t* materialAlbedoTextureIndices;
    const uint32_t* materialRoughnessTextureIndices;
    const uint32_t* materialMetallicTextureIndices;
    const MaterialTextureParams* materialTextureParamsBuffer;
    PathTexturedMaterialParams* pathTexturedParamsBuffer;

    const NodeProcedureSet* nodeProcedureSetBuffer;
    const SmallNodeDescriptor* smallNodeDescriptorBuffer;
    const MediumNodeDescriptor* mediumNodeDescriptorBuffer;
    const LargeNodeDescriptor* largeNodeDescriptorBuffer;
    const BSDFProcedureSet* bsdfProcedureSetBuffer;
    const EDFProcedureSet* edfProcedureSetBuffer;
    const IDFProcedureSet* idfProcedureSetBuffer;
    const SurfaceMaterialDescriptor* materialDescriptorBuffer;

    const GeometryInstance* geomInstBuffer;
    const Instance* instBuffer;
    const Point3D* vertexPositions;
    const Normal3D* vertexNormals;
    const TexCoord2D* vertexTexCoords;
    uint64_t topGroup;
    const SceneBounds* sceneBounds;
    const uint32_t* instIndices;
    DiscreteDistribution1D lightInstDist;
    uint32_t envLightInstIndex;
    EnvironmentImportanceMap envImportanceMap;

    int32_t progSampleLensPosition;
    int32_t progTestLensIntersection;
    int32_t progEvaluateIDF;
    CameraDescriptor cameraDescriptor;

    WavefrontPathState* pathStateBuffer;
    WavefrontHitInfo* hitInfoBuffer;
    SurfacePoint* surfacePointBuffer;

    WavefrontWorkQueue activePathQueue;
    WavefrontWorkQueue nextActivePathQueue;
    WavefrontMaterialQueues materialQueues;

    NativeBlockBuffer2D<::vlr::KernelRNG> rngBuffer;
    BlockBuffer2D<::vlr::SpectrumStorage, 0> accumBuffer;
    ::vlr::DiscretizedSpectrum* accumAlbedoBuffer;
    ::vlr::Normal3D* accumNormalBuffer;

    uint2 imageSize;
    uint32_t imageStrideInPixels;
    uint32_t numAccumFrames;
    uint32_t limitNumAccumFrames;

    uint32_t maxPathLength;
    uint32_t maxNumPaths;
    uint32_t currentDepth;

    uint32_t* numActiveRays;
    uint32_t* numShadowRays;
    uint32_t* numTerminatedPaths;

    int32_t probePixX;
    int32_t probePixY;
    uint32_t debugMode;

    // === LVC-BPT 数据（必须在末尾，不能破坏已有字段偏移） ===
    LightPathVertex* lightVertexCache;
    uint32_t* numLightVertices;
    LightPathState* lightPathStateBuffer;
    WavefrontHitInfo* lightHitInfoBuffer;
    SurfacePoint* lightSurfacePointBuffer;
    uint32_t numLightPaths;
    uint32_t maxLightVertices;
    bool useBDPT;
};
#endif

// ============================================================================
// SBT 记录数据：存储 launch parameters 指针
// ============================================================================
#ifndef VLR_WAVEFRONT_SBT_DATA_MINIMAL_DEFINED
#define VLR_WAVEFRONT_SBT_DATA_MINIMAL_DEFINED
struct WavefrontSBTData {
    const WavefrontLaunchParameters* params;
};
#endif

}  // namespace shared
}  // namespace vlr
