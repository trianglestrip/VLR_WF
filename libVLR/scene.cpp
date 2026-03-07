// ============================================================================
// VLR 场景管理 - 实现
//
// 本文件实现了 Scene 类的全部逻辑，包括几何、材质、光源、实例、相机管理，
// 以及 OptiX 加速结构构建与 GPU 数据上传。
//
// 作者：VLR 开发团队
// 创建日期：2026-03-07
// 环境：CUDA 13.1, OptiX 8.0.0, VS2022
// ============================================================================

#include "scene.h"
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

namespace vlr {

using namespace shared;

// ============================================================================
// 构造函数与析构函数
// ============================================================================

Scene::Scene(OptixDeviceContext optixContext, cudaStream_t stream, cudau::Context* cudaContext)
    : m_optixContext(optixContext)
    , m_stream(stream)
    , m_cudaContext(cudaContext)
    , m_envLightInstIndex(0xFFFFFFFF)
    , m_topGroup(0)
    , m_accelOutputBuffer(nullptr)
    , m_accelTempBuffer(nullptr)
    , m_accelOutputSize(0)
    , m_accelTempSize(0)
    , m_geomInstBuffer(nullptr)
    , m_instBuffer(nullptr)
    , m_instGeomIndicesBuffer(nullptr)
    , m_materialBuffer(nullptr)
    , m_vertexPositionBuffer(nullptr)
    , m_vertexNormalBuffer(nullptr)
    , m_vertexTexCoordBuffer(nullptr)
    , m_triangleBuffer(nullptr)
    , m_lightInstIndicesBuffer(nullptr)
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
    delete m_geomInstBuffer;
    delete m_instBuffer;
    delete m_instGeomIndicesBuffer;
    delete m_materialBuffer;
    delete m_vertexPositionBuffer;
    delete m_vertexNormalBuffer;
    delete m_vertexTexCoordBuffer;
    delete m_triangleBuffer;
    delete m_lightInstIndicesBuffer;
    m_geomInstBuffer = nullptr;
    m_instBuffer = nullptr;
    m_instGeomIndicesBuffer = nullptr;
    m_materialBuffer = nullptr;
    m_vertexPositionBuffer = nullptr;
    m_vertexNormalBuffer = nullptr;
    m_vertexTexCoordBuffer = nullptr;
    m_triangleBuffer = nullptr;
    m_lightInstIndicesBuffer = nullptr;
}

// ============================================================================
// 几何管理
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
// 材质管理
// ============================================================================

uint32_t Scene::createMaterial(
    uint32_t bsdfType,
    float albedoR, float albedoG, float albedoB,
    float roughness,
    float emissionR, float emissionG, float emissionB)
{
    SurfaceMaterialDescriptor mat;
    memset(&mat, 0, sizeof(mat));
    mat.bsdfProcedureSetIndex = bsdfType;
    mat.edfProcedureSetIndex = 0xFFFFFFFF;
    mat.data[MaterialDataLayout::BSDFType] = *reinterpret_cast<uint32_t*>(&bsdfType);
    mat.data[MaterialDataLayout::AlbedoR] = *reinterpret_cast<uint32_t*>(&albedoR);
    mat.data[MaterialDataLayout::AlbedoG] = *reinterpret_cast<uint32_t*>(&albedoG);
    mat.data[MaterialDataLayout::AlbedoB] = *reinterpret_cast<uint32_t*>(&albedoB);
    mat.data[MaterialDataLayout::Roughness] = *reinterpret_cast<uint32_t*>(&roughness);
    mat.data[MaterialDataLayout::EmissionR] = *reinterpret_cast<uint32_t*>(&emissionR);
    mat.data[MaterialDataLayout::EmissionG] = *reinterpret_cast<uint32_t*>(&emissionG);
    mat.data[MaterialDataLayout::EmissionB] = *reinterpret_cast<uint32_t*>(&emissionB);
    m_materials.push_back(mat);
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
    SurfaceMaterialDescriptor mat;
    memset(&mat, 0, sizeof(mat));
    mat.bsdfProcedureSetIndex = bsdfType;
    mat.edfProcedureSetIndex = 0xFFFFFFFF;
    mat.data[MaterialDataLayout::BSDFType] = *reinterpret_cast<uint32_t*>(&bsdfType);
    mat.data[MaterialDataLayout::AlbedoR] = *reinterpret_cast<uint32_t*>(&albedoR);
    mat.data[MaterialDataLayout::AlbedoG] = *reinterpret_cast<uint32_t*>(&albedoG);
    mat.data[MaterialDataLayout::AlbedoB] = *reinterpret_cast<uint32_t*>(&albedoB);
    mat.data[MaterialDataLayout::Roughness] = *reinterpret_cast<uint32_t*>(&roughness);
    mat.data[MaterialDataLayout::Metallic] = *reinterpret_cast<uint32_t*>(&metallic);
    mat.data[MaterialDataLayout::IOR] = *reinterpret_cast<uint32_t*>(&ior);
    mat.data[MaterialDataLayout::EmissionR] = *reinterpret_cast<uint32_t*>(&emissionR);
    mat.data[MaterialDataLayout::EmissionG] = *reinterpret_cast<uint32_t*>(&emissionG);
    mat.data[MaterialDataLayout::EmissionB] = *reinterpret_cast<uint32_t*>(&emissionB);
    m_materials.push_back(mat);
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
// 实例管理
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
}

