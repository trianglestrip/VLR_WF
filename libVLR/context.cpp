// ============================================================================
// VLR Context ??
// 
// ?????? VLR ????Context ???
// 
// ??? VLR ?????
// ??: 2026-03-07
// ??: CUDA 13.1, OptiX 8.0.0, VS2022
// ============================================================================

// ????????????????
// #define VLR_ENABLE_CPU_DEBUG 1

#ifdef VLR_ENABLE_CPU_DEBUG
    #define VLR_DEBUG_PRINTF(...) printf(__VA_ARGS__)
#else
    #define VLR_DEBUG_PRINTF(...) ((void)0)
#endif

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
#include <optix_function_table_definition.h>  // OptiX: ?? g_optixFunctionTable ??
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
// PTX ??????
// ============================================================================

namespace {

/// ??libVLR/GPU_kernels/ ??build/Release ???? PTX ????
/// ?????????????????????? build/Release ????
std::vector<char> loadPTXFile(const char* filename) {
    // ?????????libVLR?build/Release?build/Debug ??
    const char* searchPaths[] = {
        "bin/GPU_kernels/",                // ??????
        "GPU_kernels/",                    // build/Release ??build/Debug ????
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

}  // ??????

// ============================================================================
// ?????????
// ============================================================================

// OptiX ??????
static void optixLogCallback(unsigned int level, const char* tag, const char* message, void* cbdata) {
    VLR_DEBUG_PRINTF("[OptiX][%s] %s\n", tag, message);
}

Context::Context(cudaStream_t cudaStream, bool enableLogging)
    : m_stream(cudaStream)
    , m_cudaContext(nullptr)
    , m_sceneSource(nullptr)
{
    // 增加 CUDA printf 缓冲区大小以便调试
    size_t printfBufferSize = 8 * 1024 * 1024;  // 8 MB
    cudaDeviceSetLimit(cudaLimitPrintfFifoSize, printfBufferSize);
    
    // ????CUDA ????
    m_cudaContext = new cudau::Context();

    // ????????
    m_denoiserConfig.enabled = false;
    m_denoiserConfig.useAlbedo = true;
    m_denoiserConfig.useNormal = true;
    m_denoiserConfig.hdrIntensity = 1.0f;

    // ????????
    m_debugMode = VLRDebugMode_Normal;
    m_probePixelX = -1;
    m_probePixelY = -1;
    
    // ????OptiX ????
    m_optix.stream = cudaStream;
    m_optix.enableLogging = enableLogging;
    m_optix.context = nullptr;
    
    // ??????OptiX???????optixu::Context ??
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
    
    // ????Wavefront ??
    initializeWavefrontPipeline();
}

Context::~Context() {
    // ?? Wavefront ??
    cleanupWavefrontResources();
    
    // ?? OptiX ????
    if (m_optix.context) {
        optixDeviceContextDestroy(m_optix.context);
        m_optix.context = nullptr;
    }
    
    // ?? CUDA ????
    if (m_cudaContext) {
        delete m_cudaContext;
        m_cudaContext = nullptr;
    }
}


// ============================================================================
// Wavefront ??????
// ============================================================================

void Context::initializeWavefrontPipeline() {
    auto& wf = m_optix.wavefrontPathTracing;

    if (wf.isInitialized) {
        return;
    }

    // ------------------------------------------------------------------------
    // 1. ?? PTX ??
    // ------------------------------------------------------------------------
    std::vector<char> ptxCode;
    try {
        ptxCode = loadPTXFile("trace_rays.ptx");
    } catch (const std::exception& e) {
        fprintf(stderr, "[VLR] Error: PTX load failed - %s\n", e.what());
        throw;
    }
    
    // ------------------------------------------------------------------------
    // 2. ????????
    // ------------------------------------------------------------------------
    OptixPipelineCompileOptions pipelineCompileOptions = {
        .usesMotionBlur = false,
        .traversableGraphFlags = OPTIX_TRAVERSABLE_GRAPH_FLAG_ALLOW_SINGLE_LEVEL_INSTANCING,
        .numPayloadValues = 7,  // WFTracePayload: 28 ?? = 7 ????
        .numAttributeValues = 2,  // ????????
        .exceptionFlags = OPTIX_EXCEPTION_FLAG_NONE,
        .pipelineLaunchParamsVariableName = nullptr  // ????launch params??? SBT ???
    };
    
    // ????????
    OptixModuleCompileOptions moduleCompileOptions = {
        .maxRegisterCount = OPTIX_COMPILE_DEFAULT_MAX_REGISTER_COUNT,
        .optLevel = OPTIX_COMPILE_OPTIMIZATION_DEFAULT,
        .debugLevel = OPTIX_COMPILE_DEBUG_LEVEL_MINIMAL
    };
    
    // ------------------------------------------------------------------------
    // 3. ?? OptiX ??
    // ------------------------------------------------------------------------
    char moduleLog[2048];
    size_t moduleLogSize = sizeof(moduleLog);
    
    OptixResult moduleRes = optixModuleCreate(
        m_optix.context,
        &moduleCompileOptions,
        &pipelineCompileOptions,
        ptxCode.data(),
        ptxCode.size() - 1,  // ???? '\0'
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
    VLR_DEBUG_PRINTF("[VLR] OptiX module created successfully\n");
    
    // ------------------------------------------------------------------------
    // 4. ??????RayGen?Miss?HitGroup?ShadowMiss?ShadowHitGroup??
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
    // 5. ?????????(SBT)
    // ------------------------------------------------------------------------
    try {
        createWavefrontSBT();
    } catch (const std::exception& e) {
        fprintf(stderr, "[VLR] Error: createWavefrontSBT failed - %s\n", e.what());
        cleanupWavefrontResources();
        throw;
    }
    
    // ------------------------------------------------------------------------
    // 6. ?? Pipeline
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
    pipelineLinkOptions.maxTraceDepth = 2;  // ????+ ????
    // OptiX 8: OptixPipelineLinkOptions ????maxTraceDepth?? debugLevel
    
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
    VLR_DEBUG_PRINTF("[VLR] OptiX pipeline created successfully\n");
    
    // ???????? OptiX ??????
    const uint32_t maxTraceDepth = 2;  // ????+ ????
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
    
    // ????CUDA ??
    CUDA_CHECK(cudaEventCreate(&wf.startEvent));
    CUDA_CHECK(cudaEventCreate(&wf.endEvent));
    wf.eventsCreated = true;

    // ????CUDA Graphs ???
    wf.graphCaptured = false;
    wf.useGraphExecution = shared::PerformanceConfig::UseCudaGraphs;
    wf.renderGraph = nullptr;
    wf.renderGraphExec = nullptr;

    wf.isInitialized = true;
    VLR_DEBUG_PRINTF("[VLR] Pipeline initialization complete\n");
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
            VLR_DEBUG_PRINTF("[OptiX] Program group log:\n%.*s\n", static_cast<int>(logSize), logBuffer);
        }
        return pg;
    };
    
    // ========================================================================
    // 1. Ray Generation Program - traceRays
    // ??????????????????
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
    // ????????????????/????
    // ========================================================================
    {
        OptixProgramGroupDesc desc = {};
        desc.kind = OPTIX_PROGRAM_GROUP_KIND_MISS;
        desc.miss.module = wf.module;
        desc.miss.entryFunctionName = "__miss__miss";
        wf.missProgram = createProgramGroup(desc);
    }
    
    // ========================================================================
    // 3. Hit Group - Closest Hit??????Alpha ????
    // closestHit ????????hitInfoBuffer
    // ========================================================================
    {
        OptixProgramGroupDesc desc = {};
        desc.kind = OPTIX_PROGRAM_GROUP_KIND_HITGROUP;
        desc.hitgroup.moduleCH = wf.module;
        desc.hitgroup.entryFunctionNameCH = "__closesthit__closestHit";
        desc.hitgroup.moduleAH = nullptr;
        desc.hitgroup.entryFunctionNameAH = nullptr;
        desc.hitgroup.moduleIS = nullptr;
        desc.hitgroup.entryFunctionNameIS = nullptr;  // ??????????
        wf.hitGroupProgram = createProgramGroup(desc);
    }
    
    // ========================================================================
    // 4. Shadow Miss Program - shadowMiss
    // ????????????
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
    // ????????????????????
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
    
    VLR_DEBUG_PRINTF("[VLR] Program groups created (RayGen, Miss, HitGroup, ShadowMiss, ShadowHitGroup)\n");
}


void Context::createWavefrontSBT() {
    auto& wf = m_optix.wavefrontPathTracing;
    
    // ========================================================================
    // ?? optixu::createSBTRecord ?? SBT ??
    // SBT ???RayGen(1) | Miss(2: Closest + Shadow) | HitGroup(2: Closest + Shadow)
    // ?????? WavefrontSBTData?launch parameters ????
    // ========================================================================
    
    // SBT ??????launch parameters ?????? nullptr??????
    shared::WavefrontSBTData sbtData;
    sbtData.params = nullptr;  // ????setupWavefrontLaunchParams ????
    
    // 1. RayGen ??????WavefrontSBTData??
    wf.raygenRecord = optixu::createSBTRecord(wf.raygenProgram, sbtData);
    
    // 2. Miss ?? - ???2 ??RayType 0: miss, RayType 1: shadowMiss??
    wf.missRecord = optixu::createSBTRecord(wf.missProgram, sbtData);
    wf.shadowMissRecord = optixu::createSBTRecord(wf.shadowMissProgram, sbtData);
    
    // 3. HitGroup ?? - ???2 ??RayType 0: closest hit, RayType 1: shadow any hit??
    // ??????????????????SBT ???stride = 2
    wf.hitgroupRecord = optixu::createSBTRecord(wf.hitGroupProgram, sbtData);
    wf.shadowHitgroupRecord = optixu::createSBTRecord(wf.shadowHitGroupProgram, sbtData);
    
    // ========================================================================
    // ?? OptixShaderBindingTable ??
    // ========================================================================
    memset(&wf.sbt, 0, sizeof(wf.sbt));
    
    // RayGen ??
    wf.sbt.raygenRecord = reinterpret_cast<CUdeviceptr>(wf.raygenRecord);
    
    // Miss ??2 ????stride = OPTIX_SBT_RECORD_HEADER_SIZE + sizeof(WavefrontSBTData)
    // ????miss ??????
    // ???SBT ?? stride ????16 ????
    size_t missRecordSize = OPTIX_SBT_RECORD_HEADER_SIZE + sizeof(shared::WavefrontSBTData);
    missRecordSize = (missRecordSize + 15) & ~15;  // ??????16 ??
    wf.sbt.missRecordBase = reinterpret_cast<CUdeviceptr>(wf.missRecord);
    wf.sbt.missRecordStrideInBytes = static_cast<uint32_t>(missRecordSize);
    wf.sbt.missRecordCount = 2;  // Closest + Shadow
    
    // ???Miss ??????????[miss, shadowMiss]
    // ???????????????OptiX ?? missRecordBase ??????
    // ?? numRayTypes ????????????? miss ?????
    {
        // ??????2 ??Miss ??
        size_t totalMissSize = 2 * missRecordSize;
        void* missBuffer = nullptr;
        CUDA_CHECK(cudaMalloc(&missBuffer, totalMissSize));
        
        // ????????header + data????????
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
        
        // ??????
        CUDA_CHECK(cudaMemcpy(missBuffer, hostMissBuffer, totalMissSize, cudaMemcpyHostToDevice));
        free(hostMissBuffer);
        
        // ????????????????
        cudaFree(wf.missRecord);
        cudaFree(wf.shadowMissRecord);
        wf.missRecord = missBuffer;
        wf.shadowMissRecord = nullptr;  // ???? missRecord
        
        wf.sbt.missRecordBase = reinterpret_cast<CUdeviceptr>(wf.missRecord);
        wf.sbt.missRecordStrideInBytes = static_cast<uint32_t>(missRecordSize);
        wf.sbt.missRecordCount = 2;
    }
    
    // HitGroup ???? GAS ???RAY_TYPE_COUNT ????
    // ???[GAS0_Closest, GAS0_Shadow, GAS1_Closest, GAS1_Shadow, ...]
    // stride = RAY_TYPE_COUNT??????????
    // ???SBT ?? stride ????16 ????
    const uint32_t RAY_TYPE_COUNT = 2;  // Closest Hit + Shadow
    size_t hitgroupRecordSize = OPTIX_SBT_RECORD_HEADER_SIZE + sizeof(shared::WavefrontSBTData);
    hitgroupRecordSize = (hitgroupRecordSize + 15) & ~15;  // ??????16 ??
    
    // ???? SBT ?????????????? GAS
    // ?????????? HitGroup ?????? setupWavefrontLaunchParams ??????
    {
        // ??????RAY_TYPE_COUNT ??HitGroup ?????? 1 ??GAS??
        size_t totalHitgroupSize = RAY_TYPE_COUNT * hitgroupRecordSize;
        void* hitgroupBuffer = nullptr;
        CUDA_CHECK(cudaMalloc(&hitgroupBuffer, totalHitgroupSize));
        
        // ????????header + data
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
        
        // ??????
        CUDA_CHECK(cudaMemcpy(hitgroupBuffer, hostHitgroupBuffer, totalHitgroupSize, cudaMemcpyHostToDevice));
        free(hostHitgroupBuffer);
        
        cudaFree(wf.hitgroupRecord);
        cudaFree(wf.shadowHitgroupRecord);
        wf.hitgroupRecord = hitgroupBuffer;
        wf.shadowHitgroupRecord = nullptr;
        
        wf.sbt.hitgroupRecordBase = reinterpret_cast<CUdeviceptr>(wf.hitgroupRecord);
        wf.sbt.hitgroupRecordStrideInBytes = static_cast<uint32_t>(hitgroupRecordSize);
        wf.sbt.hitgroupRecordCount = RAY_TYPE_COUNT;  // ?? 1 ??GAS ? 2 ??Ray Types
    }
    
    VLR_DEBUG_PRINTF("[VLR] SBT created (RayGen, Miss x2, HitGroup x2)\n");
}


// ============================================================================
// ??????
// ============================================================================

void Context::allocateWavefrontBuffers(uint32_t width, uint32_t height) {
    auto& wf = m_optix.wavefrontPathTracing;
    
    uint32_t numPixels = width * height;
    
    // ?????????
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
    
    // ??????
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
    
    // ????????????
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
    
    // ????????
    if (!wf.accumBuffer) {
        wf.accumBuffer = std::make_unique<cudau::Buffer<shared::SpectrumStorage>>();
    }
    wf.accumBuffer->initialize(m_cudaContext, cudau::BufferType::Device, numPixels);
    wf.accumBuffer->clear(m_stream);
    
    if (!wf.rngBuffer) {
        wf.rngBuffer = std::make_unique<cudau::Buffer<shared::KernelRNG>>();
    }
    wf.rngBuffer->initialize(m_cudaContext, cudau::BufferType::Device, numPixels);
    
    // ????RNG ???????????????????
    // ???????????????????????
    uint64_t baseSeed = static_cast<uint64_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count());
    initializeRNGBuffer(wf.rngBuffer->getDevicePointer(), numPixels, baseSeed, m_stream);
    
