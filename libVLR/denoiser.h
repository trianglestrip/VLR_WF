#pragma once

#include <optix.h>
#include <optix_types.h>
#include <cuda_runtime.h>
#include <cstdint>

namespace vlr {

// 降噪器配置
struct DenoiserConfig {
    bool enabled = false;
    bool useAlbedo = true;    // 使用 albedo guide layer
    bool useNormal = true;    // 使用 normal guide layer
    float hdrIntensity = 1.0f;
};

// OptiX 降噪器实现
class Denoiser {
public:
    Denoiser() = default;
    ~Denoiser();
    
    void initialize(uint32_t width, uint32_t height, const DenoiserConfig& config, OptixDeviceContext context);
    void denoise(CUdeviceptr input, CUdeviceptr output, 
                 CUdeviceptr albedo = 0, CUdeviceptr normal = 0, 
                 uint32_t numSamples = 1);
    void cleanup();
    
    // Getters
    uint32_t getWidth() const { return m_width; }
    uint32_t getHeight() const { return m_height; }
    const DenoiserConfig& getConfig() const { return m_config; }
    bool isInitialized() const { return m_denoiser != nullptr; }
    
private:
    OptixDenoiser m_denoiser = nullptr;
    OptixDenoiserSizes m_denoiserSizes = {};
    CUdeviceptr m_d_denoiserState = 0;
    CUdeviceptr m_d_denoiserScratch = 0;
    CUdeviceptr m_d_denoiseInput = 0;
    CUdeviceptr m_d_denoiseOutput = 0;
    CUdeviceptr m_d_denoiseAlbedo = 0;
    CUdeviceptr m_d_denoiseNormal = 0;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    DenoiserConfig m_config;
};

} // namespace vlr
