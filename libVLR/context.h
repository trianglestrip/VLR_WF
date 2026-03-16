// ============================================================================
// VLR Context 头文件
// 
// 本文件定义了 VLR 渲染的主 Context 类。
// 
// 作者：VLR 开发团队
// 创建日期：2026-03-07
// 环境：CUDA 13.1, OptiX 8.0.0, VS2022
// ============================================================================

#pragma once

#include "include/vlr/public_types.h"
#include "shared/path_types.h"
#include "shared/texture_types.h"
#include "config_loader.h"
#include "denoiser.h"
#include "utils/cuda_util.h"

// 前向声明 Scene（避免循环依赖）
namespace vlr { class Scene; }
#include <cuda_runtime.h>
#ifdef _WIN32
#undef max
#undef min
#endif
#include <optix.h>
#include <array>
#include <memory>
#include <vector>

namespace vlr {

// 前向声明
namespace cudau {
    template<typename T> class Buffer;
    class Context;
}

namespace optixu {
    class Context;
    class Pipeline;
    class Module;
    class Program;
}

// ============================================================================
// Context 类
// ============================================================================

class Context {
public:
    // 构造函数和析构函数
    Context(cudaStream_t cudaStream, bool enableLogging = false);
    ~Context();

    // 禁用拷贝
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;

    // 场景设置（供 C API 使用）
    /// 创建场景（需传入 OptiX/CUDA 资源，由 Context 提供）
    Scene* createScene();
    void destroyScene(Scene* scene);
    void setScene(const Scene* scene);

    // 渲染方法
    void render(
        VLRRenderer renderer,
        uint32_t width,
        uint32_t height,
        uint32_t numSamples,
        void* outputBuffer);

    void renderWavefront(
        uint32_t width,
        uint32_t height,
        uint32_t numSamples,
        void* outputBuffer);
    
    // 降噪器配置
    void setDenoiserConfig(const DenoiserConfig& config);
    const DenoiserConfig& getDenoiserConfig() const;

    // 调试模式与探针像素
    void setDebugMode(VLRDebugMode mode);
    VLRDebugMode getDebugMode() const;
    void setProbePixel(int32_t x, int32_t y);
    void getProbePixel(int32_t* outX, int32_t* outY) const;

    // 缓冲区管理
    void resizeOutputBuffer(uint32_t width, uint32_t height);
    void resizeWavefrontBuffers(uint32_t width, uint32_t height);
    void resetWavefrontQueues();

    /// 获取累积输出缓冲区设备指针（供 vlrGetOutputBuffer 使用）
    void* getAccumBufferDevicePointer() const;

    // 配置
    void setMaxPathLength(uint32_t maxLength);
    void setWavefrontConfig(const WavefrontConfig& config);
    void setWavefrontPathSorting(bool enable);
    void setWavefrontStreamCompaction(bool enable);
    
    /// 设置性能配置（从 INI 加载后应用）
    void setPerformanceConfig(const RuntimePerformanceConfig& config);

    // ------------------------------------------------------------------------
    // 纹理管理
    // ------------------------------------------------------------------------

    /// 创建 2D 纹理（从图像文件加载）
    /// @param imagePath 图像路径（PNG/JPG/EXR/HDR）
    /// @param outTextureIndex 输出纹理索引
    /// @return 成功返回 true
    bool createTexture2D(const char* imagePath, uint32_t* outTextureIndex);

    /// 从内存创建 2D 纹理
    /// @param data 像素数据
    /// @param width 宽度
    /// @param height 高度
    /// @param format 0=RGBA8, 1=RGB32F, 2=RGBA32F
    /// @param outTextureIndex 输出纹理索引
    bool createTexture2DFromMemory(const void* data, uint32_t width, uint32_t height,
                                   uint32_t format, uint32_t* outTextureIndex);

    /// 销毁纹理
    void destroyTexture(uint32_t textureIndex);

    /// 设置纹理滤波模式（0=Nearest, 1=Linear）
    bool setTextureFilterMode(uint32_t textureIndex, uint32_t filterMode);

    /// 设置纹理环绕模式（0=Repeat, 1=Clamp）
    bool setTextureWrapMode(uint32_t textureIndex, uint32_t wrapU, uint32_t wrapV);

    /// 获取纹理描述符（供 Scene/渲染使用）
    const shared::Texture2DDescriptor* getTextureDescriptor(uint32_t textureIndex) const;

