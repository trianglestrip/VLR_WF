#ifdef VLR_ENABLE_CPU_DEBUG
    #define VLR_DEBUG_PRINTF(...) printf(__VA_ARGS__)
#else
    #define VLR_DEBUG_PRINTF(...) ((void)0)
#endif

#include "context.h"
#include "scene.h"
#include "GPU_kernels/kernel_launch.h"
#include "utils/cuda_util.h"
#include "utils/optix_util.h"
#ifdef _WIN32
#undef max
#undef min
#endif
#include <optix_function_table_definition.h>
#include <optix_stack_size.h>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>
#include "shared/performance_config.h"

namespace vlr {

namespace {

std::vector<char> loadPTXFile(const char* filename) {
    const char* searchPaths[] = {
        "bin/GPU_kernels/",
        "GPU_kernels/",
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

}  // anonymous namespace

void optixLogCallback(unsigned int level, const char* tag, const char* message, void* cbdata) {
    VLR_DEBUG_PRINTF("[OptiX][%s] %s\n", tag, message);
    (void)level;
    (void)cbdata;
}

void Context::initializeWavefrontPipeline() {
    auto& wf = m_optix.wavefrontPathTracing;

    if (wf.isInitialized) {
        return;
    }

    std::vector<char> ptxCode;
    try {
        ptxCode = loadPTXFile("trace_rays.ptx");
    } catch (const std::exception& e) {
        fprintf(stderr, "[VLR] Error: PTX load failed - %s\n", e.what());
        throw;
    }

    OptixPipelineCompileOptions pipelineCompileOptions = {
        .usesMotionBlur = false,
        .traversableGraphFlags = OPTIX_TRAVERSABLE_GRAPH_FLAG_ALLOW_SINGLE_LEVEL_INSTANCING,
        .numPayloadValues = 7,
        .numAttributeValues = 2,
        .exceptionFlags = OPTIX_EXCEPTION_FLAG_NONE,
        .pipelineLaunchParamsVariableName = nullptr
    };

    OptixModuleCompileOptions moduleCompileOptions = {
        .maxRegisterCount = OPTIX_COMPILE_DEFAULT_MAX_REGISTER_COUNT,
        .optLevel = OPTIX_COMPILE_OPTIMIZATION_DEFAULT,
        .debugLevel = OPTIX_COMPILE_DEBUG_LEVEL_MINIMAL
    };

    char moduleLog[2048];
    size_t moduleLogSize = sizeof(moduleLog);

    OptixResult moduleRes = optixModuleCreate(
        m_optix.context,
        &moduleCompileOptions,
        &pipelineCompileOptions,
        ptxCode.data(),
        ptxCode.size() - 1,
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

    try {
        createWavefrontSBT();
    } catch (const std::exception& e) {
        fprintf(stderr, "[VLR] Error: createWavefrontSBT failed - %s\n", e.what());
        cleanupWavefrontResources();
        throw;
    }

    OptixProgramGroup programGroups[] = {
        wf.raygenProgram,
        wf.missProgram,
        wf.hitGroupProgram,
        wf.shadowMissProgram,
        wf.shadowHitGroupProgram,
        wf.lightRaygenProgram,
        wf.lightHitGroupProgram,
        wf.lightMissProgram,
        wf.shadowRaygenProgram
    };
    const uint32_t numProgramGroups = sizeof(programGroups) / sizeof(programGroups[0]);

    OptixPipelineLinkOptions pipelineLinkOptions = {};
    pipelineLinkOptions.maxTraceDepth = 2;

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

    const uint32_t maxTraceDepth = 2;
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
        0,
        0,
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
            2
        );
    }
    if (stackRes != OPTIX_SUCCESS) {
        fprintf(stderr, "[VLR] Warning: stack size setup failed - %s (%d), may affect rendering\n",
                optixGetErrorName(stackRes), stackRes);
    }

    CUDA_CHECK(cudaEventCreate(&wf.startEvent));
    CUDA_CHECK(cudaEventCreate(&wf.endEvent));
    wf.eventsCreated = true;

    wf.graphCaptured = false;
    wf.useGraphExecution = shared::PerformanceConfig::UseCudaGraphs;
    wf.renderGraph = nullptr;
    wf.renderGraphExec = nullptr;

    wf.isInitialized = true;
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
        return pg;
    };

