// ============================================================================
// SceneLoader - 场景加载器（C++20优化版本）
//
// 功能：
//   - 使用 Assimp 加载 OBJ/FBX/GLTF/DAE 等格式
//   - 使用内置 PbrtParser 加载 PBRT v4 格式（.pbrt + .ply）
// 优化：std::span、移动语义、零拷贝、Taskflow 并行
// ============================================================================

#pragma once

#include "VLRRenderer.h"
#include "MeshData.h"
#include "PbrtSceneData.h"
#include <string>
#include <vector>
#include <memory>
#include <span>
#include <optional>
#include <unordered_map>
#include <mutex>

// 前向声明Assimp类型
struct aiScene;
struct aiNode;
struct aiMesh;
struct aiMaterial;

namespace viewer {

/**
 * @brief 场景加载器 - 使用Assimp加载3D模型（支持并行）
 */
class SceneLoader {
public:
    struct LoadOptions {
        bool flipUVs = false;             // 翻转UV坐标
        bool triangulate = false;         // 三角化网格
        bool generateNormals = false;     // 生成法线（如果缺失）
        bool generateSmoothNormals = false; // 生成平滑法线
        bool optimizeMeshes = false;      // 优化网格
        float scale = 1.0f;               // 缩放因子
        
        // 并行加载选项（Taskflow）
        bool enableParallel = true;    // 启用并行加载
        uint32_t numThreads = 0;          // 0 = 自动检测CPU核心数
        bool enableTaskGraph = true;   // 启用任务图优化
        bool showTaskGraph = true;       // 显示任务图（调试用）
        
        // 追加模式（不清空现有网格和材质）
        bool appendMode = true;       // true = 追加到现有场景，false = 清空后加载
    };

    SceneLoader(VLRRenderer& renderer);
    ~SceneLoader();

    /**
     * @brief 从文件加载场景
     * @param filepath 模型文件路径
     * @param options 加载选项
     * @return 是否成功
     */
    bool loadScene(const std::string& filepath, const LoadOptions& options = LoadOptions{});

    /**
     * @brief 加载 PBRT v4 场景（.pbrt 文件）
     * @param filepath .pbrt 文件路径
     * @param options  加载选项（并行配置沿用 LoadOptions）
     * @param outCamera 可选输出：PBRT 相机参数（Transform + FOV）
     * @param outFilm   可选输出：PBRT 胶片参数（分辨率）
     * @return 是否成功
     */
    bool loadPbrtScene(const std::string& filepath,
                       const LoadOptions& options = LoadOptions{},
                       PbrtCamera* outCamera = nullptr,
                       PbrtFilm* outFilm = nullptr);

    /**
     * @brief 获取场景边界盒
     */
    void getSceneBounds(float min[3], float max[3]) const;

    /**
     * @brief 获取建议的相机位置（基于场景边界）
     */
    void getSuggestedCameraPosition(float position[3], float target[3]) const;

    /**
     * @brief 获取加载的网格数量
     */
    [[nodiscard]] size_t getMeshCount() const noexcept { return m_meshCount; }

    /**
     * @brief 获取加载的材质数量
     */
    [[nodiscard]] size_t getMaterialCount() const noexcept { return m_materialCount; }
    
    /**
     * @brief 获取加载的网格数据（零拷贝访问）
     */
    [[nodiscard]] std::span<const MeshData> getMeshes() const noexcept {
        return m_meshes;
    }
    
    /**
     * @brief 获取加载统计信息
     */
    struct Statistics {
        double loadTime = 0.0;        // 加载时间（秒）
        size_t totalTasks = 0;        // 总任务数
        size_t parallelTasks = 0;     // 并行任务数
        uint32_t threadsUsed = 0;     // 使用的线程数
    };
    
    [[nodiscard]] const Statistics& getStatistics() const noexcept {
        return m_stats;
    }
    
    /**
     * @brief 导出任务图为DOT格式（仅在启用任务图时有效）
     */
    void exportTaskGraph(const std::string& filename) const;

protected:
    VLRRenderer& m_renderer;
    std::vector<MeshData> m_meshes;           // 使用移动语义存储
    std::vector<VLRMaterial> m_materials;     // 材质句柄
    size_t m_meshCount = 0;
    size_t m_materialCount = 0;
    float m_boundsMin[3] = {0, 0, 0};
    float m_boundsMax[3] = {0, 0, 0};
    Statistics m_stats;                       // 加载统计
    
    // 线程安全保护
    std::mutex m_meshesMutex;                 // 保护m_meshes向量的线程安全
    
    // Taskflow 并行支持（仅在启用时使用）
    class TaskflowImpl;
    std::unique_ptr<TaskflowImpl> m_taskflow;

    // Assimp场景处理（C++20优化）
    bool processScene(const aiScene* scene, const LoadOptions& options);
    void processNode(const aiScene* scene, const aiNode* node, const LoadOptions& options);
    [[nodiscard]] std::optional<MeshData> processMesh(
        const aiScene* scene, 
        const aiMesh* mesh, 
        const LoadOptions& options
    );
    [[nodiscard]] std::optional<MaterialData> extractMaterialData(
        const aiScene* scene, 
        const aiMaterial* material
    ) const;
    [[nodiscard]] VLRMaterial createVLRMaterial(const MaterialData& data);
    
    // 并行处理（Taskflow - Assimp 路径）
    bool loadSceneParallel(const aiScene* scene, const LoadOptions& options);
    void parallelProcessMaterials(const aiScene* scene, const LoadOptions& options);
    void parallelProcessMeshes(const aiScene* scene, const LoadOptions& options);
    void buildTaskGraph(const aiScene* scene, const LoadOptions& options);

    // PBRT 路径：将 PbrtSceneData 转换为 VLR 场景
    bool buildFromPbrt(PbrtSceneData& pbrtScene, const LoadOptions& options);
    [[nodiscard]] VLRMaterial createVLRMaterialFromPbrt(
        const PbrtMaterial& mat,
        const std::unordered_map<std::string, PbrtTexture>& textures,
        const std::string& baseDir
    );
    
    // 辅助函数（使用span避免拷贝）
    void updateBounds(std::span<const float, 3> position) noexcept;
    void computeBounds();
};

} // namespace viewer
