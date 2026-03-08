// VLR Material API Test
// Simple test to verify existing material creation functions

#include <vlr/vlr.h>
#include <cstdio>

int main() {
    printf("=== VLR Material API Test ===\n\n");
    
    VLRContext context = nullptr;
    VLRScene scene = nullptr;
    VLRResult res;
    int passed = 0;
    int failed = 0;
    
    // Create context
    res = vlrCreateContext(nullptr, 0, &context);
    if (res != VLRResult_Success || !context) {
        fprintf(stderr, "[FAIL] Context creation\n");
        return 1;
    }
    printf("[PASS] Context created\n");
    
    // Create scene
    res = vlrCreateScene(context, &scene);
    if (res != VLRResult_Success || !scene) {
        fprintf(stderr, "[FAIL] Scene creation\n");
        vlrDestroyContext(context);
        return 1;
    }
    printf("[PASS] Scene created\n\n");
    
    printf("Testing Material Creation APIs:\n");
    printf("--------------------------------\n");
    
    // Test 1: Lambert (vlrCreateMaterial)
    {
        VLRMaterial mat = nullptr;
        float color[] = {0.8f, 0.8f, 0.8f};
        res = vlrCreateMaterial(scene, 0, color, nullptr, &mat);
        if (res == VLRResult_Success && mat) {
            printf("[1/7] vlrCreateMaterial (Lambert) - PASS\n");
            passed++;
        } else {
            printf("[1/7] vlrCreateMaterial (Lambert) - FAIL\n");
            failed++;
        }
    }
    
    // Test 2: GGX/Specular with roughness (vlrCreateMaterialEx)
    {
        VLRMaterial mat = nullptr;
        float color[] = {0.8f, 0.8f, 0.8f};
        res = vlrCreateMaterialEx(scene, 3, color, 0.2f, 0.0f, 1.5f, nullptr, &mat);
        if (res == VLRResult_Success && mat) {
            printf("[2/7] vlrCreateMaterialEx (Specular) - PASS\n");
            passed++;
        } else {
            printf("[2/7] vlrCreateMaterialEx (Specular) - FAIL\n");
            failed++;
        }
    }
    
    // Test 3: Perfect glass (vlrCreateMaterialEx with type 6)
    {
        VLRMaterial mat = nullptr;
        float color[] = {0.999f, 0.999f, 0.999f};
        res = vlrCreateMaterialEx(scene, 6, color, 0.0f, 0.0f, 1.5f, nullptr, &mat);
        if (res == VLRResult_Success && mat) {
            printf("[3/7] vlrCreateMaterialEx (Glass) - PASS\n");
            passed++;
        } else {
            printf("[3/7] vlrCreateMaterialEx (Glass) - FAIL\n");
            failed++;
        }
    }
    
    // Test 4: Conductor/Metal (vlrCreateMaterialConductor)
    {
        VLRMaterial mat = nullptr;
        float eta[] = {0.143f, 0.374f, 1.442f};  // Gold
        float kappa[] = {3.984f, 2.386f, 1.603f};
        res = vlrCreateMaterialConductor(scene, eta, kappa, 0.15f, &mat);
        if (res == VLRResult_Success && mat) {
            printf("[4/7] vlrCreateMaterialConductor (Gold) - PASS\n");
            passed++;
        } else {
            printf("[4/7] vlrCreateMaterialConductor (Gold) - FAIL\n");
            failed++;
        }
    }
    
    // Test 5: Rough glass (vlrCreateMaterialMicrofacetScattering)
    {
        VLRMaterial mat = nullptr;
        res = vlrCreateMaterialMicrofacetScattering(scene, 1.5f, 0.1f, &mat);
        if (res == VLRResult_Success && mat) {
            printf("[5/7] vlrCreateMaterialMicrofacetScattering - PASS\n");
            passed++;
        } else {
            printf("[5/7] vlrCreateMaterialMicrofacetScattering - FAIL\n");
            failed++;
        }
    }
    
    // Test 6: Checkerboard (vlrCreateMaterialCheckerboard)
    {
        VLRMaterial mat = nullptr;
        float black[] = {0.05f, 0.05f, 0.05f};
        float white[] = {0.8f, 0.8f, 0.8f};
        res = vlrCreateMaterialCheckerboard(scene, black, white, 20, 2.0f, &mat);
        if (res == VLRResult_Success && mat) {
            printf("[6/7] vlrCreateMaterialCheckerboard - PASS\n");
            passed++;
        } else {
            printf("[6/7] vlrCreateMaterialCheckerboard - FAIL\n");
            failed++;
        }
    }

    // Test 7: LambertianScattering (vlrCreateMaterialLambertianScattering)
    {
        VLRMaterial mat = nullptr;
        float sssColor[] = {0.9f, 0.4f, 0.4f};
        res = vlrCreateMaterialLambertianScattering(scene, sssColor, &mat);
        if (res == VLRResult_Success && mat) {
            printf("[7/7] vlrCreateMaterialLambertianScattering - PASS\n");
            passed++;
        } else {
            printf("[7/7] vlrCreateMaterialLambertianScattering - FAIL\n");
            failed++;
        }
    }
    
    // Cleanup
    vlrDestroyScene(scene);
    vlrDestroyContext(context);
    
    // Summary
    printf("\n================================\n");
    printf("Test Summary:\n");
    printf("  PASSED: %d/7\n", passed);
    printf("  FAILED: %d/7\n", failed);
    printf("================================\n");
    
    return (failed > 0) ? 1 : 0;
}
