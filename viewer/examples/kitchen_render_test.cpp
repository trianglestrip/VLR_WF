// ============================================================================
// Kitchen Scene Rendering Test
// 
// Loads the Kitchen scene (295 PLY meshes) and renders using VLR
// Demonstrates parallel loading performance on a complex real-world scene
// ============================================================================

#include "SceneLoader.h"
#include "VLRRenderer.h"
#include <iostream>
#include <chrono>
#include <filesystem>
#include <vector>
#include <string>

using namespace viewer;
namespace fs = std::filesystem;

void printUsage(const char* progName) {
    std::cout << "Usage: " << progName << " [options]" << std::endl;
    std::cout << "\nOptions:" << std::endl;
    std::cout << "  --serial          Use serial loading (default: parallel)" << std::endl;
    std::cout << "  --no-taskgraph    Disable task graph optimization" << std::endl;
    std::cout << "  --threads N       Number of threads (default: auto)" << std::endl;
    std::cout << "  --width W         Image width (default: 1280)" << std::endl;
    std::cout << "  --height H        Image height (default: 720)" << std::endl;
    std::cout << "  --output FILE     Output filename (default: kitchen_render.png)" << std::endl;
    std::cout << "  --help            Show this help" << std::endl;
}

struct TestConfig {
    bool enableParallel = true;
    bool enableTaskGraph = true;
    int numThreads = 0;
    int width = 1280;
    int height = 720;
    std::string outputFile = "kitchen_render.png";
};

TestConfig parseArgs(int argc, char* argv[]) {
    TestConfig config;
    
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        
        if (arg == "--serial") {
            config.enableParallel = false;
        } else if (arg == "--no-taskgraph") {
            config.enableTaskGraph = false;
        } else if (arg == "--threads" && i + 1 < argc) {
            config.numThreads = std::stoi(argv[++i]);
        } else if (arg == "--width" && i + 1 < argc) {
            config.width = std::stoi(argv[++i]);
        } else if (arg == "--height" && i + 1 < argc) {
            config.height = std::stoi(argv[++i]);
        } else if (arg == "--output" && i + 1 < argc) {
            config.outputFile = argv[++i];
        } else if (arg == "--help") {
            printUsage(argv[0]);
            std::exit(0);
        }
    }
    
    return config;
}

std::vector<std::string> findKitchenMeshes(const std::string& kitchenDir) {
    std::vector<std::string> meshFiles;
    fs::path modelsPath = fs::path(kitchenDir) / "models";
    
    if (!fs::exists(modelsPath)) {
        std::cerr << "Error: Kitchen models directory not found: " << modelsPath << std::endl;
        return meshFiles;
    }
    
    for (const auto& entry : fs::directory_iterator(modelsPath)) {
        if (entry.is_regular_file() && entry.path().extension() == ".ply") {
            meshFiles.push_back(entry.path().string());
        }
    }
    
    std::sort(meshFiles.begin(), meshFiles.end());
    return meshFiles;
}

