// VLR MultiSurface Material Test
// Tests vlrCreateMaterialMultiSurface for 2, 3, and 4 layer blends

#include <vlr/vlr.h>
#include <cstdio>
#include <cstring>

// BSDFType from material_types.h: 0=Lambert, 2=GGX, 5=Specular, 12=UE4BRDF
static constexpr uint32_t BSDF_Lambert = 0;
static constexpr uint32_t BSDF_GGX = 2;
static constexpr uint32_t BSDF_Specular = 5;
static constexpr uint32_t BSDF_UE4BRDF = 12;

int main() {
    printf("=== VLR MultiSurface Material Test ===\n\n");

    VLRContext context = nullptr;
    VLRScene scene = nullptr;
    VLRResult res;
    int passed = 0;
    int failed = 0;

    res = vlrCreateContext(nullptr, 0, &context);
    if (res != VLRResult_Success || !context) {
        fprintf(stderr, "[FAIL] Context creation\n");
        return 1;
    }
    printf("[PASS] Context created\n");

    res = vlrCreateScene(context, &scene);
    if (res != VLRResult_Success || !scene) {
        fprintf(stderr, "[FAIL] Scene creation\n");
        vlrDestroyContext(context);
        return 1;
    }
    printf("[PASS] Scene created\n\n");

    printf("Testing vlrCreateMaterialMultiSurface:\n");
    printf("--------------------------------------\n");

    // Test 1: 2-layer blend - 50% Lambert + 50% GGX
    {
        VLRMaterial mat = nullptr;
        uint32_t subTypes[] = { BSDF_Lambert, BSDF_GGX };
        float albedo0[] = { 0.7f, 0.7f, 0.7f };
        float albedo1[] = { 0.9f, 0.9f, 0.9f };
        const float* subAlbedos[] = { albedo0, albedo1 };
        float subRoughness[] = { 0.5f, 0.3f };
        float weights[] = { 0.5f, 0.5f };

        res = vlrCreateMaterialMultiSurface(scene, 2, subTypes, subAlbedos,
            subRoughness, weights, &mat);
        if (res == VLRResult_Success && mat) {
            printf("[1/3] 2-layer (50%% Lambert + 50%% GGX) - PASS\n");
            passed++;
            vlrDestroyMaterial(mat);
        } else {
            printf("[1/3] 2-layer (50%% Lambert + 50%% GGX) - FAIL\n");
            failed++;
        }
    }

    // Test 2: 3-layer blend - 30% Lambert + 40% rough metal (GGX) + 30% specular
    {
        VLRMaterial mat = nullptr;
        uint32_t subTypes[] = { BSDF_Lambert, BSDF_GGX, BSDF_Specular };
        float alb0[] = { 0.6f, 0.6f, 0.6f };
        float alb1[] = { 0.8f, 0.6f, 0.2f };  // rough metal
        float alb2[] = { 0.95f, 0.95f, 0.95f };  // specular
        const float* subAlbedos[] = { alb0, alb1, alb2 };
        float subRoughness[] = { 0.8f, 0.25f, 0.001f };
        float weights[] = { 0.3f, 0.4f, 0.3f };

        res = vlrCreateMaterialMultiSurface(scene, 3, subTypes, subAlbedos,
            subRoughness, weights, &mat);
        if (res == VLRResult_Success && mat) {
            printf("[2/3] 3-layer (30%% Lambert + 40%% GGX + 30%% Specular) - PASS\n");
            passed++;
            vlrDestroyMaterial(mat);
        } else {
            printf("[2/3] 3-layer - FAIL\n");
            failed++;
        }
    }

    // Test 3: 4-layer blend - complex material
    {
        VLRMaterial mat = nullptr;
        uint32_t subTypes[] = { BSDF_Lambert, BSDF_GGX, BSDF_Specular, BSDF_UE4BRDF };
        float a0[] = { 0.5f, 0.5f, 0.5f };
        float a1[] = { 0.7f, 0.5f, 0.3f };
        float a2[] = { 0.9f, 0.9f, 0.9f };
        float a3[] = { 0.4f, 0.6f, 0.8f };
        const float* subAlbedos[] = { a0, a1, a2, a3 };
        float subRoughness[] = { 0.9f, 0.2f, 0.001f, 0.4f };
        float weights[] = { 0.25f, 0.25f, 0.25f, 0.25f };

        res = vlrCreateMaterialMultiSurface(scene, 4, subTypes, subAlbedos,
            subRoughness, weights, &mat);
        if (res == VLRResult_Success && mat) {
            printf("[3/3] 4-layer (Lambert + GGX + Specular + UE4BRDF) - PASS\n");
            passed++;
            vlrDestroyMaterial(mat);
        } else {
            printf("[3/3] 4-layer - FAIL\n");
            failed++;
        }
    }

    // Test 4: Invalid arguments
    {
        VLRMaterial mat = nullptr;
        uint32_t subTypes[] = { BSDF_Lambert, BSDF_GGX };
        float albedo0[] = { 0.5f, 0.5f, 0.5f };
        float albedo1[] = { 0.5f, 0.5f, 0.5f };
        const float* subAlbedos[] = { albedo0, albedo1 };
        float subRoughness[] = { 0.5f, 0.5f };
        float weights[] = { 0.5f, 0.5f };

        res = vlrCreateMaterialMultiSurface(scene, 1, subTypes, subAlbedos,
            subRoughness, weights, &mat);
        if (res == VLRResult_InvalidArgument && !mat) {
            printf("[4/4] Invalid numLayers=1 rejected - PASS\n");
            passed++;
        } else {
            printf("[4/4] Invalid numLayers=1 rejected - FAIL\n");
            failed++;
        }
    }

    printf("\n--------------------------------------\n");
    printf("Results: %d passed, %d failed\n", passed, failed);

    vlrDestroyScene(scene);
    vlrDestroyContext(context);

    return (failed > 0) ? 1 : 0;
}
