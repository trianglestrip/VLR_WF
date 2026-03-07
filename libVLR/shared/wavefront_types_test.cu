// ============================================================================
// Wavefront 类型测试
//
// 本文件测试 Wavefront 数据结构的编译和大小。
//
// 编译命令：nvcc -std=c++17 -arch=sm_75 path_types_test.cu -o test.exe
// ============================================================================

#include "path_types.h"
#include <stdio.h>

using namespace vlr::shared;

// 测试内核以验证设备端编译
__global__ void testKernel() {
    WavefrontPathState pathState;
    pathState.setActive(true);
    pathState.pathLength = 0;

    WavefrontHitInfo hitInfo;
    hitInfo.reset();
}

int main() {
    printf("=== VLR Wavefront Data Structure Size Verification ===\n\n");

    // 测试 WavefrontPathState
    printf("WavefrontPathState:\n");
    printf("  Size: %zu bytes (expected: 144 bytes)\n", sizeof(WavefrontPathState));
    printf("  Alignment: %zu bytes\n", alignof(WavefrontPathState));

    if (sizeof(WavefrontPathState) == 144) {
        printf("  [OK] Size correct!\n");
    } else {
        printf("  [FAIL] Size mismatch!\n");
    }
    printf("\n");

    // 测试 WavefrontHitInfo
    printf("WavefrontHitInfo:\n");
    printf("  Size: %zu bytes (expected: 32 bytes)\n", sizeof(WavefrontHitInfo));
    printf("  Alignment: %zu bytes\n", alignof(WavefrontHitInfo));

    if (sizeof(WavefrontHitInfo) == 32) {
        printf("  [OK] Size correct!\n");
    } else {
        printf("  [FAIL] Size mismatch!\n");
    }
    printf("\n");

    // 测试 WavefrontWorkQueue
    printf("WavefrontWorkQueue:\n");
    printf("  Size: %zu bytes\n", sizeof(WavefrontWorkQueue));
    printf("  Alignment: %zu bytes\n", alignof(WavefrontWorkQueue));
    printf("\n");

    // 测试 WavefrontMaterialQueues
    printf("WavefrontMaterialQueues:\n");
    printf("  Size: %zu bytes\n", sizeof(WavefrontMaterialQueues));
    printf("  Category count: %d\n", NumMaterialCategories);
    printf("\n");

    // 测试 WavefrontLaunchParameters
    printf("WavefrontLaunchParameters：\n");
    printf("  大小：%zu 字节\n", sizeof(WavefrontLaunchParameters));
    printf("  对齐：%zu 字节\n", alignof(WavefrontLaunchParameters));
    printf("\n");

    // 测试 WFTracePayload
    printf("WFTracePayload：\n");
    printf("  大小：%zu 字节（期望：28 字节）\n", sizeof(WFTracePayload));
    printf("  对齐：%zu 字节\n", alignof(WFTracePayload));
    printf("\n");

    // 测试辅助结构
    printf("WavefrontMaterialEvaluation：\n");
    printf("  大小：%zu 字节\n", sizeof(WavefrontMaterialEvaluation));
    printf("\n");

    printf("WavefrontLightSample：\n");
    printf("  大小：%zu 字节\n", sizeof(WavefrontLightSample));
    printf("\n");

    printf("WavefrontBSDFSample：\n");
    printf("  大小：%zu 字节\n", sizeof(WavefrontBSDFSample));
    printf("\n");

    printf("WavefrontPerformanceStats：\n");
    printf("  大小：%zu 字节\n", sizeof(WavefrontPerformanceStats));
    printf("\n");

    // 测试基本类型
    printf("=== 基本类型 ===\n");
    printf("KernelRNG：%zu 字节（期望：16 字节）\n", sizeof(KernelRNG));
    printf("WavelengthSamples：%zu 字节（期望：24 字节）\n", sizeof(WavelengthSamples));
    printf("SampledSpectrum：%zu 字节\n", sizeof(SampledSpectrum));
    printf("Vector3D：%zu 字节\n", sizeof(Vector3D));
    printf("ReferenceFrame：%zu 字节\n", sizeof(ReferenceFrame));
    printf("\n");

    // 测试枚举
    printf("=== 枚举 ===\n");
    printf("MaterialCategory：%d 个类别\n", NumMaterialCategories);
    printf("WFRayType：%d 种类型\n", NumWFRayTypes);
    printf("\n");

    // 测试配置常量
    printf("=== 配置 ===\n");
    printf("默认最大路径长度：%u\n", WavefrontConfig::DefaultMaxPathLength);
    printf("RR 起始深度：%u\n", WavefrontConfig::RRStartDepth);
    printf("RR 阈值：%.3f\n", WavefrontConfig::RRThreshold);
    printf("块大小：%u\n", WavefrontConfig::BlockSize);
    printf("使用路径排序：%s\n", WavefrontConfig::UsePathSorting ? "是" : "否");
    printf("使用材质队列：%s\n", WavefrontConfig::UseMaterialQueues ? "是" : "否");
    printf("使用流压缩：%s\n", WavefrontConfig::UseStreamCompaction ? "是" : "否");
    printf("\n");

    // 测试版本
    printf("=== 版本 ===\n");
    printf("Wavefront 版本：%s\n", WavefrontVersion::String);
    printf("版本号：%u.%u.%u\n",
        WavefrontVersion::Major,
        WavefrontVersion::Minor,
        WavefrontVersion::Patch);
    printf("\n");

    // 启动测试内核
    printf("=== 设备编译测试 ===\n");
    testKernel<<<1, 1>>>();
    cudaError_t err = cudaDeviceSynchronize();
    if (err == cudaSuccess) {
        printf("✓ 设备内核编译并执行成功！\n");
    } else {
        printf("✗ 设备内核失败：%s\n", cudaGetErrorString(err));
    }
    printf("\n");

    printf("=== 所有测试已完成 ===\n");

    return 0;
}
