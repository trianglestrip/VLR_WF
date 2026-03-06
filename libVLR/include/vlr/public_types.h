// ============================================================================
// VLR Public Types
// 
// This file defines public API types and enumerations for VLR.
// 
// Author: VLR Development Team
// Updated: 2026-03-07 - Added Wavefront Path Tracing renderer
// Environment: CUDA 13.1, OptiX 8.0.0, VS2022
// ============================================================================

#pragma once

#include <cstdint>

namespace vlr {

// ============================================================================
// Renderer Types
// ============================================================================

/// Renderer enumeration
/// Defines all available rendering algorithms in VLR
enum VLRRenderer : uint32_t {
    VLRRenderer_PathTracing = 0,           // Classic recursive path tracing
    VLRRenderer_LightTracing,              // Light tracing (from light sources)
    VLRRenderer_BidirectionalPathTracing,  // Bidirectional path tracing (BPT/LVC-BPT)
    VLRRenderer_WavefrontPathTracing,      // Wavefront path tracing (NEW - optimized for GPU)
    NumVLRRenderers
};


/// Get renderer name as string
inline const char* getRendererName(VLRRenderer renderer) {
    static const char* names[] = {
        "Path Tracing",
        "Light Tracing",
        "Bidirectional Path Tracing",
        "Wavefront Path Tracing"
    };
    
    if (renderer < NumVLRRenderers)
        return names[renderer];
    return "Unknown";
}


// ============================================================================
// Wavefront Configuration
// ============================================================================

/// Wavefront renderer configuration
struct WavefrontConfig {
    uint32_t maxPathLength;            // Maximum path length (default: 25)
    bool usePathSorting;               // Enable path sorting by material (default: true)
    bool useMaterialQueues;            // Enable material-specific queues (default: true)
    bool useStreamCompaction;          // Enable stream compaction (default: true)
    bool enablePerfStats;              // Enable performance statistics (default: false)
    
    // Constructor with defaults
    WavefrontConfig()
        : maxPathLength(25)
        , usePathSorting(true)
        , useMaterialQueues(true)
        , useStreamCompaction(true)
        , enablePerfStats(false)
    {}
};


// ============================================================================
// Render Settings
// ============================================================================

/// General render settings (applicable to all renderers)
struct RenderSettings {
    VLRRenderer renderer;              // Selected renderer
    uint32_t maxPathLength;            // Maximum path length
    uint32_t numSamplesPerPixel;       // Samples per pixel
    bool enableDenoiser;               // Enable denoiser
    
    // Wavefront-specific settings (only used when renderer == VLRRenderer_WavefrontPathTracing)
    WavefrontConfig wavefrontConfig;
    
    // Constructor with defaults
    RenderSettings()
        : renderer(VLRRenderer_PathTracing)
        , maxPathLength(25)
        , numSamplesPerPixel(1)
        , enableDenoiser(false)
    {}
};


// ============================================================================
// Camera Types
// ============================================================================

enum VLRCameraType : uint32_t {
    VLRCameraType_Perspective = 0,
    VLRCameraType_Equirectangular,
    NumVLRCameraTypes
};


// ============================================================================
// Material Types
// ============================================================================

enum VLRMaterialType : uint32_t {
    VLRMaterialType_Matte = 0,
    VLRMaterialType_SpecularReflection,
    VLRMaterialType_SpecularScattering,
    VLRMaterialType_Microfacet,
    VLRMaterialType_Disney,
    NumVLRMaterialTypes
};


// ============================================================================
// Spectrum Types
// ============================================================================

enum VLRSpectrumType : uint32_t {
    VLRSpectrumType_RGB = 0,
    VLRSpectrumType_Spectral,
    NumVLRSpectrumTypes
};


// ============================================================================
// Image Format Types
// ============================================================================

enum VLRImageFormat : uint32_t {
    VLRImageFormat_RGB8 = 0,
    VLRImageFormat_RGBA8,
    VLRImageFormat_RGB32F,
    VLRImageFormat_RGBA32F,
    NumVLRImageFormats
};


// ============================================================================
// Error Codes
// ============================================================================

enum VLRResult : int32_t {
    VLRResult_Success = 0,
    VLRResult_InvalidArgument = -1,
    VLRResult_OutOfMemory = -2,
    VLRResult_NotImplemented = -3,
    VLRResult_InternalError = -4,
    VLRResult_CUDAError = -5,
    VLRResult_OptiXError = -6,
};


/// Get error message string
inline const char* getResultString(VLRResult result) {
    static const char* messages[] = {
        "Success",
        "Invalid argument",
        "Out of memory",
        "Not implemented",
        "Internal error",
        "CUDA error",
        "OptiX error"
    };
    
    int index = -static_cast<int>(result);
    if (index >= 0 && index < 7)
        return messages[index];
    return "Unknown error";
}


// ============================================================================
// Version Information
// ============================================================================

struct VLRVersion {
    uint32_t major;
    uint32_t minor;
    uint32_t patch;
    const char* string;
};

constexpr VLRVersion VLR_VERSION = {1, 0, 0, "1.0.0-wavefront-alpha"};

} // namespace vlr
