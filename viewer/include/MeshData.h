// ============================================================================
// MeshData - 网格数据容器（C++20优化）
//
// 使用C++20特性优化内存管理和数据传递
// ============================================================================

#pragma once

#include <span>
#include <vector>
#include <memory>
#include <concepts>
#include <ranges>

namespace viewer {

/**
 * @brief 网格数据容器 - 零拷贝设计
 * 
 * 使用std::span避免不必要的数据拷贝
 */
struct MeshData {
    // 使用vector存储，但通过span暴露接口避免拷贝
    std::vector<float> positions;      // [x,y,z, x,y,z, ...]
    std::vector<float> normals;        // [nx,ny,nz, nx,ny,nz, ...]
    std::vector<float> texcoords;      // [u,v, u,v, ...]
    std::vector<uint32_t> indices;     // [i0,i1,i2, i0,i1,i2, ...]
    
    // C++20: 使用span提供零拷贝视图
    [[nodiscard]] std::span<const float> getPositions() const noexcept {
        return positions;
    }
    
    [[nodiscard]] std::span<const float> getNormals() const noexcept {
        return normals;
    }
    
    [[nodiscard]] std::span<const float> getTexcoords() const noexcept {
        return texcoords;
    }
    
    [[nodiscard]] std::span<const uint32_t> getIndices() const noexcept {
        return indices;
    }
    
    // 获取顶点数量
    [[nodiscard]] size_t getVertexCount() const noexcept {
        return positions.size() / 3;
    }
    
    // 获取三角形数量
    [[nodiscard]] size_t getTriangleCount() const noexcept {
        return indices.size() / 3;
    }
    
    // 检查是否有法线
    [[nodiscard]] bool hasNormals() const noexcept {
        return !normals.empty();
    }
    
    // 检查是否有纹理坐标
    [[nodiscard]] bool hasTexcoords() const noexcept {
        return !texcoords.empty();
    }
    
    // 预留空间（避免重分配）
    void reserve(size_t vertexCount, size_t triangleCount) {
        positions.reserve(vertexCount * 3);
        normals.reserve(vertexCount * 3);
        texcoords.reserve(vertexCount * 2);
        indices.reserve(triangleCount * 3);
    }
    
    // 清空数据
    void clear() noexcept {
        positions.clear();
        normals.clear();
        texcoords.clear();
        indices.clear();
    }
    
    // 移动语义（避免拷贝）
    MeshData() = default;
    MeshData(MeshData&&) noexcept = default;
    MeshData& operator=(MeshData&&) noexcept = default;
    
    // 禁用拷贝（强制使用移动或引用）
    MeshData(const MeshData&) = delete;
    MeshData& operator=(const MeshData&) = delete;
};

/**
 * @brief 材质数据容器
 */
struct MaterialData {
    float baseColor[3] = {0.8f, 0.8f, 0.8f};
    float emissionColor[3] = {0.0f, 0.0f, 0.0f};
    float roughness = 0.5f;
    float metallic = 0.0f;
    float ior = 1.5f;
    uint32_t materialType = 0; // 0=Matte
    
    // C++20: 使用designated initializers
    static MaterialData createMatte(std::span<const float, 3> color) {
        MaterialData data;
        std::ranges::copy(color, data.baseColor);
        data.materialType = 0;
        return data;
    }
    
    static MaterialData createMetal(std::span<const float, 3> color, float roughness) {
        MaterialData data;
        std::ranges::copy(color, data.baseColor);
        data.roughness = roughness;
        data.metallic = 1.0f;
        data.materialType = 1;
        return data;
    }
    
    static MaterialData createGlass(float ior, float roughness = 0.0f) {
        MaterialData data;
        data.ior = ior;
        data.roughness = roughness;
        data.materialType = 2;
        return data;
    }
};

/**
 * @brief 批量网格数据（用于批处理优化）
 */
class MeshBatch {
public:
    // C++20: 使用concepts约束
    template<std::ranges::contiguous_range R>
    requires std::same_as<std::ranges::range_value_t<R>, MeshData>
    void addMeshes(R&& meshes) {
        for (auto&& mesh : meshes) {
            m_meshes.push_back(std::move(mesh));
        }
    }
    
    void addMesh(MeshData&& mesh) {
        m_meshes.push_back(std::move(mesh));
    }
    
    [[nodiscard]] std::span<const MeshData> getMeshes() const noexcept {
        return m_meshes;
    }
    
    [[nodiscard]] size_t size() const noexcept {
        return m_meshes.size();
    }
    
    void clear() noexcept {
        m_meshes.clear();
    }

private:
    std::vector<MeshData> m_meshes;
};

} // namespace viewer
