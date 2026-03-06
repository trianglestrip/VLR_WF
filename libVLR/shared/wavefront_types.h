// ============================================================================
// VLR Wavefront Path Tracing - Core Data Structures
// 
// This file defines all core data structures for the Wavefront rendering mode.
// 
// Author: VLR Development Team
// Created: 2026-03-07
// Environment: CUDA 13.1, OptiX 8.0.0, VS2022
// ============================================================================

#pragma once

#include "kernel_common.h"
#include <cstdio>

namespace vlr {
namespace shared {

// ============================================================================
// 1. Core Data Structures
// ============================================================================

/// Path State: Stores complete information for a single ray path
/// This is the core data structure of the Wavefront architecture
/// Size: 144 bytes (optimized for memory alignment and cache efficiency)
struct alignas(16) WavefrontPathState {
    // === Ray Information (32 bytes) ===
    Point3D origin;                    // Ray origin (12 bytes)
    Vector3D direction;                // Ray direction (12 bytes)
    float _padding1[2];                // Alignment padding (8 bytes)
    
    // === Spectrum and Throughput (64 bytes) ===
    SampledSpectrum throughput;        // Path throughput/weight (16 bytes)
    SampledSpectrum contribution;      // Accumulated radiance contribution (16 bytes)
    WavelengthSamples wls;             // Wavelength samples (24 bytes)
    float initImportance;              // Initial importance (for RR) (4 bytes)
    float selectWLPDF;                 // Wavelength selection PDF (4 bytes)
    
    // === Random Number Generator (16 bytes) ===
    KernelRNG rng;                     // RNG state (PCG32: 16 bytes)
    
    // === Path History (16 bytes) ===
    float prevDirPDF;                  // Previous bounce direction PDF (4 bytes)
    DirectionType prevSampledType;     // Previous bounce sampling type (4 bytes)
    uint32_t pathLength;               // Current path length (4 bytes)
    uint32_t _padding2;                // Alignment padding (4 bytes)
    
    // === Pixel Coordinates (8 bytes) ===
    uint32_t pixelX;                   // Pixel X coordinate (4 bytes)
    uint32_t pixelY;                   // Pixel Y coordinate (4 bytes)
    
    // === State Flags (8 bytes) ===
    uint32_t flags;                    // State flag bits (4 bytes)
    uint32_t materialCategory;         // Material category (4 bytes)
    
    // === Total Size: 144 bytes ===
    
    // Flags bit definitions:
    // bit 0: isActive - Is path active
    // bit 1: isTerminated - Is path terminated
    // bit 2: maxLengthReached - Has reached max length
    // bit 3: singleWlSelected - Has single wavelength selected
    // bit 4: hitEmissive - Has hit emissive surface
    // bit 5-7: Reserved
    
    CUDA_DEVICE_FUNCTION CUDA_INLINE bool isActive() const {
        return flags & 0x1;
    }
    
    CUDA_DEVICE_FUNCTION CUDA_INLINE bool isTerminated() const {
        return flags & 0x2;
    }
    
    CUDA_DEVICE_FUNCTION CUDA_INLINE void setActive(bool active) {
        if (active) flags |= 0x1;
        else flags &= ~0x1;
    }
    
    CUDA_DEVICE_FUNCTION CUDA_INLINE void setTerminated() {
        flags |= 0x2;
        flags &= ~0x1;
    }
    
    CUDA_DEVICE_FUNCTION CUDA_INLINE bool maxLengthReached() const {
        return flags & 0x4;
    }
    
    CUDA_DEVICE_FUNCTION CUDA_INLINE void setMaxLengthReached() {
        flags |= 0x4;
    }
    
    CUDA_DEVICE_FUNCTION CUDA_INLINE bool singleWlSelected() const {
        return flags & 0x8;
    }
    
    CUDA_DEVICE_FUNCTION CUDA_INLINE void setSingleWlSelected() {
        flags |= 0x8;
    }
    
    CUDA_DEVICE_FUNCTION CUDA_INLINE bool hitEmissive() const {
        return flags & 0x10;
    }
    
