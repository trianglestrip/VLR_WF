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
#include "image_loader.h"
#include "GPU_kernels/kernel_launch.h"
#include "GPU_kernels/compact.h"
#include "utils/cuda_util.h"
#include "utils/optix_util.h"
#ifdef _WIN32
#undef max
#undef min
#endif
#include <optix_function_table_definition.h>  // OptiX: 提供 g_optixFunctionTable 定义
#include <optix_stack_size.h>                 // OptiX: optixUtilAccumulateStackSizes, optixUtilComputeStackSizes
#include <cstring>
#include <stdexcept>
#include <fstream>
#include <sstream>
#include <vector>
#include <chrono>
#include <memory>
#include "shared/performance_config.h"

namespace vlr {

// ============================================================================
// PTX 文件加载辅助
// ============================================================================

namespace {

/// 从 libVLR/GPU_kernels/ 或 build/Release 目录加载 PTX 文件内容
/// 尝试多个路径以支持不同构建/运行目录布局（含 build/Release 运行时）
std::vector<char> loadPTXFile(const char* filename) {
    // 候选路径：项目根、libVLR、build/Release、build/Debug 等
    const char* searchPaths[] = {
        "bin/GPU_kernels/",                // 统一输出目录
        "GPU_kernels/",                    // build/Release 或 build/Debug 运行时
        "libVLR/GPU_kernels/",
        "../GPU_kernels/",
        "../../GPU_kernels/",
        "../bin/GPU_kernels/",
        "../../bin/GPU_kernels/",
        "Release/GPU_kernels/",
        "Debug/GPU_kernels/",
        "../libVLR/GPU_kernels/",
        "../../libVLR/GPU_kernels/",
        "build/Release/GPU_kernels/",
        "build/Debug/GPU_kernels/",
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
        std::string("Failed to load PTX file: ") + filename +
        ". Ensure the file is in GPU_kernels/ or libVLR/GPU_kernels/; when running from build/Release, PTX should be in build/Release/GPU_kernels/.");
}

}  // 匿名命名空间

// ============================================================================
// 构造函数与析构函数
// ============================================================================

// OptiX 日志回调函数
static void optixLogCallback(unsigned int level, const char* tag, const char* message, void* cbdata) {
    printf("[OptiX][%s] %s\n", tag, message);
}

Context::Context(cudaStream_t cudaStream, bool enableLogging)
    : m_stream(cudaStream)
    , m_cudaContext(nullptr)
    , m_sceneSource(nullptr)
{
    // 初始化 CUDA 上下文
    m_cudaContext = new cudau::Context();
    
    // 初始化降噪器配置
    m_denoiserConfig.enabled = false;
    m_denoiserConfig.useAlbedo = true;
    m_denoiserConfig.useNormal = true;
    m_denoiserConfig.hdrIntensity = 1.0f;
    
    // 初始化调试状态
    m_debugMode = VLRDebugMode_Normal;
    m_probePixelX = -1;
    m_probePixelY = -1;
    
    // 初始化 OptiX 上下文
    m_optix.stream = cudaStream;
    m_optix.enableLogging = enableLogging;
    m_optix.context = nullptr;
    
    // 直接初始化 OptiX，不使用局部 optixu::Context 对象
    OptixResult optixRes = optixInit();
    if (optixRes != OPTIX_SUCCESS) {
        fprintf(stderr, "ERROR: optixInit failed: %d\n", optixRes);
        fflush(stderr);
        throw std::runtime_error("Failed to initialize OptiX");
    }
    
    CUcontext cuContext = nullptr;
    CUresult cuRes = cuCtxGetCurrent(&cuContext);
    if (cuRes != CUDA_SUCCESS || !cuContext) {
        fprintf(stderr, "ERROR: cuCtxGetCurrent failed: %d\n", cuRes);
        fflush(stderr);
        throw std::runtime_error("Failed to get CUDA context");
    }
    
    OptixDeviceContextOptions options = {
        .logCallbackFunction = enableLogging ? &optixLogCallback : nullptr,
        .logCallbackLevel = 4
    };
    
    optixRes = optixDeviceContextCreate(cuContext, &options, &m_optix.context);
    if (optixRes != OPTIX_SUCCESS) {
        fprintf(stderr, "ERROR: optixDeviceContextCreate failed: %d\n", optixRes);
        fflush(stderr);
        throw std::runtime_error("Failed to create OptiX device context");
    }
    
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

    // ------------------------------------------------------------------------
    // 1. 加载 PTX 文件
    // ------------------------------------------------------------------------
    std::vector<char> ptxCode;
    try {
        ptxCode = loadPTXFile("trace_rays.ptx");
    } catch (const std::exception& e) {
        fprintf(stderr, "[VLR] Error: PTX load failed - %s\n", e.what());
        throw;
    }
    
    // ------------------------------------------------------------------------
    // 2. 创建管线编译选项
    // ------------------------------------------------------------------------
    OptixPipelineCompileOptions pipelineCompileOptions = {
        .usesMotionBlur = false,
        .traversableGraphFlags = OPTIX_TRAVERSABLE_GRAPH_FLAG_ALLOW_SINGLE_LEVEL_INSTANCING,
        .numPayloadValues = 7,  // WFTracePayload: 28 字节 = 7 个双字
        .numAttributeValues = 2,  // 标准三角形属性
        .exceptionFlags = OPTIX_EXCEPTION_FLAG_NONE,
        .pipelineLaunchParamsVariableName = "wlp"
    };
    
    // 创建模块编译选项
    OptixModuleCompileOptions moduleCompileOptions = {
        .maxRegisterCount = OPTIX_COMPILE_DEFAULT_MAX_REGISTER_COUNT,
        .optLevel = OPTIX_COMPILE_OPTIMIZATION_DEFAULT,
        .debugLevel = OPTIX_COMPILE_DEBUG_LEVEL_MINIMAL
    };
    
    // ------------------------------------------------------------------------
    // 3. 创建 OptiX 模块
    // ------------------------------------------------------------------------
    char moduleLog[2048];
    size_t moduleLogSize = sizeof(moduleLog);
    
    OptixResult moduleRes = optixModuleCreate(
        m_optix.context,
        &moduleCompileOptions,
        &pipelineCompileOptions,
        ptxCode.data(),
        ptxCode.size() - 1,  // 不含结尾 '\0'
        moduleLog,
        &moduleLogSize,
        &wf.module
    );
    
    if (moduleRes != OPTIX_SUCCESS) {
        fprintf(stderr, "[VLR] Error: optixModuleCreate failed - %s (%d)\n", optixGetErrorName(moduleRes), moduleRes);
        if (moduleLogSize > 1) {
            fprintf(stderr, "[VLR] Module compile log:\n%.*s\n", static_cast<int>(moduleLogSize), moduleLog);
        }
        throw std::runtime_error(
            std::string("OptiX module creation failed: ") + optixGetErrorName(moduleRes) + " (" + std::to_string(moduleRes) + ")");
    }
    printf("[VLR] OptiX module created successfully\n");
    
    // ------------------------------------------------------------------------
    // 4. 创建程序组（RayGen、Miss、HitGroup、ShadowMiss、ShadowHitGroup）
    // ------------------------------------------------------------------------
    try {
        createWavefrontPrograms();
    } catch (const std::exception& e) {
        fprintf(stderr, "[VLR] Error: createWavefrontPrograms failed - %s\n", e.what());
        if (wf.module) {
            optixModuleDestroy(wf.module);
            wf.module = nullptr;
        }
        throw;
    }
    
    // ------------------------------------------------------------------------
    // 5. 创建着色器绑定表 (SBT)
    // ------------------------------------------------------------------------
    try {
        createWavefrontSBT();
    } catch (const std::exception& e) {
        fprintf(stderr, "[VLR] Error: createWavefrontSBT failed - %s\n", e.what());
        cleanupWavefrontResources();
        throw;
    }
    
    // ------------------------------------------------------------------------
    // 6. 创建 Pipeline
    // ------------------------------------------------------------------------
    OptixProgramGroup programGroups[] = {
        wf.raygenProgram,
        wf.missProgram,
        wf.hitGroupProgram,
        wf.shadowMissProgram,
        wf.shadowHitGroupProgram
    };
    const uint32_t numProgramGroups = sizeof(programGroups) / sizeof(programGroups[0]);
    
    OptixPipelineLinkOptions pipelineLinkOptions = {};
    pipelineLinkOptions.maxTraceDepth = 2;  // 主光线 + 阴影光线
    // OptiX 8: OptixPipelineLinkOptions 仅包含 maxTraceDepth，无 debugLevel
    
    char pipelineLog[2048];
    size_t pipelineLogSize = sizeof(pipelineLog);
    OptixResult pipelineRes = optixPipelineCreate(
        m_optix.context,
        &pipelineCompileOptions,
        &pipelineLinkOptions,
        programGroups,
        numProgramGroups,
        pipelineLog,
        &pipelineLogSize,
        &wf.pipeline
    );
    if (pipelineRes != OPTIX_SUCCESS) {
        fprintf(stderr, "[VLR] Error: optixPipelineCreate failed - %s (%d)\n", optixGetErrorName(pipelineRes), pipelineRes);
        if (pipelineLogSize > 1) {
            fprintf(stderr, "[VLR] Pipeline link log:\n%.*s\n", static_cast<int>(pipelineLogSize), pipelineLog);
        }
        cleanupWavefrontResources();
        throw std::runtime_error(
            std::string("OptiX pipeline creation failed: ") + optixGetErrorName(pipelineRes) + " (" + std::to_string(pipelineRes) + ")");
    }
    printf("[VLR] OptiX pipeline created successfully\n");
    
    // 设置栈大小（使用 OptiX 工具计算）
    const uint32_t maxTraceDepth = 2;  // 主光线 + 阴影光线
    OptixStackSizes stackSizes = {};
    for (OptixProgramGroup pg : programGroups) {
        OptixResult accRes = optixUtilAccumulateStackSizes(pg, &stackSizes, wf.pipeline);
        if (accRes != OPTIX_SUCCESS) {
            fprintf(stderr, "[VLR] Warning: optixUtilAccumulateStackSizes failed - %s (%d)\n",
                    optixGetErrorName(accRes), accRes);
        }
    }
    uint32_t directCallableStackSizeFromTraversal = 0;
    uint32_t directCallableStackSizeFromState = 0;
    uint32_t continuationStackSize = 0;
    OptixResult stackRes = optixUtilComputeStackSizes(
        &stackSizes,
        maxTraceDepth,
        0,  // maxCCDepth
        0,  // maxDCDepth
        &directCallableStackSizeFromTraversal,
        &directCallableStackSizeFromState,
        &continuationStackSize
    );
    if (stackRes == OPTIX_SUCCESS) {
        stackRes = optixPipelineSetStackSize(
            wf.pipeline,
            directCallableStackSizeFromTraversal,
            directCallableStackSizeFromState,
            continuationStackSize,
            2  // maxTraversableGraphDepth
        );
    }
    if (stackRes != OPTIX_SUCCESS) {
        fprintf(stderr, "[VLR] Warning: stack size setup failed - %s (%d), may affect rendering\n",
                optixGetErrorName(stackRes), stackRes);
    }
    
    // 初始化 CUDA 事件
    CUDA_CHECK(cudaEventCreate(&wf.startEvent));
    CUDA_CHECK(cudaEventCreate(&wf.endEvent));
    wf.eventsCreated = true;

    // 初始化 CUDA Graphs 状态
    wf.graphCaptured = false;
    wf.useGraphExecution = shared::PerformanceConfig::UseCudaGraphs;
    wf.renderGraph = nullptr;
    wf.renderGraphExec = nullptr;

    wf.isInitialized = true;
    printf("[VLR] Pipeline initialization complete\n");
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
                std::string("Program group creation failed: ") + optixGetErrorName(res) +
                "\nLog:\n" + std::string(logBuffer, logSize));
        }
        if (logSize > 1) {
            printf("[OptiX] Program group log:\n%.*s\n", static_cast<int>(logSize), logBuffer);
        }
        return pg;
    };
    
    // ========================================================================
    // 1. Ray Generation Program - traceRays
    // 从活跃队列读取路径，发射光线进行求交
    // ========================================================================
    {
        OptixProgramGroupDesc desc = {};
        desc.kind = OPTIX_PROGRAM_GROUP_KIND_RAYGEN;
        desc.raygen.module = wf.module;
        desc.raygen.entryFunctionName = "__raygen__traceRays";
        wf.raygenProgram = createProgramGroup(desc);
    }
    
    // ========================================================================
    // 2. Miss Program - miss
    // 主光线未击中几何体时（命中环境光/天空）
    // ========================================================================
    {
        OptixProgramGroupDesc desc = {};
        desc.kind = OPTIX_PROGRAM_GROUP_KIND_MISS;
        desc.miss.module = wf.module;
        desc.miss.entryFunctionName = "__miss__miss";
        wf.missProgram = createProgramGroup(desc);
    }
    
    // ========================================================================
    // 3. Hit Group - Closest Hit（默认，无 Alpha 测试）
    // closestHit 填充命中信息到 hitInfoBuffer
    // ========================================================================
    {
        OptixProgramGroupDesc desc = {};
        desc.kind = OPTIX_PROGRAM_GROUP_KIND_HITGROUP;
        desc.hitgroup.moduleCH = wf.module;
        desc.hitgroup.entryFunctionNameCH = "__closesthit__closestHit";
        desc.hitgroup.moduleAH = nullptr;
        desc.hitgroup.entryFunctionNameAH = nullptr;
        desc.hitgroup.moduleIS = nullptr;
        desc.hitgroup.entryFunctionNameIS = nullptr;  // 使用内置三角形求交
        wf.hitGroupProgram = createProgramGroup(desc);
    }
    
    // ========================================================================
    // 4. Shadow Miss Program - shadowMiss
    // 阴影光线未击中，光源可见
    // ========================================================================
    {
        OptixProgramGroupDesc desc = {};
        desc.kind = OPTIX_PROGRAM_GROUP_KIND_MISS;
        desc.miss.module = wf.module;
        desc.miss.entryFunctionName = "__miss__shadowMiss";
        wf.shadowMissProgram = createProgramGroup(desc);
    }
    
    // ========================================================================
    // 5. Shadow Hit Group - shadowAnyHit
    // 阴影光线击中几何体，光源被遮挡，立即终止
    // ========================================================================
    {
        OptixProgramGroupDesc desc = {};
        desc.kind = OPTIX_PROGRAM_GROUP_KIND_HITGROUP;
        desc.hitgroup.moduleCH = nullptr;
        desc.hitgroup.entryFunctionNameCH = nullptr;
        desc.hitgroup.moduleAH = wf.module;
        desc.hitgroup.entryFunctionNameAH = "__anyhit__shadowAnyHit";
        desc.hitgroup.moduleIS = nullptr;
        desc.hitgroup.entryFunctionNameIS = nullptr;
        wf.shadowHitGroupProgram = createProgramGroup(desc);
    }
    
    printf("[VLR] Program groups created (RayGen, Miss, HitGroup, ShadowMiss, ShadowHitGroup)\n");
}