    // ????????
    if (!wf.eventsCreated) {
        CUDA_CHECK(cudaEventCreate(&wf.startEvent));
        CUDA_CHECK(cudaEventCreate(&wf.endEvent));
        wf.eventsCreated = true;
    }
    
    // ???????????
    if (!wf.accumAlbedoBuffer) {
        wf.accumAlbedoBuffer = std::make_unique<cudau::Buffer<shared::DiscretizedSpectrum>>();
    }
    wf.accumAlbedoBuffer->initialize(m_cudaContext, cudau::BufferType::Device, numPixels);
    
    if (!wf.accumNormalBuffer) {
        wf.accumNormalBuffer = std::make_unique<cudau::Buffer<shared::Normal3D>>();
    }
    wf.accumNormalBuffer->initialize(m_cudaContext, cudau::BufferType::Device, numPixels);
    
    // ??????????
    if (!wf.perfStatsBuffer) {
        wf.perfStatsBuffer = std::make_unique<cudau::Buffer<uint32_t>>();
    }
    wf.perfStatsBuffer->initialize(m_cudaContext, cudau::BufferType::Device, 16);
    
    // ?? CUB ??????????????
    if (wf.usePathSorting || wf.useStreamCompaction) {
        // ??????????
        size_t sortBytes = 0;
        size_t compactBytes = 0;
        
        if (wf.usePathSorting) {
            sortBytes = shared::sortPathsByMaterialTempStorageBytes(numPixels);
            VLR_DEBUG_PRINTF("[VLR] CUB sort temp storage: %.2f KB\n", sortBytes / 1024.0f);
        }
        
        if (wf.useStreamCompaction) {
            compactBytes = shared::compactPathsCUBTempStorageBytes(numPixels);
            VLR_DEBUG_PRINTF("[VLR] CUB compact temp storage: %.2f KB\n", compactBytes / 1024.0f);
        }
        
        // ?????????????????????
        // ??????????????????fallback
        wf.cubTempStorageBytes = static_cast<size_t>(
            std::max(sortBytes, compactBytes) * shared::PerformanceConfig::CubTempStorageMultiplier);
        
        VLR_DEBUG_PRINTF("[VLR] CUB temp storage allocated: %.2f KB (multiplier: %.1fx)\n", 
               wf.cubTempStorageBytes / 1024.0f, 
               shared::PerformanceConfig::CubTempStorageMultiplier);
        
        if (wf.cubTempStorageBytes > 0) {
            if (!wf.cubTempStorage) {
                wf.cubTempStorage = std::make_unique<cudau::Buffer<uint8_t>>();
            }
            wf.cubTempStorage->initialize(m_cudaContext, cudau::BufferType::Device, wf.cubTempStorageBytes);
            
            // ????/????????
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
            
            VLR_DEBUG_PRINTF("[VLR] CUB buffers allocated (temp: %.2f KB)\n", wf.cubTempStorageBytes / 1024.0f);
        }
    }
    
