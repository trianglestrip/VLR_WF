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
#include "scene.h"
#include "GPU_kernels/wavefront_launch.h"
#include "utils/cuda_util.h"
#include "utils/optix_util.h"
#include <optix_function_table_definition.h>  // OptiX: 提供 g_optixFunctionTable 定义
#include <cstring>
#include <stdexcept>
#include <fstream>
#include <sstream>
#include <vector>

namespace vlr {

// ============================================================================
// PTX 文件加载辅助
// ============================================================================

namespace {

/// 从 libVLR/GPU_kernels/ 目录加载 PTX 文件内容
/// 尝试多个路径以支持不同构建/运行目录布局
std::vector<char> loadPTXFile(const char* filename) {
    // 候选路径：支持从项目根目录或 libVLR 目录运行
    const char* searchPaths[] = {
        "libVLR/GPU_kernels/",
        "GPU_kernels/",
        "../libVLR/GPU_kernels/",
        "../../libVLR/GPU_kernels/",
    };
    
    for (const char* basePath : searchPaths) {
        std::string path = std::string(basePath) + filename;
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (file.is_open()) {
            std::streamsize size = file.tellg();
            file.seekg(0, std::ios::beg);
            std::vector<char> buffer(static_cast<size_t>(size) + 1);
            if (file.read(buffer.data(), size)) {
                buffer[static_cast<size_t>(size)] = '\0';
                return buffer;
            }
            file.close();
        }
    }
    
    throw std::runtime_error(
        std::string("无法加载 PTX 文件: ") + filename +
        "。请确保文件位于 libVLR/GPU_kernels/ 目录，并已运行 compile_wavefront_ptx.bat 生成 PTX。");
}

}  // 匿名命名空间

// ============================================================================
// 构造函数与析构函数
// ============================================================================

Context::Context(cudaStream_t cudaStream, bool enableLogging)
    : m_stream(cudaStream)
    , m_cudaContext(nullptr)
    , m_sceneSource(nullptr)
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
    auto& wf = m_optix.wavefrontPathTracing;
    
    char logBuffer[2048];
    size_t logSize = sizeof(logBuffer);
    
    auto createProgramGroup = [&](const OptixProgramGroupDesc& desc) -> OptixProgramGroup {
        OptixProgramGroupOptions options = {};
        OptixProgramGroup pg = nullptr;
        OptixResult res = optixProgramGroupCreate(
            m_optix.context,
            &desc,
            1,
            &options,
            logBuffer,
            &logSize,
            &pg
        );
        if (res != OPTIX_SUCCESS) {
            throw std::runtime_error(
                std::string("程序组创建失败: ") + optixGetErrorName(res) +
                "\n日志:\n" + std::string(logBuffer, logSize));
        }
        if (logSize > 1) {
            printf("[OptiX] 程序组日志:\n%.*s\n", static_cast<int>(logSize), logBuffer);
        }
        return pg;
    };
    
    // ========================================================================
    // 1. Ray Generation Program - wavefrontTraceRays
    // 从活跃队列读取路径，发射光线进行求交
    // ========================================================================
    {
        OptixProgramGroupDesc desc = {};
        desc.kind = OPTIX_PROGRAM_GROUP_KIND_RAYGEN;
        desc.raygen.module = wf.module;
        desc.raygen.entryFunctionName = "__raygen__wavefrontTraceRays";
        wf.raygenProgram = createProgramGroup(desc);
    }
    
    // ========================================================================
    // 2. Miss Program - wavefrontMiss
    // 主光线未击中几何体时（命中环境光/天空）
    // ========================================================================
    {
        OptixProgramGroupDesc desc = {};
        desc.kind = OPTIX_PROGRAM_GROUP_KIND_MISS;
        desc.miss.module = wf.module;
        desc.miss.entryFunctionName = "__miss__wavefrontMiss";
        wf.missProgram = createProgramGroup(desc);
    }
    
    // ========================================================================
    // 3. Hit Group - Closest Hit（默认，无 Alpha 测试）
    // wavefrontClosestHit 填充命中信息到 hitInfoBuffer
    // ========================================================================
    {
        OptixProgramGroupDesc desc = {};
        desc.kind = OPTIX_PROGRAM_GROUP_KIND_HITGROUP;
        desc.hitgroup.moduleCH = wf.module;
        desc.hitgroup.entryFunctionNameCH = "__closesthit__wavefrontClosestHit";
        desc.hitgroup.moduleAH = nullptr;
        desc.hitgroup.entryFunctionNameAH = nullptr;
        desc.hitgroup.moduleIS = nullptr;
        desc.hitgroup.entryFunctionNameIS = nullptr;  // 使用内置三角形求交
        wf.hitGroupProgram = createProgramGroup(desc);
    }
    
