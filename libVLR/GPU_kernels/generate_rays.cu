// ============================================================================
// VLR Wavefront - GenerateRays Kernel
//
// ??????Wavefront ???????????????
// ???RNG ??????????????IDF ???PathState ????????????
//
// ???VLR ?????
// ??????026-03-07
// ???CUDA 13.1, OptiX 8.0.0, VS2022
// ============================================================================

#define VLR_DEBUG_GENERATE_RAYS 0

// ??????
// #define VLR_ENABLE_GPU_DEBUG 1

#ifdef VLR_ENABLE_GPU_DEBUG
    #define VLR_DEBUG_PRINTF(...) printf(__VA_ARGS__)
#else
    #define VLR_DEBUG_PRINTF(...) ((void)0)
#endif

#include "../shared/path_types.h"
#include "../include/vlr/basic_types.h"

#include <cuda_runtime.h>

// ????????
// 1. ?? CUDA ??????generateRays(params) ??????????
// 2. OptiX ???? WavefrontLaunchParameters ????????????__constant__
//    ???kernel ?? Ray Gen ????? pipeline ??launch params ????wrapper ??

// ============================================================================
// ??????????
// ============================================================================

namespace {

using namespace vlr;
using namespace vlr::shared;

/// ?????????????? callable ????
/// ??lensRadius = 0 ??????
CUDA_DEVICE_FUNCTION CUDA_INLINE void samplePerspectiveCamera(
    const CameraDescriptor& camera,
    float pixelCoordX,
    float pixelCoordY,
    uint32_t imageWidth,
    uint32_t imageHeight,
    Point3D* rayOrigin,
    Vector3D* rayDirection,
    float* dirPDF) {
    
    // ???? + [0,1) ????????????
    float vh = 2.0f * tanf(camera.fovY * 0.5f);
    float vw = camera.aspect * vh;
    
    // Pixel to NDC [-0.5, 0.5]
    float ndcX = (pixelCoordX / static_cast<float>(imageWidth)) - 0.5f;
    float ndcY = (pixelCoordY / static_cast<float>(imageHeight)) - 0.5f;
    
    // ??????????????
    *rayOrigin = camera.position;
    
    // ????????????????
    Vector3D rayDir = normalize(
        camera.orientation.x * (vw * ndcX) +
        camera.orientation.y * (vh * ndcY) +
        camera.orientation.z);
    
    *rayDirection = rayDir;
    
    // ????????PDF???? VLR PerspectiveCameraIDF ?????
    // dirPDF = imageSize.x * imageSize.y / (cos^3 * imgPlaneArea)
    // ?? imgPlaneArea = opWidth * opHeight?opHeight = 2*tan(fovY/2)?opWidth = aspect * opHeight
    // orientation.z ??????
    float cosTheta = dot(rayDir, camera.orientation.z);
    if (cosTheta <= 0.0f) cosTheta = 1e-6f;
    float cos3 = cosTheta * cosTheta * cosTheta;
    float imgPlaneArea = vw * vh;
    *dirPDF = (static_cast<float>(imageWidth) * static_cast<float>(imageHeight)) / (cos3 * imgPlaneArea);
}

}  // anonymous namespace

// ============================================================================
// GenerateRays Kernel
// ============================================================================
// ??????????????__constant__ ????????struct ????

