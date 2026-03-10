// ============================================================================
// Parallel Scene Loading Example
// 
// 演示如何使用 ParallelSceneLoader 进行高性能并行加载
// ============================================================================

#include "SceneLoader.h"
#include "VLRRenderer.h"
#include <iostream>

using namespace viewer;

int main(int argc, char* argv[]) {
    std::cout << "=== Parallel Scene Loading Example ===" << std::endl;
    std::cout << "C++20 + Taskflow + Zero-Copy" << std::endl;
    std::cout << std::endl;
    
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <model_file>" << std::endl;
        return 1;
    }
    
    std::string modelPath = argv[1];
    
    // 1. 配置渲染器
    VLRRenderer::Config renderConfig;
    renderConfig.width = 1920;
    renderConfig.height = 1080;
    renderConfig.maxBounces = 8;
    
    VLRRenderer renderer;
    if (!renderer.initialize(renderConfig)) {
        std::cerr << "Failed to initialize renderer" << std::endl;
        return 1;
    }
    
    // 2. 创建场景加载器
    SceneLoader loader(renderer);
    
    // 3. 配置并行加载选项
    SceneLoader::LoadOptions loadOptions;
    loadOptions.triangulate = true;
    loadOptions.generateNormals = true;
    loadOptions.optimizeMeshes = true;
    loadOptions.scale = 1.0f;
    
    // 并行配置
    loadOptions.enableParallel = true;       // 启用并行
    loadOptions.numThreads = 0;              // 自动检测CPU核心数
    loadOptions.enableTaskGraph = true;      // 启用任务图优化
    loadOptions.showTaskGraph = true;        // 显示任务图（调试用）
    
    // 4. 并行加载场景
    std::cout << "Loading scene: " << modelPath << std::endl;
    
    if (!loader.loadScene(modelPath, loadOptions)) {
        std::cerr << "Failed to load scene" << std::endl;
        return 1;
    }
    
    // 4. 获取统计信息
    const auto& stats = loader.getStatistics();
    std::cout << "\n=== Loading Statistics ===" << std::endl;
    std::cout << "Total time: " << stats.totalTime * 1000.0 << " ms" << std::endl;
    std::cout << "Threads used: " << stats.threadsUsed << std::endl;
    std::cout << "Total tasks: " << stats.totalTasks << std::endl;
    std::cout << "Parallel tasks: " << stats.parallelTasks << std::endl;
    std::cout << "Meshes loaded: " << loader.getMeshCount() << std::endl;
    std::cout << "Materials loaded: " << loader.getMaterialCount() << std::endl;
    
    // 5. 导出任务图（可选）
    loader.exportTaskGraph("my_taskgraph.dot");
    std::cout << "\nTask graph exported to: my_taskgraph.dot" << std::endl;
    std::cout << "Visualize with: dot -Tpng my_taskgraph.dot -o taskgraph.png" << std::endl;
    
    // 6. 设置相机
    float position[] = {0.0f, 2.0f, 5.0f};
    float target[] = {0.0f, 0.0f, 0.0f};
    float up[] = {0.0f, 1.0f, 0.0f};
    renderer.setCamera(position, target, up, 45.0f);
    
    // 7. 渲染
    std::cout << "\nRendering..." << std::endl;
    if (renderer.render("output.png")) {
        std::cout << "Render complete: output.png" << std::endl;
    }
    
    return 0;
}
