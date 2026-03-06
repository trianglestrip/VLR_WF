// ============================================================================
// OptiX 工具集
// 
// 本文件提供 OptiX 工具类和函数。
// 
// 作者: VLR 开发团队
// 创建日期: 2026-03-07
// 环境: CUDA 13.1, OptiX 8.0.0, VS2022
// ============================================================================

#pragma once

#include <optix.h>
#include <optix_stubs.h>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace vlr {
namespace optixu {

// ============================================================================
// 错误处理
// ============================================================================

inline void checkError(OptixResult result, const char* expr, const char* file, int line) {
    if (result != OPTIX_SUCCESS) {
        char msg[1024];
        snprintf(msg, sizeof(msg), "OptiX Error at %s:%d\n  %s\n  Error: %s (%d)",
                 file, line, expr, optixGetErrorName(result), result);
        throw std::runtime_error(msg);
    }
}

#define OPTIX_CHECK(call) ::vlr::optixu::checkError(call, #call, __FILE__, __LINE__)


// ============================================================================
// OptiX 上下文
// ============================================================================

class Context {
public:
    Context(bool enableLogging = false) : m_context(nullptr) {
        // 初始化 OptiX
        OPTIX_CHECK(optixInit());
        
        // 获取 CUDA 上下文
        CUcontext cuContext;
        cuCtxGetCurrent(&cuContext);
        
        // 创建 OptiX 设备上下文
        OptixDeviceContextOptions options = {};
        options.logCallbackFunction = enableLogging ? &logCallback : nullptr;
        options.logCallbackLevel = 4;  // 打印所有消息
        
        OPTIX_CHECK(optixDeviceContextCreate(cuContext, &options, &m_context));
    }
    
    ~Context() {
        if (m_context) {
            optixDeviceContextDestroy(m_context);
        }
    }
    
    OptixDeviceContext get() const { return m_context; }
    operator OptixDeviceContext() const { return m_context; }

private:
    OptixDeviceContext m_context;
    
    static void logCallback(unsigned int level, const char* tag, const char* message, void* cbdata) {
        printf("[OptiX][%s] %s\n", tag, message);
    }
};


// ============================================================================
// 模块
// ============================================================================

class Module {
public:
    Module() : m_module(nullptr) {}
    
    Module(OptixDeviceContext context, const char* ptxCode, size_t ptxSize,
           const OptixModuleCompileOptions* moduleCompileOptions,
           const OptixPipelineCompileOptions* pipelineCompileOptions) {
        
        char log[2048];
        size_t logSize = sizeof(log);
        
        OPTIX_CHECK(optixModuleCreateFromPTX(
            context,
            moduleCompileOptions,
            pipelineCompileOptions,
            ptxCode,
            ptxSize,
            log,
            &logSize,
            &m_module
        ));
        
        if (logSize > 1) {
            printf("OptiX Module Creation Log:\n%s\n", log);
        }
    }
    
    ~Module() {
        if (m_module) {
            optixModuleDestroy(m_module);
        }
    }
    
    OptixModule get() const { return m_module; }
    operator OptixModule() const { return m_module; }

private:
    OptixModule m_module;
};


// ============================================================================
// 管线
// ============================================================================

class Pipeline {
public:
    Pipeline() : m_pipeline(nullptr) {}
    
    Pipeline(OptixDeviceContext context,
             const OptixPipelineCompileOptions* pipelineCompileOptions,
             const OptixPipelineLinkOptions* pipelineLinkOptions,
             const OptixProgramGroup* programGroups,
             uint32_t numProgramGroups) {
        
        char log[2048];
        size_t logSize = sizeof(log);
        
        OPTIX_CHECK(optixPipelineCreate(
            context,
            pipelineCompileOptions,
            pipelineLinkOptions,
            programGroups,
            numProgramGroups,
            log,
            &logSize,
            &m_pipeline
        ));
        
        if (logSize > 1) {
            printf("OptiX Pipeline Creation Log:\n%s\n", log);
        }
    }
    
    ~Pipeline() {
        if (m_pipeline) {
            optixPipelineDestroy(m_pipeline);
        }
    }
    
    void setStackSize(uint32_t directCallableStackSize,
                      uint32_t continuationStackSize,
                      uint32_t maxTraversableDepth) {
        OPTIX_CHECK(optixPipelineSetStackSize(
            m_pipeline,
            directCallableStackSize,
            continuationStackSize,
            maxTraversableDepth,
            2  // 最大管线追踪深度
        ));
    }
    
    OptixPipeline get() const { return m_pipeline; }
    operator OptixPipeline() const { return m_pipeline; }

private:
    OptixPipeline m_pipeline;
};


// ============================================================================
// 程序组
// ============================================================================

class ProgramGroup {
public:
    ProgramGroup() : m_programGroup(nullptr) {}
    
    static ProgramGroup createRayGen(OptixDeviceContext context, OptixModule module, 
                                     const char* entryFunctionName) {
        OptixProgramGroupOptions options = {};
        OptixProgramGroupDesc desc = {};
        desc.kind = OPTIX_PROGRAM_GROUP_KIND_RAYGEN;
        desc.raygen.module = module;
        desc.raygen.entryFunctionName = entryFunctionName;
        
        return create(context, &desc, 1, &options);
    }
    
