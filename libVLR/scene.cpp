// ============================================================================
// VLR 渲染引擎 - 场景
//
// 本文件实现 Scene 类的加速结构构建、场景边界计算、GPU 数据上传等核心逻辑
// 基于 OptiX 几何加速结构 (GAS) 与实例加速结构 (IAS) 以及 GPU 数据缓冲区
//
// 隶属于 VLR 渲染引擎
// 最后修改：2026-03-07
// 依赖：CUDA 13.1, OptiX 8.0.0, VS2022
// ============================================================================

// 启用后可在 CPU 端打印调试信息
// #define VLR_ENABLE_CPU_DEBUG 1

#ifdef VLR_ENABLE_CPU_DEBUG
    #define VLR_DEBUG_PRINTF(...) printf(__VA_ARGS__)
#else
    #define VLR_DEBUG_PRINTF(...) ((void)0)
#endif

#include "scene.h"
#include "shared/env_importance.h"
#include "utils/cuda_util.h"
#include "utils/optix_util.h"
#include <optix_stubs.h>
#include <cstring>
#include <stdexcept>
#include <algorithm>
#include <cmath>
#include <mutex>
#include <numeric>
#include <chrono>

#ifdef _WIN32
#undef min
#undef max
#endif

#include <taskflow/taskflow.hpp>
#include <taskflow/algorithm/for_each.hpp>

#define OPTIX_CHECK(call) ::vlr::optixu::checkError(call, #call, __FILE__, __LINE__)
#define CUDA_CHECK(call) ::vlr::cudau::checkError(call, #call, __FILE__, __LINE__)

#include "vlr_profile.h"

namespace {
using namespace vlr::shared;

class MaterialDescriptorBuilder {
    SurfaceMaterialDescriptor m_mat;

    void setFloat(int slot, float v) {
        m_mat.data[slot] = *reinterpret_cast<uint32_t*>(&v);
    }
    void setUint(int slot, uint32_t v) {
        m_mat.data[slot] = v;
    }

public:
    MaterialDescriptorBuilder(uint32_t bsdfType) {
        memset(&m_mat, 0, sizeof(m_mat));
        m_mat.bsdfProcedureSetIndex = bsdfType;
        m_mat.edfProcedureSetIndex = 0xFFFFFFFF;
        setUint(MaterialDataLayout::BSDFType, bsdfType);
    }

    MaterialDescriptorBuilder& setAlbedo(float r, float g, float b) {
        setFloat(MaterialDataLayout::AlbedoR, r);
        setFloat(MaterialDataLayout::AlbedoG, g);
        setFloat(MaterialDataLayout::AlbedoB, b);
        return *this;
    }

    MaterialDescriptorBuilder& setRoughness(float v) {
        setFloat(MaterialDataLayout::Roughness, v);
        return *this;
    }

    MaterialDescriptorBuilder& setMetallic(float v) {
        setFloat(MaterialDataLayout::Metallic, v);
        return *this;
    }

    MaterialDescriptorBuilder& setIOR(float v) {
        setFloat(MaterialDataLayout::IOR, v);
        return *this;
    }

    MaterialDescriptorBuilder& setEmission(float r, float g, float b) {
        setFloat(MaterialDataLayout::EmissionR, r);
        setFloat(MaterialDataLayout::EmissionG, g);
        setFloat(MaterialDataLayout::EmissionB, b);
        return *this;
    }

    MaterialDescriptorBuilder& setEta(float r, float g, float b) {
        setFloat(MaterialDataLayout::EtaR, r);
        setFloat(MaterialDataLayout::EtaG, g);
        setFloat(MaterialDataLayout::EtaB, b);
        return *this;
    }

    MaterialDescriptorBuilder& setKappa(float r, float g, float b) {
        setFloat(MaterialDataLayout::KappaR, r);
        setFloat(MaterialDataLayout::KappaG, g);
        setFloat(MaterialDataLayout::KappaB, b);
        return *this;
    }

    MaterialDescriptorBuilder& setAnisotropy(float v) {
        setFloat(MaterialDataLayout::Anisotropy, v);
        return *this;
    }

    MaterialDescriptorBuilder& setDispersion(float v) {
        setFloat(MaterialDataLayout::DispersionStrength, v);
        return *this;
    }

    MaterialDescriptorBuilder& setCheckerboard(float c1r, float c1g, float c1b, float gridSize, float extent) {
        setFloat(MaterialDataLayout::CheckerboardColor1R, c1r);
        setFloat(MaterialDataLayout::CheckerboardColor1G, c1g);
        setFloat(MaterialDataLayout::CheckerboardColor1B, c1b);
        setFloat(MaterialDataLayout::CheckerboardGridSize, gridSize);
        setFloat(MaterialDataLayout::CheckerboardExtent, extent);
        return *this;
    }

    MaterialDescriptorBuilder& setDataFloat(int slot, float v) {
        setFloat(slot, v);
        return *this;
    }

    MaterialDescriptorBuilder& setDataUint(int slot, uint32_t v) {
        setUint(slot, v);
        return *this;
    }

    MaterialDescriptorBuilder& setEmissive(bool hasEmission) {
        if (hasEmission) m_mat.edfProcedureSetIndex = 0;
        return *this;
    }

    const SurfaceMaterialDescriptor& build() const { return m_mat; }
};

} // anonymous namespace

