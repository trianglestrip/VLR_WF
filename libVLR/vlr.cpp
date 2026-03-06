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
#include "include/vlr/public_types.h"
#include "include/vlr/basic_types.h"
#include <cuda_runtime.h>
#include <cmath>
#include <cstring>
#include <new>
#include <stdexcept>

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

// 将 C 句柄转换为实现
#define TO_CTX(h) (reinterpret_cast<VLRContextImpl*>(h))
#define TO_SCENE(h) (reinterpret_cast<VLRSceneImpl*>(h))
#define TO_MESH(h) (reinterpret_cast<VLRTriangleMeshImpl*>(h))
#define TO_MAT(h) (reinterpret_cast<VLRMaterialImpl*>(h))
#define TO_INST(h) (reinterpret_cast<VLRInstanceImpl*>(h))

// 从实现获取句柄
#define FROM_CTX(p) (reinterpret_cast<VLRContext>(p))
#define FROM_SCENE(p) (reinterpret_cast<VLRScene>(p))
#define FROM_MESH(p) (reinterpret_cast<VLRTriangleMesh>(p))
#define FROM_MAT(p) (reinterpret_cast<VLRMaterial>(p))
#define FROM_INST(p) (reinterpret_cast<VLRInstance>(p))

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
        cudaStream_t stream = static_cast<cudaStream_t>(cudaStream);
        if (!stream) {
            cudaStreamCreate(&stream);
        }
        VLRContextImpl* impl = new VLRContextImpl();
        impl->ctx = new vlr::Context(stream, enableLogging != 0);
        *outContext = FROM_CTX(impl);
        return static_cast<VLRResult>(VLRResult_Success);
    } catch (...) {
        return translateException();
    }
}

void vlrDestroyContext(VLRContext context) {
    if (!context) return;
    VLRContextImpl* impl = TO_CTX(context);
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
        float r = baseColor ? baseColor[0] : 0.7f;
        float g = baseColor ? baseColor[1] : 0.7f;
        float b = baseColor ? baseColor[2] : 0.7f;
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

void vlrGetVersion(uint32_t* major, uint32_t* minor, uint32_t* patch) {
    if (major) *major = vlr::VLR_VERSION.major;
    if (minor) *minor = vlr::VLR_VERSION.minor;
    if (patch) *patch = vlr::VLR_VERSION.patch;
}

}  // extern "C"
