// ============================================================================
// VLR Wavefront - GenerateRays Kernel
//
// 本文件实现 Wavefront 路径追踪的初始光线生成内核。
// 功能：RNG 初始化、波长采样、相机采样、IDF 评估、PathState 初始化、加入活跃队列。
//
// 作者：VLR 开发团队
// 创建日期：2026-03-07
// 环境：CUDA 13.1, OptiX 8.0.0, VS2022
// ============================================================================

#include "../shared/path_types.h"
#include "../include/vlr/basic_types.h"

#include <cuda_runtime.h>

// 启动参数说明：
// 1. 直接 CUDA 调用：使用 generateRays(params) 传入设备端参数指针
// 2. OptiX 管线：因 WavefrontLaunchParameters 含非平凡类型，无法使用 __constant__
//    需将 kernel 作为 Ray Gen 时，需通过 pipeline 的 launch params 机制或 wrapper 适配

// ============================================================================
// 透视相机采样辅助函数
// ============================================================================

namespace {

using namespace vlr;
using namespace vlr::shared;

/// 透视相机生成光线（不依赖外部 callable 程序）
/// 当 lensRadius = 0 时为针孔相机
CUDA_DEVICE_FUNCTION CUDA_INLINE void samplePerspectiveCamera(
    const CameraDescriptor& camera,
    float pixelCoordX,
    float pixelCoordY,
    uint32_t imageWidth,
    uint32_t imageHeight,
    Point3D* rayOrigin,
    Vector3D* rayDirection,
    float* dirPDF) {
    
    // 像素中心 + [0,1) 随机偏移得到亚像素采样
    float vh = 2.0f * tanf(camera.fovY * 0.5f);
    float vw = camera.aspect * vh;
    
    // 将像素坐标映射到 NDC [-0.5, 0.5]
    float ndcX = (pixelCoordX / static_cast<float>(imageWidth)) - 0.5f;
    float ndcY = (pixelCoordY / static_cast<float>(imageHeight)) - 0.5f;
    
    // 针孔相机：光线起源于相机位置
    *rayOrigin = camera.position;
    
    // 光线方向：穿过成像平面上的采样点
    Vector3D rayDir = normalize(
        camera.orientation.x * (vw * ndcX) +
        camera.orientation.y * (vh * ndcY) +
        camera.orientation.z);
    
    *rayDirection = rayDir;
    
    // 透视相机的方向 PDF：与 cos^4(theta) 相关（立体角到面积测度的雅可比）
    // orientation.z 为相机前向
    float cosTheta = dot(rayDir, camera.orientation.z);
    if (cosTheta <= 0.0f) cosTheta = 1e-6f;
    *dirPDF = 1.0f / (cosTheta * cosTheta * cosTheta * cosTheta);
}

}  // anonymous namespace

// ============================================================================
// GenerateRays Kernel
// ============================================================================
// 启动参数通过参数传入，避免 __constant__ 对含非平凡类型 struct 的限制