namespace vlr {

using namespace shared;

// ============================================================================
// 构造函数与析构函数
// ============================================================================

Scene::Scene(OptixDeviceContext optixContext, cudaStream_t stream, cudau::Context* cudaContext)
    : m_optixContext(optixContext)
    , m_stream(stream)
    , m_cudaContext(cudaContext)
    , m_topGroup(0)
    , m_accelOutputBuffer(nullptr)
    , m_accelTempBuffer(nullptr)
    , m_accelOutputSize(0)
    , m_accelTempSize(0)
{
    m_sceneBounds.minPoint = Point3D(1e10f, 1e10f, 1e10f);
    m_sceneBounds.maxPoint = Point3D(-1e10f, -1e10f, -1e10f);
}

Scene::~Scene() {
    for (void* p : m_gasOutputBuffers)
        cudaFree(p);
    m_gasOutputBuffers.clear();
    if (m_accelOutputBuffer) {
        cudaFree(m_accelOutputBuffer);
        m_accelOutputBuffer = nullptr;
    }
    if (m_accelTempBuffer) {
        cudaFree(m_accelTempBuffer);
        m_accelTempBuffer = nullptr;
    }
}

// ============================================================================
// 三角网格
// ============================================================================

uint32_t Scene::createTriangleMesh(
    const float* positions, size_t numVertices,
    const float* normals, size_t numNormals,
    const float* texCoords, size_t numTexCoords,
    const uint32_t* indices, size_t numIndices)
{
    TriangleMeshData mesh;
    mesh.positions.resize(numVertices);
    for (size_t i = 0; i < numVertices; ++i) {
        mesh.positions[i].x = positions[i * 3 + 0];
        mesh.positions[i].y = positions[i * 3 + 1];
        mesh.positions[i].z = positions[i * 3 + 2];
    }
    if (normals && numNormals >= numVertices) {
        mesh.normals.resize(numVertices);
        for (size_t i = 0; i < numVertices; ++i) {
            mesh.normals[i].x = normals[i * 3 + 0];
            mesh.normals[i].y = normals[i * 3 + 1];
            mesh.normals[i].z = normals[i * 3 + 2];
        }
    }
    if (texCoords && numTexCoords >= numVertices) {
        mesh.texCoords.resize(numVertices);
        for (size_t i = 0; i < numVertices; ++i) {
            mesh.texCoords[i].x = texCoords[i * 2 + 0];
            mesh.texCoords[i].y = texCoords[i * 2 + 1];
        }
    }
    size_t numTriangles = numIndices / 3;
    mesh.triangles.resize(numTriangles);
    for (size_t i = 0; i < numTriangles; ++i) {
        uint32_t i0 = indices[i * 3 + 0];
        uint32_t i1 = indices[i * 3 + 1];
        uint32_t i2 = indices[i * 3 + 2];
        mesh.triangles[i].indices[0] = i0;
        mesh.triangles[i].indices[1] = i1;
        mesh.triangles[i].indices[2] = i2;
        const Point3D& p0 = mesh.positions[i0];
        const Point3D& p1 = mesh.positions[i1];
        const Point3D& p2 = mesh.positions[i2];
        Vector3D e1 = p1 - p0;
        Vector3D e2 = p2 - p0;
        mesh.triangles[i].area = 0.5f * length(cross(e1, e2));
    }
    if (mesh.normals.empty()) {
        mesh.normals.resize(numVertices);
        for (size_t i = 0; i < numVertices; ++i)
            mesh.normals[i] = Normal3D(0, 0, 0);
        for (size_t i = 0; i < numTriangles; ++i) {
            const Triangle& tri = mesh.triangles[i];
            Vector3D e1 = mesh.positions[tri.indices[1]] - mesh.positions[tri.indices[0]];
            Vector3D e2 = mesh.positions[tri.indices[2]] - mesh.positions[tri.indices[0]];
            Normal3D n = normalize(cross(e1, e2));
            mesh.normals[tri.indices[0]] = mesh.normals[tri.indices[0]] + n;
            mesh.normals[tri.indices[1]] = mesh.normals[tri.indices[1]] + n;
            mesh.normals[tri.indices[2]] = mesh.normals[tri.indices[2]] + n;
        }
        for (size_t i = 0; i < numVertices; ++i) {
            float len = std::sqrt(mesh.normals[i].x * mesh.normals[i].x + mesh.normals[i].y * mesh.normals[i].y + mesh.normals[i].z * mesh.normals[i].z);
            if (len > 1e-8f)
                mesh.normals[i] = Normal3D(mesh.normals[i].x / len, mesh.normals[i].y / len, mesh.normals[i].z / len);
            else
                mesh.normals[i] = Normal3D(0, 1, 0);
        }
    }
    m_meshes.push_back(std::move(mesh));
    return static_cast<uint32_t>(m_meshes.size() - 1);
}

void Scene::removeTriangleMesh(uint32_t meshId) {
    if (meshId >= m_meshes.size()) return;
    m_meshes.erase(m_meshes.begin() + meshId);
}

// ============================================================================
// 材质
// ============================================================================

uint32_t Scene::createMaterial(
    uint32_t bsdfType,
    float albedoR, float albedoG, float albedoB,
    float roughness,
    float emissionR, float emissionG, float emissionB)
{
    MaterialDescriptorBuilder b(bsdfType);
    b.setAlbedo(albedoR, albedoG, albedoB).setRoughness(roughness).setEmission(emissionR, emissionG, emissionB);
    if (bsdfType == static_cast<uint32_t>(BSDFType_Specular)) {
        b.setEta(1.0f, 1.0f, 1.0f).setKappa(0.0f, 0.0f, 0.0f);
    }
    m_materials.push_back(b.build());
    return static_cast<uint32_t>(m_materials.size() - 1);
}

uint32_t Scene::createMaterialEx(
    uint32_t bsdfType,
    float albedoR, float albedoG, float albedoB,
    float roughness,
    float metallic,
    float ior,
    float emissionR, float emissionG, float emissionB)
{
    MaterialDescriptorBuilder b(bsdfType);
    b.setAlbedo(albedoR, albedoG, albedoB).setRoughness(roughness).setMetallic(metallic)
     .setIOR(ior).setEmission(emissionR, emissionG, emissionB);
    if (bsdfType == static_cast<uint32_t>(BSDFType_Specular)) {
        b.setEta(1.0f, 1.0f, 1.0f).setKappa(0.0f, 0.0f, 0.0f);
    }

    uint32_t matIndex = static_cast<uint32_t>(m_materials.size());
    m_materials.push_back(b.build());

#ifdef VLR_DEBUG_MATERIAL
    const SurfaceMaterialDescriptor& mat = m_materials.back();
    VLR_DEBUG_PRINTF("[Material Debug] createMaterialEx: index=%u, bsdfType=%u\n", matIndex, bsdfType);
    VLR_DEBUG_PRINTF("  Albedo: (%.3f, %.3f, %.3f)\n", albedoR, albedoG, albedoB);
    VLR_DEBUG_PRINTF("  Roughness: %.3f, Metallic: %.3f, IOR: %.3f\n", roughness, metallic, ior);
    VLR_DEBUG_PRINTF("  Emission: (%.3f, %.3f, %.3f)\n", emissionR, emissionG, emissionB);
    VLR_DEBUG_PRINTF("  bsdfProcedureSetIndex: %u\n", mat.bsdfProcedureSetIndex);

    const float* dataAsFloat = reinterpret_cast<const float*>(mat.data);
    VLR_DEBUG_PRINTF("  Verify data[AlbedoR]: %.3f (expected %.3f)\n",
        dataAsFloat[MaterialDataLayout::AlbedoR], albedoR);
    VLR_DEBUG_PRINTF("  Verify data[IOR]: %.3f (expected %.3f)\n",
        dataAsFloat[MaterialDataLayout::IOR], ior);
#endif

    return matIndex;
}

uint32_t Scene::createMaterialConductor(
    float etaR, float etaG, float etaB,
    float kappaR, float kappaG, float kappaB,
    float roughness)
{
    MaterialDescriptorBuilder b(static_cast<uint32_t>(BSDFType_MicrofacetReflection));
    b.setEta(etaR, etaG, etaB).setKappa(kappaR, kappaG, kappaB)
     .setRoughness(roughness).setAlbedo(1.0f, 1.0f, 1.0f).setEmission(0.0f, 0.0f, 0.0f);

    uint32_t matIndex = static_cast<uint32_t>(m_materials.size());
    m_materials.push_back(b.build());

#ifdef VLR_DEBUG_MATERIAL
    const SurfaceMaterialDescriptor& mat = m_materials.back();
    VLR_DEBUG_PRINTF("[Material Debug] createMaterialConductor: index=%u, bsdfType=%u (MicrofacetReflection)\n",
        matIndex, static_cast<uint32_t>(BSDFType_MicrofacetReflection));
    VLR_DEBUG_PRINTF("  Eta: (%.3f, %.3f, %.3f)\n", etaR, etaG, etaB);
    VLR_DEBUG_PRINTF("  Kappa: (%.3f, %.3f, %.3f)\n", kappaR, kappaG, kappaB);
    VLR_DEBUG_PRINTF("  Roughness: %.3f\n", roughness);
    VLR_DEBUG_PRINTF("  bsdfProcedureSetIndex: %u\n", mat.bsdfProcedureSetIndex);

    const float* dataAsFloat = reinterpret_cast<const float*>(mat.data);
    VLR_DEBUG_PRINTF("  Verify data[EtaR]: %.3f (expected %.3f)\n",
        dataAsFloat[MaterialDataLayout::EtaR], etaR);
    VLR_DEBUG_PRINTF("  Verify data[KappaR]: %.3f (expected %.3f)\n",
        dataAsFloat[MaterialDataLayout::KappaR], kappaR);
    VLR_DEBUG_PRINTF("  Verify data[Roughness]: %.3f (expected %.3f)\n",
        dataAsFloat[MaterialDataLayout::Roughness], roughness);
#endif

    return matIndex;
}

uint32_t Scene::createMaterialConductorAniso(
    float etaR, float etaG, float etaB,
    float kappaR, float kappaG, float kappaB,
    float roughness,
    float anisotropy)
{
    MaterialDescriptorBuilder b(static_cast<uint32_t>(BSDFType_MicrofacetReflection));
    b.setEta(etaR, etaG, etaB).setKappa(kappaR, kappaG, kappaB)
     .setRoughness(roughness).setAnisotropy(anisotropy)
     .setAlbedo(1.0f, 1.0f, 1.0f).setEmission(0.0f, 0.0f, 0.0f);
    m_materials.push_back(b.build());
    return static_cast<uint32_t>(m_materials.size() - 1);
}

uint32_t Scene::createMaterialMicrofacetScattering(
    float ior,
    float roughness)
{
    MaterialDescriptorBuilder b(static_cast<uint32_t>(BSDFType_MicrofacetScattering));
    b.setIOR(ior).setRoughness(roughness).setAlbedo(0.999f, 0.999f, 0.999f).setEmission(0.0f, 0.0f, 0.0f);
    m_materials.push_back(b.build());
    return static_cast<uint32_t>(m_materials.size() - 1);
}

uint32_t Scene::createMaterialLambertianScattering(
    float albedoR, float albedoG, float albedoB)
{
    MaterialDescriptorBuilder b(static_cast<uint32_t>(BSDFType_LambertianScattering));
    b.setAlbedo(albedoR, albedoG, albedoB).setEmission(0.0f, 0.0f, 0.0f);
    m_materials.push_back(b.build());
    return static_cast<uint32_t>(m_materials.size() - 1);
}

uint32_t Scene::createMaterialDisney(
    const float baseColor[3],
    float metallic, float subsurface, float specular, float roughness,
    float specularTint, float anisotropic, float sheen, float sheenTint,
    float clearcoat, float clearcoatGloss)
{
    MaterialDescriptorBuilder b(static_cast<uint32_t>(BSDFType_DisneyBRDF));
    b.setAlbedo(baseColor[0], baseColor[1], baseColor[2]).setRoughness(roughness)
     .setDataFloat(MaterialDataLayout::Disney_Metallic, metallic)
     .setDataFloat(MaterialDataLayout::Disney_Subsurface, subsurface)
     .setDataFloat(MaterialDataLayout::Disney_Specular, specular)
     .setDataFloat(MaterialDataLayout::Disney_SpecularTint, specularTint)
     .setDataFloat(MaterialDataLayout::Disney_Anisotropic, anisotropic)
     .setDataFloat(MaterialDataLayout::Disney_Sheen, sheen)
     .setDataFloat(MaterialDataLayout::Disney_SheenTint, sheenTint)
     .setDataFloat(MaterialDataLayout::Disney_Clearcoat, clearcoat)
     .setDataFloat(MaterialDataLayout::Disney_ClearcoatGloss, clearcoatGloss)
     .setEmission(0.0f, 0.0f, 0.0f);
    m_materials.push_back(b.build());
    return static_cast<uint32_t>(m_materials.size() - 1);
}

uint32_t Scene::createMaterialCheckerboard(
    float color0R, float color0G, float color0B,
    float color1R, float color1G, float color1B,
    uint32_t gridSize,
    float extent)
{
    float gridSizeF = static_cast<float>(gridSize > 0 ? gridSize : 8);
    MaterialDescriptorBuilder b(static_cast<uint32_t>(BSDFType_LambertCheckerboard));
    b.setAlbedo(color0R, color0G, color0B)
     .setCheckerboard(color1R, color1G, color1B, gridSizeF, extent)
     .setRoughness(0.5f).setEmission(0.0f, 0.0f, 0.0f);
    m_materials.push_back(b.build());
    return static_cast<uint32_t>(m_materials.size() - 1);
}

uint32_t Scene::createMaterialMultiSurface(
    int numLayers,
    const uint32_t* subBSDFTypes,
    const float* const* subAlbedos,
    const float* subRoughness,
    const float* weights)
{
    if (numLayers < 2 || numLayers > 4 || !subBSDFTypes || !subAlbedos || !subRoughness || !weights)
        return 0xFFFFFFFF;

    auto storeSub = [&](MaterialDescriptorBuilder& builder, int i, int baseType, int baseR, int baseG, int baseB, int baseRough) {
        uint32_t t = (i < numLayers) ? subBSDFTypes[i] : 0;
        float r = (i < numLayers && subAlbedos[i]) ? subAlbedos[i][0] : 0.5f;
        float g = (i < numLayers && subAlbedos[i]) ? subAlbedos[i][1] : 0.5f;
        float b = (i < numLayers && subAlbedos[i]) ? subAlbedos[i][2] : 0.5f;
        float rough = (i < numLayers && subRoughness) ? subRoughness[i] : 0.5f;
        builder.setDataUint(baseType, t)
               .setDataFloat(baseR, r).setDataFloat(baseG, g).setDataFloat(baseB, b)
               .setDataFloat(baseRough, rough);
    };

    MaterialDescriptorBuilder b(static_cast<uint32_t>(BSDFType_MultiSurface));
    b.setDataFloat(MaterialDataLayout::MultiSurface_NumLayers, static_cast<float>(numLayers));
    storeSub(b, 0, MaterialDataLayout::SubMaterial0_BSDFType,
             MaterialDataLayout::SubMaterial0_AlbedoR, MaterialDataLayout::SubMaterial0_AlbedoG,
             MaterialDataLayout::SubMaterial0_AlbedoB, MaterialDataLayout::SubMaterial0_Roughness);
    storeSub(b, 1, MaterialDataLayout::SubMaterial1_BSDFType,
             MaterialDataLayout::SubMaterial1_AlbedoR, MaterialDataLayout::SubMaterial1_AlbedoG,
             MaterialDataLayout::SubMaterial1_AlbedoB, MaterialDataLayout::SubMaterial1_Roughness);
    storeSub(b, 2, MaterialDataLayout::SubMaterial2_BSDFType,
             MaterialDataLayout::SubMaterial2_AlbedoR, MaterialDataLayout::SubMaterial2_AlbedoG,
             MaterialDataLayout::SubMaterial2_AlbedoB, MaterialDataLayout::SubMaterial2_Roughness);
    storeSub(b, 3, MaterialDataLayout::SubMaterial3_BSDFType,
             MaterialDataLayout::SubMaterial3_AlbedoR, MaterialDataLayout::SubMaterial3_AlbedoG,
             MaterialDataLayout::SubMaterial3_AlbedoB, MaterialDataLayout::SubMaterial3_Roughness);

    float w[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    for (int i = 0; i < numLayers; ++i)
        w[i] = ::vlr::vlr_max(0.0f, weights[i]);
    b.setDataFloat(MaterialDataLayout::MultiSurface_Weight0, w[0])
     .setDataFloat(MaterialDataLayout::MultiSurface_Weight1, w[1])
     .setDataFloat(MaterialDataLayout::MultiSurface_Weight2, w[2])
     .setDataFloat(MaterialDataLayout::MultiSurface_Weight3, w[3])
     .setEmission(0.0f, 0.0f, 0.0f);

    m_materials.push_back(b.build());
    return static_cast<uint32_t>(m_materials.size() - 1);
}

void Scene::setMaterial(uint32_t materialId,
    float albedoR, float albedoG, float albedoB,
    float roughness,
    float emissionR, float emissionG, float emissionB)
{
    if (materialId >= m_materials.size()) return;
    auto& mat = m_materials[materialId];
    mat.data[MaterialDataLayout::AlbedoR] = *reinterpret_cast<uint32_t*>(&albedoR);
    mat.data[MaterialDataLayout::AlbedoG] = *reinterpret_cast<uint32_t*>(&albedoG);
    mat.data[MaterialDataLayout::AlbedoB] = *reinterpret_cast<uint32_t*>(&albedoB);
    mat.data[MaterialDataLayout::Roughness] = *reinterpret_cast<uint32_t*>(&roughness);
    mat.data[MaterialDataLayout::EmissionR] = *reinterpret_cast<uint32_t*>(&emissionR);
    mat.data[MaterialDataLayout::EmissionG] = *reinterpret_cast<uint32_t*>(&emissionG);
    mat.data[MaterialDataLayout::EmissionB] = *reinterpret_cast<uint32_t*>(&emissionB);
}

// ============================================================================
// 实例变换
// ============================================================================

ReferenceFrame Scene::transformToReferenceFrame(const InstanceTransform& t) {
    float c = std::cos(t.rotationRadians);
    float s = std::sin(t.rotationRadians);
    ReferenceFrame rf;
    rf.x.x = c * t.scale.x;   rf.x.y = 0;               rf.x.z = -s * t.scale.x;
    rf.y.x = 0;               rf.y.y = t.scale.y;       rf.y.z = 0;
    rf.z.x = s * t.scale.z;   rf.z.y = 0;               rf.z.z = c * t.scale.z;
    return rf;
}

uint32_t Scene::createInstance(uint32_t meshId, uint32_t materialId,
    const InstanceTransform& transform)
{
    if (meshId >= m_meshes.size() || materialId >= m_materials.size())
        throw std::runtime_error("createInstance: invalid meshId or materialId");
    GeometryInstance geomInst;
    memset(&geomInst, 0, sizeof(geomInst));
    geomInst.geomType = GeometryType_TriangleMesh;
    geomInst.instIndex = static_cast<uint32_t>(m_instances.size());
    geomInst.materialIndex = materialId;
    geomInst.importance = 1.0f;
    geomInst.progDecodeHitPoint = -1;
    geomInst.progSampleLightPosition = -1;
    geomInst.nodeNormal = -1;
    geomInst.nodeTangent = -1;
    geomInst.asTriMesh.triangleBuffer = nullptr;
    uint32_t geomInstIndex = static_cast<uint32_t>(m_geometryInstances.size());
    m_geometryInstances.push_back(geomInst);
    Instance inst;
    memset(&inst, 0, sizeof(inst));
    inst.geomInstIndices = nullptr;
    inst.numGeomInsts = 1;
    inst.transform = transformToReferenceFrame(transform);
    inst.rotationPhi = transform.rotationRadians;
    inst.lightGeomInstDistribution = 0;
    m_instances.push_back(inst);
    InstanceRecord rec;
    rec.meshId = meshId;
    rec.materialId = materialId;
    rec.geomInstIndex = geomInstIndex;
    rec.transform = transform;
    rec.geomInstIndices.push_back(geomInstIndex);
    m_instanceRecords.push_back(rec);
    return static_cast<uint32_t>(m_instances.size() - 1);
}

void Scene::setInstanceTransform(uint32_t instanceId, const InstanceTransform& transform) {
    if (instanceId >= m_instances.size()) return;
    m_instances[instanceId].transform = transformToReferenceFrame(transform);
    m_instances[instanceId].rotationPhi = transform.rotationRadians;
    // 保持 VLR 侧 IAS 重建时，InstanceRecord.transform 与实际 Instance 同步
    if (instanceId < m_instanceRecords.size())
        m_instanceRecords[instanceId].transform = transform;
}

// ============================================================================
// 材质纹理
// ============================================================================

namespace {
    constexpr uint32_t InvalidTexIdx = 0xFFFFFFFF;
}

static void ensureMaterialTextureArraysSize(std::vector<uint32_t>& albedo,
    std::vector<uint32_t>& rough, std::vector<uint32_t>& metal, std::vector<uint32_t>& normal,
    std::vector<shared::MaterialTextureParams>& params, size_t numMaterials) {
    if (albedo.size() < numMaterials) {
        albedo.resize(numMaterials, InvalidTexIdx);
        rough.resize(numMaterials, InvalidTexIdx);
        metal.resize(numMaterials, InvalidTexIdx);
        normal.resize(numMaterials, InvalidTexIdx);
        params.resize(numMaterials);
    }
}

void Scene::setMaterialBaseColorTexture(uint32_t materialId, uint32_t textureIndex) {
    if (materialId >= m_materials.size()) return;
    ensureMaterialTextureArraysSize(m_materialAlbedoTextureIndices, m_materialRoughnessTextureIndices,
        m_materialMetallicTextureIndices, m_materialNormalMapIndices, m_materialTextureParams, m_materials.size());
    m_materialAlbedoTextureIndices[materialId] = textureIndex;
}

void Scene::setMaterialRoughnessTexture(uint32_t materialId, uint32_t textureIndex) {
    if (materialId >= m_materials.size()) return;
    ensureMaterialTextureArraysSize(m_materialAlbedoTextureIndices, m_materialRoughnessTextureIndices,
        m_materialMetallicTextureIndices, m_materialNormalMapIndices, m_materialTextureParams, m_materials.size());
    m_materialRoughnessTextureIndices[materialId] = textureIndex;
}

void Scene::setMaterialMetallicTexture(uint32_t materialId, uint32_t textureIndex) {
    if (materialId >= m_materials.size()) return;
    ensureMaterialTextureArraysSize(m_materialAlbedoTextureIndices, m_materialRoughnessTextureIndices,
        m_materialMetallicTextureIndices, m_materialNormalMapIndices, m_materialTextureParams, m_materials.size());
    m_materialMetallicTextureIndices[materialId] = textureIndex;
}

void Scene::setMaterialNormalTexture(uint32_t materialId, uint32_t textureIndex, float normalScale) {
    if (materialId >= m_materials.size()) return;
    ensureMaterialTextureArraysSize(m_materialAlbedoTextureIndices, m_materialRoughnessTextureIndices,
        m_materialMetallicTextureIndices, m_materialNormalMapIndices, m_materialTextureParams, m_materials.size());
    m_materialNormalMapIndices[materialId] = textureIndex;
    m_materialTextureParams[materialId].normalScale = normalScale;
}

void Scene::setMaterialTextureTransform(uint32_t materialId,
    float scaleU, float scaleV, float offsetU, float offsetV) {
    if (materialId >= m_materials.size()) return;
    ensureMaterialTextureArraysSize(m_materialAlbedoTextureIndices, m_materialRoughnessTextureIndices,
        m_materialMetallicTextureIndices, m_materialNormalMapIndices, m_materialTextureParams, m_materials.size());
    m_materialTextureParams[materialId].scaleU = scaleU;
    m_materialTextureParams[materialId].scaleV = scaleV;
    m_materialTextureParams[materialId].offsetU = offsetU;
    m_materialTextureParams[materialId].offsetV = offsetV;
}

// ============================================================================
// 光源
// ============================================================================

void Scene::addAreaLight(const AreaLightParams& params) {
    m_lightInstIndices.push_back(params.instIndex);
}

void Scene::addPointLight(const PointLightParams& params) {
    // 创建 emissive 材质用于点光源
    SurfaceMaterialDescriptor mat;
    memset(&mat, 0, sizeof(mat));
    uint32_t bsdfType = static_cast<uint32_t>(BSDFType_Lambert);
    mat.bsdfProcedureSetIndex = bsdfType;
    mat.edfProcedureSetIndex = 0xFFFFFFFF;
    mat.data[MaterialDataLayout::BSDFType] = *reinterpret_cast<const uint32_t*>(&bsdfType);
    mat.data[MaterialDataLayout::EmissionR] = *reinterpret_cast<const uint32_t*>(&params.intensity.values[0]);
    mat.data[MaterialDataLayout::EmissionG] = *reinterpret_cast<const uint32_t*>(&params.intensity.values[1]);
    mat.data[MaterialDataLayout::EmissionB] = *reinterpret_cast<const uint32_t*>(&params.intensity.values[2]);
    uint32_t materialIndex = static_cast<uint32_t>(m_materials.size());
    m_materials.push_back(mat);
    
    // 创建 Point 几何体实例
    GeometryInstance geomInst;
    memset(&geomInst, 0, sizeof(geomInst));
    geomInst.geomType = GeometryType_Point;
    geomInst.materialIndex = materialIndex;
    geomInst.importance = 1.0f;
    geomInst.progDecodeHitPoint = -1;
    geomInst.progSampleLightPosition = -1;
    geomInst.nodeNormal = -1;
    geomInst.nodeTangent = -1;
    
    // 填充 asPoint 结构
    geomInst.asPoint.x = params.position.x;
    geomInst.asPoint.y = params.position.y;
    geomInst.asPoint.z = params.position.z;
    
    uint32_t geomInstIndex = static_cast<uint32_t>(m_geometryInstances.size());
    m_geometryInstances.push_back(geomInst);
    
    // 创建包含该几何体的 Instance
    shared::Instance inst;
    memset(&inst, 0, sizeof(inst));
    
    // 分配几何实例索引数组
    uint32_t* geomIndices = new uint32_t[1];
    geomIndices[0] = geomInstIndex;
    inst.geomInstIndices = geomIndices;
    inst.numGeomInsts = 1;
    inst.transform = ReferenceFrame(Vector3D(1, 0, 0), Normal3D(0, 1, 0));
    inst.rotationPhi = 0.0f;
    inst.lightGeomInstDistribution = 0;
    
    uint32_t instIndex = static_cast<uint32_t>(m_instances.size());
    m_instances.push_back(inst);
    
    // 记录实例
    InstanceRecord rec;
    rec.meshId = 0;
    rec.materialId = materialIndex;
    rec.geomInstIndex = geomInstIndex;
    rec.transform = InstanceTransform();
    rec.geomInstIndices.push_back(geomInstIndex);
    m_instanceRecords.push_back(rec);
    
    // 加入光源列表
    m_lightInstIndices.push_back(instIndex);
}

void Scene::addDirectionalLight(const Vector3D& direction, const SampledSpectrum& radiance) {
    // 创建 emissive 材质用于平行光
    SurfaceMaterialDescriptor mat;
    memset(&mat, 0, sizeof(mat));
    uint32_t bsdfType = static_cast<uint32_t>(BSDFType_Lambert);
    mat.bsdfProcedureSetIndex = bsdfType;
    mat.edfProcedureSetIndex = 0xFFFFFFFF;
    mat.data[MaterialDataLayout::BSDFType] = *reinterpret_cast<const uint32_t*>(&bsdfType);
    mat.data[MaterialDataLayout::EmissionR] = *reinterpret_cast<const uint32_t*>(&radiance.values[0]);
    mat.data[MaterialDataLayout::EmissionG] = *reinterpret_cast<const uint32_t*>(&radiance.values[1]);
    mat.data[MaterialDataLayout::EmissionB] = *reinterpret_cast<const uint32_t*>(&radiance.values[2]);
    uint32_t materialIndex = static_cast<uint32_t>(m_materials.size());
    m_materials.push_back(mat);
    
    // 创建 Directional 几何体，方向存于 Instance.transform.z
    GeometryInstance geomInst;
    memset(&geomInst, 0, sizeof(geomInst));
    geomInst.geomType = GeometryType_Directional;
    geomInst.materialIndex = materialIndex;
    geomInst.importance = 1.0f;
    geomInst.progDecodeHitPoint = -1;
    geomInst.progSampleLightPosition = -1;
    geomInst.nodeNormal = -1;
    geomInst.nodeTangent = -1;
    uint32_t geomInstIndex = static_cast<uint32_t>(m_geometryInstances.size());
    m_geometryInstances.push_back(geomInst);
    
    // 创建 Instance，方向存于 transform.z
    Instance inst;
    memset(&inst, 0, sizeof(inst));
    uint32_t* geomIndices = new uint32_t[1];
    geomIndices[0] = geomInstIndex;
    inst.geomInstIndices = geomIndices;
    inst.numGeomInsts = 1;
    Vector3D dir = normalize(direction);
    inst.transform = ReferenceFrame(Vector3D(1, 0, 0), Normal3D(dir.x, dir.y, dir.z));
    inst.rotationPhi = 0.0f;
    inst.lightGeomInstDistribution = 0;
    uint32_t instIndex = static_cast<uint32_t>(m_instances.size());
    m_instances.push_back(inst);
    
    InstanceRecord rec;
    rec.meshId = 0;
    rec.materialId = materialIndex;
    rec.geomInstIndex = geomInstIndex;
    rec.transform = InstanceTransform();
    rec.geomInstIndices.push_back(geomInstIndex);
    m_instanceRecords.push_back(rec);
    
    // 加入光源列表
    m_lightInstIndices.push_back(instIndex);
}

void Scene::setEnvironmentLight(const EnvironmentLightParams& params) {
    // 环境光说明：
    // - 使用 NEE 时需将环境光加入 m_lightInstIndices
    // - 在 miss shader 中 processEnvironmentHit 处理
    // - GeometryType_InfiniteSphere 在 miss shader 中采样
    
    // 若已有旧环境光，先从光源列表移除
    if (m_envLightInstIndex.has_value() && m_envLightInstIndex.value() < m_instances.size()) {
        // 从 m_lightInstIndices 移除旧索引
        auto it = std::find(m_lightInstIndices.begin(), m_lightInstIndices.end(), m_envLightInstIndex.value());
        if (it != m_lightInstIndices.end())
            m_lightInstIndices.erase(it);
    }
    
    // 创建环境光材质
    SurfaceMaterialDescriptor mat;
    memset(&mat, 0, sizeof(mat));
    uint32_t bsdfType = static_cast<uint32_t>(BSDFType_Lambert);
    mat.bsdfProcedureSetIndex = bsdfType;
    mat.edfProcedureSetIndex = 0xFFFFFFFF;
    mat.data[MaterialDataLayout::BSDFType] = *reinterpret_cast<const uint32_t*>(&bsdfType);
    
    // 使用常量颜色或纹理
    if (params.useConstant) {
        mat.data[MaterialDataLayout::EmissionR] = *reinterpret_cast<const uint32_t*>(&params.constantColor.values[0]);
        mat.data[MaterialDataLayout::EmissionG] = *reinterpret_cast<const uint32_t*>(&params.constantColor.values[1]);
        mat.data[MaterialDataLayout::EmissionB] = *reinterpret_cast<const uint32_t*>(&params.constantColor.values[2]);
    } else {
        // 使用纹理时 emission 填 1.0 表示缩放系数
        float defaultEmission = 1.0f;
        mat.data[MaterialDataLayout::EmissionR] = *reinterpret_cast<const uint32_t*>(&defaultEmission);
        mat.data[MaterialDataLayout::EmissionG] = *reinterpret_cast<const uint32_t*>(&defaultEmission);
        mat.data[MaterialDataLayout::EmissionB] = *reinterpret_cast<const uint32_t*>(&defaultEmission);
    }
    
    uint32_t materialIndex = static_cast<uint32_t>(m_materials.size());
    m_materials.push_back(mat);
    
    // 创建 InfiniteSphere 几何体
    GeometryInstance geomInst;
    memset(&geomInst, 0, sizeof(geomInst));
    geomInst.geomType = GeometryType_InfiniteSphere;
    geomInst.materialIndex = materialIndex;
    geomInst.importance = 1.0f;
    geomInst.progDecodeHitPoint = -1;
    geomInst.progSampleLightPosition = -1;
    geomInst.nodeNormal = -1;
    geomInst.nodeTangent = -1;
    
    // 使用纹理时需构建重要性图
    if (!params.useConstant && params.textureData && params.textureWidth > 0 && params.textureHeight > 0) {
        geomInst.asInfSphere.importanceMap = 1;  // 1 = 使用重要性采样
        // 拷贝纹理数据
        size_t numPixels = static_cast<size_t>(params.textureWidth) * params.textureHeight * 3;
        m_envTextureData.resize(numPixels);
        std::memcpy(m_envTextureData.data(), params.textureData, numPixels * sizeof(float));
        m_envTextureWidth = params.textureWidth;
        m_envTextureHeight = params.textureHeight;
        // 构建 CDF 表用于重要性采样
        float* cdfTheta = nullptr;
        float* cdfPhi = nullptr;
        if (buildEnvironmentImportanceMap(
                m_envTextureData.data(), params.textureWidth, params.textureHeight,
                &cdfTheta, &cdfPhi, &m_envTotalLuminance)) {
            m_envCdfTheta.assign(cdfTheta, cdfTheta + params.textureHeight + 1);
            m_envCdfPhi.assign(cdfPhi, cdfPhi + params.textureHeight * (params.textureWidth + 1));
            freeEnvironmentImportanceMap(cdfTheta, cdfPhi);
        } else {
            m_envCdfTheta.clear();
            m_envCdfPhi.clear();
            m_envTotalLuminance = 1.0f;
        }
    } else {
        geomInst.asInfSphere.importanceMap = 0;
        m_envTextureData.clear();
        m_envCdfTheta.clear();
        m_envCdfPhi.clear();
        m_envTextureWidth = 0;
        m_envTextureHeight = 0;
        m_envTotalLuminance = 1.0f;
    }
    
    uint32_t geomInstIndex = static_cast<uint32_t>(m_geometryInstances.size());
    m_geometryInstances.push_back(geomInst);
    
    // 创建 Instance
    shared::Instance inst;
    memset(&inst, 0, sizeof(inst));
    
    uint32_t* geomIndices = new uint32_t[1];
    geomIndices[0] = geomInstIndex;
    inst.geomInstIndices = geomIndices;
    inst.numGeomInsts = 1;
    inst.transform = ReferenceFrame(Vector3D(1, 0, 0), Normal3D(0, 1, 0));
    inst.rotationPhi = params.rotation;
    inst.lightGeomInstDistribution = 0;
    
    uint32_t instIndex = static_cast<uint32_t>(m_instances.size());
    m_instances.push_back(inst);
    
    // 记录实例
    InstanceRecord rec;
    rec.meshId = 0;
    rec.materialId = materialIndex;
    rec.geomInstIndex = geomInstIndex;
    rec.transform = InstanceTransform();
    rec.transform.rotationRadians = params.rotation;
    rec.geomInstIndices.push_back(geomInstIndex);
    m_instanceRecords.push_back(rec);
    
    // 加入 m_lightInstIndices 供 NEE 采样
    m_lightInstIndices.push_back(instIndex);
    m_envLightInstIndex = instIndex;
}

// ============================================================================
// 相机
// ============================================================================

void Scene::setCamera(const CameraParams& params) {
    m_camera.position = params.position;
    Vector3D dir = params.lookAt - params.position;
    float len = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
    if (len > 1e-6f) { dir.x /= len; dir.y /= len; dir.z /= len; }
    else { dir = Vector3D(0, 0, 1); }
    // cross(dir, up) for correct right-handed camera frame (no horizontal mirror)
    Vector3D tangent = Vector3D(dir.y * params.up.z - dir.z * params.up.y, dir.z * params.up.x - dir.x * params.up.z, dir.x * params.up.y - dir.y * params.up.x);
    len = std::sqrt(tangent.x * tangent.x + tangent.y * tangent.y + tangent.z * tangent.z);
    if (len > 1e-6f) { tangent.x /= len; tangent.y /= len; tangent.z /= len; }
    else { tangent = Vector3D(1, 0, 0); }
    m_camera.orientation = ReferenceFrame(tangent, Normal3D(dir.x, dir.y, dir.z));
    m_camera.fovY = params.fovYDegrees * VLR_M_PI / 180.0f;
    m_camera.aspect = params.aspect;
    m_camera.lensRadius = params.lensRadius;
    m_camera.focusDistance = params.focusDistance;
    m_camera.cameraType = params.cameraType;
}

// ============================================================================
// 几何加速结构构建
// ============================================================================

void Scene::buildGeometryAccelerationStructures() {
    for (void* p : m_gasOutputBuffers) cudaFree(p);
    m_gasOutputBuffers.clear();
    m_gasHandles.clear();

    const size_t numMeshes = m_meshes.size();
    if (numMeshes == 0) return;

    // 收集有效 mesh 索引（跳过空 mesh）
    std::vector<size_t> validIndices;
    validIndices.reserve(numMeshes);
    for (size_t i = 0; i < numMeshes; ++i) {
        if (!m_meshes[i].positions.empty() && !m_meshes[i].triangles.empty())
            validIndices.push_back(i);
    }
    const size_t numValid = validIndices.size();
    if (numValid == 0) return;

    // 预分配 CPU 端数据缓冲区（每个 mesh 独立，写不同下标无需锁）
    struct MeshCPUData {
        std::vector<float> vertices;
        std::vector<uint32_t> indices;
        uint32_t numVertices = 0;
        uint32_t numTriangles = 0;
    };
    std::vector<MeshCPUData> cpuData(numValid);

    // ---- 阶段 A：并行准备 CPU 端顶点/索引数据 ----
    {
        tf::Executor executor;
        tf::Taskflow taskflow;
        taskflow.for_each_index(size_t(0), numValid, size_t(1), [&](size_t slot) {
            const size_t meshIdx = validIndices[slot];
            const TriangleMeshData& mesh = m_meshes[meshIdx];
            auto& cd = cpuData[slot];
            cd.numVertices = static_cast<uint32_t>(mesh.positions.size());
            cd.numTriangles = static_cast<uint32_t>(mesh.triangles.size());

            cd.vertices.resize(mesh.positions.size() * 3);
            for (size_t i = 0; i < mesh.positions.size(); ++i) {
                cd.vertices[i * 3 + 0] = mesh.positions[i].x;
                cd.vertices[i * 3 + 1] = mesh.positions[i].y;
                cd.vertices[i * 3 + 2] = mesh.positions[i].z;
            }

            cd.indices.resize(mesh.triangles.size() * 3);
            for (size_t i = 0; i < mesh.triangles.size(); ++i) {
                cd.indices[i * 3 + 0] = mesh.triangles[i].indices[0];
                cd.indices[i * 3 + 1] = mesh.triangles[i].indices[1];
                cd.indices[i * 3 + 2] = mesh.triangles[i].indices[2];
            }
        });
        executor.run(taskflow).wait();
    }

    // ---- 阶段 B：GAS 构建（OptiX API 使用独立 CUDA stream 实现流水线重叠）----
    m_gasOutputBuffers.resize(numValid, nullptr);
    m_gasHandles.resize(numValid, 0);

    OptixAccelBuildOptions accelOptions = {
        .buildFlags = OPTIX_BUILD_FLAG_NONE,
        .operation = OPTIX_BUILD_OPERATION_BUILD
    };
    static const uint32_t triangleInputFlags[1] = { OPTIX_GEOMETRY_FLAG_NONE };

    constexpr size_t kMaxConcurrentStreams = 4;
    const size_t numStreams = (std::min)(numValid, kMaxConcurrentStreams);
    std::vector<cudaStream_t> buildStreams(numStreams);
    for (size_t i = 0; i < numStreams; ++i)
        CUDA_CHECK(cudaStreamCreate(&buildStreams[i]));

    for (size_t slot = 0; slot < numValid; ++slot) {
        const auto& cd = cpuData[slot];
        cudaStream_t bstream = buildStreams[slot % numStreams];

        CUdeviceptr d_vertices = 0;
        CUDA_CHECK(cudaMalloc(reinterpret_cast<void**>(&d_vertices), cd.vertices.size() * sizeof(float)));
        CUDA_CHECK(cudaMemcpyAsync(reinterpret_cast<void*>(d_vertices), cd.vertices.data(),
            cd.vertices.size() * sizeof(float), cudaMemcpyHostToDevice, bstream));

        CUdeviceptr d_indices = 0;
        CUDA_CHECK(cudaMalloc(reinterpret_cast<void**>(&d_indices), cd.indices.size() * sizeof(uint32_t)));
        CUDA_CHECK(cudaMemcpyAsync(reinterpret_cast<void*>(d_indices), cd.indices.data(),
            cd.indices.size() * sizeof(uint32_t), cudaMemcpyHostToDevice, bstream));

        OptixBuildInput triangleInput = {};
        triangleInput.type = OPTIX_BUILD_INPUT_TYPE_TRIANGLES;
        triangleInput.triangleArray.vertexFormat = OPTIX_VERTEX_FORMAT_FLOAT3;
        triangleInput.triangleArray.vertexStrideInBytes = sizeof(float) * 3;
        triangleInput.triangleArray.numVertices = cd.numVertices;
        triangleInput.triangleArray.vertexBuffers = &d_vertices;
        triangleInput.triangleArray.indexFormat = OPTIX_INDICES_FORMAT_UNSIGNED_INT3;
        triangleInput.triangleArray.indexStrideInBytes = sizeof(uint32_t) * 3;
        triangleInput.triangleArray.numIndexTriplets = cd.numTriangles;
        triangleInput.triangleArray.indexBuffer = d_indices;
        triangleInput.triangleArray.flags = triangleInputFlags;
        triangleInput.triangleArray.numSbtRecords = 1;

        OptixAccelBufferSizes gasBufferSizes;
        OPTIX_CHECK(optixAccelComputeMemoryUsage(m_optixContext, &accelOptions, &triangleInput, 1, &gasBufferSizes));

        void* d_temp = nullptr;
        void* d_output = nullptr;
        CUDA_CHECK(cudaMalloc(&d_temp, gasBufferSizes.tempSizeInBytes));
        CUDA_CHECK(cudaMalloc(&d_output, gasBufferSizes.outputSizeInBytes));

        OptixTraversableHandle gasHandle = 0;
        OPTIX_CHECK(optixAccelBuild(m_optixContext, bstream, &accelOptions, &triangleInput, 1,
            reinterpret_cast<CUdeviceptr>(d_temp), gasBufferSizes.tempSizeInBytes,
            reinterpret_cast<CUdeviceptr>(d_output), gasBufferSizes.outputSizeInBytes,
            &gasHandle, nullptr, 0));

        // 异步释放临时缓冲区需要先同步当前 stream
        CUDA_CHECK(cudaStreamSynchronize(bstream));
        cudaFree(reinterpret_cast<void*>(d_vertices));
        cudaFree(reinterpret_cast<void*>(d_indices));
        cudaFree(d_temp);

        m_gasOutputBuffers[slot] = d_output;
        m_gasHandles[slot] = gasHandle;
    }

    for (size_t i = 0; i < numStreams; ++i)
        CUDA_CHECK(cudaStreamDestroy(buildStreams[i]));
}

void Scene::buildInstanceAccelerationStructure() {
    if (m_gasHandles.empty() || m_instanceRecords.empty()) {
        m_topGroup = 0;
        return;
    }

    std::vector<OptixInstance> optixInstances;
    optixInstances.reserve(m_instances.size());
    for (size_t i = 0; i < m_instances.size(); ++i) {
        const InstanceRecord& rec = m_instanceRecords[i];
        // Point/Directional/InfiniteSphere 不加入 IAS，由 NEE 单独采样
        if (!rec.geomInstIndices.empty()) {
            const GeometryInstance& geomInst = m_geometryInstances[rec.geomInstIndices[0]];
            if (geomInst.geomType == GeometryType_Point ||
                geomInst.geomType == GeometryType_Directional ||
                geomInst.geomType == GeometryType_InfiniteSphere) {
                continue;
            }
        }
        uint32_t gasIdx = (std::min)(rec.meshId, static_cast<uint32_t>(m_gasHandles.size() - 1));
        OptixInstance oi = {};
        oi.instanceId = static_cast<uint32_t>(i);
        oi.sbtOffset = 0;
        oi.visibilityMask = 0xFF;
        oi.flags = OPTIX_INSTANCE_FLAG_NONE;
        oi.traversableHandle = m_gasHandles[gasIdx];
        const ReferenceFrame& rf = m_instances[i].transform;
        const InstanceTransform& it = rec.transform;
        // OptiX transform 使用 row-major 3x4 矩阵
        // Row 0: [m00, m01, m02, tx]
        // Row 1: [m10, m11, m12, ty]
        // Row 2: [m20, m21, m22, tz]
        // ReferenceFrame 的 x, y, z 分别对应行 0, 1, 2
        oi.transform[0] = rf.x.x; oi.transform[1] = rf.y.x; oi.transform[2] = rf.z.x; oi.transform[3] = it.position.x;
        oi.transform[4] = rf.x.y; oi.transform[5] = rf.y.y; oi.transform[6] = rf.z.y; oi.transform[7] = it.position.y;
        oi.transform[8] = rf.x.z; oi.transform[9] = rf.y.z; oi.transform[10] = rf.z.z; oi.transform[11] = it.position.z;

        optixInstances.push_back(oi);
    }
    CUdeviceptr d_instances = 0;
    CUDA_CHECK(cudaMalloc(reinterpret_cast<void**>(&d_instances), optixInstances.size() * sizeof(OptixInstance)));
    CUDA_CHECK(cudaMemcpy(reinterpret_cast<void*>(d_instances), optixInstances.data(), optixInstances.size() * sizeof(OptixInstance), cudaMemcpyHostToDevice));
    OptixBuildInput iasInput = {};
    iasInput.type = OPTIX_BUILD_INPUT_TYPE_INSTANCES;
    iasInput.instanceArray.instances = d_instances;
    iasInput.instanceArray.numInstances = static_cast<uint32_t>(optixInstances.size());
    OptixAccelBuildOptions iasOptions = {
        .buildFlags = OPTIX_BUILD_FLAG_NONE,
        .operation = OPTIX_BUILD_OPERATION_BUILD
    };
    OptixAccelBufferSizes iasBufferSizes;
    OPTIX_CHECK(optixAccelComputeMemoryUsage(m_optixContext, &iasOptions, &iasInput, 1, &iasBufferSizes));
    if (m_accelTempBuffer) cudaFree(m_accelTempBuffer);
    if (m_accelOutputBuffer) cudaFree(m_accelOutputBuffer);
    CUDA_CHECK(cudaMalloc(&m_accelTempBuffer, iasBufferSizes.tempSizeInBytes));
    CUDA_CHECK(cudaMalloc(&m_accelOutputBuffer, iasBufferSizes.outputSizeInBytes));
    m_accelTempSize = iasBufferSizes.tempSizeInBytes;
    m_accelOutputSize = iasBufferSizes.outputSizeInBytes;
    OPTIX_CHECK(optixAccelBuild(m_optixContext, m_stream, &iasOptions, &iasInput, 1,
        reinterpret_cast<CUdeviceptr>(m_accelTempBuffer), m_accelTempSize,
        reinterpret_cast<CUdeviceptr>(m_accelOutputBuffer), m_accelOutputSize,
        &m_topGroup, nullptr, 0));
    cudaFree(reinterpret_cast<void*>(d_instances));
}

void Scene::buildAccelerationStructure() {
    buildGeometryAccelerationStructures();
    buildInstanceAccelerationStructure();
}

// ============================================================================
// GPU 数据上传
// ============================================================================

void Scene::computeSceneBounds() {
    const size_t numInst = m_instanceRecords.size();
    if (numInst == 0) {
        m_sceneBounds.minPoint = Point3D(0, 0, 0);
        m_sceneBounds.maxPoint = Point3D(0, 0, 0);
        return;
    }

    // 每个 instance 独立计算局部 AABB，最终合并
    struct LocalBounds {
        Point3D minPt{1e10f, 1e10f, 1e10f};
        Point3D maxPt{-1e10f, -1e10f, -1e10f};
    };
    std::vector<LocalBounds> perInstBounds(numInst);

    {
        tf::Executor executor;
        tf::Taskflow taskflow;
        taskflow.for_each_index(size_t(0), numInst, size_t(1), [&](size_t i) {
            const InstanceRecord& instRec = m_instanceRecords[i];
            const TriangleMeshData& mesh = m_meshes[instRec.meshId];
            const Instance& inst = m_instances[i];
            auto& lb = perInstBounds[i];
            for (const auto& p : mesh.positions) {
                Point3D wp = inst.transform.toWorld(p);
                wp.x += instRec.transform.position.x;
                wp.y += instRec.transform.position.y;
                wp.z += instRec.transform.position.z;
                lb.minPt.x = (std::min)(lb.minPt.x, wp.x);
                lb.minPt.y = (std::min)(lb.minPt.y, wp.y);
                lb.minPt.z = (std::min)(lb.minPt.z, wp.z);
                lb.maxPt.x = (std::max)(lb.maxPt.x, wp.x);
                lb.maxPt.y = (std::max)(lb.maxPt.y, wp.y);
                lb.maxPt.z = (std::max)(lb.maxPt.z, wp.z);
            }
        });
        executor.run(taskflow).wait();
    }

    // reduce 合并所有局部 AABB
    m_sceneBounds.minPoint = Point3D(1e10f, 1e10f, 1e10f);
    m_sceneBounds.maxPoint = Point3D(-1e10f, -1e10f, -1e10f);
    for (const auto& lb : perInstBounds) {
        m_sceneBounds.minPoint.x = (std::min)(m_sceneBounds.minPoint.x, lb.minPt.x);
        m_sceneBounds.minPoint.y = (std::min)(m_sceneBounds.minPoint.y, lb.minPt.y);
        m_sceneBounds.minPoint.z = (std::min)(m_sceneBounds.minPoint.z, lb.minPt.z);
        m_sceneBounds.maxPoint.x = (std::max)(m_sceneBounds.maxPoint.x, lb.maxPt.x);
        m_sceneBounds.maxPoint.y = (std::max)(m_sceneBounds.maxPoint.y, lb.maxPt.y);
        m_sceneBounds.maxPoint.z = (std::max)(m_sceneBounds.maxPoint.z, lb.maxPt.z);
    }
}

Scene::AggregatedMeshData Scene::aggregateMeshData() {
    const size_t numMeshes = m_meshes.size();
    AggregatedMeshData agg;

    if (numMeshes == 0) return agg;

    // Prefix sum 计算每个 mesh 在全局数组中的偏移
    std::vector<size_t> posOffsets(numMeshes + 1, 0);
    std::vector<size_t> normOffsets(numMeshes + 1, 0);
    std::vector<size_t> uvOffsets(numMeshes + 1, 0);
    std::vector<size_t> triOffsets(numMeshes + 1, 0);
    for (size_t i = 0; i < numMeshes; ++i) {
        posOffsets[i + 1]  = posOffsets[i]  + m_meshes[i].positions.size();
        normOffsets[i + 1] = normOffsets[i] + m_meshes[i].normals.size();
        uvOffsets[i + 1]   = uvOffsets[i]   + m_meshes[i].texCoords.size();
        triOffsets[i + 1]  = triOffsets[i]  + m_meshes[i].triangles.size();
    }

    agg.positions.resize(posOffsets[numMeshes]);
    agg.normals.resize(normOffsets[numMeshes]);
    agg.texCoords.resize(uvOffsets[numMeshes]);
    agg.triangles.resize(triOffsets[numMeshes]);
    agg.geomInstTriangleOffsets.resize(numMeshes);

    // 并行拷贝各 mesh 数据到全局数组（每个 mesh 写独立区间，无锁）
    tf::Executor executor;
    tf::Taskflow taskflow;
    taskflow.for_each_index(size_t(0), numMeshes, size_t(1), [&](size_t m) {
        const TriangleMeshData& mesh = m_meshes[m];
        const uint32_t vOff = static_cast<uint32_t>(posOffsets[m]);

        std::memcpy(&agg.positions[posOffsets[m]], mesh.positions.data(),
                    mesh.positions.size() * sizeof(Point3D));
        if (!mesh.normals.empty())
            std::memcpy(&agg.normals[normOffsets[m]], mesh.normals.data(),
                        mesh.normals.size() * sizeof(Normal3D));
        if (!mesh.texCoords.empty())
            std::memcpy(&agg.texCoords[uvOffsets[m]], mesh.texCoords.data(),
                        mesh.texCoords.size() * sizeof(TexCoord2D));

        agg.geomInstTriangleOffsets[m] = static_cast<uint32_t>(triOffsets[m]);
        const size_t tOff = triOffsets[m];
        for (size_t t = 0; t < mesh.triangles.size(); ++t) {
            Triangle tri = mesh.triangles[t];
            tri.indices[0] += vOff;
            tri.indices[1] += vOff;
            tri.indices[2] += vOff;
            agg.triangles[tOff + t] = tri;
        }
    });
    executor.run(taskflow).wait();

    return agg;
}

void Scene::uploadAggregatedData(const AggregatedMeshData& agg) {
    if (!m_vertexPositionBuffer) m_vertexPositionBuffer = std::make_unique<cudau::Buffer<Point3D>>();
    m_vertexPositionBuffer->initialize(m_cudaContext, cudau::BufferType::Device, agg.positions.size());
    m_vertexPositionBuffer->copyToDevice(agg.positions.data(), agg.positions.size(), m_stream);
    if (!agg.normals.empty()) {
        if (!m_vertexNormalBuffer) m_vertexNormalBuffer = std::make_unique<cudau::Buffer<Normal3D>>();
        m_vertexNormalBuffer->initialize(m_cudaContext, cudau::BufferType::Device, agg.normals.size());
        m_vertexNormalBuffer->copyToDevice(agg.normals.data(), agg.normals.size(), m_stream);
    }
    if (!agg.texCoords.empty()) {
        if (!m_vertexTexCoordBuffer) m_vertexTexCoordBuffer = std::make_unique<cudau::Buffer<TexCoord2D>>();
        m_vertexTexCoordBuffer->initialize(m_cudaContext, cudau::BufferType::Device, agg.texCoords.size());
        m_vertexTexCoordBuffer->copyToDevice(agg.texCoords.data(), agg.texCoords.size(), m_stream);
    }
    if (!m_triangleBuffer) m_triangleBuffer = std::make_unique<cudau::Buffer<Triangle>>();
    m_triangleBuffer->initialize(m_cudaContext, cudau::BufferType::Device, agg.triangles.size());
    m_triangleBuffer->copyToDevice(agg.triangles.data(), agg.triangles.size(), m_stream);
    const auto& geomInstTriangleOffsets = agg.geomInstTriangleOffsets;
    for (size_t g = 0; g < m_geometryInstances.size(); ++g) {
        // 跳过 triangleBuffer：Point/Directional/InfiniteSphere 使用 union 其他成员
        if (m_geometryInstances[g].geomType != GeometryType_TriangleMesh)
            continue;
        uint32_t meshId = (g < m_instanceRecords.size()) ? m_instanceRecords[g].meshId : 0;
        uint32_t offset = geomInstTriangleOffsets[(std::min)(meshId, static_cast<uint32_t>(geomInstTriangleOffsets.size() - 1))];
        m_geometryInstances[g].asTriMesh.triangleBuffer = m_triangleBuffer->getDevicePointer() + offset;
        m_geometryInstances[g].asTriMesh.numTriangles = (meshId < m_meshes.size())
            ? static_cast<uint32_t>(m_meshes[meshId].triangles.size()) : 0;
    }
    size_t totalGeomIndices = 0;
    for (const auto& rec : m_instanceRecords) totalGeomIndices += rec.geomInstIndices.size();
    if (!m_instGeomIndicesBuffer) m_instGeomIndicesBuffer = std::make_unique<cudau::Buffer<uint32_t>>();
    m_instGeomIndicesBuffer->initialize(m_cudaContext, cudau::BufferType::Device, totalGeomIndices);
    std::vector<uint32_t> geomIndicesFlat;
    uint32_t offset = 0;
    for (size_t i = 0; i < m_instances.size(); ++i) {
        m_instances[i].geomInstIndices = m_instGeomIndicesBuffer->getDevicePointerAt(offset);
        for (uint32_t idx : m_instanceRecords[i].geomInstIndices) geomIndicesFlat.push_back(idx);
        offset += static_cast<uint32_t>(m_instanceRecords[i].geomInstIndices.size());
    }
    m_instGeomIndicesBuffer->copyToDevice(geomIndicesFlat.data(), geomIndicesFlat.size(), m_stream);
    if (!m_geomInstBuffer) m_geomInstBuffer = std::make_unique<cudau::Buffer<GeometryInstance>>();
    m_geomInstBuffer->initialize(m_cudaContext, cudau::BufferType::Device, m_geometryInstances.size());
    m_geomInstBuffer->copyToDevice(m_geometryInstances.data(), m_geometryInstances.size(), m_stream);
    if (!m_instBuffer) m_instBuffer = std::make_unique<cudau::Buffer<Instance>>();
    m_instBuffer->initialize(m_cudaContext, cudau::BufferType::Device, m_instances.size());
    m_instBuffer->copyToDevice(m_instances.data(), m_instances.size(), m_stream);
    if (!m_materialBuffer) m_materialBuffer = std::make_unique<cudau::Buffer<SurfaceMaterialDescriptor>>();
    m_materialBuffer->initialize(m_cudaContext, cudau::BufferType::Device, m_materials.size());
    
#ifdef VLR_DEBUG_MATERIAL
    VLR_DEBUG_PRINTF("\n[Material Debug] updateToGPU: Uploading %zu materials to GPU\n", m_materials.size());
    for (size_t i = 0; i < m_materials.size(); ++i) {
        const SurfaceMaterialDescriptor& mat = m_materials[i];
        const float* dataAsFloat = reinterpret_cast<const float*>(mat.data);
        VLR_DEBUG_PRINTF("  Material[%zu]: bsdfProcedureSetIndex=%u\n", i, mat.bsdfProcedureSetIndex);
        VLR_DEBUG_PRINTF("    Albedo: (%.3f, %.3f, %.3f)\n", 
            dataAsFloat[MaterialDataLayout::AlbedoR],
            dataAsFloat[MaterialDataLayout::AlbedoG],
            dataAsFloat[MaterialDataLayout::AlbedoB]);
        VLR_DEBUG_PRINTF("    Roughness: %.3f, IOR: %.3f\n", 
            dataAsFloat[MaterialDataLayout::Roughness],
            dataAsFloat[MaterialDataLayout::IOR]);
        VLR_DEBUG_PRINTF("    Eta: (%.3f, %.3f, %.3f)\n",
            dataAsFloat[MaterialDataLayout::EtaR],
            dataAsFloat[MaterialDataLayout::EtaG],
            dataAsFloat[MaterialDataLayout::EtaB]);
        VLR_DEBUG_PRINTF("    Kappa: (%.3f, %.3f, %.3f)\n",
            dataAsFloat[MaterialDataLayout::KappaR],
            dataAsFloat[MaterialDataLayout::KappaG],
            dataAsFloat[MaterialDataLayout::KappaB]);
    }
#endif
    
    m_materialBuffer->copyToDevice(m_materials.data(), m_materials.size(), m_stream);

    // 确保材质纹理数组大小并上传到 GPU
    ensureMaterialTextureArraysSize(m_materialAlbedoTextureIndices, m_materialRoughnessTextureIndices,
        m_materialMetallicTextureIndices, m_materialNormalMapIndices, m_materialTextureParams, m_materials.size());
    if (!m_materialAlbedoTextureIndices.empty()) {
        if (!m_materialAlbedoTextureIndicesBuffer) m_materialAlbedoTextureIndicesBuffer = std::make_unique<cudau::Buffer<uint32_t>>();
        m_materialAlbedoTextureIndicesBuffer->initialize(m_cudaContext, cudau::BufferType::Device, m_materialAlbedoTextureIndices.size());
        m_materialAlbedoTextureIndicesBuffer->copyToDevice(m_materialAlbedoTextureIndices.data(), m_materialAlbedoTextureIndices.size(), m_stream);
        if (!m_materialRoughnessTextureIndicesBuffer) m_materialRoughnessTextureIndicesBuffer = std::make_unique<cudau::Buffer<uint32_t>>();
        m_materialRoughnessTextureIndicesBuffer->initialize(m_cudaContext, cudau::BufferType::Device, m_materialRoughnessTextureIndices.size());
        m_materialRoughnessTextureIndicesBuffer->copyToDevice(m_materialRoughnessTextureIndices.data(), m_materialRoughnessTextureIndices.size(), m_stream);
        if (!m_materialMetallicTextureIndicesBuffer) m_materialMetallicTextureIndicesBuffer = std::make_unique<cudau::Buffer<uint32_t>>();
        m_materialMetallicTextureIndicesBuffer->initialize(m_cudaContext, cudau::BufferType::Device, m_materialMetallicTextureIndices.size());
        m_materialMetallicTextureIndicesBuffer->copyToDevice(m_materialMetallicTextureIndices.data(), m_materialMetallicTextureIndices.size(), m_stream);
        if (!m_materialNormalMapIndicesBuffer) m_materialNormalMapIndicesBuffer = std::make_unique<cudau::Buffer<uint32_t>>();
        m_materialNormalMapIndicesBuffer->initialize(m_cudaContext, cudau::BufferType::Device, m_materialNormalMapIndices.size());
        m_materialNormalMapIndicesBuffer->copyToDevice(m_materialNormalMapIndices.data(), m_materialNormalMapIndices.size(), m_stream);
        if (!m_materialTextureParamsBuffer) m_materialTextureParamsBuffer = std::make_unique<cudau::Buffer<shared::MaterialTextureParams>>();
        m_materialTextureParamsBuffer->initialize(m_cudaContext, cudau::BufferType::Device, m_materialTextureParams.size());
        m_materialTextureParamsBuffer->copyToDevice(m_materialTextureParams.data(), m_materialTextureParams.size(), m_stream);
    }

    if (!m_lightInstIndicesBuffer) m_lightInstIndicesBuffer = std::make_unique<cudau::Buffer<uint32_t>>();
    m_lightInstIndicesBuffer->initialize(m_cudaContext, cudau::BufferType::Device, m_lightInstIndices.size());
    if (!m_lightInstIndices.empty())
        m_lightInstIndicesBuffer->copyToDevice(m_lightInstIndices.data(), m_lightInstIndices.size(), m_stream);

    // 上传环境光 CDF 表
    if (!m_envCdfTheta.empty() && !m_envCdfPhi.empty()) {
        if (!m_envCdfThetaBuffer) m_envCdfThetaBuffer = std::make_unique<cudau::Buffer<float>>();
        m_envCdfThetaBuffer->initialize(m_cudaContext, cudau::BufferType::Device, m_envCdfTheta.size());
        m_envCdfThetaBuffer->copyToDevice(m_envCdfTheta.data(), m_envCdfTheta.size(), m_stream);
        if (!m_envCdfPhiBuffer) m_envCdfPhiBuffer = std::make_unique<cudau::Buffer<float>>();
        m_envCdfPhiBuffer->initialize(m_cudaContext, cudau::BufferType::Device, m_envCdfPhi.size());
        m_envCdfPhiBuffer->copyToDevice(m_envCdfPhi.data(), m_envCdfPhi.size(), m_stream);
    }
    if (!m_envTextureData.empty()) {
        if (!m_envTextureBuffer) m_envTextureBuffer = std::make_unique<cudau::Buffer<float>>();
        m_envTextureBuffer->initialize(m_cudaContext, cudau::BufferType::Device, m_envTextureData.size());
        m_envTextureBuffer->copyToDevice(m_envTextureData.data(), m_envTextureData.size(), m_stream);
    }

    CUDA_CHECK(cudaStreamSynchronize(m_stream));
}

void Scene::updateToGPU() {
    computeSceneBounds();
    auto agg = aggregateMeshData();
    uploadAggregatedData(agg);
}

// ============================================================================
// TaskFlow DAG 编排：GAS + Bounds + Aggregate 并行 -> Upload + IAS
// ============================================================================

void Scene::prepareSceneParallel() {
    VLR_PROFILE_BEGIN(_wall);

    tf::Executor executor;
    tf::Taskflow taskflow;

    AggregatedMeshData agg;
#ifdef VLR_PROFILE_SCENE_PREPARE
    double gasMs = 0, boundsMs = 0, aggMs = 0, uploadMs = 0, iasMs = 0;
#endif

    auto gasTask = taskflow.emplace([&]() {
        VLR_PROFILE_BEGIN(_t);
        buildGeometryAccelerationStructures();
        VLR_PROFILE_END(_t, gasMs);
    }).name("buildGAS");

    auto boundsTask = taskflow.emplace([&]() {
        VLR_PROFILE_BEGIN(_t);
        computeSceneBounds();
        VLR_PROFILE_END(_t, boundsMs);
    }).name("computeBounds");

    auto aggTask = taskflow.emplace([&]() {
        VLR_PROFILE_BEGIN(_t);
        agg = aggregateMeshData();
        VLR_PROFILE_END(_t, aggMs);
    }).name("aggregateData");

    auto uploadTask = taskflow.emplace([&]() {
        VLR_PROFILE_BEGIN(_t);
        uploadAggregatedData(agg);
        VLR_PROFILE_END(_t, uploadMs);
    }).name("uploadGPU");

    boundsTask.precede(uploadTask);
    aggTask.precede(uploadTask);

    auto iasTask = taskflow.emplace([&]() {
        VLR_PROFILE_BEGIN(_t);
        buildInstanceAccelerationStructure();
        VLR_PROFILE_END(_t, iasMs);
    }).name("buildIAS");

    gasTask.precede(iasTask);
    uploadTask.precede(iasTask);

    executor.run(taskflow).wait();

    VLR_PROFILE_END_NEW(_wall, wallMs);

#ifdef VLR_PROFILE_SCENE_PREPARE
    double serialSum = gasMs + boundsMs + aggMs + uploadMs + iasMs;
    printf("\n");
    printf("[VLR-Profile] ===== Scene Prepare Timing =====\n");
    printf("[VLR-Profile]   buildGAS         : %8.2f ms  (%zu meshes, %zu valid)\n",
           gasMs, m_meshes.size(), m_gasHandles.size());
    printf("[VLR-Profile]   computeBounds    : %8.2f ms  (%zu instances)\n",
           boundsMs, m_instanceRecords.size());
    printf("[VLR-Profile]   aggregateData    : %8.2f ms\n", aggMs);
    printf("[VLR-Profile]   uploadGPU        : %8.2f ms\n", uploadMs);
    printf("[VLR-Profile]   buildIAS         : %8.2f ms  (%zu GAS handles)\n",
           iasMs, m_gasHandles.size());
    printf("[VLR-Profile]   ---------------------------------\n");
    printf("[VLR-Profile]   Serial sum       : %8.2f ms\n", serialSum);
    printf("[VLR-Profile]   Wall clock       : %8.2f ms\n", wallMs);
    printf("[VLR-Profile]   Parallel speedup : %8.2fx\n",
           wallMs > 0 ? serialSum / wallMs : 0.0);
    printf("[VLR-Profile] ================================\n\n");
    fflush(stdout);
#endif
}

// ============================================================================
// 内存释放
// ============================================================================

void Scene::releaseHostMeshData() {
    size_t freedBytes = 0;
    for (auto& mesh : m_meshes) {
        freedBytes += mesh.positions.capacity() * sizeof(Point3D);
        freedBytes += mesh.normals.capacity() * sizeof(Normal3D);
        freedBytes += mesh.texCoords.capacity() * sizeof(TexCoord2D);
        freedBytes += mesh.triangles.capacity() * sizeof(Triangle);
        std::vector<Point3D>().swap(mesh.positions);
        std::vector<Normal3D>().swap(mesh.normals);
        std::vector<TexCoord2D>().swap(mesh.texCoords);
        std::vector<Triangle>().swap(mesh.triangles);
    }
    std::cout << "[Scene] releaseHostMeshData: 释放 CPU 端网格数据 "
              << (freedBytes / (1024 * 1024)) << " MB\n";
}

// ============================================================================
// 数据访问
// ============================================================================

const shared::GeometryInstance* Scene::getGeomInstBuffer() const {
    return m_geomInstBuffer ? m_geomInstBuffer->getDevicePointer() : nullptr;
}

const shared::Instance* Scene::getInstBuffer() const {
    return m_instBuffer ? m_instBuffer->getDevicePointer() : nullptr;
}

const shared::SurfaceMaterialDescriptor* Scene::getMaterialBuffer() const {
    return m_materialBuffer ? m_materialBuffer->getDevicePointer() : nullptr;
}

const uint32_t* Scene::getMaterialAlbedoTextureIndices() const {
    return m_materialAlbedoTextureIndicesBuffer ? m_materialAlbedoTextureIndicesBuffer->getDevicePointer() : nullptr;
}

const uint32_t* Scene::getMaterialRoughnessTextureIndices() const {
    return m_materialRoughnessTextureIndicesBuffer ? m_materialRoughnessTextureIndicesBuffer->getDevicePointer() : nullptr;
}

const uint32_t* Scene::getMaterialMetallicTextureIndices() const {
    return m_materialMetallicTextureIndicesBuffer ? m_materialMetallicTextureIndicesBuffer->getDevicePointer() : nullptr;
}

const uint32_t* Scene::getMaterialNormalMapIndices() const {
    return m_materialNormalMapIndicesBuffer ? m_materialNormalMapIndicesBuffer->getDevicePointer() : nullptr;
}

const shared::MaterialTextureParams* Scene::getMaterialTextureParams() const {
    return m_materialTextureParamsBuffer ? m_materialTextureParamsBuffer->getDevicePointer() : nullptr;
}

const Point3D* Scene::getVertexPositions() const {
    return m_vertexPositionBuffer ? m_vertexPositionBuffer->getDevicePointer() : nullptr;
}

const Normal3D* Scene::getVertexNormals() const {
    return m_vertexNormalBuffer ? m_vertexNormalBuffer->getDevicePointer() : nullptr;
}

const TexCoord2D* Scene::getVertexTexCoords() const {
    return m_vertexTexCoordBuffer ? m_vertexTexCoordBuffer->getDevicePointer() : nullptr;
}

const uint32_t* Scene::getLightInstIndices() const {
    return m_lightInstIndicesBuffer ? m_lightInstIndicesBuffer->getDevicePointer() : nullptr;
}

uint32_t Scene::getNumGeomInsts() const { return static_cast<uint32_t>(m_geometryInstances.size()); }
uint32_t Scene::getNumInstances() const { return static_cast<uint32_t>(m_instances.size()); }
uint32_t Scene::getNumMaterials() const { return static_cast<uint32_t>(m_materials.size()); }
uint32_t Scene::getNumLightInsts() const { return static_cast<uint32_t>(m_lightInstIndices.size()); }
uint32_t Scene::getEnvLightInstIndex() const { return m_envLightInstIndex.value_or(0xFFFFFFFF); }

shared::EnvironmentImportanceMap Scene::getEnvImportanceMap() const {
    shared::EnvironmentImportanceMap m;
    m.cdfTheta = nullptr;
    m.cdfPhi = nullptr;
    m.thetaRes = 0;
    m.phiRes = 0;
    m.totalLuminance = 1.0f;
    if (m_envCdfThetaBuffer && m_envCdfPhiBuffer && !m_envCdfTheta.empty() && !m_envCdfPhi.empty()) {
        m.cdfTheta = m_envCdfThetaBuffer->getDevicePointer();
        m.cdfPhi = m_envCdfPhiBuffer->getDevicePointer();
        m.thetaRes = m_envTextureHeight;
        m.phiRes = m_envTextureWidth;
        m.totalLuminance = m_envTotalLuminance;
    }
    return m;
}

OptixTraversableHandle Scene::getTopGroup() const { return m_topGroup; }
const shared::CameraDescriptor& Scene::getCamera() const { return m_camera; }
const shared::SceneBounds& Scene::getSceneBounds() const { return m_sceneBounds; }

void Scene::computeLightImportanceWeights(std::vector<float>& weights, std::vector<float>& cdf) const {
    weights.clear();
    cdf.clear();
    const uint32_t numLights = static_cast<uint32_t>(m_lightInstIndices.size());
    if (numLights == 0) return;

    weights.resize(numLights, 1e-6f);
    for (uint32_t i = 0; i < numLights; ++i) {
        const uint32_t instIndex = m_lightInstIndices[i];
        if (instIndex >= m_instances.size() || instIndex >= m_instanceRecords.size())
            continue;
        const Instance& inst = m_instances[instIndex];
        const InstanceRecord& rec = m_instanceRecords[instIndex];
        if (rec.geomInstIndices.empty()) continue;

        const uint32_t geomInstIndex = rec.geomInstIndices[0];
        if (geomInstIndex >= m_geometryInstances.size()) continue;
        const GeometryInstance& geomInst = m_geometryInstances[geomInstIndex];
        const uint32_t matIndex = geomInst.materialIndex;
        if (matIndex >= m_materials.size()) continue;

        const SurfaceMaterialDescriptor& mat = m_materials[matIndex];
        const float* d = reinterpret_cast<const float*>(mat.data);
        const float er = d[MaterialDataLayout::EmissionR];
        const float eg = d[MaterialDataLayout::EmissionG];
        const float eb = d[MaterialDataLayout::EmissionB];
        const float luminance = (er + eg + eb) / 3.0f;
        if (luminance < 1e-10f) continue;

        float power = luminance;
        if (geomInst.geomType == GeometryType_TriangleMesh && rec.meshId < m_meshes.size()) {
            float totalArea = 0.0f;
            const TriangleMeshData& mesh = m_meshes[rec.meshId];
            for (const Triangle& tri : mesh.triangles)
                totalArea += tri.area;
            power = luminance * std::max(totalArea, 1e-10f);
        }
        weights[i] = std::max(power, 1e-6f);
    }

    float sum = 0.0f;
    for (float w : weights) sum += w;
    if (sum < 1e-10f) {
        for (float& w : weights) w = 1.0f / numLights;
        sum = 1.0f;
    }
    cdf.resize(numLights + 1);
    cdf[0] = 0.0f;
    for (uint32_t i = 0; i < numLights; ++i)
        cdf[i + 1] = cdf[i] + weights[i] / sum;
    cdf[numLights] = 1.0f;
}

}  // namespace vlr
