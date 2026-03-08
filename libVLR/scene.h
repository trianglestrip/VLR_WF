// ============================================================================
// VLR 场景管理 - 头文件
//
// 本文件定义了 Scene 类，为 Context 提供场景数据管理功能。
// 支持几何、材质、光源、实例、相机的创建与管理，以及 OptiX 加速结构构建。
//
// 作者：VLR 开发团队
// 创建日期：2026-03-07
// 环境：CUDA 13.1, OptiX 8.0.0, VS2022
// ============================================================================

#pragma once

#include "include/vlr/basic_types.h"
#include "shared/geometry_types.h"
#include "shared/material_types.h"
#include "shared/light_types.h"
#include "shared/path_types.h"
#include <optix.h>
#include <cuda_runtime.h>
#include <vector>
#include <cstdint>
#include <optional>
#include <memory>

namespace vlr {

namespace cudau {
    template<typename T> class Buffer;
    class Context;
}

// ============================================================================
// 三角形网格数据（CPU 端）
// ============================================================================

/// 三角形网格：顶点、法线、纹理坐标和三角形索引
struct TriangleMeshData {
    std::vector<Point3D> positions;
    std::vector<Normal3D> normals;
    std::vector<TexCoord2D> texCoords;
    std::vector<Triangle> triangles;
};

// ============================================================================
// 实例变换参数
// ============================================================================

/// 实例变换：用于 createInstance 时指定位姿
struct InstanceTransform {
    Point3D position;           ///< 位置
    Vector3D scale;             ///< 缩放 (1,1,1 为无缩放)
    float rotationRadians;       ///< 绕 Y 轴旋转弧度

    InstanceTransform()
        : position(0, 0, 0)
        , scale(1, 1, 1)
        , rotationRadians(0)
    {}
};

// ============================================================================
// 相机参数
// ============================================================================

/// 相机设置参数：用于 setCamera
struct CameraParams {
    Point3D position;
    Vector3D lookAt;
    Vector3D up;
    float fovYDegrees;
    float aspect;
    float lensRadius;       ///< 光圈半径（0=针孔）
    float focusDistance;    ///< 焦平面距离
    float focalLength;     ///< 焦距（0=从 FOV 推导）
    uint32_t cameraType;   ///< CameraType 枚举值

    CameraParams()
        : position(0, 0, 5)
        , lookAt(0, 0, 0)
        , up(0, 1, 0)
        , fovYDegrees(45.0f)
        , aspect(16.0f / 9.0f)
        , lensRadius(0.0f)
        , focusDistance(1.0f)
        , focalLength(0.0f)
        , cameraType(0)     // CameraType_Perspective
    {}
};

// ============================================================================
// 区域光参数
// ============================================================================

/// 区域光创建参数
struct AreaLightParams {
    uint32_t instIndex;         ///< 实例索引
    uint32_t geomInstIndex;     ///< 几何实例索引
    SampledSpectrum radiance;   ///< 辐射度 (Le)
};

// ============================================================================
// 点光源参数
// ============================================================================

/// 点光源创建参数
struct PointLightParams {
    Point3D position;
    SampledSpectrum intensity;
};

// ============================================================================
// 环境光参数
// ============================================================================

/// 环境光设置参数（恒色或 IBL 占位）
struct EnvironmentLightParams {
    SampledSpectrum constantColor;  ///< 恒色环境光
    bool useConstant;              ///< 是否使用恒色（否则使用纹理）
    
    // 纹理环境光
    float* textureData;            ///< HDR 纹理数据（RGB float，线性空间）
    uint32_t textureWidth;         ///< 纹理宽度
    uint32_t textureHeight;        ///< 纹理高度
    float rotation;                ///< 环境旋转（弧度）
    uint32_t importanceMapHandle;  ///< 重要性采样贴图句柄（CUDA纹理对象或缓冲区ID）
    
    EnvironmentLightParams()
        : useConstant(false)
        , textureData(nullptr)
        , textureWidth(0)
        , textureHeight(0)
        , rotation(0.0f)
        , importanceMapHandle(0)
    {}
};

// ============================================================================
// Scene 类
// ============================================================================

/// 场景类：管理几何、材质、光源、实例、相机，构建 OptiX 加速结构
class Scene {
public:
    Scene(OptixDeviceContext optixContext, cudaStream_t stream, cudau::Context* cudaContext);
    ~Scene();

    Scene(const Scene&) = delete;
    Scene& operator=(const Scene&) = delete;

    // ------------------------------------------------------------------------
    // 几何管理
    // ------------------------------------------------------------------------

    /// 创建三角形网格，返回网格 ID
    uint32_t createTriangleMesh(
        const float* positions, size_t numVertices,
        const float* normals, size_t numNormals,
        const float* texCoords, size_t numTexCoords,
        const uint32_t* indices, size_t numIndices);

    /// 删除三角形网格
    void removeTriangleMesh(uint32_t meshId);

    // ------------------------------------------------------------------------
    // 材质管理
    // ------------------------------------------------------------------------

