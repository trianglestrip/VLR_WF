// ============================================================================
// VLR ???? - ??
//
// ?????? Scene ??????????????????????????
// ?? OptiX ??????? GPU ??????
//
// ???VLR ?????
// ??????026-03-07
// ???CUDA 13.1, OptiX 8.0.0, VS2022
// ============================================================================

// ????????????????
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

#ifdef _WIN32
#undef min
#undef max
#endif

#define OPTIX_CHECK(call) ::vlr::optixu::checkError(call, #call, __FILE__, __LINE__)
#define CUDA_CHECK(call) ::vlr::cudau::checkError(call, #call, __FILE__, __LINE__)

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
// ?????????
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
// ????
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
// ????
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
// ????
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
    // ?? VLR?IAS ???? InstanceRecord.transform????????
    if (instanceId < m_instanceRecords.size())
        m_instanceRecords[instanceId].transform = transform;
}

// ============================================================================
// ??????
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
// ????
// ============================================================================

void Scene::addAreaLight(const AreaLightParams& params) {
    m_lightInstIndices.push_back(params.instIndex);
}

void Scene::addPointLight(const PointLightParams& params) {
    // ????????
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
    
    // ??????????
    GeometryInstance geomInst;
    memset(&geomInst, 0, sizeof(geomInst));
    geomInst.geomType = GeometryType_Point;
    geomInst.materialIndex = materialIndex;
    geomInst.importance = 1.0f;
    geomInst.progDecodeHitPoint = -1;
    geomInst.progSampleLightPosition = -1;
    geomInst.nodeNormal = -1;
    geomInst.nodeTangent = -1;
    
    // ???????? asPoint ??
    geomInst.asPoint.x = params.position.x;
    geomInst.asPoint.y = params.position.y;
    geomInst.asPoint.z = params.position.z;
    
    uint32_t geomInstIndex = static_cast<uint32_t>(m_geometryInstances.size());
    m_geometryInstances.push_back(geomInst);
    
    // ??????????????
    shared::Instance inst;
    memset(&inst, 0, sizeof(inst));
    
    // ??????????????
    uint32_t* geomIndices = new uint32_t[1];
    geomIndices[0] = geomInstIndex;
    inst.geomInstIndices = geomIndices;
    inst.numGeomInsts = 1;
    inst.transform = ReferenceFrame(Vector3D(1, 0, 0), Normal3D(0, 1, 0));
    inst.rotationPhi = 0.0f;
    inst.lightGeomInstDistribution = 0;
    
    uint32_t instIndex = static_cast<uint32_t>(m_instances.size());
    m_instances.push_back(inst);
    
    // ??????
    InstanceRecord rec;
    rec.meshId = 0;
    rec.materialId = materialIndex;
    rec.geomInstIndex = geomInstIndex;
    rec.transform = InstanceTransform();
    rec.geomInstIndices.push_back(geomInstIndex);
    m_instanceRecords.push_back(rec);
    
    // ????????
    m_lightInstIndices.push_back(instIndex);
}

void Scene::addDirectionalLight(const Vector3D& direction, const SampledSpectrum& radiance) {
    // ????????
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
    
    // ????????????????Instance.transform.z ??
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
    
    // ??????????????transform.z ??
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
    
    // ????????
    m_lightInstIndices.push_back(instIndex);
}