    // ========================================================================
    // 4. Shadow Miss Program - wavefrontShadowMiss
    // 阴影光线未击中，光源可见
    // ========================================================================
    {
        OptixProgramGroupDesc desc = {};
        desc.kind = OPTIX_PROGRAM_GROUP_KIND_MISS;
        desc.miss.module = wf.module;
        desc.miss.entryFunctionName = "__miss__wavefrontShadowMiss";
        wf.shadowMissProgram = createProgramGroup(desc);
    }
    
    // ========================================================================
    // 5. Shadow Hit Group - wavefrontShadowAnyHit
    // 阴影光线击中几何体，光源被遮挡，立即终止
    // ========================================================================
    {
        OptixProgramGroupDesc desc = {};
        desc.kind = OPTIX_PROGRAM_GROUP_KIND_HITGROUP;
        desc.hitgroup.moduleCH = nullptr;
        desc.hitgroup.entryFunctionNameCH = nullptr;
        desc.hitgroup.moduleAH = wf.module;
        desc.hitgroup.entryFunctionNameAH = "__anyhit__wavefrontShadowAnyHit";
        desc.hitgroup.moduleIS = nullptr;
        desc.hitgroup.entryFunctionNameIS = nullptr;
        wf.shadowHitGroupProgram = createProgramGroup(desc);
    }
    
    printf("Wavefront 程序组创建完成（RayGen、Miss、HitGroup、ShadowMiss、ShadowHitGroup）\n");
}