    {
        OptixProgramGroupDesc desc = {};
        desc.kind = OPTIX_PROGRAM_GROUP_KIND_RAYGEN;
        desc.raygen.module = wf.module;
        desc.raygen.entryFunctionName = "__raygen__traceRays";
        wf.raygenProgram = createProgramGroup(desc);
    }

    {
        OptixProgramGroupDesc desc = {};
        desc.kind = OPTIX_PROGRAM_GROUP_KIND_MISS;
        desc.miss.module = wf.module;
        desc.miss.entryFunctionName = "__miss__miss";
        wf.missProgram = createProgramGroup(desc);
    }

    {
        OptixProgramGroupDesc desc = {};
        desc.kind = OPTIX_PROGRAM_GROUP_KIND_HITGROUP;
        desc.hitgroup.moduleCH = wf.module;
        desc.hitgroup.entryFunctionNameCH = "__closesthit__closestHit";
        desc.hitgroup.moduleAH = nullptr;
        desc.hitgroup.entryFunctionNameAH = nullptr;
        desc.hitgroup.moduleIS = nullptr;
        desc.hitgroup.entryFunctionNameIS = nullptr;
        wf.hitGroupProgram = createProgramGroup(desc);
    }

    {
        OptixProgramGroupDesc desc = {};
        desc.kind = OPTIX_PROGRAM_GROUP_KIND_MISS;
        desc.miss.module = wf.module;
        desc.miss.entryFunctionName = "__miss__shadowMiss";
        wf.shadowMissProgram = createProgramGroup(desc);
    }

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

    {
        OptixProgramGroupDesc desc = {};
        desc.kind = OPTIX_PROGRAM_GROUP_KIND_RAYGEN;
        desc.raygen.module = wf.module;
        desc.raygen.entryFunctionName = "__raygen__traceLightRays";
        wf.lightRaygenProgram = createProgramGroup(desc);
    }

    {
        OptixProgramGroupDesc desc = {};
        desc.kind = OPTIX_PROGRAM_GROUP_KIND_HITGROUP;
        desc.hitgroup.moduleCH = wf.module;
        desc.hitgroup.entryFunctionNameCH = "__closesthit__lightClosestHit";
        desc.hitgroup.moduleAH = nullptr;
        desc.hitgroup.entryFunctionNameAH = nullptr;
        desc.hitgroup.moduleIS = nullptr;
        desc.hitgroup.entryFunctionNameIS = nullptr;
        wf.lightHitGroupProgram = createProgramGroup(desc);
    }

    {
        OptixProgramGroupDesc desc = {};
        desc.kind = OPTIX_PROGRAM_GROUP_KIND_MISS;
        desc.miss.module = wf.module;
        desc.miss.entryFunctionName = "__miss__lightMiss";
        wf.lightMissProgram = createProgramGroup(desc);
    }

