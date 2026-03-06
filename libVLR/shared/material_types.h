// ============================================================================
// VLR 材质系统 - 类型定义
//
// 本文件定义了 Wavefront 路径追踪所需的材质类型、BSDF 和 EDF 结构。
// 为 ProcessHits 和 SampleBSDF kernel 提供基础材质支持。
//
// 作者：VLR 开发团队
// 创建日期：2026-03-07
// 环境：CUDA 13.1, OptiX 8.0.0, VS2022
// ============================================================================

#pragma once

#include "kernel_common.h"

namespace vlr {
namespace shared {

// ============================================================================
// 1. 材质类别枚举
// ============================================================================

/// 材质类别：用于路径分类、排序和 BSDF 分发
/// 与 wavefront_types.h 中的 MaterialCategory 保持一致
enum MaterialCategory : uint32_t {
    MaterialCategory_Diffuse = 0,      ///< 漫反射材质（Lambert）
    MaterialCategory_Glossy,            ///< 光滑反射材质（GGX）
    MaterialCategory_Specular,          ///< 完美镜面反射
    MaterialCategory_Transmissive,      ///< 透射材质（玻璃等）
    MaterialCategory_Emissive,           ///< 发光材质
    MaterialCategory_Mixed,              ///< 混合材质
    NumMaterialCategories
};


// ============================================================================
// 2. BSDF 类型标识
// ============================================================================

/// BSDF 类型：标识材质使用的反射/透射模型
enum BSDFType : uint32_t {
    BSDFType_Lambert = 0,       ///< Lambert 漫反射
    BSDFType_GGX,               ///< GGX 微表面镜面反射
    BSDFType_Specular,          ///< 完美镜面反射
    BSDFType_SpecularTransmission,  ///< 完美镜面透射
    BSDFType_FresnelBlend,      ///< Fresnel 混合（未来扩展）
    NumBSDFTypes
};


// ============================================================================
// 3. 材质描述符数据布局
// ============================================================================

/// 材质描述符数据布局常量
/// SurfaceMaterialDescriptor::data[] 的索引约定
namespace MaterialDataLayout {
    constexpr int BSDFType = 0;            ///< data[0]: BSDF 类型 (uint32_t  reinterpret)
    constexpr int AlbedoR = 1;              ///< data[1]: 反照率/反射率 R
    constexpr int AlbedoG = 2;              ///< data[2]: 反照率/反射率 G
    constexpr int AlbedoB = 3;              ///< data[3]: 反照率/反射率 B
    constexpr int Roughness = 4;            ///< data[4]: 粗糙度 (GGX, 0~1)
    constexpr int Metallic = 5;              ///< data[5]: 金属度 (未来扩展)
    constexpr int IOR = 6;                  ///< data[6]: 折射率 (透射材质)
    constexpr int EmissionR = 7;            ///< data[7]: 发光强度 R
    constexpr int EmissionG = 8;            ///< data[8]: 发光强度 G
    constexpr int EmissionB = 9;             ///< data[9]: 发光强度 B
}


// ============================================================================
// 4. SurfaceMaterialDescriptor 扩展接口
// ============================================================================

/// 获取材质 data 的 float 视图
/// data[] 以 uint32_t 存储，按 float 解释用于 RGB、粗糙度等参数
CUDA_DEVICE_FUNCTION CUDA_INLINE const float* getMaterialDataAsFloats(
    const SurfaceMaterialDescriptor& matDesc) {
    return reinterpret_cast<const float*>(matDesc.data);
}

/// 从材质描述符获取 BSDF 类型
/// procedural 材质使用 bsdfProcedureSetIndex 存储类型 ID（0=Lambert, 1=GGX, 2=Specular 等）
CUDA_DEVICE_FUNCTION CUDA_INLINE BSDFType getBSDFType(
    const SurfaceMaterialDescriptor& matDesc) {
    uint32_t typeId = matDesc.bsdfProcedureSetIndex;
    if (typeId >= NumBSDFTypes)
        return BSDFType_Lambert;
    return static_cast<BSDFType>(typeId);
}

/// 从材质描述符获取 Lambert 反照率（RGB 转 SampledSpectrum）
CUDA_DEVICE_FUNCTION CUDA_INLINE void getLambertAlbedo(
    const SurfaceMaterialDescriptor& matDesc,
    SampledSpectrum* albedo) {
    const float* d = getMaterialDataAsFloats(matDesc);
    float r = d[MaterialDataLayout::AlbedoR];
    float g = d[MaterialDataLayout::AlbedoG];
    float b = d[MaterialDataLayout::AlbedoB];
    // 简化为均匀光谱（可扩展为波长相关）
    for (int i = 0; i < NumSpectralSamples; ++i)
        albedo->values[i] = (r + g + b) / 3.0f * ::vlr::VLR_M_INV_PI;
}

/// 从材质描述符获取 GGX 参数
CUDA_DEVICE_FUNCTION CUDA_INLINE void getGGXParams(
    const SurfaceMaterialDescriptor& matDesc,
    SampledSpectrum* reflectance,
    float* roughness) {
    float r = matDesc.data[MaterialDataLayout::AlbedoR];
    float g = matDesc.data[MaterialDataLayout::AlbedoG];
    float b = matDesc.data[MaterialDataLayout::AlbedoB];
    for (int i = 0; i < NumSpectralSamples; ++i)
        reflectance->values[i] = (r + g + b) / 3.0f;
    *roughness = matDesc.data[MaterialDataLayout::Roughness];
    *roughness = (*roughness < 0.001f) ? 0.001f : *roughness;  // 避免除零
}

/// 从材质描述符获取镜面反射率
CUDA_DEVICE_FUNCTION CUDA_INLINE void getSpecularReflectance(
    const SurfaceMaterialDescriptor& matDesc,
    SampledSpectrum* reflectance) {
    const float* d = getMaterialDataAsFloats(matDesc);
    float r = d[MaterialDataLayout::AlbedoR];
    float g = d[MaterialDataLayout::AlbedoG];
    float b = d[MaterialDataLayout::AlbedoB];
    for (int i = 0; i < NumSpectralSamples; ++i)
        reflectance->values[i] = (r + g + b) / 3.0f;
}

/// 从材质描述符获取发光强度
CUDA_DEVICE_FUNCTION CUDA_INLINE void getEmissiveRadiance(
    const SurfaceMaterialDescriptor& matDesc,
    SampledSpectrum* radiance) {
    const float* d = getMaterialDataAsFloats(matDesc);
    float r = d[MaterialDataLayout::EmissionR];
    float g = d[MaterialDataLayout::EmissionG];
    float b = d[MaterialDataLayout::EmissionB];
    for (int i = 0; i < NumSpectralSamples; ++i)
        radiance->values[i] = (r + g + b) / 3.0f;
}


// ============================================================================
// 5. BSDF 上下文结构
// ============================================================================

/// BSDF 评估上下文：包含材质、表面点和波长信息
/// 用于 ProcessHits/SampleBSDF kernel 中传递 BSDF 参数
struct BSDFContext {
    const SurfaceMaterialDescriptor* matDesc;  ///< 材质描述符
    const SurfacePoint* surfPt;                ///< 表面点
    const WavelengthSamples* wls;               ///< 波长采样
    Normal3D geomNormalLocal;                  ///< 几何法线（局部坐标）