    /// 获取纹理描述符数组的设备指针（供 Wavefront 启动参数使用）
    const shared::Texture2DDescriptor* getTextureDescriptorBuffer() const;
    
    // 统计信息
    const shared::WavefrontPerformanceStats& getPerformanceStats() const;
    void resetPerformanceStats();

private:
    // ========================================================================
    // OptiX 资源
    // ========================================================================
    struct OptiX {
        // 通用 OptiX 资源
        OptixDeviceContext context;
        cudaStream_t stream;
        bool enableLogging;

        // ====================================================================
        // Wavefront 路径追踪资源
        // ====================================================================
        struct WavefrontPathTracing {
            // Pipeline 和模块
            OptixPipeline pipeline;
            OptixModule module;
            
            // 程序
            OptixProgramGroup raygenProgram;
            OptixProgramGroup missProgram;
            OptixProgramGroup hitGroupProgram;
            OptixProgramGroup shadowMissProgram;
            OptixProgramGroup shadowHitGroupProgram;
            // LVC-BPT light path programs
            OptixProgramGroup lightRaygenProgram;
            OptixProgramGroup lightHitGroupProgram;
            OptixProgramGroup lightMissProgram;
            // Shadow ray batch program
            OptixProgramGroup shadowRaygenProgram;
            
            // 着色器绑定表
            OptixShaderBindingTable sbt;
            
            // SBT 的设备缓冲区
            void* raygenRecord;
            void* missRecord;
            void* hitgroupRecord;
            void* shadowMissRecord;
            void* shadowHitgroupRecord;
            // LVC-BPT light path SBT records
            void* lightRaygenRecord;
            void* lightMissRecord;
            void* lightHitgroupRecord;
            // Shadow ray batch SBT record
            void* shadowRaygenRecord;
            
            // 路径状态缓冲区
            std::unique_ptr<cudau::Buffer<shared::WavefrontPathState>> pathStateBuffer;
            std::unique_ptr<cudau::Buffer<shared::WavefrontHitInfo>> hitInfoBuffer;
            std::unique_ptr<cudau::Buffer<shared::SurfacePoint>> surfacePointBuffer;
            std::unique_ptr<cudau::Buffer<shared::PathTexturedMaterialParams>> pathTexturedParamsBuffer;
            
            // 工作队列
            std::unique_ptr<cudau::Buffer<uint32_t>> activePathIndices;
            std::unique_ptr<cudau::Buffer<uint32_t>> nextActivePathIndices;
            std::unique_ptr<cudau::Buffer<uint32_t>> queueCounters;  // [0]: 当前活跃, [1]: 下一轮
            
            // 材质队列（可选）
            std::array<std::unique_ptr<cudau::Buffer<uint32_t>>, shared::NumMaterialCategories> materialQueueIndices;
            std::unique_ptr<cudau::Buffer<uint32_t>> materialQueueCounters;
            
            // 输出缓冲区
            std::unique_ptr<cudau::Buffer<shared::SpectrumStorage>> accumBuffer;
            std::unique_ptr<cudau::Buffer<shared::DiscretizedSpectrum>> accumAlbedoBuffer;
            std::unique_ptr<cudau::Buffer<shared::Normal3D>> accumNormalBuffer;
            std::unique_ptr<cudau::Buffer<shared::KernelRNG>> rngBuffer;
            
            // 光源重要性采样（CDF 用于多光源重要性采样）
            std::unique_ptr<cudau::Buffer<float>> lightImportanceWeightsBuffer;
            std::unique_ptr<cudau::Buffer<float>> lightImportanceCDFBuffer;
            
            // 场景设备缓冲区（从 Scene 上传）
            std::unique_ptr<cudau::Buffer<shared::GeometryInstance>> sceneGeomInstBuffer;
            std::unique_ptr<cudau::Buffer<shared::Instance>> sceneInstBuffer;
            std::unique_ptr<cudau::Buffer<shared::SurfaceMaterialDescriptor>> sceneMaterialBuffer;
            std::unique_ptr<cudau::Buffer<shared::Triangle>> sceneTriangleBuffer;
            std::unique_ptr<cudau::Buffer<shared::Point3D>> sceneVertexBuffer;
            std::unique_ptr<cudau::Buffer<uint32_t>> sceneGeomInstIndicesBuffer;  // 打包的 geom 索引
            std::unique_ptr<cudau::Buffer<shared::SceneBounds>> sceneBoundsBuffer;
            
