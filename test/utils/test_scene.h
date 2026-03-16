// ============================================================================
// VLR 测试工具 - 场景创建辅助函数
// ============================================================================

#pragma once

#include <vlr/vlr.h>
#include <cstdio>
#include <cstdint>

namespace vlr_test {

static inline VLRResult createMatteMaterial(
    VLRScene scene, const float* color, const float* emission, 
    VLRMaterial* outMat, const char* name) {
    VLRResult res = vlrCreateMaterial(scene, 0 /* Matte */, color, emission, outMat);
    if (res != VLRResult_Success) {
        fprintf(stderr, "[Error] %s material creation failed\n", name);
    }
    return res;
}

static inline VLRResult createQuadMesh(
    VLRScene scene, const float* vertices, const uint32_t* indices,
    VLRMaterial material, VLRTriangleMesh* outMesh, const char* name) {
    VLRResult res = vlrCreateTriangleMesh(scene, vertices, 4, indices, 2, material, outMesh);
    if (res != VLRResult_Success) {
        fprintf(stderr, "[Error] %s mesh creation failed\n", name);
    }
    return res;
}

static inline VLRResult createSimpleInstance(
    VLRScene scene, VLRTriangleMesh mesh, 
    const float* origin, const float* scale, const float* axis, float angle,
    VLRInstance* outInstance, const char* name) {
    VLRResult res = vlrCreateInstance(scene, mesh, origin, scale, axis, angle, outInstance);
    if (res != VLRResult_Success) {
        fprintf(stderr, "[Error] %s instance creation failed\n", name);
    }
    return res;
}

} // namespace vlr_test
