// ============================================================================
// VLR 光源公共函数
//
// 本文件定义了 SampleLights kernel 使用的光源采样、评估和 PDF 计算函数。
// 支持区域光、点光源和环境光的统一接口。
//
// 依赖：需在使用前定义 wlp (WavefrontLaunchParameters) 常量
// 用法：在 wavefront_sample_lights.cu 中包含此头文件
//
// 作者：VLR 开发团队
// 创建日期：2026-03-07
// 环境：CUDA 13.1, OptiX 8.0.0, VS2022
// ============================================================================

#pragma once

#include "light_types.h"
#include "wavefront_types.h"
#include "kernel_common.h"
#include <limits>

namespace vlr {
namespace shared {

#if defined(VLR_Device) || defined(OPTIXU_Platform_CodeCompletion)

// ============================================================================
// 辅助：使用 ReferenceFrame 变换 SurfacePoint
// ============================================================================
CUDA_DEVICE_FUNCTION CUDA_INLINE void transformSurfacePoint(
    const ReferenceFrame& frame,
    const SurfacePoint& local,
    SurfacePoint* world) {
    world->position = frame.toWorld(local.position);
    world->geometricNormal = normalize(frame.toWorld(local.geometricNormal));
    world->shadingFrame.x = normalize(frame.toWorld(local.shadingFrame.x));
    world->shadingFrame.y = normalize(frame.toWorld(local.shadingFrame.y));
    world->shadingFrame.z = normalize(frame.toWorld(local.shadingFrame.z));
    world->texCoord = local.texCoord;
    world->atInfinity = local.atInfinity;
}

// ============================================================================
// 1. selectLight - 选择光源
// ============================================================================

/// 根据随机数从场景光源分布中选择一个光源
///
/// @param uLight       [in]   [0,1) 区间的随机数
/// @param result       [out] 选中的光源描述符及选择概率
/// @param wlp          [in]  启动参数（包含 lightInstDist、instBuffer 等）
/// @return 是否成功选中有效光源
CUDA_DEVICE_FUNCTION CUDA_INLINE bool selectLight(
    float uLight,
    LightSelectResult* result,
    const WavefrontLaunchParameters& wlp) {
    
    if (!result) return false;
    
    // 从光源实例分布中采样
    float instProb;
    uint32_t instIndex = wlp.lightInstDist.sample(uLight, &instProb);
    
    // 边界检查
    const Instance& inst = wlp.instBuffer[instIndex];
    
    // 环境光：使用 envLightInstIndex 对应的实例
    // 若选中的是环境光实例，geomInstIndex 取实例内首图元
    uint32_t geomInstIndex;
    float geomInstProb = 1.0f;
    
    if (instIndex == wlp.envLightInstIndex) {
        // 环境光：通常实例只有一个几何
        result->descriptor.type = LightType_Environment;
        geomInstIndex = inst.geomInstIndices[0];
    } else {
        // 区域光或点光源：从实例的几何分布中采样
        // 注意：Instance.lightGeomInstDistribution 可能为 DiscreteDistribution1D 索引
        // 此处使用简化逻辑：若仅有一个几何则直接取用
        if (inst.numGeomInsts == 1) {
            geomInstIndex = inst.geomInstIndices[0];
            geomInstProb = 1.0f;
        } else {
            // 多几何情况：均匀采样（完整实现需 geomInstDistBuffer）
            uint32_t idx = static_cast<uint32_t>(uLight * inst.numGeomInsts) % inst.numGeomInsts;
            geomInstIndex = inst.geomInstIndices[idx];
            geomInstProb = 1.0f / inst.numGeomInsts;
        }
        
        const GeometryInstance& geomInst = wlp.geomInstBuffer[geomInstIndex];
        
        if (geomInst.geomType == GeometryType_InfiniteSphere) {
            result->descriptor.type = LightType_Environment;
        } else if (geomInst.geomType == GeometryType_Point) {
            result->descriptor.type = LightType_Point;
        } else {
            result->descriptor.type = LightType_Area;
        }
    }
    
    result->descriptor.instIndex = instIndex;
    result->descriptor.geomInstIndex = geomInstIndex;
    result->selectProb = instProb * geomInstProb;
    result->uRemapped = uLight;  // 供下层采样使用
    
    return result->descriptor.isValid();
}


// ============================================================================
// 2. sampleLight - 采样光源位置
// ============================================================================

/// 在选定光源上采样一个表面点
///
/// @param descriptor   [in]  光源描述符（来自 selectLight）
/// @param refPosition  [in]  参考点位置（着色点）
/// @param u0, u1       [in]  [0,1) 随机数
/// @param result       [out] 采样结果（表面点、面积 PDF 等）
/// @param wlp          [in]  启动参数
/// @return 是否成功采样
CUDA_DEVICE_FUNCTION CUDA_INLINE bool sampleLight(
    const LightDescriptor& descriptor,
    const Point3D& refPosition,
    float u0, float u1,
    LightSampleResult* result,
    const WavefrontLaunchParameters& wlp) {
    
    if (!result || !descriptor.isValid()) return false;
    
    const Instance& inst = wlp.instBuffer[descriptor.instIndex];
    const GeometryInstance& geomInst = wlp.geomInstBuffer[descriptor.geomInstIndex];
    
    result->isValid = false;
    result->lightSelectProb = 0.0f;
    
    switch (descriptor.type) {
    case LightType_Environment: {
        // 环境光：在无穷远球面上采样 (theta, phi)
        // 使用 importance map 或均匀采样
        float theta = u0 * VLR_M_PI;
        float phi = u1 * VLR_M_2PI;
        
        // 球面方向转笛卡尔坐标
        float sinTheta = sin(theta);
        float cosTheta = cos(theta);
        float sinPhi = sin(phi);
        float cosPhi = cos(phi);
        
        SurfacePoint surfPt;
        surfPt.position = Point3D(sinTheta * cosPhi, cosTheta, sinTheta * sinPhi);
        surfPt.atInfinity = true;
        surfPt.geometricNormal = Normal3D(-surfPt.position.x, -surfPt.position.y, -surfPt.position.z);
        
        // 应用实例旋转
        phi += inst.rotationPhi;
        phi = phi - floor(phi / VLR_M_2PI) * VLR_M_2PI;
        surfPt.texCoord = TexCoord2D(phi / VLR_M_2PI, theta / VLR_M_PI);
        
        // 无穷远球面的面积 PDF：1 / (2 * pi^2 * sin(theta))
        float sinThetaSafe = (sinTheta > 1e-6f) ? sinTheta : 1e-6f;
        result->areaPDF = 1.0f / (VLR_M_2PI * VLR_M_PI * sinThetaSafe);
        
        transformSurfacePoint(inst.transform, surfPt, &result->lightSurfPt);
        result->lightSurfPt.atInfinity = true;
        
        result->isValid = true;
        break;
    }
    
    case LightType_Point: {
        // 点光源：位置唯一，无面积采样，返回 delta
        // 点光源位置需从几何解码获取，此处简化从实例原点
        SurfacePoint surfPt;
        surfPt.position = Point3D(0, 0, 0);
        surfPt.atInfinity = false;
        surfPt.geometricNormal = Normal3D(0, 1, 0);
        surfPt.shadingFrame = ReferenceFrame(Vector3D(1, 0, 0), surfPt.geometricNormal);
        surfPt.texCoord = TexCoord2D(0, 0);
        
        transformSurfacePoint(inst.transform, surfPt, &result->lightSurfPt);
        result->areaPDF = 1.0f;  // Delta 光源
        result->isValid = true;
        break;
    }
    
    case LightType_Area:
    default: {
        // 区域光：三角形网格采样（当无 OptiX callable 时）
        LightPosSample lightPosSample;
        if (geomInst.geomType == GeometryType_TriangleMesh &&
            geomInst.asTriMesh.triangleBuffer != nullptr &&
            wlp.vertexPositions != nullptr) {
            // Uniform triangle sampling: sample first triangle as fallback
            const Triangle& tri = geomInst.asTriMesh.triangleBuffer[0];
            float u = u0, v = u1;
            if (u + v > 1.0f) { u = 1.0f - u; v = 1.0f - v; }
            float w = 1.0f - u - v;
            Point3D p0 = wlp.vertexPositions[tri.indices[0]];
            Point3D p1 = wlp.vertexPositions[tri.indices[1]];
            Point3D p2 = wlp.vertexPositions[tri.indices[2]];
            Point3D posLocal = Point3D(
                p0.x * w + p1.x * u + p2.x * v,
                p0.y * w + p1.y * u + p2.y * v,
                p0.z * w + p1.z * u + p2.z * v);
            lightPosSample.surfPt.position = posLocal;
            Vector3D e1 = p1 - p0, e2 = p2 - p0;
            lightPosSample.surfPt.geometricNormal = normalize(cross(e1, e2));
            lightPosSample.surfPt.shadingFrame = ReferenceFrame(
                Vector3D(1,0,0), lightPosSample.surfPt.geometricNormal);
            lightPosSample.surfPt.texCoord = TexCoord2D(u, v);
            lightPosSample.surfPt.atInfinity = false;
            lightPosSample.areaPDF = (tri.area > 0.0f) ? (1.0f / tri.area) : 1.0f;
        } else if (geomInst.progSampleLightPosition >= 0) {
            // OptiX callable would go here
            return false;
        } else {
            // 无有效采样：使用实例原点
            lightPosSample.surfPt.position = Point3D(0, 0, 0);
            lightPosSample.surfPt.geometricNormal = Normal3D(0, 1, 0);
            lightPosSample.surfPt.shadingFrame = ReferenceFrame(
                Vector3D(1,0,0), lightPosSample.surfPt.geometricNormal);
            lightPosSample.surfPt.texCoord = TexCoord2D(0, 0);
            lightPosSample.surfPt.atInfinity = false;
            lightPosSample.areaPDF = 1.0f;
        }
        
        // 变换到世界空间
        transformSurfacePoint(inst.transform, lightPosSample.surfPt, &result->lightSurfPt);
        result->areaPDF = lightPosSample.areaPDF;
        result->isValid = result->areaPDF > 0.0f;
        break;
    }
    }
    
    return result->isValid;
}


// ============================================================================
// 3. evaluateLightEmission - 评估光源辐射
// ============================================================================

/// 评估光源在给定方向上的辐射度 Le
///
/// @param descriptor    [in]  光源描述符
/// @param lightSurfPt   [in]  光源表面点（来自 sampleLight）
/// @param dirToShading  [in]  从光源指向着色点的方向（世界空间，已归一化）
/// @param wls           [in]  波长采样
/// @param result        [out] 辐射度 Le
/// @param wlp           [in]  启动参数
/// @return 是否有效
CUDA_DEVICE_FUNCTION CUDA_INLINE bool evaluateLightEmission(
    const LightDescriptor& descriptor,
    const SurfacePoint& lightSurfPt,
    const Vector3D& dirToShading,
    const WavelengthSamples& wls,
    LightEmissionResult* result,
    const WavefrontLaunchParameters& wlp) {
    
    if (!result || !descriptor.isValid()) return false;
    
    const GeometryInstance& geomInst = wlp.geomInstBuffer[descriptor.geomInstIndex];
    const SurfaceMaterialDescriptor& matDesc = wlp.materialDescriptorBuffer[geomInst.materialIndex];
    
    EDF edf(matDesc, lightSurfPt, wls);
    SampledSpectrum spEmittance = edf.evaluateEmittance();
    
    if (!spEmittance.hasNonZero()) {
        result->isValid = false;
        return false;
    }
    
    // 光源向外发射的方向（与 dirToShading 相反）
    Vector3D dirOutLocal = lightSurfPt.shadingFrame.toLocal(-dirToShading);
    EDFQuery feQuery(DirectionType::All(), wls);
    result->Le = spEmittance * edf.evaluate(feQuery, dirOutLocal);
    result->isValid = result->Le.hasNonZero();
    
    return result->isValid;
}


// ============================================================================
// 4. computeLightPDF - 计算光源 PDF
// ============================================================================

/// 计算给定光源采样点的概率密度
/// 用于 MIS（多重要性采样）权重计算
///
/// @param descriptor      [in] 光源描述符
/// @param lightSurfPt     [in] 光源表面点
/// @param refPosition     [in] 参考点（着色点）
/// @param dirToLight      [in] 从参考点指向光源的方向（已归一化）
/// @param lightSelectProb [in] 光源选择概率
/// @param areaPDF         [in] 面积 PDF（来自 sampleLight，区域光必需）
/// @param wlp             [in] 启动参数
/// @return 立体角 PDF（用于与 BSDF PDF 做 MIS）
CUDA_DEVICE_FUNCTION CUDA_INLINE float computeLightPDF(
    const LightDescriptor& descriptor,
    const SurfacePoint& lightSurfPt,
    const Point3D& refPosition,
    const Vector3D& dirToLight,
    float lightSelectProb,
    float areaPDF,
    const WavefrontLaunchParameters& wlp) {
    
    if (!descriptor.isValid() || lightSelectProb <= 0.0f)
        return 0.0f;
    
    const GeometryInstance& geomInst = wlp.geomInstBuffer[descriptor.geomInstIndex];
    
    switch (descriptor.type) {
    case LightType_Point:
        // 点光源：立体角上为 delta，PDF 无穷大（MIS 时通常取 1 或特殊处理）
        return std::numeric_limits<float>::infinity();
    
    case LightType_Environment: {
        // 环境光：从立体角转回 (theta, phi) 评估 PDF
        Vector3D dir = dirToLight;
        float theta, phi;
        dir.toPolarYUp(&theta, &phi);
        float sinTheta = sin(theta);
        float sinThetaSafe = (sinTheta > 1e-6f) ? sinTheta : 1e-6f;
        
        // 简化：均匀球面 PDF
        float envAreaPDF = 1.0f / (VLR_M_2PI * VLR_M_PI * sinThetaSafe);
        float solidAnglePDF = envAreaPDF * sinThetaSafe;
        return lightSelectProb * solidAnglePDF;
    }
    
    case LightType_Area:
    default: {
        // 区域光：面积 PDF 转立体角 PDF
        Vector3D delta = lightSurfPt.position - refPosition;
        float dist = length(delta);
        if (dist < 1e-8f) return 0.0f;
        
        float cosLight = absDot(-dirToLight, lightSurfPt.geometricNormal);
        if (cosLight < 1e-6f) return 0.0f;
        
        float distSq = dist * dist;
        float solidAnglePDF = lightSelectProb * areaPDF * distSq / cosLight;
        return solidAnglePDF;
    }
    }
}


// ============================================================================
// 5. 辅助函数：面积 PDF 转立体角 PDF
// ============================================================================

/// 将面积 PDF 转换为立体角 PDF
/// 在 SampleLights kernel 中用于 MIS
CUDA_DEVICE_FUNCTION CUDA_INLINE float lightAreaToSolidAnglePDF(
    float areaPDF,
    float distance,
    float cosThetaLight) {
    
    if (cosThetaLight <= 0.0f || distance < 1e-8f)
        return 0.0f;
    
    float distSq = distance * distance;
    return areaPDF * distSq / cosThetaLight;
}


// ============================================================================
// 6. 便捷封装：完整光源采样流程
// ============================================================================

/// 一步完成选择、采样、评估光源（用于 SampleLights kernel）
/// 整合 selectLight + sampleLight + evaluateLightEmission
CUDA_DEVICE_FUNCTION CUDA_INLINE bool sampleAndEvaluateLight(
    float uLight, float u0, float u1, float u2,
    const Point3D& refPosition,
    const WavelengthSamples& wls,
    LightSelectResult* selectResult,
    LightSampleResult* sampleResult,
    LightEmissionResult* emissionResult,
    const WavefrontLaunchParameters& wlp) {
    
    if (!selectLight(uLight, selectResult, wlp))
        return false;
    
    if (!sampleLight(selectResult->descriptor, refPosition, u0, u1, sampleResult, wlp))
        return false;
    
    Vector3D dirToShading = refPosition - sampleResult->lightSurfPt.position;
    float dist = length(dirToShading);
    if (dist < 1e-8f) return false;
    dirToShading = dirToShading / dist;
    
    if (!evaluateLightEmission(
            selectResult->descriptor,
            sampleResult->lightSurfPt,
            dirToShading,
            wls,
            emissionResult,
            wlp))
        return false;
    
    sampleResult->lightSelectProb = selectResult->selectProb;
    return true;
}

#endif // VLR_Device

} // namespace shared
} // namespace vlr
