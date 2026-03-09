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
#include "texture_types.h"

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
    MaterialCategory_Count
};
static_assert(static_cast<uint32_t>(MaterialCategory_Count) == NumMaterialCategories, "MaterialCategory count mismatch");


// ============================================================================
// 2. BSDF 类型标识
// ============================================================================

/// BSDF 类型：标识材质使用的反射/透射模型
enum BSDFType : uint32_t {
    BSDFType_Lambert = 0,              ///< Lambert 漫反射
    BSDFType_LambertCheckerboard,      ///< Lambert 漫反射 + 棋盘格纹理
    BSDFType_GGX,                      ///< GGX 微表面镜面反射（电介质）
    BSDFType_MicrofacetReflection,     ///< GGX 微表面反射（导体，Fresnel）
    BSDFType_MicrofacetScattering,     ///< GGX 微表面散射（电介质，反射+折射）
    BSDFType_Specular,                 ///< 完美镜面反射
    BSDFType_SpecularTransmission,      ///< 完美镜面透射（支持色散）
    BSDFType_LambertianScattering,       ///< 次表面散射（双向 Lambert，允许透射）
    BSDFType_GGXTransmission,           ///< 粗糙透射（GGX 微表面透射）
    BSDFType_FresnelBlend,             ///< Fresnel 混合 Lambertian
    BSDFType_UE4BRDF,                  ///< UE4 风格 BRDF（金属工作流）
    BSDFType_FrostbiteBRDF,            ///< Frostbite 风格 BRDF
    BSDFType_DisneyBRDF,               ///< Disney Principled BRDF (Burley 2012)
    BSDFType_MixedBSDF,                ///< 混合 BSDF（2 层简化版，向后兼容）
    BSDFType_MultiSurface,             ///< 多表面材质（2-4 层完整版）
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
    constexpr int Anisotropy = 5;           ///< data[5]: 各向异性强度 (MicrofacetReflection, 0=各向同性, 0.9=强各向异性)
    // 棋盘格纹理（LambertCheckerboard）
    constexpr int CheckerboardColor1R = 10; ///< data[10]: 棋盘格第二种颜色 R（复用 EtaR 槽位）
    constexpr int CheckerboardColor1G = 11; ///< data[11]: 棋盘格第二种颜色 G
    constexpr int CheckerboardColor1B = 12; ///< data[12]: 棋盘格第二种颜色 B
    constexpr int CheckerboardGridSize = 13;///< data[13]: 棋盘格密度 (如 8 表示 8x8)
    constexpr int CheckerboardExtent = 14;  ///< data[14]: 水平面 extent（半边长，>0 时将 position 归一化到 [0,1]，0=使用 frac 周期）
    // FresnelBlend: Roughness=镜面粗糙度, Albedo=漫反射
    // GGXTransmission: IOR + Roughness
    // MixedBSDF: Metallic=混合权重
    // MultiSurface: 4 层子材质（slots 16-43）
    constexpr int MultiSurface_NumLayers = 16;   ///< data[16]: 层数 (2-4)
    constexpr int SubMaterial0_BSDFType = 17;    ///< data[17]: 子材质 0 类型
    constexpr int SubMaterial0_AlbedoR = 18;     ///< data[18-20]: 子材质 0 反照率 RGB
    constexpr int SubMaterial0_AlbedoG = 19;
    constexpr int SubMaterial0_AlbedoB = 20;
    constexpr int SubMaterial0_Roughness = 21;   ///< data[21]: 子材质 0 粗糙度
    constexpr int SubMaterial1_BSDFType = 22;   ///< data[22]: 子材质 1 类型
    constexpr int SubMaterial1_AlbedoR = 23;
    constexpr int SubMaterial1_AlbedoG = 24;
    constexpr int SubMaterial1_AlbedoB = 25;
    constexpr int SubMaterial1_Roughness = 26;
    constexpr int SubMaterial2_BSDFType = 27;
    constexpr int SubMaterial2_AlbedoR = 28;
    constexpr int SubMaterial2_AlbedoG = 29;
    constexpr int SubMaterial2_AlbedoB = 30;
    constexpr int SubMaterial2_Roughness = 31;
    constexpr int SubMaterial3_BSDFType = 32;
    constexpr int SubMaterial3_AlbedoR = 33;
    constexpr int SubMaterial3_AlbedoG = 34;
    constexpr int SubMaterial3_AlbedoB = 35;
    constexpr int SubMaterial3_Roughness = 36;
    constexpr int MultiSurface_Weight0 = 37;     ///< data[37-40]: 混合权重
    constexpr int MultiSurface_Weight1 = 38;
    constexpr int MultiSurface_Weight2 = 39;
    constexpr int MultiSurface_Weight3 = 40;
    // Disney BRDF（使用 slots 41-50，避免与 MultiSurface 重叠）
    constexpr int Disney_Metallic = 41;
    constexpr int Disney_Subsurface = 42;
    constexpr int Disney_Specular = 43;
    constexpr int Disney_SpecularTint = 44;
    constexpr int Disney_Anisotropic = 45;
    constexpr int Disney_Sheen = 46;
    constexpr int Disney_SheenTint = 47;
    constexpr int Disney_Clearcoat = 48;
    constexpr int Disney_ClearcoatGloss = 49;
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

/// 从纹理化参数获取 Lambert 反照率
CUDA_DEVICE_FUNCTION CUDA_INLINE void getLambertAlbedoFromTextured(
    const PathTexturedMaterialParams& tp,
    SampledSpectrum* albedo) {
    albedo->values[0] = tp.baseColorR;
    albedo->values[1] = tp.baseColorG;
    albedo->values[2] = tp.baseColorB;
    albedo->values[3] = (tp.baseColorR + tp.baseColorG + tp.baseColorB) / 3.0f;
}

/// 获取有效 Lambert 反照率（优先纹理化参数）
CUDA_DEVICE_FUNCTION CUDA_INLINE void getEffectiveLambertAlbedo(
    const SurfaceMaterialDescriptor& matDesc,
    const PathTexturedMaterialParams* texParams,
    SampledSpectrum* albedo) {
    if (texParams != nullptr && (texParams->flags & PathTexturedFlags::HasBaseColorTex)) {
        getLambertAlbedoFromTextured(*texParams, albedo);
    } else {
        getLambertAlbedo(matDesc, albedo);
    }
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

    float extent = d[MaterialDataLayout::CheckerboardExtent];
    float u, v;
    if (surfPt == nullptr) {
        u = v = 0.0f;
    } else {
        float ny = (surfPt->geometricNormal.y >= 0) ? surfPt->geometricNormal.y : -surfPt->geometricNormal.y;
        if (ny > 0.9f) {
            if (extent > 0.0f) {
                // 将 position 归一化到 [0,1]，使 gridSize 个格子均匀覆盖整个平面
                u = (surfPt->position.x + extent) / (2.0f * extent);
                v = (surfPt->position.z + extent) / (2.0f * extent);
                u = ::vlr::vlr_max(0.0f, ::vlr::vlr_min(1.0f, u));
                v = ::vlr::vlr_max(0.0f, ::vlr::vlr_min(1.0f, v));
            } else {
                u = surfPt->position.x;
                v = surfPt->position.z;
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
        } else {
            u = surfPt->texCoord.x;
            v = surfPt->texCoord.y;
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

/// 获取有效 GGX 参数（优先纹理化参数）
CUDA_DEVICE_FUNCTION CUDA_INLINE void getEffectiveGGXParams(
    const SurfaceMaterialDescriptor& matDesc,
    const PathTexturedMaterialParams* texParams,
    SampledSpectrum* reflectance,
    float* roughness) {
    if (texParams != nullptr) {
        reflectance->values[0] = texParams->baseColorR;
        reflectance->values[1] = texParams->baseColorG;
        reflectance->values[2] = texParams->baseColorB;
        reflectance->values[3] = (texParams->baseColorR + texParams->baseColorG + texParams->baseColorB) / 3.0f;
        *roughness = (texParams->flags & PathTexturedFlags::HasRoughnessTex) ? texParams->roughness : getMaterialDataAsFloats(matDesc)[MaterialDataLayout::Roughness];
    } else {
        getGGXParams(matDesc, reflectance, roughness);
        return;
    }
    *roughness = (*roughness < 0.001f) ? 0.001f : *roughness;
}

/// 从材质描述符获取镜面反射率（简单镜面，无 Fresnel）
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

/// 从材质描述符获取镜面导体参数（coeffR, eta, k）
/// 与原始 VLR SpecularReflectionSurfaceMaterial 一致：coeff * FresnelConductor(eta, k)
/// 当 eta、k 未设置（全为 0）时，使用完美镜面 F=1
CUDA_DEVICE_FUNCTION CUDA_INLINE void getSpecularConductorParams(
    const SurfaceMaterialDescriptor& matDesc,
    SampledSpectrum* coeffR,
    SampledSpectrum* eta,
    SampledSpectrum* kappa) {
    const float* d = getMaterialDataAsFloats(matDesc);
    float cr = d[MaterialDataLayout::AlbedoR];
    float cg = d[MaterialDataLayout::AlbedoG];
    float cb = d[MaterialDataLayout::AlbedoB];
    coeffR->values[0] = cr;
    coeffR->values[1] = cg;
    coeffR->values[2] = cb;
    coeffR->values[3] = (cr + cg + cb) / 3.0f;
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

/// 从材质描述符获取导体微表面反射参数（eta, kappa, roughness）
/// 用于 BSDFType_MicrofacetReflection
CUDA_DEVICE_FUNCTION CUDA_INLINE void getMicrofacetReflectionParams(
    const SurfaceMaterialDescriptor& matDesc,
    SampledSpectrum* eta,
    SampledSpectrum* kappa,
    float* roughness) {
    const float* d = getMaterialDataAsFloats(matDesc);
    float er = d[MaterialDataLayout::EtaR];
    float eg = d[MaterialDataLayout::EtaG];
    float eb = d[MaterialDataLayout::EtaB];
    float kr = d[MaterialDataLayout::KappaR];
    float kg = d[MaterialDataLayout::KappaG];
    float kb = d[MaterialDataLayout::KappaB];
    eta->values[0] = er;
    eta->values[1] = eg;
    eta->values[2] = eb;
    eta->values[3] = (er + eg + eb) / 3.0f;
    kappa->values[0] = kr;
    kappa->values[1] = kg;
    kappa->values[2] = kb;
    kappa->values[3] = (kr + kg + kb) / 3.0f;
    *roughness = d[MaterialDataLayout::Roughness];
    *roughness = (*roughness < 0.001f) ? 0.001f : *roughness;
}

/// 从材质描述符获取导体微表面反射参数（含各向异性）
/// anisotropy: 0=各向同性, 0.9=强各向异性 (Disney/Burley: alphaX/alphaY 从 roughness+anisotropy 推导)
CUDA_DEVICE_FUNCTION CUDA_INLINE void getMicrofacetReflectionParamsAniso(
    const SurfaceMaterialDescriptor& matDesc,
    SampledSpectrum* eta,
    SampledSpectrum* kappa,
    float* roughness,
    float* anisotropy) {
    getMicrofacetReflectionParams(matDesc, eta, kappa, roughness);
    const float* d = getMaterialDataAsFloats(matDesc);
    *anisotropy = d[MaterialDataLayout::Anisotropy];
    *anisotropy = ::vlr::vlr_max(0.0f, ::vlr::vlr_min(0.999f, *anisotropy));
}

/// 从材质描述符获取微表面散射参数（IOR + roughness）
/// 用于 MicrofacetScattering（电介质，反射+折射）
CUDA_DEVICE_FUNCTION CUDA_INLINE void getMicrofacetScatteringParams(
    const SurfaceMaterialDescriptor& matDesc,
    float* ior,
    float* roughness,
    SampledSpectrum* coeff = nullptr) {
    const float* d = getMaterialDataAsFloats(matDesc);
    *ior = d[MaterialDataLayout::IOR];
    *ior = (*ior < 1.0f) ? 1.0f : *ior;
    *roughness = d[MaterialDataLayout::Roughness];
    *roughness = (*roughness < 0.001f) ? 0.001f : *roughness;
    // 读取透射系数
    if (coeff) {
        float r = d[MaterialDataLayout::AlbedoR];
        float g = d[MaterialDataLayout::AlbedoG];
        float b = d[MaterialDataLayout::AlbedoB];
        coeff->values[0] = r;
        coeff->values[1] = g;
        coeff->values[2] = b;
    }
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

/// 获取有效 FresnelBlend 参数（优先纹理化参数）
CUDA_DEVICE_FUNCTION CUDA_INLINE void getEffectiveFresnelBlendParams(
    const SurfaceMaterialDescriptor& matDesc,
    const PathTexturedMaterialParams* texParams,
    SampledSpectrum* diffuseReflectance,
    SampledSpectrum* specularReflectance,
    float* roughness) {
    if (texParams != nullptr) {
        diffuseReflectance->values[0] = texParams->baseColorR;
        diffuseReflectance->values[1] = texParams->baseColorG;
        diffuseReflectance->values[2] = texParams->baseColorB;
        diffuseReflectance->values[3] = (texParams->baseColorR + texParams->baseColorG + texParams->baseColorB) / 3.0f;
        *specularReflectance = *diffuseReflectance;
        *roughness = (texParams->flags & PathTexturedFlags::HasRoughnessTex) ? texParams->roughness : getMaterialDataAsFloats(matDesc)[MaterialDataLayout::Roughness];
    } else {
        getFresnelBlendParams(matDesc, diffuseReflectance, specularReflectance, roughness);
        return;
    }
    *roughness = ::vlr::vlr_max(*roughness, 0.001f);
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

/// 获取有效 UE4 参数（优先纹理化参数）
CUDA_DEVICE_FUNCTION CUDA_INLINE void getEffectiveUE4Params(
    const SurfaceMaterialDescriptor& matDesc,
    const PathTexturedMaterialParams* texParams,
    SampledSpectrum* baseColor,
    float* metallic,
    float* roughness) {
    if (texParams != nullptr) {
        baseColor->values[0] = texParams->baseColorR;
        baseColor->values[1] = texParams->baseColorG;
        baseColor->values[2] = texParams->baseColorB;
        baseColor->values[3] = (texParams->baseColorR + texParams->baseColorG + texParams->baseColorB) / 3.0f;
        *metallic = (texParams->flags & PathTexturedFlags::HasMetallicTex) ? texParams->metallic : getMaterialDataAsFloats(matDesc)[MaterialDataLayout::Metallic];
        *roughness = (texParams->flags & PathTexturedFlags::HasRoughnessTex) ? texParams->roughness : getMaterialDataAsFloats(matDesc)[MaterialDataLayout::Roughness];
        *metallic = ::vlr::vlr_max(0.0f, ::vlr::vlr_min(1.0f, *metallic));
        *roughness = ::vlr::vlr_max(0.001f, *roughness);
    } else {
        getUE4Params(matDesc, baseColor, metallic, roughness);
    }
}

/// 从材质描述符获取 Disney Principled BRDF 参数
/// baseColor=AlbedoR/G/B, roughness=Roughness, 其余在 Disney_* 槽位
CUDA_DEVICE_FUNCTION CUDA_INLINE void getDisneyParams(
    const SurfaceMaterialDescriptor& matDesc,
    SampledSpectrum* baseColor,
    float* metallic, float* subsurface, float* specular, float* roughness,
    float* specularTint, float* anisotropic, float* sheen, float* sheenTint,
    float* clearcoat, float* clearcoatGloss) {
    const float* d = getMaterialDataAsFloats(matDesc);
    float r = d[MaterialDataLayout::AlbedoR];
    float g = d[MaterialDataLayout::AlbedoG];
    float b = d[MaterialDataLayout::AlbedoB];
    baseColor->values[0] = r;
    baseColor->values[1] = g;
    baseColor->values[2] = b;
    baseColor->values[3] = (r + g + b) / 3.0f;
    *roughness = ::vlr::vlr_max(0.001f, d[MaterialDataLayout::Roughness]);
    *metallic = ::vlr::vlr_max(0.0f, ::vlr::vlr_min(1.0f, d[MaterialDataLayout::Disney_Metallic]));
    *subsurface = ::vlr::vlr_max(0.0f, ::vlr::vlr_min(1.0f, d[MaterialDataLayout::Disney_Subsurface]));
    *specular = ::vlr::vlr_max(0.0f, ::vlr::vlr_min(1.0f, d[MaterialDataLayout::Disney_Specular]));
    *specularTint = ::vlr::vlr_max(0.0f, ::vlr::vlr_min(1.0f, d[MaterialDataLayout::Disney_SpecularTint]));
    *anisotropic = ::vlr::vlr_max(0.0f, ::vlr::vlr_min(0.999f, d[MaterialDataLayout::Disney_Anisotropic]));
    *sheen = ::vlr::vlr_max(0.0f, ::vlr::vlr_min(1.0f, d[MaterialDataLayout::Disney_Sheen]));
    *sheenTint = ::vlr::vlr_max(0.0f, ::vlr::vlr_min(1.0f, d[MaterialDataLayout::Disney_SheenTint]));
    *clearcoat = ::vlr::vlr_max(0.0f, ::vlr::vlr_min(1.0f, d[MaterialDataLayout::Disney_Clearcoat]));
    *clearcoatGloss = ::vlr::vlr_max(0.0f, ::vlr::vlr_min(1.0f, d[MaterialDataLayout::Disney_ClearcoatGloss]));
}

/// 从材质描述符获取 MultiSurface 参数（2-4 层）
CUDA_DEVICE_FUNCTION CUDA_INLINE void getMultiSurfaceParams(
    const SurfaceMaterialDescriptor& matDesc,
    int* numLayers,
    BSDFType subTypes[4],
    SampledSpectrum subAlbedos[4],
    float subRoughness[4],
    float weights[4]) {
    const float* d = getMaterialDataAsFloats(matDesc);
    int n = static_cast<int>(d[MaterialDataLayout::MultiSurface_NumLayers]);
    *numLayers = (n >= 2 && n <= 4) ? n : 2;

    auto loadSub = [&](int i, int baseType, int baseR, int baseG, int baseB, int baseRough) {
        subTypes[i] = static_cast<BSDFType>(static_cast<uint32_t>(d[baseType]) % NumBSDFTypes);
        subAlbedos[i].values[0] = d[baseR];
        subAlbedos[i].values[1] = d[baseG];
        subAlbedos[i].values[2] = d[baseB];
        subAlbedos[i].values[3] = (d[baseR] + d[baseG] + d[baseB]) / 3.0f;
        subRoughness[i] = ::vlr::vlr_max(0.001f, d[baseRough]);
    };
    loadSub(0, MaterialDataLayout::SubMaterial0_BSDFType,
            MaterialDataLayout::SubMaterial0_AlbedoR, MaterialDataLayout::SubMaterial0_AlbedoG,
            MaterialDataLayout::SubMaterial0_AlbedoB, MaterialDataLayout::SubMaterial0_Roughness);
    loadSub(1, MaterialDataLayout::SubMaterial1_BSDFType,
            MaterialDataLayout::SubMaterial1_AlbedoR, MaterialDataLayout::SubMaterial1_AlbedoG,
            MaterialDataLayout::SubMaterial1_AlbedoB, MaterialDataLayout::SubMaterial1_Roughness);
    loadSub(2, MaterialDataLayout::SubMaterial2_BSDFType,
            MaterialDataLayout::SubMaterial2_AlbedoR, MaterialDataLayout::SubMaterial2_AlbedoG,
            MaterialDataLayout::SubMaterial2_AlbedoB, MaterialDataLayout::SubMaterial2_Roughness);
    loadSub(3, MaterialDataLayout::SubMaterial3_BSDFType,
            MaterialDataLayout::SubMaterial3_AlbedoR, MaterialDataLayout::SubMaterial3_AlbedoG,
            MaterialDataLayout::SubMaterial3_AlbedoB, MaterialDataLayout::SubMaterial3_Roughness);

    float w[4] = {
        ::vlr::vlr_max(0.0f, d[MaterialDataLayout::MultiSurface_Weight0]),
        ::vlr::vlr_max(0.0f, d[MaterialDataLayout::MultiSurface_Weight1]),
        ::vlr::vlr_max(0.0f, d[MaterialDataLayout::MultiSurface_Weight2]),
        ::vlr::vlr_max(0.0f, d[MaterialDataLayout::MultiSurface_Weight3])
    };
    float sum = 0.0f;
    for (int i = 0; i < *numLayers; ++i) sum += w[i];
    if (sum < 1e-6f) sum = 1.0f;
    for (int i = 0; i < 4; ++i)
        weights[i] = (i < *numLayers) ? (w[i] / sum) : 0.0f;
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
    const PathTexturedMaterialParams* texturedParams;  ///< 纹理化材质参数覆盖（可选）

    CUDA_DEVICE_FUNCTION CUDA_INLINE BSDFContext() : singleWlSelected(false), texturedParams(nullptr) {}

    CUDA_DEVICE_FUNCTION CUDA_INLINE BSDFContext(
        const SurfaceMaterialDescriptor& desc,
        const SurfacePoint& sp,
        const WavelengthSamples& wavelengthSamples,
        bool singleWl = false,
        const PathTexturedMaterialParams* texParams = nullptr)
        : matDesc(&desc)
        , surfPt(&sp)
        , wls(&wavelengthSamples)
        , geomNormalLocal(sp.shadingFrame.toLocal(sp.geometricNormal))
        , singleWlSelected(singleWl)
        , texturedParams(texParams)
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
    case BSDFType_LambertianScattering:
        return MaterialCategory_Transmissive;
    case BSDFType_GGX:
    case BSDFType_MicrofacetReflection:
    case BSDFType_UE4BRDF:
    case BSDFType_FrostbiteBRDF:
    case BSDFType_DisneyBRDF:
        return MaterialCategory_Glossy;
    case BSDFType_Specular:
    case BSDFType_SpecularTransmission:
        return MaterialCategory_Specular;
    case BSDFType_GGXTransmission:
    case BSDFType_MicrofacetScattering:
        return MaterialCategory_Transmissive;
    case BSDFType_MixedBSDF:
    case BSDFType_MultiSurface:
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
        || type == BSDFType_MicrofacetReflection || type == BSDFType_MicrofacetScattering
        || type == BSDFType_LambertianScattering || type == BSDFType_FresnelBlend || type == BSDFType_UE4BRDF
        || type == BSDFType_FrostbiteBRDF || type == BSDFType_DisneyBRDF || type == BSDFType_GGXTransmission
        || type == BSDFType_MixedBSDF || type == BSDFType_MultiSurface;
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
    case BSDFType_MicrofacetReflection:
    case BSDFType_UE4BRDF:
    case BSDFType_FrostbiteBRDF:
    case BSDFType_DisneyBRDF:
        return DirectionType::HighFreq() | DirectionType::Reflection();
    case BSDFType_Specular:
        return DirectionType::Delta0D() | DirectionType::Reflection();
    case BSDFType_SpecularTransmission:
        return DirectionType::Delta0D() | DirectionType::Transmission();
    case BSDFType_LambertianScattering:
    case BSDFType_GGXTransmission:
    case BSDFType_MicrofacetScattering:
        return DirectionType::HighFreq() | DirectionType::Transmission();
    case BSDFType_MixedBSDF:
    case BSDFType_MultiSurface:
        return DirectionType::HighFreq() | DirectionType::Reflection();
    default:
        return DirectionType();
    }
}

/// 检查 BSDF 类型是否为色散材质（透射且 dispersionStrength > 0）
/// 仅当材质实际启用色散时才返回 true，否则会错误触发单波长选择导致 pdf 除以 4、throughput 爆炸
CUDA_DEVICE_FUNCTION CUDA_INLINE bool isDispersiveBSDFType(
    BSDFType type,
    const SurfaceMaterialDescriptor& matDesc) {
    if (type != BSDFType_SpecularTransmission && type != BSDFType_GGXTransmission)
        return false;
    float ior, dispersionStrength;
    getTransmissionParams(matDesc, &ior, &dispersionStrength);
    return dispersionStrength > 0.0f;
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