void Context::createWavefrontSBT() {
    auto& wf = m_optix.wavefrontPathTracing;
    
    // ========================================================================
    // 使用 optixu::createSBTRecord 创建 SBT 记录
    // SBT 布局：RayGen(1) | Miss(2: Closest + Shadow) | HitGroup(2: Closest + Shadow)
    // 每个记录包含 WavefrontSBTData（launch parameters 指针）
    // ========================================================================
    
    // SBT 数据：包含 launch parameters 指针（初始为 nullptr，稍后更新）
    shared::WavefrontSBTData sbtData;
    sbtData.params = nullptr;  // 稍后在 setupWavefrontLaunchParams 中更新
    
    // 1. RayGen 记录（附加 WavefrontSBTData）
    wf.raygenRecord = optixu::createSBTRecord(wf.raygenProgram, sbtData);
    
    // 2. Miss 记录 - 需要 2 条（RayType 0: miss, RayType 1: shadowMiss）
    wf.missRecord = optixu::createSBTRecord(wf.missProgram, sbtData);
    wf.shadowMissRecord = optixu::createSBTRecord(wf.shadowMissProgram, sbtData);
    
    // 3. HitGroup 记录 - 需要 2 条（RayType 0: closest hit, RayType 1: shadow any hit）
    // 同一几何体的不同光线类型使用相邻的 SBT 记录，stride = 2
    wf.hitgroupRecord = optixu::createSBTRecord(wf.hitGroupProgram, sbtData);
    wf.shadowHitgroupRecord = optixu::createSBTRecord(wf.shadowHitGroupProgram, sbtData);
    
    // ========================================================================
    // 填充 OptixShaderBindingTable 结构
    // ========================================================================
    memset(&wf.sbt, 0, sizeof(wf.sbt));
    
    // RayGen 区
    wf.sbt.raygenRecord = reinterpret_cast<CUdeviceptr>(wf.raygenRecord);
    
    // Miss 区：2 条记录，stride = OPTIX_SBT_RECORD_HEADER_SIZE + sizeof(WavefrontSBTData)
    // 将两条 miss 记录紧密排列
    // 注意：SBT 记录 stride 必须是 16 字节对齐
    size_t missRecordSize = OPTIX_SBT_RECORD_HEADER_SIZE + sizeof(shared::WavefrontSBTData);
    missRecordSize = (missRecordSize + 15) & ~15;  // 向上对齐到 16 字节
    wf.sbt.missRecordBase = reinterpret_cast<CUdeviceptr>(wf.missRecord);
    wf.sbt.missRecordStrideInBytes = static_cast<uint32_t>(missRecordSize);
    wf.sbt.missRecordCount = 2;  // Closest + Shadow
    
    // 注意：Miss 区需要连续内存存放 [miss, shadowMiss]
    // 当前分别分配，需确保布局正确。OptiX 要求 missRecordBase 指向的缓冲区
    // 包含 numRayTypes 条记录。我们分配一个连续的 miss 缓冲区。
    {
        // 分配连续的 2 条 Miss 记录
        size_t totalMissSize = 2 * missRecordSize;
        void* missBuffer = nullptr;
        CUDA_CHECK(cudaMalloc(&missBuffer, totalMissSize));
        
        // 使用主机内存打包header + data，然后复制到设备
        void* hostMissBuffer = malloc(totalMissSize);
        if (!hostMissBuffer) {
            cudaFree(missBuffer);
            throw std::runtime_error("Failed to allocate host memory for Miss SBT");
        }
        
        OPTIX_CHECK(optixSbtRecordPackHeader(wf.missProgram, hostMissBuffer));
        memcpy(static_cast<char*>(hostMissBuffer) + OPTIX_SBT_RECORD_HEADER_SIZE, &sbtData, sizeof(sbtData));
        
        OPTIX_CHECK(optixSbtRecordPackHeader(
            wf.shadowMissProgram,
            static_cast<char*>(hostMissBuffer) + missRecordSize));
        memcpy(static_cast<char*>(hostMissBuffer) + missRecordSize + OPTIX_SBT_RECORD_HEADER_SIZE,
               &sbtData, sizeof(sbtData));
        
        // 复制到设备
        CUDA_CHECK(cudaMemcpy(missBuffer, hostMissBuffer, totalMissSize, cudaMemcpyHostToDevice));
        free(hostMissBuffer);
        
        // 释放单独分配的，使用连续缓冲区
        cudaFree(wf.missRecord);
        cudaFree(wf.shadowMissRecord);
        wf.missRecord = missBuffer;
        wf.shadowMissRecord = nullptr;  // 已合并到 missRecord
        
        wf.sbt.missRecordBase = reinterpret_cast<CUdeviceptr>(wf.missRecord);
        wf.sbt.missRecordStrideInBytes = static_cast<uint32_t>(missRecordSize);
        wf.sbt.missRecordCount = 2;
    }
    
    // HitGroup 区：每个 GAS 需要 RAY_TYPE_COUNT 条记录
    // 布局：[GAS0_Closest, GAS0_Shadow, GAS1_Closest, GAS1_Shadow, ...]
    // stride = RAY_TYPE_COUNT（用于多光线类型）
    // 注意：SBT 记录 stride 必须是 16 字节对齐
    const uint32_t RAY_TYPE_COUNT = 2;  // Closest Hit + Shadow
    size_t hitgroupRecordSize = OPTIX_SBT_RECORD_HEADER_SIZE + sizeof(shared::WavefrontSBTData);
    hitgroupRecordSize = (hitgroupRecordSize + 15) & ~15;  // 向上对齐到 16 字节
    
    // 临时：在 SBT 创建时，我们还不知道有多少个 GAS
    // 所以先创建一个默认的 HitGroup 记录，稍后在 setupWavefrontLaunchParams 中重新创建
    {
        // 分配连续的 RAY_TYPE_COUNT 条 HitGroup 记录（默认为 1 个 GAS）
        size_t totalHitgroupSize = RAY_TYPE_COUNT * hitgroupRecordSize;
        void* hitgroupBuffer = nullptr;
        CUDA_CHECK(cudaMalloc(&hitgroupBuffer, totalHitgroupSize));
        
        // 使用主机内存打包header + data
        void* hostHitgroupBuffer = malloc(totalHitgroupSize);
        if (!hostHitgroupBuffer) {
            cudaFree(hitgroupBuffer);
            throw std::runtime_error("Failed to allocate host memory for HitGroup SBT");
        }
        
        OPTIX_CHECK(optixSbtRecordPackHeader(wf.hitGroupProgram, hostHitgroupBuffer));
        memcpy(static_cast<char*>(hostHitgroupBuffer) + OPTIX_SBT_RECORD_HEADER_SIZE, &sbtData, sizeof(sbtData));
        
        OPTIX_CHECK(optixSbtRecordPackHeader(
            wf.shadowHitGroupProgram,
            static_cast<char*>(hostHitgroupBuffer) + hitgroupRecordSize));
        memcpy(static_cast<char*>(hostHitgroupBuffer) + hitgroupRecordSize + OPTIX_SBT_RECORD_HEADER_SIZE,
               &sbtData, sizeof(sbtData));
        
        // 复制到设备
        CUDA_CHECK(cudaMemcpy(hitgroupBuffer, hostHitgroupBuffer, totalHitgroupSize, cudaMemcpyHostToDevice));
        free(hostHitgroupBuffer);
        
        cudaFree(wf.hitgroupRecord);
        cudaFree(wf.shadowHitgroupRecord);
        wf.hitgroupRecord = hitgroupBuffer;
        wf.shadowHitgroupRecord = nullptr;
        
        wf.sbt.hitgroupRecordBase = reinterpret_cast<CUdeviceptr>(wf.hitgroupRecord);
        wf.sbt.hitgroupRecordStrideInBytes = static_cast<uint32_t>(hitgroupRecordSize);
        wf.sbt.hitgroupRecordCount = RAY_TYPE_COUNT;  // 默认 1 个 GAS × 2 个 Ray Types
    }
    
    printf("[VLR] SBT created (RayGen, Miss x2, HitGroup x2)\n");
}