    /// 创建材质，返回材质 ID
    uint32_t createMaterial(
        uint32_t bsdfType,
        float albedoR, float albedoG, float albedoB,
        float roughness = 0.5f,
        float emissionR = 0.0f, float emissionG = 0.0f, float emissionB = 0.0f);

    /// 创建扩展材质（支持 IOR 和 metallic）
    uint32_t createMaterialEx(
        uint32_t bsdfType,
        float albedoR, float albedoG, float albedoB,
        float roughness,
        float metallic,
        float ior,
        float emissionR = 0.0f, float emissionG = 0.0f, float emissionB = 0.0f);

    /// 创建导体微表面反射材质（MicrofacetReflection）
    uint32_t createMaterialConductor(
        float etaR, float etaG, float etaB,
        float kappaR, float kappaG, float kappaB,
        float roughness);

    /// 创建各向异性导体微表面反射材质（MicrofacetReflection + Anisotropic GGX）
    uint32_t createMaterialConductorAniso(
        float etaR, float etaG, float etaB,
        float kappaR, float kappaG, float kappaB,
        float roughness,
        float anisotropy);

    /// 创建微表面散射材质（电介质，反射+折射）
    uint32_t createMaterialMicrofacetScattering(
        float ior,
        float roughness);

    /// 创建 LambertianScattering 次表面散射材质（双向 Lambert）
    uint32_t createMaterialLambertianScattering(
        float albedoR, float albedoG, float albedoB);

    /// 创建 Disney Principled BRDF 材质（Burley 2012）
    uint32_t createMaterialDisney(
        const float baseColor[3],
        float metallic, float subsurface, float specular, float roughness,
        float specularTint, float anisotropic, float sheen, float sheenTint,
        float clearcoat, float clearcoatGloss);

    /// 创建棋盘格材质（Lambert + 8x8 黑白棋盘格）
    /// @param color0 第一种颜色 RGB（如黑色）
    /// @param color1 第二种颜色 RGB（如白色）
    /// @param gridSize 棋盘格密度（默认 8 表示 8x8）
    /// @param extent 水平面半边长（>0 时将 position 归一化到 [0,1] 使格子均匀，0=使用 frac 周期）
    uint32_t createMaterialCheckerboard(
        float color0R, float color0G, float color0B,
        float color1R, float color1G, float color1B,
        uint32_t gridSize = 8,
        float extent = 0.0f);

    /// 创建多表面材质（2-4 层子材质混合）
    /// @param numLayers 层数 (2-4)
    /// @param subBSDFTypes 子材质类型数组
    /// @param subAlbedos 每个子材质的颜色 [numLayers][3]
    /// @param subRoughness 每个子材质的粗糙度
    /// @param weights 混合权重
    uint32_t createMaterialMultiSurface(
        int numLayers,
        const uint32_t* subBSDFTypes,
        const float* const* subAlbedos,
        const float* subRoughness,
        const float* weights);

    /// 设置材质参数
    void setMaterial(uint32_t materialId,
        float albedoR, float albedoG, float albedoB,
        float roughness = 0.5f,
        float emissionR = 0.0f, float emissionG = 0.0f, float emissionB = 0.0f);

    // ------------------------------------------------------------------------
    // 材质纹理绑定
    // ------------------------------------------------------------------------

    /// 设置材质基础色纹理（textureIndex 为 Context 纹理缓冲区中的索引，0xFFFFFFFF 表示清除）
    void setMaterialBaseColorTexture(uint32_t materialId, uint32_t textureIndex);
    void setMaterialRoughnessTexture(uint32_t materialId, uint32_t textureIndex);
    void setMaterialMetallicTexture(uint32_t materialId, uint32_t textureIndex);
    void setMaterialNormalTexture(uint32_t materialId, uint32_t textureIndex, float normalScale = 1.0f);
    /// 设置材质纹理坐标变换
    void setMaterialTextureTransform(uint32_t materialId,
        float scaleU, float scaleV, float offsetU, float offsetV);

    // ------------------------------------------------------------------------
    // 实例管理
    // ------------------------------------------------------------------------

    /// 创建实例：将网格与材质绑定，应用变换
    uint32_t createInstance(uint32_t meshId, uint32_t materialId,
        const InstanceTransform& transform = InstanceTransform());

    /// 设置实例变换
    void setInstanceTransform(uint32_t instanceId, const InstanceTransform& transform);

    // ------------------------------------------------------------------------
    // 光源管理
    // ------------------------------------------------------------------------

    /// 添加区域光（基于几何表面的发光）
    void addAreaLight(const AreaLightParams& params);

    /// 添加点光源
    void addPointLight(const PointLightParams& params);

    /// 添加方向光（平行光）
    void addDirectionalLight(const Vector3D& direction, const SampledSpectrum& radiance);

    /// 设置环境光
    void setEnvironmentLight(const EnvironmentLightParams& params);

    // ------------------------------------------------------------------------
    // 相机管理
    // ------------------------------------------------------------------------