    CUDA_DEVICE_FUNCTION CUDA_INLINE BSDFContext() = default;

    CUDA_DEVICE_FUNCTION CUDA_INLINE BSDFContext(
        const SurfaceMaterialDescriptor& desc,
        const SurfacePoint& sp,
        const WavelengthSamples& wavelengthSamples)
        : matDesc(&desc)
        , surfPt(&sp)
        , wls(&wavelengthSamples)
        , geomNormalLocal(sp.shadingFrame.toLocal(sp.geometricNormal))
    {}
};


// ============================================================================
// 6. BSDF 采样结果
// ============================================================================

/// BSDF 采样结果：sampleBSDF() 的返回值
struct BSDFSampleResult {
    Vector3D dirLocal;             ///< 采样方向（局部坐标系，半球空间）
    SampledSpectrum f;             ///< BSDF 值 f(w_o | w_i)
    float pdf;                     ///< 方向概率密度
    BSDFType sampledBSDFType;      ///< 采样的 BSDF 分量类型
    bool isDelta;                  ///< 是否为 delta 分布（镜面）

    CUDA_DEVICE_FUNCTION CUDA_INLINE bool isValid() const {
        return pdf > 0.0f && f.hasNonZero();
    }
};


// ============================================================================
// 7. EDF 上下文与结果
// ============================================================================

/// EDF（发光分布函数）上下文
struct EDFContext {
    const SurfaceMaterialDescriptor* matDesc;
    const SurfacePoint* surfPt;
    const WavelengthSamples* wls;