    CUDA_DEVICE_FUNCTION CUDA_INLINE void setHitEmissive() {
        flags |= 0x10;
    }
};

static_assert(sizeof(WavefrontPathState) == 144, "PathState size must be 144 bytes");


/// Hit Information: Stores ray intersection results
/// Size: 32 bytes (optimized for memory bandwidth)
struct alignas(16) WavefrontHitInfo {
    // === Hit Geometry Information (16 bytes) ===
    uint32_t instIndex;                // Instance index (4 bytes)
    uint32_t geomInstIndex;            // Geometry instance index (4 bytes)
    uint32_t primIndex;                // Primitive index (4 bytes)
    uint32_t hitFlags;                 // Hit flag bits (4 bytes)
    
    // === Parametric Coordinates (16 bytes) ===
    float u, v;                        // Barycentric or parametric coordinates (8 bytes)
    float t;                           // Ray parameter t (4 bytes)
    float _padding;                    // Alignment padding (4 bytes)
    
    // === Total Size: 32 bytes ===
    
    // HitFlags bit definitions:
    // bit 0: hasHit - Has hit any geometry
    // bit 1: hitInfinity - Has hit infinity (environment)
    // bit 2: hitEmissive - Has hit emissive surface
    // bit 3: hitTransmissive - Has hit transmissive surface
    // bit 4-7: Reserved
    
    CUDA_DEVICE_FUNCTION CUDA_INLINE bool hasHit() const {
        return hitFlags & 0x1;
    }
    
    CUDA_DEVICE_FUNCTION CUDA_INLINE bool hitInfinity() const {
        return hitFlags & 0x2;
    }
    
    CUDA_DEVICE_FUNCTION CUDA_INLINE bool hitEmissive() const {
        return hitFlags & 0x4;
    }
    
    CUDA_DEVICE_FUNCTION CUDA_INLINE bool hitTransmissive() const {
        return hitFlags & 0x8;
    }
    
    CUDA_DEVICE_FUNCTION CUDA_INLINE void setHasHit(bool hit) {
        if (hit) hitFlags |= 0x1;
        else hitFlags &= ~0x1;
    }
    
    CUDA_DEVICE_FUNCTION CUDA_INLINE void setHitInfinity(bool inf) {
        if (inf) hitFlags |= 0x2;
        else hitFlags &= ~0x2;
    }
    
    CUDA_DEVICE_FUNCTION CUDA_INLINE void setHitEmissive(bool emissive) {
        if (emissive) hitFlags |= 0x4;
        else hitFlags &= ~0x4;
    }
    
    CUDA_DEVICE_FUNCTION CUDA_INLINE void reset() {
        instIndex = 0xFFFFFFFF;
        geomInstIndex = 0xFFFFFFFF;
        primIndex = 0xFFFFFFFF;
        hitFlags = 0;
        u = v = t = 0.0f;
    }
};

static_assert(sizeof(WavefrontHitInfo) == 32, "HitInfo size must be 32 bytes");


// ============================================================================
// 2. Work Queue Management
// ============================================================================

/// Work Queue: Manages indices of active paths
/// Uses atomic operations for thread-safe enqueue/dequeue
struct WavefrontWorkQueue {
    uint32_t* pathIndices;             // Path index array (GPU memory)
    uint32_t* counter;                 // Atomic counter (GPU memory)
    uint32_t capacity;                 // Queue capacity
    
    CUDA_DEVICE_FUNCTION CUDA_INLINE uint32_t size() const {
        return *counter;
    }
    
    CUDA_DEVICE_FUNCTION CUDA_INLINE uint32_t enqueue(uint32_t pathIndex) {
        uint32_t slot = atomicAdd(counter, 1);
        if (slot < capacity) {
            pathIndices[slot] = pathIndex;
            return slot;
        }
        return 0xFFFFFFFF; // Queue full
    }
    