extern "C" __global__ void generateRays(
    vlr::shared::WavefrontLaunchParameters* params) {
    using namespace vlr::shared;
    WavefrontLaunchParameters& wlp = *params;
    
#ifdef __CUDACC__
    // ????????????
    uint32_t pixelX = blockIdx.x * blockDim.x + threadIdx.x;
    uint32_t pixelY = blockIdx.y * blockDim.y + threadIdx.y;
    
    // ?????
    if (pixelX >= wlp.imageSize.x || pixelY >= wlp.imageSize.y)
        return;
    
    uint32_t pathIndex = pixelY * wlp.imageSize.x + pixelX;
    
    // ========================================================================
    // 1. RNG ?????? PCG32??
    // ========================================================================
    KernelRNG rng;
    if (wlp.rngBuffer.data != nullptr) {
        uint32_t stride = wlp.imageStrideInPixels > 0 ? wlp.imageStrideInPixels : wlp.imageSize.x;
        uint32_t pixelIdx = pixelY * stride + pixelX;
        rng = wlp.rngBuffer.data[pixelIdx];
    } else {
        // ??RNG ????????????????
        uint64_t seed = (static_cast<uint64_t>(wlp.numAccumFrames) * wlp.imageSize.x * wlp.imageSize.y + pathIndex) * 0x853c49e6748fea9bULL;
        rng.state = seed ^ 0xda3e39cb94b95bdbULL;
        rng.inc = 0xda3e39cb94b95bdbULL;
    }
    
    // ========================================================================
    // 2. ?????? ??????
    // ========================================================================
    float selectWLPDF;
    WavelengthSamples wls = WavelengthSamples::createWithEqualOffsets(
        rng.getFloat0cTo1o(),
        rng.getFloat0cTo1o(),
        &selectWLPDF);
    
    // ========================================================================
    // 3. ?????Perspective ??????
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
    // 4. IDF ????????
    // ========================================================================
    // ???????????We ???? PDF ????????throughput = 1
    // ???https://agraphicsguynotes.com/posts/the_missing_primary_ray_pdf_in_path_tracing/
    // ????throughput = (We*cos)/(areaPDF*dirPDF*selectWLPDF) ??dirPDF ??????
    SampledSpectrum We = SampledSpectrum::One();
    
    float areaPDF = 1.0f;
    if (camera.cameraType == CameraType_Perspective && camera.lensRadius > 0.0f) {
        float lensArea = VLR_M_PI * camera.lensRadius * camera.lensRadius;
        areaPDF = 1.0f / lensArea;
    }
    
    float cosTheta = dot(rayDirection, camera.orientation.z);
    if (cosTheta <= 0.0f) cosTheta = 1e-6f;
    
    SampledSpectrum throughput;
    if (camera.lensRadius <= 0.0f) {
        // ?????PDF ????throughput = 1??????????radiance??
        throughput = SampledSpectrum::One();
    } else {
        // ????????????
        throughput = (We * cosTheta) / (areaPDF * dirPDF * selectWLPDF);
    }
    
    // ========================================================================
    // 5. ????PathState ?????
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
    
    // ?? HitInfo??????
    if (wlp.hitInfoBuffer != nullptr) {
        wlp.hitInfoBuffer[pathIndex].reset();
    }
    
#ifdef VLR_DEBUG_GENERATE_RAYS
    if (pixelX == 256 && pixelY == 256) {
        VLR_DEBUG_PRINTF("[GPU GenerateRays] pixel(%u,%u): origin=(%.3f,%.3f,%.3f), dir=(%.3f,%.3f,%.3f), dirPDF=%.6f\n",
               pixelX, pixelY,
               rayOrigin.x, rayOrigin.y, rayOrigin.z,
               rayDirection.x, rayDirection.y, rayDirection.z,
               dirPDF);
    }
#endif
    
    // ========================================================================
    // 6. ??????????
    // ========================================================================
    // ??????? pathIndex ????????:1 ????
    wlp.activePathQueue.pathIndices[pathIndex] = pathIndex;
    
    // ??????????????????
    if (pathIndex == wlp.imageSize.x * wlp.imageSize.y - 1) {
        // ?????????????
        *wlp.activePathQueue.counter = wlp.imageSize.x * wlp.imageSize.y;
    }
    
    // ========================================================================
    // 7. ?? Denoiser ??????
    // ========================================================================
    // ??????????????????????????
    uint32_t stride = wlp.imageStrideInPixels > 0 ? wlp.imageStrideInPixels : wlp.imageSize.x;
    uint32_t pixelIdx = pixelY * stride + pixelX;
    
    if (wlp.accumAlbedoBuffer != nullptr) {
        // ????????????????????
        wlp.accumAlbedoBuffer[pixelIdx].r = 0.0f;
        wlp.accumAlbedoBuffer[pixelIdx].g = 0.0f;
        wlp.accumAlbedoBuffer[pixelIdx].b = 0.0f;
    }
    
    if (wlp.accumNormalBuffer != nullptr) {
        // ??????????????????????
        wlp.accumNormalBuffer[pixelIdx].x = rayDirection.x;
        wlp.accumNormalBuffer[pixelIdx].y = rayDirection.y;
        wlp.accumNormalBuffer[pixelIdx].z = rayDirection.z;
    }
#endif
}
