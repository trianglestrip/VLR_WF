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
/// 与 path_types.h 中的 MaterialCategory 保持一致
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
    BSDFType_Lambert = 0,              ///< Lambert 漫反射
    BSDFType_LambertCheckerboard,      ///< Lambert 漫反射 + 棋盘格纹理
    BSDFType_GGX,                      ///< GGX 微表面镜面反射
    BSDFType_Specular,                 ///< 完美镜面反射
    BSDFType_SpecularTransmission,      ///< 完美镜面透射（支持色散）
    BSDFType_GGXTransmission,           ///< 粗糙透射（GGX 微表面透射）
    BSDFType_FresnelBlend,             ///< Fresnel 混合 Lambertian
    BSDFType_UE4BRDF,                  ///< UE4 风格 BRDF（金属工作流）
    BSDFType_FrostbiteBRDF,            ///< Frostbite 风格 BRDF
    BSDFType_MixedBSDF,                ///< 混合 BSDF（多层材质）
    NumBSDFTypes
};


// ============================================================================
// 3. 材质描述符数据布局
// ============================================================================

/// 材质描述符数据布局常量
/// SurfaceMaterialDescriptor::data[] 以 uint32_t 存储，按 float 解释（getMaterialDataAsFloats）
namespace MaterialDataLayout {
    constexpr int BSDFType = 0;             ///< data[0]: BSDF 类型 (uint32_t reinterpret)
    constexpr int AlbedoR = 1;              ///< data[1]: 反照率/基础色 R
    constexpr int AlbedoG = 2;             ///< data[2]: 反照率/基础色 G
    constexpr int AlbedoB = 3;             ///< data[3]: 反照率/基础色 B
    constexpr int Roughness = 4;            ///< data[4]: 粗糙度 (GGX, 0~1)
    constexpr int Metallic = 5;             ///< data[5]: 金属度 (UE4/Frostbite)
    constexpr int IOR = 6;                  ///< data[6]: 折射率 (透射材质，电介质)
    constexpr int EmissionR = 7;            ///< data[7]: 发光强度 R
    constexpr int EmissionG = 8;            ///< data[8]: 发光强度 G
    constexpr int EmissionB = 9;            ///< data[9]: 发光强度 B
    constexpr int DispersionStrength = 10;  ///< data[10]: 色散强度 (透射) / 导体 EtaR
    constexpr int EtaR = 10;
    constexpr int EtaG = 11;                ///< data[11]: 导体 eta G
    constexpr int EtaB = 12;                ///< data[12]: 导体 eta B
    constexpr int KappaR = 13;              ///< data[13]: 导体消光系数 k R
    constexpr int KappaG = 14;              ///< data[14]: 导体消光系数 k G
    constexpr int KappaB = 15;              ///< data[15]: 导体消光系数 k B
    // 棋盘格纹理（LambertCheckerboard）
    constexpr int CheckerboardColor1R = 10; ///< data[10]: 棋盘格第二种颜色 R（复用 EtaR 槽位）
    constexpr int CheckerboardColor1G = 11; ///< data[11]: 棋盘格第二种颜色 G
    constexpr int CheckerboardColor1B = 12; ///< data[12]: 棋盘格第二种颜色 B
    constexpr int CheckerboardGridSize = 13;///< data[13]: 棋盘格密度 (如 8 表示 8x8)
    // FresnelBlend: Roughness=镜面粗糙度, Albedo=漫反射
    // GGXTransmission: IOR + Roughness
    // MixedBSDF: Metallic=混合权重
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
/// 使用 values[0..2]=RGB 保留颜色，values[3]=亮度用于重要性采样
CUDA_DEVICE_FUNCTION CUDA_INLINE void getLambertAlbedo(
    const SurfaceMaterialDescriptor& matDesc,
    SampledSpectrum* albedo) {
    const float* d = getMaterialDataAsFloats(matDesc);
    float r = d[MaterialDataLayout::AlbedoR];
    float g = d[MaterialDataLayout::AlbedoG];
    float b = d[MaterialDataLayout::AlbedoB];
    albedo->values[0] = r;
    albedo->values[1] = g;
    albedo->values[2] = b;
    albedo->values[3] = (r + g + b) / 3.0f;  // 亮度用于重要性采样
}

/// 从材质描述符获取 Lambert 棋盘格反照率（按 UV 在两种颜色间切换）
/// 对于水平表面（法线接近 ±Y）使用 position.x/z 作为 UV；否则使用 texCoord
CUDA_DEVICE_FUNCTION CUDA_INLINE void getLambertAlbedoCheckerboard(
    const SurfaceMaterialDescriptor& matDesc,
    const SurfacePoint* surfPt,
    SampledSpectrum* albedo) {
    const float* d = getMaterialDataAsFloats(matDesc);
    float r0 = d[MaterialDataLayout::AlbedoR];
    float g0 = d[MaterialDataLayout::AlbedoG];
    float b0 = d[MaterialDataLayout::AlbedoB];
    float r1 = d[MaterialDataLayout::CheckerboardColor1R];
    float g1 = d[MaterialDataLayout::CheckerboardColor1G];
    float b1 = d[MaterialDataLayout::CheckerboardColor1B];
    float gridSize = d[MaterialDataLayout::CheckerboardGridSize];
    if (gridSize < 1.0f) gridSize = 8.0f;

    float u, v;
    if (surfPt == nullptr) {
        u = v = 0.0f;
    } else {
        float ny = (surfPt->geometricNormal.y >= 0) ? surfPt->geometricNormal.y : -surfPt->geometricNormal.y;
        if (ny > 0.9f) {
            u = surfPt->position.x;
            v = surfPt->position.z;
        } else {
            u = surfPt->texCoord.x;
            v = surfPt->texCoord.y;
        }
#ifdef __CUDACC__
        u = u - floorf(u);
        v = v - floorf(v);
#else
        u = u - std::floor(u);
        v = v - std::floor(v);
#endif
        if (u < 0.0f) u += 1.0f;
        if (v < 0.0f) v += 1.0f;
    }

#ifdef __CUDACC__
    int iu = (int)floorf(u * gridSize);
    int iv = (int)floorf(v * gridSize);
#else
    int iu = (int)std::floor(u * gridSize);
    int iv = (int)std::floor(v * gridSize);
#endif
    bool useColor0 = ((iu + iv) % 2 == 0);
    if (useColor0) {
        albedo->values[0] = r0;
        albedo->values[1] = g0;
        albedo->values[2] = b0;
    } else {
        albedo->values[0] = r1;
        albedo->values[1] = g1;
        albedo->values[2] = b1;
    }
    albedo->values[3] = (albedo->values[0] + albedo->values[1] + albedo->values[2]) / 3.0f;
}

/// 从材质描述符获取 GGX 参数
CUDA_DEVICE_FUNCTION CUDA_INLINE void getGGXParams(
    const SurfaceMaterialDescriptor& matDesc,
    SampledSpectrum* reflectance,
    float* roughness) {
    const float* d = getMaterialDataAsFloats(matDesc);
    float r = d[MaterialDataLayout::AlbedoR];
    float g = d[MaterialDataLayout::AlbedoG];
    float b = d[MaterialDataLayout::AlbedoB];
    reflectance->values[0] = r;
    reflectance->values[1] = g;
    reflectance->values[2] = b;
    reflectance->values[3] = (r + g + b) / 3.0f;
    *roughness = d[MaterialDataLayout::Roughness];
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
    reflectance->values[0] = r;
    reflectance->values[1] = g;
    reflectance->values[2] = b;
    reflectance->values[3] = (r + g + b) / 3.0f;
}

/// 从材质描述符获取透射材质参数（IOR + 色散）
/// dispersionStrength: 色散强度，0=无色散，>0 时 n(λ) 随波长变化（Cauchy 近似）
CUDA_DEVICE_FUNCTION CUDA_INLINE void getTransmissionParams(
    const SurfaceMaterialDescriptor& matDesc,
    float* ior,
    float* dispersionStrength) {
    const float* d = getMaterialDataAsFloats(matDesc);
    *ior = d[MaterialDataLayout::IOR];
    *ior = (*ior < 1.0f) ? 1.0f : *ior;
    *dispersionStrength = d[MaterialDataLayout::DispersionStrength];
}

/// 从材质描述符获取导体 Fresnel 参数（eta, k 复折射率）
/// 用于完整 Fresnel 导体计算
CUDA_DEVICE_FUNCTION CUDA_INLINE void getConductorParams(
    const SurfaceMaterialDescriptor& matDesc,
    SampledSpectrum* eta,
    SampledSpectrum* kappa) {
    const float* d = getMaterialDataAsFloats(matDesc);
    // 简化：使用 RGB 或灰度
    float er = d[MaterialDataLayout::EtaR];
    float eg = d[MaterialDataLayout::EtaG];
    float eb = d[MaterialDataLayout::EtaB];
    float kr = d[MaterialDataLayout::KappaR];
    float kg = d[MaterialDataLayout::KappaG];
    float kb = d[MaterialDataLayout::KappaB];
    for (int i = 0; i < NumSpectralSamples; ++i) {
        eta->values[i] = (er + eg + eb) / 3.0f;
        kappa->values[i] = (kr + kg + kb) / 3.0f;
    }
}

/// 从材质描述符获取 FresnelBlend 参数
CUDA_DEVICE_FUNCTION CUDA_INLINE void getFresnelBlendParams(
    const SurfaceMaterialDescriptor& matDesc,
    SampledSpectrum* diffuseReflectance,
    SampledSpectrum* specularReflectance,
    float* roughness) {
    const float* d = getMaterialDataAsFloats(matDesc);
    float r = d[MaterialDataLayout::AlbedoR];
    float g = d[MaterialDataLayout::AlbedoG];
    float b = d[MaterialDataLayout::AlbedoB];
    diffuseReflectance->values[0] = r;
    diffuseReflectance->values[1] = g;
    diffuseReflectance->values[2] = b;
    diffuseReflectance->values[3] = (r + g + b) / 3.0f;
    *specularReflectance = *diffuseReflectance;  // 可扩展为独立参数
    *roughness = ::vlr::vlr_max(d[MaterialDataLayout::Roughness], 0.001f);
}

/// 从材质描述符获取 UE4/Frostbite BRDF 参数
CUDA_DEVICE_FUNCTION CUDA_INLINE void getUE4Params(
    const SurfaceMaterialDescriptor& matDesc,
    SampledSpectrum* baseColor,
    float* metallic,
    float* roughness) {
    const float* d = getMaterialDataAsFloats(matDesc);
    float r = d[MaterialDataLayout::AlbedoR];
    float g = d[MaterialDataLayout::AlbedoG];
    float b = d[MaterialDataLayout::AlbedoB];
    baseColor->values[0] = r;
    baseColor->values[1] = g;
    baseColor->values[2] = b;
    baseColor->values[3] = (r + g + b) / 3.0f;
    *metallic = ::vlr::vlr_max(0.0f, ::vlr::vlr_min(1.0f, d[MaterialDataLayout::Metallic]));
    *roughness = ::vlr::vlr_max(0.001f, d[MaterialDataLayout::Roughness]);
}

/// 从材质描述符获取 Mixed BSDF 混合权重
CUDA_DEVICE_FUNCTION CUDA_INLINE void getMixedParams(
    const SurfaceMaterialDescriptor& matDesc,
    SampledSpectrum* albedo0,
    SampledSpectrum* albedo1,
    float* blendWeight,
    float* roughness) {
    const float* d = getMaterialDataAsFloats(matDesc);
    float r = d[MaterialDataLayout::AlbedoR];
    float g = d[MaterialDataLayout::AlbedoG];
    float b = d[MaterialDataLayout::AlbedoB];
    albedo0->values[0] = albedo1->values[0] = r;
    albedo0->values[1] = albedo1->values[1] = g;
    albedo0->values[2] = albedo1->values[2] = b;
    albedo0->values[3] = albedo1->values[3] = (r + g + b) / 3.0f;
    *blendWeight = ::vlr::vlr_max(0.0f, ::vlr::vlr_min(1.0f, d[MaterialDataLayout::Metallic]));
    *roughness = ::vlr::vlr_max(0.001f, d[MaterialDataLayout::Roughness]);
}

/// 从材质描述符获取发光强度
CUDA_DEVICE_FUNCTION CUDA_INLINE void getEmissiveRadiance(
    const SurfaceMaterialDescriptor& matDesc,
    SampledSpectrum* radiance) {
    const float* d = getMaterialDataAsFloats(matDesc);
    float r = d[MaterialDataLayout::EmissionR];
    float g = d[MaterialDataLayout::EmissionG];
    float b = d[MaterialDataLayout::EmissionB];
    radiance->values[0] = r;
    radiance->values[1] = g;
    radiance->values[2] = b;
    radiance->values[3] = (r + g + b) / 3.0f;
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
    bool singleWlSelected;                    ///< 色散材质是否已选择单波长

    CUDA_DEVICE_FUNCTION CUDA_INLINE BSDFContext() : singleWlSelected(false) {}

    CUDA_DEVICE_FUNCTION CUDA_INLINE BSDFContext(
        const SurfaceMaterialDescriptor& desc,
        const SurfacePoint& sp,
        const WavelengthSamples& wavelengthSamples,
        bool singleWl = false)
        : matDesc(&desc)
        , surfPt(&sp)
        , wls(&wavelengthSamples)
        , geomNormalLocal(sp.shadingFrame.toLocal(sp.geometricNormal))
        , singleWlSelected(singleWl)
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
    case BSDFType_LambertCheckerboard:
    case BSDFType_FresnelBlend:
        return MaterialCategory_Diffuse;
    case BSDFType_GGX:
    case BSDFType_UE4BRDF:
    case BSDFType_FrostbiteBRDF:
        return MaterialCategory_Glossy;
    case BSDFType_Specular:
    case BSDFType_SpecularTransmission:
        return MaterialCategory_Specular;
    case BSDFType_GGXTransmission:
        return MaterialCategory_Transmissive;
    case BSDFType_MixedBSDF:
        return MaterialCategory_Mixed;
    default:
        return MaterialCategory_Diffuse;
    }
}