void Scene::setEnvironmentLight(const EnvironmentLightParams& params) {
    // ????????
    // - ??????NEE????m_lightInstIndices??????????
    // - ?? miss ??? processEnvironmentHit ??????
    // - ?????GeometryType_InfiniteSphere ????miss shader ??
    
    // ????????????????
    if (m_envLightInstIndex.has_value() && m_envLightInstIndex.value() < m_instances.size()) {
        // ??m_lightInstIndices ????????
        auto it = std::find(m_lightInstIndices.begin(), m_lightInstIndices.end(), m_envLightInstIndex.value());
        if (it != m_lightInstIndices.end())
            m_lightInstIndices.erase(it);
    }
    
    // ????????
    SurfaceMaterialDescriptor mat;
    memset(&mat, 0, sizeof(mat));
    uint32_t bsdfType = static_cast<uint32_t>(BSDFType_Lambert);
    mat.bsdfProcedureSetIndex = bsdfType;
    mat.edfProcedureSetIndex = 0xFFFFFFFF;
    mat.data[MaterialDataLayout::BSDFType] = *reinterpret_cast<const uint32_t*>(&bsdfType);
    
    // ??????
    if (params.useConstant) {
        mat.data[MaterialDataLayout::EmissionR] = *reinterpret_cast<const uint32_t*>(&params.constantColor.values[0]);
        mat.data[MaterialDataLayout::EmissionG] = *reinterpret_cast<const uint32_t*>(&params.constantColor.values[1]);
        mat.data[MaterialDataLayout::EmissionB] = *reinterpret_cast<const uint32_t*>(&params.constantColor.values[2]);
    } else {
        // ?????????????? 1.0??????????
        float defaultEmission = 1.0f;
        mat.data[MaterialDataLayout::EmissionR] = *reinterpret_cast<const uint32_t*>(&defaultEmission);
        mat.data[MaterialDataLayout::EmissionG] = *reinterpret_cast<const uint32_t*>(&defaultEmission);
        mat.data[MaterialDataLayout::EmissionB] = *reinterpret_cast<const uint32_t*>(&defaultEmission);
    }
    
    uint32_t materialIndex = static_cast<uint32_t>(m_materials.size());
    m_materials.push_back(mat);
    
    // ??????????
    GeometryInstance geomInst;
    memset(&geomInst, 0, sizeof(geomInst));
    geomInst.geomType = GeometryType_InfiniteSphere;
    geomInst.materialIndex = materialIndex;
    geomInst.importance = 1.0f;
    geomInst.progDecodeHitPoint = -1;
    geomInst.progSampleLightPosition = -1;
    geomInst.nodeNormal = -1;
    geomInst.nodeTangent = -1;
    
    // ????????????
    if (!params.useConstant && params.textureData && params.textureWidth > 0 && params.textureHeight > 0) {
        geomInst.asInfSphere.importanceMap = 1;  // 1 = ????????
        // ??????
        size_t numPixels = static_cast<size_t>(params.textureWidth) * params.textureHeight * 3;
        m_envTextureData.resize(numPixels);
        std::memcpy(m_envTextureData.data(), params.textureData, numPixels * sizeof(float));
        m_envTextureWidth = params.textureWidth;
        m_envTextureHeight = params.textureHeight;
        // ????????
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
    
    // ????
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
    
    // ??????
    InstanceRecord rec;
    rec.meshId = 0;
    rec.materialId = materialIndex;
    rec.geomInstIndex = geomInstIndex;
    rec.transform = InstanceTransform();
    rec.transform.rotationRadians = params.rotation;
    rec.geomInstIndices.push_back(geomInstIndex);
    m_instanceRecords.push_back(rec);
    
    // ??????m_lightInstIndices ????NEE ??????
    m_lightInstIndices.push_back(instIndex);
    m_envLightInstIndex = instIndex;
}

// ============================================================================
// ????
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
// ???????
// ============================================================================

void Scene::buildGeometryAccelerationStructures() {
    for (void* p : m_gasOutputBuffers) cudaFree(p);
    m_gasOutputBuffers.clear();
    m_gasHandles.clear();
    m_gasHandles.reserve(m_meshes.size());
    
    OptixAccelBuildOptions accelOptions = {
        .buildFlags = OPTIX_BUILD_FLAG_NONE,
        .operation = OPTIX_BUILD_OPERATION_BUILD
    };
    
    for (size_t meshIdx = 0; meshIdx < m_meshes.size(); ++meshIdx) {
        const TriangleMeshData& mesh = m_meshes[meshIdx];
        if (mesh.positions.empty() || mesh.triangles.empty()) continue;

        std::vector<float> vertices(mesh.positions.size() * 3);
        for (size_t i = 0; i < mesh.positions.size(); ++i) {
            vertices[i * 3 + 0] = mesh.positions[i].x;
            vertices[i * 3 + 1] = mesh.positions[i].y;
            vertices[i * 3 + 2] = mesh.positions[i].z;
        }

        
        CUdeviceptr d_vertices = 0;
        CUDA_CHECK(cudaMalloc(reinterpret_cast<void**>(&d_vertices), vertices.size() * sizeof(float)));
        CUDA_CHECK(cudaMemcpy(reinterpret_cast<void*>(d_vertices), vertices.data(), vertices.size() * sizeof(float), cudaMemcpyHostToDevice));
        
        std::vector<uint32_t> ind(mesh.triangles.size() * 3);
        for (size_t i = 0; i < mesh.triangles.size(); ++i) {
            ind[i * 3 + 0] = mesh.triangles[i].indices[0];
            ind[i * 3 + 1] = mesh.triangles[i].indices[1];
            ind[i * 3 + 2] = mesh.triangles[i].indices[2];
        }
        
        CUdeviceptr d_indices = 0;
        CUDA_CHECK(cudaMalloc(reinterpret_cast<void**>(&d_indices), ind.size() * sizeof(uint32_t)));
        CUDA_CHECK(cudaMemcpy(reinterpret_cast<void*>(d_indices), ind.data(), ind.size() * sizeof(uint32_t), cudaMemcpyHostToDevice));
        
        OptixBuildInput triangleInput = {};
        triangleInput.type = OPTIX_BUILD_INPUT_TYPE_TRIANGLES;
        triangleInput.triangleArray.vertexFormat = OPTIX_VERTEX_FORMAT_FLOAT3;
        triangleInput.triangleArray.vertexStrideInBytes = sizeof(float) * 3;
        triangleInput.triangleArray.numVertices = static_cast<uint32_t>(mesh.positions.size());
        triangleInput.triangleArray.vertexBuffers = &d_vertices;
        triangleInput.triangleArray.indexFormat = OPTIX_INDICES_FORMAT_UNSIGNED_INT3;
        triangleInput.triangleArray.indexStrideInBytes = sizeof(uint32_t) * 3;
        triangleInput.triangleArray.numIndexTriplets = static_cast<uint32_t>(mesh.triangles.size());
        triangleInput.triangleArray.indexBuffer = d_indices;
        
        // ?????flags ????nullptr???????? flags ??
        static const uint32_t triangleInputFlags[1] = { OPTIX_GEOMETRY_FLAG_NONE };
        triangleInput.triangleArray.flags = triangleInputFlags;
        triangleInput.triangleArray.numSbtRecords = 1;
        
        OptixAccelBufferSizes gasBufferSizes;
        
        OPTIX_CHECK(optixAccelComputeMemoryUsage(m_optixContext, &accelOptions, &triangleInput, 1, &gasBufferSizes));
        
        void* d_temp = nullptr;
        void* d_output = nullptr;
        CUDA_CHECK(cudaMalloc(&d_temp, gasBufferSizes.tempSizeInBytes));
        CUDA_CHECK(cudaMalloc(&d_output, gasBufferSizes.outputSizeInBytes));
        OptixTraversableHandle gasHandle = 0;
        OPTIX_CHECK(optixAccelBuild(m_optixContext, m_stream, &accelOptions, &triangleInput, 1,
            reinterpret_cast<CUdeviceptr>(d_temp), gasBufferSizes.tempSizeInBytes,
            reinterpret_cast<CUdeviceptr>(d_output), gasBufferSizes.outputSizeInBytes,
            &gasHandle, nullptr, 0));
        cudaFree(reinterpret_cast<void*>(d_vertices));
        cudaFree(reinterpret_cast<void*>(d_indices));
        cudaFree(d_temp);
        m_gasOutputBuffers.push_back(d_output);
        m_gasHandles.push_back(gasHandle);
    }
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
        // ??????????????????????IAS??????????NEE ????
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
        // OptiX transform ?????row-major??x4 ????
        // Row 0: [m00, m01, m02, tx]
        // Row 1: [m10, m11, m12, ty]
        // Row 2: [m20, m21, m22, tz]
        // ReferenceFrame ??x, y, z ???????????????????
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
// GPU ????
// ============================================================================

void Scene::computeSceneBounds() {
    m_sceneBounds.minPoint = Point3D(1e10f, 1e10f, 1e10f);
    m_sceneBounds.maxPoint = Point3D(-1e10f, -1e10f, -1e10f);
    for (size_t i = 0; i < m_instanceRecords.size(); ++i) {
        const InstanceRecord& instRec = m_instanceRecords[i];
        const TriangleMeshData& mesh = m_meshes[instRec.meshId];
        const Instance& inst = m_instances[i];
        for (const auto& p : mesh.positions) {
            Point3D wp = inst.transform.toWorld(p);
            wp.x += instRec.transform.position.x;
            wp.y += instRec.transform.position.y;
            wp.z += instRec.transform.position.z;
            m_sceneBounds.minPoint.x = (std::min)(m_sceneBounds.minPoint.x, wp.x);
            m_sceneBounds.minPoint.y = (std::min)(m_sceneBounds.minPoint.y, wp.y);
            m_sceneBounds.minPoint.z = (std::min)(m_sceneBounds.minPoint.z, wp.z);
            m_sceneBounds.maxPoint.x = (std::max)(m_sceneBounds.maxPoint.x, wp.x);
            m_sceneBounds.maxPoint.y = (std::max)(m_sceneBounds.maxPoint.y, wp.y);
            m_sceneBounds.maxPoint.z = (std::max)(m_sceneBounds.maxPoint.z, wp.z);
        }
    }
}

void Scene::updateToGPU() {
    computeSceneBounds();
    std::vector<Point3D> allPositions;
    std::vector<Normal3D> allNormals;
    std::vector<TexCoord2D> allTexCoords;
    std::vector<Triangle> allTriangles;
    std::vector<uint32_t> geomInstTriangleOffsets;
    uint32_t vertexOffset = 0;
    for (const auto& mesh : m_meshes) {
        for (const auto& p : mesh.positions) allPositions.push_back(p);
        for (const auto& n : mesh.normals) allNormals.push_back(n);
        for (const auto& uv : mesh.texCoords) allTexCoords.push_back(uv);
        geomInstTriangleOffsets.push_back(static_cast<uint32_t>(allTriangles.size()));
        for (auto tri : mesh.triangles) {
            tri.indices[0] += vertexOffset;
            tri.indices[1] += vertexOffset;
            tri.indices[2] += vertexOffset;
            allTriangles.push_back(tri);
        }
        vertexOffset += static_cast<uint32_t>(mesh.positions.size());
    }
    if (!m_vertexPositionBuffer) m_vertexPositionBuffer = std::make_unique<cudau::Buffer<Point3D>>();
    m_vertexPositionBuffer->initialize(m_cudaContext, cudau::BufferType::Device, allPositions.size());
    m_vertexPositionBuffer->copyToDevice(allPositions.data(), allPositions.size(), m_stream);
    if (!allNormals.empty()) {
        if (!m_vertexNormalBuffer) m_vertexNormalBuffer = std::make_unique<cudau::Buffer<Normal3D>>();
        m_vertexNormalBuffer->initialize(m_cudaContext, cudau::BufferType::Device, allNormals.size());
        m_vertexNormalBuffer->copyToDevice(allNormals.data(), allNormals.size(), m_stream);
    }
    if (!allTexCoords.empty()) {
        if (!m_vertexTexCoordBuffer) m_vertexTexCoordBuffer = std::make_unique<cudau::Buffer<TexCoord2D>>();
        m_vertexTexCoordBuffer->initialize(m_cudaContext, cudau::BufferType::Device, allTexCoords.size());
        m_vertexTexCoordBuffer->copyToDevice(allTexCoords.data(), allTexCoords.size(), m_stream);
    }
    if (!m_triangleBuffer) m_triangleBuffer = std::make_unique<cudau::Buffer<Triangle>>();
    m_triangleBuffer->initialize(m_cudaContext, cudau::BufferType::Device, allTriangles.size());
    m_triangleBuffer->copyToDevice(allTriangles.data(), allTriangles.size(), m_stream);
    for (size_t g = 0; g < m_geometryInstances.size(); ++g) {
        // ??????????triangleBuffer?Point/Directional/InfiniteSphere ?? union ????
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

    // ????????????????????????
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

    // ???????????
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

// ============================================================================
// ????
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
