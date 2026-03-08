// ============================================================================
// VLR 公共 C API 头文件
//
// 本文件声明 VLR 渲染库的 C 风格公共接口。
// 使用不透明句柄实现 ABI 稳定性，便于其他语言绑定。
//
// 作者：VLR 开发团队
// 创建日期：2026-03-07
// 环境：CUDA 13.1, OptiX 8.0.0, VS2022
// ============================================================================

#pragma once

#include <stdint.h>

// Windows DLL 导出/导入
#ifdef _WIN32
#  ifdef VLR_EXPORTS
#    define VLR_API __declspec(dllexport)
#  else
#    define VLR_API __declspec(dllimport)
#  endif
#else
#  define VLR_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
#include "public_types.h"
extern "C" {
#endif

// C compatible: result codes and renderer types
typedef int32_t VLRResult;
#define VLRResult_Success 0
#define VLRResult_InvalidArgument (-1)
#define VLRResult_OutOfMemory (-2)
#define VLRResult_NotImplemented (-3)
#define VLRResult_InternalError (-4)
#define VLRResult_CUDAError (-5)
#define VLRResult_OptiXError (-6)

#ifndef __cplusplus
typedef uint32_t VLRRenderer;
#define VLRRenderer_PathTracing 0
#define VLRRenderer_LightTracing 1
#define VLRRenderer_BidirectionalPathTracing 2
#define VLRRenderer_WavefrontPathTracing 3
#endif

// ============================================================================
// 前置声明：不透明句柄（使用不同结构体名避免 C++ 歧义）
// ============================================================================

struct VLRContextImpl;
struct VLRSceneImpl;
struct VLRTriangleMeshImpl;
struct VLRMaterialImpl;
struct VLRInstanceImpl;
struct VLRTextureImpl;

/// 上下文句柄：管理渲染资源与管线
typedef struct VLRContextImpl* VLRContext;

/// 场景句柄：管理几何体、材质、实例、相机
typedef struct VLRSceneImpl* VLRScene;

/// 网格句柄：三角形网格几何体
typedef struct VLRTriangleMeshImpl* VLRTriangleMesh;

/// 材质句柄：表面材质
typedef struct VLRMaterialImpl* VLRMaterial;

/// 实例句柄：场景中的几何实例（网格+变换）
typedef struct VLRInstanceImpl* VLRInstance;

/// 纹理句柄：2D 纹理（从文件或内存创建）
typedef struct VLRTextureImpl* VLRTexture;


// ============================================================================
// 相机参数（用于 vlrSetCamera）
// ============================================================================

/// 相机参数结构体
typedef struct VLRCameraParams {
    float position[3];      ///< 相机位置 (x, y, z)
    float direction[3];     ///< 观察方向 (单位向量)
    float up[3];            ///< 上方向 (单位向量)
    float fovY;             ///< 垂直视场角（弧度）
    float aspect;           ///< 宽高比
    float lensRadius;       ///< 光圈半径（景深，0 表示针孔）
    float focusDistance;    ///< 焦平面距离
    float focalLength;      ///< 焦距（0 表示从 FOV 推导）
    uint32_t cameraType;    ///< VLRCameraType：0=透视，1=等距柱状
} VLRCameraParams;


// ============================================================================
// 核心 API
// ============================================================================

/// 创建渲染上下文
/// @param cudaStream CUDA 流（可为 0 使用默认流）
/// @param enableLogging 是否启用 OptiX 日志
/// @param outContext 输出上下文句柄
/// @return VLRResult_Success 或错误码
VLR_API VLRResult vlrCreateContext(void* cudaStream, int enableLogging, VLRContext* outContext);

/// 销毁渲染上下文
/// @param context 上下文句柄（可为 NULL，无操作）
VLR_API void vlrDestroyContext(VLRContext context);

/// 创建场景
/// @param context 所属上下文
/// @param outScene 输出场景句柄
/// @return VLRResult_Success 或错误码
VLR_API VLRResult vlrCreateScene(VLRContext context, VLRScene* outScene);

/// 销毁场景
/// @param scene 场景句柄（可为 NULL，无操作）
VLR_API void vlrDestroyScene(VLRScene scene);

/// 创建三角形网格
/// @param scene 所属场景
/// @param vertices 顶点位置数组 [x,y,z, x,y,z, ...]（3*numVertices 个 float）
/// @param numVertices 顶点数量
/// @param indices 三角形索引数组 [i0,i1,i2, i0,i1,i2, ...]（3*numTriangles 个 uint32_t）
/// @param numTriangles 三角形数量
/// @param material 材质句柄（需先通过 vlrCreateMaterial 创建）
/// @param outMesh 输出网格句柄
/// @return VLRResult_Success 或错误码
VLR_API VLRResult vlrCreateTriangleMesh(
    VLRScene scene,
    const float* vertices,
    uint32_t numVertices,
    const uint32_t* indices,
    uint32_t numTriangles,
    VLRMaterial material,
    VLRTriangleMesh* outMesh);

/// 销毁三角形网格
/// @param mesh 网格句柄（可为 NULL，无操作）
VLR_API void vlrDestroyTriangleMesh(VLRTriangleMesh mesh);

/// 创建材质
/// @param scene 所属场景
/// @param materialType 材质类型（VLRMaterialType，0=Matte 漫反射）
/// @param baseColor 基础颜色 RGB [0..1]（3 个 float，可为 NULL 使用默认灰）
/// @param emissionColor 发光颜色 RGB [0..1]（3 个 float，可为 NULL 表示不发光）
/// @param outMaterial 输出材质句柄
/// @return VLRResult_Success 或错误码
VLR_API VLRResult vlrCreateMaterial(
    VLRScene scene,
    uint32_t materialType,
    const float* baseColor,
    const float* emissionColor,
    VLRMaterial* outMaterial);

/// 销毁材质
/// @param material 材质句柄（可为 NULL，无操作）
VLR_API void vlrDestroyMaterial(VLRMaterial material);

/// 创建扩展材质（支持粗糙度、金属度、折射率等参数）
/// @param scene 所属场景
/// @param materialType 材质类型（0=Matte, 3=Specular, 4=SpecularTransmission）
/// @param baseColor 基础颜色 RGB [0..1]（3 个 float）
/// @param roughness 粗糙度 [0..1]（0=完美镜面，1=完全粗糙）
/// @param metallic 金属度 [0..1]（0=电介质，1=金属）
/// @param ior 折射率（透射材质，如玻璃 1.5）
/// @param emissionColor 发光颜色 RGB [0..∞]（3 个 float，可为 NULL）
/// @param outMaterial 输出材质句柄
/// @return VLRResult_Success 或错误码
VLR_API VLRResult vlrCreateMaterialEx(
    VLRScene scene,
    uint32_t materialType,
    const float baseColor[3],
    float roughness,
    float metallic,
    float ior,
    const float* emissionColor,
    VLRMaterial* outMaterial);

/// 创建导体微表面反射材质（MicrofacetReflection）
/// @param scene 所属场景
/// @param eta 折射率实部 RGB（如铝 [1.28, 0.94, 0.57]）
/// @param kappa 折射率虚部 RGB（如铝 [7.30, 6.33, 5.17]）
/// @param roughness 粗糙度 [0..1]（0=完美镜面，1=完全粗糙）
/// @param outMaterial 输出材质句柄
VLR_API VLRResult vlrCreateMaterialConductor(
    VLRScene scene,
    const float eta[3],
    const float kappa[3],
    float roughness,
    VLRMaterial* outMaterial);

/// 创建各向异性导体微表面反射材质（MicrofacetReflection + Anisotropic GGX）
/// @param scene 所属场景
/// @param eta 折射率实部 RGB
/// @param kappa 折射率虚部 RGB
/// @param roughness 粗糙度 [0..1]
/// @param anisotropy 各向异性强度 [0..0.9]（0=各向同性，0.9=强各向异性，拉丝金属效果）
/// @param outMaterial 输出材质句柄
VLR_API VLRResult vlrCreateMaterialConductorAniso(
    VLRScene scene,
    const float eta[3],
    const float kappa[3],
    float roughness,
    float anisotropy,
    VLRMaterial* outMaterial);

/// 创建微表面散射材质（电介质，反射+折射）
/// @param scene 所属场景
/// @param ior 折射率（如玻璃 1.5）
/// @param roughness 粗糙度 [0..1]（0=完美镜面，1=完全粗糙）
/// @param outMaterial 输出材质句柄
/// @return VLRResult_Success 或错误码
VLR_API VLRResult vlrCreateMaterialMicrofacetScattering(
    VLRScene scene,
    float ior,
    float roughness,
    VLRMaterial* outMaterial);

/// 创建 LambertianScattering 次表面散射材质（双向 Lambert）
/// 允许光从表面一侧进入、另一侧出射，模拟薄片/皮肤等半透明效果
/// @param scene 所属场景
/// @param albedo 反照率 RGB [0..1]（3 个 float）
/// @param outMaterial 输出材质句柄
/// @return VLRResult_Success 或错误码
VLR_API VLRResult vlrCreateMaterialLambertianScattering(
    VLRScene scene,
    const float albedo[3],
    VLRMaterial* outMaterial);

/// 创建 Disney Principled BRDF 材质（Burley 2012）
/// @param scene 所属场景
/// @param baseColor 基础颜色 RGB [0..1]
/// @param metallic 金属度 [0..1]
/// @param subsurface 次表面散射强度 [0..1]
/// @param specular 镜面反射强度 [0..1]
/// @param roughness 粗糙度 [0..1]
/// @param specularTint 镜面色调 [0..1]
/// @param anisotropic 各向异性 [0..1]
/// @param sheen 织物光泽 [0..1]
/// @param sheenTint 光泽色调 [0..1]
/// @param clearcoat 清漆层 [0..1]
/// @param clearcoatGloss 清漆光泽度 [0..1]
/// @param outMaterial 输出材质句柄
VLR_API VLRResult vlrCreateMaterialDisney(
    VLRScene scene,
    const float baseColor[3],
    float metallic,
    float subsurface,
    float specular,
    float roughness,
    float specularTint,
    float anisotropic,
    float sheen,
    float sheenTint,
    float clearcoat,
    float clearcoatGloss,
    VLRMaterial* outMaterial);

/// 创建棋盘格材质（用于地板等）
/// @param scene 所属场景
/// @param color0 第一种颜色 RGB [0..1]（如黑色）
/// @param color1 第二种颜色 RGB [0..1]（如白色）
/// @param gridSize 棋盘格密度（默认 8 表示 8x8）
/// @param outMaterial 输出材质句柄
VLR_API VLRResult vlrCreateMaterialCheckerboard(
    VLRScene scene,
    const float color0[3],
    const float color1[3],
    uint32_t gridSize,
    float extent,
    VLRMaterial* outMaterial);

/// 创建多表面材质（2-4 层子材质混合）
/// @param scene 所属场景
/// @param numLayers 层数 (2-4)
/// @param subBSDFTypes 子材质类型数组 [numLayers]，BSDFType: 0=Lambert, 2=GGX, 5=Specular 等
/// @param subAlbedos 每个子材质的颜色 [numLayers][3]，subAlbedos[i] 指向 RGB
/// @param subRoughness 每个子材质的粗糙度 [numLayers]
/// @param weights 混合权重 [numLayers]，运行时归一化
/// @param outMaterial 输出材质句柄
VLR_API VLRResult vlrCreateMaterialMultiSurface(
    VLRScene scene,
    int numLayers,
    const uint32_t* subBSDFTypes,
    const float* const* subAlbedos,
    const float* subRoughness,
    const float* weights,
    VLRMaterial* outMaterial);

/// 创建实例（将网格放入场景）
/// @param scene 所属场景
/// @param mesh 网格句柄
/// @param position 位置 [x, y, z]
/// @param scale 缩放 [sx, sy, sz]
/// @param rotationAxis 旋转轴 [x, y, z]（单位向量）
/// @param rotationAngle 旋转角度（弧度）
/// @param outInstance 输出实例句柄
/// @return VLRResult_Success 或错误码
VLR_API VLRResult vlrCreateInstance(
    VLRScene scene,
    VLRTriangleMesh mesh,
    const float position[3],
    const float scale[3],
    const float rotationAxis[3],
    float rotationAngle,
    VLRInstance* outInstance);

/// 销毁实例
/// @param instance 实例句柄（可为 NULL，无操作）
VLR_API void vlrDestroyInstance(VLRInstance instance);

// ============================================================================
// 纹理 API
// ============================================================================

/// 从图像文件创建 2D 纹理（支持 PNG, JPG, EXR, HDR）
/// @param context 所属上下文
/// @param imagePath 图像文件路径
/// @param outTexture 输出纹理句柄
/// @return VLRResult_Success 或错误码
VLR_API VLRResult vlrCreateTexture2D(
    VLRContext context,
    const char* imagePath,
    VLRTexture* outTexture);

/// 从内存创建 2D 纹理
/// @param context 所属上下文
/// @param data 像素数据指针
/// @param width 纹理宽度
/// @param height 纹理高度
/// @param format 像素格式：0=RGBA8, 1=RGB32F, 2=RGBA32F
/// @param outTexture 输出纹理句柄
/// @return VLRResult_Success 或错误码
VLR_API VLRResult vlrCreateTexture2DFromMemory(
    VLRContext context,
    const void* data,
    uint32_t width,
    uint32_t height,
    uint32_t format,
    VLRTexture* outTexture);

/// 销毁纹理
/// @param texture 纹理句柄（可为 NULL，无操作）
VLR_API VLRResult vlrDestroyTexture(VLRTexture texture);

/// 设置纹理滤波模式
/// @param texture 纹理句柄
/// @param filterMode 0=Nearest, 1=Linear
/// @return VLRResult_Success 或错误码
VLR_API VLRResult vlrSetTextureFilterMode(
    VLRTexture texture,
    uint32_t filterMode);

/// 设置纹理环绕模式
/// @param texture 纹理句柄
/// @param wrapU U 方向：0=Repeat, 1=Clamp
/// @param wrapV V 方向：0=Repeat, 1=Clamp
/// @return VLRResult_Success 或错误码
VLR_API VLRResult vlrSetTextureWrapMode(
    VLRTexture texture,
    uint32_t wrapU,
    uint32_t wrapV);

// ============================================================================
// 材质纹理绑定 API
// ============================================================================

/// 设置材质基础色纹理
/// @param material 材质句柄
/// @param texture 纹理句柄（可为 NULL 清除绑定）
/// @return VLRResult_Success 或错误码
VLR_API VLRResult vlrSetMaterialBaseColorTexture(
    VLRMaterial material,
    VLRTexture texture);

/// 设置材质粗糙度纹理
VLR_API VLRResult vlrSetMaterialRoughnessTexture(
    VLRMaterial material,
    VLRTexture texture);

/// 设置材质金属度纹理
VLR_API VLRResult vlrSetMaterialMetallicTexture(
    VLRMaterial material,
    VLRTexture texture);

/// 设置材质法线贴图
/// @param material 材质句柄
/// @param texture 法线贴图纹理（可为 NULL 清除绑定）
/// @param normalScale 法线强度，默认 1.0
VLR_API VLRResult vlrSetMaterialNormalTexture(
    VLRMaterial material,
    VLRTexture texture,
    float normalScale);

/// 设置材质纹理坐标变换
/// @param material 材质句柄
/// @param scaleU U 方向缩放（默认 1.0）
/// @param scaleV V 方向缩放（默认 1.0）
/// @param offsetU U 方向偏移（默认 0）
/// @param offsetV V 方向偏移（默认 0）
VLR_API VLRResult vlrSetMaterialTextureTransform(
    VLRMaterial material,
    float scaleU,
    float scaleV,
    float offsetU,
    float offsetV);

/// 添加区域光
/// @param scene 所属场景
/// @param instance 发光几何的实例句柄（其材质需设置 emissionColor）
/// @return VLRResult_Success 或错误码
VLR_API VLRResult vlrAddAreaLight(VLRScene scene, VLRInstance instance);

/// 设置环境光
/// @param scene 所属场景
/// @param color 环境光颜色 RGB [0..∞]（3 个 float，线性空间）
/// @return VLRResult_Success 或错误码
VLR_API VLRResult vlrSetEnvironmentLight(VLRScene scene, const float color[3]);

/// 从图像文件设置环境光（IBL）
/// @param scene 所属场景
/// @param imagePath HDR 图像文件路径（.exr 或 .hdr）
/// @param rotation 环境旋转角度（弧度，0 表示不旋转）
/// @return VLRResult_Success 或错误码
VLR_API VLRResult vlrSetEnvironmentLightFromImage(VLRScene scene, const char* imagePath, float rotation);

/// 添加点光源
/// @param scene 所属场景
/// @param position 光源位置（世界坐标）
/// @param intensity 辐射强度 RGB [0..∞]（W/sr）
/// @return VLRResult_Success 或错误码
VLR_API VLRResult vlrAddPointLight(
    VLRScene scene,
    const float position[3],
    const float intensity[3]);

/// 添加方向光（平行光，如太阳光）
/// @param scene 所属场景
/// @param direction 光线方向（归一化，指向场景）
/// @param radiance 辐射度 RGB [0..∞]（W/(m²·sr)）
/// @return VLRResult_Success 或错误码
VLR_API VLRResult vlrAddDirectionalLight(
    VLRScene scene,
    const float direction[3],
    const float radiance[3]);

/// 设置场景相机
/// @param scene 场景句柄
/// @param params 相机参数
/// @return VLRResult_Success 或错误码
VLR_API VLRResult vlrSetCamera(VLRScene scene, const VLRCameraParams* params);

/// 执行渲染
/// @param context 上下文句柄
/// @param scene 场景句柄
/// @param width 图像宽度
/// @param height 图像高度
/// @param numSamples 每像素采样数
/// @param renderer 渲染器类型（VLRRenderer）
/// @return VLRResult_Success 或错误码
VLR_API VLRResult vlrRender(
    VLRContext context,
    VLRScene scene,
    uint32_t width,
    uint32_t height,
    uint32_t numSamples,
    uint32_t renderer);

/// 获取输出缓冲区指针
/// @param context 上下文句柄
/// @return 设备端累积缓冲区指针（SpectrumStorage 数组，需用户 cudaMemcpy 到主机）
///         或 NULL（未初始化/渲染后）
/// @note 缓冲区大小为 width*height*sizeof(SpectrumStorage)，在 vlrRender 之后有效
VLR_API void* vlrGetOutputBuffer(VLRContext context);


// ============================================================================
// Wavefront 优化配置
// ============================================================================

/// 设置 Wavefront 路径排序（按材质类别）
/// @param context 上下文句柄
/// @param enable 是否启用排序（1=启用，0=禁用）
/// @return VLRResult_Success 或错误码
VLR_API VLRResult vlrContextSetWavefrontPathSorting(VLRContext context, int enable);

/// 设置 Wavefront Stream Compaction（移除已终止路径）
/// @param context 上下文句柄
/// @param enable 是否启用压缩（1=启用，0=禁用）
/// @return VLRResult_Success 或错误码
VLR_API VLRResult vlrContextSetWavefrontStreamCompaction(VLRContext context, int enable);

/// 从 INI 文件加载性能配置
/// @param context 上下文句柄
/// @param perfConfigFile INI 配置文件路径（仅包含 Optimization、KernelConfig、EarlyTermination 等性能相关节）
/// @return VLRResult_Success 成功，VLRResult_InvalidArgument 参数无效或文件加载失败
/// @note 仅加载性能优化参数，不涉及场景参数（分辨率、采样数等）
VLR_API VLRResult vlrLoadPerformanceConfig(VLRContext context, const char* perfConfigFile);

/// 设置降噪器配置
/// @param context 渲染上下文句柄
/// @param enabled 是否启用降噪
/// @param useAlbedo 是否使用 Albedo guide layer
/// @param useNormal 是否使用 Normal guide layer
/// @param hdrIntensity HDR 强度参数
VLR_API VLRResult vlrSetDenoiserConfig(
    VLRContext context, 
    bool enabled,
    bool useAlbedo,
    bool useNormal,
    float hdrIntensity);


// ============================================================================
// 调试与可视化
// ============================================================================

/// 设置调试渲染模式
/// @param context 渲染上下文句柄
/// @param mode 调试模式（参见 VLRDebugMode 枚举）
/// @return VLRResult_Success 或错误码
VLR_API VLRResult vlrSetDebugMode(
    VLRContext context,
    uint32_t mode);

/// 获取当前调试渲染模式
/// @param context 渲染上下文句柄
/// @param outMode 输出当前模式
/// @return VLRResult_Success 或错误码
VLR_API VLRResult vlrGetDebugMode(
    VLRContext context,
    uint32_t* outMode);

/// 设置探针像素（用于单像素调试）
/// @param context 渲染上下文句柄
/// @param x 像素 X 坐标（-1 表示禁用探针）
/// @param y 像素 Y 坐标
/// @return VLRResult_Success 或错误码
VLR_API VLRResult vlrSetProbePixel(
    VLRContext context,
    int32_t x,
    int32_t y);


// ============================================================================
// 版本与工具
// ============================================================================

/// 获取 VLR 版本信息
VLR_API void vlrGetVersion(uint32_t* major, uint32_t* minor, uint32_t* patch);

#ifdef __cplusplus
}
#endif