    // LVC-BPT buffers
    if (wf.useBDPT) {
        uint32_t numLightPaths = numPixels;
        uint32_t maxLightVertices = numLightPaths * 4;
        
        if (!wf.lightVertexCacheBuffer) {
            wf.lightVertexCacheBuffer = std::make_unique<cudau::Buffer<shared::LightPathVertex>>();
        }
        wf.lightVertexCacheBuffer->initialize(m_cudaContext, cudau::BufferType::Device, maxLightVertices);
        
        if (!wf.numLightVerticesBuffer) {
            wf.numLightVerticesBuffer = std::make_unique<cudau::Buffer<uint32_t>>();
        }
        wf.numLightVerticesBuffer->initialize(m_cudaContext, cudau::BufferType::Device, 1);
        wf.numLightVerticesBuffer->clear(m_stream);
        
        if (!wf.lightPathStateBuffer) {
            wf.lightPathStateBuffer = std::make_unique<cudau::Buffer<shared::LightPathState>>();
        }
        wf.lightPathStateBuffer->initialize(m_cudaContext, cudau::BufferType::Device, numLightPaths);
        
        if (!wf.lightHitInfoBuffer) {
            wf.lightHitInfoBuffer = std::make_unique<cudau::Buffer<shared::WavefrontHitInfo>>();
        }
        wf.lightHitInfoBuffer->initialize(m_cudaContext, cudau::BufferType::Device, numLightPaths);
        
        if (!wf.lightSurfacePointBuffer) {
            wf.lightSurfacePointBuffer = std::make_unique<cudau::Buffer<shared::SurfacePoint>>();
        }
        wf.lightSurfacePointBuffer->initialize(m_cudaContext, cudau::BufferType::Device, numLightPaths);
        
        printf("[VLR] LVC-BPT buffers allocated: %u light paths, %u max vertices (%.2f MB)\n",
               numLightPaths, maxLightVertices,
               (maxLightVertices * sizeof(shared::LightPathVertex) +
                numLightPaths * sizeof(shared::LightPathState)) / (1024.0f * 1024.0f));
    }

    // ????
    wf.maxNumPaths = numPixels;
    wf.currentWidth = width;
    wf.currentHeight = height;
    
    VLR_DEBUG_PRINTF("[VLR] Buffers allocated: %ux%u (%u paths, ~%.2f MB)\n",
           width, height, numPixels,
           (numPixels * (sizeof(shared::WavefrontPathState) + 
                        sizeof(shared::WavefrontHitInfo) +
                        sizeof(shared::SurfacePoint))) / (1024.0f * 1024.0f));
}


