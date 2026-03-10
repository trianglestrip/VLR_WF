// ============================================================================
// VLRRenderer 实现（C++20优化版本）
// ============================================================================

#include "VLRRenderer.h"
#include <iostream>
#include <fstream>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <ranges>
#include <numbers>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb/stb_image_write.h>

namespace viewer {

VLRRenderer::VLRRenderer() = default;

VLRRenderer::~VLRRenderer() {
    shutdown();
}

bool VLRRenderer::initialize(const Config& config) {
    if (m_initialized) {
        std::cerr << "[VLRRenderer] 已经初始化" << std::endl;
        return true;
    }

    m_config = config;

    // 创建VLR上下文
    VLRResult result = vlrCreateContext(nullptr, 1, &m_context);
    if (result != VLRResult_Success || !m_context) {
        std::cerr << "[VLRRenderer] 创建VLR上下文失败: " << result << std::endl;
        return false;
    }

    std::cout << "[VLRRenderer] VLR上下文创建成功" << std::endl;

    m_initialized = true;
    std::cout << "[VLRRenderer] 初始化成功 (" 
              << config.width << "x" << config.height 
              << ", " << config.maxSamples << " samples)" << std::endl;

    return true;
}

bool VLRRenderer::loadPTX() {
    // PTX在Context创建时自动加载，无需手动加载
    return true;
}

VLRScene VLRRenderer::createScene() {
    if (!m_initialized) {
        std::cerr << "[VLRRenderer] 渲染器未初始化" << std::endl;
        return nullptr;
    }

    VLRScene scene = nullptr;
    VLRResult result = vlrCreateScene(m_context, &scene);
    if (result != VLRResult_Success || !scene) {
        std::cerr << "[VLRRenderer] 创建场景失败: " << result << std::endl;
        return nullptr;
    }

    return scene;
}

void VLRRenderer::setScene(VLRScene scene) {
    m_scene = scene;
}

bool VLRRenderer::setCamera(
    std::span<const float, 3> position,
    std::span<const float, 3> target,
    std::span<const float, 3> up,
    float fov,
    float aspect
) noexcept {
    if (!m_initialized || !m_scene) {
        std::cerr << "[VLRRenderer] 渲染器未初始化或场景未设置" << std::endl;
        return false;
    }

    // C++20: 使用ranges::copy零拷贝赋值
    std::ranges::copy(position, m_cameraParams.position);
    std::ranges::copy(up, m_cameraParams.up);
    
    // C++20: 使用structured binding和算法计算方向
    const auto [px, py, pz] = std::tuple{position[0], position[1], position[2]};
    const auto [tx, ty, tz] = std::tuple{target[0], target[1], target[2]};
    
    std::array<float, 3> dir = {tx - px, ty - py, tz - pz};
    
    // 归一化方向
    float lenSq = 0.0f;
    for (const float v : dir) {
        lenSq += v * v;
    }
    const float len = std::sqrt(lenSq);
    
    if (len > 1e-6f) {
        std::ranges::transform(dir, dir.begin(), [len](float v) { return v / len; });
    }
    std::ranges::copy(dir, m_cameraParams.direction);
    
    // 设置FOV和宽高比（使用C++20常量）
    m_cameraParams.fovY = fov * std::numbers::pi_v<float> / 180.0f;
    m_cameraParams.aspect = aspect;
    m_cameraParams.lensRadius = 0.0f;
    m_cameraParams.focusDistance = len;
    m_cameraParams.focalLength = 0.0f;
    m_cameraParams.cameraType = 0; // 透视相机

    // 应用到场景
    VLRResult result = vlrSetCamera(m_scene, &m_cameraParams);
    if (result != VLRResult_Success) {
        std::cerr << "[VLRRenderer] 设置相机失败: " << result << std::endl;
        return false;
    }

    return true;
}

bool VLRRenderer::render(std::string_view outputPath) {
    if (!m_initialized || !m_scene) {
        std::cerr << "[VLRRenderer] 渲染器或场景未准备好" << std::endl;
        return false;
    }

    // C++20: 使用constexpr计算缓冲区大小
    const size_t bufferSize = m_config.width * m_config.height * 4; // RGBA
    std::vector<float> buffer(bufferSize);

    // 渲染多个采样
    std::cout << "[VLRRenderer] 开始渲染 " << m_config.maxSamples << " 采样..." << std::endl;

    // C++20: 使用views生成采样序列
    for (const auto sample : std::views::iota(0u, m_config.maxSamples)) {
        if (!renderToBuffer(buffer, sample)) {
            std::cerr << "[VLRRenderer] 渲染失败于采样 " << sample << std::endl;
            return false;
        }

        // 每100个采样打印进度
        if ((sample + 1) % 100 == 0) {
            std::cout << "[VLRRenderer] 进度: " << (sample + 1) << "/" << m_config.maxSamples << std::endl;
        }
    }

    // C++20: 使用ranges::transform进行tone mapping和gamma校正
    std::vector<uint8_t> output(bufferSize);
    
    // Tone mapping + Gamma correction pipeline
    constexpr float gamma = 1.0f / 2.2f;
    auto tonemapAndGamma = [gamma](float value) -> uint8_t {
        // Reinhard tone mapping
        value = value / (value + 1.0f);
        // Gamma correction
        value = std::pow(value, gamma);
        // Clamp and convert to 8-bit
        return static_cast<uint8_t>(std::clamp(value * 255.0f, 0.0f, 255.0f));
    };
    
    std::ranges::transform(buffer, output.begin(), tonemapAndGamma);

    // 保存为PNG（string_view需要转换为C字符串）
    const int result = stbi_write_png(
        std::string(outputPath).c_str(),
        static_cast<int>(m_config.width),
        static_cast<int>(m_config.height),
        4,
        output.data(),
        static_cast<int>(m_config.width * 4)
    );

    if (result == 0) {
        std::cerr << "[VLRRenderer] 保存图像失败: " << outputPath << std::endl;
        return false;
    }

    std::cout << "[VLRRenderer] 渲染完成: " << outputPath << std::endl;
    return true;
}

bool VLRRenderer::renderToBuffer(std::span<float> buffer, uint32_t samples) noexcept {
    if (!m_initialized || !m_scene) {
        return false;
    }

    // C++20: 使用span进行边界检查
    const size_t expectedSize = m_config.width * m_config.height * 4;
    if (buffer.size() < expectedSize) {
        std::cerr << "[VLRRenderer] 缓冲区太小: " 
                  << buffer.size() << " < " << expectedSize << std::endl;
        return false;
    }

    // 调用VLR渲染函数（使用Wavefront路径追踪）
    VLRResult result = vlrRender(
        m_context,
        m_scene,
        m_config.width,
        m_config.height,
        samples,
        3 // VLRRenderer_WavefrontPathTracing
    );

    if (result != VLRResult_Success) {
        std::cerr << "[VLRRenderer] 渲染失败: " << result << std::endl;
        return false;
    }

    // 获取输出缓冲区
    void* deviceBuffer = vlrGetOutputBuffer(m_context);
    if (!deviceBuffer) {
        std::cerr << "[VLRRenderer] 获取输出缓冲区失败" << std::endl;
        return false;
    }

    // TODO: 需要从设备缓冲区复制到主机buffer
    // C++20: 使用span.data()获取原始指针
    // cudaMemcpy(buffer.data(), deviceBuffer, expectedSize * sizeof(float), cudaMemcpyDeviceToHost);

    return true;
}

void VLRRenderer::reset() {
    // 重置累积缓冲区
    if (m_scene) {
        // TODO: 调用VLR的重置函数
    }
}

void VLRRenderer::shutdown() {
    if (m_scene) {
        vlrDestroyScene(m_scene);
        m_scene = nullptr;
    }

    if (m_context) {
        vlrDestroyContext(m_context);
        m_context = nullptr;
    }

    m_initialized = false;
    std::cout << "[VLRRenderer] 已关闭" << std::endl;
}

void VLRRenderer::setupDefaultMaterials() {
    // 可以在这里创建一些默认材质
}

} // namespace viewer