    CUDA_DEVICE_FUNCTION CUDA_INLINE EDFContext(
        const SurfaceMaterialDescriptor& desc,
        const SurfacePoint& sp,
        const WavelengthSamples& wavelengthSamples)
        : matDesc(&desc)
        , surfPt(&sp)
        , wls(&wavelengthSamples)
    {}
};

/// EDF 评估结果
struct EDFEvaluateResult {
    SampledSpectrum Le;            ///< 出射辐射度
    bool hasEmission;              ///< 是否有发光

    CUDA_DEVICE_FUNCTION CUDA_INLINE explicit operator bool() const {
        return hasEmission && Le.hasNonZero();
    }
};


// ============================================================================
// 8. 材质分类辅助
// ============================================================================

/// 根据 BSDF 类型返回材质类别
CUDA_DEVICE_FUNCTION CUDA_INLINE MaterialCategory bsdfTypeToMaterialCategory(
    BSDFType type) {
    switch (type) {
    case BSDFType_Lambert:
        return MaterialCategory_Diffuse;
    case BSDFType_GGX:
        return MaterialCategory_Glossy;
    case BSDFType_Specular:
    case BSDFType_SpecularTransmission:
        return MaterialCategory_Specular;
    default:
        return MaterialCategory_Diffuse;
    }
}

/// 检查材质是否有非 Delta 分量（可用于 NEE）
CUDA_DEVICE_FUNCTION CUDA_INLINE bool materialHasNonDelta(
    const SurfaceMaterialDescriptor& matDesc) {
    BSDFType type = getBSDFType(matDesc);
    return type == BSDFType_Lambert || type == BSDFType_GGX;
}

/// 检查材质是否为 Delta（完美镜面）
CUDA_DEVICE_FUNCTION CUDA_INLINE bool materialIsDelta(
    const SurfaceMaterialDescriptor& matDesc) {
    BSDFType type = getBSDFType(matDesc);
    return type == BSDFType_Specular || type == BSDFType_SpecularTransmission;
}

/// 将 BSDF 类型映射为 DirectionType（用于 MIS、路径历史等）
/// 需与 kernel_common.h 中的 DirectionType 配合使用
CUDA_DEVICE_FUNCTION CUDA_INLINE DirectionType bsdfTypeToDirectionType(BSDFType type) {
    switch (type) {
    case BSDFType_Lambert:
        return DirectionType::Reflection();
    case BSDFType_GGX:
        return DirectionType::HighFreq() | DirectionType::Reflection();
    case BSDFType_Specular:
        return DirectionType::Delta0D() | DirectionType::Reflection();
    case BSDFType_SpecularTransmission:
        return DirectionType::Delta0D() | DirectionType::Transmission();
    default:
        return DirectionType();
    }
}

/// 检查 BSDF 类型是否为色散材质（透射且与波长相关）
/// 色散材质需在首次采样后选择单一波长
CUDA_DEVICE_FUNCTION CUDA_INLINE bool isDispersiveBSDFType(BSDFType type) {
    return type == BSDFType_SpecularTransmission;
}

/// 检查材质是否有发光
CUDA_DEVICE_FUNCTION CUDA_INLINE bool materialHasEmission(
    const SurfaceMaterialDescriptor& matDesc) {
    const float* d = getMaterialDataAsFloats(matDesc);
    float r = d[MaterialDataLayout::EmissionR];
    float g = d[MaterialDataLayout::EmissionG];
    float b = d[MaterialDataLayout::EmissionB];
    return (r > 0.0f || g > 0.0f || b > 0.0f);
}

} // namespace shared
} // namespace vlr