void Context::resizeWavefrontBuffers(uint32_t width, uint32_t height) {
    auto& wf = m_optix.wavefrontPathTracing;
    
    if (wf.currentWidth == width && wf.currentHeight == height) {
        return;  // ??????
    }
    
    // ???????????????CUDA Graph
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

    // ??????cudaMemset ??????
    if (wf.queueCounters) {
        CUDA_CHECK(cudaMemset(wf.queueCounters->getDevicePointer(), 0, 2 * sizeof(uint32_t)));
    }

    if (wf.useMaterialQueues && wf.materialQueueCounters) {
        wf.materialQueueCounters->clear(m_stream);
    }
    
    // ????????????????????????????
    if (wf.pathStateBuffer) {
        wf.pathStateBuffer->clear(m_stream);
        CUDA_CHECK(cudaStreamSynchronize(m_stream));
    }
    
}


// ============================================================================
// ??????
// ============================================================================

void Context::setupWavefrontLaunchParams() {
    auto& wf = m_optix.wavefrontPathTracing;
    auto& lp = wf.launchParams;
    
    // ?????????
    lp.pathStateBuffer = wf.pathStateBuffer ? wf.pathStateBuffer->getDevicePointer() : nullptr;
    lp.hitInfoBuffer = wf.hitInfoBuffer ? wf.hitInfoBuffer->getDevicePointer() : nullptr;
    lp.surfacePointBuffer = wf.surfacePointBuffer ? wf.surfacePointBuffer->getDevicePointer() : nullptr;
    lp.pathTexturedParamsBuffer = wf.pathTexturedParamsBuffer ? wf.pathTexturedParamsBuffer->getDevicePointer() : nullptr;
    
    // ??????
    if (wf.activePathIndices && wf.queueCounters) {
        lp.activePathQueue.pathIndices = wf.activePathIndices->getDevicePointer();
        lp.activePathQueue.counter = wf.queueCounters->getDevicePointerAt(0);
        lp.activePathQueue.capacity = wf.maxNumPaths;
        
        static bool firstSetup = true;
        if (firstSetup) {
            VLR_DEBUG_PRINTF("[VLR] setupWavefrontLaunchParams: counter ptr=%p (from wf.queueCounters->getDevicePointerAt(0))\n", 
                   lp.activePathQueue.counter);
            VLR_DEBUG_PRINTF("[VLR] setupWavefrontLaunchParams: wf.queueCounters base ptr=%p\n",
                   wf.queueCounters->getDevicePointer());
            firstSetup = false;
        }
    }
    
    if (wf.nextActivePathIndices && wf.queueCounters) {
        lp.nextActivePathQueue.pathIndices = wf.nextActivePathIndices->getDevicePointer();
        lp.nextActivePathQueue.counter = wf.queueCounters->getDevicePointerAt(1);
        lp.nextActivePathQueue.capacity = wf.maxNumPaths;
    }
    
    // ??????
    if (wf.useMaterialQueues && wf.materialQueueCounters) {
        for (int i = 0; i < shared::NumMaterialCategories; ++i) {
            if (wf.materialQueueIndices[i]) {
                lp.materialQueues.queues[i].pathIndices = wf.materialQueueIndices[i]->getDevicePointer();
                lp.materialQueues.queues[i].counter = wf.materialQueueCounters->getDevicePointerAt(i);
                lp.materialQueues.queues[i].capacity = wf.maxNumPaths;
            }
        }
    }
    
    // ????????
    lp.rngBuffer = optixu::NativeBlockBuffer2D<shared::KernelRNG>();
    lp.rngBuffer.data = wf.rngBuffer ? wf.rngBuffer->getDevicePointer() : nullptr;
    lp.accumBuffer = optixu::BlockBuffer2D<shared::SpectrumStorage, 0>();
    lp.accumBuffer.data = wf.accumBuffer ? wf.accumBuffer->getDevicePointer() : nullptr;
    lp.accumAlbedoBuffer = wf.accumAlbedoBuffer ? wf.accumAlbedoBuffer->getDevicePointer() : nullptr;
    lp.accumNormalBuffer = wf.accumNormalBuffer ? wf.accumNormalBuffer->getDevicePointer() : nullptr;
    
    // ??????????Scene ??????
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
        // ??????????????????
        lp.instIndices = m_sceneSource->getLightInstIndices();

        // SceneBounds ?????????????
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
        lp.cameraDescriptor = m_scene.camera;  // ???? SceneData
        lp.progSampleLensPosition = -1;
        lp.progTestLensIntersection = -1;
        lp.progEvaluateIDF = -1;
    }
    
    // ?????????? VLR ???numAccumFrames ??????????
    lp.imageSize = make_uint2(wf.currentWidth, wf.currentHeight);
    lp.imageStrideInPixels = wf.currentWidth;
    lp.numAccumFrames = wf.numAccumFrames;
    lp.limitNumAccumFrames = 0;  // 0 = ????
    
    // ?? Wavefront ??
    lp.maxPathLength = wf.maxPathLength;
    lp.maxNumPaths = wf.maxNumPaths;
    lp.currentDepth = 0;
    
    // ????????
    lp.numActiveRays = wf.perfStatsBuffer ? wf.perfStatsBuffer->getDevicePointerAt(0) : nullptr;
    lp.numShadowRays = wf.perfStatsBuffer ? wf.perfStatsBuffer->getDevicePointerAt(1) : nullptr;
    lp.numTerminatedPaths = wf.perfStatsBuffer ? wf.perfStatsBuffer->getDevicePointerAt(2) : nullptr;
    
    // ????????????????????
    if (m_sceneSource) {
        const uint32_t numLights = m_sceneSource->getNumLightInsts();
        lp.lightInstDist.numValues = numLights;
        lp.envLightInstIndex = m_sceneSource->getEnvLightInstIndex();
        VLR_DEBUG_PRINTF("[VLR] renderWavefront: numLights=%u, envLightInstIndex=%u\n",
            numLights, lp.envLightInstIndex);
        lp.envImportanceMap = m_sceneSource->getEnvImportanceMap();
        lp.lightInstDist.weights = nullptr;
        lp.lightInstDist.cdf = nullptr;
        if (numLights > 0) {
            std::vector<float> weights, cdf;
            m_sceneSource->computeLightImportanceWeights(weights, cdf);
            if (!weights.empty() && !cdf.empty()) {
                if (!wf.lightImportanceWeightsBuffer || wf.lightImportanceWeightsBuffer->size() < weights.size()) {
                    if (!wf.lightImportanceWeightsBuffer)
                        wf.lightImportanceWeightsBuffer = std::make_unique<cudau::Buffer<float>>();
                    wf.lightImportanceWeightsBuffer->initialize(m_cudaContext, cudau::BufferType::Device, weights.size());
                    if (!wf.lightImportanceCDFBuffer)
                        wf.lightImportanceCDFBuffer = std::make_unique<cudau::Buffer<float>>();
                    wf.lightImportanceCDFBuffer->initialize(m_cudaContext, cudau::BufferType::Device, cdf.size());
                }
                wf.lightImportanceWeightsBuffer->copyToDevice(weights.data(), weights.size(), m_stream);
                wf.lightImportanceCDFBuffer->copyToDevice(cdf.data(), cdf.size(), m_stream);
                lp.lightInstDist.weights = wf.lightImportanceWeightsBuffer->getDevicePointer();
                lp.lightInstDist.cdf = wf.lightImportanceCDFBuffer->getDevicePointer();
            }
        }
    } else {
        lp.lightInstDist.weights = nullptr;
        lp.lightInstDist.cdf = nullptr;
        lp.lightInstDist.numValues = 0;
        lp.envLightInstIndex = 0xFFFFFFFF;
        lp.envImportanceMap.cdfTheta = nullptr;
        lp.envImportanceMap.cdfPhi = nullptr;
        lp.envImportanceMap.thetaRes = 0;
        lp.envImportanceMap.phiRes = 0;
        lp.envImportanceMap.totalLuminance = 1.0f;
    }

    // LVC-BPT params
    uint32_t numPixels = wf.currentWidth * wf.currentHeight;
    lp.useBDPT = wf.useBDPT;
    if (wf.useBDPT && wf.lightVertexCacheBuffer) {
        lp.lightVertexCache = wf.lightVertexCacheBuffer->getDevicePointer();
        lp.numLightVertices = wf.numLightVerticesBuffer->getDevicePointer();
        lp.lightPathStateBuffer = wf.lightPathStateBuffer->getDevicePointer();
        lp.lightHitInfoBuffer = wf.lightHitInfoBuffer->getDevicePointer();
        lp.lightSurfacePointBuffer = wf.lightSurfacePointBuffer->getDevicePointer();
        lp.numLightPaths = numPixels;
        lp.maxLightVertices = numPixels * 4;
    } else {
        lp.lightVertexCache = nullptr;
        lp.numLightVertices = nullptr;
        lp.lightPathStateBuffer = nullptr;
        lp.lightHitInfoBuffer = nullptr;
        lp.lightSurfacePointBuffer = nullptr;
        lp.numLightPaths = 0;
        lp.maxLightVertices = 0;
    }

    // ??????
    lp.probePixX = m_probePixelX;
    lp.probePixY = m_probePixelY;
    lp.debugMode = static_cast<uint32_t>(m_debugMode);
    
    // ????????????
    if (!wf.launchParamsBuffer) {
        CUDA_CHECK(cudaMalloc(&wf.launchParamsBuffer, sizeof(shared::WavefrontLaunchParameters)));
    }
    
    // ??????
    CUDA_CHECK(cudaMemcpyAsync(
        wf.launchParamsBuffer,
        &lp,
        sizeof(shared::WavefrontLaunchParameters),
        cudaMemcpyHostToDevice,
        m_stream
    ));
    
    // ????????????
    CUDA_CHECK(cudaStreamSynchronize(m_stream));
    
    // ??????????counter ??
    static bool firstVerify = true;
    if (firstVerify) {
        shared::WavefrontLaunchParameters lpVerify;
        CUDA_CHECK(cudaMemcpy(
            &lpVerify,
            wf.launchParamsBuffer,
            sizeof(shared::WavefrontLaunchParameters),
            cudaMemcpyDeviceToHost
        ));
        VLR_DEBUG_PRINTF("[VLR] setupWavefrontLaunchParams VERIFY: Device-side counter ptr=%p (expected %p)\n",
               lpVerify.activePathQueue.counter, lp.activePathQueue.counter);
        firstVerify = false;
    }
    
    // ========================================================================
    // ?? SBT ???? launch parameters ??
    // ========================================================================
    shared::WavefrontSBTData sbtData;
    sbtData.params = static_cast<shared::WavefrontLaunchParameters*>(wf.launchParamsBuffer);
    
    static bool firstSBTUpdate = true;
    if (firstSBTUpdate) {
        VLR_DEBUG_PRINTF("[VLR] setupWavefrontLaunchParams: sbtData.params=%p (wf.launchParamsBuffer)\n", sbtData.params);
        VLR_DEBUG_PRINTF("[VLR] setupWavefrontLaunchParams: Uploading sbtData to raygenRecord=%p\n", wf.raygenRecord);
        firstSBTUpdate = false;
    }
    
    // ?? RayGen ??
    CUDA_CHECK(cudaMemcpy(
        static_cast<char*>(wf.raygenRecord) + OPTIX_SBT_RECORD_HEADER_SIZE,
        &sbtData,
        sizeof(sbtData),
        cudaMemcpyHostToDevice
    ));
    
    // ?? Miss ???? ??- ??????createWavefrontSBT ????16 ???? stride
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
    
    // ?? HitGroup ???? ??- ??????createWavefrontSBT ????16 ???? stride
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
// ??
// ============================================================================

