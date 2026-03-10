// ============================================================================
// ZeroCopyMesh - 零拷贝网格数据视图
//
// 设计目标：直接使用Assimp的内存布局，避免数据拷贝
// 使用std::span提供类型安全的零拷贝访问
// ============================================================================

#pragma once

#include <span>
#include <array>
#include <cstdint>
#include <concepts>

// 包含Assimp头文件以获取完整类型定义
#include <assimp/mesh.h>
#include <assimp/vector3.h>

namespace viewer {

/**
 * @brief 验证Assimp数据结构的内存布局
 * 
 * 如果aiVector3D的内存布局与float[3]兼容，我们可以零拷贝访问
 */
class AssimpMemoryLayout {
public:
    // 编译期检查aiVector3D是否可以零拷贝转换为float数组
    static constexpr bool isCompatibleWithFloatArray() noexcept {
        // aiVector3D应该是连续的3个float
        return sizeof(aiVector3D) == sizeof(float) * 3 &&
               alignof(aiVector3D) == alignof(float);
    }
    
    /**
     * @brief 零拷贝转换aiVector3D数组为float span
     * 
     * 前提：aiVector3D内存布局兼容（x,y,z连续存储）
     */
    static std::span<const float> asFloatSpan(std::span<const aiVector3D> vectors) noexcept {
        static_assert(sizeof(aiVector3D) == sizeof(float) * 3, 
                     "aiVector3D必须是3个连续的float");
        
        // 零拷贝：直接reinterpret指针
        return std::span<const float>(
            reinterpret_cast<const float*>(vectors.data()),
            vectors.size() * 3
        );
    }
};

/**
 * @brief 零拷贝网格视图
 * 
 * 直接引用Assimp的内存，不分配新内存
 * 生命周期：必须保证aiMesh在使用期间有效
 */
class ZeroCopyMeshView {
public:
    /**
     * @brief 从aiMesh创建零拷贝视图
     * 
     * @param mesh Assimp网格指针（必须在视图生命周期内有效）
     * @param scale 缩放因子（注意：需要缩放时无法零拷贝）
     */
    explicit ZeroCopyMeshView(const aiMesh* mesh, float scale = 1.0f) noexcept
        : m_mesh(mesh), m_scale(scale) {}
    
    /**
     * @brief 获取顶点位置（零拷贝）
     * 
     * 如果scale=1.0，返回直接指向Assimp内存的span
     * 如果scale!=1.0，调用者需要自行处理缩放
     */
    [[nodiscard]] std::span<const float> getPositions() const noexcept {
        if (!m_mesh || !m_mesh->mVertices) {
            return {};
        }
        
        // 零拷贝：直接转换Assimp的aiVector3D数组为float span
        auto vertices = std::span(m_mesh->mVertices, m_mesh->mNumVertices);
        return AssimpMemoryLayout::asFloatSpan(vertices);
    }
    
    /**
     * @brief 获取法线（零拷贝）
     */
    [[nodiscard]] std::span<const float> getNormals() const noexcept {
        if (!m_mesh || !m_mesh->mNormals) {
            return {};
        }
        
        auto normals = std::span(m_mesh->mNormals, m_mesh->mNumVertices);
        return AssimpMemoryLayout::asFloatSpan(normals);
    }
    
    /**
     * @brief 获取纹理坐标（零拷贝）
     * 
     * 注意：aiVector3D是3D的，但UV只需要2D
     * 这里需要特殊处理
     */
    [[nodiscard]] std::span<const aiVector3D> getTexCoordsRaw() const noexcept {
        if (!m_mesh || !m_mesh->mTextureCoords[0]) {
            return {};
        }
        return std::span(m_mesh->mTextureCoords[0], m_mesh->mNumVertices);
    }
    
    /**
     * @brief 获取索引数据
     * 
     * 注意：Assimp的索引是aiFace结构，需要展平
     * 这是一个无法零拷贝的操作
     */
    [[nodiscard]] bool needsIndexConversion() const noexcept {
        return true; // aiFace需要转换为平坦数组
    }
    
