// ============================================================================
// Kitchen Scene Rendering Test
//
// Loads the Kitchen scene from a PBRT v4 file (.pbrt + PLY meshes)
// Camera position, FOV, and resolution are read from the PBRT file.
// ============================================================================

#include "SceneLoader.h"
#include "VLRRenderer.h"
#include <iostream>
#include <chrono>
#include <filesystem>
#include <string>
#include <cmath>
#include <array>

namespace fs = std::filesystem;
using viewer::VLRRenderer;
using viewer::SceneLoader;
using viewer::PbrtCamera;
using viewer::PbrtFilm;
using viewer::Mat4f;

struct CameraVectors {
    float position[3];
    float target[3];
    float up[3];
};

static CameraVectors cameraFromPbrtTransform(const Mat4f& m) {
    // m is row-major world-to-camera. Transpose the 3x3 block to get R^T.
    // Row-major layout: m[row*4+col]
    float r00 = m[0], r01 = m[1], r02 = m[2];
    float r10 = m[4], r11 = m[5], r12 = m[6];
    float r20 = m[8], r21 = m[9], r22 = m[10];
    float tx  = m[3], ty  = m[7], tz  = m[11];

    // Inverted rotation (transpose)
    float ir00 = r00, ir01 = r10, ir02 = r20;
    float ir10 = r01, ir11 = r11, ir12 = r21;
    float ir20 = r02, ir21 = r12, ir22 = r22;

    // Camera position = -R^T * t
    float px = -(ir00*tx + ir01*ty + ir02*tz);
    float py = -(ir10*tx + ir11*ty + ir12*tz);
    float pz = -(ir20*tx + ir21*ty + ir22*tz);

    // Forward direction in world space = R^T * (0,0,-1) = third column of R^T negated
    float fwd_x = -ir02;
    float fwd_y = -ir12;
    float fwd_z = -ir22;

    // Up direction in world space = R^T * (0,1,0) = second column of R^T
    float up_x = ir01;
    float up_y = ir11;
    float up_z = ir21;

    CameraVectors cam{};
    cam.position[0] = px;  cam.position[1] = py;  cam.position[2] = pz;
    cam.target[0] = px + fwd_x;
    cam.target[1] = py + fwd_y;
    cam.target[2] = pz + fwd_z;
    cam.up[0] = up_x;  cam.up[1] = up_y;  cam.up[2] = up_z;
    return cam;
}

// ---------------------------------------------------------------------------
void printUsage(const char* progName) {
    std::cout << "Usage: " << progName << " [options]\n\n"
              << "Options:\n"
              << "  --pbrt FILE       PBRT scene file (default: models/kitchen/scene-v4.pbrt)\n"
              << "  --serial          Use serial loading (default: parallel)\n"
              << "  --threads N       Number of threads (default: auto)\n"
              << "  --output FILE     Output filename (default: kitchen_render.png)\n"
              << "  --samples N       Override sample count from PBRT file\n"
              << "  --help            Show this help\n";
}

struct TestConfig {
    std::string pbrtFile   = "models/kitchen/scene-v4.pbrt";
    bool enableParallel    = true;
    int  numThreads        = 0;
    std::string outputFile = "kitchen_render.png";
    int  samplesOverride   = 0;
};

TestConfig parseArgs(int argc, char* argv[]) {
    TestConfig config;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--pbrt" && i + 1 < argc) {
            config.pbrtFile = argv[++i];
        } else if (arg == "--serial") {
            config.enableParallel = false;
        } else if (arg == "--threads" && i + 1 < argc) {
            config.numThreads = std::stoi(argv[++i]);
        } else if (arg == "--output" && i + 1 < argc) {
            config.outputFile = argv[++i];
        } else if (arg == "--samples" && i + 1 < argc) {
            config.samplesOverride = std::stoi(argv[++i]);
        } else if (arg == "--help") {
            printUsage(argv[0]);
            std::exit(0);
        }
    }
    return config;
}

