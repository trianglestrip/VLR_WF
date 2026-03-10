// ============================================================================
// 零拷贝性能基准测试
//
// 对比传统拷贝方法和零拷贝方法的性能差异
// ============================================================================

#include "ZeroCopyMesh.h"
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <iostream>
#include <chrono>
#include <vector>
#include <span>

using namespace viewer;
using namespace std::chrono;

// 传统方法：逐个拷贝
void traditionalCopy(const aiMesh* mesh, std::vector<float>& positions) {
    positions.clear();
    positions.reserve(mesh->mNumVertices * 3);
    
    for (unsigned int i = 0; i < mesh->mNumVertices; ++i) {
        const aiVector3D& pos = mesh->mVertices[i];
        positions.push_back(pos.x);
        positions.push_back(pos.y);
        positions.push_back(pos.z);
    }
}

// 零拷贝方法：直接使用Assimp内存
std::span<const float> zeroCopyAccess(const aiMesh* mesh) {
    ZeroCopyMeshView view(mesh);
    return view.getPositions();
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cout << "用法: " << argv[0] << " <模型文件路径>" << std::endl;
        return 1;
    }

    std::cout << "=== 零拷贝性能基准测试 ===" << std::endl;
    std::cout << "模型: " << argv[1] << std::endl << std::endl;

    // 加载模型
    Assimp::Importer importer;
    const aiScene* scene = importer.ReadFile(argv[1], 
        aiProcess_Triangulate | 
        aiProcess_JoinIdenticalVertices |
        aiProcess_GenNormals
    );

    if (!scene || !scene->mRootNode || scene->mNumMeshes == 0) {
        std::cerr << "加载模型失败: " << importer.GetErrorString() << std::endl;
        return 1;
    }

    const aiMesh* mesh = scene->mMeshes[0];
    std::cout << "网格信息:" << std::endl;
    std::cout << "  - 顶点数: " << mesh->mNumVertices << std::endl;
    std::cout << "  - 三角形数: " << mesh->mNumFaces << std::endl;
    std::cout << "  - 内存大小: " << (mesh->mNumVertices * 3 * sizeof(float)) / 1024.0 << " KB" << std::endl;
    std::cout << std::endl;

    // ========================================================================
    // 测试1: 传统拷贝方法
    // ========================================================================
    std::cout << "[测试1] 传统拷贝方法 (push_back)" << std::endl;
    
    std::vector<float> copiedData;
    const int iterations = 100;
    
    auto start1 = high_resolution_clock::now();
    for (int i = 0; i < iterations; ++i) {
        traditionalCopy(mesh, copiedData);
    }
    auto end1 = high_resolution_clock::now();
    auto duration1 = duration_cast<microseconds>(end1 - start1).count();
    
    std::cout << "  - 总时间: " << duration1 << " us" << std::endl;
    std::cout << "  - 平均: " << duration1 / iterations << " us/iter" << std::endl;
    std::cout << "  - 数据量: " << copiedData.size() * sizeof(float) / 1024.0 << " KB" << std::endl;
    std::cout << std::endl;

    // ========================================================================
    // 测试2: 零拷贝方法
    // ========================================================================
    std::cout << "[测试2] 零拷贝方法 (span + reinterpret_cast)" << std::endl;
    
    std::span<const float> zeroCopyData;
    
    auto start2 = high_resolution_clock::now();
    for (int i = 0; i < iterations; ++i) {
        zeroCopyData = zeroCopyAccess(mesh);
    }
    auto end2 = high_resolution_clock::now();
    auto duration2 = duration_cast<microseconds>(end2 - start2).count();
    
    std::cout << "  - 总时间: " << duration2 << " us" << std::endl;
    std::cout << "  - 平均: " << duration2 / iterations << " us/iter" << std::endl;
    std::cout << "  - 数据量: " << zeroCopyData.size() * sizeof(float) / 1024.0 << " KB" << std::endl;
    std::cout << std::endl;

    // ========================================================================
    // 性能对比
    // ========================================================================
    std::cout << "=== 性能对比 ===" << std::endl;
    std::cout << "  - 加速比: " << static_cast<double>(duration1) / duration2 << "x" << std::endl;
    std::cout << "  - 时间节省: " << (duration1 - duration2) << " us ("
              << (100.0 * (duration1 - duration2) / duration1) << "%)" << std::endl;
    
    // 内存使用对比
    size_t copiedMemory = copiedData.capacity() * sizeof(float);
    size_t zeroCopyMemory = sizeof(std::span<const float>); // 只有指针+大小
    
    std::cout << "  - 内存节省: " << (copiedMemory - zeroCopyMemory) / 1024.0 << " KB" << std::endl;
    std::cout << std::endl;

    // ========================================================================
    // 验证数据正确性
    // ========================================================================
    std::cout << "=== 数据验证 ===" << std::endl;
    
    bool dataMatches = true;
    for (size_t i = 0; i < std::min(copiedData.size(), zeroCopyData.size()); ++i) {
        if (std::abs(copiedData[i] - zeroCopyData[i]) > 1e-6f) {
            dataMatches = false;
            std::cout << "  [X] Data mismatch at index " << i << std::endl;
            break;
        }
    }
    
    if (dataMatches) {
        std::cout << "  [OK] Data matches perfectly" << std::endl;
    }
    
    std::cout << std::endl;
    std::cout << "=== Conclusion ===" << std::endl;
    if (duration2 < duration1) {
        std::cout << "  [OK] Zero-copy method is faster!" << std::endl;
    } else {
        std::cout << "  [!] Zero-copy advantage not clear (test scale may be too small)" << std::endl;
    }

    return 0;
}
