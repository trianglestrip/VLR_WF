// ============================================================================
// VLR 几何公共函数
//
// 本文件实现了几何相关的通用函数，包括：
// - 命中点解码
// - 表面点计算
// - 光线起点偏移（避免自相交）
// - 法线和切线计算
//
// 作者：VLR 开发团队
// 创建日期：2026-03-07
// 环境：CUDA 13.1, OptiX 8.0.0, VS2022
// ============================================================================

#pragma once

#include "geometry_types.h"
#include "../include/vlr/basic_types.h"
#include <cmath>

// 使用 basic_types 中的类型
using ::vlr::SurfacePoint;
using ::vlr::WavelengthSamples;

namespace vlr {
namespace shared {

// ============================================================================
// 0. 实例变换辅助
// ============================================================================

/// 将局部空间点变换到世界空间
CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE
Point3D transformInstancePoint(const Instance& inst, const Point3D& p) {
    return inst.transform.toWorld(p);
}

/// 将局部空间向量/法线变换到世界空间
CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE
Vector3D transformInstanceVector(const Instance& inst, const Vector3D& v) {
    return inst.transform.toWorld(v);
}

/// 将局部法线变换到世界空间（并归一化）
CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE
Normal3D transformInstanceNormal(const Instance& inst, const Normal3D& n) {
    return normalize(transformInstanceVector(inst, n));
}


// ============================================================================
// 1. 法线和切线计算
// ============================================================================

/// 从三角形顶点位置计算几何法线
/// 法线方向遵循右手定则（逆时针顶点顺序为正面）
CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE
Normal3D computeTriangleGeometryNormal(
    const Point3D& p0, const Point3D& p1, const Point3D& p2) {
    Vector3D edge1 = p1 - p0;
    Vector3D edge2 = p2 - p0;
    return normalize(cross(edge1, edge2));
}


/// 从顶点 UV 坐标计算切线方向
/// 使用 Mikkelsen 切线空间算法简化版
/// 当 duv 过小时返回零向量（调用者应使用备用切线）
CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE
Vector3D computeTriangleTangentFromUV(
    const Point3D& p0, const Point3D& p1, const Point3D& p2,
    const TexCoord2D& uv0, const TexCoord2D& uv1, const TexCoord2D& uv2) {
    float du1 = uv1.x - uv0.x;
    float dv1 = uv1.y - uv0.y;
    float du2 = uv2.x - uv0.x;
    float dv2 = uv2.y - uv0.y;

    float denom = du1 * dv2 - dv1 * du2;
    if (std::abs(denom) < 1e-8f)
        return Vector3D(0.0f, 0.0f, 0.0f);

    float r = 1.0f / denom;
    Vector3D dp1 = p1 - p0;
    Vector3D dp2 = p2 - p0;

    Vector3D tangent = (dp1 * (-dv2) + dp2 * dv1) * r;
    return tangent;
}


/// 使用顶点数据构建 shading 参考系
/// 若无 UV 则使用几何法线构造任意正交基
CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE
ReferenceFrame computeShadingFrame(
    const Normal3D& geometricNormal,
    const Vector3D& tangent) {
    // 若切线有效则使用，否则从法线构造
    float tangentLenSq = dot(tangent, tangent);
    Vector3D effectiveTangent;
    if (tangentLenSq > 1e-10f) {
        effectiveTangent = normalize(tangent);
        // 确保与法线正交
        effectiveTangent = normalize(effectiveTangent - geometricNormal * dot(effectiveTangent, geometricNormal));
    } else {
        // 构造任意正交基
        Vector3D absN(std::abs(geometricNormal.x), std::abs(geometricNormal.y), std::abs(geometricNormal.z));
        if (absN.x <= absN.y && absN.x <= absN.z)
            effectiveTangent = normalize(cross(Vector3D(1.0f, 0.0f, 0.0f), geometricNormal));
        else if (absN.y <= absN.z)
            effectiveTangent = normalize(cross(Vector3D(0.0f, 1.0f, 0.0f), geometricNormal));
        else
            effectiveTangent = normalize(cross(Vector3D(0.0f, 0.0f, 1.0f), geometricNormal));
    }
    return ReferenceFrame(effectiveTangent, geometricNormal);
}


// ============================================================================
// 2. 三角形网格命中点解码
// ============================================================================

/// 解码三角形网格的命中点
/// 使用重心坐标 (u, v, w=1-u-v) 插值顶点属性，并变换到世界空间
CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE
void decodeHitPointTriangleMesh(
    const HitPointDecodeInput& input,
    const GeometryDecodeContext& ctx,
    SurfacePoint* surfPt) {
    if (!ctx.isValid() || surfPt == nullptr)
        return;

    const GeometryInstance& geomInst = *ctx.geomInst;
    const Instance& inst = *ctx.inst;

    if (geomInst.geomType != ::vlr::GeometryType_TriangleMesh ||
        geomInst.asTriMesh.triangleBuffer == nullptr ||
        ctx.vertexData.positions == nullptr)
        return;

    const Triangle& tri = geomInst.asTriMesh.triangleBuffer[input.primIndex];
    float w = input.w();

    // 获取顶点位置
    Point3D p0 = ctx.vertexData.positions[tri.indices[0]];
    Point3D p1 = ctx.vertexData.positions[tri.indices[1]];
    Point3D p2 = ctx.vertexData.positions[tri.indices[2]];

    // 重心插值
    Point3D posLocal = p0 * w + p1 * input.u + p2 * input.v;
    surfPt->position = transformInstancePoint(inst, posLocal);

    // 几何法线
    Normal3D geomNormalLocal = computeTriangleGeometryNormal(p0, p1, p2);
    surfPt->geometricNormal = transformInstanceNormal(inst, geomNormalLocal);

    // 顶点法线插值（若有）
    Normal3D shadingNormalLocal;
    if (ctx.vertexData.hasNormals()) {
        Normal3D n0 = ctx.vertexData.normals[tri.indices[0]];
        Normal3D n1 = ctx.vertexData.normals[tri.indices[1]];
        Normal3D n2 = ctx.vertexData.normals[tri.indices[2]];
        shadingNormalLocal = normalize(n0 * w + n1 * input.u + n2 * input.v);
    } else {
        shadingNormalLocal = geomNormalLocal;
    }
    Normal3D shadingNormal = transformInstanceNormal(inst, shadingNormalLocal);

    // 切线（从 UV 或几何构造）
    Vector3D tangentLocal;
    if (ctx.vertexData.hasTexCoords()) {
        TexCoord2D uv0 = ctx.vertexData.texCoords[tri.indices[0]];
        TexCoord2D uv1 = ctx.vertexData.texCoords[tri.indices[1]];
        TexCoord2D uv2 = ctx.vertexData.texCoords[tri.indices[2]];
        tangentLocal = computeTriangleTangentFromUV(p0, p1, p2, uv0, uv1, uv2);
    } else {
        tangentLocal = Vector3D(0.0f, 0.0f, 0.0f);
    }

    surfPt->shadingFrame = computeShadingFrame(shadingNormal, tangentLocal);

    // 纹理坐标插值（若有）
    if (ctx.vertexData.hasTexCoords()) {
        TexCoord2D uv0 = ctx.vertexData.texCoords[tri.indices[0]];
        TexCoord2D uv1 = ctx.vertexData.texCoords[tri.indices[1]];
        TexCoord2D uv2 = ctx.vertexData.texCoords[tri.indices[2]];
        surfPt->texCoord = TexCoord2D(
            uv0.x * w + uv1.x * input.u + uv2.x * input.v,
            uv0.y * w + uv1.y * input.u + uv2.y * input.v
        );
    } else {
        surfPt->texCoord = TexCoord2D(input.u, input.v);
    }

    surfPt->atInfinity = false;
}


// ============================================================================
// 3. 通用命中点解码
// ============================================================================

/// 解码命中点信息
/// 根据几何类型分发到具体解码实现
/// 当 GeometryInstance 有自定义 progDecodeHitPoint 时，需由调用方通过 callable 调用
CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE
void decodeHitPoint(
    const HitPointDecodeInput& input,
    const GeometryDecodeContext& ctx,
    SurfacePoint* surfPt) {
    if (!ctx.isValid() || surfPt == nullptr)
        return;

    const GeometryInstance& geomInst = *ctx.geomInst;

    switch (geomInst.geomType) {
    case ::vlr::GeometryType_TriangleMesh:
        decodeHitPointTriangleMesh(input, ctx, surfPt);
        break;
    case ::vlr::GeometryType_InfiniteSphere:
    case ::vlr::GeometryType_Point:
        // 其他几何类型需自定义解码，此处不做处理
        break;
    default:
        break;
    }
}


/// 从离散命中参数字段解码（与 WavefrontHitInfo 等兼容）
CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE
void decodeHitPoint(
    uint32_t instIndex, uint32_t geomInstIndex, uint32_t primIndex,
    float u, float v,
    const GeometryInstance* geomInstBuffer,
    const Instance* instBuffer,
    const TriangleMeshVertexData& vertexData,
    SurfacePoint* surfPt) {
    HitPointDecodeInput input(instIndex, geomInstIndex, primIndex, u, v);
    GeometryDecodeContext ctx(
        geomInstBuffer + geomInstIndex,
        instBuffer + instIndex,
        vertexData
    );
    decodeHitPoint(input, ctx, surfPt);
}


// ============================================================================
// 4. 表面点完整计算
// ============================================================================

/// 计算表面点（包含法线贴图、切线修改、面积 PDF）
/// 此为完整流程：解码 -> 法线贴图 -> 切线修改
/// 需提供 applyBumpMapping、modifyTangent、calcNode 等外部函数
template <typename ApplyBumpFunc, typename ModifyTangentFunc, typename CalcNodeFunc>
CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE
void computeSurfacePoint(
    const HitPointDecodeInput& input,
    const GeometryDecodeContext& ctx,
    const WavelengthSamples& wls,
    SurfacePoint* surfPt,
    float* hypAreaPDF,
    ApplyBumpFunc applyBumpMapping,
    ModifyTangentFunc modifyTangent,
    CalcNodeFunc calcNode) {
    decodeHitPoint(input, ctx, surfPt);

    const GeometryInstance& geomInst = *ctx.geomInst;

    // 法线节点（法线贴图等）
    Normal3D localNormal = calcNode(geomInst.nodeNormal,
        Normal3D(0.0f, 0.0f, 1.0f), *surfPt, wls);
    applyBumpMapping(localNormal, surfPt);

    // 切线节点
    Vector3D newTangent = calcNode(geomInst.nodeTangent,
        surfPt->shadingFrame.x, *surfPt, wls);
    modifyTangent(newTangent, surfPt);

    // 面积 PDF（用于光源采样、MIS 等）
    if (hypAreaPDF != nullptr) {
        if (geomInst.geomType == ::vlr::GeometryType_TriangleMesh &&
            geomInst.asTriMesh.triangleBuffer != nullptr) {
            const Triangle& tri = geomInst.asTriMesh.triangleBuffer[input.primIndex];
            *hypAreaPDF = (tri.area > 0.0f) ? (1.0f / tri.area) : 1.0f;
        } else {
            *hypAreaPDF = 1.0f;
        }
    }
}


/// 简化版：仅解码，不做法线贴图/切线修改
CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE
void computeSurfacePointBasic(
    const HitPointDecodeInput& input,
    const GeometryDecodeContext& ctx,
    SurfacePoint* surfPt,
    float* hypAreaPDF) {
    decodeHitPoint(input, ctx, surfPt);

    if (hypAreaPDF != nullptr && ctx.isValid()) {
        const GeometryInstance& geomInst = *ctx.geomInst;
        if (geomInst.geomType == ::vlr::GeometryType_TriangleMesh &&
            geomInst.asTriMesh.triangleBuffer != nullptr) {
            const Triangle& tri = geomInst.asTriMesh.triangleBuffer[input.primIndex];
            *hypAreaPDF = (tri.area > 0.0f) ? (1.0f / tri.area) : 1.0f;
        } else {
            *hypAreaPDF = 1.0f;
        }
    }
}


// ============================================================================
// 5. 光线起点偏移（避免自相交）
// ============================================================================

/// 沿法线方向偏移光线起点，避免自相交
/// 用于路径追踪中发射下一跳光线时
/// normal 应为朝向光线发射方向的法线（即命中面朝外的一侧）
CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE
Point3D offsetRayOrigin(
    const Point3D& position,
    const Normal3D& normal) {
    using namespace GeometryConstants;

    float offset = RayOriginOffsetScale;
    offset = (offset < RayOriginOffsetMin) ? RayOriginOffsetMin : offset;
    offset = (offset > RayOriginOffsetMax) ? RayOriginOffsetMax : offset;

    return position + normal * offset;
}


/// 偏移光线起点（允许自定义偏移量）
CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE
Point3D offsetRayOrigin(
    const Point3D& position,
    const Normal3D& normal,
    float offsetScale) {
    using namespace GeometryConstants;

    float offset = offsetScale * RayOriginOffsetScale;
    offset = (offset < RayOriginOffsetMin) ? RayOriginOffsetMin : offset;
    offset = (offset > RayOriginOffsetMax) ? RayOriginOffsetMax : offset;

    return position + normal * offset;
}


/// 根据光线与法线的夹角选择正确的法线方向
/// cosFactor = dot(采样方向, 几何法线)，用于判断光线是从正面还是反面击中
CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE
Normal3D selectOffsetNormal(float cosFactor, const Normal3D& geometricNormal) {
    return (cosFactor > 0.0f) ? geometricNormal : (-geometricNormal);
}


/// 计算偏移后的光线起点（常用组合）
CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE
Point3D offsetRayOriginForNextBounce(
    const SurfacePoint& surfPt,
    float cosFactor) {
    Normal3D offsetDir = selectOffsetNormal(cosFactor, surfPt.geometricNormal);
    return offsetRayOrigin(surfPt.position, offsetDir);
}

}  // namespace shared
}  // namespace vlr
