// ============================================================================
// VLR Wavefront TraceRays - 鏈€灏忓寲绫诲瀷瀹氫箟
//
// 鏈枃浠朵粎鍖呭惈 TraceRays OptiX 绋嬪簭鎵€闇€鐨勬渶灏忕被鍨嬮泦鍚堛€?
// 鐢ㄤ簬 PTX 缂栬瘧锛岄伩鍏?material_types銆乼exture_types 绛夐噸鍨嬩緷璧栥€?
//
// 浣滆€咃細VLR 寮€鍙戝洟闃?
// 鍒涘缓鏃ユ湡锛?026-03-07
// ============================================================================

#pragma once

#include "vlr/basic_types.h"
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

// OptiX Buffer 绫诲瀷锛堟湰鍦板畾涔夛紝閬垮厤妯℃澘瑙ｆ瀽闂锛?
template <typename T>
struct NativeBlockBuffer2D {
    T* data;
};

template <typename T, int N>
struct BlockBuffer2D {
    T* data;
};

// 鍓嶅悜澹版槑锛岄伩鍏嶅寘鍚?texture_types.h
struct Texture2DDescriptor;
struct PathTexturedMaterialParams;
struct MaterialTextureParams;

// 鏉愯川绫诲埆鏁伴噺锛堜笌 kernel_common.h 淇濇寔涓€鑷达紝鐢ㄤ簬 WavefrontMaterialQueues 甯冨眬锛?
constexpr uint32_t NumMaterialCategories = 6;

// ============================================================================
// 1. WavefrontPathState锛圱raceRays 浠呴渶 origin銆乨irection銆亀ls銆乮sActive锛?
// 甯冨眬蹇呴』涓?path_types.h 瀹屽叏涓€鑷达紙144 瀛楄妭锛夛紝鍥?pathStateBuffer 鍏变韩
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
// 4. WavefrontMaterialQueues锛堝竷灞€鍖归厤锛岀敤浜?WavefrontLaunchParameters锛?
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
// 6. WavefrontLaunchParameters锛圱raceRays 鐢ㄥ埌鐨勫瓧娈碉紝甯冨眬涓?path_types.h 瀹屽叏涓€鑷达級
// ============================================================================
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
};

// ============================================================================
// SBT 璁板綍鏁版嵁锛氬瓨鍌?launch parameters 鎸囬拡
// ============================================================================
struct WavefrontSBTData {
    const WavefrontLaunchParameters* params;
};

}  // namespace shared
}  // namespace vlr