    CUDA_DEVICE_FUNCTION CUDA_INLINE uint32_t dequeue() {
        uint32_t slot = atomicSub(counter, 1);
        if (slot > 0 && slot <= capacity) {
            return pathIndices[slot - 1];
        }
        return 0xFFFFFFFF; // Queue empty
    }
    
    CUDA_DEVICE_FUNCTION CUDA_INLINE void reset() {
        *counter = 0;
    }
};


/// Material Classification: Used for path sorting and grouping
enum MaterialCategory : uint32_t {
    MaterialCategory_Diffuse = 0,      // Diffuse materials (Lambert)
    MaterialCategory_Glossy,           // Glossy reflection materials (GGX)
    MaterialCategory_Specular,         // Perfect specular reflection
    MaterialCategory_Transmissive,     // Transmissive materials (glass, etc.)
    MaterialCategory_Emissive,         // Emissive materials
    MaterialCategory_Mixed,            // Mixed materials
    NumMaterialCategories
};


/// Material Queue Set: Work queues classified by material type
struct WavefrontMaterialQueues {
    WavefrontWorkQueue queues[NumMaterialCategories];
    
    CUDA_DEVICE_FUNCTION CUDA_INLINE void enqueueByCategory(
        uint32_t pathIndex, MaterialCategory category) {
        queues[category].enqueue(pathIndex);
    }
    
    CUDA_DEVICE_FUNCTION CUDA_INLINE void resetAll() {
        for (int i = 0; i < NumMaterialCategories; ++i) {
            queues[i].reset();
        }
    }
};


// ============================================================================
// 3. Launch Parameters
// ============================================================================

/// Wavefront rendering launch parameters
/// This structure is uploaded to GPU constant memory
struct WavefrontLaunchParameters {
    // === Common data inherited from PipelineLaunchParameters ===
    // Note: In actual implementation, may need to inherit or include PipelineLaunchParameters
    
    // Spectrum upsampling data
    DiscretizedSpectrumAlwaysSpectral::CMF DiscretizedSpectrum_xbar;
    DiscretizedSpectrumAlwaysSpectral::CMF DiscretizedSpectrum_ybar;
    DiscretizedSpectrumAlwaysSpectral::CMF DiscretizedSpectrum_zbar;
    float DiscretizedSpectrum_integralCMF;
    
    // Material and node data
    const NodeProcedureSet* nodeProcedureSetBuffer;
    const SmallNodeDescriptor* smallNodeDescriptorBuffer;
    const MediumNodeDescriptor* mediumNodeDescriptorBuffer;
    const LargeNodeDescriptor* largeNodeDescriptorBuffer;
    const BSDFProcedureSet* bsdfProcedureSetBuffer;
    const EDFProcedureSet* edfProcedureSetBuffer;
    const IDFProcedureSet* idfProcedureSetBuffer;
    const SurfaceMaterialDescriptor* materialDescriptorBuffer;
    
    // Scene data
    const GeometryInstance* geomInstBuffer;
    const Instance* instBuffer;
    uint64_t topGroup;  // OptixTraversableHandle (defined when OptiX is available)
    const SceneBounds* sceneBounds;
    const uint32_t* instIndices;
    DiscreteDistribution1D lightInstDist;
    uint32_t envLightInstIndex;
    
    // Camera data
    int32_t progSampleLensPosition;
    int32_t progTestLensIntersection;
    CameraDescriptor cameraDescriptor;
    
    // === Wavefront Specific Data ===
    
    // Path state buffers
    WavefrontPathState* pathStateBuffer;
    WavefrontHitInfo* hitInfoBuffer;
    SurfacePoint* surfacePointBuffer;
    
    // Work queues
    WavefrontWorkQueue activePathQueue;      // Current active path queue
    WavefrontWorkQueue nextActivePathQueue;  // Next round active path queue
    
    // Material classification queues (optional optimization)
    WavefrontMaterialQueues materialQueues;
    
    // Output buffers
    optixu::NativeBlockBuffer2D<KernelRNG> rngBuffer;
    optixu::BlockBuffer2D<SpectrumStorage, 0> accumBuffer;
    DiscretizedSpectrum* accumAlbedoBuffer;  // Denoiser Albedo
    Normal3D* accumNormalBuffer;             // Denoiser Normal
    