int main(int argc, char* argv[]) {
    std::cout << "========================================" << std::endl;
    std::cout << "    Kitchen Scene Rendering Test" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "C++20 + Taskflow + Zero-Copy + VLR" << std::endl;
    std::cout << std::endl;
    
    TestConfig config = parseArgs(argc, argv);
    
    // Find kitchen directory
    std::string kitchenDir = "models/kitchen";
    if (!fs::exists(kitchenDir)) {
        std::cerr << "Error: Kitchen directory not found: " << kitchenDir << std::endl;
        std::cerr << "Please ensure the kitchen scene is in models/kitchen/" << std::endl;
        return 1;
    }
    
    // Find all PLY meshes
    std::cout << "Scanning for kitchen meshes..." << std::endl;
    auto meshFiles = findKitchenMeshes(kitchenDir);
    
    if (meshFiles.empty()) {
        std::cerr << "Error: No PLY meshes found in " << kitchenDir << "/models" << std::endl;
        return 1;
    }
    
    std::cout << "Found " << meshFiles.size() << " PLY meshes" << std::endl;
    std::cout << std::endl;
    
    // Initialize VLR renderer
    std::cout << "Initializing VLR renderer..." << std::endl;
    VLRRenderer::Config renderConfig;
    renderConfig.width = config.width;
    renderConfig.height = config.height;
    renderConfig.maxBounces = 8;
    renderConfig.maxSamples = 64;
    
    VLRRenderer renderer;
    if (!renderer.initialize(renderConfig)) {
        std::cerr << "Failed to initialize VLR renderer" << std::endl;
        return 1;
    }
    std::cout << "Renderer initialized: " << config.width << "x" << config.height << std::endl;
    
    // Create VLR scene
    VLRScene scene = renderer.createScene();
    if (!scene) {
        std::cerr << "Failed to create VLR scene" << std::endl;
        return 1;
    }
    renderer.setScene(scene);
    std::cout << "VLR scene created" << std::endl;
    std::cout << std::endl;
    
    // Create scene loader
    SceneLoader loader(renderer);
    
    // Configure loading options
    SceneLoader::LoadOptions loadOptions;
    loadOptions.triangulate = true;
    loadOptions.generateNormals = true;
    loadOptions.optimizeMeshes = true;
    loadOptions.scale = 1.0f;
    
    // Parallel configuration
    loadOptions.enableParallel = config.enableParallel;
    loadOptions.numThreads = config.numThreads;
    loadOptions.enableTaskGraph = config.enableTaskGraph;
    loadOptions.showTaskGraph = false;
    loadOptions.appendMode = false;  // First load clears scene
    
    std::cout << "=== Loading Configuration ===" << std::endl;
    std::cout << "Mode: " << (config.enableParallel ? "Parallel" : "Serial") << std::endl;
    if (config.enableParallel) {
        std::cout << "Task Graph: " << (config.enableTaskGraph ? "Enabled" : "Disabled") << std::endl;
        std::cout << "Threads: " << (config.numThreads == 0 ? "Auto" : std::to_string(config.numThreads)) << std::endl;
    }
    std::cout << std::endl;
    
    // Load kitchen meshes (limit to first 50 for stability)
    std::cout << "Loading kitchen scene..." << std::endl;
    size_t maxMeshesToLoad = (std::min)(static_cast<size_t>(50), meshFiles.size());
    std::cout << "Loading first " << maxMeshesToLoad << " meshes (out of " << meshFiles.size() << " total)" << std::endl;
    
    auto loadStart = std::chrono::high_resolution_clock::now();
    
    size_t successCount = 0;
    size_t failCount = 0;
    
    for (size_t i = 0; i < maxMeshesToLoad; ++i) {
        const auto& meshFile = meshFiles[i];
        
        // After first load, use append mode
        if (i > 0) {
            loadOptions.appendMode = true;
        }
        
        if (loader.loadScene(meshFile, loadOptions)) {
            ++successCount;
            if ((i + 1) % 10 == 0) {
                std::cout << "  Loaded " << (i + 1) << "/" << maxMeshesToLoad << " meshes..." << std::endl;
            }
        } else {
            ++failCount;
            std::cerr << "Warning: Failed to load " << meshFile << std::endl;
        }
    }
    
    auto loadEnd = std::chrono::high_resolution_clock::now();
    double loadTime = std::chrono::duration<double>(loadEnd - loadStart).count();
    
    std::cout << std::endl;
    std::cout << "=== Loading Results ===" << std::endl;
    std::cout << "Successfully loaded: " << successCount << " meshes" << std::endl;
    std::cout << "Failed: " << failCount << " meshes" << std::endl;
    std::cout << "Total loading time: " << loadTime * 1000.0 << " ms" << std::endl;
    std::cout << "Average per mesh: " << (loadTime * 1000.0 / maxMeshesToLoad) << " ms" << std::endl;
    
    // Get statistics from last load
    const auto& stats = loader.getStatistics();
    std::cout << "\n=== Last Load Statistics ===" << std::endl;
    std::cout << "Load time: " << stats.loadTime * 1000.0 << " ms" << std::endl;
    std::cout << "Threads used: " << stats.threadsUsed << std::endl;
    std::cout << "Total tasks: " << stats.totalTasks << std::endl;
    std::cout << "Parallel tasks: " << stats.parallelTasks << std::endl;
    std::cout << "Total meshes in scene: " << loader.getMeshCount() << std::endl;
    std::cout << "Total materials: " << loader.getMaterialCount() << std::endl;
    std::cout << std::endl;
    
    // Export task graph if enabled
    if (config.enableParallel && config.enableTaskGraph) {
        std::string graphFile = "kitchen_taskgraph.dot";
        loader.exportTaskGraph(graphFile);
        std::cout << "Task graph exported to: " << graphFile << std::endl;
        std::cout << "Visualize with: dot -Tpng " << graphFile << " -o kitchen_taskgraph.png" << std::endl;
        std::cout << std::endl;
    }
    
    // Get scene bounds
    float boundsMin[3], boundsMax[3];
    loader.getSceneBounds(boundsMin, boundsMax);
    std::cout << "=== Scene Bounds ===" << std::endl;
    std::cout << "Min: (" << boundsMin[0] << ", " << boundsMin[1] << ", " << boundsMin[2] << ")" << std::endl;
    std::cout << "Max: (" << boundsMax[0] << ", " << boundsMax[1] << ", " << boundsMax[2] << ")" << std::endl;
    float sceneSize[3] = {
        boundsMax[0] - boundsMin[0],
        boundsMax[1] - boundsMin[1],
        boundsMax[2] - boundsMin[2]
    };
    std::cout << "Size: (" << sceneSize[0] << ", " << sceneSize[1] << ", " << sceneSize[2] << ")" << std::endl;
    std::cout << std::endl;
    
    // Set up camera using auto-calculated position
    float cameraPos[3], targetPos[3];
    loader.getSuggestedCameraPosition(cameraPos, targetPos);
    float upVec[] = {0.0f, 1.0f, 0.0f};
    float fov = 45.0f;
    
    std::cout << "Setting up camera..." << std::endl;
    std::cout << "Position: (" << cameraPos[0] << ", " << cameraPos[1] << ", " << cameraPos[2] << ")" << std::endl;
    std::cout << "FOV: " << fov << " degrees" << std::endl;
    float aspect = static_cast<float>(config.width) / static_cast<float>(config.height);
    renderer.setCamera(cameraPos, targetPos, upVec, fov, aspect);
    std::cout << std::endl;
    
    // Add lighting to the scene (positioned relative to scene bounds)
    std::cout << "Setting up lighting..." << std::endl;
    
    // Calculate scene center and size for light positioning
    float sceneCenter[3] = {
        (boundsMin[0] + boundsMax[0]) * 0.5f,
        (boundsMin[1] + boundsMax[1]) * 0.5f,
        (boundsMin[2] + boundsMax[2]) * 0.5f
    };
    float maxSize = (std::max)({sceneSize[0], sceneSize[1], sceneSize[2]});
    
    // Add environment light (soft ambient illumination)
    float envColor[] = {1.0f, 1.0f, 1.0f};
    VLRResult result = vlrSetEnvironmentLight(renderer.getScene(), envColor);
    if (result == VLRResult_Success) {
        std::cout << "  Environment light added (intensity: 1.0)" << std::endl;
    }
    
    // Add key light (main illumination from top-front, scaled to scene)
    float keyLightPos[] = {
        sceneCenter[0] + maxSize * 0.5f,
        sceneCenter[1] + maxSize * 1.0f,
        sceneCenter[2] + maxSize * 0.5f
    };
    float keyLightIntensity[] = {100.0f * maxSize, 100.0f * maxSize, 100.0f * maxSize};
    result = vlrAddPointLight(renderer.getScene(), keyLightPos, keyLightIntensity);
    if (result == VLRResult_Success) {
        std::cout << "  Key light added at (" << keyLightPos[0] << ", " << keyLightPos[1] << ", " << keyLightPos[2] << ")" << std::endl;
    }
    
    // Add fill light (softer light from side)
    float fillLightPos[] = {
        sceneCenter[0] - maxSize * 0.5f,
        sceneCenter[1] + maxSize * 0.5f,
        sceneCenter[2] + maxSize * 0.5f
    };
    float fillLightIntensity[] = {40.0f * maxSize, 40.0f * maxSize, 40.0f * maxSize};
    result = vlrAddPointLight(renderer.getScene(), fillLightPos, fillLightIntensity);
    if (result == VLRResult_Success) {
        std::cout << "  Fill light added at (" << fillLightPos[0] << ", " << fillLightPos[1] << ", " << fillLightPos[2] << ")" << std::endl;
    }
    
    std::cout << std::endl;
    
    // Render the scene
    std::cout << "Rendering kitchen scene..." << std::endl;
    std::cout << "Output: " << config.outputFile << std::endl;
    std::cout << "Resolution: " << config.width << "x" << config.height << std::endl;
    std::cout << "Samples per pixel: " << renderConfig.maxSamples << std::endl;
    std::cout << std::endl;
    
    auto renderStart = std::chrono::high_resolution_clock::now();
    
    if (renderer.render(config.outputFile)) {
        auto renderEnd = std::chrono::high_resolution_clock::now();
        double renderTime = std::chrono::duration<double>(renderEnd - renderStart).count();
        
        std::cout << "\n=== Render Complete ===" << std::endl;
        std::cout << "Render time: " << renderTime << " seconds" << std::endl;
        std::cout << "Output saved to: " << config.outputFile << std::endl;
        std::cout << std::endl;
        
        std::cout << "=== Performance Summary ===" << std::endl;
        std::cout << "Scene loading: " << loadTime * 1000.0 << " ms" << std::endl;
        std::cout << "Rendering: " << renderTime << " s" << std::endl;
        std::cout << "Total: " << (loadTime + renderTime) << " s" << std::endl;
    } else {
        std::cerr << "Render failed!" << std::endl;
        return 1;
    }
    
    return 0;
}
