// ============================================================================
// VLR 公共 C API 实现
//
// 本文件实现 VLR 的 C 风格公共接口，使用 C++ Context 和 Scene 类包装。
//
// 作者：VLR 开发团队
// 创建日期：2026-03-07
// 环境：CUDA 13.1, OptiX 8.0.0, VS2022
// ============================================================================

#include "include/vlr/vlr.h"
#include "context.h"
#include "scene.h"
#include "config_loader.h"
#include "image_loader.h"
#include "include/vlr/public_types.h"
#include "include/vlr/basic_types.h"
#include <cuda_runtime.h>
#include <cmath>
#include <cstring>
#include <new>
#include <stdexcept>
#include <algorithm>
#include <unordered_map>
#include <vector>

namespace {
constexpr float VLR_PI = 3.14159265358979323846f;
}

// C++ 类型到 C 句柄的包装
struct VLRContextImpl {
    vlr::Context* ctx;
    VLRContextImpl() : ctx(nullptr) {}
};

struct VLRSceneImpl {
    vlr::Scene* scene;
    VLRContextImpl* contextImpl;
    VLRSceneImpl() : scene(nullptr), contextImpl(nullptr) {}
};

struct VLRTriangleMeshImpl {
    VLRSceneImpl* sceneImpl;
    uint32_t meshId;
    uint32_t materialIndex;
    VLRTriangleMeshImpl() : sceneImpl(nullptr), meshId(0xFFFFFFFF), materialIndex(0) {}
};

struct VLRMaterialImpl {
    VLRSceneImpl* sceneImpl;
    uint32_t materialIndex;
    VLRMaterialImpl() : sceneImpl(nullptr), materialIndex(0xFFFFFFFF) {}
};

struct VLRInstanceImpl {
    VLRSceneImpl* sceneImpl;
    uint32_t instanceIndex;
    VLRInstanceImpl() : sceneImpl(nullptr), instanceIndex(0xFFFFFFFF) {}
};

struct VLRTextureImpl {
    VLRContextImpl* contextImpl;
    uint32_t textureIndex;
    VLRTextureImpl() : contextImpl(nullptr), textureIndex(0xFFFFFFFF) {}
};

// 纹理句柄注册表：Context 销毁时使所有关联纹理句柄失效
static std::unordered_map<VLRContextImpl*, std::vector<VLRTextureImpl*>> g_textureHandles;

// 将 C 句柄转换为实现
#define TO_CTX(h) (reinterpret_cast<VLRContextImpl*>(h))
#define TO_SCENE(h) (reinterpret_cast<VLRSceneImpl*>(h))
#define TO_MESH(h) (reinterpret_cast<VLRTriangleMeshImpl*>(h))
#define TO_MAT(h) (reinterpret_cast<VLRMaterialImpl*>(h))
#define TO_INST(h) (reinterpret_cast<VLRInstanceImpl*>(h))
#define TO_TEXTURE(h) (reinterpret_cast<VLRTextureImpl*>(h))

// 从实现获取句柄
#define FROM_CTX(p) (reinterpret_cast<VLRContext>(p))
#define FROM_SCENE(p) (reinterpret_cast<VLRScene>(p))
#define FROM_MESH(p) (reinterpret_cast<VLRTriangleMesh>(p))
#define FROM_MAT(p) (reinterpret_cast<VLRMaterial>(p))
#define FROM_INST(p) (reinterpret_cast<VLRInstance>(p))
#define FROM_TEXTURE(p) (reinterpret_cast<VLRTexture>(p))

static VLRResult translateException() {
    try {
        throw;
    } catch (const std::bad_alloc&) {
        return static_cast<VLRResult>(VLRResult_OutOfMemory);
    } catch (const std::invalid_argument&) {
        return static_cast<VLRResult>(VLRResult_InvalidArgument);
    } catch (...) {
        return static_cast<VLRResult>(VLRResult_InternalError);
    }
}

// ============================================================================
// 核心 API 实现
// ============================================================================

