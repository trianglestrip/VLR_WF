#include <optix.h>
#include <optix_function_table_definition.h>
#include <optix_stubs.h>
#include <cuda.h>
#include <cuda_runtime.h>
#include <stdio.h>
#include <stdlib.h>

static void optixLogCallback(unsigned int level, const char* tag, const char* message, void*) {
    fprintf(stderr, "[OptiX][%s] %s\n", tag, message);
}

int main() {
    printf("=== OptiX Minimal Test ===\n");
    
    // 1. Initialize CUDA
    printf("[1] Initializing CUDA...\n");
    cudaFree(0);
    
    CUcontext cuContext;
    CUresult cuRes = cuCtxGetCurrent(&cuContext);
    if (cuRes != CUDA_SUCCESS || !cuContext) {
        fprintf(stderr, "ERROR: Failed to get CUDA context: %d\n", cuRes);
        return 1;
    }
    printf("[1] CUDA context: %p\n", cuContext);
    
    // 2. Initialize OptiX
    printf("[2] Initializing OptiX...\n");
    OptixResult optixRes = optixInit();
    if (optixRes != OPTIX_SUCCESS) {
        fprintf(stderr, "ERROR: optixInit failed: %d\n", optixRes);
        return 1;
    }
    printf("[2] OptiX initialized\n");
    
    // 3. Create OptiX device context
    printf("[3] Creating OptiX device context...\n");
    OptixDeviceContextOptions options = {};
    options.logCallbackFunction = &optixLogCallback;
    options.logCallbackLevel = 4;
    
    // 禁用磁盘缓存以避免潜在的驱动bug
    options.validationMode = OPTIX_DEVICE_CONTEXT_VALIDATION_MODE_OFF;
    
    OptixDeviceContext context;
    optixRes = optixDeviceContextCreate(cuContext, &options, &context);
    if (optixRes == OPTIX_SUCCESS) {
        // 尝试禁用缓存
        optixDeviceContextSetCacheEnabled(context, 0);
        printf("[3] OptiX cache disabled\n");
    }
    if (optixRes != OPTIX_SUCCESS) {
        fprintf(stderr, "ERROR: optixDeviceContextCreate failed: %d\n", optixRes);
        return 1;
    }
    printf("[3] OptiX device context created: %p\n", context);
    
    // 4. Create a simple module (empty PTX)
    printf("[4] Creating OptiX module...\n");
    const char* ptxCode = R"(
.version 9.1
.target sm_75
.address_size 64

.visible .entry __raygen__test()
{
    ret;
}
)";
    
    OptixModuleCompileOptions moduleOptions = {};
    moduleOptions.maxRegisterCount = OPTIX_COMPILE_DEFAULT_MAX_REGISTER_COUNT;
    moduleOptions.optLevel = OPTIX_COMPILE_OPTIMIZATION_DEFAULT;
    moduleOptions.debugLevel = OPTIX_COMPILE_DEBUG_LEVEL_MINIMAL;
    
    OptixPipelineCompileOptions pipelineOptions = {};
    pipelineOptions.usesMotionBlur = false;
    pipelineOptions.traversableGraphFlags = OPTIX_TRAVERSABLE_GRAPH_FLAG_ALLOW_SINGLE_LEVEL_INSTANCING;
    pipelineOptions.numPayloadValues = 2;
    pipelineOptions.numAttributeValues = 2;
    pipelineOptions.exceptionFlags = OPTIX_EXCEPTION_FLAG_NONE;
    pipelineOptions.pipelineLaunchParamsVariableName = "params";
    
    char log[2048];
    size_t logSize = sizeof(log);
    OptixModule module;
    
    optixRes = optixModuleCreate(context, &moduleOptions, &pipelineOptions,
                                 ptxCode, strlen(ptxCode), log, &logSize, &module);
    if (optixRes != OPTIX_SUCCESS) {
        fprintf(stderr, "ERROR: optixModuleCreate failed: %s (%d)\n", optixGetErrorName(optixRes), optixRes);
        if (logSize > 1) {
            fprintf(stderr, "Log:\n%.*s\n", (int)logSize, log);
        }
        return 1;
    }
    printf("[4] Module created: %p\n", module);
    
    // 5. Create program group
    printf("[5] Creating program group...\n");
    OptixProgramGroupDesc pgDesc = {};
    pgDesc.kind = OPTIX_PROGRAM_GROUP_KIND_RAYGEN;
    pgDesc.raygen.module = module;
    pgDesc.raygen.entryFunctionName = "__raygen__test";
    
    OptixProgramGroupOptions pgOptions = {};
    OptixProgramGroup programGroup;
    logSize = sizeof(log);
    
    optixRes = optixProgramGroupCreate(context, &pgDesc, 1, &pgOptions, log, &logSize, &programGroup);
    if (optixRes != OPTIX_SUCCESS) {
        fprintf(stderr, "ERROR: optixProgramGroupCreate failed: %s (%d)\n", optixGetErrorName(optixRes), optixRes);
        if (logSize > 1) {
            fprintf(stderr, "Log:\n%.*s\n", (int)logSize, log);
        }
        return 1;
    }
    printf("[5] Program group created: %p\n", programGroup);
    
    // 6. Test optixSbtRecordPackHeader
    printf("[6] Testing optixSbtRecordPackHeader...\n");
    
    // 尝试使用主机内存分配
    void* sbtRecordHost = malloc(OPTIX_SBT_RECORD_HEADER_SIZE);
    if (!sbtRecordHost) {
        fprintf(stderr, "ERROR: malloc failed\n");
        return 1;
    }
    printf("[6] Host SBT record allocated: %p\n", sbtRecordHost);
    
    printf("[6] Calling optixSbtRecordPackHeader on HOST memory...\n");
    fflush(stdout);
    
    optixRes = optixSbtRecordPackHeader(programGroup, sbtRecordHost);
    
    printf("[6] optixSbtRecordPackHeader returned: %d\n", optixRes);
    
    if (optixRes != OPTIX_SUCCESS) {
        fprintf(stderr, "ERROR: optixSbtRecordPackHeader failed: %s (%d)\n", optixGetErrorName(optixRes), optixRes);
        free(sbtRecordHost);
        return 1;
    }
    
    printf("[6] SUCCESS! Now copying to device...\n");
    
    // 复制到设备内存
    void* sbtRecord;
    cudaError_t cudaErr = cudaMalloc(&sbtRecord, OPTIX_SBT_RECORD_HEADER_SIZE);
    if (cudaErr != cudaSuccess) {
        fprintf(stderr, "ERROR: cudaMalloc failed: %s\n", cudaGetErrorString(cudaErr));
        free(sbtRecordHost);
        return 1;
    }
    
    cudaErr = cudaMemcpy(sbtRecord, sbtRecordHost, OPTIX_SBT_RECORD_HEADER_SIZE, cudaMemcpyHostToDevice);
    if (cudaErr != cudaSuccess) {
        fprintf(stderr, "ERROR: cudaMemcpy failed: %s\n", cudaGetErrorString(cudaErr));
        free(sbtRecordHost);
        cudaFree(sbtRecord);
        return 1;
    }
    
    printf("[6] Copied to device: %p\n", sbtRecord);
    printf("[6] COMPLETE SUCCESS!\n");
    
    free(sbtRecordHost);
    
    // Cleanup
    cudaFree(sbtRecord);
    optixProgramGroupDestroy(programGroup);
    optixModuleDestroy(module);
    optixDeviceContextDestroy(context);
    
    printf("\n=== All tests passed ===\n");
    return 0;
}