/// 检查材质是否有非 Delta 分量（可用于 NEE）
CUDA_DEVICE_FUNCTION CUDA_INLINE bool materialHasNonDelta(
    const SurfaceMaterialDescriptor& matDesc) {
    BSDFType type = getBSDFType(matDesc);
    return type == BSDFType_Lambert || type == BSDFType_LambertCheckerboard || type == BSDFType_GGX
        || type == BSDFType_FresnelBlend || type == BSDFType_UE4BRDF
        || type == BSDFType_FrostbiteBRDF || type == BSDFType_GGXTransmission
        || type == BSDFType_MixedBSDF;
}

/// 检查材质是否为 Delta（完美镜面）
CUDA_DEVICE_FUNCTION CUDA_INLINE bool materialIsDelta(
    const SurfaceMaterialDescriptor& matDesc) {
    BSDFType type = getBSDFType(matDesc);
    return type == BSDFType_Specular || type == BSDFType_SpecularTransmission;
}

/// 将 BSDF 类型映射为 DirectionType（用于 MIS、路径历史等）
CUDA_DEVICE_FUNCTION CUDA_INLINE DirectionType bsdfTypeToDirectionType(BSDFType type) {
    switch (type) {
    case BSDFType_Lambert:
    case BSDFType_LambertCheckerboard:
    case BSDFType_FresnelBlend:
        return DirectionType::Reflection();
    case BSDFType_GGX:
    case BSDFType_UE4BRDF:
    case BSDFType_FrostbiteBRDF:
        return DirectionType::HighFreq() | DirectionType::Reflection();
    case BSDFType_Specular:
        return DirectionType::Delta0D() | DirectionType::Reflection();
    case BSDFType_SpecularTransmission:
        return DirectionType::Delta0D() | DirectionType::Transmission();
    case BSDFType_GGXTransmission:
        return DirectionType::HighFreq() | DirectionType::Transmission();
    case BSDFType_MixedBSDF:
        return DirectionType::HighFreq() | DirectionType::Reflection();
    default:
        return DirectionType();
    }
}

/// 检查 BSDF 类型是否为色散材质（透射且与波长相关）
/// 色散材质需在首次采样后选择单一波长
CUDA_DEVICE_FUNCTION CUDA_INLINE bool isDispersiveBSDFType(BSDFType type) {
    return type == BSDFType_SpecularTransmission || type == BSDFType_GGXTransmission;
}

/// 色散材质单波长选择逻辑：根据随机数或当前索引选择单一波长
/// 当 singleWlSelected 后，路径仅使用 selectedLambda 对应波长
CUDA_DEVICE_FUNCTION CUDA_INLINE uint32_t selectSingleWavelengthForDispersion(
    const WavelengthSamples* wls, float u) {
    if (!wls) return 0;
    return static_cast<uint32_t>(u * NumSpectralSamples) % NumSpectralSamples;
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
