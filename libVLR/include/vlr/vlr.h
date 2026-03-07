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

// C 兼容：结果码与渲染器类型
#ifdef __cplusplus
typedef int32_t VLRResult;
#else
typedef int32_t VLRResult;
#endif
typedef uint32_t VLRRenderer;
#define VLRResult_Success 0
#define VLRResult_InvalidArgument (-1)
#define VLRResult_OutOfMemory (-2)
#define VLRResult_NotImplemented (-3)
#define VLRResult_InternalError (-4)
#define VLRResult_CUDAError (-5)
#define VLRResult_OptiXError (-6)
#define VLRRenderer_PathTracing 0
#define VLRRenderer_LightTracing 1
#define VLRRenderer_BidirectionalPathTracing 2
#define VLRRenderer_WavefrontPathTracing 3

// ============================================================================
// 前置声明：不透明句柄（使用不同结构体名避免 C++ 歧义）
// ============================================================================

struct VLRContextImpl;
struct VLRSceneImpl;
struct VLRTriangleMeshImpl;
struct VLRMaterialImpl;
struct VLRInstanceImpl;

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

/// 添加区域光
/// @param scene 所属场景
/// @param instance 发光几何的实例句柄（其材质需设置 emissionColor）
/// @return VLRResult_Success 或错误码
VLR_API VLRResult vlrAddAreaLight(VLRScene scene, VLRInstance instance);

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


// ============================================================================
// 版本与工具
// ============================================================================

/// 获取 VLR 版本信息
VLR_API void vlrGetVersion(uint32_t* major, uint32_t* minor, uint32_t* patch);

#ifdef __cplusplus
}
#endif