    /**
     * @brief 获取顶点数量
     */
    [[nodiscard]] uint32_t getVertexCount() const noexcept {
        return m_mesh ? m_mesh->mNumVertices : 0;
    }
    
    /**
     * @brief 获取三角形数量
     */
    [[nodiscard]] uint32_t getTriangleCount() const noexcept {
        if (!m_mesh) return 0;
        
        uint32_t count = 0;
        for (uint32_t i = 0; i < m_mesh->mNumFaces; ++i) {
            if (m_mesh->mFaces[i].mNumIndices == 3) {
                ++count;
            }
        }
        return count;
    }
    
    /**
     * @brief 检查是否需要缩放
     */
    [[nodiscard]] bool needsScaling() const noexcept {
        return m_scale != 1.0f;
    }
    
    /**
     * @brief 获取缩放因子
     */
    [[nodiscard]] float getScale() const noexcept {
        return m_scale;
    }
    
    /**
     * @brief 获取原始aiMesh指针
     */
    [[nodiscard]] const aiMesh* getMesh() const noexcept {
        return m_mesh;
    }

private:
    const aiMesh* m_mesh;
    float m_scale;
};

/**
 * @brief 索引提取器 - 最小化拷贝
 * 
 * 将aiFace数组展平为uint32_t数组
 * 这是唯一无法避免的拷贝操作
 */
class IndexExtractor {
public:
    /**
     * @brief 提取三角形索引（最小拷贝）
     * 
     * @param mesh Assimp网格
     * @param output 输出缓冲区（必须足够大）
     * @return 实际提取的索引数量
     */
    static size_t extractTriangleIndices(
        const aiMesh* mesh,
        std::span<uint32_t> output
    ) noexcept {
        if (!mesh || output.empty()) {
            return 0;
        }
        
        size_t index = 0;
        for (uint32_t i = 0; i < mesh->mNumFaces && index + 2 < output.size(); ++i) {
            const auto& face = mesh->mFaces[i];
            if (face.mNumIndices == 3) {
                output[index++] = face.mIndices[0];
                output[index++] = face.mIndices[1];
                output[index++] = face.mIndices[2];
            }
        }
        
        return index;
    }
    
    /**
     * @brief 计算需要的索引缓冲区大小
     */
    static size_t calculateIndexCount(const aiMesh* mesh) noexcept {
        if (!mesh) return 0;
        
        size_t count = 0;
        for (uint32_t i = 0; i < mesh->mNumFaces; ++i) {
            if (mesh->mFaces[i].mNumIndices == 3) {
                count += 3;
            }
        }
        return count;
    }
};

/**
 * @brief 顶点数据转换器 - 处理需要转换的情况
 * 
 * 当需要缩放或格式转换时使用
 */
class VertexTransformer {
public:
    /**
     * @brief 应用缩放变换（原地操作或输出到新缓冲区）
     */
    static void applyScale(
        std::span<const float> input,
        std::span<float> output,
        float scale
    ) noexcept {
        if (scale == 1.0f) {
            // 零拷贝：直接使用输入
            return;
        }
        
        // 需要缩放：使用SIMD优化的循环
        for (size_t i = 0; i < input.size() && i < output.size(); ++i) {
            output[i] = input[i] * scale;
        }
    }
    
    /**
     * @brief 提取UV坐标（从3D转2D）
     * 
     * aiVector3D包含3个分量，但UV只需要2个
     * 这是必须的拷贝操作
     */
    static void extractUV(
        std::span<const aiVector3D> texcoords3D,
        std::span<float> output2D
    ) noexcept {
        const size_t count = std::min(texcoords3D.size() * 2, output2D.size());
        
        for (size_t i = 0, j = 0; j < count; ++i, j += 2) {
            output2D[j]     = texcoords3D[i].x;
            output2D[j + 1] = texcoords3D[i].y;
        }
    }
};

} // namespace viewer
