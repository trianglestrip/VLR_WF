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
#include "utils/cuda_util.h"
#include "utils/optix_util.h"
#ifdef _WIN32
#undef max
#undef min
#endif
#include <optix.h>
#include <cstring>
#include <stdexcept>

namespace vlr {

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
// ??????
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

    if (m_sceneSource) {
        VLR_DEBUG_PRINTF("[VLR] Preparing scene (TaskFlow DAG: GAS + Bounds + Aggregate -> Upload -> IAS)...\n");
        fflush(stdout);
        Scene* mutableScene = const_cast<Scene*>(m_sceneSource);
        mutableScene->prepareSceneParallel();
        m_scene.camera = m_sceneSource->getCamera();
        m_scene.bounds = m_sceneSource->getSceneBounds();
        mutableScene->releaseHostMeshData();
        VLR_DEBUG_PRINTF("[VLR] Scene ready (host mesh data released)\n");
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

void Context::setWavefrontPathSorting(bool enable) {
    m_optix.wavefrontPathTracing.usePathSorting = enable;
}

void Context::setWavefrontStreamCompaction(bool enable) {
    m_optix.wavefrontPathTracing.useStreamCompaction = enable;
}

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