void Context::createWavefrontSBT() {
    auto& wf = m_optix.wavefrontPathTracing;
    
    // ========================================================================
    // 使用 optixu::createSBTRecord 创建 SBT 记录
    // SBT 布局：RayGen(1) | Miss(2: Closest + Shadow) | HitGroup(2: Closest + Shadow)
    // ========================================================================
    
    // 1. RayGen 记录（无附加数据）
    wf.raygenRecord = optixu::createSBTRecord(wf.raygenProgram);
    
    // 2. Miss 记录 - 需要 2 条（RayType 0: wavefrontMiss, RayType 1: wavefrontShadowMiss）
    wf.missRecord = optixu::createSBTRecord(wf.missProgram);
    wf.shadowMissRecord = optixu::createSBTRecord(wf.shadowMissProgram);
    
    // 3. HitGroup 记录 - 需要 2 条（RayType 0: closest hit, RayType 1: shadow any hit）
    // 同一几何体的不同光线类型使用相邻的 SBT 记录，stride = 2
    wf.hitgroupRecord = optixu::createSBTRecord(wf.hitGroupProgram);
    wf.shadowHitgroupRecord = optixu::createSBTRecord(wf.shadowHitGroupProgram);
    
    // ========================================================================
    // 填充 OptixShaderBindingTable 结构
    // ========================================================================
    memset(&wf.sbt, 0, sizeof(wf.sbt));
    
    // RayGen 区
    wf.sbt.raygenRecord = reinterpret_cast<CUdeviceptr>(wf.raygenRecord);
    
    // Miss 区：2 条记录，stride = OPTIX_SBT_RECORD_HEADER_SIZE（无附加数据时）
    // 将两条 miss 记录紧密排列
    size_t missRecordSize = OPTIX_SBT_RECORD_HEADER_SIZE;
    wf.sbt.missRecordBase = reinterpret_cast<CUdeviceptr>(wf.missRecord);
    wf.sbt.missRecordStrideInBytes = static_cast<uint32_t>(missRecordSize);
    wf.sbt.missRecordCount = 2;  // Closest + Shadow
    
    // 注意：Miss 区需要连续内存存放 [wavefrontMiss, wavefrontShadowMiss]
    // 当前分别分配，需确保布局正确。OptiX 要求 missRecordBase 指向的缓冲区
    // 包含 numRayTypes 条记录。我们分配一个连续的 miss 缓冲区。
    {
        // 分配连续的 2 条 Miss 记录
        size_t totalMissSize = 2 * missRecordSize;
        void* missBuffer = nullptr;
        CUDA_CHECK(cudaMalloc(&missBuffer, totalMissSize));
        
        // 打包两条记录的 header
        OPTIX_CHECK(optixSbtRecordPackHeader(wf.missProgram, missBuffer));
        OPTIX_CHECK(optixSbtRecordPackHeader(
            wf.shadowMissProgram,
            static_cast<char*>(missBuffer) + missRecordSize));
        
        // 释放单独分配的，使用连续缓冲区
        cudaFree(wf.missRecord);
        cudaFree(wf.shadowMissRecord);
        wf.missRecord = missBuffer;
        wf.shadowMissRecord = nullptr;  // 已合并到 missRecord
        
        wf.sbt.missRecordBase = reinterpret_cast<CUdeviceptr>(wf.missRecord);
        wf.sbt.missRecordStrideInBytes = static_cast<uint32_t>(missRecordSize);
        wf.sbt.missRecordCount = 2;
    }
    
    // HitGroup 区：2 条记录（stride = 2 用于多光线类型）
    // 同一几何体：Record 0 = Closest Hit, Record 1 = Shadow Any Hit
    size_t hitgroupRecordSize = OPTIX_SBT_RECORD_HEADER_SIZE;
    {
        // 分配连续的 2 条 HitGroup 记录
        size_t totalHitgroupSize = 2 * hitgroupRecordSize;
        void* hitgroupBuffer = nullptr;
        CUDA_CHECK(cudaMalloc(&hitgroupBuffer, totalHitgroupSize));
        
        OPTIX_CHECK(optixSbtRecordPackHeader(wf.hitGroupProgram, hitgroupBuffer));
        OPTIX_CHECK(optixSbtRecordPackHeader(
            wf.shadowHitGroupProgram,
            static_cast<char*>(hitgroupBuffer) + hitgroupRecordSize));
        
        cudaFree(wf.hitgroupRecord);
        cudaFree(wf.shadowHitgroupRecord);
        wf.hitgroupRecord = hitgroupBuffer;
        wf.shadowHitgroupRecord = nullptr;
        
        wf.sbt.hitgroupRecordBase = reinterpret_cast<CUdeviceptr>(wf.hitgroupRecord);
        wf.sbt.hitgroupRecordStrideInBytes = static_cast<uint32_t>(hitgroupRecordSize);
        wf.sbt.hitgroupRecordCount = 2;
    }
    
    printf("Wavefront SBT 创建完成（RayGen、Miss×2、HitGroup×2）\n");
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
    
    // 设置输出缓冲区
    lp.rngBuffer = optixu::NativeBlockBuffer2D<shared::KernelRNG>();
    lp.rngBuffer.data = wf.rngBuffer ? wf.rngBuffer->getDevicePointer() : nullptr;
    lp.accumBuffer = optixu::BlockBuffer2D<shared::SpectrumStorage, 0>();
    lp.accumBuffer.data = wf.accumBuffer ? wf.accumBuffer->getDevicePointer() : nullptr;
    lp.accumAlbedoBuffer = wf.accumAlbedoBuffer ? wf.accumAlbedoBuffer->getDevicePointer() : nullptr;
    lp.accumNormalBuffer = wf.accumNormalBuffer ? wf.accumNormalBuffer->getDevicePointer() : nullptr;
    
    // 设置场景数据（来自 Scene 或默认空）
    if (m_sceneSource) {
        lp.geomInstBuffer = m_sceneSource->getGeomInstBuffer();
        lp.instBuffer = m_sceneSource->getInstBuffer();
        lp.materialDescriptorBuffer = m_sceneSource->getMaterialBuffer();
        lp.textureDescriptorBuffer = nullptr;       // 纹理描述符（法线贴图等），可由应用设置
        lp.materialNormalMapIndices = nullptr;      // 材质法线贴图索引，可由应用设置
        lp.vertexPositions = m_sceneSource->getVertexPositions();
        lp.vertexNormals = m_sceneSource->getVertexNormals();
        lp.vertexTexCoords = m_sceneSource->getVertexTexCoords();
        lp.topGroup = m_sceneSource->getTopGroup();
        lp.cameraDescriptor = m_sceneSource->getCamera();
        // SceneBounds 需设备指针，上传到小缓冲区
        if (!wf.sceneBoundsBuffer) {
            wf.sceneBoundsBuffer = new cudau::Buffer<shared::SceneBounds>();
        }
        shared::SceneBounds bounds = m_sceneSource->getSceneBounds();
        wf.sceneBoundsBuffer->initialize(m_cudaContext, cudau::BufferType::Device, 1);
        wf.sceneBoundsBuffer->copyToDevice(&bounds, 1, m_stream);
        lp.sceneBounds = wf.sceneBoundsBuffer->getDevicePointer();
    } else {
        lp.geomInstBuffer = nullptr;
        lp.instBuffer = nullptr;
        lp.materialDescriptorBuffer = nullptr;
        lp.textureDescriptorBuffer = nullptr;
        lp.materialNormalMapIndices = nullptr;
        lp.vertexPositions = nullptr;
        lp.vertexNormals = nullptr;
        lp.vertexTexCoords = nullptr;
        lp.topGroup = 0;
        lp.sceneBounds = nullptr;
        lp.cameraDescriptor = m_scene.camera;  // 使用默认 SceneData
    }
    
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
    delete wf.sceneBoundsBuffer;
    wf.sceneBoundsBuffer = nullptr;
    
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
// 场景设置
// ============================================================================

Scene* Context::createScene() {
    return new Scene(m_optix.context, m_stream, m_cudaContext);
}

void Context::destroyScene(Scene* scene) {
    delete scene;
}

void Context::setScene(const Scene* scene) {
    m_sceneSource = scene;
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
    
    // 若有外部场景则构建加速结构并上传到设备
    if (m_sceneSource) {
        const_cast<Scene*>(m_sceneSource)->buildAccelerationStructure();
        const_cast<Scene*>(m_sceneSource)->updateToGPU();
        m_scene.camera = m_sceneSource->getCamera();
        m_scene.bounds = m_sceneSource->getSceneBounds();
    }
    
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
// 内核启动方法
// ============================================================================

void Context::launchGenerateRays(uint32_t numPaths) {
    auto& wf = m_optix.wavefrontPathTracing;
    if (!wf.launchParamsBuffer) return;

    // 根据图像尺寸计算 grid/block，调用 wavefrontGenerateRays CUDA kernel
    uint32_t width = wf.currentWidth;
    uint32_t height = wf.currentHeight;
    if (width == 0 || height == 0) return;

    shared::WavefrontLaunchParameters* d_params =
        static_cast<shared::WavefrontLaunchParameters*>(wf.launchParamsBuffer);

    launchGenerateRaysKernel(d_params, width, height, m_stream);
}

void Context::launchTraceRays(uint32_t numActivePaths) {
    auto& wf = m_optix.wavefrontPathTracing;

    // 使用 optixLaunch 启动 OptiX Ray Generation 程序，传入 SBT 和 launchParams
    if (!wf.pipeline) {
        throw std::runtime_error("launchTraceRays: OptiX pipeline 未初始化，请先调用 createWavefrontPrograms");
    }
    if (!wf.launchParamsBuffer) return;
    if (numActivePaths == 0) return;

    // 将当前深度等参数更新到设备
    setupWavefrontLaunchParams();

    vlr::optixu::launch(
        wf.pipeline,
        m_stream,
        wf.launchParamsBuffer,
        sizeof(shared::WavefrontLaunchParameters),
        &wf.sbt,
        numActivePaths,  // 每个线程处理一条活跃路径
        1,
        1
    );
}

void Context::launchProcessHits(uint32_t numActivePaths) {
    auto& wf = m_optix.wavefrontPathTracing;
    if (!wf.launchParamsBuffer) return;
    if (numActivePaths == 0) return;

    // 调用 wavefrontProcessHits CUDA kernel
    shared::WavefrontLaunchParameters* d_params =
        static_cast<shared::WavefrontLaunchParameters*>(wf.launchParamsBuffer);

    launchProcessHitsKernel(d_params, numActivePaths, m_stream);
}

void Context::launchSampleLights(uint32_t numActivePaths) {
    auto& wf = m_optix.wavefrontPathTracing;
    if (!wf.launchParamsBuffer) return;
    if (numActivePaths == 0) return;

    // 调用 wavefrontSampleLights CUDA kernel
    shared::WavefrontLaunchParameters* d_params =
        static_cast<shared::WavefrontLaunchParameters*>(wf.launchParamsBuffer);

    launchSampleLightsKernel(d_params, numActivePaths, m_stream);
}

void Context::launchSampleBSDF(uint32_t numActivePaths) {
    auto& wf = m_optix.wavefrontPathTracing;
    if (!wf.launchParamsBuffer) return;
    if (numActivePaths == 0) return;

    // 调用 wavefrontSampleBSDF CUDA kernel
    shared::WavefrontLaunchParameters* d_params =
        static_cast<shared::WavefrontLaunchParameters*>(wf.launchParamsBuffer);

    launchSampleBSDFKernel(d_params, numActivePaths, m_stream);
}

void Context::launchAccumulate(uint32_t numPaths) {
    auto& wf = m_optix.wavefrontPathTracing;
    if (!wf.launchParamsBuffer) return;
    if (numPaths == 0) return;

    // 调用 wavefrontAccumulateResults CUDA kernel
    shared::WavefrontLaunchParameters* d_params =
        static_cast<shared::WavefrontLaunchParameters*>(wf.launchParamsBuffer);

    launchAccumulateKernel(d_params, numPaths, m_stream);
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

void* Context::getAccumBufferDevicePointer() const {
    auto& wf = m_optix.wavefrontPathTracing;
    if (wf.accumBuffer && wf.accumBuffer->size() > 0) {
        return wf.accumBuffer->getDevicePointer();
    }
    return nullptr;
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