    // Image parameters
    uint2 imageSize;                   // Image dimensions
    uint32_t imageStrideInPixels;      // Image stride
    uint32_t numAccumFrames;           // Accumulated frame count
    uint32_t limitNumAccumFrames;      // Maximum accumulated frames
    
    // Wavefront configuration
    uint32_t maxPathLength;            // Maximum path length (default 25)
    uint32_t maxNumPaths;              // Maximum number of paths
    uint32_t currentDepth;             // Current path depth being processed
    
    // Performance statistics (optional)
    uint32_t* numActiveRays;           // Current active ray count
    uint32_t* numShadowRays;           // Shadow ray count
    uint32_t* numTerminatedPaths;      // Terminated path count
    
    // Debug parameters
    int32_t probePixX;                 // Probe pixel X
    int32_t probePixY;                 // Probe pixel Y
    uint32_t debugMode;                // Debug mode
    
    CUDA_DEVICE_FUNCTION CUDA_INLINE void print() const {
        printf("=== Wavefront Launch Parameters ===\n");
        printf("Image Size: %ux%u\n", imageSize.x, imageSize.y);
        printf("Max Path Length: %u\n", maxPathLength);
        printf("Max Num Paths: %u\n", maxNumPaths);
        printf("Current Depth: %u\n", currentDepth);
        printf("Num Accum Frames: %u\n", numAccumFrames);
        printf("Active Path Queue Size: %u\n", activePathQueue.size());
    }
};


// ============================================================================
// 4. Payload Definitions
// ============================================================================

/// Wavefront ray tracing payload
/// Design principle: Minimize payload size, only pass necessary information
struct WFTracePayload {
    uint32_t pathIndex;                // Path index (4 bytes)
    WavelengthSamples wls;             // Wavelength samples (24 bytes)
    // Total: 28 bytes (7 dwords)
};

using WFTracePayloadSignature = optixu::PayloadSignature<WFTracePayload>;


/// Shadow ray payload (reuse existing ShadowPayloadSignature)
// using ShadowPayloadSignature = optixu::PayloadSignature<WavelengthSamples, float>;


// ============================================================================
// 5. Ray Types
// ============================================================================

enum WFRayType {
    WFRayType_Closest = 0,             // Closest hit ray
    WFRayType_Shadow,                  // Shadow ray
    NumWFRayTypes
};


// ============================================================================
// 6. Auxiliary Data Structures
// ============================================================================

/// Material Evaluation Result: Caches BSDF/EDF evaluation results
struct WavefrontMaterialEvaluation {
    SampledSpectrum baseColor;         // Base color
    DirectionType bsdfType;            // BSDF type
    bool hasNonDelta;                  // Has non-delta component
    bool hasEmission;                  // Has emission
    MaterialCategory category;         // Material category
};


/// Light Sample Result: Caches light sampling information
struct WavefrontLightSample {
    SurfacePoint lightSurfPt;          // Light surface point
    SampledSpectrum Le;                // Light emission
    float lightPDF;                    // Light sampling PDF
    float bsdfPDF;                     // BSDF PDF
    float MISWeight;                   // MIS weight
    bool isVisible;                    // Is visible
};


/// BSDF Sample Result: Caches BSDF sampling information
struct WavefrontBSDFSample {
    Vector3D dirLocal;                 // Sampled direction (local coordinates)
    SampledSpectrum f;                 // BSDF value
    float pdf;                         // PDF
    DirectionType sampledType;         // Sampled type
    bool isValid;                      // Is sample valid
};


// ============================================================================
// 7. Performance Statistics
// ============================================================================

/// Wavefront performance statistics
struct WavefrontPerformanceStats {
    // Path statistics
    uint32_t numInitialPaths;
    uint32_t numActivePathsPerDepth[32];  // Active paths per depth
    uint32_t numTerminatedPathsPerDepth[32];
    