int main(int argc, char* argv[]) {
    std::cout << "========================================\n"
              << "    Kitchen Scene Rendering Test\n"
              << "========================================\n"
              << "PBRT v4 + Taskflow + VLR Wavefront\n\n";

    TestConfig config = parseArgs(argc, argv);

    if (!fs::exists(config.pbrtFile)) {
        std::cerr << "Error: PBRT file not found: " << config.pbrtFile << "\n";
        return 1;
    }

    // ------------------------------------------------------------------
    // Phase 1: Initialize VLR renderer (use default resolution; will be
    //          refined after PBRT parsing returns film settings)
    // ------------------------------------------------------------------
    VLRRenderer::Config renderConfig;
    renderConfig.width      = 1280;
    renderConfig.height     = 720;
    renderConfig.maxBounces = 8;
    renderConfig.maxSamples = 64;

    VLRRenderer renderer;
    if (!renderer.initialize(renderConfig)) {
        std::cerr << "Failed to initialize VLR renderer\n";
        return 1;
    }

    VLRScene scene = renderer.createScene();
    if (!scene) {
        std::cerr << "Failed to create VLR scene\n";
        return 1;
    }
    renderer.setScene(scene);

    // ------------------------------------------------------------------
    // Phase 2: Load scene geometry + retrieve camera/film from PBRT
    // ------------------------------------------------------------------
    SceneLoader loader(renderer);

    SceneLoader::LoadOptions loadOptions;
    loadOptions.enableParallel  = config.enableParallel;
    loadOptions.numThreads      = config.numThreads;
    loadOptions.enableTaskGraph = true;
    loadOptions.showTaskGraph   = false;
    loadOptions.appendMode      = false;

    PbrtCamera pbrtCamera;
    PbrtFilm   pbrtFilm;

    std::cout << "Loading PBRT scene: " << config.pbrtFile << "\n";
    std::cout << "  CWD: " << fs::current_path().string() << "\n";
    auto loadStart = std::chrono::high_resolution_clock::now();

    if (!loader.loadPbrtScene(config.pbrtFile, loadOptions, &pbrtCamera, &pbrtFilm)) {
        std::cerr << "Failed to load PBRT scene\n";
        return 1;
    }

    auto loadEnd = std::chrono::high_resolution_clock::now();
    double loadTime = std::chrono::duration<double>(loadEnd - loadStart).count();

    int width  = static_cast<int>(pbrtFilm.xResolution);
    int height = static_cast<int>(pbrtFilm.yResolution);
    int spp    = config.samplesOverride > 0
                     ? config.samplesOverride
                     : static_cast<int>(renderConfig.maxSamples);

    std::cout << "  Resolution: " << width << "x" << height << "\n"
              << "  FOV: " << pbrtCamera.fov << " deg\n"
              << "  Samples: " << spp << "\n"
              << "  Meshes: " << loader.getMeshCount()
              << ", Materials: " << loader.getMaterialCount()
              << ", Time: " << loadTime * 1000.0 << " ms\n\n";

    // ------------------------------------------------------------------
    // Phase 3: Set camera from PBRT Transform matrix
    // ------------------------------------------------------------------
    CameraVectors cam = cameraFromPbrtTransform(pbrtCamera.cameraToWorld);
    float aspect = static_cast<float>(width) / static_cast<float>(height);

    std::cout << "=== Camera (from PBRT) ===\n"
              << "  Position: (" << cam.position[0] << ", "
              << cam.position[1] << ", " << cam.position[2] << ")\n"
              << "  Target:   (" << cam.target[0] << ", "
              << cam.target[1] << ", " << cam.target[2] << ")\n"
              << "  Up:       (" << cam.up[0] << ", "
              << cam.up[1] << ", " << cam.up[2] << ")\n"
              << "  FOV: " << pbrtCamera.fov << " deg, Aspect: " << aspect << "\n\n";

    renderer.setCamera(cam.position, cam.target, cam.up, pbrtCamera.fov, aspect);

    // ------------------------------------------------------------------
    // Phase 5: Lighting (use PBRT area lights already parsed; add env fallback)
    // ------------------------------------------------------------------
    float envColor[] = {1.0f, 1.0f, 1.0f};
    vlrSetEnvironmentLight(renderer.getScene(), envColor);

    // ------------------------------------------------------------------
    // Phase 6: Render
    // ------------------------------------------------------------------
    std::cout << "Rendering " << width << "x" << height
              << " @ " << spp << " spp...\n";

    auto renderStart = std::chrono::high_resolution_clock::now();

    if (renderer.render(config.outputFile)) {
        auto renderEnd = std::chrono::high_resolution_clock::now();
        double renderTime = std::chrono::duration<double>(renderEnd - renderStart).count();

        std::cout << "\n=== Complete ===\n"
                  << "  Load:   " << loadTime * 1000.0 << " ms\n"
                  << "  Render: " << renderTime << " s\n"
                  << "  Total:  " << (loadTime + renderTime) << " s\n"
                  << "  Output: " << config.outputFile << "\n";
    } else {
        std::cerr << "Render failed!\n";
        return 1;
    }

    return 0;
}