    /// 设置相机参数
    void setCamera(const CameraParams& params);

    // ------------------------------------------------------------------------
    // 加速结构与 GPU 更新
    // ------------------------------------------------------------------------

    /// 构建 OptiX 加速结构（Geometry AS + Instance AS）
    void buildAccelerationStructure();

    /// 上传场景数据到 GPU
    void updateToGPU();

    // ------------------------------------------------------------------------
    // 数据访问（供 Context 使用）
    // ------------------------------------------------------------------------

    const shared::GeometryInstance* getGeomInstBuffer() const;
    const shared::Instance* getInstBuffer() const;
    const shared::SurfaceMaterialDescriptor* getMaterialBuffer() const;
    const uint32_t* getMaterialAlbedoTextureIndices() const;
    const uint32_t* getMaterialRoughnessTextureIndices() const;
    const uint32_t* getMaterialMetallicTextureIndices() const;
    const uint32_t* getMaterialNormalMapIndices() const;
    const shared::MaterialTextureParams* getMaterialTextureParams() const;
    const Point3D* getVertexPositions() const;
    const Normal3D* getVertexNormals() const;
    const TexCoord2D* getVertexTexCoords() const;
    const uint32_t* getLightInstIndices() const;
    uint32_t getNumGeomInsts() const;
    uint32_t getNumInstances() const;
    uint32_t getNumMaterials() const;
    uint32_t getNumLightInsts() const;
    uint32_t getEnvLightInstIndex() const;
    OptixTraversableHandle getTopGroup() const;
    const shared::CameraDescriptor& getCamera() const;
    const shared::SceneBounds& getSceneBounds() const;

private:
    OptixDeviceContext m_optixContext;
    cudaStream_t m_stream;
    cudau::Context* m_cudaContext;

    std::vector<TriangleMeshData> m_meshes;
    std::vector<shared::SurfaceMaterialDescriptor> m_materials;
    std::vector<shared::GeometryInstance> m_geometryInstances;
    std::vector<shared::Instance> m_instances;
    std::vector<uint32_t> m_lightInstIndices;
    shared::CameraDescriptor m_camera;
    shared::SceneBounds m_sceneBounds;
    std::optional<uint32_t> m_envLightInstIndex;

    struct InstanceRecord {
        uint32_t meshId;
        uint32_t materialId;
        uint32_t geomInstIndex;
        InstanceTransform transform;
        std::vector<uint32_t> geomInstIndices;
    };
    std::vector<InstanceRecord> m_instanceRecords;

    OptixTraversableHandle m_topGroup;
    std::vector<OptixTraversableHandle> m_gasHandles;
    std::vector<void*> m_gasOutputBuffers;
    void* m_accelOutputBuffer;
    void* m_accelTempBuffer;
    size_t m_accelOutputSize;
    size_t m_accelTempSize;

    std::unique_ptr<cudau::Buffer<shared::GeometryInstance>> m_geomInstBuffer;
    std::unique_ptr<cudau::Buffer<shared::Instance>> m_instBuffer;
    std::unique_ptr<cudau::Buffer<uint32_t>> m_instGeomIndicesBuffer;
    std::unique_ptr<cudau::Buffer<shared::SurfaceMaterialDescriptor>> m_materialBuffer;
    std::vector<uint32_t> m_materialAlbedoTextureIndices;
    std::vector<uint32_t> m_materialRoughnessTextureIndices;
    std::vector<uint32_t> m_materialMetallicTextureIndices;
    std::vector<uint32_t> m_materialNormalMapIndices;
    std::vector<shared::MaterialTextureParams> m_materialTextureParams;
    std::unique_ptr<cudau::Buffer<uint32_t>> m_materialAlbedoTextureIndicesBuffer;
    std::unique_ptr<cudau::Buffer<uint32_t>> m_materialRoughnessTextureIndicesBuffer;
    std::unique_ptr<cudau::Buffer<uint32_t>> m_materialMetallicTextureIndicesBuffer;
    std::unique_ptr<cudau::Buffer<uint32_t>> m_materialNormalMapIndicesBuffer;
    std::unique_ptr<cudau::Buffer<shared::MaterialTextureParams>> m_materialTextureParamsBuffer;
    std::unique_ptr<cudau::Buffer<Point3D>> m_vertexPositionBuffer;
    std::unique_ptr<cudau::Buffer<Normal3D>> m_vertexNormalBuffer;
    std::unique_ptr<cudau::Buffer<TexCoord2D>> m_vertexTexCoordBuffer;
    std::unique_ptr<cudau::Buffer<Triangle>> m_triangleBuffer;
    std::unique_ptr<cudau::Buffer<uint32_t>> m_lightInstIndicesBuffer;

    void computeSceneBounds();
    void buildGeometryAccelerationStructures();
    void buildInstanceAccelerationStructure();
    ReferenceFrame transformToReferenceFrame(const InstanceTransform& t);
};

}  // namespace vlr