// ============================================================================
// 缓冲区分配
// ============================================================================

void Context::allocateWavefrontBuffers(uint32_t width, uint32_t height) {
    auto& wf = m_optix.wavefrontPathTracing;
    
    uint32_t numPixels = width * height;
    
    // 分配路径状态缓冲区
    if (!wf.pathStateBuffer) {
        wf.pathStateBuffer = std::make_unique<cudau::Buffer<shared::WavefrontPathState>>();
    }
    wf.pathStateBuffer->initialize(m_cudaContext, cudau::BufferType::Device, numPixels);
    
    if (!wf.hitInfoBuffer) {
        wf.hitInfoBuffer = std::make_unique<cudau::Buffer<shared::WavefrontHitInfo>>();
    }
    wf.hitInfoBuffer->initialize(m_cudaContext, cudau::BufferType::Device, numPixels);
    
    if (!wf.surfacePointBuffer) {
        wf.surfacePointBuffer = std::make_unique<cudau::Buffer<shared::SurfacePoint>>();
    }
    wf.surfacePointBuffer->initialize(m_cudaContext, cudau::BufferType::Device, numPixels);

    if (!wf.pathTexturedParamsBuffer) {
        wf.pathTexturedParamsBuffer = std::make_unique<cudau::Buffer<shared::PathTexturedMaterialParams>>();
    }
    wf.pathTexturedParamsBuffer->initialize(m_cudaContext, cudau::BufferType::Device, numPixels);
    
    // 分配工作队列
    if (!wf.activePathIndices) {
        wf.activePathIndices = std::make_unique<cudau::Buffer<uint32_t>>();
    }
    wf.activePathIndices->initialize(m_cudaContext, cudau::BufferType::Device, numPixels);
    
    if (!wf.nextActivePathIndices) {
        wf.nextActivePathIndices = std::make_unique<cudau::Buffer<uint32_t>>();
    }
    wf.nextActivePathIndices->initialize(m_cudaContext, cudau::BufferType::Device, numPixels);
    
    if (!wf.queueCounters) {
        wf.queueCounters = std::make_unique<cudau::Buffer<uint32_t>>();
    }
    wf.queueCounters->initialize(m_cudaContext, cudau::BufferType::Device, 2);
    wf.queueCounters->clear(m_stream);
    
    // 分配材质队列（若已启用）
    if (wf.useMaterialQueues) {
        for (int i = 0; i < shared::NumMaterialCategories; ++i) {
            if (!wf.materialQueueIndices[i]) {
                wf.materialQueueIndices[i] = std::make_unique<cudau::Buffer<uint32_t>>();
            }
            wf.materialQueueIndices[i]->initialize(m_cudaContext, cudau::BufferType::Device, numPixels);
        }
        
        if (!wf.materialQueueCounters) {
            wf.materialQueueCounters = std::make_unique<cudau::Buffer<uint32_t>>();
        }
        wf.materialQueueCounters->initialize(m_cudaContext, cudau::BufferType::Device, shared::NumMaterialCategories);
        wf.materialQueueCounters->clear(m_stream);
    }
    
    // 分配输出缓冲区
    if (!wf.accumBuffer) {
        wf.accumBuffer = std::make_unique<cudau::Buffer<shared::SpectrumStorage>>();
    }
    wf.accumBuffer->initialize(m_cudaContext, cudau::BufferType::Device, numPixels);
    wf.accumBuffer->clear(m_stream);
    
    if (!wf.rngBuffer) {
        wf.rngBuffer = std::make_unique<cudau::Buffer<shared::KernelRNG>>();
    }
    wf.rngBuffer->initialize(m_cudaContext, cudau::BufferType::Device, numPixels);
    
    // 初始化 RNG 缓冲区（为每个像素生成唯一的随机种子）
    // 使用当前时间戳作为基础种子，确保每次运行都不同
    uint64_t baseSeed = static_cast<uint64_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count());
    initializeRNGBuffer(wf.rngBuffer->getDevicePointer(), numPixels, baseSeed, m_stream);
    
    // 创建性能测量事件
    if (!wf.eventsCreated) {
        CUDA_CHECK(cudaEventCreate(&wf.startEvent));
        CUDA_CHECK(cudaEventCreate(&wf.endEvent));
        wf.eventsCreated = true;
    }
    
    // 分配降噪缓冲区（可选）
    if (!wf.accumAlbedoBuffer) {
        wf.accumAlbedoBuffer = std::make_unique<cudau::Buffer<shared::DiscretizedSpectrum>>();
    }
    wf.accumAlbedoBuffer->initialize(m_cudaContext, cudau::BufferType::Device, numPixels);
    
    if (!wf.accumNormalBuffer) {
        wf.accumNormalBuffer = std::make_unique<cudau::Buffer<shared::Normal3D>>();
    }
    wf.accumNormalBuffer->initialize(m_cudaContext, cudau::BufferType::Device, numPixels);
    
    // 分配性能统计缓冲区
    if (!wf.perfStatsBuffer) {
        wf.perfStatsBuffer = std::make_unique<cudau::Buffer<uint32_t>>();
    }
    wf.perfStatsBuffer->initialize(m_cudaContext, cudau::BufferType::Device, 16);
    
    // 分配 CUB 临时存储（用于排序和压缩）
    if (wf.usePathSorting || wf.useStreamCompaction) {
        // 查询所需临时存储大小
        size_t sortBytes = 0;
        size_t compactBytes = 0;
        
        if (wf.usePathSorting) {
            sortBytes = shared::sortPathsByMaterialTempStorageBytes(numPixels);
            printf("[VLR] CUB sort temp storage: %.2f KB\n", sortBytes / 1024.0f);
        }
        
        if (wf.useStreamCompaction) {
            compactBytes = shared::compactPathsCUBTempStorageBytes(numPixels);
            printf("[VLR] CUB compact temp storage: %.2f KB\n", compactBytes / 1024.0f);
        }
        
        // 取最大值（两个操作不会同时使用临时存储）
        // 优化：增加额外的缓冲区以避免频繁的 fallback
        wf.cubTempStorageBytes = static_cast<size_t>(
            std::max(sortBytes, compactBytes) * shared::PerformanceConfig::CubTempStorageMultiplier);
        
        printf("[VLR] CUB temp storage allocated: %.2f KB (multiplier: %.1fx)\n", 
               wf.cubTempStorageBytes / 1024.0f, 
               shared::PerformanceConfig::CubTempStorageMultiplier);
        
        if (wf.cubTempStorageBytes > 0) {
            if (!wf.cubTempStorage) {
                wf.cubTempStorage = std::make_unique<cudau::Buffer<uint8_t>>();
            }
            wf.cubTempStorage->initialize(m_cudaContext, cudau::BufferType::Device, wf.cubTempStorageBytes);
            
            // 分配排序/压缩辅助缓冲区
            if (!wf.sortedPathIndices) {
                wf.sortedPathIndices = std::make_unique<cudau::Buffer<uint32_t>>();
            }
            wf.sortedPathIndices->initialize(m_cudaContext, cudau::BufferType::Device, numPixels);
            
            if (!wf.compactedPathIndices) {
                wf.compactedPathIndices = std::make_unique<cudau::Buffer<uint32_t>>();
            }
            wf.compactedPathIndices->initialize(m_cudaContext, cudau::BufferType::Device, numPixels);
            
            if (!wf.numCompactedPaths) {
                wf.numCompactedPaths = std::make_unique<cudau::Buffer<uint32_t>>();
            }
            wf.numCompactedPaths->initialize(m_cudaContext, cudau::BufferType::Device, 1);
            
            printf("[VLR] CUB buffers allocated (temp: %.2f KB)\n", wf.cubTempStorageBytes / 1024.0f);
        }
    }
    
    // 更新配置
    wf.maxNumPaths = numPixels;
    wf.currentWidth = width;
    wf.currentHeight = height;
    
    printf("[VLR] Buffers allocated: %ux%u (%u paths, ~%.2f MB)\n",
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
    
    // 缓冲区大小改变，需要重新捕获 CUDA Graph
    if (wf.graphCaptured) {
        cudaGraphExecDestroy(wf.renderGraphExec);
        cudaGraphDestroy(wf.renderGraph);
        wf.graphCaptured = false;
    }
    
    allocateWavefrontBuffers(width, height);
    setupWavefrontLaunchParams();
}