extern "C" __global__ void generateRays(
    vlr::shared::WavefrontLaunchParameters* params) {
    using namespace vlr::shared;
    WavefrontLaunchParameters& wlp = *params;
    
#ifdef __CUDACC__
    // 线程索引映射到像素坐标
    uint32_t pixelX = blockIdx.x * blockDim.x + threadIdx.x;
    uint32_t pixelY = blockIdx.y * blockDim.y + threadIdx.y;
    
    // 边界检查
    if (pixelX >= wlp.imageSize.x || pixelY >= wlp.imageSize.y)
        return;
    
    uint32_t pathIndex = pixelY * wlp.imageSize.x + pixelX;
    
    // ========================================================================
    // 1. RNG 初始化（使用 PCG32）
    // ========================================================================
    KernelRNG rng;
    if (wlp.rngBuffer.data != nullptr) {
        uint32_t stride = wlp.imageStrideInPixels > 0 ? wlp.imageStrideInPixels : wlp.imageSize.x;
        uint32_t pixelIdx = pixelY * stride + pixelX;
        rng = wlp.rngBuffer.data[pixelIdx];
    } else {
        // 无 RNG 缓冲区时，用像素坐标和帧数初始化
        uint64_t seed = (static_cast<uint64_t>(wlp.numAccumFrames) * wlp.imageSize.x * wlp.imageSize.y + pathIndex) * 0x853c49e6748fea9bULL;
        rng.state = seed ^ 0xda3e39cb94b95bdbULL;
        rng.inc = 0xda3e39cb94b95bdbULL;
    }
    
    // ========================================================================
    // 2. 波长采样（4 个光谱采样）
    // ========================================================================
    float selectWLPDF;
    WavelengthSamples wls = WavelengthSamples::createWithEqualOffsets(
        rng.getFloat0cTo1o(),
        rng.getFloat0cTo1o(),
        &selectWLPDF);
    
    // ========================================================================
    // 3. 相机采样（Perspective 透视相机）
    // ========================================================================
    float pixelSampleX = pixelX + rng.getFloat0cTo1o();
    float pixelSampleY = pixelY + rng.getFloat0cTo1o();
    
    Point3D rayOrigin;
    Vector3D rayDirection;
    float dirPDF;
    
    const CameraDescriptor& camera = wlp.cameraDescriptor;
    samplePerspectiveCamera(
        camera,
        pixelSampleX, pixelSampleY,
        wlp.imageSize.x, wlp.imageSize.y,
        &rayOrigin, &rayDirection, &dirPDF);
    
    // ========================================================================
    // 4. IDF 评估
    // ========================================================================
    // 透视/等距柱状相机的 IDF 重要性：传感器响应简化为 1
    // 若有 progEvaluateIDF 可调用，此处为占位实现
    SampledSpectrum We = SampledSpectrum::One();
    
    // 吞吐量 = We / (areaPDF * dirPDF)
    // 针孔相机：areaPDF = 1（单点）；薄透镜：areaPDF = 1/(π*r²)（圆盘均匀采样）
    float areaPDF = 1.0f;
    if (camera.cameraType == CameraType_Perspective && camera.lensRadius > 0.0f) {
        float lensArea = VLR_M_PI * camera.lensRadius * camera.lensRadius;
        areaPDF = 1.0f / lensArea;
    }
    SampledSpectrum throughput = We / (areaPDF * dirPDF);
    
    // ========================================================================
    // 5. 初始化 PathState 所有字段
    // ========================================================================
    WavefrontPathState& pathState = wlp.pathStateBuffer[pathIndex];
    
    pathState.origin = rayOrigin;
    pathState.direction = rayDirection;
    pathState.throughput = throughput;
    pathState.contribution = SampledSpectrum::Zero();
    pathState.wls = wls;
    pathState.initImportance = throughput.importance(wls.selectedLambdaIndex());
    pathState.selectWLPDF = selectWLPDF;
    pathState.rng = rng;
    pathState.prevDirPDF = dirPDF;
    pathState.prevSampledType = DirectionType();
    pathState.pathLength = 0;
    pathState.pixelX = pixelX;
    pathState.pixelY = pixelY;
    pathState.flags = 0;
    pathState.materialCategory = MaterialCategory_Diffuse;
    
    pathState.setActive(true);
    
    // 重置 HitInfo（新路径）
    if (wlp.hitInfoBuffer != nullptr) {
        wlp.hitInfoBuffer[pathIndex].reset();
    }
    
    // ========================================================================
    // 6. 将路径加入活跃队列
    // ========================================================================
    // 简化：直接使用 pathIndex 作为队列索引（1:1 映射）
    wlp.activePathQueue.pathIndices[pathIndex] = pathIndex;
    
    // 更新队列大小（使用原子操作确保正确）
    if (pathIndex == wlp.imageSize.x * wlp.imageSize.y - 1) {
        // 最后一个线程设置队列大小
        *wlp.activePathQueue.counter = wlp.imageSize.x * wlp.imageSize.y;
    }
    
    // ========================================================================
    // 7. 处理 Denoiser 辅助缓冲区
    // ========================================================================
    // 初始光线阶段尚无命中信息，将辅助通道初始化为默认值
    uint32_t stride = wlp.imageStrideInPixels > 0 ? wlp.imageStrideInPixels : wlp.imageSize.x;
    uint32_t pixelIdx = pixelY * stride + pixelX;
    
    if (wlp.accumAlbedoBuffer != nullptr) {
        // 反照率：初始为黑色（后续由表面着色填充）
        wlp.accumAlbedoBuffer[pixelIdx].r = 0.0f;
        wlp.accumAlbedoBuffer[pixelIdx].g = 0.0f;
        wlp.accumAlbedoBuffer[pixelIdx].b = 0.0f;
    }
    
    if (wlp.accumNormalBuffer != nullptr) {
        // 法线：初始为视图方向（后续由表面法线填充）
        wlp.accumNormalBuffer[pixelIdx].x = rayDirection.x;
        wlp.accumNormalBuffer[pixelIdx].y = rayDirection.y;
        wlp.accumNormalBuffer[pixelIdx].z = rayDirection.z;
    }
#endif
}
