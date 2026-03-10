// ============================================================================
// VLRRenderer - VLR渲染器封装类（C++20优化版本）
//
// 功能：封装libVLR的调用，提供简化的渲染接口
// 用途：供测试和场景加载器使用
// 优化：使用std::span、concepts、零拷贝设计
// ============================================================================

#pragma once

#include <vlr/vlr.h>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <span>
#include <concepts>
#include <array>

namespace viewer {

/**
 * @brief VLR渲染器封装类
 * 
 * 提供简化的VLR渲染接口，管理上下文、场景、相机等资源
 */
class VLRRenderer {
public:
    struct Config {
        uint32_t width = 1024;
        uint32_t height = 768;
        uint32_t maxSamples = 1024;
        uint32_t maxBounces = 8;
        bool enableDenoiser = true;
    };

    VLRRenderer();
    ~VLRRenderer();

    // 禁用拷贝
    VLRRenderer(const VLRRenderer&) = delete;
    VLRRenderer& operator=(const VLRRenderer&) = delete;

    /**
     * @brief 初始化渲染器
     * @param config 渲染器配置
     * @return 是否成功
     */
    bool initialize(const Config& config);

    /**
     * @brief 创建新场景
     * @return 场景句柄
     */
    VLRScene createScene();

    /**
     * @brief 设置当前场景
     */
    void setScene(VLRScene scene);

    /**
     * @brief 设置相机（C++20: 使用span避免数组退化）
     * @param position 相机位置
     * @param target 观察目标
     * @param up 上方向
     * @param fov 视野角度（度）
     * @param aspect 宽高比
     * @return 是否成功
     */
    bool setCamera(
        std::span<const float, 3> position,
        std::span<const float, 3> target,
        std::span<const float, 3> up,
        float fov,
        float aspect
    ) noexcept;
    
    // 便捷重载：接受数组
    bool setCamera(
        const float (&position)[3],
        const float (&target)[3],
        const float (&up)[3],
        float fov,
        float aspect
    ) noexcept {
        return setCamera(
            std::span<const float, 3>(position, 3),
            std::span<const float, 3>(target, 3),
            std::span<const float, 3>(up, 3),
            fov,
            aspect
        );
    }

    /**
     * @brief 渲染当前场景（C++20: 使用string_view避免拷贝）
     * @param outputPath 输出图像路径
     * @return 是否成功
     */
    bool render(std::string_view outputPath);

    /**
     * @brief 渲染到缓冲区（C++20: 使用span提供类型安全）
     * @param buffer 输出缓冲区（RGBA float）
     * @param samples 当前采样数
     * @return 是否成功
     */
    bool renderToBuffer(std::span<float> buffer, uint32_t samples) noexcept;

    /**
     * @brief 获取上下文句柄
     */
    [[nodiscard]] VLRContext getContext() const noexcept { return m_context; }

    /**
     * @brief 获取渲染配置
     */
    [[nodiscard]] const Config& getConfig() const noexcept { return m_config; }
    
    /**
     * @brief 获取当前场景
     */
    [[nodiscard]] VLRScene getScene() const noexcept { return m_scene; }

    /**
     * @brief 重置渲染器（清空累积）
     */
    void reset();

    /**
     * @brief 关闭渲染器，释放资源
     */
    void shutdown();

private:
    Config m_config;
    VLRContext m_context = nullptr;
    VLRScene m_scene = nullptr;
    VLRCameraParams m_cameraParams = {};
    bool m_initialized = false;

    // 内部辅助函数
    bool loadPTX();
    void setupDefaultMaterials();
};

} // namespace viewer