void Context::cleanupWavefrontResources() {
    auto& wf = m_optix.wavefrontPathTracing;
    
    // ???OptiX ??
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
    
    // ?? SBT ??
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
    
    // ??????????????????reset ??????
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
    wf.lightImportanceWeightsBuffer.reset();
    wf.lightImportanceCDFBuffer.reset();
    
    // ???CUDA ??
    if (wf.eventsCreated) {
        cudaEventDestroy(wf.startEvent);
        cudaEventDestroy(wf.endEvent);
        wf.eventsCreated = false;
    }

    // ???CUDA Graphs
    if (wf.graphCaptured) {
        cudaGraphExecDestroy(wf.renderGraphExec);
        cudaGraphDestroy(wf.renderGraph);
        wf.graphCaptured = false;
    }
    
    // ?? CUB ????
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
// ????
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
// ????
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

    VLR_DEBUG_PRINTF("[VLR] renderWavefront started: %ux%u, %u samples\n", width, height, numSamples);
    fflush(stdout);

    // ????????????????????
    if (m_sceneSource) {
        VLR_DEBUG_PRINTF("[VLR] Building acceleration structure...\n");
        fflush(stdout);
        const_cast<Scene*>(m_sceneSource)->buildAccelerationStructure();
        VLR_DEBUG_PRINTF("[VLR] Uploading scene data to GPU...\n");
        fflush(stdout);
        const_cast<Scene*>(m_sceneSource)->updateToGPU();
        m_scene.camera = m_sceneSource->getCamera();
        m_scene.bounds = m_sceneSource->getSceneBounds();
        VLR_DEBUG_PRINTF("[VLR] Scene ready\n");
        fflush(stdout);
    }

    // ????????
    if (wf.currentWidth != width || wf.currentHeight != height) {
        VLR_DEBUG_PRINTF("[VLR] Resizing buffers...\n");
        fflush(stdout);
        resizeWavefrontBuffers(width, height);
    }

    // ????VLR ?????????????????????
    wf.numAccumFrames = 0;
    if (wf.accumBuffer && wf.accumBuffer->size() > 0) {
        wf.accumBuffer->clear(m_stream);
        CUDA_CHECK(cudaStreamSynchronize(m_stream));
    }

    // ??????
    VLR_DEBUG_PRINTF("[VLR] Setting launch parameters...\n");
    fflush(stdout);
    setupWavefrontLaunchParams();

    // ???????
    CUDA_CHECK(cudaEventRecord(wf.startEvent, m_stream));

    // ?????????Normal ??????????????????????
    if (m_debugMode != VLRDebugMode_Normal) {
        VLR_DEBUG_PRINTF("[VLR] Debug mode: %s (single sample, no multi-bounce)\n", getDebugModeName(m_debugMode));
        fflush(stdout);
        wf.numAccumFrames = 1;
        executeWavefrontRenderDebug(static_cast<uint32_t>(m_debugMode));
    } else {
        // ??????
        printf("[VLR] Starting render loop...\n");
        fflush(stdout);
        for (uint32_t sample = 0; sample < numSamples; ++sample) {
            ++wf.numAccumFrames;
            executeWavefrontRender(1);
            // ?????sample????
            printf("\r[VLR] Progress: %u/%u samples (%.1f%%)", 
                   sample + 1, numSamples, 
                   (sample + 1) * 100.0f / numSamples);
            fflush(stdout);
        }
        printf("\n");  // ?????
        fflush(stdout);
    }
    
    // ??????????????
    CUDA_CHECK(cudaEventRecord(wf.endEvent, m_stream));
    CUDA_CHECK(cudaEventSynchronize(wf.endEvent));
    
    float renderTimeMs = 0.0f;
    CUDA_CHECK(cudaEventElapsedTime(&renderTimeMs, wf.startEvent, wf.endEvent));
    
    VLR_DEBUG_PRINTF("[VLR] Render completed in %.2f ms (%.2f ms/sample, %.2f Msamples/s)\n",
           renderTimeMs,
           renderTimeMs / numSamples,
           (width * height * numSamples) / (renderTimeMs * 1000.0f));
    fflush(stdout);
    
    // ??????????????????
    if (m_denoiserConfig.enabled && m_debugMode == VLRDebugMode_Normal && wf.accumBuffer) {
        VLR_DEBUG_PRINTF("[VLR] Applying OptiX denoiser...\n");
        fflush(stdout);
        
        // ????????????????
        if (!m_denoiser.isInitialized()) {
            m_denoiser.initialize(width, height, m_denoiserConfig, m_optix.context);
        }
        
        // ?????????? SpectrumStorage ????float3??
        // ????????????kernel??????????
        CUdeviceptr d_colorBuffer = reinterpret_cast<CUdeviceptr>(wf.accumBuffer->getDevicePointer());
        CUdeviceptr d_albedoBuffer = wf.accumAlbedoBuffer ? reinterpret_cast<CUdeviceptr>(wf.accumAlbedoBuffer->getDevicePointer()) : 0;
        CUdeviceptr d_normalBuffer = wf.accumNormalBuffer ? reinterpret_cast<CUdeviceptr>(wf.accumNormalBuffer->getDevicePointer()) : 0;
        
        // ????????????????????
        m_denoiser.denoise(d_colorBuffer, d_colorBuffer, d_albedoBuffer, d_normalBuffer, numSamples);
        
        VLR_DEBUG_PRINTF("[VLR] Denoising completed\n");
        fflush(stdout);
    }
    
    // ????????????
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

    // ????
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

    // ?? 1: ??????
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

    // ?? 2: ??????????????
    launchTraceRays(numPixels);

    // ?? 3: ????????surfacePointBuffer?accumAlbedo?accumNormal??
    launchProcessHits(numPixels);

    // ?? 4: ?????????? accumBuffer??
    launchRenderDebugMode(numPixels, debugMode);

    CUDA_CHECK(cudaStreamSynchronize(m_stream));
}