void Context::resetWavefrontQueues() {
    auto& wf = m_optix.wavefrontPathTracing;

    // 使用同步的 cudaMemset 确保立即清除
    if (wf.queueCounters) {
        CUDA_CHECK(cudaMemset(wf.queueCounters->getDevicePointer(), 0, 2 * sizeof(uint32_t)));
    }

    if (wf.useMaterialQueues && wf.materialQueueCounters) {
        wf.materialQueueCounters->clear(m_stream);
    }
    
    // 清除路径状态缓冲区（重要：避免旧的终止状态影响新的渲染）
    if (wf.pathStateBuffer) {
        wf.pathStateBuffer->clear(m_stream);
        CUDA_CHECK(cudaStreamSynchronize(m_stream));
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
    lp.pathTexturedParamsBuffer = wf.pathTexturedParamsBuffer ? wf.pathTexturedParamsBuffer->getDevicePointer() : nullptr;
    
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
        lp.textureDescriptorBuffer = getTextureDescriptorBuffer();
        lp.materialNormalMapIndices = m_sceneSource->getMaterialNormalMapIndices();
        lp.materialAlbedoTextureIndices = m_sceneSource->getMaterialAlbedoTextureIndices();
        lp.materialRoughnessTextureIndices = m_sceneSource->getMaterialRoughnessTextureIndices();
        lp.materialMetallicTextureIndices = m_sceneSource->getMaterialMetallicTextureIndices();
        lp.materialTextureParamsBuffer = m_sceneSource->getMaterialTextureParams();
        lp.pathTexturedParamsBuffer = wf.pathTexturedParamsBuffer ? wf.pathTexturedParamsBuffer->getDevicePointer() : nullptr;
        lp.vertexPositions = m_sceneSource->getVertexPositions();
        lp.vertexNormals = m_sceneSource->getVertexNormals();
        lp.vertexTexCoords = m_sceneSource->getVertexTexCoords();
        lp.topGroup = m_sceneSource->getTopGroup();
        lp.cameraDescriptor = m_sceneSource->getCamera();
        lp.progSampleLensPosition = -1;
        lp.progTestLensIntersection = -1;
        lp.progEvaluateIDF = -1;
        // 设置光源实例索引数组（用于光源采样）
        lp.instIndices = m_sceneSource->getLightInstIndices();

        // SceneBounds 需设备指针，上传到小缓冲区
        if (!wf.sceneBoundsBuffer) {
            wf.sceneBoundsBuffer = std::make_unique<cudau::Buffer<shared::SceneBounds>>();
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
        lp.materialAlbedoTextureIndices = nullptr;
        lp.materialRoughnessTextureIndices = nullptr;
        lp.materialMetallicTextureIndices = nullptr;
        lp.materialTextureParamsBuffer = nullptr;
        lp.pathTexturedParamsBuffer = wf.pathTexturedParamsBuffer ? wf.pathTexturedParamsBuffer->getDevicePointer() : nullptr;
        lp.vertexPositions = nullptr;
        lp.vertexNormals = nullptr;
        lp.vertexTexCoords = nullptr;
        lp.topGroup = 0;
        lp.sceneBounds = nullptr;
        lp.cameraDescriptor = m_scene.camera;  // 使用默认 SceneData
        lp.progSampleLensPosition = -1;
        lp.progTestLensIntersection = -1;
        lp.progEvaluateIDF = -1;
    }
    
    // 设置图像参数（与原始 VLR 一致：numAccumFrames 用于多采样正确平均）
    lp.imageSize = make_uint2(wf.currentWidth, wf.currentHeight);
    lp.imageStrideInPixels = wf.currentWidth;
    lp.numAccumFrames = wf.numAccumFrames;
    lp.limitNumAccumFrames = 0;  // 0 = 无限制
    
    // 设置 Wavefront 配置
    lp.maxPathLength = wf.maxPathLength;
    lp.maxNumPaths = wf.maxNumPaths;
    lp.currentDepth = 0;
    
    // 设置性能统计指针
    lp.numActiveRays = wf.perfStatsBuffer ? wf.perfStatsBuffer->getDevicePointerAt(0) : nullptr;
    lp.numShadowRays = wf.perfStatsBuffer ? wf.perfStatsBuffer->getDevicePointerAt(1) : nullptr;
    lp.numTerminatedPaths = wf.perfStatsBuffer ? wf.perfStatsBuffer->getDevicePointerAt(2) : nullptr;
    
    // 设置光源分布
    if (m_sceneSource) {
        lp.lightInstDist.weights = nullptr;  // 简化实现：均匀分布
        lp.lightInstDist.numValues = m_sceneSource->getNumLightInsts();
        lp.envLightInstIndex = m_sceneSource->getEnvLightInstIndex();
    } else {
        lp.lightInstDist.weights = nullptr;
        lp.lightInstDist.numValues = 0;
        lp.envLightInstIndex = 0xFFFFFFFF;
    }

    // 设置调试参数
    lp.probePixX = m_probePixelX;
    lp.probePixY = m_probePixelY;
    lp.debugMode = static_cast<uint32_t>(m_debugMode);
    
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
    
    // 同步以确保参数上传完成
    CUDA_CHECK(cudaStreamSynchronize(m_stream));
    
    // ========================================================================
    // 更新 SBT 记录中的 launch parameters 指针
    // ========================================================================
    shared::WavefrontSBTData sbtData;
    sbtData.params = static_cast<shared::WavefrontLaunchParameters*>(wf.launchParamsBuffer);
    
    // 更新 RayGen 记录
    CUDA_CHECK(cudaMemcpy(
        static_cast<char*>(wf.raygenRecord) + OPTIX_SBT_RECORD_HEADER_SIZE,
        &sbtData,
        sizeof(sbtData),
        cudaMemcpyHostToDevice
    ));
    
    // 更新 Miss 记录（2 条）- 必须使用与 createWavefrontSBT 相同的 16 字节对齐 stride
    size_t missRecordSize = OPTIX_SBT_RECORD_HEADER_SIZE + sizeof(shared::WavefrontSBTData);
    missRecordSize = (missRecordSize + 15) & ~15;
    CUDA_CHECK(cudaMemcpy(
        static_cast<char*>(wf.missRecord) + OPTIX_SBT_RECORD_HEADER_SIZE,
        &sbtData,
        sizeof(sbtData),
        cudaMemcpyHostToDevice
    ));
    CUDA_CHECK(cudaMemcpy(
        static_cast<char*>(wf.missRecord) + missRecordSize + OPTIX_SBT_RECORD_HEADER_SIZE,
        &sbtData,
        sizeof(sbtData),
        cudaMemcpyHostToDevice
    ));
    
    // 更新 HitGroup 记录（2 条）- 必须使用与 createWavefrontSBT 相同的 16 字节对齐 stride
    size_t hitgroupRecordSize = OPTIX_SBT_RECORD_HEADER_SIZE + sizeof(shared::WavefrontSBTData);
    hitgroupRecordSize = (hitgroupRecordSize + 15) & ~15;
    CUDA_CHECK(cudaMemcpy(
        static_cast<char*>(wf.hitgroupRecord) + OPTIX_SBT_RECORD_HEADER_SIZE,
        &sbtData,
        sizeof(sbtData),
        cudaMemcpyHostToDevice
    ));
    CUDA_CHECK(cudaMemcpy(
        static_cast<char*>(wf.hitgroupRecord) + hitgroupRecordSize + OPTIX_SBT_RECORD_HEADER_SIZE,
        &sbtData,
        sizeof(sbtData),
        cudaMemcpyHostToDevice
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
    
    // 释放缓冲区（智能指针自动析构，显式 reset 以立即释放）
    wf.pathStateBuffer.reset();
    wf.hitInfoBuffer.reset();
    wf.surfacePointBuffer.reset();
    wf.pathTexturedParamsBuffer.reset();
    wf.activePathIndices.reset();
    wf.nextActivePathIndices.reset();
    wf.queueCounters.reset();
    wf.accumBuffer.reset();
    wf.accumAlbedoBuffer.reset();
    wf.accumNormalBuffer.reset();
    wf.rngBuffer.reset();
    wf.perfStatsBuffer.reset();
    wf.sceneBoundsBuffer.reset();
    
    // 销毁 CUDA 事件
    if (wf.eventsCreated) {
        cudaEventDestroy(wf.startEvent);
        cudaEventDestroy(wf.endEvent);
        wf.eventsCreated = false;
    }

    // 销毁 CUDA Graphs
    if (wf.graphCaptured) {
        cudaGraphExecDestroy(wf.renderGraphExec);
        cudaGraphDestroy(wf.renderGraph);
        wf.graphCaptured = false;
    }
    
    // 释放 CUB 临时存储
    wf.cubTempStorage.reset();
    wf.sortedPathIndices.reset();
    wf.compactedPathIndices.reset();
    wf.numCompactedPaths.reset();
    
    for (int i = 0; i < shared::NumMaterialCategories; ++i) {
        wf.materialQueueIndices[i].reset();
    }
    wf.materialQueueCounters.reset();
    
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

    printf("[VLR] renderWavefront started: %ux%u, %u samples\n", width, height, numSamples);
    fflush(stdout);

    // 若有外部场景则构建加速结构并上传到设备
    if (m_sceneSource) {
        printf("[VLR] Building acceleration structure...\n");
        fflush(stdout);
        const_cast<Scene*>(m_sceneSource)->buildAccelerationStructure();
        printf("[VLR] Uploading scene data to GPU...\n");
        fflush(stdout);
        const_cast<Scene*>(m_sceneSource)->updateToGPU();
        m_scene.camera = m_sceneSource->getCamera();
        m_scene.bounds = m_sceneSource->getSceneBounds();
        printf("[VLR] Scene ready\n");
        fflush(stdout);
    }

    // 确保缓冲区已分配
    if (wf.currentWidth != width || wf.currentHeight != height) {
        printf("[VLR] Resizing buffers...\n");
        fflush(stdout);
        resizeWavefrontBuffers(width, height);
    }

    // 与原始 VLR 一致：新渲染开始时清除累加缓冲区和帧计数
    wf.numAccumFrames = 0;
    if (wf.accumBuffer && wf.accumBuffer->size() > 0) {
        wf.accumBuffer->clear(m_stream);
        CUDA_CHECK(cudaStreamSynchronize(m_stream));
    }

    // 设置启动参数
    printf("[VLR] Setting launch parameters...\n");
    fflush(stdout);
    setupWavefrontLaunchParams();

    // 记录开始时间
    CUDA_CHECK(cudaEventRecord(wf.startEvent, m_stream));

    // 检查调试模式：非 Normal 时使用简化渲染路径（单次采样，无多次反弹）
    if (m_debugMode != VLRDebugMode_Normal) {
        printf("[VLR] Debug mode: %s (single sample, no multi-bounce)\n", getDebugModeName(m_debugMode));
        fflush(stdout);
        wf.numAccumFrames = 1;
        executeWavefrontRenderDebug(static_cast<uint32_t>(m_debugMode));
    } else {
        // 正常路径追踪
        printf("[VLR] Starting render loop...\n");
        fflush(stdout);
        for (uint32_t sample = 0; sample < numSamples; ++sample) {
            printf("[VLR] Sample %u/%u\n", sample + 1, numSamples);
            fflush(stdout);
            ++wf.numAccumFrames;
            executeWavefrontRender(1);
        }
    }
    
    // 记录结束时间并计算渲染时间
    CUDA_CHECK(cudaEventRecord(wf.endEvent, m_stream));
    CUDA_CHECK(cudaEventSynchronize(wf.endEvent));
    
    float renderTimeMs = 0.0f;
    CUDA_CHECK(cudaEventElapsedTime(&renderTimeMs, wf.startEvent, wf.endEvent));
    
    printf("[VLR] Render completed in %.2f ms (%.2f ms/sample, %.2f Msamples/s)\n",
           renderTimeMs,
           renderTimeMs / numSamples,
           (width * height * numSamples) / (renderTimeMs * 1000.0f));
    fflush(stdout);
    
    // 执行降噪（如果启用，调试模式跳过）
    if (m_denoiserConfig.enabled && m_debugMode == VLRDebugMode_Normal && wf.accumBuffer) {
        printf("[VLR] Applying OptiX denoiser...\n");
        fflush(stdout);
        
        // 初始化降噪器（如果尚未初始化）
        if (!m_denoiser.isInitialized()) {
            m_denoiser.initialize(width, height, m_denoiserConfig, m_optix.context);
        }
        
        // 准备降噪输入（需要将 SpectrumStorage 转换为 float3）
        // 注意：这里需要一个转换 kernel，暂时使用原始缓冲区
        CUdeviceptr d_colorBuffer = reinterpret_cast<CUdeviceptr>(wf.accumBuffer->getDevicePointer());
        CUdeviceptr d_albedoBuffer = wf.accumAlbedoBuffer ? reinterpret_cast<CUdeviceptr>(wf.accumAlbedoBuffer->getDevicePointer()) : 0;
        CUdeviceptr d_normalBuffer = wf.accumNormalBuffer ? reinterpret_cast<CUdeviceptr>(wf.accumNormalBuffer->getDevicePointer()) : 0;
        
        // 执行降噪（输入和输出使用同一个缓冲区）
        m_denoiser.denoise(d_colorBuffer, d_colorBuffer, d_albedoBuffer, d_normalBuffer, numSamples);
        
        printf("[VLR] Denoising completed\n");
        fflush(stdout);
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


void Context::executeWavefrontRenderDebug(uint32_t debugMode) {
    auto& wf = m_optix.wavefrontPathTracing;

#ifdef VLR_DEBUG_NAN_TRACKING
    resetNanDebugCount();
#endif

    uint32_t numPixels = wf.currentWidth * wf.currentHeight;

    // 重置队列
    resetWavefrontQueues();
    setupWavefrontLaunchParams();

    if (wf.queueCounters) {
        uint32_t zero[2] = {0, 0};
        CUDA_CHECK(cudaMemcpy(
            wf.queueCounters->getDevicePointer(),
            zero,
            2 * sizeof(uint32_t),
            cudaMemcpyHostToDevice
        ));
    }

    // 阶段 1: 生成初始光线
    launchGenerateRays(numPixels);

    if (wf.queueCounters) {
        CUDA_CHECK(cudaMemcpyAsync(
            wf.queueCounters->getDevicePointerAt(0),
            &numPixels,
            sizeof(uint32_t),
            cudaMemcpyHostToDevice,
            m_stream
        ));
    }

    // 阶段 2: 光线追踪（单次，无多次反弹）
    launchTraceRays(numPixels);

    // 阶段 3: 处理命中（填充 surfacePointBuffer、accumAlbedo、accumNormal）
    launchProcessHits(numPixels);

    // 阶段 4: 调试可视化（直接写入 accumBuffer）
    launchRenderDebugMode(numPixels, debugMode);

    CUDA_CHECK(cudaStreamSynchronize(m_stream));
}

void Context::executeWavefrontRender(uint32_t numSamples) {
    auto& wf = m_optix.wavefrontPathTracing;

#ifdef VLR_DEBUG_NAN_TRACKING
    resetNanDebugCount();
#endif

    uint32_t numPixels = wf.currentWidth * wf.currentHeight;
    
    // 重置队列
    resetWavefrontQueues();
    
    // 更新启动参数到设备（确保 counter 指针指向已清除的计数器）
    setupWavefrontLaunchParams();
    
    // 再次显式重置计数器（确保 GPU 能看到）
    if (wf.queueCounters) {
        uint32_t zero[2] = {0, 0};
        CUDA_CHECK(cudaMemcpy(
            wf.queueCounters->getDevicePointer(),
            zero,
            2 * sizeof(uint32_t),
            cudaMemcpyHostToDevice
        ));
    }
    
    // 阶段 1: 生成初始光线
    launchGenerateRays(numPixels);
    
    // 与原始 VLR 一致：在主机端可靠设置活跃队列计数，避免 GPU 内核竞态
    if (wf.queueCounters) {
        CUDA_CHECK(cudaMemcpyAsync(
            wf.queueCounters->getDevicePointerAt(0),
            &numPixels,
            sizeof(uint32_t),
            cudaMemcpyHostToDevice,
            m_stream
        ));
    }
    
        // 主 Wavefront 循环
        // 优化：减少同步频率，使用配置的同步间隔
        constexpr uint32_t SYNC_INTERVAL = shared::PerformanceConfig::SyncInterval;
        uint32_t numActivePaths = numPixels;  // 初始时所有路径都活跃
        
        for (uint32_t depth = 0; depth < wf.maxPathLength; ++depth) {
        wf.launchParams.currentDepth = depth;
        
        // 优化：在同步间隔时检查活跃路径数，并支持早期终止
        if (depth % SYNC_INTERVAL == 0 && depth > 0) {
            if (wf.queueCounters) {
                wf.queueCounters->copyToHost(&numActivePaths, 1, m_stream);
                CUDA_CHECK(cudaStreamSynchronize(m_stream));
            }
            
            // 早期终止：当路径数很少时，提前退出
            constexpr float EARLY_TERMINATION_THRESHOLD = shared::PerformanceConfig::EarlyTerminationThreshold;
            constexpr uint32_t MIN_DEPTH = shared::PerformanceConfig::EarlyTerminationMinDepth;
            uint32_t minPaths = static_cast<uint32_t>(numPixels * EARLY_TERMINATION_THRESHOLD);
            
            if (numActivePaths == 0) {
                break;  // 所有路径已终止
            } else if (numActivePaths < minPaths && depth > MIN_DEPTH) {
                // 路径数很少且已经足够深，提前终止
                break;
            }
        }
        
        // 阶段 2: 光线追踪
        launchTraceRays(numActivePaths);
        
        // 阶段 3: 处理命中
        launchProcessHits(numActivePaths);
        
        // 阶段 4: 采样光源 (NEE)
        launchSampleLights(numActivePaths);
        
        // 阶段 5: 采样 BSDF
        launchSampleBSDF(numActivePaths);
        
        // 阶段 6: 路径压缩和排序
        // 获取下一轮路径数量
        uint32_t numNextPaths = 0;
        if (wf.queueCounters) {
            CUDA_CHECK(cudaMemcpyAsync(
                &numNextPaths,
                wf.queueCounters->getDevicePointerAt(1),
                sizeof(uint32_t),
                cudaMemcpyDeviceToHost,
                m_stream
            ));
            CUDA_CHECK(cudaStreamSynchronize(m_stream));
        }
        
        // 优化：只在路径数下降超过阈值且路径数足够多时才执行压缩
        constexpr float COMPRESSION_THRESHOLD = shared::PerformanceConfig::CompressionThreshold;
        constexpr uint32_t MIN_PATHS = shared::PerformanceConfig::MinPathsForCompression;
        float compressionRatio = (numActivePaths > 0) ? 
            static_cast<float>(numNextPaths) / numActivePaths : 0.0f;
        bool shouldCompress = (compressionRatio < COMPRESSION_THRESHOLD) && 
                             (numNextPaths > MIN_PATHS);
        
        if (wf.useStreamCompaction && shouldCompress) {
            // 使用 CUB Stream Compaction 移除已终止路径
            CUDA_CHECK(shared::compactPathsCUB(
                static_cast<uint32_t*>(wf.nextActivePathIndices->getDevicePointer()),
                numNextPaths,
                wf.pathStateBuffer->getDevicePointer(),
                static_cast<uint32_t*>(wf.activePathIndices->getDevicePointer()),
                static_cast<uint32_t*>(wf.queueCounters->getDevicePointerAt(0)),
                wf.cubTempStorage->getDevicePointer(),
                wf.cubTempStorageBytes,
                m_stream
            ));
            
            // 重置下一队列计数
            uint32_t zero = 0;
            CUDA_CHECK(cudaMemcpyAsync(
                wf.queueCounters->getDevicePointerAt(1),
                &zero,
                sizeof(uint32_t),
                cudaMemcpyHostToDevice,
                m_stream
            ));
        } else if (wf.usePathSorting && numNextPaths > 0 && depth > 0) {
            // 使用 CUB RadixSort 按材质排序
            // 注意：仅在深度>0时排序，因为深度0时materialCategory未初始化
            // 注意：临时存储大小需要与实际路径数匹配
            size_t requiredTempBytes = shared::sortPathsByMaterialTempStorageBytes(numNextPaths);
            
            // 调试：仅在第一次警告时打印详细信息
            static bool firstWarning = true;
            if (requiredTempBytes > wf.cubTempStorageBytes && firstWarning) {
                fprintf(stderr, "[VLR] DEBUG: paths=%u, required=%zu bytes (%.2f KB), allocated=%zu bytes (%.2f KB)\n",
                        numNextPaths, requiredTempBytes, requiredTempBytes/1024.0f, 
                        wf.cubTempStorageBytes, wf.cubTempStorageBytes/1024.0f);
                firstWarning = false;
            }
            
            if (requiredTempBytes > wf.cubTempStorageBytes) {
                fprintf(stderr, "[VLR] Warning: CUB temp storage insufficient (%zu > %zu), using simple swap\n",
                        requiredTempBytes, wf.cubTempStorageBytes);

                // 回退到简单交换
                std::swap(wf.activePathIndices, wf.nextActivePathIndices);
                CUDA_CHECK(cudaMemcpyAsync(
                    wf.queueCounters->getDevicePointerAt(0),
                    wf.queueCounters->getDevicePointerAt(1),
                    sizeof(uint32_t),
                    cudaMemcpyDeviceToDevice,
                    m_stream
                ));
                uint32_t zero = 0;
                CUDA_CHECK(cudaMemcpyAsync(
                    wf.queueCounters->getDevicePointerAt(1),
                    &zero,
                    sizeof(uint32_t),
                    cudaMemcpyHostToDevice,
                    m_stream
                ));
            } else {
                size_t tempBytes = wf.cubTempStorageBytes;
                CUDA_CHECK(shared::sortPathsByMaterial(
                    static_cast<uint32_t*>(wf.nextActivePathIndices->getDevicePointer()),
                    numNextPaths,
                    wf.pathStateBuffer->getDevicePointer(),
                    static_cast<uint32_t*>(wf.activePathIndices->getDevicePointer()),
                    wf.cubTempStorage->getDevicePointer(),
                    tempBytes,
                    m_stream
                ));

                
                // 同步队列计数器：counters[0] = counters[1], counters[1] = 0
                CUDA_CHECK(cudaMemcpyAsync(
                    wf.queueCounters->getDevicePointerAt(0),
                    wf.queueCounters->getDevicePointerAt(1),
                    sizeof(uint32_t),
                    cudaMemcpyDeviceToDevice,
                    m_stream
                ));
                uint32_t zero = 0;
                CUDA_CHECK(cudaMemcpyAsync(
                    wf.queueCounters->getDevicePointerAt(1),
                    &zero,
                    sizeof(uint32_t),
                    cudaMemcpyHostToDevice,
                    m_stream
                ));
            }
        } else {
            // 简单队列交换（默认）
            std::swap(wf.activePathIndices, wf.nextActivePathIndices);
            
            // 同步队列计数器：counters[0] = counters[1], counters[1] = 0
            CUDA_CHECK(cudaMemcpyAsync(
                wf.queueCounters->getDevicePointerAt(0),
                wf.queueCounters->getDevicePointerAt(1),
                sizeof(uint32_t),
                cudaMemcpyDeviceToDevice,
                m_stream
            ));
            uint32_t zero = 0;
            CUDA_CHECK(cudaMemcpyAsync(
                wf.queueCounters->getDevicePointerAt(1),
                &zero,
                sizeof(uint32_t),
                cudaMemcpyHostToDevice,
                m_stream
            ));
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

    // 根据图像尺寸计算 grid/block，调用 generateRays CUDA kernel
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
        throw std::runtime_error("launchTraceRays: OptiX pipeline not initialized, call createWavefrontPrograms first");
    }
    if (!wf.launchParamsBuffer) return;
    if (numActivePaths == 0) return;

    // 将当前深度等参数更新到设备
    setupWavefrontLaunchParams();

    try {
        // 注意：我们通过 SBT 数据传递 launch parameters，所以 launchParams 参数设为 0
        OPTIX_CHECK(optixLaunch(
            wf.pipeline,
            m_stream,
            0,  // 不使用 launchParams（通过 SBT 传递）
            0,
            &wf.sbt,
            numActivePaths,  // 每个线程处理一条活跃路径
            1,
            1
        ));
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("launchTraceRays: optixLaunch failed - ") + e.what());
    }
}

void Context::launchProcessHits(uint32_t numActivePaths) {
    auto& wf = m_optix.wavefrontPathTracing;
    if (!wf.launchParamsBuffer) return;
    if (numActivePaths == 0) return;

    // 调用 processHits CUDA kernel
    shared::WavefrontLaunchParameters* d_params =
        static_cast<shared::WavefrontLaunchParameters*>(wf.launchParamsBuffer);

    launchProcessHitsKernel(d_params, numActivePaths, m_stream);
}

void Context::launchSampleLights(uint32_t numActivePaths) {
    auto& wf = m_optix.wavefrontPathTracing;
    if (!wf.launchParamsBuffer) return;
    if (numActivePaths == 0) return;

    // 调用 sampleLights CUDA kernel
    shared::WavefrontLaunchParameters* d_params =
        static_cast<shared::WavefrontLaunchParameters*>(wf.launchParamsBuffer);

    launchSampleLightsKernel(d_params, numActivePaths, m_stream);
}

void Context::launchSampleBSDF(uint32_t numActivePaths) {
    auto& wf = m_optix.wavefrontPathTracing;
    if (!wf.launchParamsBuffer) return;
    if (numActivePaths == 0) return;

    // 调用 sampleBSDF CUDA kernel
    shared::WavefrontLaunchParameters* d_params =
        static_cast<shared::WavefrontLaunchParameters*>(wf.launchParamsBuffer);

    launchSampleBSDFKernel(d_params, numActivePaths, m_stream);
}

void Context::setWavefrontPathSorting(bool enable) {
    m_optix.wavefrontPathTracing.usePathSorting = enable;
}

void Context::setWavefrontStreamCompaction(bool enable) {
    m_optix.wavefrontPathTracing.useStreamCompaction = enable;
}

void Context::launchAccumulate(uint32_t numPaths) {
    auto& wf = m_optix.wavefrontPathTracing;
    if (!wf.launchParamsBuffer) return;
    if (numPaths == 0) return;

    // 调用 accumulateResults CUDA kernel
    shared::WavefrontLaunchParameters* d_params =
        static_cast<shared::WavefrontLaunchParameters*>(wf.launchParamsBuffer);

    launchAccumulateKernel(d_params, numPaths, m_stream);
}

void Context::launchRenderDebugMode(uint32_t numPixels, uint32_t debugMode) {
    auto& wf = m_optix.wavefrontPathTracing;
    if (!wf.launchParamsBuffer) return;
    if (numPixels == 0) return;

    shared::WavefrontLaunchParameters* d_params =
        static_cast<shared::WavefrontLaunchParameters*>(wf.launchParamsBuffer);

    launchRenderDebugModeKernel(d_params, numPixels, debugMode, m_stream);
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

void Context::setPerformanceConfig(const RuntimePerformanceConfig& config) {
    m_perfConfig = config;
    auto& wf = m_optix.wavefrontPathTracing;
    wf.usePathSorting = config.enablePathSorting;
    wf.useStreamCompaction = config.enableStreamCompaction;
    // 其他字段（syncInterval、block sizes 等）已存储于 m_perfConfig，供后续 kernel 启动使用
}


// ============================================================================
// 纹理管理
// ============================================================================

namespace {

shared::TextureFormat apiFormatToInternal(uint32_t format) {
    switch (format) {
        case 0: return shared::TextureFormat_RGBA8;
        case 1: return shared::TextureFormat_RGB32F;
        case 2: return shared::TextureFormat_RGBA32F;
        default: return shared::TextureFormat_RGBA8;
    }
}

size_t bytesPerPixel(shared::TextureFormat fmt) {
    switch (fmt) {
        case shared::TextureFormat_RGBA8: return 4;
        case shared::TextureFormat_RGB32F: return 3 * sizeof(float);
        case shared::TextureFormat_RGBA32F: return 4 * sizeof(float);
        default: return 4;
    }
}

}  // namespace

bool Context::createTexture2D(const char* imagePath, uint32_t* outTextureIndex) {
    if (!imagePath || !outTextureIndex) {
        printf("[VLR] createTexture2D: invalid arguments (null)\n");
        return false;
    }
    TextureImage img;
    std::string error;
    if (!loadTextureImage(imagePath, img, &error)) {
        printf("[VLR] createTexture2D: failed to load '%s': %s\n", imagePath, error.c_str());
        return false;
    }
    shared::TextureFormat fmt = (img.format == TextureImageFormat::RGBA8)
        ? shared::TextureFormat_RGBA8
        : shared::TextureFormat_RGBA32F;
    bool ok = createTexture2DFromMemory(img.data, img.width, img.height,
                                        static_cast<uint32_t>(img.format), outTextureIndex);
    freeTextureImage(img);
    if (ok) {
        printf("[VLR] createTexture2D: loaded '%s' %ux%u -> texture index %u\n",
               imagePath, img.width, img.height, *outTextureIndex);
    }
    return ok;
}

bool Context::createTexture2DFromMemory(const void* data, uint32_t width, uint32_t height,
                                       uint32_t format, uint32_t* outTextureIndex) {
    if (!data || width == 0 || height == 0 || !outTextureIndex) {
        printf("[VLR] createTexture2DFromMemory: invalid arguments\n");
        return false;
    }
    if (format > 2) {
        printf("[VLR] createTexture2DFromMemory: invalid format %u (0=RGBA8, 1=RGB32F, 2=RGBA32F)\n", format);
        return false;
    }
    try {
        shared::TextureFormat fmt = apiFormatToInternal(format);
        size_t bytesPerPixelVal = bytesPerPixel(fmt);
        size_t totalBytes = static_cast<size_t>(width) * height * bytesPerPixelVal;

        TextureRecord rec;
        rec.gpuBuffer = std::make_unique<cudau::Buffer<uint8_t>>();
        rec.gpuBuffer->initialize(m_cudaContext, cudau::BufferType::Device, totalBytes);
        rec.gpuBuffer->copyToDevice(static_cast<const uint8_t*>(data), totalBytes, m_stream);

        rec.descriptor = shared::Texture2DDescriptor(
            rec.gpuBuffer->getDevicePointer(),
            width, height,
            fmt,
            1, 0);
        rec.filterMode = shared::TextureFilter_Linear;
        rec.wrapU = shared::TextureWrap_Repeat;
        rec.wrapV = shared::TextureWrap_Repeat;

        uint32_t idx = static_cast<uint32_t>(m_textures.size());
        m_textures.push_back(std::move(rec));
        m_textureDescriptorBufferDirty = true;
        *outTextureIndex = idx;
        printf("[VLR] createTexture2DFromMemory: created texture %u %ux%u format %u\n",
               idx, width, height, format);
        return true;
    } catch (const std::exception& e) {
        printf("[VLR] createTexture2DFromMemory: exception: %s\n", e.what());
        return false;
    }
}

void Context::destroyTexture(uint32_t textureIndex) {
    if (textureIndex >= m_textures.size()) {
        printf("[VLR] destroyTexture: invalid index %u (max %zu)\n",
               textureIndex, m_textures.size());
        return;
    }
    m_textures[textureIndex].gpuBuffer.reset();
    m_textures[textureIndex].descriptor = shared::Texture2DDescriptor();
    m_textureDescriptorBufferDirty = true;
    printf("[VLR] destroyTexture: texture %u destroyed\n", textureIndex);
}

bool Context::setTextureFilterMode(uint32_t textureIndex, uint32_t filterMode) {
    if (textureIndex >= m_textures.size()) {
        printf("[VLR] setTextureFilterMode: invalid index %u\n", textureIndex);
        return false;
    }
    if (filterMode > 1) {
        printf("[VLR] setTextureFilterMode: invalid mode %u (0=Nearest, 1=Linear)\n", filterMode);
        return false;
    }
    m_textures[textureIndex].filterMode = static_cast<shared::TextureFilterMode>(filterMode);
    return true;
}

bool Context::setTextureWrapMode(uint32_t textureIndex, uint32_t wrapU, uint32_t wrapV) {
    if (textureIndex >= m_textures.size()) {
        printf("[VLR] setTextureWrapMode: invalid index %u\n", textureIndex);
        return false;
    }
    if (wrapU > 1 || wrapV > 1) {
        printf("[VLR] setTextureWrapMode: invalid wrap mode (0=Repeat, 1=Clamp)\n");
        return false;
    }
    m_textures[textureIndex].wrapU = static_cast<shared::TextureWrapMode>(wrapU);
    m_textures[textureIndex].wrapV = static_cast<shared::TextureWrapMode>(wrapV);
    return true;
}

const shared::Texture2DDescriptor* Context::getTextureDescriptor(uint32_t textureIndex) const {
    if (textureIndex >= m_textures.size()) return nullptr;
    const auto& rec = m_textures[textureIndex];
    if (!rec.gpuBuffer || !rec.gpuBuffer->getDevicePointer()) return nullptr;
    return &rec.descriptor;
}

void Context::updateTextureDescriptorBuffer() const {
    if (!m_textureDescriptorBufferDirty || m_textures.empty()) return;
    m_textureDescriptorBufferDirty = false;
    std::vector<shared::Texture2DDescriptor> hostDescriptors(m_textures.size());
    for (size_t i = 0; i < m_textures.size(); ++i) {
        const auto& rec = m_textures[i];
        if (rec.gpuBuffer && rec.gpuBuffer->getDevicePointer())
            hostDescriptors[i] = rec.descriptor;
        else
            hostDescriptors[i] = shared::Texture2DDescriptor();
    }
    if (!m_textureDescriptorBuffer)
        m_textureDescriptorBuffer = std::make_unique<cudau::Buffer<shared::Texture2DDescriptor>>();
    m_textureDescriptorBuffer->initialize(m_cudaContext, cudau::BufferType::Device, hostDescriptors.size());
    m_textureDescriptorBuffer->copyToDevice(hostDescriptors.data(), hostDescriptors.size(), m_stream);
}

const shared::Texture2DDescriptor* Context::getTextureDescriptorBuffer() const {
    updateTextureDescriptorBuffer();
    return (m_textureDescriptorBuffer && m_textureDescriptorBuffer->getDevicePointer())
        ? m_textureDescriptorBuffer->getDevicePointer() : nullptr;
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

// ============================================================================
// 降噪器配置
// ============================================================================

void Context::setDenoiserConfig(const DenoiserConfig& config) {
    m_denoiserConfig = config;
}

const DenoiserConfig& Context::getDenoiserConfig() const {
    return m_denoiserConfig;
}

// ============================================================================
// 调试模式与探针像素
// ============================================================================

void Context::setDebugMode(VLRDebugMode mode) {
    m_debugMode = mode;
}

VLRDebugMode Context::getDebugMode() const {
    return m_debugMode;
}

void Context::setProbePixel(int32_t x, int32_t y) {
    m_probePixelX = x;
    m_probePixelY = y;
}

void Context::getProbePixel(int32_t* outX, int32_t* outY) const {
    if (outX) *outX = m_probePixelX;
    if (outY) *outY = m_probePixelY;
}

} // namespace vlr
