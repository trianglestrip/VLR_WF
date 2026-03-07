// ============================================================================
// VLR 光源类型定义
//
// 本文件定义了波前路径追踪 SampleLights kernel 所需的光源类型和描述符。
// 支持区域光、点光源和环境光三种光源类型。
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
// 1. 光源类型枚举
// ============================================================================

/// 光源类型：用于区分不同光源的采样和评估逻辑
enum LightType : uint32_t {
    LightType_Area = 0,        // 区域光（基于几何的表面光源）
    LightType_Point,           // 点光源（Delta 光源）
    LightType_Environment,     // 环境光（IBL，无穷远球面）
    NumLightTypes
};


// ============================================================================
// 2. 光源描述符
// ============================================================================

/// 光源描述符：统一描述任意类型的光源
/// 用于 selectLight 的返回值，供后续 sampleLight、evaluateLightEmission、computeLightPDF 使用
struct LightDescriptor {
    LightType type;                // 光源类型
    uint32_t instIndex;            // 实例索引（指向 instBuffer）
    uint32_t geomInstIndex;        // 几何实例索引（指向 geomInstBuffer）
    
    // 对于环境光：通常 instIndex 为 envLightInstIndex，geomInstIndex 为实例内的首图元
    // 对于区域光：instIndex + geomInstIndex 唯一确定一个发光几何
    // 对于点光源：geomInst 的 geomType 为 GeometryType_Point
    
    CUDA_DEVICE_FUNCTION CUDA_INLINE bool isValid() const {
        return type < NumLightTypes;
    }
    
    CUDA_DEVICE_FUNCTION CUDA_INLINE bool isDelta() const {
        return type == LightType_Point;
    }
    
    CUDA_DEVICE_FUNCTION CUDA_INLINE bool isInfinity() const {
        return type == LightType_Environment;
    }
};


// ============================================================================
// 3. 区域光（Area Light）
// ============================================================================

/// 区域光：基于几何表面的光源
/// 采样在三角形或其它几何表面上进行，具有非零面积
/// 对应 GeometryType_TriangleMesh 等几何类型
struct AreaLight {
    uint32_t instIndex;            // 实例索引
    uint32_t geomInstIndex;        // 几何实例索引
    uint32_t primIndex;            // 图元索引（三角形等）
    float u, v;                    // 表面参数化坐标（如重心坐标）
    
    // 预计算或缓存的表面点信息（可选，由 sampleLight 填充）
    // 实际使用时通过 SurfacePoint 传递
};


// ============================================================================
// 4. 点光源（Point Light）
// ============================================================================

/// 点光源：Delta 光源，在空间中单点发射
/// 对应 GeometryType_Point，采样时方向确定，无面积 PDF
struct PointLight {
    Point3D position;              // 光源位置
    SampledSpectrum intensity;      // 辐射强度（已含距离衰减的等效值）
    uint32_t instIndex;            // 实例索引（用于材质/EDF 评估）
    uint32_t geomInstIndex;        // 几何实例索引
    
    CUDA_DEVICE_FUNCTION CUDA_INLINE bool isValid() const {
        return geomInstIndex != 0xFFFFFFFF;
    }
};


// ============================================================================
// 5. 环境光（Environment Light）
// ============================================================================

/// 环境光：基于无穷远球面的 IBL
/// 对应 GeometryType_InfiniteSphere，采样在球面参数 (theta, phi) 上进行
struct EnvironmentLight {
    uint32_t instIndex;            // 环境光实例索引（通常为 envLightInstIndex）
    uint32_t geomInstIndex;        // 无穷远球几何实例索引
    float theta;                   // 球面参数：天顶角 [0, pi]
    float phi;                    // 球面参数：方位角 [0, 2*pi]
    
    CUDA_DEVICE_FUNCTION CUDA_INLINE bool isValid() const {
        return geomInstIndex != 0xFFFFFFFF;
    }
};


// ============================================================================
// 6. 光源采样结果
// ============================================================================

/// 光源位置采样结果：sampleLight 的输出
struct LightSampleResult {
    SurfacePoint lightSurfPt;      // 光源上的表面点（世界空间）
    float areaPDF;                 // 面积概率密度（1/面积 或 参数空间 PDF 转换）
    float lightSelectProb;          // 光源选择概率（用于 MIS）
    bool isValid;                  // 采样是否有效
};


/// 光源辐射评估结果：evaluateLightEmission 的输出
struct LightEmissionResult {
    SampledSpectrum Le;            // 辐射度（W/(m^2*sr) 或 W/sr）
    bool isValid;                  // 评估是否有效
};


/// 光源选择结果：selectLight 的输出
struct LightSelectResult {
    LightDescriptor descriptor;   // 选中的光源描述符
    float selectProb;              // 选择该光源的概率
    float uRemapped;               // 重映射的随机数（用于下层采样）
};

} // namespace shared
} // namespace vlr