            // 启动参数（主机端副本）
            shared::WavefrontLaunchParameters launchParams;
            
            // 启动参数缓冲区（设备端）
            void* launchParamsBuffer;
            
            // 性能统计
            shared::WavefrontPerformanceStats perfStats;
            std::unique_ptr<cudau::Buffer<uint32_t>> perfStatsBuffer;
            
            // CUDA 事件（用于性能测量）
            cudaEvent_t startEvent;
            cudaEvent_t endEvent;
            bool eventsCreated;
            
            // CUDA Graphs（用于减少 kernel 启动开销）
            cudaGraph_t renderGraph;
            cudaGraphExec_t renderGraphExec;
            bool graphCaptured;
            bool useGraphExecution;
            
            // CUB 临时存储（用于排序和压缩）
            std::unique_ptr<cudau::Buffer<uint8_t>> cubTempStorage;
            size_t cubTempStorageBytes;
            std::unique_ptr<cudau::Buffer<uint32_t>> sortedPathIndices;  // 排序后的路径索引
            std::unique_ptr<cudau::Buffer<uint32_t>> compactedPathIndices;  // 压缩后的路径索引
            std::unique_ptr<cudau::Buffer<uint32_t>> numCompactedPaths;  // CUB 输出的压缩后路径数
            
            // LVC-BPT buffers
            std::unique_ptr<cudau::Buffer<shared::LightPathVertex>> lightVertexCacheBuffer;
            std::unique_ptr<cudau::Buffer<uint32_t>> numLightVerticesBuffer;
            std::unique_ptr<cudau::Buffer<shared::LightPathState>> lightPathStateBuffer;
            std::unique_ptr<cudau::Buffer<shared::WavefrontHitInfo>> lightHitInfoBuffer;
            std::unique_ptr<cudau::Buffer<shared::SurfacePoint>> lightSurfacePointBuffer;
            // Shadow ray batch buffers
            std::unique_ptr<cudau::Buffer<shared::ShadowRayRequest>> shadowRayQueueBuffer;
            std::unique_ptr<cudau::Buffer<float>> shadowRayResultsBuffer;
            std::unique_ptr<cudau::Buffer<uint32_t>> numShadowRayRequestsBuffer;
            // Light path SBT
            OptixShaderBindingTable lightSbt;
            // Shadow ray SBT
            OptixShaderBindingTable shadowSbt;
            bool useBDPT;
            
            // 配置
            uint32_t maxPathLength;
            uint32_t maxNumPaths;
            bool usePathSorting;
            bool useMaterialQueues;
            bool useStreamCompaction;
            
            // 初始化状态
            bool isInitialized;
            uint32_t currentWidth;
            uint32_t currentHeight;
            
            // 累加帧计数（与原始 VLR 一致，用于多采样正确平均）
            uint32_t numAccumFrames;
            
            WavefrontPathTracing()
                : pipeline(nullptr)
                , module(nullptr)
                , raygenProgram(nullptr)
                , missProgram(nullptr)
                , hitGroupProgram(nullptr)
                , shadowMissProgram(nullptr)
                , shadowHitGroupProgram(nullptr)
                , lightRaygenProgram(nullptr)
                , lightHitGroupProgram(nullptr)
                , lightMissProgram(nullptr)
                , shadowRaygenProgram(nullptr)
                , raygenRecord(nullptr)
                , missRecord(nullptr)
                , hitgroupRecord(nullptr)
                , shadowMissRecord(nullptr)
                , shadowHitgroupRecord(nullptr)
                , lightRaygenRecord(nullptr)
                , lightMissRecord(nullptr)
                , lightHitgroupRecord(nullptr)
                , shadowRaygenRecord(nullptr)
                , launchParamsBuffer(nullptr)
                , startEvent(nullptr)
                , endEvent(nullptr)
                , eventsCreated(false)
                , renderGraph(nullptr)
                , renderGraphExec(nullptr)
                , graphCaptured(false)
                , useGraphExecution(true)  // 默认启用 CUDA Graphs
                , cubTempStorageBytes(0)
                , maxPathLength(shared::WavefrontConfig::DefaultMaxPathLength)
                , maxNumPaths(0)
                , useBDPT(true)
                , usePathSorting(shared::WavefrontConfig::UsePathSorting)
                , useMaterialQueues(shared::WavefrontConfig::UseMaterialQueues)
                , useStreamCompaction(shared::WavefrontConfig::UseStreamCompaction)
                , isInitialized(false)
                , currentWidth(0)
                , currentHeight(0)
                , numAccumFrames(0)
            {
                memset(&sbt, 0, sizeof(sbt));
            }
        };
        
