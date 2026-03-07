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
    
    OptixDeviceContextOptions options = {};
    options.logCallbackFunction = enableLogging ? &optixLogCallback : nullptr;
    options.logCallbackLevel = 4;
    
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
        wf.cubTempStorageBytes = std::max(sortBytes, compactBytes);
        
        if (wf.cubTempStorageBytes > 0) {
            if (!wf.cubTempStorage) {
                wf.cubTempStorage = new cudau::Buffer<uint8_t>();
            }
            wf.cubTempStorage->initialize(m_cudaContext, cudau::BufferType::Device, wf.cubTempStorageBytes);
            
            // 分配排序/压缩辅助缓冲区
            if (!wf.sortedPathIndices) {
                wf.sortedPathIndices = new cudau::Buffer<uint32_t>();
            }
            wf.sortedPathIndices->initialize(m_cudaContext, cudau::BufferType::Device, numPixels);
            
            if (!wf.compactedPathIndices) {
                wf.compactedPathIndices = new cudau::Buffer<uint32_t>();
            }
            wf.compactedPathIndices->initialize(m_cudaContext, cudau::BufferType::Device, numPixels);
            
            if (!wf.numCompactedPaths) {
                wf.numCompactedPaths = new cudau::Buffer<uint32_t>();
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
        lp.progSampleLensPosition = -1;
        lp.progTestLensIntersection = -1;
        lp.progEvaluateIDF = -1;
        // 设置光源实例索引数组（用于光源采样）
        lp.instIndices = m_sceneSource->getLightInstIndices();

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
        lp.progSampleLensPosition = -1;
        lp.progTestLensIntersection = -1;
        lp.progEvaluateIDF = -1;
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
    
    // 更新 Miss 记录（2 条）
    size_t missRecordSize = OPTIX_SBT_RECORD_HEADER_SIZE + sizeof(shared::WavefrontSBTData);
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
    
    // 更新 HitGroup 记录（2 条）
    size_t hitgroupRecordSize = OPTIX_SBT_RECORD_HEADER_SIZE + sizeof(shared::WavefrontSBTData);
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
    
    // 释放 CUB 临时存储
    delete wf.cubTempStorage;
    delete wf.sortedPathIndices;
    delete wf.compactedPathIndices;
    delete wf.numCompactedPaths;
    wf.cubTempStorage = nullptr;
    wf.sortedPathIndices = nullptr;
    wf.compactedPathIndices = nullptr;
    wf.numCompactedPaths = nullptr;
    
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

    // 设置启动参数
    printf("[VLR] Setting launch parameters...\n");
    fflush(stdout);
    setupWavefrontLaunchParams();

    // 执行渲染
    printf("[VLR] Starting render loop...\n");
    fflush(stdout);
    for (uint32_t sample = 0; sample < numSamples; ++sample) {
        printf("[VLR] Sample %u/%u\n", sample + 1, numSamples);
        fflush(stdout);
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
        
        
        if (wf.useStreamCompaction && numNextPaths > 0) {
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
