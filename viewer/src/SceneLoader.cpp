// ============================================================================
// SceneLoader 实现（C++20零拷贝优化版本 + Taskflow并行）
// ============================================================================

#include "SceneLoader.h"
#include "MeshData.h"
#include "ZeroCopyMesh.h"
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

    // 其他常用标志
    flags |= aiProcess_JoinIdenticalVertices;
    flags |= aiProcess_ImproveCacheLocality;
    flags |= aiProcess_RemoveRedundantMaterials;
    flags |= aiProcess_SortByPType;

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

    // 计算场景边界
    computeBounds(scene);

    // 递归处理节点树
    processNode(scene, scene->mRootNode, options);

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

void SceneLoader::computeBounds(const aiScene* scene) {
    // 重置边界
    constexpr float fmax = std::numeric_limits<float>::max();
    constexpr float fmin = std::numeric_limits<float>::lowest();
    m_boundsMin[0] = m_boundsMin[1] = m_boundsMin[2] = fmax;
    m_boundsMax[0] = m_boundsMax[1] = m_boundsMax[2] = fmin;

    // C++20: 使用ranges遍历所有网格
    auto meshes = std::span(scene->mMeshes, scene->mNumMeshes);
    for (const auto* mesh : meshes) {
        // C++20: 使用span避免索引访问
        auto vertices = std::span(mesh->mVertices, mesh->mNumVertices);
        for (const auto& pos : vertices) {
            const float position[3] = {pos.x, pos.y, pos.z};
            updateBounds(position);
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
            processMesh(scene, mesh, options);
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
    
    for (uint32_t i = 0; i < scene->mNumMeshes; ++i) {
        taskflow.emplace([this, scene, i, options]() {
            const aiMesh* mesh = scene->mMeshes[i];
            processMesh(scene, mesh, options);
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

} // namespace viewer
