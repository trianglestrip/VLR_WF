// ============================================================================
// Parallel Scene Loading Benchmark
// 
// 对比：
// 1. 串行加载 vs 并行加载
// 2. 简单并行 vs 任务图优化
// ============================================================================

#include "SceneLoader.h"
#include "VLRRenderer.h"
#include <iostream>
#include <chrono>
#include <iomanip>
#include <vector>
#include <string>
#include <thread>

using namespace viewer;
using namespace std::chrono;

// 性能测试结果
struct BenchmarkResult {
    std::string name;
    double loadTime = 0.0;  // 秒
    size_t meshCount = 0;
    size_t materialCount = 0;
    size_t threadsUsed = 0;
};

// 串行加载测试
BenchmarkResult benchmarkSerial(const std::string& modelPath) {
    std::cout << "\n[1/3] Serial loading test..." << std::endl;
    std::cout << "======================================" << std::endl;
    
    BenchmarkResult result;
    result.name = "Serial Loading";
    result.threadsUsed = 1;
    
    VLRRenderer::Config config;
    config.width = 1024;
    config.height = 768;
    config.maxBounces = 8;
    
    VLRRenderer renderer;
    if (!renderer.initialize(config)) {
        std::cerr << "Failed to initialize renderer" << std::endl;
        return result;
    }
    
    SceneLoader loader(renderer);
    
    SceneLoader::LoadOptions loadOptions;
    loadOptions.enableParallel = false;  // 串行模式
    
    auto start = high_resolution_clock::now();
    bool success = loader.loadScene(modelPath, loadOptions);
    auto end = high_resolution_clock::now();
    
    result.loadTime = duration<double>(end - start).count();
    
    if (success) {
        result.meshCount = loader.getMeshCount();
        result.materialCount = loader.getMaterialCount();
        std::cout << "[OK] Serial loading succeeded" << std::endl;
        std::cout << "  - Meshes: " << result.meshCount << std::endl;
        std::cout << "  - Materials: " << result.materialCount << std::endl;
        std::cout << "  - Time: " << result.loadTime * 1000.0 << " ms" << std::endl;
    } else {
        std::cerr << "[FAIL] Serial loading failed" << std::endl;
    }
    
    return result;
}

// 简单并行加载测试
BenchmarkResult benchmarkSimpleParallel(const std::string& modelPath) {
    std::cout << "\n[2/3] Simple parallel loading test..." << std::endl;
    std::cout << "======================================" << std::endl;
    
    BenchmarkResult result;
    result.name = "Simple Parallel";
    
    VLRRenderer::Config config;
    config.width = 1024;
    config.height = 768;
    config.maxBounces = 8;
    
    VLRRenderer renderer;
    if (!renderer.initialize(config)) {
        std::cerr << "Failed to initialize renderer" << std::endl;
        return result;
    }
    
    SceneLoader loader(renderer);
    
    SceneLoader::LoadOptions loadOptions;
    loadOptions.enableParallel = true;       // 启用并行
    loadOptions.enableTaskGraph = false;     // 禁用任务图
    loadOptions.numThreads = 0;              // 自动检测
    
    auto start = high_resolution_clock::now();
    bool success = loader.loadScene(modelPath, loadOptions);
    auto end = high_resolution_clock::now();
    
    result.loadTime = duration<double>(end - start).count();
    
    if (success) {
        const auto& stats = loader.getStatistics();
        result.meshCount = loader.getMeshCount();
        result.materialCount = loader.getMaterialCount();
        result.threadsUsed = stats.threadsUsed;
        
        std::cout << "[OK] Simple parallel loading succeeded" << std::endl;
        std::cout << "  - Meshes: " << result.meshCount << std::endl;
        std::cout << "  - Materials: " << result.materialCount << std::endl;
        std::cout << "  - Threads: " << result.threadsUsed << std::endl;
        std::cout << "  - Time: " << result.loadTime * 1000.0 << " ms" << std::endl;
    } else {
        std::cerr << "[FAIL] Simple parallel loading failed" << std::endl;
    }
    
    return result;
}