    {
        OptixProgramGroupDesc desc = {};
        desc.kind = OPTIX_PROGRAM_GROUP_KIND_RAYGEN;
        desc.raygen.module = wf.module;
        desc.raygen.entryFunctionName = "__raygen__traceShadowRays";
        wf.shadowRaygenProgram = createProgramGroup(desc);
    }
}


void Context::createWavefrontSBT() {
    auto& wf = m_optix.wavefrontPathTracing;

    shared::WavefrontSBTData sbtData;
    sbtData.params = nullptr;

    wf.raygenRecord = optixu::createSBTRecord(wf.raygenProgram, sbtData);
    wf.missRecord = optixu::createSBTRecord(wf.missProgram, sbtData);
    wf.shadowMissRecord = optixu::createSBTRecord(wf.shadowMissProgram, sbtData);
    wf.hitgroupRecord = optixu::createSBTRecord(wf.hitGroupProgram, sbtData);
    wf.shadowHitgroupRecord = optixu::createSBTRecord(wf.shadowHitGroupProgram, sbtData);

    memset(&wf.sbt, 0, sizeof(wf.sbt));
    wf.sbt.raygenRecord = reinterpret_cast<CUdeviceptr>(wf.raygenRecord);

    size_t missRecordSize = OPTIX_SBT_RECORD_HEADER_SIZE + sizeof(shared::WavefrontSBTData);
    missRecordSize = (missRecordSize + 15) & ~15;
    wf.sbt.missRecordBase = reinterpret_cast<CUdeviceptr>(wf.missRecord);
    wf.sbt.missRecordStrideInBytes = static_cast<uint32_t>(missRecordSize);
    wf.sbt.missRecordCount = 2;

    {
        size_t totalMissSize = 2 * missRecordSize;
        void* missBuffer = nullptr;
        CUDA_CHECK(cudaMalloc(&missBuffer, totalMissSize));

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

        CUDA_CHECK(cudaMemcpy(missBuffer, hostMissBuffer, totalMissSize, cudaMemcpyHostToDevice));
        free(hostMissBuffer);

        cudaFree(wf.missRecord);
        cudaFree(wf.shadowMissRecord);
        wf.missRecord = missBuffer;
        wf.shadowMissRecord = nullptr;

        wf.sbt.missRecordBase = reinterpret_cast<CUdeviceptr>(wf.missRecord);
        wf.sbt.missRecordStrideInBytes = static_cast<uint32_t>(missRecordSize);
        wf.sbt.missRecordCount = 2;
    }

    const uint32_t RAY_TYPE_COUNT = 2;
    size_t hitgroupRecordSize = OPTIX_SBT_RECORD_HEADER_SIZE + sizeof(shared::WavefrontSBTData);
    hitgroupRecordSize = (hitgroupRecordSize + 15) & ~15;

    {
        size_t totalHitgroupSize = RAY_TYPE_COUNT * hitgroupRecordSize;
        void* hitgroupBuffer = nullptr;
        CUDA_CHECK(cudaMalloc(&hitgroupBuffer, totalHitgroupSize));

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

        CUDA_CHECK(cudaMemcpy(hitgroupBuffer, hostHitgroupBuffer, totalHitgroupSize, cudaMemcpyHostToDevice));
        free(hostHitgroupBuffer);

        cudaFree(wf.hitgroupRecord);
        cudaFree(wf.shadowHitgroupRecord);
        wf.hitgroupRecord = hitgroupBuffer;
        wf.shadowHitgroupRecord = nullptr;

        wf.sbt.hitgroupRecordBase = reinterpret_cast<CUdeviceptr>(wf.hitgroupRecord);
        wf.sbt.hitgroupRecordStrideInBytes = static_cast<uint32_t>(hitgroupRecordSize);
        wf.sbt.hitgroupRecordCount = RAY_TYPE_COUNT;
    }

    {
        memset(&wf.lightSbt, 0, sizeof(wf.lightSbt));

        wf.lightRaygenRecord = optixu::createSBTRecord(wf.lightRaygenProgram, sbtData);
        wf.lightSbt.raygenRecord = reinterpret_cast<CUdeviceptr>(wf.lightRaygenRecord);

        size_t lightMissRecordSize = (OPTIX_SBT_RECORD_HEADER_SIZE + sizeof(shared::WavefrontSBTData) + 15) & ~15;
        {
            size_t totalSize = 2 * lightMissRecordSize;
            void* buf = nullptr;
            CUDA_CHECK(cudaMalloc(&buf, totalSize));
            void* hostBuf = malloc(totalSize);
            OPTIX_CHECK(optixSbtRecordPackHeader(wf.lightMissProgram, hostBuf));
            memcpy(static_cast<char*>(hostBuf) + OPTIX_SBT_RECORD_HEADER_SIZE, &sbtData, sizeof(sbtData));
            OPTIX_CHECK(optixSbtRecordPackHeader(wf.shadowMissProgram,
                static_cast<char*>(hostBuf) + lightMissRecordSize));
            memcpy(static_cast<char*>(hostBuf) + lightMissRecordSize + OPTIX_SBT_RECORD_HEADER_SIZE,
                   &sbtData, sizeof(sbtData));
            CUDA_CHECK(cudaMemcpy(buf, hostBuf, totalSize, cudaMemcpyHostToDevice));
            free(hostBuf);
            wf.lightMissRecord = buf;
        }
        wf.lightSbt.missRecordBase = reinterpret_cast<CUdeviceptr>(wf.lightMissRecord);
        wf.lightSbt.missRecordStrideInBytes = static_cast<uint32_t>(lightMissRecordSize);
        wf.lightSbt.missRecordCount = 2;

        size_t lightHitRecordSize = (OPTIX_SBT_RECORD_HEADER_SIZE + sizeof(shared::WavefrontSBTData) + 15) & ~15;
        {
            size_t totalSize = 2 * lightHitRecordSize;
            void* buf = nullptr;
            CUDA_CHECK(cudaMalloc(&buf, totalSize));
            void* hostBuf = malloc(totalSize);
            OPTIX_CHECK(optixSbtRecordPackHeader(wf.lightHitGroupProgram, hostBuf));
            memcpy(static_cast<char*>(hostBuf) + OPTIX_SBT_RECORD_HEADER_SIZE, &sbtData, sizeof(sbtData));
            OPTIX_CHECK(optixSbtRecordPackHeader(wf.shadowHitGroupProgram,
                static_cast<char*>(hostBuf) + lightHitRecordSize));
            memcpy(static_cast<char*>(hostBuf) + lightHitRecordSize + OPTIX_SBT_RECORD_HEADER_SIZE,
                   &sbtData, sizeof(sbtData));
            CUDA_CHECK(cudaMemcpy(buf, hostBuf, totalSize, cudaMemcpyHostToDevice));
            free(hostBuf);
            wf.lightHitgroupRecord = buf;
        }
        wf.lightSbt.hitgroupRecordBase = reinterpret_cast<CUdeviceptr>(wf.lightHitgroupRecord);
        wf.lightSbt.hitgroupRecordStrideInBytes = static_cast<uint32_t>(lightHitRecordSize);
        wf.lightSbt.hitgroupRecordCount = RAY_TYPE_COUNT;
    }

    {
        memset(&wf.shadowSbt, 0, sizeof(wf.shadowSbt));

        wf.shadowRaygenRecord = optixu::createSBTRecord(wf.shadowRaygenProgram, sbtData);
        wf.shadowSbt.raygenRecord = reinterpret_cast<CUdeviceptr>(wf.shadowRaygenRecord);

        wf.shadowSbt.missRecordBase = wf.sbt.missRecordBase;
        wf.shadowSbt.missRecordStrideInBytes = wf.sbt.missRecordStrideInBytes;
        wf.shadowSbt.missRecordCount = wf.sbt.missRecordCount;
        wf.shadowSbt.hitgroupRecordBase = wf.sbt.hitgroupRecordBase;
        wf.shadowSbt.hitgroupRecordStrideInBytes = wf.sbt.hitgroupRecordStrideInBytes;
        wf.shadowSbt.hitgroupRecordCount = wf.sbt.hitgroupRecordCount;
    }
}


void Context::cleanupWavefrontResources() {
    auto& wf = m_optix.wavefrontPathTracing;

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

    if (wf.shadowRaygenProgram) {
        optixProgramGroupDestroy(wf.shadowRaygenProgram);
        wf.shadowRaygenProgram = nullptr;
    }
    if (wf.lightMissProgram) {
        optixProgramGroupDestroy(wf.lightMissProgram);
        wf.lightMissProgram = nullptr;
    }
    if (wf.lightHitGroupProgram) {
        optixProgramGroupDestroy(wf.lightHitGroupProgram);
        wf.lightHitGroupProgram = nullptr;
    }
    if (wf.lightRaygenProgram) {
        optixProgramGroupDestroy(wf.lightRaygenProgram);
        wf.lightRaygenProgram = nullptr;
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
    if (wf.lightRaygenRecord) {
        cudaFree(wf.lightRaygenRecord);
        wf.lightRaygenRecord = nullptr;
    }
    if (wf.lightMissRecord) {
        cudaFree(wf.lightMissRecord);
        wf.lightMissRecord = nullptr;
    }
    if (wf.lightHitgroupRecord) {
        cudaFree(wf.lightHitgroupRecord);
        wf.lightHitgroupRecord = nullptr;
    }
    if (wf.shadowRaygenRecord) {
        cudaFree(wf.shadowRaygenRecord);
        wf.shadowRaygenRecord = nullptr;
    }

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

    if (wf.eventsCreated) {
        cudaEventDestroy(wf.startEvent);
        cudaEventDestroy(wf.endEvent);
        wf.eventsCreated = false;
    }

    if (wf.graphCaptured) {
        cudaGraphExecDestroy(wf.renderGraphExec);
        cudaGraphDestroy(wf.renderGraph);
        wf.graphCaptured = false;
    }

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

} // namespace vlr
