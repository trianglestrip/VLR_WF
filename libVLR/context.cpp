// ============================================================================
// VLR Context 实现
// 
// 本文件实现了 VLR 渲染的 Context 类。
// 
// 作者: VLR 开发团队
// 创建: 2026-03-07
// 环境: CUDA 13.1, OptiX 8.0.0, VS2022
// ============================================================================

#include "context.h"
#include "utils/cuda_util.h"
#include "utils/optix_util.h"
#include <cstring>
#include <stdexcept>

namespace vlr {

// ============================================================================
// 构造函数与析构函数
// ============================================================================

Context::Context(cudaStream_t cudaStream, bool enableLogging)
    : m_stream(cudaStream)
    , m_cudaContext(nullptr)
{
    // 初始化 CUDA 上下文
    m_cudaContext = new cudau::Context();
    
    // 初始化 OptiX 上下文
    m_optix.stream = cudaStream;
    m_optix.enableLogging = enableLogging;
    
    optixu::Context optixContext(enableLogging);
    m_optix.context = optixContext.get();
    
    // 初始化 Wavefront 管线
    initializeWavefrontPipeline();
}

Context::~Context() {
    // 清理 Wavefront 资源
    cleanupWavefrontResources();
    
    // 清理 OptiX 上下文
    if (m_optix.context) {
        optixDeviceContextDestroy(m_optix.context);
        m_optix.context = nullptr;
    }
    
    // 清理 CUDA 上下文
    if (m_cudaContext) {
        delete m_cudaContext;
        m_cudaContext = nullptr;
    }
}


// ============================================================================
// Wavefront 管线初始化
// ============================================================================

void Context::initializeWavefrontPipeline() {
    auto& wf = m_optix.wavefrontPathTracing;
    
    if (wf.isInitialized) {
        return;
    }
    
    // 创建管线编译选项
    OptixPipelineCompileOptions pipelineCompileOptions = {};
    pipelineCompileOptions.usesMotionBlur = false;
    pipelineCompileOptions.traversableGraphFlags = OPTIX_TRAVERSABLE_GRAPH_FLAG_ALLOW_SINGLE_LEVEL_INSTANCING;
    pipelineCompileOptions.numPayloadValues = 7;  // WFTracePayload: 28 字节 = 7 个双字
    pipelineCompileOptions.numAttributeValues = 2;  // 标准三角形属性
    pipelineCompileOptions.exceptionFlags = OPTIX_EXCEPTION_FLAG_NONE;
    pipelineCompileOptions.pipelineLaunchParamsVariableName = "wlp";
    
    // 创建模块编译选项
    OptixModuleCompileOptions moduleCompileOptions = {};
    moduleCompileOptions.maxRegisterCount = OPTIX_COMPILE_DEFAULT_MAX_REGISTER_COUNT;
    moduleCompileOptions.optLevel = OPTIX_COMPILE_OPTIMIZATION_DEFAULT;
    moduleCompileOptions.debugLevel = OPTIX_COMPILE_DEBUG_LEVEL_MINIMAL;
    
    // 注意: PTX 模块将在创建内核文件后加载
    // 目前，我们将管线标记为已初始化但尚不可完全使用
    
    wf.isInitialized = true;
    
    printf("Wavefront pipeline initialized (awaiting PTX modules)\n");
}


void Context::createWavefrontPrograms() {
    // 将在 PTX 文件可用时实现
    // 目前为占位符
}


void Context::createWavefrontSBT() {
    // 将在创建程序时实现
    // 目前为占位符
}


// ============================================================================
// 缓冲区分配
// ============================================================================

void Context::allocateWavefrontBuffers(uint32_t width, uint32_t height) {
    auto& wf = m_optix.wavefrontPathTracing;
    
    uint32_t numPixels = width * height;
    
    // 分配路径状态缓冲区
    if (!wf.pathStateBuffer) {
        wf.pathStateBuffer = new cudau::Buffer<shared::WavefrontPathState>();
    }
    wf.pathStateBuffer->initialize(m_cudaContext, cudau::BufferType::Device, numPixels);
    
    if (!wf.hitInfoBuffer) {
        wf.hitInfoBuffer = new cudau::Buffer<shared::WavefrontHitInfo>();
    }
    wf.hitInfoBuffer->initialize(m_cudaContext, cudau::BufferType::Device, numPixels);
    
    if (!wf.surfacePointBuffer) {
        wf.surfacePointBuffer = new cudau::Buffer<shared::SurfacePoint>();
    }
    wf.surfacePointBuffer->initialize(m_cudaContext, cudau::BufferType::Device, numPixels);
    
    // 分配工作队列
    if (!wf.activePathIndices) {
        wf.activePathIndices = new cudau::Buffer<uint32_t>();
    }
    wf.activePathIndices->initialize(m_cudaContext, cudau::BufferType::Device, numPixels);
    
    if (!wf.nextActivePathIndices) {
        wf.nextActivePathIndices = new cudau::Buffer<uint32_t>();
    }
    wf.nextActivePathIndices->initialize(m_cudaContext, cudau::BufferType::Device, numPixels);
    
    if (!wf.queueCounters) {
        wf.queueCounters = new cudau::Buffer<uint32_t>();
    }
    wf.queueCounters->initialize(m_cudaContext, cudau::BufferType::Device, 2);
    wf.queueCounters->clear(m_stream);
    
    // 分配材质队列（若已启用）
    if (wf.useMaterialQueues) {
        for (int i = 0; i < shared::NumMaterialCategories; ++i) {
            if (!wf.materialQueueIndices[i]) {
                wf.materialQueueIndices[i] = new cudau::Buffer<uint32_t>();
            }
            wf.materialQueueIndices[i]->initialize(m_cudaContext, cudau::BufferType::Device, numPixels);
        }
        
        if (!wf.materialQueueCounters) {
            wf.materialQueueCounters = new cudau::Buffer<uint32_t>();
        }
        wf.materialQueueCounters->initialize(m_cudaContext, cudau::BufferType::Device, shared::NumMaterialCategories);
        wf.materialQueueCounters->clear(m_stream);
    }
    
    // 分配输出缓冲区
    if (!wf.accumBuffer) {
        wf.accumBuffer = new cudau::Buffer<shared::SpectrumStorage>();
    }
    wf.accumBuffer->initialize(m_cudaContext, cudau::BufferType::Device, numPixels);
    wf.accumBuffer->clear(m_stream);
    
    if (!wf.rngBuffer) {
        wf.rngBuffer = new cudau::Buffer<shared::KernelRNG>();
    }
    wf.rngBuffer->initialize(m_cudaContext, cudau::BufferType::Device, numPixels);
    
    // 分配降噪缓冲区（可选）
    if (!wf.accumAlbedoBuffer) {
        wf.accumAlbedoBuffer = new cudau::Buffer<shared::DiscretizedSpectrum>();
    }
    wf.accumAlbedoBuffer->initialize(m_cudaContext, cudau::BufferType::Device, numPixels);
    
    if (!wf.accumNormalBuffer) {
        wf.accumNormalBuffer = new cudau::Buffer<shared::Normal3D>();
    }
    wf.accumNormalBuffer->initialize(m_cudaContext, cudau::BufferType::Device, numPixels);
    
    // 分配性能统计缓冲区
    if (!wf.perfStatsBuffer) {
        wf.perfStatsBuffer = new cudau::Buffer<uint32_t>();
    }
    wf.perfStatsBuffer->initialize(m_cudaContext, cudau::BufferType::Device, 16);
    
    // 更新配置
    wf.maxNumPaths = numPixels;
    wf.currentWidth = width;
    wf.currentHeight = height;
    
    printf("Wavefront buffers allocated: %ux%u (%u paths, ~%.2f MB)\n",
           width, height, numPixels,
           (numPixels * (sizeof(shared::WavefrontPathState) + 
                        sizeof(shared::WavefrontHitInfo) +
                        sizeof(shared::SurfacePoint))) / (1024.0f * 1024.0f));
}


void Context::resizeWavefrontBuffers(uint32_t width, uint32_t height) {
    auto& wf = m_optix.wavefrontPathTracing;
    
    if (wf.currentWidth == width && wf.currentHeight == height) {
        return;  // 无需调整大小
    }
    
    allocateWavefrontBuffers(width, height);
    setupWavefrontLaunchParams();
}


void Context::resetWavefrontQueues() {
    auto& wf = m_optix.wavefrontPathTracing;
    
    if (wf.queueCounters) {
        wf.queueCounters->clear(m_stream);
    }
    
    if (wf.useMaterialQueues && wf.materialQueueCounters) {
        wf.materialQueueCounters->clear(m_stream);
    }
}


// ============================================================================
// 启动参数设置
// ============================================================================

void Context::setupWavefrontLaunchParams() {
    auto& wf = m_optix.wavefrontPathTracing;
    auto& lp = wf.launchParams;
    
    // 设置路径状态缓冲区
    lp.pathStateBuffer = wf.pathStateBuffer ? wf.pathStateBuffer->getDevicePointer() : nullptr;
    lp.hitInfoBuffer = wf.hitInfoBuffer ? wf.hitInfoBuffer->getDevicePointer() : nullptr;
    lp.surfacePointBuffer = wf.surfacePointBuffer ? wf.surfacePointBuffer->getDevicePointer() : nullptr;
    
    // 设置工作队列
    if (wf.activePathIndices && wf.queueCounters) {
        lp.activePathQueue.pathIndices = wf.activePathIndices->getDevicePointer();
        lp.activePathQueue.counter = wf.queueCounters->getDevicePointerAt(0);
        lp.activePathQueue.capacity = wf.maxNumPaths;
    }
    
    if (wf.nextActivePathIndices && wf.queueCounters) {
        lp.nextActivePathQueue.pathIndices = wf.nextActivePathIndices->getDevicePointer();
        lp.nextActivePathQueue.counter = wf.queueCounters->getDevicePointerAt(1);
        lp.nextActivePathQueue.capacity = wf.maxNumPaths;
    }
    
    // 设置材质队列
    if (wf.useMaterialQueues && wf.materialQueueCounters) {
        for (int i = 0; i < shared::NumMaterialCategories; ++i) {
            if (wf.materialQueueIndices[i]) {
                lp.materialQueues.queues[i].pathIndices = wf.materialQueueIndices[i]->getDevicePointer();
                lp.materialQueues.queues[i].counter = wf.materialQueueCounters->getDevicePointerAt(i);
                lp.materialQueues.queues[i].capacity = wf.maxNumPaths;
            }
        }
    }
    
    // 设置输出缓冲区（占位符 - 将来自 BlockBuffer2D）
    // lp.rngBuffer = ...;
    // lp.accumBuffer = ...;
    lp.accumAlbedoBuffer = wf.accumAlbedoBuffer ? wf.accumAlbedoBuffer->getDevicePointer() : nullptr;
    lp.accumNormalBuffer = wf.accumNormalBuffer ? wf.accumNormalBuffer->getDevicePointer() : nullptr;
    
    // 设置图像参数
    lp.imageSize = make_uint2(wf.currentWidth, wf.currentHeight);
    lp.imageStrideInPixels = wf.currentWidth;
    lp.numAccumFrames = 0;
    lp.limitNumAccumFrames = 0;
    
    // 设置 Wavefront 配置
    lp.maxPathLength = wf.maxPathLength;
    lp.maxNumPaths = wf.maxNumPaths;
    lp.currentDepth = 0;
    
    // 设置性能统计指针
    lp.numActiveRays = wf.perfStatsBuffer ? wf.perfStatsBuffer->getDevicePointerAt(0) : nullptr;
    lp.numShadowRays = wf.perfStatsBuffer ? wf.perfStatsBuffer->getDevicePointerAt(1) : nullptr;
    lp.numTerminatedPaths = wf.perfStatsBuffer ? wf.perfStatsBuffer->getDevicePointerAt(2) : nullptr;
    
    // 设置调试参数
    lp.probePixX = -1;
    lp.probePixY = -1;
    lp.debugMode = 0;
    
    // 分配或更新启动参数缓冲区
    if (!wf.launchParamsBuffer) {
        CUDA_CHECK(cudaMalloc(&wf.launchParamsBuffer, sizeof(shared::WavefrontLaunchParameters)));
    }
    
    // 复制到设备
    CUDA_CHECK(cudaMemcpyAsync(
        wf.launchParamsBuffer,
        &lp,
        sizeof(shared::WavefrontLaunchParameters),
        cudaMemcpyHostToDevice,
        m_stream
    ));
}


// ============================================================================
// 清理
// ============================================================================

void Context::cleanupWavefrontResources() {
    auto& wf = m_optix.wavefrontPathTracing;
    
    // 销毁 OptiX 资源
    if (wf.pipeline) {
        optixPipelineDestroy(wf.pipeline);
        wf.pipeline = nullptr;
    }
    
    if (wf.module) {
        optixModuleDestroy(wf.module);
        wf.module = nullptr;
    }
    
    if (wf.raygenProgram) {
        optixProgramGroupDestroy(wf.raygenProgram);
        wf.raygenProgram = nullptr;
    }
    
    if (wf.missProgram) {
        optixProgramGroupDestroy(wf.missProgram);
        wf.missProgram = nullptr;
    }
    
    if (wf.hitGroupProgram) {
        optixProgramGroupDestroy(wf.hitGroupProgram);
        wf.hitGroupProgram = nullptr;
    }
    
    if (wf.shadowMissProgram) {
        optixProgramGroupDestroy(wf.shadowMissProgram);
        wf.shadowMissProgram = nullptr;
    }
    
    if (wf.shadowHitGroupProgram) {
        optixProgramGroupDestroy(wf.shadowHitGroupProgram);
        wf.shadowHitGroupProgram = nullptr;
    }
    
    // 释放 SBT 记录
    if (wf.raygenRecord) {
        cudaFree(wf.raygenRecord);
        wf.raygenRecord = nullptr;
    }
    
    if (wf.missRecord) {
        cudaFree(wf.missRecord);
        wf.missRecord = nullptr;
    }
    
    if (wf.hitgroupRecord) {
        cudaFree(wf.hitgroupRecord);
        wf.hitgroupRecord = nullptr;
    }
    
    if (wf.shadowMissRecord) {
        cudaFree(wf.shadowMissRecord);
        wf.shadowMissRecord = nullptr;
    }
    
    if (wf.shadowHitgroupRecord) {
        cudaFree(wf.shadowHitgroupRecord);
        wf.shadowHitgroupRecord = nullptr;
    }
    
    // 释放缓冲区
    delete wf.pathStateBuffer;
    delete wf.hitInfoBuffer;
    delete wf.surfacePointBuffer;
    delete wf.activePathIndices;
    delete wf.nextActivePathIndices;
    delete wf.queueCounters;
    delete wf.accumBuffer;
    delete wf.accumAlbedoBuffer;
    delete wf.accumNormalBuffer;
    delete wf.rngBuffer;
    delete wf.perfStatsBuffer;
    
    for (int i = 0; i < shared::NumMaterialCategories; ++i) {
        delete wf.materialQueueIndices[i];
        wf.materialQueueIndices[i] = nullptr;
    }
    delete wf.materialQueueCounters;
    
    if (wf.launchParamsBuffer) {
        cudaFree(wf.launchParamsBuffer);
        wf.launchParamsBuffer = nullptr;
    }
    
    wf.isInitialized = false;
}


// ============================================================================
// 渲染方法
// ============================================================================

void Context::render(
    VLRRenderer renderer,
    uint32_t width,
    uint32_t height,
    uint32_t numSamples,
    void* outputBuffer)
{
    switch (renderer) {
        case VLRRenderer_WavefrontPathTracing:
            renderWavefront(width, height, numSamples, outputBuffer);
            break;
            
        case VLRRenderer_PathTracing:
        case VLRRenderer_LightTracing:
        case VLRRenderer_BidirectionalPathTracing:
            throw std::runtime_error("Renderer not yet implemented");
            break;
            
        default:
            throw std::runtime_error("Unknown renderer type");
    }
}


void Context::renderWavefront(
    uint32_t width,
    uint32_t height,
    uint32_t numSamples,
    void* outputBuffer)
{
    auto& wf = m_optix.wavefrontPathTracing;
    
    // 确保缓冲区已分配
    if (wf.currentWidth != width || wf.currentHeight != height) {
        resizeWavefrontBuffers(width, height);
    }
    
    // 设置启动参数
    setupWavefrontLaunchParams();
    
    // 执行渲染
    for (uint32_t sample = 0; sample < numSamples; ++sample) {
        executeWavefrontRender(1);
    }
    
    // 将结果复制到输出缓冲区
    if (outputBuffer && wf.accumBuffer) {
        CUDA_CHECK(cudaMemcpyAsync(
            outputBuffer,
            wf.accumBuffer->getDevicePointer(),
            width * height * sizeof(shared::SpectrumStorage),
            cudaMemcpyDeviceToHost,
            m_stream
        ));
        CUDA_CHECK(cudaStreamSynchronize(m_stream));
    }
}


void Context::executeWavefrontRender(uint32_t numSamples) {
    auto& wf = m_optix.wavefrontPathTracing;
    
    uint32_t numPixels = wf.currentWidth * wf.currentHeight;
    
    // 重置队列
    resetWavefrontQueues();
    
    // 阶段 1: 生成初始光线
    launchGenerateRays(numPixels);
    
        // 主 Wavefront 循环
        for (uint32_t depth = 0; depth < wf.maxPathLength; ++depth) {
        wf.launchParams.currentDepth = depth;
        
        // 获取活跃路径数量
        uint32_t numActivePaths = 0;
        if (wf.queueCounters) {
            wf.queueCounters->copyToHost(&numActivePaths, 1, m_stream);
            CUDA_CHECK(cudaStreamSynchronize(m_stream));
        }
        
        if (numActivePaths == 0) {
            break;  // 所有路径已终止
        }
        
        // 阶段 2: 光线追踪
        launchTraceRays(numActivePaths);
        
        // 阶段 3: 处理命中
        launchProcessHits(numActivePaths);
        
        // 阶段 4: 采样光源 (NEE)
        launchSampleLights(numActivePaths);
        
        // 阶段 5: 采样 BSDF
        launchSampleBSDF(numActivePaths);
        
        // 交换当前队列与下一队列
        std::swap(wf.activePathIndices, wf.nextActivePathIndices);
        
        // 重置下一队列计数
        if (wf.queueCounters) {
            uint32_t zero = 0;
            wf.queueCounters->copyToDevice(&zero, 1, m_stream);
        }
    }
    
    // 阶段 6: 累加结果
    launchAccumulate(numPixels);
    
    CUDA_CHECK(cudaStreamSynchronize(m_stream));
}


// ============================================================================
// 内核启动方法（占位符）
// ============================================================================

void Context::launchGenerateRays(uint32_t numPaths) {
    // 在内核可用时实现
    // 目前为占位符
}

void Context::launchTraceRays(uint32_t numActivePaths) {
    // 在内核可用时实现
}

void Context::launchProcessHits(uint32_t numActivePaths) {
    // 在内核可用时实现
}

void Context::launchSampleLights(uint32_t numActivePaths) {
    // 在内核可用时实现
}

void Context::launchSampleBSDF(uint32_t numActivePaths) {
    // 在内核可用时实现
}

void Context::launchAccumulate(uint32_t numPaths) {
    // 在内核可用时实现
}


// ============================================================================
// 配置方法
// ============================================================================

void Context::setMaxPathLength(uint32_t maxLength) {
    auto& wf = m_optix.wavefrontPathTracing;
    wf.maxPathLength = maxLength;
    wf.launchParams.maxPathLength = maxLength;
}

void Context::setWavefrontConfig(const WavefrontConfig& config) {
    auto& wf = m_optix.wavefrontPathTracing;
    wf.maxPathLength = config.maxPathLength;
    wf.usePathSorting = config.usePathSorting;
    wf.useMaterialQueues = config.useMaterialQueues;
    wf.useStreamCompaction = config.useStreamCompaction;
}


// ============================================================================
// 统计方法
// ============================================================================

const shared::WavefrontPerformanceStats& Context::getPerformanceStats() const {
    return m_optix.wavefrontPathTracing.perfStats;
}

void Context::resetPerformanceStats() {
    m_optix.wavefrontPathTracing.perfStats.reset();
}


// ============================================================================
// 缓冲区管理
// ============================================================================

void Context::resizeOutputBuffer(uint32_t width, uint32_t height) {
    resizeWavefrontBuffers(width, height);
}


// ============================================================================
// 错误检查
// ============================================================================

void Context::checkOptixError(OptixResult result, const char* call, const char* file, int line) {
    if (result != OPTIX_SUCCESS) {
        char msg[1024];
        snprintf(msg, sizeof(msg), "OptiX Error at %s:%d\n  %s\n  Error: %s (%d)",
                 file, line, call, optixGetErrorName(result), result);
        throw std::runtime_error(msg);
    }
}

void Context::checkCudaError(cudaError_t error, const char* call, const char* file, int line) {
    if (error != cudaSuccess) {
        char msg[1024];
        snprintf(msg, sizeof(msg), "CUDA Error at %s:%d\n  %s\n  Error: %s (%d)",
                 file, line, call, cudaGetErrorString(error), error);
        throw std::runtime_error(msg);
    }
}

} // namespace vlr