// 任务图优化并行加载测试
BenchmarkResult benchmarkTaskGraph(const std::string& modelPath) {
    std::cout << "\n[3/3] Task graph parallel loading test..." << std::endl;
    std::cout << "======================================" << std::endl;
    
    BenchmarkResult result;
    result.name = "Task Graph Parallel";
    
    VLRRenderer::Config config;
    config.width = 1024;
    config.height = 768;
    config.maxBounces = 8;
    
    VLRRenderer renderer;
    if (!renderer.initialize(config)) {
        std::cerr << "Failed to initialize renderer" << std::endl;
        return result;
    }
    
    SceneLoader loader(renderer);
    
    SceneLoader::LoadOptions loadOptions;
    loadOptions.enableParallel = true;       // 启用并行
    loadOptions.enableTaskGraph = true;      // 启用任务图
    loadOptions.numThreads = 0;              // 自动检测
    loadOptions.showTaskGraph = true;        // 显示任务图
    
    auto start = high_resolution_clock::now();
    bool success = loader.loadScene(modelPath, loadOptions);
    auto end = high_resolution_clock::now();
    
    result.loadTime = duration<double>(end - start).count();
    
    if (success) {
        const auto& stats = loader.getStatistics();
        result.meshCount = loader.getMeshCount();
        result.materialCount = loader.getMaterialCount();
        result.threadsUsed = stats.threadsUsed;
        
        std::cout << "[OK] Task graph parallel loading succeeded" << std::endl;
        std::cout << "  - Meshes: " << result.meshCount << std::endl;
        std::cout << "  - Materials: " << result.materialCount << std::endl;
        std::cout << "  - Threads: " << result.threadsUsed << std::endl;
        std::cout << "  - Total tasks: " << stats.totalTasks << std::endl;
        std::cout << "  - Parallel tasks: " << stats.parallelTasks << std::endl;
        std::cout << "  - Time: " << result.loadTime * 1000.0 << " ms" << std::endl;
        
        // Export task graph
        loader.exportTaskGraph("taskgraph.dot");
        std::cout << "  - Task graph exported: taskgraph.dot" << std::endl;
    } else {
        std::cerr << "[FAIL] Task graph parallel loading failed" << std::endl;
    }
    
    return result;
}

// 打印对比结果
void printComparison(const std::vector<BenchmarkResult>& results) {
    std::cout << "\n" << std::string(80, '=') << std::endl;
    std::cout << "Performance Comparison Results" << std::endl;
    std::cout << std::string(80, '=') << std::endl;
    
    if (results.empty()) {
        std::cout << "No test results" << std::endl;
        return;
    }
    
    // Table header
    std::cout << std::left << std::setw(25) << "Method"
              << std::right << std::setw(15) << "Time(ms)"
              << std::right << std::setw(15) << "Speedup"
              << std::right << std::setw(12) << "Threads"
              << std::right << std::setw(13) << "Mesh/Mat" << std::endl;
    std::cout << std::string(80, '-') << std::endl;
    
    // 基准时间（串行）
    double baselineTime = results[0].loadTime;
    
    for (const auto& result : results) {
        double speedup = baselineTime / result.loadTime;
        
        std::cout << std::left << std::setw(25) << result.name
                  << std::right << std::setw(15) << std::fixed << std::setprecision(2) 
                  << result.loadTime * 1000.0
                  << std::right << std::setw(15) << std::fixed << std::setprecision(2) 
                  << speedup << "x"
                  << std::right << std::setw(12) << result.threadsUsed
                  << std::right << std::setw(8) << result.meshCount 
                  << "/" << std::left << std::setw(4) << result.materialCount << std::endl;
    }
    
    std::cout << std::string(80, '=') << std::endl;
    
    // Summary
    if (results.size() >= 3) {
        double simpleSpeedup = baselineTime / results[1].loadTime;
        double graphSpeedup = baselineTime / results[2].loadTime;
        
        std::cout << "\nSummary:" << std::endl;
        std::cout << "  - Simple parallel speedup: " << std::fixed << std::setprecision(2) 
                  << simpleSpeedup << "x" << std::endl;
        std::cout << "  - Task graph speedup: " << std::fixed << std::setprecision(2) 
                  << graphSpeedup << "x" << std::endl;
        
        if (graphSpeedup > simpleSpeedup) {
            double improvement = (graphSpeedup - simpleSpeedup) / simpleSpeedup * 100.0;
            std::cout << "  - Task graph improvement over simple: " << std::fixed << std::setprecision(1) 
                      << improvement << "%" << std::endl;
        }
    }
    
    std::cout << std::endl;
}

int main(int argc, char* argv[]) {
    std::cout << "=== Taskflow Parallel Loading Performance Test ===" << std::endl;
    std::cout << "C++20 + Taskflow + Zero-Copy Optimization" << std::endl;
    std::cout << std::endl;
    
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <model_file>" << std::endl;
        std::cerr << "Example: " << argv[0] << " test/resources/sphere/sphere.obj" << std::endl;
        return 1;
    }
    
    std::string modelPath = argv[1];
    std::cout << "Test model: " << modelPath << std::endl;
    std::cout << "CPU cores: " << std::thread::hardware_concurrency() << std::endl;
    
    std::vector<BenchmarkResult> results;
    
    // Test 1: Serial loading
    try {
        results.push_back(benchmarkSerial(modelPath));
    } catch (const std::exception& e) {
        std::cerr << "Serial test exception: " << e.what() << std::endl;
    }
    
    // Test 2: Simple parallel loading
    try {
        results.push_back(benchmarkSimpleParallel(modelPath));
    } catch (const std::exception& e) {
        std::cerr << "Simple parallel test exception: " << e.what() << std::endl;
    }
    
    // Test 3: Task graph parallel loading
    try {
        results.push_back(benchmarkTaskGraph(modelPath));
    } catch (const std::exception& e) {
        std::cerr << "Task graph test exception: " << e.what() << std::endl;
    }
    
    // 打印对比结果
    printComparison(results);
    
    return 0;
}