    // Ray statistics
    uint32_t numPrimaryRays;
    uint32_t numSecondaryRays;
    uint32_t numShadowRays;
    uint32_t totalRaysCast;
    
    // Material statistics
    uint32_t numDiffuseInteractions;
    uint32_t numGlossyInteractions;
    uint32_t numSpecularInteractions;
    uint32_t numTransmissiveInteractions;
    
    // Timing statistics (milliseconds)
    float timeGenerateRays;
    float timeTraceRays;
    float timeProcessHits;
    float timeSampleLights;
    float timeSampleBSDF;
    float timeCompactPaths;
    float timeAccumulate;
    float totalTime;
    
    // Memory statistics
    size_t memoryUsedBytes;
    size_t peakMemoryUsedBytes;
    
    void reset() {
        memset(this, 0, sizeof(WavefrontPerformanceStats));
    }
    
    void print() const {
        printf("=== Wavefront Performance Stats ===\n");
        printf("Initial Paths: %u\n", numInitialPaths);
        printf("Total Rays Cast: %u\n", totalRaysCast);
        printf("  Primary: %u\n", numPrimaryRays);
        printf("  Secondary: %u\n", numSecondaryRays);
        printf("  Shadow: %u\n", numShadowRays);
        printf("\nMaterial Interactions:\n");
        printf("  Diffuse: %u\n", numDiffuseInteractions);
        printf("  Glossy: %u\n", numGlossyInteractions);
        printf("  Specular: %u\n", numSpecularInteractions);
        printf("  Transmissive: %u\n", numTransmissiveInteractions);
        printf("\nTiming (ms):\n");
        printf("  Generate Rays: %.2f\n", timeGenerateRays);
        printf("  Trace Rays: %.2f\n", timeTraceRays);
        printf("  Process Hits: %.2f\n", timeProcessHits);
        printf("  Sample Lights: %.2f\n", timeSampleLights);
        printf("  Sample BSDF: %.2f\n", timeSampleBSDF);
        printf("  Compact Paths: %.2f\n", timeCompactPaths);
        printf("  Accumulate: %.2f\n", timeAccumulate);
        printf("  Total: %.2f\n", totalTime);
        printf("\nMemory:\n");
        printf("  Used: %.2f MB\n", memoryUsedBytes / (1024.0f * 1024.0f));
        printf("  Peak: %.2f MB\n", peakMemoryUsedBytes / (1024.0f * 1024.0f));
    }
};


// ============================================================================
// 8. Debug Data Structures
// ============================================================================

/// Wavefront debug modes
enum WavefrontDebugMode : uint32_t {
    WFDebug_None = 0,
    WFDebug_PathLength,                // Visualize path length
    WFDebug_MaterialCategory,          // Visualize material classification
    WFDebug_Throughput,                // Visualize path throughput
    WFDebug_NumBounces,                // Visualize bounce count
    WFDebug_ActivePaths,               // Visualize active path distribution
    WFDebug_RayDensity,                // Visualize ray density
    WFDebug_TerminationReason,         // Visualize termination reason
};


/// Path termination reasons
enum PathTerminationReason : uint32_t {
    TerminationReason_None = 0,
    TerminationReason_MaxLength,       // Reached maximum length
    TerminationReason_RussianRoulette, // Russian roulette
    TerminationReason_ZeroThroughput,  // Zero throughput
    TerminationReason_Absorption,      // Absorbed
    TerminationReason_EscapeScene,     // Escaped scene
};


// ============================================================================
// 9. Configuration Constants
// ============================================================================

namespace WavefrontConfig {
    // Path configuration
    constexpr uint32_t DefaultMaxPathLength = 25;
    constexpr uint32_t MinPathLength = 1;
    constexpr uint32_t MaxPathLength = 64;
    
    // Russian roulette configuration
    constexpr uint32_t RRStartDepth = 3;      // Depth to start using RR
    constexpr float RRThreshold = 0.05f;      // RR threshold
    
    // Queue configuration
    constexpr uint32_t DefaultQueueCapacity = 1920 * 1080;
    constexpr uint32_t MaxQueueCapacity = 3840 * 2160;
    