void Context::executeWavefrontRender(uint32_t numSamples) {
    auto& wf = m_optix.wavefrontPathTracing;

    VLR_DEBUG_PRINTF("[VLR] ========== executeWavefrontRender START (numSamples=%u) ==========\n", numSamples);
    fflush(stdout);

#ifdef VLR_DEBUG_NAN_TRACKING
    resetNanDebugCount();
#endif

    uint32_t numPixels = wf.currentWidth * wf.currentHeight;
    
    // ????
    resetWavefrontQueues();
    
    // ???????????? counter ????????????
    setupWavefrontLaunchParams();
    
    // ???????????? GPU ????
    if (wf.queueCounters) {
        uint32_t zero[2] = {0, 0};
        CUDA_CHECK(cudaMemcpy(
            wf.queueCounters->getDevicePointer(),
            zero,
            2 * sizeof(uint32_t),
            cudaMemcpyHostToDevice
        ));
    }
    
    // ?? 1: ??????
    launchGenerateRays(numPixels);
    
    // ????VLR ?????????????????????GPU ?????
    if (wf.queueCounters) {
        void* counterPtr = wf.queueCounters->getDevicePointerAt(0);
        VLR_DEBUG_PRINTF("[VLR] executeWavefrontRender: Setting counter at %p to %u\n", counterPtr, numPixels);
        CUDA_CHECK(cudaMemcpyAsync(
            counterPtr,
            &numPixels,
            sizeof(uint32_t),
            cudaMemcpyHostToDevice,
            m_stream
        ));
        // ??????????
        CUDA_CHECK(cudaStreamSynchronize(m_stream));
        VLR_DEBUG_PRINTF("[VLR] executeWavefrontRender: Counter set complete\n");
    }
    
    // LVC-BPT: Generate light paths before the main eye path loop
    if (wf.useBDPT && wf.numLightVerticesBuffer && wf.lightVertexCacheBuffer) {
        uint32_t zero = 0;
        CUDA_CHECK(cudaMemcpy(
            wf.numLightVerticesBuffer->getDevicePointer(),
            &zero,
            sizeof(uint32_t),
            cudaMemcpyHostToDevice
        ));
        
        shared::WavefrontLaunchParameters* d_params =
            static_cast<shared::WavefrontLaunchParameters*>(wf.launchParamsBuffer);
        launchGenerateLightPathsKernel(d_params, numPixels, m_stream);
        CUDA_CHECK(cudaStreamSynchronize(m_stream));
        
        uint32_t numLV = 0;
        CUDA_CHECK(cudaMemcpy(
            &numLV,
            wf.numLightVerticesBuffer->getDevicePointer(),
            sizeof(uint32_t),
            cudaMemcpyDeviceToHost
        ));
        if (wf.numAccumFrames <= 1) {
            printf("[VLR-BDPT] Light vertices generated: %u\n", numLV);
        }
    }

        // ??Wavefront ??
        // ????????????????????
        constexpr uint32_t SYNC_INTERVAL = shared::PerformanceConfig::SyncInterval;
        uint32_t numActivePaths = numPixels;  // ??????????
        
        for (uint32_t depth = 0; depth < wf.maxPathLength; ++depth) {
        wf.launchParams.currentDepth = depth;
        
        // ????????????????????????
        if (depth % SYNC_INTERVAL == 0 && depth > 0) {
            if (wf.queueCounters) {
                wf.queueCounters->copyToHost(&numActivePaths, 1, m_stream);
                CUDA_CHECK(cudaStreamSynchronize(m_stream));
            }
            
            // ??????????????????
            constexpr float EARLY_TERMINATION_THRESHOLD = shared::PerformanceConfig::EarlyTerminationThreshold;
            constexpr uint32_t MIN_DEPTH = shared::PerformanceConfig::EarlyTerminationMinDepth;
            uint32_t minPaths = static_cast<uint32_t>(numPixels * EARLY_TERMINATION_THRESHOLD);
            
            if (numActivePaths == 0) {
                break;  // ???????
            } else if (numActivePaths < minPaths && depth > MIN_DEPTH) {
                // ????????????????
                break;
            }
        }
        
        // ?? 2: ????
        launchTraceRays(numActivePaths);
        
        // ?? 3: ????
        launchProcessHits(numActivePaths);
        
        // ?? 4: ???? (NEE)
        launchSampleLights(numActivePaths);
        
        // ?? 5: ?? BSDF
        launchSampleBSDF(numActivePaths);
        
        // ?? 6: ????????
        // ??????????
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
        
        // ???????????????????????????
        constexpr float COMPRESSION_THRESHOLD = shared::PerformanceConfig::CompressionThreshold;
        constexpr uint32_t MIN_PATHS = shared::PerformanceConfig::MinPathsForCompression;
        float compressionRatio = (numActivePaths > 0) ? 
            static_cast<float>(numNextPaths) / numActivePaths : 0.0f;
        bool shouldCompress = (compressionRatio < COMPRESSION_THRESHOLD) && 
                             (numNextPaths > MIN_PATHS);
        
        if (wf.useStreamCompaction && shouldCompress) {
            // ?? CUB Stream Compaction ????????
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
            
            // ????????
            uint32_t zero = 0;
            CUDA_CHECK(cudaMemcpyAsync(
                wf.queueCounters->getDevicePointerAt(1),
                &zero,
                sizeof(uint32_t),
                cudaMemcpyHostToDevice,
                m_stream
            ));
        } else if (wf.usePathSorting && numNextPaths > 0 && depth > 0) {
            // ?? CUB RadixSort ??????
            // ????????0????????0?materialCategory????
            // ????????????????????
            size_t requiredTempBytes = shared::sortPathsByMaterialTempStorageBytes(numNextPaths);
            
            // ?????????????????
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

                // ????????
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

                
                // ????????counters[0] = counters[1], counters[1] = 0
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
            // ???????????
            std::swap(wf.activePathIndices, wf.nextActivePathIndices);
            
            // ????????counters[0] = counters[1], counters[1] = 0
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
    
    // ?? 6: ????
    launchAccumulate(numPixels);
    CUDA_CHECK(cudaStreamSynchronize(m_stream));
}


// ============================================================================
// ??????
// ============================================================================

void Context::launchGenerateRays(uint32_t numPaths) {
    auto& wf = m_optix.wavefrontPathTracing;
    if (!wf.launchParamsBuffer) return;

    // ???????? grid/block????generateRays CUDA kernel
    uint32_t width = wf.currentWidth;
    uint32_t height = wf.currentHeight;
    if (width == 0 || height == 0) return;

    shared::WavefrontLaunchParameters* d_params =
        static_cast<shared::WavefrontLaunchParameters*>(wf.launchParamsBuffer);

    launchGenerateRaysKernel(d_params, width, height, m_stream);
}

void Context::launchTraceRays(uint32_t numActivePaths) {
    auto& wf = m_optix.wavefrontPathTracing;

    // ?? optixLaunch ?? OptiX Ray Generation ??????SBT ??launchParams
    if (!wf.pipeline) {
        throw std::runtime_error("launchTraceRays: OptiX pipeline not initialized, call createWavefrontPrograms first");
    }
    if (!wf.launchParamsBuffer) return;
    if (numActivePaths == 0) return;

    // ?????? launchParams ????????????
    static bool firstCall = true;
    if (firstCall) {
        VLR_DEBUG_PRINTF("[VLR] launchTraceRays: Re-uploading entire launchParams to ensure consistency\n");
        VLR_DEBUG_PRINTF("[VLR] launchTraceRays HOST: counter ptr=%p, imageSize=(%u,%u), maxPathLength=%u\n",
               wf.launchParams.activePathQueue.counter,
               wf.launchParams.imageSize.x, wf.launchParams.imageSize.y,
               wf.launchParams.maxPathLength);
        VLR_DEBUG_PRINTF("[VLR] launchTraceRays HOST: pathStateBuffer=%p, topGroup=%llu\n",
               wf.launchParams.pathStateBuffer, (unsigned long long)wf.launchParams.topGroup);
        VLR_DEBUG_PRINTF("[VLR] launchTraceRays HOST: sizeof(WavefrontLaunchParameters)=%zu\n",
               sizeof(shared::WavefrontLaunchParameters));
        VLR_DEBUG_PRINTF("[VLR] launchTraceRays HOST: offsetof(pathStateBuffer)=%zu, offsetof(activePathQueue)=%zu\n",
               offsetof(shared::WavefrontLaunchParameters, pathStateBuffer),
               offsetof(shared::WavefrontLaunchParameters, activePathQueue));
        firstCall = false;
    }
    
    CUDA_CHECK(cudaMemcpy(
        wf.launchParamsBuffer,
        &wf.launchParams,
        sizeof(shared::WavefrontLaunchParameters),
        cudaMemcpyHostToDevice
    ));
    
    // ????????
    CUDA_CHECK(cudaStreamSynchronize(m_stream));
    
    // ??????
    static bool firstVerify = true;
    if (firstVerify) {
        shared::WavefrontLaunchParameters lpVerify;
        CUDA_CHECK(cudaMemcpy(
            &lpVerify,
            wf.launchParamsBuffer,
            sizeof(shared::WavefrontLaunchParameters),
            cudaMemcpyDeviceToHost
        ));
        VLR_DEBUG_PRINTF("[VLR] launchTraceRays VERIFY after re-upload: Device counter ptr=%p\n",
               lpVerify.activePathQueue.counter);
        firstVerify = false;
    }

    try {
        // ??????? SBT ?????launch parameters????launchParams ???? 0
        OptixResult launchResult = optixLaunch(
            wf.pipeline,
            m_stream,
            0,  // ????launchParams??? SBT ???
            0,
            &wf.sbt,
            numActivePaths,  // ?????????????
            1,
            1
        );
        if (launchResult != OPTIX_SUCCESS) {
            fprintf(stderr, "[VLR] Error: optixLaunch failed - %s (%d)\n",
                    optixGetErrorName(launchResult), launchResult);
            throw std::runtime_error(std::string("optixLaunch failed: ") + optixGetErrorName(launchResult));
        }
        CUDA_CHECK(cudaStreamSynchronize(m_stream));
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("launchTraceRays: optixLaunch failed - ") + e.what());
    }
}

void Context::launchProcessHits(uint32_t numActivePaths) {
    auto& wf = m_optix.wavefrontPathTracing;
    if (!wf.launchParamsBuffer) return;
    if (numActivePaths == 0) return;

    // ?? processHits CUDA kernel
    shared::WavefrontLaunchParameters* d_params =
        static_cast<shared::WavefrontLaunchParameters*>(wf.launchParamsBuffer);

    launchProcessHitsKernel(d_params, numActivePaths, m_stream);
}

void Context::launchSampleLights(uint32_t numActivePaths) {
    auto& wf = m_optix.wavefrontPathTracing;
    if (!wf.launchParamsBuffer) return;
    if (numActivePaths == 0) return;

    // ?? sampleLights CUDA kernel
    shared::WavefrontLaunchParameters* d_params =
        static_cast<shared::WavefrontLaunchParameters*>(wf.launchParamsBuffer);

    launchSampleLightsKernel(d_params, numActivePaths, m_stream);
}

void Context::launchSampleBSDF(uint32_t numActivePaths) {
    auto& wf = m_optix.wavefrontPathTracing;
    if (!wf.launchParamsBuffer) return;
    if (numActivePaths == 0) return;

    // ?? sampleBSDF CUDA kernel
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

    // ?? accumulateResults CUDA kernel
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
// ????
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
    // ?????syncInterval?block sizes ?????? m_perfConfig???? kernel ????
}


// ============================================================================
// ????
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
        VLR_DEBUG_PRINTF("[VLR] createTexture2D: invalid arguments (null)\n");
        return false;
    }
    TextureImage img;
    std::string error;
    if (!loadTextureImage(imagePath, img, &error)) {
        VLR_DEBUG_PRINTF("[VLR] createTexture2D: failed to load '%s': %s\n", imagePath, error.c_str());
        return false;
    }
    shared::TextureFormat fmt = (img.format == TextureImageFormat::RGBA8)
        ? shared::TextureFormat_RGBA8
        : shared::TextureFormat_RGBA32F;
    bool ok = createTexture2DFromMemory(img.data, img.width, img.height,
                                        static_cast<uint32_t>(img.format), outTextureIndex);
    freeTextureImage(img);
    if (ok) {
        VLR_DEBUG_PRINTF("[VLR] createTexture2D: loaded '%s' %ux%u -> texture index %u\n",
               imagePath, img.width, img.height, *outTextureIndex);
    }
    return ok;
}

