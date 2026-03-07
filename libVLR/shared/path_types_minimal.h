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

#include "../include/vlr/basic_types.h"
#include <cstdint>

namespace vlr {
namespace shared {

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

// 前向声明，避免包含 texture_types.h
struct Texture2DDescriptor;

// 材质类别数量（与 material_types.h 保持一致，用于 WavefrontMaterialQueues 布局）
constexpr uint32_t NumMaterialCategories = 6;

// ============================================================================
// 1. WavefrontPathState（TraceRays 仅需 origin、direction、wls、isActive）
// 布局必须与 path_types.h 完全一致（144 字节），因 pathStateBuffer 共享
// ============================================================================
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
};

static_assert(sizeof(WavefrontPathState) == 144, "PathState layout must match path_types.h");

// ============================================================================
// 2. WavefrontHitInfo
// ============================================================================
struct alignas(16) WavefrontHitInfo {
    uint32_t instIndex;
    uint32_t geomInstIndex;
    uint32_t primIndex;
    uint32_t hitFlags;
    float u, v;
    float t;
    float _padding;

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

static_assert(sizeof(WavefrontHitInfo) == 32, "HitInfo layout must match path_types.h");

// ============================================================================
// 3. WavefrontWorkQueue
// ============================================================================
struct WavefrontWorkQueue {
    uint32_t* pathIndices;
    uint32_t* counter;
    uint32_t capacity;
};

// ============================================================================
// 4. WavefrontMaterialQueues（布局匹配，用于 WavefrontLaunchParameters）
// ============================================================================
struct WavefrontMaterialQueues {
    WavefrontWorkQueue queues[NumMaterialCategories];
};

// ============================================================================
// 5. WFTracePayload
// ============================================================================
struct WFTracePayload {
    uint32_t pathIndex;
    WavelengthSamples wls;
};

// ============================================================================
// 6. WavefrontLaunchParameters（TraceRays 用到的字段，布局与 path_types.h 完全一致）
// ============================================================================
struct WavefrontLaunchParameters {
    DiscretizedSpectrumAlwaysSpectral::CMF DiscretizedSpectrum_xbar;
    DiscretizedSpectrumAlwaysSpectral::CMF DiscretizedSpectrum_ybar;
    DiscretizedSpectrumAlwaysSpectral::CMF DiscretizedSpectrum_zbar;
    float DiscretizedSpectrum_integralCMF;

    const Texture2DDescriptor* textureDescriptorBuffer;
    const uint32_t* materialNormalMapIndices;

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

    ::vlr::optixu::NativeBlockBuffer2D<::vlr::KernelRNG> rngBuffer;
    ::vlr::optixu::BlockBuffer2D<::vlr::SpectrumStorage, 0> accumBuffer;
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
};

// ============================================================================
// SBT 记录数据：存储 launch parameters 指针
// ============================================================================
struct WavefrontSBTData {
    const WavefrontLaunchParameters* params;
};

}  // namespace shared
}  // namespace vlr
