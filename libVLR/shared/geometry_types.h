// ============================================================================
// VLR 几何类型定义
//
// 本文件定义了光线追踪所需的几何类型和结构。
// 为 Wavefront 路径追踪及其他渲染模式提供几何系统基础。
//
// 作者：VLR 开发团队
// 创建日期：2026-03-07
// 环境：CUDA 13.1, OptiX 8.0.0, VS2022
// ============================================================================

#pragma once

#include "../include/vlr/basic_types.h"

namespace vlr {
namespace shared {

// ============================================================================
// 类型重导出（与 basic_types 保持一致）
// ============================================================================

using ::vlr::GeometryType;
using ::vlr::NumGeometryTypes;

using ::vlr::Point3D;
using ::vlr::Vector3D;
using ::vlr::Normal3D;
using ::vlr::Vector2D;
using ::vlr::TexCoord2D;
using ::vlr::ReferenceFrame;

// ============================================================================
// 1. 基础几何类型（从 basic_types 重导出）
// ============================================================================

using ::vlr::Triangle;
using ::vlr::GeometryInstance;
using ::vlr::Instance;


// ============================================================================
// 2. 命中点解码输入
// ============================================================================

/// 命中点解码输入
/// 光线求交后得到的原始命中信息，用于解码为表面点
struct HitPointDecodeInput {
    uint32_t instIndex;     // 实例索引
    uint32_t geomInstIndex;  // 几何实例索引
    uint32_t primIndex;      // 图元索引（三角形索引等）
    float u, v;             // 参数坐标（重心坐标 b1, b2，b3 = 1-u-v）

    CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE
    HitPointDecodeInput()
        : instIndex(0xFFFFFFFF)
        , geomInstIndex(0xFFFFFFFF)
        , primIndex(0xFFFFFFFF)
        , u(0.0f)
        , v(0.0f)
    {}

    CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE
    HitPointDecodeInput(uint32_t inst, uint32_t geomInst, uint32_t prim, float u_, float v_)
        : instIndex(inst)
        , geomInstIndex(geomInst)
        , primIndex(prim)
        , u(u_)
        , v(v_)
    {}

    /// 获取第三个重心坐标
    CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE
    float w() const {
        return 1.0f - u - v;
    }

    /// 检查是否有效命中
    CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE
    bool isValid() const {
        return geomInstIndex != 0xFFFFFFFF && primIndex != 0xFFFFFFFF;
    }
};


// ============================================================================
// 3. 三角形网格顶点数据
// ============================================================================

/// 三角形网格顶点数据
/// 用于几何解码时访问顶点属性
/// 指针可为 nullptr，表示该属性不存在（将使用默认值或从几何计算）
struct TriangleMeshVertexData {
    const Point3D* positions;       // 顶点位置（必需）
    const Normal3D* normals;        // 顶点法线（可选，nullptr 时从几何计算）
    const TexCoord2D* texCoords;    // 顶点纹理坐标（可选）

    CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE
    TriangleMeshVertexData()
        : positions(nullptr)
        , normals(nullptr)
        , texCoords(nullptr)
    {}

    CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE
    TriangleMeshVertexData(const Point3D* pos, const Normal3D* norm = nullptr,
                           const TexCoord2D* uv = nullptr)
        : positions(pos)
        , normals(norm)
        , texCoords(uv)
    {}

    /// 是否有顶点法线
    CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE
    bool hasNormals() const { return normals != nullptr; }

    /// 是否有纹理坐标
    CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE
    bool hasTexCoords() const { return texCoords != nullptr; }
};


// ============================================================================
// 4. 几何解码上下文
// ============================================================================

/// 几何解码上下文
/// 包含解码命中点所需的全部数据引用
struct GeometryDecodeContext {
    const GeometryInstance* geomInst;       // 几何实例
    const Instance* inst;                   // 场景实例
    TriangleMeshVertexData vertexData;      // 顶点数据（三角形网格）

    CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE
    GeometryDecodeContext()
        : geomInst(nullptr)
        , inst(nullptr)
    {}

    CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE
    GeometryDecodeContext(const GeometryInstance* gi, const Instance* i,
                          const TriangleMeshVertexData& vd = TriangleMeshVertexData())
        : geomInst(gi)
        , inst(i)
        , vertexData(vd)
    {}

    /// 是否有效
    CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE
    bool isValid() const {
        return geomInst != nullptr && inst != nullptr;
    }
};


// ============================================================================
// 5. 光线偏移常量
// ============================================================================

namespace GeometryConstants {
    /// 光线起点偏移系数（避免自相交）
    /// 沿法线方向偏移此距离的倍数，基于表面曲率
    constexpr float RayOriginOffsetScale = 1e-4f;

    /// 最小偏移距离（绝对保证）
    constexpr float RayOriginOffsetMin = 1e-6f;

    /// 最大偏移距离（防止过度偏移）
    constexpr float RayOriginOffsetMax = 1e-2f;
}

}  // namespace shared
}  // namespace vlr