    static ProgramGroup createMiss(OptixDeviceContext context, OptixModule module,
                                   const char* entryFunctionName) {
        OptixProgramGroupOptions options = {};
        OptixProgramGroupDesc desc = {};
        desc.kind = OPTIX_PROGRAM_GROUP_KIND_MISS;
        desc.miss.module = module;
        desc.miss.entryFunctionName = entryFunctionName;
        
        return create(context, &desc, 1, &options);
    }
    
    static ProgramGroup createHitGroup(OptixDeviceContext context, OptixModule module,
                                       const char* chFunctionName,
                                       const char* ahFunctionName = nullptr,
                                       const char* isFunctionName = nullptr) {
        OptixProgramGroupOptions options = {};
        OptixProgramGroupDesc desc = {};
        desc.kind = OPTIX_PROGRAM_GROUP_KIND_HITGROUP;
        desc.hitgroup.moduleCH = module;
        desc.hitgroup.entryFunctionNameCH = chFunctionName;
        desc.hitgroup.moduleAH = ahFunctionName ? module : nullptr;
        desc.hitgroup.entryFunctionNameAH = ahFunctionName;
        desc.hitgroup.moduleIS = isFunctionName ? module : nullptr;
        desc.hitgroup.entryFunctionNameIS = isFunctionName;
        
        return create(context, &desc, 1, &options);
    }
    
    ~ProgramGroup() {
        if (m_programGroup) {
            optixProgramGroupDestroy(m_programGroup);
        }
    }
    
    OptixProgramGroup get() const { return m_programGroup; }
    operator OptixProgramGroup() const { return m_programGroup; }

private:
    OptixProgramGroup m_programGroup;
    
    static ProgramGroup create(OptixDeviceContext context,
                              const OptixProgramGroupDesc* descs,
                              uint32_t numDescs,
                              const OptixProgramGroupOptions* options) {
        ProgramGroup pg;
        
        char log[2048];
        size_t logSize = sizeof(log);
        
        OPTIX_CHECK(optixProgramGroupCreate(
            context,
            descs,
            numDescs,
            options,
            log,
            &logSize,
            &pg.m_programGroup
        ));
        
        if (logSize > 1) {
            printf("OptiX Program Group Creation Log:\n%s\n", log);
        }
        
        return pg;
    }
};


// ============================================================================
// 着色器绑定表辅助工具
// ============================================================================

template <typename T>
inline void* createSBTRecord(OptixProgramGroup programGroup, const T& data) {
    void* record;
    size_t recordSize = OPTIX_SBT_RECORD_HEADER_SIZE + sizeof(T);
    CUDA_CHECK(cudaMalloc(&record, recordSize));
    
    // 打包头信息
    OPTIX_CHECK(optixSbtRecordPackHeader(programGroup, record));
    
    // 复制数据
    if (sizeof(T) > 0) {
        CUDA_CHECK(cudaMemcpy(
            static_cast<char*>(record) + OPTIX_SBT_RECORD_HEADER_SIZE,
            &data,
            sizeof(T),
            cudaMemcpyHostToDevice
        ));
    }
    
    return record;
}

inline void* createSBTRecord(OptixProgramGroup programGroup) {
    void* record;
    CUDA_CHECK(cudaMalloc(&record, OPTIX_SBT_RECORD_HEADER_SIZE));
    OPTIX_CHECK(optixSbtRecordPackHeader(programGroup, record));
    return record;
}


// ============================================================================
// 启动辅助工具
// ============================================================================

inline void launch(OptixPipeline pipeline,
                  cudaStream_t stream,
                  const void* launchParams,
                  size_t launchParamsSize,
                  const OptixShaderBindingTable* sbt,
                  uint32_t width,
                  uint32_t height,
                  uint32_t depth = 1) {
    OPTIX_CHECK(optixLaunch(
        pipeline,
        stream,
        reinterpret_cast<CUdeviceptr>(launchParams),
        launchParamsSize,
        sbt,
        width,
        height,
        depth
    ));
}


// ============================================================================
// 载荷签名辅助工具
// ============================================================================

template <typename... PayloadTypes>
struct PayloadSignature {
    static constexpr uint32_t numPayloads = sizeof...(PayloadTypes);
    
    static std::vector<uint32_t> getPayloadSemantics() {
        return std::vector<uint32_t>(numPayloads, 0);
    }
};


// ============================================================================
// 块缓冲 2D（占位符）
// ============================================================================

template <typename T, int BlockSize = 0>
struct BlockBuffer2D {
    T* data;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    
    void initialize(uint32_t w, uint32_t h) {
        width = w;
        height = h;
        stride = w;
        CUDA_CHECK(cudaMalloc(&data, w * h * sizeof(T)));
    }
    
    void finalize() {
        if (data) {
            cudaFree(data);
            data = nullptr;
        }
    }
};

template <typename T>
using NativeBlockBuffer2D = BlockBuffer2D<T, 0>;

}  // 命名空间 optixu
}  // 命名空间 vlr
