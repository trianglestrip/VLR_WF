// ============================================================================
// Simple Viewer Test - 演示如何使用Viewer加载和渲染场景
// ============================================================================

#include "VLRRenderer.h"
#include "SceneLoader.h"
#include <iostream>
#include <string>

int main(int argc, char* argv[]) {
    std::cout << "=== VLR Viewer 简单测试 ===" << std::endl;

    // 检查命令行参数
    if (argc < 2) {
        std::cout << "用法: " << argv[0] << " <模型文件路径> [输出图像路径]" << std::endl;
        std::cout << "示例: " << argv[0] << " test/resources/sphere/sphere.obj output.png" << std::endl;
        return 1;
    }

    std::string modelPath = argv[1];
    std::string outputPath = argc >= 3 ? argv[2] : "viewer_output.png";

    // 1. 创建并初始化渲染器
    viewer::VLRRenderer renderer;
    viewer::VLRRenderer::Config config;
    config.width = 1024;
    config.height = 768;
    config.maxSamples = 512;
    config.maxBounces = 8;
    config.enableDenoiser = true;

    if (!renderer.initialize(config)) {
        std::cerr << "渲染器初始化失败" << std::endl;
        return 1;
    }

    // 2. 创建场景
    VLRScene scene = renderer.createScene();
    if (!scene) {
        std::cerr << "创建场景失败" << std::endl;
        return 1;
    }
    renderer.setScene(scene);

    // 3. 加载模型
    viewer::SceneLoader loader(renderer);
    viewer::SceneLoader::LoadOptions loadOptions;
    loadOptions.flipUVs = true;
    loadOptions.triangulate = true;
    loadOptions.generateSmoothNormals = true;
    loadOptions.scale = 1.0f;

    std::cout << "\n正在加载模型: " << modelPath << std::endl;
    if (!loader.loadScene(modelPath, loadOptions)) {
        std::cerr << "加载场景失败" << std::endl;
        return 1;
    }

    std::cout << "模型加载成功!" << std::endl;
    std::cout << "  - 网格数: " << loader.getMeshCount() << std::endl;
    std::cout << "  - 材质数: " << loader.getMaterialCount() << std::endl;

    // 4. 设置相机（基于场景边界自动计算）
    float cameraPos[3], cameraTarget[3];
    loader.getSuggestedCameraPosition(cameraPos, cameraTarget);

    float up[3] = {0.0f, 1.0f, 0.0f};
    float aspect = static_cast<float>(config.width) / config.height;
    
    if (!renderer.setCamera(cameraPos, cameraTarget, up, 45.0f, aspect)) {
        std::cerr << "设置相机失败" << std::endl;
        return 1;
    }

    // 5. 渲染场景
    std::cout << "\n开始渲染..." << std::endl;
    if (!renderer.render(outputPath)) {
        std::cerr << "渲染失败" << std::endl;
        return 1;
    }

    std::cout << "\n渲染完成! 输出: " << outputPath << std::endl;
    return 0;
}