    // Performance configuration
    constexpr uint32_t BlockSize = 256;       // CUDA block size
    constexpr uint32_t WarpSize = 32;         // Warp size
    
    // Memory configuration
    constexpr bool UsePathSorting = true;     // Use path sorting
    constexpr bool UseMaterialQueues = true;  // Use material queues
    constexpr bool UseStreamCompaction = true; // Use stream compaction
    
    // Debug configuration
    constexpr bool EnablePerfStats = true;    // Enable performance statistics
    constexpr bool EnableValidation = false;  // Enable validation checks
}


// ============================================================================
// 10. Memory Layout Optimization (Optional SoA Version)
// ============================================================================

/// Structure of Arrays version of PathState (for memory optimization)
struct WavefrontPathStateBuffers_SoA {
    // Ray information
    Point3D* origins;
    Vector3D* directions;
    
    // Spectrum information
    SampledSpectrum* throughputs;
    SampledSpectrum* contributions;
    WavelengthSamples* wavelengthSamples;
    float* initImportances;
    
    // RNG
    KernelRNG* rngs;
    
    // Path history
    float* prevDirPDFs;
    DirectionType* prevSampledTypes;
    uint32_t* pathLengths;
    
    // Pixel coordinates
    uint32_t* pixelXs;
    uint32_t* pixelYs;
    
    // Flags
    uint32_t* flags;
    uint32_t* materialCategories;
    
    // Access interface
    CUDA_DEVICE_FUNCTION CUDA_INLINE void load(
        uint32_t index, WavefrontPathState* state) const {
        state->origin = origins[index];
        state->direction = directions[index];
        state->throughput = throughputs[index];
        state->contribution = contributions[index];
        state->wls = wavelengthSamples[index];
        state->initImportance = initImportances[index];
        state->rng = rngs[index];
        state->prevDirPDF = prevDirPDFs[index];
        state->prevSampledType = prevSampledTypes[index];
        state->pathLength = pathLengths[index];
        state->pixelX = pixelXs[index];
        state->pixelY = pixelYs[index];
        state->flags = flags[index];
        state->materialCategory = materialCategories[index];
    }
    
    CUDA_DEVICE_FUNCTION CUDA_INLINE void store(
        uint32_t index, const WavefrontPathState& state) {
        origins[index] = state.origin;
        directions[index] = state.direction;
        throughputs[index] = state.throughput;
        contributions[index] = state.contribution;
        wavelengthSamples[index] = state.wls;
        initImportances[index] = state.initImportance;
        rngs[index] = state.rng;
        prevDirPDFs[index] = state.prevDirPDF;
        prevSampledTypes[index] = state.prevSampledType;
        pathLengths[index] = state.pathLength;
        pixelXs[index] = state.pixelX;
        pixelYs[index] = state.pixelY;
        flags[index] = state.flags;
        materialCategories[index] = state.materialCategory;
    }
};


// ============================================================================
// 11. Stream Compaction Helpers
// ============================================================================

/// Path activity predicate (for stream compaction)
struct PathIsActivePredicate {
    WavefrontPathState* pathStates;
    
    CUDA_DEVICE_FUNCTION CUDA_INLINE bool operator()(uint32_t pathIndex) const {
        return pathStates[pathIndex].isActive();
    }
};


/// Material category comparator (for path sorting)
struct MaterialCategoryComparator {
    WavefrontPathState* pathStates;
    
    CUDA_DEVICE_FUNCTION CUDA_INLINE bool operator()(
        uint32_t pathIndex1, uint32_t pathIndex2) const {
        return pathStates[pathIndex1].materialCategory < 
               pathStates[pathIndex2].materialCategory;
    }
};


// ============================================================================
// 12. Version Information
// ============================================================================

namespace WavefrontVersion {
    constexpr uint32_t Major = 1;
    constexpr uint32_t Minor = 0;
    constexpr uint32_t Patch = 0;
    constexpr const char* String = "1.0.0-alpha";
}

} // namespace shared
} // namespace vlr