        WavefrontPathTracing wavefrontPathTracing;
        
        // 其他渲染器（占位符）
        struct PathTracing {
            OptixPipeline pipeline;
            // ... 其他资源
        };
        
        struct LightTracing {
            OptixPipeline pipeline;
            // ... 其他资源
        };
        
        struct BidirectionalPathTracing {
            OptixPipeline pipeline;
            // ... 其他资源
        };
        
        PathTracing pathTracing;
        LightTracing lightTracing;
        BidirectionalPathTracing bidirectionalPathTracing;
    };
    
    OptiX m_optix;
    
    // ========================================================================
    // CUDA 资源
    // ========================================================================
    cudau::Context* m_cudaContext;
    cudaStream_t m_stream;
    
    // ========================================================================
    // 场景资源（占位符，无外部场景时使用的默认数据）
    // ========================================================================
    struct SceneData {
        // 几何体数据
        std::vector<shared::GeometryInstance> geometryInstances;
        std::vector<shared::Instance> instances;
        
        // 材质数据
        std::vector<shared::SurfaceMaterialDescriptor> materials;
        
        // 光源数据
        std::vector<uint32_t> lightInstances;
        
        // 摄像机
        shared::CameraDescriptor camera;
        
        // 场景边界
        shared::SceneBounds bounds;
    };
    
    SceneData m_scene;
    const ::vlr::Scene* m_sceneSource;  // 外部场景源（setScene 设置）
    
    /// 运行时性能配置（从 INI 加载，供 vlrLoadPerformanceConfig 使用）
    RuntimePerformanceConfig m_perfConfig;
    
    // 降噪器
    Denoiser m_denoiser;
    DenoiserConfig m_denoiserConfig;
    
    // 调试状态
    VLRDebugMode m_debugMode;
    int32_t m_probePixelX;
    int32_t m_probePixelY;

    // ========================================================================
    // 纹理资源
    // ========================================================================
    struct TextureRecord {
        std::unique_ptr<cudau::Buffer<uint8_t>> gpuBuffer;
        shared::Texture2DDescriptor descriptor;
        shared::TextureFilterMode filterMode;
        shared::TextureWrapMode wrapU;
        shared::TextureWrapMode wrapV;
    };
    std::vector<TextureRecord> m_textures;
    mutable std::unique_ptr<cudau::Buffer<shared::Texture2DDescriptor>> m_textureDescriptorBuffer;
    mutable bool m_textureDescriptorBufferDirty = true;

    void updateTextureDescriptorBuffer() const;
    
    // ========================================================================
    // 私有方法
    // ========================================================================
    
    // Wavefront 初始化
    void initializeWavefrontPipeline();
    void createWavefrontPrograms();
    void createWavefrontSBT();
    void allocateWavefrontBuffers(uint32_t width, uint32_t height);
    void setupWavefrontLaunchParams();
    
    // Wavefront 清理
    void cleanupWavefrontResources();
    
    // Wavefront 渲染
    void executeWavefrontRender(uint32_t numSamples);
    void executeWavefrontRenderDebug(uint32_t debugMode);
    void launchGenerateRays(uint32_t numPaths);
    void launchTraceRays(uint32_t numActivePaths);
    void launchTraceLightRays(uint32_t numLightPaths);
    void launchTraceShadowRays(uint32_t numShadowRays);
    void launchProcessHits(uint32_t numActivePaths);
    void launchSampleLights(uint32_t numActivePaths);
    void launchSampleBSDF(uint32_t numActivePaths);
    void launchAccumulate(uint32_t numPaths);
    void launchRenderDebugMode(uint32_t numPixels, uint32_t debugMode);
    
    // 工具方法
    void checkOptixError(OptixResult result, const char* call, const char* file, int line);
    void checkCudaError(cudaError_t error, const char* call, const char* file, int line);
};

// 注意：OPTIX_CHECK 和 CUDA_CHECK 宏在 utils/optix_util.h 和 utils/cuda_util.h 中定义
// 这里不再重复定义以避免警告

} // namespace vlr