extern "C" {

VLRResult vlrCreateContext(void* cudaStream, int enableLogging, VLRContext* outContext) {
    if (!outContext) return static_cast<VLRResult>(VLRResult_InvalidArgument);
    *outContext = nullptr;
    try {
        printf("[VLR] vlrCreateContext: start\n");
        fflush(stdout);
        
        cudaStream_t stream = static_cast<cudaStream_t>(cudaStream);
        if (!stream) {
            printf("[VLR] Creating CUDA stream...\n");
            fflush(stdout);
            cudaStreamCreate(&stream);
        }
        
        printf("[VLR] Allocating VLRContextImpl...\n");
        fflush(stdout);
        VLRContextImpl* impl = new VLRContextImpl();
        
        printf("[VLR] Creating vlr::Context...\n");
        fflush(stdout);
        impl->ctx = new vlr::Context(stream, enableLogging != 0);
        
        printf("[VLR] Context created successfully\n");
        fflush(stdout);
        *outContext = FROM_CTX(impl);
        return static_cast<VLRResult>(VLRResult_Success);
    } catch (const std::exception& e) {
        printf("[VLR] Exception in vlrCreateContext: %s\n", e.what());
        fflush(stdout);
        return translateException();
    } catch (...) {
        printf("[VLR] Unknown exception in vlrCreateContext\n");
        fflush(stdout);
        return translateException();
    }
}

void vlrDestroyContext(VLRContext context) {
    if (!context) return;
    VLRContextImpl* impl = TO_CTX(context);
    // 使所有关联纹理句柄失效（避免悬空指针）
    auto it = g_textureHandles.find(impl);
    if (it != g_textureHandles.end()) {
        for (VLRTextureImpl* tex : it->second) {
            tex->contextImpl = nullptr;
            tex->textureIndex = 0xFFFFFFFF;
        }
        g_textureHandles.erase(it);
    }
    if (impl->ctx) {
        delete impl->ctx;
        impl->ctx = nullptr;
    }
    delete impl;
}

VLRResult vlrCreateScene(VLRContext context, VLRScene* outScene) {
    if (!context || !outScene) return static_cast<VLRResult>(VLRResult_InvalidArgument);
    *outScene = nullptr;
    try {
        VLRContextImpl* ctxImpl = TO_CTX(context);
        if (!ctxImpl->ctx) return static_cast<VLRResult>(VLRResult_InvalidArgument);
        vlr::Scene* scene = ctxImpl->ctx->createScene();
        VLRSceneImpl* impl = new VLRSceneImpl();
        impl->scene = scene;
        impl->contextImpl = ctxImpl;
        *outScene = FROM_SCENE(impl);
        return static_cast<VLRResult>(VLRResult_Success);
    } catch (...) {
        return translateException();
    }
}

void vlrDestroyScene(VLRScene scene) {
    if (!scene) return;
    VLRSceneImpl* impl = TO_SCENE(scene);
    if (impl->scene && impl->contextImpl && impl->contextImpl->ctx) {
        impl->contextImpl->ctx->destroyScene(impl->scene);
    }
    impl->scene = nullptr;
    delete impl;
}

void vlrReleaseHostMeshData(VLRScene scene) {
    if (!scene) return;
    VLRSceneImpl* impl = TO_SCENE(scene);
    if (impl->scene) {
        impl->scene->releaseHostMeshData();
    }
}

VLRResult vlrCreateTriangleMesh(
    VLRScene scene,
    const float* vertices,
    uint32_t numVertices,
    const uint32_t* indices,
    uint32_t numTriangles,
    VLRMaterial material,
    VLRTriangleMesh* outMesh)
{
    if (!scene || !outMesh || !material) return static_cast<VLRResult>(VLRResult_InvalidArgument);
    if (!vertices && numVertices > 0) return static_cast<VLRResult>(VLRResult_InvalidArgument);
    if (!indices && numTriangles > 0) return static_cast<VLRResult>(VLRResult_InvalidArgument);
    *outMesh = nullptr;
    try {
        VLRSceneImpl* sceneImpl = TO_SCENE(scene);
        VLRMaterialImpl* matImpl = TO_MAT(material);
        if (!sceneImpl->scene) return static_cast<VLRResult>(VLRResult_InvalidArgument);
        if (!matImpl || matImpl->sceneImpl != sceneImpl) return static_cast<VLRResult>(VLRResult_InvalidArgument);
        uint32_t numIndices = numTriangles * 3;
        uint32_t meshId = sceneImpl->scene->createTriangleMesh(
            vertices, numVertices,
            nullptr, 0,
            nullptr, 0,
            indices, numIndices);
        VLRTriangleMeshImpl* meshImpl = new VLRTriangleMeshImpl();
        meshImpl->sceneImpl = sceneImpl;
        meshImpl->meshId = meshId;
        meshImpl->materialIndex = matImpl->materialIndex;
        *outMesh = FROM_MESH(meshImpl);
        return static_cast<VLRResult>(VLRResult_Success);
    } catch (...) {
        return translateException();
    }
}

VLRResult vlrCreateTriangleMeshWithNormals(
    VLRScene scene,
    const float* vertices,
    uint32_t numVertices,
    const float* normals,
    uint32_t numNormals,
    const uint32_t* indices,
    uint32_t numTriangles,
    VLRMaterial material,
    VLRTriangleMesh* outMesh)
{
    if (!scene || !outMesh || !material) return static_cast<VLRResult>(VLRResult_InvalidArgument);
    if (!vertices && numVertices > 0) return static_cast<VLRResult>(VLRResult_InvalidArgument);
    if (!indices && numTriangles > 0) return static_cast<VLRResult>(VLRResult_InvalidArgument);
    *outMesh = nullptr;
    try {
        VLRSceneImpl* sceneImpl = TO_SCENE(scene);
        VLRMaterialImpl* matImpl = TO_MAT(material);
        if (!sceneImpl->scene) return static_cast<VLRResult>(VLRResult_InvalidArgument);
        if (!matImpl || matImpl->sceneImpl != sceneImpl) return static_cast<VLRResult>(VLRResult_InvalidArgument);
        uint32_t numIndices = numTriangles * 3;
        uint32_t meshId = sceneImpl->scene->createTriangleMesh(
            vertices, numVertices,
            normals, numNormals,
            nullptr, 0,
            indices, numIndices);
        VLRTriangleMeshImpl* meshImpl = new VLRTriangleMeshImpl();
        meshImpl->sceneImpl = sceneImpl;
        meshImpl->meshId = meshId;
        meshImpl->materialIndex = matImpl->materialIndex;
        *outMesh = FROM_MESH(meshImpl);
        return static_cast<VLRResult>(VLRResult_Success);
    } catch (...) {
        return translateException();
    }
}

void vlrDestroyTriangleMesh(VLRTriangleMesh mesh) {
    if (!mesh) return;
    delete TO_MESH(mesh);
}

VLRResult vlrCreateMaterial(
    VLRScene scene,
    uint32_t materialType,
    const float* baseColor,
    const float* emissionColor,
    VLRMaterial* outMaterial)
{
    if (!scene || !outMaterial) return static_cast<VLRResult>(VLRResult_InvalidArgument);
    *outMaterial = nullptr;
    try {
        VLRSceneImpl* sceneImpl = TO_SCENE(scene);
        if (!sceneImpl->scene) return static_cast<VLRResult>(VLRResult_InvalidArgument);
        // 与原始 VLR MatteSurfaceMaterial 一致，默认反照率 0.18
        float r = baseColor ? baseColor[0] : 0.18f;
        float g = baseColor ? baseColor[1] : 0.18f;
        float b = baseColor ? baseColor[2] : 0.18f;
        float er = emissionColor ? emissionColor[0] : 0.0f;
        float eg = emissionColor ? emissionColor[1] : 0.0f;
        float eb = emissionColor ? emissionColor[2] : 0.0f;
        uint32_t materialIndex = sceneImpl->scene->createMaterial(
            materialType, r, g, b, 0.5f, er, eg, eb);
        VLRMaterialImpl* matImpl = new VLRMaterialImpl();
        matImpl->sceneImpl = sceneImpl;
        matImpl->materialIndex = materialIndex;
        *outMaterial = FROM_MAT(matImpl);
        return static_cast<VLRResult>(VLRResult_Success);
    } catch (...) {
        return translateException();
    }
}

void vlrDestroyMaterial(VLRMaterial material) {
    if (!material) return;
    delete TO_MAT(material);
}

VLRResult vlrCreateMaterialEx(
    VLRScene scene,
    uint32_t materialType,
    const float baseColor[3],
    float roughness,
    float metallic,
    float ior,
    const float* emissionColor,
    VLRMaterial* outMaterial)
{
    if (!scene || !baseColor || !outMaterial) return static_cast<VLRResult>(VLRResult_InvalidArgument);
    *outMaterial = nullptr;
    try {
        VLRSceneImpl* sceneImpl = TO_SCENE(scene);
        if (!sceneImpl->scene) return static_cast<VLRResult>(VLRResult_InvalidArgument);
        
        float r = baseColor[0];
        float g = baseColor[1];
        float b = baseColor[2];
        float er = emissionColor ? emissionColor[0] : 0.0f;
        float eg = emissionColor ? emissionColor[1] : 0.0f;
        float eb = emissionColor ? emissionColor[2] : 0.0f;
        
        // 调用 Scene::createMaterialEx，传递完整参数（包括 IOR 和 metallic）
        uint32_t materialIndex = sceneImpl->scene->createMaterialEx(
            materialType, r, g, b, roughness, metallic, ior, er, eg, eb);
        
        VLRMaterialImpl* matImpl = new VLRMaterialImpl();
        matImpl->sceneImpl = sceneImpl;
        matImpl->materialIndex = materialIndex;
        *outMaterial = FROM_MAT(matImpl);
        return static_cast<VLRResult>(VLRResult_Success);
    } catch (...) {
        return translateException();
    }
}

VLRResult vlrCreateMaterialDispersive(
    VLRScene scene,
    const float baseColor[3],
    float ior,
    float dispersionStrength,
    VLRMaterial* outMaterial)
{
    if (!scene || !baseColor || !outMaterial) return static_cast<VLRResult>(VLRResult_InvalidArgument);
    *outMaterial = nullptr;
    try {
        VLRSceneImpl* sceneImpl = TO_SCENE(scene);
        if (!sceneImpl->scene) return static_cast<VLRResult>(VLRResult_InvalidArgument);
        uint32_t materialIndex = sceneImpl->scene->createMaterialDispersive(
            baseColor[0], baseColor[1], baseColor[2], ior, dispersionStrength);
        VLRMaterialImpl* matImpl = new VLRMaterialImpl();
        matImpl->sceneImpl = sceneImpl;
        matImpl->materialIndex = materialIndex;
        *outMaterial = FROM_MAT(matImpl);
        return static_cast<VLRResult>(VLRResult_Success);
    } catch (...) {
        return translateException();
    }
}

VLRResult vlrCreateMaterialConductor(
    VLRScene scene,
    const float eta[3],
    const float kappa[3],
    float roughness,
    VLRMaterial* outMaterial)
{
    if (!scene || !eta || !kappa || !outMaterial) return static_cast<VLRResult>(VLRResult_InvalidArgument);
    *outMaterial = nullptr;
    try {
        VLRSceneImpl* sceneImpl = TO_SCENE(scene);
        if (!sceneImpl->scene) return static_cast<VLRResult>(VLRResult_InvalidArgument);
        uint32_t materialIndex = sceneImpl->scene->createMaterialConductor(
            eta[0], eta[1], eta[2],
            kappa[0], kappa[1], kappa[2],
            roughness);
        VLRMaterialImpl* matImpl = new VLRMaterialImpl();
        matImpl->sceneImpl = sceneImpl;
        matImpl->materialIndex = materialIndex;
        *outMaterial = FROM_MAT(matImpl);
        return static_cast<VLRResult>(VLRResult_Success);
    } catch (...) {
        return translateException();
    }
}

VLRResult vlrCreateMaterialConductorAniso(
    VLRScene scene,
    const float eta[3],
    const float kappa[3],
    float roughness,
    float anisotropy,
    VLRMaterial* outMaterial)
{
    if (!scene || !eta || !kappa || !outMaterial) return static_cast<VLRResult>(VLRResult_InvalidArgument);
    *outMaterial = nullptr;
    try {
        VLRSceneImpl* sceneImpl = TO_SCENE(scene);
        if (!sceneImpl->scene) return static_cast<VLRResult>(VLRResult_InvalidArgument);
        uint32_t materialIndex = sceneImpl->scene->createMaterialConductorAniso(
            eta[0], eta[1], eta[2],
            kappa[0], kappa[1], kappa[2],
            roughness,
            anisotropy);
        VLRMaterialImpl* matImpl = new VLRMaterialImpl();
        matImpl->sceneImpl = sceneImpl;
        matImpl->materialIndex = materialIndex;
        *outMaterial = FROM_MAT(matImpl);
        return static_cast<VLRResult>(VLRResult_Success);
    } catch (...) {
        return translateException();
    }
}

VLRResult vlrCreateMaterialMicrofacetScattering(
    VLRScene scene,
    float ior,
    float roughness,
    VLRMaterial* outMaterial)
{
    if (!scene || !outMaterial) return static_cast<VLRResult>(VLRResult_InvalidArgument);
    *outMaterial = nullptr;
    try {
        VLRSceneImpl* sceneImpl = TO_SCENE(scene);
        if (!sceneImpl->scene) return static_cast<VLRResult>(VLRResult_InvalidArgument);
        uint32_t materialIndex = sceneImpl->scene->createMaterialMicrofacetScattering(ior, roughness);
        VLRMaterialImpl* matImpl = new VLRMaterialImpl();
        matImpl->sceneImpl = sceneImpl;
        matImpl->materialIndex = materialIndex;
        *outMaterial = FROM_MAT(matImpl);
        return static_cast<VLRResult>(VLRResult_Success);
    } catch (...) {
        return translateException();
    }
}

VLRResult vlrCreateMaterialLambertianScattering(
    VLRScene scene,
    const float albedo[3],
    VLRMaterial* outMaterial)
{
    if (!scene || !outMaterial || !albedo) return static_cast<VLRResult>(VLRResult_InvalidArgument);
    *outMaterial = nullptr;
    try {
        VLRSceneImpl* sceneImpl = TO_SCENE(scene);
        if (!sceneImpl->scene) return static_cast<VLRResult>(VLRResult_InvalidArgument);
        uint32_t materialIndex = sceneImpl->scene->createMaterialLambertianScattering(
            albedo[0], albedo[1], albedo[2]);
        VLRMaterialImpl* matImpl = new VLRMaterialImpl();
        matImpl->sceneImpl = sceneImpl;
        matImpl->materialIndex = materialIndex;
        *outMaterial = FROM_MAT(matImpl);
        return static_cast<VLRResult>(VLRResult_Success);
    } catch (...) {
        return translateException();
    }
}

VLRResult vlrCreateMaterialDisney(
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
    VLRMaterial* outMaterial)
{
    if (!scene || !outMaterial || !baseColor) return static_cast<VLRResult>(VLRResult_InvalidArgument);
    *outMaterial = nullptr;
    try {
        VLRSceneImpl* sceneImpl = TO_SCENE(scene);
        if (!sceneImpl->scene) return static_cast<VLRResult>(VLRResult_InvalidArgument);
        uint32_t materialIndex = sceneImpl->scene->createMaterialDisney(
            baseColor,
            metallic, subsurface, specular, roughness,
            specularTint, anisotropic, sheen, sheenTint,
            clearcoat, clearcoatGloss);
        VLRMaterialImpl* matImpl = new VLRMaterialImpl();
        matImpl->sceneImpl = sceneImpl;
        matImpl->materialIndex = materialIndex;
        *outMaterial = FROM_MAT(matImpl);
        return static_cast<VLRResult>(VLRResult_Success);
    } catch (...) {
        return translateException();
    }
}

VLRResult vlrCreateMaterialCheckerboard(
    VLRScene scene,
    const float color0[3],
    const float color1[3],
    uint32_t gridSize,
    float extent,
    VLRMaterial* outMaterial)
{
    if (!scene || !outMaterial || !color0 || !color1) return static_cast<VLRResult>(VLRResult_InvalidArgument);
    *outMaterial = nullptr;
    try {
        VLRSceneImpl* sceneImpl = TO_SCENE(scene);
        if (!sceneImpl->scene) return static_cast<VLRResult>(VLRResult_InvalidArgument);
        uint32_t materialIndex = sceneImpl->scene->createMaterialCheckerboard(
            color0[0], color0[1], color0[2],
            color1[0], color1[1], color1[2],
            gridSize > 0 ? gridSize : 8,
            extent);
        VLRMaterialImpl* matImpl = new VLRMaterialImpl();
        matImpl->sceneImpl = sceneImpl;
        matImpl->materialIndex = materialIndex;
        *outMaterial = FROM_MAT(matImpl);
        return static_cast<VLRResult>(VLRResult_Success);
    } catch (...) {
        return translateException();
    }
}

VLRResult vlrCreateMaterialMultiSurface(
    VLRScene scene,
    int numLayers,
    const uint32_t* subBSDFTypes,
    const float* const* subAlbedos,
    const float* subRoughness,
    const float* weights,
    VLRMaterial* outMaterial)
{
    if (!scene || !outMaterial || numLayers < 2 || numLayers > 4 ||
        !subBSDFTypes || !subAlbedos || !subRoughness || !weights)
        return static_cast<VLRResult>(VLRResult_InvalidArgument);
    *outMaterial = nullptr;
    try {
        VLRSceneImpl* sceneImpl = TO_SCENE(scene);
        if (!sceneImpl->scene) return static_cast<VLRResult>(VLRResult_InvalidArgument);
        uint32_t materialIndex = sceneImpl->scene->createMaterialMultiSurface(
            numLayers, subBSDFTypes, subAlbedos, subRoughness, weights);
        if (materialIndex == 0xFFFFFFFF)
            return static_cast<VLRResult>(VLRResult_InvalidArgument);
        VLRMaterialImpl* matImpl = new VLRMaterialImpl();
        matImpl->sceneImpl = sceneImpl;
        matImpl->materialIndex = materialIndex;
        *outMaterial = FROM_MAT(matImpl);
        return static_cast<VLRResult>(VLRResult_Success);
    } catch (...) {
        return translateException();
    }
}

VLRResult vlrCreateInstance(
    VLRScene scene,
    VLRTriangleMesh mesh,
    const float position[3],
    const float scale[3],
    const float rotationAxis[3],
    float rotationAngle,
    VLRInstance* outInstance)
{
    if (!scene || !mesh || !outInstance) return static_cast<VLRResult>(VLRResult_InvalidArgument);
    *outInstance = nullptr;
    try {
        VLRSceneImpl* sceneImpl = TO_SCENE(scene);
        VLRTriangleMeshImpl* meshImpl = TO_MESH(mesh);
        if (meshImpl->sceneImpl != sceneImpl) return static_cast<VLRResult>(VLRResult_InvalidArgument);
        vlr::InstanceTransform transform;
        transform.position.x = position ? position[0] : 0.0f;
        transform.position.y = position ? position[1] : 0.0f;
        transform.position.z = position ? position[2] : 0.0f;
        transform.scale.x = scale ? scale[0] : 1.0f;
        transform.scale.y = scale ? scale[1] : 1.0f;
        transform.scale.z = scale ? scale[2] : 1.0f;
        transform.rotationRadians = rotationAngle;
        uint32_t instIndex = sceneImpl->scene->createInstance(
            meshImpl->meshId, meshImpl->materialIndex, transform);
        VLRInstanceImpl* instImpl = new VLRInstanceImpl();
        instImpl->sceneImpl = sceneImpl;
        instImpl->instanceIndex = instIndex;
        *outInstance = FROM_INST(instImpl);
        return static_cast<VLRResult>(VLRResult_Success);
    } catch (...) {
        return translateException();
    }
}

void vlrDestroyInstance(VLRInstance instance) {
    if (!instance) return;
    delete TO_INST(instance);
}

// ============================================================================
// 纹理 API
// ============================================================================

VLRResult vlrCreateTexture2D(
    VLRContext context,
    const char* imagePath,
    VLRTexture* outTexture)
{
    if (!context || !imagePath || !outTexture) {
        printf("[VLR] vlrCreateTexture2D: invalid argument (null)\n");
        return static_cast<VLRResult>(VLRResult_InvalidArgument);
    }
    *outTexture = nullptr;
    try {
        VLRContextImpl* ctxImpl = TO_CTX(context);
        if (!ctxImpl->ctx) {
            printf("[VLR] vlrCreateTexture2D: context not initialized\n");
            return static_cast<VLRResult>(VLRResult_InvalidArgument);
        }
        uint32_t texIndex = 0;
        if (!ctxImpl->ctx->createTexture2D(imagePath, &texIndex)) {
            return static_cast<VLRResult>(VLRResult_InvalidArgument);
        }
        VLRTextureImpl* texImpl = new VLRTextureImpl();
        texImpl->contextImpl = ctxImpl;
        texImpl->textureIndex = texIndex;
        g_textureHandles[ctxImpl].push_back(texImpl);
        *outTexture = FROM_TEXTURE(texImpl);
        return static_cast<VLRResult>(VLRResult_Success);
    } catch (...) {
        return translateException();
    }
}

VLRResult vlrCreateTexture2DFromMemory(
    VLRContext context,
    const void* data,
    uint32_t width,
    uint32_t height,
    uint32_t format,
    VLRTexture* outTexture)
{
    if (!context || !data || !outTexture) {
        printf("[VLR] vlrCreateTexture2DFromMemory: invalid argument (null)\n");
        return static_cast<VLRResult>(VLRResult_InvalidArgument);
    }
    if (width == 0 || height == 0) {
        printf("[VLR] vlrCreateTexture2DFromMemory: invalid dimensions %ux%u\n", width, height);
        return static_cast<VLRResult>(VLRResult_InvalidArgument);
    }
    if (format > 2) {
        printf("[VLR] vlrCreateTexture2DFromMemory: invalid format %u\n", format);
        return static_cast<VLRResult>(VLRResult_InvalidArgument);
    }
    *outTexture = nullptr;
    try {
        VLRContextImpl* ctxImpl = TO_CTX(context);
        if (!ctxImpl->ctx) {
            printf("[VLR] vlrCreateTexture2DFromMemory: context not initialized\n");
            return static_cast<VLRResult>(VLRResult_InvalidArgument);
        }
        uint32_t texIndex = 0;
        if (!ctxImpl->ctx->createTexture2DFromMemory(data, width, height, format, &texIndex)) {
            return static_cast<VLRResult>(VLRResult_InternalError);
        }
        VLRTextureImpl* texImpl = new VLRTextureImpl();
        texImpl->contextImpl = ctxImpl;
        texImpl->textureIndex = texIndex;
        g_textureHandles[ctxImpl].push_back(texImpl);
        *outTexture = FROM_TEXTURE(texImpl);
        return static_cast<VLRResult>(VLRResult_Success);
    } catch (...) {
        return translateException();
    }
}

VLRResult vlrDestroyTexture(VLRTexture texture) {
    if (!texture) return static_cast<VLRResult>(VLRResult_Success);
    try {
        VLRTextureImpl* texImpl = TO_TEXTURE(texture);
        if (texImpl->contextImpl && texImpl->contextImpl->ctx &&
            texImpl->textureIndex != 0xFFFFFFFF) {
            texImpl->contextImpl->ctx->destroyTexture(texImpl->textureIndex);
            // 从注册表移除
            auto it = g_textureHandles.find(texImpl->contextImpl);
            if (it != g_textureHandles.end()) {
                auto& vec = it->second;
                vec.erase(std::remove(vec.begin(), vec.end(), texImpl), vec.end());
            }
        }
        texImpl->contextImpl = nullptr;
        texImpl->textureIndex = 0xFFFFFFFF;
        delete texImpl;
        return static_cast<VLRResult>(VLRResult_Success);
    } catch (...) {
        return translateException();
    }
}

VLRResult vlrSetTextureFilterMode(VLRTexture texture, uint32_t filterMode) {
    if (!texture) {
        printf("[VLR] vlrSetTextureFilterMode: invalid texture (null)\n");
        return static_cast<VLRResult>(VLRResult_InvalidArgument);
    }
    if (filterMode > 1) {
        printf("[VLR] vlrSetTextureFilterMode: invalid filterMode %u\n", filterMode);
        return static_cast<VLRResult>(VLRResult_InvalidArgument);
    }
    try {
        VLRTextureImpl* texImpl = TO_TEXTURE(texture);
        if (!texImpl->contextImpl || !texImpl->contextImpl->ctx) {
            return static_cast<VLRResult>(VLRResult_InvalidArgument);
        }
        if (!texImpl->contextImpl->ctx->setTextureFilterMode(texImpl->textureIndex, filterMode)) {
            return static_cast<VLRResult>(VLRResult_InvalidArgument);
        }
        return static_cast<VLRResult>(VLRResult_Success);
    } catch (...) {
        return translateException();
    }
}

VLRResult vlrSetTextureWrapMode(VLRTexture texture, uint32_t wrapU, uint32_t wrapV) {
    if (!texture) {
        printf("[VLR] vlrSetTextureWrapMode: invalid texture (null)\n");
        return static_cast<VLRResult>(VLRResult_InvalidArgument);
    }
    if (wrapU > 1 || wrapV > 1) {
        printf("[VLR] vlrSetTextureWrapMode: invalid wrap mode (0=Repeat, 1=Clamp)\n");
        return static_cast<VLRResult>(VLRResult_InvalidArgument);
    }
    try {
        VLRTextureImpl* texImpl = TO_TEXTURE(texture);
        if (!texImpl->contextImpl || !texImpl->contextImpl->ctx) {
            return static_cast<VLRResult>(VLRResult_InvalidArgument);
        }
        if (!texImpl->contextImpl->ctx->setTextureWrapMode(texImpl->textureIndex, wrapU, wrapV)) {
            return static_cast<VLRResult>(VLRResult_InvalidArgument);
        }
        return static_cast<VLRResult>(VLRResult_Success);
    } catch (...) {
        return translateException();
    }
}

// ============================================================================
// 材质纹理绑定 API
// ============================================================================

namespace {
    constexpr uint32_t InvalidTextureIdx = 0xFFFFFFFF;
}

static VLRResult setMaterialTextureSlot(VLRMaterial material, VLRTexture texture,
    void (vlr::Scene::*setter)(uint32_t, uint32_t), const char* slotName) {
    if (!material) {
        printf("[VLR] vlrSetMaterial%sTexture: invalid material (null)\n", slotName);
        return static_cast<VLRResult>(VLRResult_InvalidArgument);
    }
    try {
        VLRMaterialImpl* matImpl = TO_MAT(material);
        if (!matImpl->sceneImpl || !matImpl->sceneImpl->scene) {
            printf("[VLR] vlrSetMaterial%sTexture: material has no scene\n", slotName);
            return static_cast<VLRResult>(VLRResult_InvalidArgument);
        }
        uint32_t texIdx = InvalidTextureIdx;
        if (texture) {
            VLRTextureImpl* texImpl = TO_TEXTURE(texture);
            if (!texImpl->contextImpl || !texImpl->contextImpl->ctx) {
                printf("[VLR] vlrSetMaterial%sTexture: invalid texture\n", slotName);
                return static_cast<VLRResult>(VLRResult_InvalidArgument);
            }
            if (matImpl->sceneImpl->contextImpl != texImpl->contextImpl) {
                printf("[VLR] vlrSetMaterial%sTexture: material and texture must belong to same context\n", slotName);
                return static_cast<VLRResult>(VLRResult_InvalidArgument);
            }
            texIdx = texImpl->textureIndex;
        }
        (matImpl->sceneImpl->scene->*setter)(matImpl->materialIndex, texIdx);
        printf("[VLR] vlrSetMaterial%sTexture: material %u -> texture %u\n", slotName, matImpl->materialIndex, texIdx);
        return static_cast<VLRResult>(VLRResult_Success);
    } catch (...) {
        return translateException();
    }
}

VLRResult vlrSetMaterialBaseColorTexture(VLRMaterial material, VLRTexture texture) {
    return setMaterialTextureSlot(material, texture, &vlr::Scene::setMaterialBaseColorTexture, "BaseColor");
}

VLRResult vlrSetMaterialRoughnessTexture(VLRMaterial material, VLRTexture texture) {
    return setMaterialTextureSlot(material, texture, &vlr::Scene::setMaterialRoughnessTexture, "Roughness");
}

VLRResult vlrSetMaterialMetallicTexture(VLRMaterial material, VLRTexture texture) {
    return setMaterialTextureSlot(material, texture, &vlr::Scene::setMaterialMetallicTexture, "Metallic");
}

VLRResult vlrSetMaterialNormalTexture(VLRMaterial material, VLRTexture texture, float normalScale) {
    if (!material) {
        printf("[VLR] vlrSetMaterialNormalTexture: invalid material (null)\n");
        return static_cast<VLRResult>(VLRResult_InvalidArgument);
    }
    try {
        VLRMaterialImpl* matImpl = TO_MAT(material);
        if (!matImpl->sceneImpl || !matImpl->sceneImpl->scene) {
            printf("[VLR] vlrSetMaterialNormalTexture: material has no scene\n");
            return static_cast<VLRResult>(VLRResult_InvalidArgument);
        }
        uint32_t texIdx = InvalidTextureIdx;
        if (texture) {
            VLRTextureImpl* texImpl = TO_TEXTURE(texture);
            if (!texImpl->contextImpl || !texImpl->contextImpl->ctx) {
                printf("[VLR] vlrSetMaterialNormalTexture: invalid texture\n");
                return static_cast<VLRResult>(VLRResult_InvalidArgument);
            }
            if (matImpl->sceneImpl->contextImpl != texImpl->contextImpl) {
                printf("[VLR] vlrSetMaterialNormalTexture: material and texture must belong to same context\n");
                return static_cast<VLRResult>(VLRResult_InvalidArgument);
            }
            texIdx = texImpl->textureIndex;
        }
        matImpl->sceneImpl->scene->setMaterialNormalTexture(matImpl->materialIndex, texIdx, normalScale);
        printf("[VLR] vlrSetMaterialNormalTexture: material %u -> texture %u, normalScale=%.2f\n",
            matImpl->materialIndex, texIdx, normalScale);
        return static_cast<VLRResult>(VLRResult_Success);
    } catch (...) {
        return translateException();
    }
}

VLRResult vlrSetMaterialTextureTransform(VLRMaterial material, float scaleU, float scaleV, float offsetU, float offsetV) {
    if (!material) {
        printf("[VLR] vlrSetMaterialTextureTransform: invalid material (null)\n");
        return static_cast<VLRResult>(VLRResult_InvalidArgument);
    }
    try {
        VLRMaterialImpl* matImpl = TO_MAT(material);
        if (!matImpl->sceneImpl || !matImpl->sceneImpl->scene) {
            printf("[VLR] vlrSetMaterialTextureTransform: material has no scene\n");
            return static_cast<VLRResult>(VLRResult_InvalidArgument);
        }
        matImpl->sceneImpl->scene->setMaterialTextureTransform(matImpl->materialIndex, scaleU, scaleV, offsetU, offsetV);
        printf("[VLR] vlrSetMaterialTextureTransform: material %u scale(%.2f,%.2f) offset(%.2f,%.2f)\n",
            matImpl->materialIndex, scaleU, scaleV, offsetU, offsetV);
        return static_cast<VLRResult>(VLRResult_Success);
    } catch (...) {
        return translateException();
    }
}

VLRResult vlrAddAreaLight(VLRScene scene, VLRInstance instance) {
    if (!scene || !instance) return static_cast<VLRResult>(VLRResult_InvalidArgument);
    try {
        VLRSceneImpl* sceneImpl = TO_SCENE(scene);
        VLRInstanceImpl* instImpl = TO_INST(instance);
        if (!sceneImpl->scene) return static_cast<VLRResult>(VLRResult_InvalidArgument);
        if (instImpl->sceneImpl != sceneImpl) return static_cast<VLRResult>(VLRResult_InvalidArgument);
        vlr::AreaLightParams params;
        params.instIndex = instImpl->instanceIndex;
        params.geomInstIndex = 0;
        params.radiance = vlr::SampledSpectrum(1.0f);
        sceneImpl->scene->addAreaLight(params);
        return static_cast<VLRResult>(VLRResult_Success);
    } catch (...) {
        return translateException();
    }
}

VLRResult vlrSetEnvironmentLight(VLRScene scene, const float color[3]) {
    if (!scene || !color) return static_cast<VLRResult>(VLRResult_InvalidArgument);
    try {
        VLRSceneImpl* sceneImpl = TO_SCENE(scene);
        if (!sceneImpl->scene) return static_cast<VLRResult>(VLRResult_InvalidArgument);
        vlr::EnvironmentLightParams params;
        params.useConstant = true;
        // SampledSpectrum uses RGB for simplicity (NumSpectralSamples = 3)
        params.constantColor = vlr::SampledSpectrum(0.0f);
        params.constantColor.values[0] = color[0];
        params.constantColor.values[1] = color[1];
        params.constantColor.values[2] = color[2];
        sceneImpl->scene->setEnvironmentLight(params);
        return static_cast<VLRResult>(VLRResult_Success);
    } catch (...) {
        return translateException();
    }
}

VLRResult vlrSetEnvironmentLightFromImage(VLRScene scene, const char* imagePath, float rotation) {
    if (!scene || !imagePath) return static_cast<VLRResult>(VLRResult_InvalidArgument);
    try {
        VLRSceneImpl* sceneImpl = TO_SCENE(scene);
        if (!sceneImpl->scene) return static_cast<VLRResult>(VLRResult_InvalidArgument);
        
        // 加载 HDR 图像
        vlr::HDRImage image;
        std::string error;
        if (!vlr::loadHDRImage(imagePath, image, &error)) {
            fprintf(stderr, "[VLR] Failed to load environment image '%s': %s\n", imagePath, error.c_str());
            return static_cast<VLRResult>(VLRResult_InvalidArgument);
        }
        
        vlr::EnvironmentLightParams params;
        params.useConstant = false;
        params.textureData = image.data;
        params.textureWidth = image.width;
        params.textureHeight = image.height;
        params.rotation = rotation;
        
        // 转移所有权给 Scene
        image.data = nullptr;
        
        sceneImpl->scene->setEnvironmentLight(params);
        return static_cast<VLRResult>(VLRResult_Success);
    } catch (...) {
        return translateException();
    }
}

VLRResult vlrAddPointLight(
    VLRScene scene,
    const float position[3],
    const float intensity[3])
{
    if (!scene || !position || !intensity) return static_cast<VLRResult>(VLRResult_InvalidArgument);
    try {
        VLRSceneImpl* sceneImpl = TO_SCENE(scene);
        if (!sceneImpl->scene) return static_cast<VLRResult>(VLRResult_InvalidArgument);

        vlr::PointLightParams params;
        params.position = vlr::Point3D(position[0], position[1], position[2]);
        params.intensity.values[0] = intensity[0];
        params.intensity.values[1] = intensity[1];
        params.intensity.values[2] = intensity[2];

        sceneImpl->scene->addPointLight(params);
        return static_cast<VLRResult>(VLRResult_Success);
    } catch (...) {
        return translateException();
    }
}

VLRResult vlrAddDirectionalLight(
    VLRScene scene,
    const float direction[3],
    const float radiance[3])
{
    if (!scene || !direction || !radiance) return static_cast<VLRResult>(VLRResult_InvalidArgument);
    try {
        VLRSceneImpl* sceneImpl = TO_SCENE(scene);
        if (!sceneImpl->scene) return static_cast<VLRResult>(VLRResult_InvalidArgument);

        vlr::Vector3D dir(direction[0], direction[1], direction[2]);
        vlr::SampledSpectrum rad;
        rad.values[0] = radiance[0];
        rad.values[1] = radiance[1];
        rad.values[2] = radiance[2];

        sceneImpl->scene->addDirectionalLight(dir, rad);
        return static_cast<VLRResult>(VLRResult_Success);
    } catch (...) {
        return translateException();
    }
}

VLRResult vlrSetCamera(VLRScene scene, const VLRCameraParams* params) {
    if (!scene) return static_cast<VLRResult>(VLRResult_InvalidArgument);
    if (!params) return static_cast<VLRResult>(VLRResult_InvalidArgument);
    try {
        VLRSceneImpl* sceneImpl = TO_SCENE(scene);
        if (!sceneImpl->scene) return static_cast<VLRResult>(VLRResult_InvalidArgument);
        vlr::CameraParams cp;
        cp.position.x = params->position[0];
        cp.position.y = params->position[1];
        cp.position.z = params->position[2];
        cp.lookAt.x = params->position[0] + params->direction[0];
        cp.lookAt.y = params->position[1] + params->direction[1];
        cp.lookAt.z = params->position[2] + params->direction[2];
        cp.up.x = params->up[0];
        cp.up.y = params->up[1];
        cp.up.z = params->up[2];
        cp.fovYDegrees = params->fovY * 180.0f / VLR_PI;
        cp.aspect = params->aspect;
        cp.lensRadius = params->lensRadius;
        cp.focusDistance = params->focusDistance;
        cp.focalLength = params->focalLength;
        cp.cameraType = params->cameraType;
        sceneImpl->scene->setCamera(cp);
        return static_cast<VLRResult>(VLRResult_Success);
    } catch (...) {
        return translateException();
    }
}

VLRResult vlrRender(
    VLRContext context,
    VLRScene scene,
    uint32_t width,
    uint32_t height,
    uint32_t numSamples,
    uint32_t renderer)
{
    if (!context || !scene) return static_cast<VLRResult>(VLRResult_InvalidArgument);
    try {
        VLRContextImpl* ctxImpl = TO_CTX(context);
        VLRSceneImpl* sceneImpl = TO_SCENE(scene);
        if (!ctxImpl->ctx) return static_cast<VLRResult>(VLRResult_InvalidArgument);
        ctxImpl->ctx->setScene(sceneImpl->scene);
        vlr::VLRRenderer r = static_cast<vlr::VLRRenderer>(renderer);
        ctxImpl->ctx->render(r, width, height, numSamples, nullptr);
        return static_cast<VLRResult>(VLRResult_Success);
    } catch (const std::runtime_error&) {
        return static_cast<VLRResult>(VLRResult_OptiXError);
    } catch (...) {
        return translateException();
    }
}

void* vlrGetOutputBuffer(VLRContext context) {
    if (!context) return nullptr;
    VLRContextImpl* impl = TO_CTX(context);
    if (!impl->ctx) return nullptr;
    return impl->ctx->getAccumBufferDevicePointer();
}

// ============================================================================
// 渐进渲染 API
// ============================================================================

VLRResult vlrBeginProgressive(
    VLRContext context, VLRScene scene,
    uint32_t width, uint32_t height)
{
    if (!context || !scene)
        return static_cast<VLRResult>(VLRResult_InvalidArgument);
    try {
        VLRContextImpl* ctxImpl = TO_CTX(context);
        VLRSceneImpl* scnImpl = reinterpret_cast<VLRSceneImpl*>(scene);
        if (!ctxImpl->ctx || !scnImpl->scene)
            return static_cast<VLRResult>(VLRResult_InvalidArgument);
        ctxImpl->ctx->setScene(scnImpl->scene);
        ctxImpl->ctx->beginProgressive(width, height);
        return static_cast<VLRResult>(VLRResult_Success);
    } catch (...) {
        return static_cast<VLRResult>(VLRResult_InternalError);
    }
}

VLRResult vlrRenderOneSample(VLRContext context, uint32_t* outAccumFrames) {
    if (!context)
        return static_cast<VLRResult>(VLRResult_InvalidArgument);
    try {
        VLRContextImpl* impl = TO_CTX(context);
        if (!impl->ctx)
            return static_cast<VLRResult>(VLRResult_InvalidArgument);
        impl->ctx->renderOneSample(outAccumFrames);
        return static_cast<VLRResult>(VLRResult_Success);
    } catch (...) {
        return static_cast<VLRResult>(VLRResult_InternalError);
    }
}

VLRResult vlrTonemapToRGBA8(VLRContext context, void* outRGBA8, float exposure, float gamma) {
    if (!context || !outRGBA8)
        return static_cast<VLRResult>(VLRResult_InvalidArgument);
    try {
        VLRContextImpl* impl = TO_CTX(context);
        if (!impl->ctx)
            return static_cast<VLRResult>(VLRResult_InvalidArgument);
        impl->ctx->tonemapToRGBA8(outRGBA8, exposure, gamma);
        return static_cast<VLRResult>(VLRResult_Success);
    } catch (...) {
        return static_cast<VLRResult>(VLRResult_InternalError);
    }
}

void vlrGetVersion(uint32_t* major, uint32_t* minor, uint32_t* patch) {
    if (major) *major = vlr::VLR_VERSION.major;
    if (minor) *minor = vlr::VLR_VERSION.minor;
    if (patch) *patch = vlr::VLR_VERSION.patch;
}

VLRResult vlrContextSetWavefrontPathSorting(VLRContext context, int enable) {
    if (!context) return static_cast<VLRResult>(VLRResult_InvalidArgument);
    try {
        VLRContextImpl* impl = TO_CTX(context);
        if (!impl->ctx) return static_cast<VLRResult>(VLRResult_InvalidArgument);
        impl->ctx->setWavefrontPathSorting(enable != 0);
        return static_cast<VLRResult>(VLRResult_Success);
    } catch (...) {
        return translateException();
    }
}

VLRResult vlrContextSetWavefrontStreamCompaction(VLRContext context, int enable) {
    if (!context) return static_cast<VLRResult>(VLRResult_InvalidArgument);
    try {
        VLRContextImpl* impl = TO_CTX(context);
        if (!impl->ctx) return static_cast<VLRResult>(VLRResult_InvalidArgument);
        impl->ctx->setWavefrontStreamCompaction(enable != 0);
        return static_cast<VLRResult>(VLRResult_Success);
    } catch (...) {
        return translateException();
    }
}

VLRResult vlrLoadPerformanceConfig(VLRContext context, const char* perfConfigFile) {
    if (!context || !perfConfigFile) return static_cast<VLRResult>(VLRResult_InvalidArgument);
    try {
        VLRContextImpl* impl = TO_CTX(context);
        if (!impl->ctx) return static_cast<VLRResult>(VLRResult_InvalidArgument);
        vlr::RuntimePerformanceConfig perfConfig;
        if (!vlr::ConfigLoader::loadPerformanceConfig(perfConfigFile, perfConfig)) {
            return static_cast<VLRResult>(VLRResult_InvalidArgument);
        }
        impl->ctx->setPerformanceConfig(perfConfig);
        return static_cast<VLRResult>(VLRResult_Success);
    } catch (...) {
        return translateException();
    }
}

VLRResult vlrSetDenoiserConfig(
    VLRContext context, 
    bool enabled,
    bool useAlbedo,
    bool useNormal,
    float hdrIntensity)
{
    if (!context) return static_cast<VLRResult>(VLRResult_InvalidArgument);
    try {
        VLRContextImpl* impl = TO_CTX(context);
        if (!impl->ctx) return static_cast<VLRResult>(VLRResult_InvalidArgument);
        
        vlr::DenoiserConfig config;
        config.enabled = enabled;
        config.useAlbedo = useAlbedo;
        config.useNormal = useNormal;
        config.hdrIntensity = hdrIntensity;
        
        impl->ctx->setDenoiserConfig(config);
        return static_cast<VLRResult>(VLRResult_Success);
    } catch (...) {
        return translateException();
    }
}

// ============================================================================
// 调试与可视化 API
// ============================================================================

VLRResult vlrSetDebugMode(VLRContext context, uint32_t mode) {
    if (!context) {
        printf("[VLR] vlrSetDebugMode: invalid context (null)\n");
        fflush(stdout);
        return static_cast<VLRResult>(VLRResult_InvalidArgument);
    }
    if (mode >= static_cast<uint32_t>(vlr::NumVLRDebugModes)) {
        printf("[VLR] vlrSetDebugMode: invalid mode %u (max %u)\n",
               mode, static_cast<uint32_t>(vlr::NumVLRDebugModes) - 1);
        fflush(stdout);
        return static_cast<VLRResult>(VLRResult_InvalidArgument);
    }
    try {
        VLRContextImpl* impl = TO_CTX(context);
        if (!impl->ctx) {
            printf("[VLR] vlrSetDebugMode: context not initialized\n");
            fflush(stdout);
            return static_cast<VLRResult>(VLRResult_InvalidArgument);
        }
        vlr::VLRDebugMode dm = static_cast<vlr::VLRDebugMode>(mode);
        impl->ctx->setDebugMode(dm);
        printf("[VLR] vlrSetDebugMode: set to %s (%u)\n",
               vlr::getDebugModeName(dm), mode);
        fflush(stdout);
        return static_cast<VLRResult>(VLRResult_Success);
    } catch (...) {
        return translateException();
    }
}

VLRResult vlrGetDebugMode(VLRContext context, uint32_t* outMode) {
    if (!context) {
        printf("[VLR] vlrGetDebugMode: invalid context (null)\n");
        fflush(stdout);
        return static_cast<VLRResult>(VLRResult_InvalidArgument);
    }
    if (!outMode) {
        printf("[VLR] vlrGetDebugMode: invalid outMode (null)\n");
        fflush(stdout);
        return static_cast<VLRResult>(VLRResult_InvalidArgument);
    }
    try {
        VLRContextImpl* impl = TO_CTX(context);
        if (!impl->ctx) {
            printf("[VLR] vlrGetDebugMode: context not initialized\n");
            fflush(stdout);
            return static_cast<VLRResult>(VLRResult_InvalidArgument);
        }
        *outMode = static_cast<uint32_t>(impl->ctx->getDebugMode());
        return static_cast<VLRResult>(VLRResult_Success);
    } catch (...) {
        return translateException();
    }
}

VLRResult vlrSetProbePixel(VLRContext context, int32_t x, int32_t y) {
    if (!context) {
        printf("[VLR] vlrSetProbePixel: invalid context (null)\n");
        fflush(stdout);
        return static_cast<VLRResult>(VLRResult_InvalidArgument);
    }
    if (x != -1 && (x < 0 || y < 0)) {
        printf("[VLR] vlrSetProbePixel: invalid pixel (%d, %d), use (-1, y) to disable\n",
               static_cast<int>(x), static_cast<int>(y));
        fflush(stdout);
        return static_cast<VLRResult>(VLRResult_InvalidArgument);
    }
    try {
        VLRContextImpl* impl = TO_CTX(context);
        if (!impl->ctx) {
            printf("[VLR] vlrSetProbePixel: context not initialized\n");
            fflush(stdout);
            return static_cast<VLRResult>(VLRResult_InvalidArgument);
        }
        impl->ctx->setProbePixel(x, y);
        if (x == -1) {
            printf("[VLR] vlrSetProbePixel: probe disabled\n");
        } else {
            printf("[VLR] vlrSetProbePixel: set to (%d, %d)\n",
                   static_cast<int>(x), static_cast<int>(y));
        }
        fflush(stdout);
        return static_cast<VLRResult>(VLRResult_Success);
    } catch (...) {
        return translateException();
    }
}

}  // extern "C"