// ============================================================================
// 光源管理
// ============================================================================

void Scene::addAreaLight(const AreaLightParams& params) {
    m_lightInstIndices.push_back(params.instIndex);
}

void Scene::addPointLight(const PointLightParams& params) {
    (void)params;
}

void Scene::setEnvironmentLight(const EnvironmentLightParams& params) {
    if (params.useConstant)
        m_envLightInstIndex = 0;
    else
        m_envLightInstIndex = 0xFFFFFFFF;
}

// ============================================================================
// 相机管理
// ============================================================================

void Scene::setCamera(const CameraParams& params) {
    m_camera.position = params.position;
    Vector3D dir = params.lookAt - params.position;
    float len = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
    if (len > 1e-6f) { dir.x /= len; dir.y /= len; dir.z /= len; }
    else { dir = Vector3D(0, 0, 1); }
    Vector3D tangent = Vector3D(params.up.y * dir.z - params.up.z * dir.y, params.up.z * dir.x - params.up.x * dir.z, params.up.x * dir.y - params.up.y * dir.x);
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
// 加速结构构建
// ============================================================================

void Scene::buildGeometryAccelerationStructures() {
    for (void* p : m_gasOutputBuffers) cudaFree(p);
    m_gasOutputBuffers.clear();
    m_gasHandles.clear();
    m_gasHandles.reserve(m_meshes.size());
    
    OptixAccelBuildOptions accelOptions = {};
    accelOptions.buildFlags = OPTIX_BUILD_FLAG_NONE;
    accelOptions.operation = OPTIX_BUILD_OPERATION_BUILD;
    
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
        
        // 关键修复：flags 不能为 nullptr，需要指向有效的 flags 数组
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
        uint32_t gasIdx = (std::min)(rec.meshId, static_cast<uint32_t>(m_gasHandles.size() - 1));
        OptixInstance oi = {};
        oi.instanceId = static_cast<uint32_t>(i);
        oi.sbtOffset = 0;
        oi.visibilityMask = 0xFF;
        oi.flags = OPTIX_INSTANCE_FLAG_NONE;
        oi.traversableHandle = m_gasHandles[gasIdx];
        const ReferenceFrame& rf = m_instances[i].transform;
        const InstanceTransform& it = rec.transform;
        oi.transform[0] = rf.x.x; oi.transform[1] = rf.x.y; oi.transform[2] = rf.x.z; oi.transform[3] = it.position.x;
        oi.transform[4] = rf.y.x; oi.transform[5] = rf.y.y; oi.transform[6] = rf.y.z; oi.transform[7] = it.position.y;
        oi.transform[8] = rf.z.x; oi.transform[9] = rf.z.y; oi.transform[10] = rf.z.z; oi.transform[11] = it.position.z;
        optixInstances.push_back(oi);
    }
    CUdeviceptr d_instances = 0;
    CUDA_CHECK(cudaMalloc(reinterpret_cast<void**>(&d_instances), optixInstances.size() * sizeof(OptixInstance)));
    CUDA_CHECK(cudaMemcpy(reinterpret_cast<void*>(d_instances), optixInstances.data(), optixInstances.size() * sizeof(OptixInstance), cudaMemcpyHostToDevice));
    OptixBuildInput iasInput = {};
    iasInput.type = OPTIX_BUILD_INPUT_TYPE_INSTANCES;
    iasInput.instanceArray.instances = d_instances;
    iasInput.instanceArray.numInstances = static_cast<uint32_t>(optixInstances.size());
    OptixAccelBuildOptions iasOptions = {};
    iasOptions.buildFlags = OPTIX_BUILD_FLAG_NONE;
    iasOptions.operation = OPTIX_BUILD_OPERATION_BUILD;
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
    if (!m_vertexPositionBuffer) m_vertexPositionBuffer = new cudau::Buffer<Point3D>();
    m_vertexPositionBuffer->initialize(m_cudaContext, cudau::BufferType::Device, allPositions.size());
    m_vertexPositionBuffer->copyToDevice(allPositions.data(), allPositions.size(), m_stream);
    if (!allNormals.empty()) {
        if (!m_vertexNormalBuffer) m_vertexNormalBuffer = new cudau::Buffer<Normal3D>();
        m_vertexNormalBuffer->initialize(m_cudaContext, cudau::BufferType::Device, allNormals.size());
        m_vertexNormalBuffer->copyToDevice(allNormals.data(), allNormals.size(), m_stream);
    }
    if (!allTexCoords.empty()) {
        if (!m_vertexTexCoordBuffer) m_vertexTexCoordBuffer = new cudau::Buffer<TexCoord2D>();
        m_vertexTexCoordBuffer->initialize(m_cudaContext, cudau::BufferType::Device, allTexCoords.size());
        m_vertexTexCoordBuffer->copyToDevice(allTexCoords.data(), allTexCoords.size(), m_stream);
    }
    if (!m_triangleBuffer) m_triangleBuffer = new cudau::Buffer<Triangle>();
    m_triangleBuffer->initialize(m_cudaContext, cudau::BufferType::Device, allTriangles.size());
    m_triangleBuffer->copyToDevice(allTriangles.data(), allTriangles.size(), m_stream);
    for (size_t g = 0; g < m_geometryInstances.size(); ++g) {
        uint32_t offset = geomInstTriangleOffsets[(std::min)(g, geomInstTriangleOffsets.size() - 1)];
        m_geometryInstances[g].asTriMesh.triangleBuffer = m_triangleBuffer->getDevicePointer() + offset;
    }
    size_t totalGeomIndices = 0;
    for (const auto& rec : m_instanceRecords) totalGeomIndices += rec.geomInstIndices.size();
    if (!m_instGeomIndicesBuffer) m_instGeomIndicesBuffer = new cudau::Buffer<uint32_t>();
    m_instGeomIndicesBuffer->initialize(m_cudaContext, cudau::BufferType::Device, totalGeomIndices);
    std::vector<uint32_t> geomIndicesFlat;
    uint32_t offset = 0;
    for (size_t i = 0; i < m_instances.size(); ++i) {
        m_instances[i].geomInstIndices = m_instGeomIndicesBuffer->getDevicePointerAt(offset);
        for (uint32_t idx : m_instanceRecords[i].geomInstIndices) geomIndicesFlat.push_back(idx);
        offset += static_cast<uint32_t>(m_instanceRecords[i].geomInstIndices.size());
    }
    m_instGeomIndicesBuffer->copyToDevice(geomIndicesFlat.data(), geomIndicesFlat.size(), m_stream);
    if (!m_geomInstBuffer) m_geomInstBuffer = new cudau::Buffer<GeometryInstance>();
    m_geomInstBuffer->initialize(m_cudaContext, cudau::BufferType::Device, m_geometryInstances.size());
    m_geomInstBuffer->copyToDevice(m_geometryInstances.data(), m_geometryInstances.size(), m_stream);
    if (!m_instBuffer) m_instBuffer = new cudau::Buffer<Instance>();
    m_instBuffer->initialize(m_cudaContext, cudau::BufferType::Device, m_instances.size());
    m_instBuffer->copyToDevice(m_instances.data(), m_instances.size(), m_stream);
    if (!m_materialBuffer) m_materialBuffer = new cudau::Buffer<SurfaceMaterialDescriptor>();
    m_materialBuffer->initialize(m_cudaContext, cudau::BufferType::Device, m_materials.size());
    m_materialBuffer->copyToDevice(m_materials.data(), m_materials.size(), m_stream);
    if (!m_lightInstIndicesBuffer) m_lightInstIndicesBuffer = new cudau::Buffer<uint32_t>();
    m_lightInstIndicesBuffer->initialize(m_cudaContext, cudau::BufferType::Device, m_lightInstIndices.size());
    if (!m_lightInstIndices.empty())
        m_lightInstIndicesBuffer->copyToDevice(m_lightInstIndices.data(), m_lightInstIndices.size(), m_stream);
    CUDA_CHECK(cudaStreamSynchronize(m_stream));
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
uint32_t Scene::getEnvLightInstIndex() const { return m_envLightInstIndex; }
OptixTraversableHandle Scene::getTopGroup() const { return m_topGroup; }
const shared::CameraDescriptor& Scene::getCamera() const { return m_camera; }
const shared::SceneBounds& Scene::getSceneBounds() const { return m_sceneBounds; }

}  // namespace vlr