bool Context::createTexture2DFromMemory(const void* data, uint32_t width, uint32_t height,
                                       uint32_t format, uint32_t* outTextureIndex) {
    if (!data || width == 0 || height == 0 || !outTextureIndex) {
        VLR_DEBUG_PRINTF("[VLR] createTexture2DFromMemory: invalid arguments\n");
        return false;
    }
    if (format > 2) {
        VLR_DEBUG_PRINTF("[VLR] createTexture2DFromMemory: invalid format %u (0=RGBA8, 1=RGB32F, 2=RGBA32F)\n", format);
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
        VLR_DEBUG_PRINTF("[VLR] createTexture2DFromMemory: created texture %u %ux%u format %u\n",
               idx, width, height, format);
        return true;
    } catch (const std::exception& e) {
        VLR_DEBUG_PRINTF("[VLR] createTexture2DFromMemory: exception: %s\n", e.what());
        return false;
    }
}

void Context::destroyTexture(uint32_t textureIndex) {
    if (textureIndex >= m_textures.size()) {
        VLR_DEBUG_PRINTF("[VLR] destroyTexture: invalid index %u (max %zu)\n",
               textureIndex, m_textures.size());
        return;
    }
    m_textures[textureIndex].gpuBuffer.reset();
    m_textures[textureIndex].descriptor = shared::Texture2DDescriptor();
    m_textureDescriptorBufferDirty = true;
    VLR_DEBUG_PRINTF("[VLR] destroyTexture: texture %u destroyed\n", textureIndex);
}

