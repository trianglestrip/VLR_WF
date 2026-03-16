// ============================================================================
// SceneLoader 实现（C++20零拷贝优化版本 + Taskflow并行）
// ============================================================================

#include "SceneLoader.h"
#include "MeshData.h"
#include "ZeroCopyMesh.h"
#include "PbrtParser.h"
#include "PbrtSceneData.h"
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <taskflow.hpp>
#include <iostream>
#include <cstring>
#include <algorithm>
#include <ranges>
#include <numbers>
#include <chrono>
#include <fstream>
#include <memory>
#include <filesystem>

// 编译期验证Assimp内存布局
// 注意：aiVector3D在assimp/vector3.h中定义为包含x,y,z的结构体
// 我们验证它是否可以安全地reinterpret为float数组

namespace viewer {

// Taskflow 实现类（Pimpl模式，避免头文件依赖）
class SceneLoader::TaskflowImpl {
public:
    tf::Executor executor;
    tf::Taskflow taskflow;
    
    explicit TaskflowImpl(uint32_t numThreads)
        : executor(numThreads == 0 ? std::thread::hardware_concurrency() : numThreads)
    {}
};

SceneLoader::SceneLoader(VLRRenderer& renderer)
    : m_renderer(renderer)
    , m_taskflow(nullptr)  // 延迟初始化
{
    // 初始化边界为无效值
    m_boundsMin[0] = m_boundsMin[1] = m_boundsMin[2] = std::numeric_limits<float>::max();
    m_boundsMax[0] = m_boundsMax[1] = m_boundsMax[2] = std::numeric_limits<float>::lowest();
}

SceneLoader::~SceneLoader() {
    if (m_taskflow) {
        m_taskflow->executor.wait_for_all();
    }
}

bool SceneLoader::loadScene(const std::string& filepath, const LoadOptions& options) {
    auto startTime = std::chrono::high_resolution_clock::now();
    
    // 如果启用并行，初始化Taskflow
    if (options.enableParallel && !m_taskflow) {
        m_taskflow = std::make_unique<TaskflowImpl>(options.numThreads);
        std::cout << "[SceneLoader] 并行模式已启用，线程数: " 
                  << m_taskflow->executor.num_workers() << std::endl;
    }
    
    bool result = false;
    std::cout << "[SceneLoader] 加载场景: " << filepath << std::endl;

    // 创建Assimp导入器
    Assimp::Importer importer;

    // 设置后处理标志
    unsigned int flags = 0;
    if (options.triangulate) {
        flags |= aiProcess_Triangulate;
    }
    if (options.flipUVs) {
        flags |= aiProcess_FlipUVs;
    }
    if (options.generateNormals) {
        flags |= aiProcess_GenNormals;
    }
    if (options.generateSmoothNormals) {
        flags |= aiProcess_GenSmoothNormals;
    }
    if (options.optimizeMeshes) {
        flags |= aiProcess_OptimizeMeshes;
        flags |= aiProcess_OptimizeGraph;
    }

    // 仅添加基础解析所需的标志，不做任何优化处理

    // 导入场景
    const aiScene* scene = importer.ReadFile(filepath, flags);

    if (!scene || scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE || !scene->mRootNode) {
        std::cerr << "[SceneLoader] Assimp错误: " << importer.GetErrorString() << std::endl;
        return false;
    }

    std::cout << "[SceneLoader] 场景加载成功:" << std::endl;
    std::cout << "  - 网格数量: " << scene->mNumMeshes << std::endl;
    std::cout << "  - 材质数量: " << scene->mNumMaterials << std::endl;
    std::cout << "  - 纹理数量: " << scene->mNumTextures << std::endl;

    // 根据配置选择串行或并行处理
    if (options.enableParallel) {
        result = loadSceneParallel(scene, options);
    } else {
        result = processScene(scene, options);
    }

    if (result) {
        std::cout << "[SceneLoader] 场景处理完成" << std::endl;
    }
    
    // 更新统计信息
    auto endTime = std::chrono::high_resolution_clock::now();
    m_stats.loadTime = std::chrono::duration<double>(endTime - startTime).count();
    if (m_taskflow) {
        m_stats.threadsUsed = m_taskflow->executor.num_workers();
    } else {
        m_stats.threadsUsed = 1;
    }

    return result;
}

bool SceneLoader::processScene(const aiScene* scene, const LoadOptions& options) {
    // 重置数据（除非是追加模式）
    if (!options.appendMode) {
        m_meshes.clear();
        m_materials.clear();
        m_meshCount = 0;
        m_materialCount = 0;
    }

    // 预留空间（避免重分配）
    size_t existingMeshes = m_meshes.size();
    size_t existingMaterials = m_materials.size();
    m_meshes.reserve(existingMeshes + scene->mNumMeshes);
    m_materials.reserve(existingMaterials + scene->mNumMaterials);

    // 递归处理节点树
    processNode(scene, scene->mRootNode, options);

    // 在处理完网格后计算场景边界
    computeBounds();

    std::cout << "[SceneLoader] 处理完成: " 
              << m_meshCount << " 网格, " 
              << m_materialCount << " 材质" << std::endl;

    return true;
}

void SceneLoader::processNode(const aiScene* scene, const aiNode* node, const LoadOptions& options) {
    // C++20: 使用ranges处理网格（更简洁）
    auto meshIndices = std::span(node->mMeshes, node->mNumMeshes);
    for (const auto meshIndex : meshIndices) {
        const aiMesh* mesh = scene->mMeshes[meshIndex];
        if (auto meshData = processMesh(scene, mesh, options)) {
            m_meshes.push_back(std::move(*meshData)); // 移动语义，零拷贝
        }
    }

    // C++20: 使用ranges递归处理子节点
    auto children = std::span(node->mChildren, node->mNumChildren);
    for (const auto* child : children) {
        processNode(scene, child, options);
    }
}

std::optional<MeshData> SceneLoader::processMesh(
    const aiScene* scene, 
    const aiMesh* mesh, 
    const LoadOptions& options
) {
    if (!mesh->HasPositions()) {
        std::cerr << "[SceneLoader] 网格缺少位置数据" << std::endl;
        return std::nullopt;
    }

    std::cout << "[SceneLoader] 处理网格: " << mesh->mName.C_Str() 
              << " (" << mesh->mNumVertices << " 顶点, " 
              << mesh->mNumFaces << " 面)" << std::endl;

    // ========================================================================
    // 零拷贝优化：直接使用Assimp的内存
    // ========================================================================
    
    ZeroCopyMeshView meshView(mesh, options.scale);
    
    // 1. 位置数据 - 零拷贝访问
    std::span<const float> positions = meshView.getPositions();
    
    // 如果需要缩放，必须拷贝并转换
    std::vector<float> scaledPositions;
    if (meshView.needsScaling()) {
        std::cout << "[SceneLoader] 应用缩放因子: " << options.scale << std::endl;
        scaledPositions.resize(positions.size());
        
        // 使用ranges::transform应用缩放
        std::ranges::transform(
            positions,
            scaledPositions.begin(),
            [scale = options.scale](float v) { return v * scale; }
        );
        positions = scaledPositions; // 更新为缩放后的span
    } else {
        std::cout << "[SceneLoader] 零拷贝：直接使用Assimp顶点数据" << std::endl;
    }
    
    // 2. 法线数据 - 零拷贝访问
    std::span<const float> normals = meshView.getNormals();
    if (!normals.empty()) {
        std::cout << "[SceneLoader] 零拷贝：直接使用Assimp法线数据" << std::endl;
    }
    
    // 3. 纹理坐标 - 需要从3D转2D（必须拷贝）
    std::vector<float> texcoords;
    auto texcoords3D = meshView.getTexCoordsRaw();
    if (!texcoords3D.empty()) {
        texcoords.resize(texcoords3D.size() * 2);
        VertexTransformer::extractUV(texcoords3D, texcoords);
        std::cout << "[SceneLoader] 提取UV坐标: " << texcoords3D.size() << " 顶点" << std::endl;
    }
    
    // 4. 索引数据 - 必须展平aiFace（唯一必须的拷贝）
    const size_t indexCount = IndexExtractor::calculateIndexCount(mesh);
    std::vector<uint32_t> indices(indexCount);
    const size_t actualCount = IndexExtractor::extractTriangleIndices(mesh, indices);
    indices.resize(actualCount);
    
    std::cout << "[SceneLoader] 提取索引: " << actualCount / 3 << " 三角形" << std::endl;

    // ========================================================================
    // 创建VLR网格 - 使用零拷贝的span传递数据
    // ========================================================================
    
    // 处理材质
    VLRMaterial vlrMaterial = nullptr;
    if (mesh->mMaterialIndex < scene->mNumMaterials) {
        const aiMaterial* material = scene->mMaterials[mesh->mMaterialIndex];
        if (auto matData = extractMaterialData(scene, material)) {
            vlrMaterial = createVLRMaterial(*matData);
        }
    }
    
    // 创建VLR网格对象
    // 注意：这里传递的是指针，VLR内部会拷贝到GPU
    // 但我们在CPU端实现了零拷贝
    VLRTriangleMesh vlrMesh = nullptr;
    VLRResult result = vlrCreateTriangleMesh(
        m_renderer.getScene(),
        positions.data(),  // 零拷贝传递（如果未缩放）
        static_cast<uint32_t>(meshView.getVertexCount()),
        indices.data(),
        static_cast<uint32_t>(indices.size() / 3),
        vlrMaterial,
        &vlrMesh
    );
    
    if (result != VLRResult_Success) {
        std::cerr << "[SceneLoader] 创建VLR网格失败" << std::endl;
        return std::nullopt;
    }

    // 返回MeshData用于缓存（如果需要）
    MeshData meshData;
    if (meshView.needsScaling()) {
        meshData.positions = std::move(scaledPositions);
    } else {
        // 即使零拷贝传递给VLR，我们仍需要存储数据
        // 因为Assimp的scene可能被销毁
        meshData.positions.assign(positions.begin(), positions.end());
    }
    meshData.indices = std::move(indices);
    
    m_meshCount++;
    return meshData;
}

std::optional<MaterialData> SceneLoader::extractMaterialData(
    const aiScene* scene, 
    const aiMaterial* material
) const {
    // 获取材质属性
    aiString name;
    material->Get(AI_MATKEY_NAME, name);
    std::cout << "[SceneLoader] 提取材质: " << name.C_Str() << std::endl;

    MaterialData data;

    // 提取基础颜色
    aiColor3D diffuseColor(0.8f, 0.8f, 0.8f);
    material->Get(AI_MATKEY_COLOR_DIFFUSE, diffuseColor);
    data.baseColor[0] = diffuseColor.r;
    data.baseColor[1] = diffuseColor.g;
    data.baseColor[2] = diffuseColor.b;

    // 提取发光颜色
    aiColor3D emissionColor(0.0f, 0.0f, 0.0f);
    material->Get(AI_MATKEY_COLOR_EMISSIVE, emissionColor);
    data.emissionColor[0] = emissionColor.r;
    data.emissionColor[1] = emissionColor.g;
    data.emissionColor[2] = emissionColor.b;

    // 提取PBR参数
    material->Get(AI_MATKEY_METALLIC_FACTOR, data.metallic);
    material->Get(AI_MATKEY_ROUGHNESS_FACTOR, data.roughness);
    
    // 如果没有PBR参数，从传统参数推导
    if (data.roughness == 0.0f) {
        float shininess = 0.0f;
        if (material->Get(AI_MATKEY_SHININESS, shininess) == AI_SUCCESS) {
            // 将shininess转换为roughness (shininess: 0-1000)
            data.roughness = 1.0f - std::sqrt(shininess / 1000.0f);
        } else {
            data.roughness = 0.5f;
        }
    }

    // 检测材质类型
    aiColor3D specularColor(0.0f, 0.0f, 0.0f);
    material->Get(AI_MATKEY_COLOR_SPECULAR, specularColor);
    float specularIntensity = (specularColor.r + specularColor.g + specularColor.b) / 3.0f;

    if (data.metallic > 0.5f || specularIntensity > 0.8f) {
        data.materialType = 1; // Metal
    } else {
        data.materialType = 0; // Matte
    }

    return data;
}

VLRMaterial SceneLoader::createVLRMaterial(const MaterialData& data) {
    VLRMaterial vlrMaterial = nullptr;
    
    // C++20: 使用structured binding和span
    VLRResult result = vlrCreateMaterial(
        m_renderer.getScene(),
        data.materialType,
        data.baseColor,
        data.emissionColor[0] > 0.0f || data.emissionColor[1] > 0.0f || data.emissionColor[2] > 0.0f 
            ? data.emissionColor 
            : nullptr,
        &vlrMaterial
    );
    
    if (result != VLRResult_Success) {
        std::cerr << "[SceneLoader] 创建VLR材质失败" << std::endl;
        return nullptr;
    }

    m_materialCount++;
    m_materials.push_back(vlrMaterial); // 缓存材质句柄
    return vlrMaterial;
}

void SceneLoader::computeBounds() {
    // 重置边界
    constexpr float fmax = std::numeric_limits<float>::max();
    constexpr float fmin = std::numeric_limits<float>::lowest();
    m_boundsMin[0] = m_boundsMin[1] = m_boundsMin[2] = fmax;
    m_boundsMax[0] = m_boundsMax[1] = m_boundsMax[2] = fmin;

    // 遍历所有已加载的网格数据，计算累积边界
    for (const auto& meshData : m_meshes) {
        // 遍历网格的所有顶点位置
        const size_t vertexCount = meshData.positions.size() / 3;
        for (size_t i = 0; i < vertexCount; ++i) {
            // Create a span from the float array for the updateBounds function
            std::span<const float, 3> positionSpan(
                &meshData.positions[i * 3], 3
            );
            updateBounds(positionSpan);
        }
    }

    std::cout << "[SceneLoader] 场景边界: "
              << "Min(" << m_boundsMin[0] << ", " << m_boundsMin[1] << ", " << m_boundsMin[2] << ") "
              << "Max(" << m_boundsMax[0] << ", " << m_boundsMax[1] << ", " << m_boundsMax[2] << ")"
              << std::endl;
}

void SceneLoader::updateBounds(std::span<const float, 3> position) noexcept {
    // C++20: 使用ranges算法
    for (size_t i = 0; i < 3; ++i) {
        m_boundsMin[i] = std::min(m_boundsMin[i], position[i]);
        m_boundsMax[i] = std::max(m_boundsMax[i], position[i]);
    }
}

void SceneLoader::getSceneBounds(float min[3], float max[3]) const {
    std::memcpy(min, m_boundsMin, sizeof(float) * 3);
    std::memcpy(max, m_boundsMax, sizeof(float) * 3);
}

void SceneLoader::getSuggestedCameraPosition(float position[3], float target[3]) const {
    // 计算场景中心
    target[0] = (m_boundsMin[0] + m_boundsMax[0]) * 0.5f;
    target[1] = (m_boundsMin[1] + m_boundsMax[1]) * 0.5f;
    target[2] = (m_boundsMin[2] + m_boundsMax[2]) * 0.5f;

    // 计算场景大小
    float sizeX = m_boundsMax[0] - m_boundsMin[0];
    float sizeY = m_boundsMax[1] - m_boundsMin[1];
    float sizeZ = m_boundsMax[2] - m_boundsMin[2];
    float maxSize = std::max({sizeX, sizeY, sizeZ});

    // 相机位置：在场景前方，距离为场景大小的2倍
    float distance = maxSize * 2.0f;
    position[0] = target[0];
    position[1] = target[1] + maxSize * 0.5f; // 稍微抬高
    position[2] = target[2] + distance;

    std::cout << "[SceneLoader] 建议相机位置: "
              << "Pos(" << position[0] << ", " << position[1] << ", " << position[2] << ") "
              << "Target(" << target[0] << ", " << target[1] << ", " << target[2] << ")"
              << std::endl;
}

// ============================================================================
// 并行加载实现（使用Taskflow）
// ============================================================================

bool SceneLoader::loadSceneParallel(const aiScene* scene, const LoadOptions& options) {
    if (!m_taskflow) {
        std::cerr << "[SceneLoader] Taskflow未初始化" << std::endl;
        return false;
    }
    
    std::cout << "[SceneLoader] 使用并行模式加载..." << std::endl;
    
    if (options.enableTaskGraph) {
        buildTaskGraph(scene, options);
        
        // 如果需要显示任务图，在执行前导出
        if (options.showTaskGraph) {
            std::ofstream ofs("taskgraph_before_exec.dot");
            m_taskflow->taskflow.dump(ofs);
            std::cout << "[SceneLoader] 任务图已导出（执行前）: taskgraph_before_exec.dot" << std::endl;
        }
        
        // 执行任务图
        m_taskflow->executor.run(m_taskflow->taskflow).wait();
    } else {
        // 简单并行处理
        parallelProcessMaterials(scene, options);
        parallelProcessMeshes(scene, options);
    }
    
    // 在所有网格处理完成后计算场景边界
    computeBounds();
    
    return true;
}

void SceneLoader::buildTaskGraph(const aiScene* scene, const LoadOptions& options) {
    std::cout << "[SceneLoader] 构建任务图..." << std::endl;
    
    // 清空之前的任务图
    m_taskflow->taskflow.clear();
    
    // 预分配材质数组（追加模式下保留现有材质）
    size_t materialOffset = options.appendMode ? m_materials.size() : 0;
    if (!options.appendMode) {
        m_materials.clear();
        m_meshes.clear();
        m_meshCount = 0;
        m_materialCount = 0;
    }
    m_materials.resize(materialOffset + scene->mNumMaterials, nullptr);
    
    std::vector<tf::Task> materialTasks;
    std::vector<tf::Task> meshTasks;
    
    // 阶段1: 并行加载所有材质
    materialTasks.reserve(scene->mNumMaterials);
    for (uint32_t i = 0; i < scene->mNumMaterials; ++i) {
        auto task = m_taskflow->taskflow.emplace([this, scene, i, materialOffset]() {
            const aiMaterial* material = scene->mMaterials[i];
            if (auto matData = extractMaterialData(scene, material)) {
                m_materials[materialOffset + i] = createVLRMaterial(*matData);
            }
        }).name(std::string("Material_") + std::to_string(i));
        
        materialTasks.push_back(task);
    }
    
    // 阶段2: 并行处理所有网格（依赖材质）
    meshTasks.reserve(scene->mNumMeshes);
    for (uint32_t i = 0; i < scene->mNumMeshes; ++i) {
        auto task = m_taskflow->taskflow.emplace([this, scene, i, options]() {
            const aiMesh* mesh = scene->mMeshes[i];
            if (auto meshData = processMesh(scene, mesh, options)) {
                // 线程安全地将meshData添加到m_meshes向量
                std::lock_guard<std::mutex> lock(m_meshesMutex);
                m_meshes.push_back(std::move(*meshData));
                m_meshCount++;
            }
        }).name(std::string("Mesh_") + std::to_string(i));
        
        // 网格任务依赖所有材质任务
        for (auto& matTask : materialTasks) {
            matTask.precede(task);
        }
        
        meshTasks.push_back(task);
    }
    
    // 阶段3: 最终化任务
    auto finalizationTask = m_taskflow->taskflow.emplace([this]() {
        std::cout << "[SceneLoader] 最终化场景..." << std::endl;
    }).name("Finalization");
    
    // 所有网格任务完成后执行最终化
    for (auto& meshTask : meshTasks) {
        meshTask.precede(finalizationTask);
    }
    
    m_stats.totalTasks = materialTasks.size() + meshTasks.size() + 1;
    m_stats.parallelTasks = materialTasks.size() + meshTasks.size();
    
    std::cout << "[SceneLoader] 任务图构建完成:" << std::endl;
    std::cout << "  - 材质任务: " << materialTasks.size() << std::endl;
    std::cout << "  - 网格任务: " << meshTasks.size() << std::endl;
    std::cout << "  - 总任务数: " << m_stats.totalTasks << std::endl;
}

void SceneLoader::parallelProcessMeshes(const aiScene* scene, const LoadOptions& options) {
    std::cout << "[SceneLoader] 并行处理网格..." << std::endl;
    
    tf::Taskflow taskflow;
    
    // 仅在非追加模式下清空网格向量
    if (!options.appendMode) {
        m_meshes.clear();
        m_meshCount = 0;
    }
    
    for (uint32_t i = 0; i < scene->mNumMeshes; ++i) {
        taskflow.emplace([this, scene, i, options]() {
            const aiMesh* mesh = scene->mMeshes[i];
            if (auto meshData = processMesh(scene, mesh, options)) {
                // 线程安全地将meshData添加到m_meshes向量
                std::lock_guard<std::mutex> lock(m_meshesMutex);
                m_meshes.push_back(std::move(*meshData));
                m_meshCount++;
            }
        });
    }
    
    m_taskflow->executor.run(taskflow).wait();
}

void SceneLoader::parallelProcessMaterials(const aiScene* scene, const LoadOptions& options) {
    std::cout << "[SceneLoader] 并行处理材质..." << std::endl;
    
    size_t materialOffset = options.appendMode ? m_materials.size() : 0;
    if (!options.appendMode) {
        m_materials.clear();
    }
    m_materials.resize(materialOffset + scene->mNumMaterials);
    
    tf::Taskflow taskflow;
    
    for (uint32_t i = 0; i < scene->mNumMaterials; ++i) {
        taskflow.emplace([this, scene, i, materialOffset]() {
            const aiMaterial* material = scene->mMaterials[i];
            if (auto matData = extractMaterialData(scene, material)) {
                m_materials[materialOffset + i] = createVLRMaterial(*matData);
            }
        });
    }
    
    m_taskflow->executor.run(taskflow).wait();
}

void SceneLoader::exportTaskGraph(const std::string& filename) const {
    if (!m_taskflow) {
        std::cerr << "[SceneLoader] 任务图不可用（未启用并行模式）" << std::endl;
        return;
    }
    
    std::ofstream ofs(filename);
    if (ofs) {
        m_taskflow->taskflow.dump(ofs);
        std::cout << "[SceneLoader] 任务图已导出: " << filename << std::endl;
    }
}

// ============================================================================
// PBRT v4 加载路径
// ============================================================================

bool SceneLoader::loadPbrtScene(const std::string& filepath, const LoadOptions& options) {
    auto startTime = std::chrono::high_resolution_clock::now();

    // 初始化 Taskflow（供 buildFromPbrt 阶段使用）
    if (options.enableParallel && !m_taskflow) {
        m_taskflow = std::make_unique<TaskflowImpl>(options.numThreads);
        std::cout << "[SceneLoader] 并行模式已启用，线程数: "
                  << m_taskflow->executor.num_workers() << std::endl;
    }

    PbrtParser::ParseOptions parseOpts;
    parseOpts.loadPlyFiles = true;
    parseOpts.verbose      = false;
    parseOpts.numThreads   = options.numThreads;

    auto pbrtOpt = PbrtParser::parseFile(filepath, parseOpts);
    if (!pbrtOpt) {
        std::cerr << "[SceneLoader] PBRT 解析失败: " << filepath << std::endl;
        return false;
    }

    bool result = buildFromPbrt(*pbrtOpt, options);

    auto endTime = std::chrono::high_resolution_clock::now();
    m_stats.loadTime = std::chrono::duration<double>(endTime - startTime).count();
    m_stats.threadsUsed = m_taskflow ? m_taskflow->executor.num_workers() : 1u;

    std::cout << "[SceneLoader] PBRT 场景加载耗时: "
              << m_stats.loadTime << "s，"
              << m_meshCount << " 网格，"
              << m_materialCount << " 材质\n";

    return result;
}

// ----------------------------------------------------------------------------
// 将 PbrtMaterial 转换为 VLRMaterial
// ----------------------------------------------------------------------------
VLRMaterial SceneLoader::createVLRMaterialFromPbrt(
    const PbrtMaterial& mat,
    const std::unordered_map<std::string, PbrtTexture>& textures,
    const std::string& baseDir
) {
    MaterialData data;

    // 映射材质类型
    switch (mat.type) {
    case PbrtMaterialType::Conductor:
        data.materialType = 1; // Metal
        data.metallic     = 1.0f;
        if (mat.conductorEta)     { /* 已包含物理 eta，暂映射为高反射率颜色 */ }
        if (mat.conductorK)       { /* k 可用于近似颜色 */ }
        // 用 k 的幅度近似颜色（简化）
        if (mat.conductorK) {
            const auto& k = *mat.conductorK;
            float scale = 1.0f / std::max({k[0], k[1], k[2], 1.f});
            data.baseColor[0] = k[0] * scale;
            data.baseColor[1] = k[1] * scale;
            data.baseColor[2] = k[2] * scale;
        } else {
            data.baseColor[0] = data.baseColor[1] = data.baseColor[2] = 0.8f;
        }
        break;
    case PbrtMaterialType::Dielectric:
        data.materialType = 2; // Glass
        data.ior = mat.dielectricEta.value_or(1.5f);
        data.baseColor[0] = data.baseColor[1] = data.baseColor[2] = 1.0f;
        break;
    case PbrtMaterialType::DiffuseTransmission:
        data.materialType = 2;
        if (mat.reflectance) {
            data.baseColor[0] = (*mat.reflectance)[0];
            data.baseColor[1] = (*mat.reflectance)[1];
            data.baseColor[2] = (*mat.reflectance)[2];
        }
        break;
    default:
        // Diffuse / CoatedDiffuse
        data.materialType = 0;
        if (!mat.reflectanceTexture.empty()) {
            // 纹理引用：暂时使用中灰色占位（VLR 纹理绑定需要额外实现）
            data.baseColor[0] = data.baseColor[1] = data.baseColor[2] = 0.5f;
        } else if (mat.reflectance) {
            data.baseColor[0] = (*mat.reflectance)[0];
            data.baseColor[1] = (*mat.reflectance)[1];
            data.baseColor[2] = (*mat.reflectance)[2];
        } else {
            data.baseColor[0] = data.baseColor[1] = data.baseColor[2] = 0.8f;
        }
        break;
    }

    // 粗糙度
    if (mat.roughness) {
        data.roughness = *mat.roughness;
    } else if (mat.uRoughness && mat.vRoughness) {
        data.roughness = (*mat.uRoughness + *mat.vRoughness) * 0.5f;
    } else {
        data.roughness = 0.5f;
    }

    return createVLRMaterial(data);
}

// ----------------------------------------------------------------------------
// 将完整的 PbrtSceneData 构建为 VLR 场景（Taskflow 并行）
// ----------------------------------------------------------------------------
bool SceneLoader::buildFromPbrt(PbrtSceneData& pbrtScene, const LoadOptions& options) {
    if (!options.appendMode) {
        m_meshes.clear();
        m_materials.clear();
        m_meshCount    = 0;
        m_materialCount = 0;
    }

    const std::string& baseDir = pbrtScene.baseDir;

    // ---- 阶段1：串行构建材质索引表（VLR 材质创建可能不线程安全）----
    // 按名称预构建 name -> VLRMaterial 映射
    std::unordered_map<std::string, VLRMaterial> matMap;
    matMap.reserve(pbrtScene.namedMaterials.size());
    for (auto& [name, mat] : pbrtScene.namedMaterials) {
        VLRMaterial vlrMat = createVLRMaterialFromPbrt(mat, pbrtScene.textures, baseDir);
        if (vlrMat) matMap.emplace(name, vlrMat);
    }

    std::cout << "[SceneLoader] 构建材质完成: " << matMap.size() << " 个\n";

    // ---- 阶段2：并行构建网格（每个 shape 独立，无依赖）----
    size_t shapeCount = pbrtScene.shapes.size();
    m_meshes.resize(m_meshes.size() + shapeCount); // 预分配槽位，写下标不冲突
    size_t meshOffset = m_meshes.size() - shapeCount;

    if (options.enableParallel && m_taskflow) {
        tf::Taskflow taskflow;
        taskflow.clear();

        // 使用 std::atomic 跟踪失败数
        std::atomic<size_t> successCount{0};

        for (size_t i = 0; i < shapeCount; ++i) {
            taskflow.emplace([&, i]() {
                auto& shape = pbrtScene.shapes[i];
                if (shape.type != PbrtShapeType::TriangleMesh) return;

                auto& tm = std::get<PbrtTriangleMesh>(shape.geometry);
                if (tm.positions.empty() || tm.indices.empty()) return;

                // 查找材质
                VLRMaterial vlrMat = nullptr;
                if (!shape.materialName.empty()) {
                    auto it = matMap.find(shape.materialName);
                    if (it != matMap.end()) vlrMat = it->second;
                }

                // 面积光：覆盖材质为发光材质
                if (shape.isAreaLight) {
                    MaterialData emitData;
                    emitData.materialType  = 0;
                    emitData.emissionColor[0] = shape.areaLightL[0];
                    emitData.emissionColor[1] = shape.areaLightL[1];
                    emitData.emissionColor[2] = shape.areaLightL[2];
                    emitData.baseColor[0] = emitData.baseColor[1] = emitData.baseColor[2] = 0.f;
                    // createVLRMaterial 需要锁保护（VLR API 可能非线程安全）
                    {
                        std::lock_guard<std::mutex> lk(m_meshesMutex);
                        vlrMat = createVLRMaterial(emitData);
                    }
                }

                // 创建 VLR 网格
                VLRTriangleMesh vlrMesh = nullptr;
                VLRResult res;
                {
                    // VLR 创建调用加锁
                    std::lock_guard<std::mutex> lk(m_meshesMutex);
                    res = vlrCreateTriangleMesh(
                        m_renderer.getScene(),
                        tm.positions.data(),
                        static_cast<uint32_t>(tm.positions.size() / 3),
                        tm.indices.data(),
                        static_cast<uint32_t>(tm.indices.size() / 3),
                        vlrMat,
                        &vlrMesh
                    );
                }

                if (res != VLRResult_Success) return;

                // 写入预分配槽位（不同 i 对应不同槽位，无需锁）
                MeshData md;
                md.positions = std::move(tm.positions);
                md.normals   = std::move(tm.normals);
                md.uvs       = std::move(tm.uvs);
                md.indices   = std::move(tm.indices);
                m_meshes[meshOffset + i] = std::move(md);
                ++successCount;
            }).name("shape_" + std::to_string(i));
        }

        // 导出任务图
        if (options.showTaskGraph) {
            std::ofstream ofs("pbrt_build_taskgraph.dot");
            taskflow.dump(ofs);
            std::cout << "[SceneLoader] 构建任务图已导出: pbrt_build_taskgraph.dot\n";
        }

        m_taskflow->executor.run(taskflow).wait();
        m_meshCount += successCount.load();

        std::cout << "[SceneLoader] 并行构建完成: " << successCount.load()
                  << "/" << shapeCount << " 网格成功\n";
    } else {
        // 串行回退路径
        size_t successCount = 0;
        for (size_t i = 0; i < shapeCount; ++i) {
            auto& shape = pbrtScene.shapes[i];
            if (shape.type != PbrtShapeType::TriangleMesh) continue;

            auto& tm = std::get<PbrtTriangleMesh>(shape.geometry);
            if (tm.positions.empty() || tm.indices.empty()) continue;

            VLRMaterial vlrMat = nullptr;
            if (!shape.materialName.empty()) {
                auto it = matMap.find(shape.materialName);
                if (it != matMap.end()) vlrMat = it->second;
            }

            VLRTriangleMesh vlrMesh = nullptr;
            VLRResult res = vlrCreateTriangleMesh(
                m_renderer.getScene(),
                tm.positions.data(),
                static_cast<uint32_t>(tm.positions.size() / 3),
                tm.indices.data(),
                static_cast<uint32_t>(tm.indices.size() / 3),
                vlrMat,
                &vlrMesh
            );
            if (res != VLRResult_Success) continue;

            MeshData md;
            md.positions = std::move(tm.positions);
            md.normals   = std::move(tm.normals);
            md.uvs       = std::move(tm.uvs);
            md.indices   = std::move(tm.indices);
            m_meshes[meshOffset + i] = std::move(md);
            ++successCount;
        }
        m_meshCount += successCount;
    }

    // 移除空槽位（加载失败的 shape 留空）
    m_meshes.erase(
        std::remove_if(m_meshes.begin() + static_cast<ptrdiff_t>(meshOffset), m_meshes.end(),
            [](const MeshData& md) { return md.positions.empty(); }),
        m_meshes.end()
    );

    computeBounds();
    return true;
}

} // namespace viewer
