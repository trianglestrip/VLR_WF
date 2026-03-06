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
#include "shared/wavefront_types.h"

// 前向声明 Scene（避免循环依赖）
namespace vlr { class Scene; }
#include <cuda_runtime.h>
#include <optix.h>
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

    // 缓冲区管理
    void resizeOutputBuffer(uint32_t width, uint32_t height);
    void resizeWavefrontBuffers(uint32_t width, uint32_t height);
    void resetWavefrontQueues();

    /// 获取累积输出缓冲区设备指针（供 vlrGetOutputBuffer 使用）
    void* getAccumBufferDevicePointer() const;

    // 配置
    void setMaxPathLength(uint32_t maxLength);
    void setWavefrontConfig(const WavefrontConfig& config);
    
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
            
            // 着色器绑定表
            OptixShaderBindingTable sbt;
            
            // SBT 的设备缓冲区
            void* raygenRecord;
            void* missRecord;
            void* hitgroupRecord;
            void* shadowMissRecord;
            void* shadowHitgroupRecord;
            
            // 路径状态缓冲区
            cudau::Buffer<shared::WavefrontPathState>* pathStateBuffer;
            cudau::Buffer<shared::WavefrontHitInfo>* hitInfoBuffer;
            cudau::Buffer<shared::SurfacePoint>* surfacePointBuffer;
            
            // 工作队列
            cudau::Buffer<uint32_t>* activePathIndices;
            cudau::Buffer<uint32_t>* nextActivePathIndices;
            cudau::Buffer<uint32_t>* queueCounters;  // [0]: 当前活跃, [1]: 下一轮
            
            // 材质队列（可选）
            cudau::Buffer<uint32_t>* materialQueueIndices[shared::NumMaterialCategories];
            cudau::Buffer<uint32_t>* materialQueueCounters;
            
            // 输出缓冲区
            cudau::Buffer<shared::SpectrumStorage>* accumBuffer;
            cudau::Buffer<shared::DiscretizedSpectrum>* accumAlbedoBuffer;
            cudau::Buffer<shared::Normal3D>* accumNormalBuffer;
            cudau::Buffer<shared::KernelRNG>* rngBuffer;
            
            // 场景设备缓冲区（从 Scene 上传）
            cudau::Buffer<shared::GeometryInstance>* sceneGeomInstBuffer;
            cudau::Buffer<shared::Instance>* sceneInstBuffer;
            cudau::Buffer<shared::SurfaceMaterialDescriptor>* sceneMaterialBuffer;
            cudau::Buffer<shared::Triangle>* sceneTriangleBuffer;
            cudau::Buffer<shared::Point3D>* sceneVertexBuffer;
            cudau::Buffer<uint32_t>* sceneGeomInstIndicesBuffer;  // 打包的 geom 索引
            cudau::Buffer<shared::SceneBounds>* sceneBoundsBuffer;
            
            // 启动参数（主机端副本）
            shared::WavefrontLaunchParameters launchParams;
            
            // 启动参数缓冲区（设备端）
            void* launchParamsBuffer;
            
            // 性能统计
            shared::WavefrontPerformanceStats perfStats;
            cudau::Buffer<uint32_t>* perfStatsBuffer;
            
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
            
            WavefrontPathTracing()
                : pipeline(nullptr)
                , module(nullptr)
                , raygenProgram(nullptr)
                , missProgram(nullptr)
                , hitGroupProgram(nullptr)
                , shadowMissProgram(nullptr)
                , shadowHitGroupProgram(nullptr)
                , raygenRecord(nullptr)
                , missRecord(nullptr)
                , hitgroupRecord(nullptr)
                , shadowMissRecord(nullptr)
                , shadowHitgroupRecord(nullptr)
                , pathStateBuffer(nullptr)
                , hitInfoBuffer(nullptr)
                , surfacePointBuffer(nullptr)
                , activePathIndices(nullptr)
                , nextActivePathIndices(nullptr)
                , queueCounters(nullptr)
                , materialQueueCounters(nullptr)
                , accumBuffer(nullptr)
                , accumAlbedoBuffer(nullptr)
                , accumNormalBuffer(nullptr)
                , rngBuffer(nullptr)
                , sceneBoundsBuffer(nullptr)
                , launchParamsBuffer(nullptr)
                , perfStatsBuffer(nullptr)
                , maxPathLength(shared::WavefrontConfig::DefaultMaxPathLength)
                , maxNumPaths(0)
                , usePathSorting(shared::WavefrontConfig::UsePathSorting)
                , useMaterialQueues(shared::WavefrontConfig::UseMaterialQueues)
                , useStreamCompaction(shared::WavefrontConfig::UseStreamCompaction)
                , isInitialized(false)
                , currentWidth(0)
                , currentHeight(0)
            {
                for (int i = 0; i < shared::NumMaterialCategories; ++i) {
                    materialQueueIndices[i] = nullptr;
                }
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
    void launchGenerateRays(uint32_t numPaths);
    void launchTraceRays(uint32_t numActivePaths);
    void launchProcessHits(uint32_t numActivePaths);
    void launchSampleLights(uint32_t numActivePaths);
    void launchSampleBSDF(uint32_t numActivePaths);
    void launchAccumulate(uint32_t numPaths);
    
    // 工具方法
    void checkOptixError(OptixResult result, const char* call, const char* file, int line);
    void checkCudaError(cudaError_t error, const char* call, const char* file, int line);
};

// 辅助宏
#define OPTIX_CHECK(call) checkOptixError(call, #call, __FILE__, __LINE__)
#define CUDA_CHECK(call) checkCudaError(call, #call, __FILE__, __LINE__)

} // namespace vlr
