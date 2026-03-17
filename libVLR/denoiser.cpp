#include "denoiser.h"
#include <optix_stubs.h>
#include <optix_host.h>
#include <stdexcept>
#include <iostream>

namespace vlr {

// CUDA 错误检查宏
#define CUDA_CHECK(call) \
    do { \
        cudaError_t error = call; \
        if (error != cudaSuccess) { \
            throw std::runtime_error(std::string("CUDA error: ") + cudaGetErrorString(error)); \
        } \
    } while(0)

// OptiX 错误检查宏
#define OPTIX_CHECK(call) \
    do { \
        OptixResult res = call; \
        if (res != OPTIX_SUCCESS) { \
            throw std::runtime_error(std::string("OptiX error: ") + optixGetErrorName(res)); \
        } \
    } while(0)

Denoiser::~Denoiser() {
    cleanup();
}

void Denoiser::initialize(uint32_t width, uint32_t height, const DenoiserConfig& config, OptixDeviceContext context) {
    m_width = width;
    m_height = height;
    m_config = config;
    
    // Create denoiser options based on config
    OptixDenoiserOptions options = {};
    options.guideAlbedo = m_config.useAlbedo ? 1 : 0;
    options.guideNormal = m_config.useNormal ? 1 : 0;
    
    // Create OptiX denoiser
    OPTIX_CHECK(optixDenoiserCreate(
        context,
        OPTIX_DENOISER_MODEL_KIND_HDR,
        &options,
        &m_denoiser
    ));
    
    // Get denoiser memory requirements
    OPTIX_CHECK(optixDenoiserComputeMemoryResources(
        m_denoiser,
        m_width,
        m_height,
        &m_denoiserSizes
    ));
    
    // Allocate memory
    size_t stateSize = m_denoiserSizes.stateSizeInBytes;
    size_t scratchSize = m_denoiserSizes.withoutOverlapScratchSizeInBytes;
    size_t bufferSize = m_width * m_height * sizeof(float3);
    
    CUDA_CHECK(cudaMalloc(reinterpret_cast<void**>(&m_d_denoiserState), stateSize));
    CUDA_CHECK(cudaMalloc(reinterpret_cast<void**>(&m_d_denoiserScratch), scratchSize));
    CUDA_CHECK(cudaMalloc(reinterpret_cast<void**>(&m_d_denoiseInput), bufferSize));
    CUDA_CHECK(cudaMalloc(reinterpret_cast<void**>(&m_d_denoiseOutput), bufferSize));
    
    if (m_config.useAlbedo) {
        CUDA_CHECK(cudaMalloc(reinterpret_cast<void**>(&m_d_denoiseAlbedo), bufferSize));
    }
    if (m_config.useNormal) {
        CUDA_CHECK(cudaMalloc(reinterpret_cast<void**>(&m_d_denoiseNormal), bufferSize));
    }
    
    // Setup denoiser
    OPTIX_CHECK(optixDenoiserSetup(
        m_denoiser,
        nullptr, // Use default stream
        m_width,
        m_height,
        m_d_denoiserState,
        stateSize,
        m_d_denoiserScratch,
        scratchSize
    ));
    
    std::cout << "[Denoiser] 已初始化 " << m_width << "x" << m_height << " 分辨率" << std::endl;
    std::cout << "[Denoiser] Albedo: " << (m_config.useAlbedo ? "启用" : "禁用") 
              << ", Normal: " << (m_config.useNormal ? "启用" : "禁用") << std::endl;
}

void Denoiser::denoise(CUdeviceptr input, CUdeviceptr output, CUdeviceptr albedo, CUdeviceptr normal, uint32_t numSamples) {
    if (!m_denoiser) {
        throw std::runtime_error("Denoiser not initialized");
    }
    
    // Copy input data
    size_t bufferSize = m_width * m_height * sizeof(float3);
    CUDA_CHECK(cudaMemcpy(
        reinterpret_cast<void*>(m_d_denoiseInput),
        reinterpret_cast<void*>(input),
        bufferSize,
        cudaMemcpyDeviceToDevice
    ));
    
    // Prepare denoiser params
    OptixDenoiserParams denoiserParams = {};
    denoiserParams.hdrIntensity = 0; // Auto exposure
    denoiserParams.blendFactor = 0.0f;
    
    // Prepare guide layer
    OptixDenoiserGuideLayer guideLayer = {};
    
    // Copy and setup albedo guide (if provided and enabled)
    if (m_config.useAlbedo && albedo && m_d_denoiseAlbedo) {
        CUDA_CHECK(cudaMemcpy(
            reinterpret_cast<void*>(m_d_denoiseAlbedo),
            reinterpret_cast<void*>(albedo),
            bufferSize,
            cudaMemcpyDeviceToDevice
        ));
        
        guideLayer.albedo.data = m_d_denoiseAlbedo;
        guideLayer.albedo.width = m_width;
        guideLayer.albedo.height = m_height;
        guideLayer.albedo.rowStrideInBytes = m_width * sizeof(float3);
        guideLayer.albedo.pixelStrideInBytes = sizeof(float3);
        guideLayer.albedo.format = OPTIX_PIXEL_FORMAT_FLOAT3;
    }
    
    // Copy and setup normal guide (if provided and enabled)
    if (m_config.useNormal && normal && m_d_denoiseNormal) {
        CUDA_CHECK(cudaMemcpy(
            reinterpret_cast<void*>(m_d_denoiseNormal),
            reinterpret_cast<void*>(normal),
            bufferSize,
            cudaMemcpyDeviceToDevice
        ));
        
        guideLayer.normal.data = m_d_denoiseNormal;
        guideLayer.normal.width = m_width;
        guideLayer.normal.height = m_height;
        guideLayer.normal.rowStrideInBytes = m_width * sizeof(float3);
        guideLayer.normal.pixelStrideInBytes = sizeof(float3);
        guideLayer.normal.format = OPTIX_PIXEL_FORMAT_FLOAT3;
    }
    
    // Prepare input/output layer
    OptixDenoiserLayer layer = {};
    layer.input.data = m_d_denoiseInput;
    layer.input.width = m_width;
    layer.input.height = m_height;
    layer.input.rowStrideInBytes = m_width * sizeof(float3);
    layer.input.pixelStrideInBytes = sizeof(float3);
    layer.input.format = OPTIX_PIXEL_FORMAT_FLOAT3;
    
    layer.output.data = m_d_denoiseOutput;
    layer.output.width = m_width;
    layer.output.height = m_height;
    layer.output.rowStrideInBytes = m_width * sizeof(float3);
    layer.output.pixelStrideInBytes = sizeof(float3);
    layer.output.format = OPTIX_PIXEL_FORMAT_FLOAT3;
    
    // Execute denoising
    OPTIX_CHECK(optixDenoiserInvoke(
        m_denoiser,
        nullptr, // Use default stream
        &denoiserParams,
        m_d_denoiserState,
        m_denoiserSizes.stateSizeInBytes,
        &guideLayer,
        &layer,
        1, // Process one layer
        0, // inputOffsetX
        0, // inputOffsetY
        m_d_denoiserScratch,
        m_denoiserSizes.withoutOverlapScratchSizeInBytes
    ));
    
    // Copy result to output buffer
    CUDA_CHECK(cudaMemcpy(
        reinterpret_cast<void*>(output),
        reinterpret_cast<void*>(m_d_denoiseOutput),
        bufferSize,
        cudaMemcpyDeviceToDevice
    ));
    
    // 降噪完成（静默运行，避免每帧日志刷屏）
}

void Denoiser::cleanup() {
    if (m_d_denoiserState) {
        cudaFree(reinterpret_cast<void*>(m_d_denoiserState));
        m_d_denoiserState = 0;
    }
    if (m_d_denoiserScratch) {
        cudaFree(reinterpret_cast<void*>(m_d_denoiserScratch));
        m_d_denoiserScratch = 0;
    }
    if (m_d_denoiseInput) {
        cudaFree(reinterpret_cast<void*>(m_d_denoiseInput));
        m_d_denoiseInput = 0;
    }
    if (m_d_denoiseOutput) {
        cudaFree(reinterpret_cast<void*>(m_d_denoiseOutput));
        m_d_denoiseOutput = 0;
    }
    if (m_d_denoiseAlbedo) {
        cudaFree(reinterpret_cast<void*>(m_d_denoiseAlbedo));
        m_d_denoiseAlbedo = 0;
    }
    if (m_d_denoiseNormal) {
        cudaFree(reinterpret_cast<void*>(m_d_denoiseNormal));
        m_d_denoiseNormal = 0;
    }
    if (m_denoiser) {
        optixDenoiserDestroy(m_denoiser);
        m_denoiser = nullptr;
    }
    
    m_width = 0;
    m_height = 0;
}

} // namespace vlr