bool Context::setTextureFilterMode(uint32_t textureIndex, uint32_t filterMode) {
    if (textureIndex >= m_textures.size()) {
        VLR_DEBUG_PRINTF("[VLR] setTextureFilterMode: invalid index %u\n", textureIndex);
        return false;
    }
    if (filterMode > 1) {
        VLR_DEBUG_PRINTF("[VLR] setTextureFilterMode: invalid mode %u (0=Nearest, 1=Linear)\n", filterMode);
        return false;
    }
    m_textures[textureIndex].filterMode = static_cast<shared::TextureFilterMode>(filterMode);
    return true;
}

bool Context::setTextureWrapMode(uint32_t textureIndex, uint32_t wrapU, uint32_t wrapV) {
    if (textureIndex >= m_textures.size()) {
        VLR_DEBUG_PRINTF("[VLR] setTextureWrapMode: invalid index %u\n", textureIndex);
        return false;
    }
    if (wrapU > 1 || wrapV > 1) {
        VLR_DEBUG_PRINTF("[VLR] setTextureWrapMode: invalid wrap mode (0=Repeat, 1=Clamp)\n");
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
// ????
// ============================================================================

const shared::WavefrontPerformanceStats& Context::getPerformanceStats() const {
    return m_optix.wavefrontPathTracing.perfStats;
}

void Context::resetPerformanceStats() {
    m_optix.wavefrontPathTracing.perfStats.reset();
}


// ============================================================================
// ??????
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
// ?????
// ============================================================================

void Context::checkOptixError(OptixResult result, const char* call, const char* file, int line) {
    if (result != OPTIX_SUCCESS) {
        char msg[1024];
        sprintf(msg, "OptiX Error at %s:%d\n  %s\n  Error: %s (%d)",
                 file, line, call, optixGetErrorName(result), result);
        throw std::runtime_error(msg);
    }
}

void Context::checkCudaError(cudaError_t error, const char* call, const char* file, int line) {
    if (error != cudaSuccess) {
        char msg[1024];
        sprintf(msg, "CUDA Error at %s:%d\n  %s\n  Error: %s (%d)",
                 file, line, call, cudaGetErrorString(error), error);
        throw std::runtime_error(msg);
    }
}

// ============================================================================
// ??????
// ============================================================================

void Context::setDenoiserConfig(const DenoiserConfig& config) {
    m_denoiserConfig = config;
}

const DenoiserConfig& Context::getDenoiserConfig() const {
    return m_denoiserConfig;
}

// ============================================================================
// ??????????
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
