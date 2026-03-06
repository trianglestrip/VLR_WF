// ============================================================================
// Wavefront Types Test
// 
// This file tests the compilation and size of Wavefront data structures.
// 
// Compile with: nvcc -std=c++17 -arch=sm_75 wavefront_types_test.cu -o test.exe
// ============================================================================

#include "wavefront_types.h"
#include <stdio.h>

using namespace vlr::shared;

// Test kernel to verify device-side compilation
__global__ void testKernel() {
    WavefrontPathState pathState;
    pathState.setActive(true);
    pathState.pathLength = 0;
    
    WavefrontHitInfo hitInfo;
    hitInfo.reset();
}

int main() {
    printf("=== VLR Wavefront Data Structure Size Verification ===\n\n");
    
    // Test WavefrontPathState
    printf("WavefrontPathState:\n");
    printf("  Size: %zu bytes (Expected: 144 bytes)\n", sizeof(WavefrontPathState));
    printf("  Alignment: %zu bytes\n", alignof(WavefrontPathState));
    
    if (sizeof(WavefrontPathState) == 144) {
        printf("  ✓ Size is correct!\n");
    } else {
        printf("  ✗ Size mismatch!\n");
    }
    printf("\n");
    
    // Test WavefrontHitInfo
    printf("WavefrontHitInfo:\n");
    printf("  Size: %zu bytes (Expected: 32 bytes)\n", sizeof(WavefrontHitInfo));
    printf("  Alignment: %zu bytes\n", alignof(WavefrontHitInfo));
    
    if (sizeof(WavefrontHitInfo) == 32) {
        printf("  ✓ Size is correct!\n");
    } else {
        printf("  ✗ Size mismatch!\n");
    }
    printf("\n");
    
    // Test WavefrontWorkQueue
    printf("WavefrontWorkQueue:\n");
    printf("  Size: %zu bytes\n", sizeof(WavefrontWorkQueue));
    printf("  Alignment: %zu bytes\n", alignof(WavefrontWorkQueue));
    printf("\n");
    
    // Test WavefrontMaterialQueues
    printf("WavefrontMaterialQueues:\n");
    printf("  Size: %zu bytes\n", sizeof(WavefrontMaterialQueues));
    printf("  Number of categories: %d\n", NumMaterialCategories);
    printf("\n");
    
    // Test WavefrontLaunchParameters
    printf("WavefrontLaunchParameters:\n");
    printf("  Size: %zu bytes\n", sizeof(WavefrontLaunchParameters));
    printf("  Alignment: %zu bytes\n", alignof(WavefrontLaunchParameters));
    printf("\n");
    
    // Test WFTracePayload
    printf("WFTracePayload:\n");
    printf("  Size: %zu bytes (Expected: 28 bytes)\n", sizeof(WFTracePayload));
    printf("  Alignment: %zu bytes\n", alignof(WFTracePayload));
    printf("\n");
    
    // Test auxiliary structures
    printf("WavefrontMaterialEvaluation:\n");
    printf("  Size: %zu bytes\n", sizeof(WavefrontMaterialEvaluation));
    printf("\n");
    
    printf("WavefrontLightSample:\n");
    printf("  Size: %zu bytes\n", sizeof(WavefrontLightSample));
    printf("\n");
    
    printf("WavefrontBSDFSample:\n");
    printf("  Size: %zu bytes\n", sizeof(WavefrontBSDFSample));
    printf("\n");
    
    printf("WavefrontPerformanceStats:\n");
    printf("  Size: %zu bytes\n", sizeof(WavefrontPerformanceStats));
    printf("\n");
    
    // Test basic types
    printf("=== Basic Types ===\n");
    printf("KernelRNG: %zu bytes (Expected: 16 bytes)\n", sizeof(KernelRNG));
    printf("WavelengthSamples: %zu bytes (Expected: 24 bytes)\n", sizeof(WavelengthSamples));
    printf("SampledSpectrum: %zu bytes\n", sizeof(SampledSpectrum));
    printf("Vector3D: %zu bytes\n", sizeof(Vector3D));
    printf("ReferenceFrame: %zu bytes\n", sizeof(ReferenceFrame));
    printf("\n");
    
    // Test enums
    printf("=== Enumerations ===\n");
    printf("MaterialCategory: %d categories\n", NumMaterialCategories);
    printf("WFRayType: %d types\n", NumWFRayTypes);
    printf("\n");
    
    // Test configuration constants
    printf("=== Configuration ===\n");
    printf("Default Max Path Length: %u\n", WavefrontConfig::DefaultMaxPathLength);
    printf("RR Start Depth: %u\n", WavefrontConfig::RRStartDepth);
    printf("RR Threshold: %.3f\n", WavefrontConfig::RRThreshold);
    printf("Block Size: %u\n", WavefrontConfig::BlockSize);
    printf("Use Path Sorting: %s\n", WavefrontConfig::UsePathSorting ? "Yes" : "No");
    printf("Use Material Queues: %s\n", WavefrontConfig::UseMaterialQueues ? "Yes" : "No");
    printf("Use Stream Compaction: %s\n", WavefrontConfig::UseStreamCompaction ? "Yes" : "No");
    printf("\n");
    
    // Test version
    printf("=== Version ===\n");
    printf("Wavefront Version: %s\n", WavefrontVersion::String);
    printf("Version: %u.%u.%u\n", 
        WavefrontVersion::Major, 
        WavefrontVersion::Minor, 
        WavefrontVersion::Patch);
    printf("\n");
    
    // Launch test kernel
    printf("=== Device Compilation Test ===\n");
    testKernel<<<1, 1>>>();
    cudaError_t err = cudaDeviceSynchronize();
    if (err == cudaSuccess) {
        printf("✓ Device kernel compiled and executed successfully!\n");
    } else {
        printf("✗ Device kernel failed: %s\n", cudaGetErrorString(err));
    }
    printf("\n");
    
    printf("=== All Tests Completed ===\n");
    
    return 0;
}
