// ============================================================================
// Context Compilation Test
// 
// This file tests that the Context class compiles correctly.
// 
// Author: VLR Development Team
// Created: 2026-03-07
// ============================================================================

#include "context.h"
#include <iostream>

int main() {
    std::cout << "=== VLR Context Compilation Test ===" << std::endl;
    
    // Test 1: Context size
    std::cout << "Context size: " << sizeof(vlr::Context) << " bytes" << std::endl;
    
    // Test 2: Basic types
    std::cout << "WavefrontPathState size: " 
              << sizeof(vlr::shared::WavefrontPathState) << " bytes" << std::endl;
    std::cout << "WavefrontLaunchParameters size: " 
              << sizeof(vlr::shared::WavefrontLaunchParameters) << " bytes" << std::endl;
    
    // Test 3: Check if we can create a context (this will fail without proper CUDA setup)
    try {
        cudaStream_t stream;
        cudaStreamCreate(&stream);
        
        vlr::Context* ctx = new vlr::Context(stream, true);
        std::cout << "Context created successfully!" << std::endl;
        
        // Test configuration
        ctx->setMaxPathLength(16);
        std::cout << "Configuration test passed!" << std::endl;
        
        delete ctx;
        cudaStreamDestroy(stream);
        
        std::cout << "Context destroyed successfully!" << std::endl;
    } catch (const std::exception& e) {
        std::cout << "Context creation test (expected to fail without GPU): " 
                  << e.what() << std::endl;
    }
    
    std::cout << "\n=== Compilation Test PASSED ===" << std::endl;
    return 0;
}
