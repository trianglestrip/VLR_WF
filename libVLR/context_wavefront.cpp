#ifdef VLR_ENABLE_CPU_DEBUG
    #define VLR_DEBUG_PRINTF(...) printf(__VA_ARGS__)
#else
    #define VLR_DEBUG_PRINTF(...) ((void)0)
#endif

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
#include <optix_stack_size.h>
#include <cstring>
#include <vector>
#include <chrono>
#include "shared/performance_config.h"

namespace vlr {


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
    
    // Shadow ray batch buffers (for NEE and LVC-BPT vertex connection)
    // Allocated unconditionally so NEE shadow visibility works even when BDPT is disabled
    {
        uint32_t maxShadowRays = numPixels;
        if (!wf.shadowRayQueueBuffer) {
            wf.shadowRayQueueBuffer = std::make_unique<cudau::Buffer<shared::ShadowRayRequest>>();
        }
        wf.shadowRayQueueBuffer->initialize(m_cudaContext, cudau::BufferType::Device, maxShadowRays);
        if (!wf.shadowRayResultsBuffer) {
            wf.shadowRayResultsBuffer = std::make_unique<cudau::Buffer<float>>();
        }
        wf.shadowRayResultsBuffer->initialize(m_cudaContext, cudau::BufferType::Device, maxShadowRays);
        if (!wf.numShadowRayRequestsBuffer) {
            wf.numShadowRayRequestsBuffer = std::make_unique<cudau::Buffer<uint32_t>>();
        }
        wf.numShadowRayRequestsBuffer->initialize(m_cudaContext, cudau::BufferType::Device, 1);
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
        // Shadow ray batch
        if (wf.shadowRayQueueBuffer && wf.numShadowRayRequestsBuffer) {
            lp.shadowRayQueue = wf.shadowRayQueueBuffer->getDevicePointer();
            lp.shadowRayResults = wf.shadowRayResultsBuffer->getDevicePointer();
            lp.numShadowRayRequests = wf.numShadowRayRequestsBuffer->getDevicePointer();
            lp.maxShadowRayRequests = numPixels;
        } else {
            lp.shadowRayQueue = nullptr;
            lp.shadowRayResults = nullptr;
            lp.numShadowRayRequests = nullptr;
            lp.maxShadowRayRequests = 0;
        }
    } else {
        lp.lightVertexCache = nullptr;
        lp.numLightVertices = nullptr;
        lp.lightPathStateBuffer = nullptr;
        lp.lightHitInfoBuffer = nullptr;
        lp.lightSurfacePointBuffer = nullptr;
        lp.numLightPaths = 0;
        lp.maxLightVertices = 0;
    }

    // Shadow ray batch (for NEE and LVC-BPT) - set whenever buffers exist
    if (wf.shadowRayQueueBuffer && wf.numShadowRayRequestsBuffer) {
        lp.shadowRayQueue = wf.shadowRayQueueBuffer->getDevicePointer();
        lp.shadowRayResults = wf.shadowRayResultsBuffer->getDevicePointer();
        lp.numShadowRayRequests = wf.numShadowRayRequestsBuffer->getDevicePointer();
        lp.maxShadowRayRequests = numPixels;
    } else {
        lp.shadowRayQueue = nullptr;
        lp.shadowRayResults = nullptr;
        lp.numShadowRayRequests = nullptr;
        lp.maxShadowRayRequests = 0;
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

    // Update light path SBT records
    if (wf.lightRaygenRecord) {
        CUDA_CHECK(cudaMemcpy(
            static_cast<char*>(wf.lightRaygenRecord) + OPTIX_SBT_RECORD_HEADER_SIZE,
            &sbtData, sizeof(sbtData), cudaMemcpyHostToDevice));
    }
    if (wf.lightMissRecord) {
        size_t lightMissRecordSize = (OPTIX_SBT_RECORD_HEADER_SIZE + sizeof(shared::WavefrontSBTData) + 15) & ~15;
        CUDA_CHECK(cudaMemcpy(
            static_cast<char*>(wf.lightMissRecord) + OPTIX_SBT_RECORD_HEADER_SIZE,
            &sbtData, sizeof(sbtData), cudaMemcpyHostToDevice));
        CUDA_CHECK(cudaMemcpy(
            static_cast<char*>(wf.lightMissRecord) + lightMissRecordSize + OPTIX_SBT_RECORD_HEADER_SIZE,
            &sbtData, sizeof(sbtData), cudaMemcpyHostToDevice));
    }
    if (wf.lightHitgroupRecord) {
        size_t lightHitRecordSize = (OPTIX_SBT_RECORD_HEADER_SIZE + sizeof(shared::WavefrontSBTData) + 15) & ~15;
        CUDA_CHECK(cudaMemcpy(
            static_cast<char*>(wf.lightHitgroupRecord) + OPTIX_SBT_RECORD_HEADER_SIZE,
            &sbtData, sizeof(sbtData), cudaMemcpyHostToDevice));
        CUDA_CHECK(cudaMemcpy(
            static_cast<char*>(wf.lightHitgroupRecord) + lightHitRecordSize + OPTIX_SBT_RECORD_HEADER_SIZE,
            &sbtData, sizeof(sbtData), cudaMemcpyHostToDevice));
    }
    // Update shadow ray batch SBT
    if (wf.shadowRaygenRecord) {
        CUDA_CHECK(cudaMemcpy(
            static_cast<char*>(wf.shadowRaygenRecord) + OPTIX_SBT_RECORD_HEADER_SIZE,
            &sbtData, sizeof(sbtData), cudaMemcpyHostToDevice));
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
    
    // LVC-BPT: Generate light paths, trace, and process hits (multi-bounce)
    if (wf.useBDPT && wf.numLightVerticesBuffer && wf.lightVertexCacheBuffer) {
        uint32_t zero = 0;
        CUDA_CHECK(cudaMemcpy(
            wf.numLightVerticesBuffer->getDevicePointer(),
            &zero, sizeof(uint32_t), cudaMemcpyHostToDevice));

        shared::WavefrontLaunchParameters* d_params =
            static_cast<shared::WavefrontLaunchParameters*>(wf.launchParamsBuffer);

        // Step 1: Generate light path origins and directions
        launchGenerateLightPathsKernel(d_params, numPixels, m_stream);
        CUDA_CHECK(cudaStreamSynchronize(m_stream));

        // Step 2-3: Multi-bounce light path tracing
        // Light paths continue through delta surfaces (glass/mirror)
        constexpr uint32_t maxLightBounces = 8;
        for (uint32_t bounce = 0; bounce < maxLightBounces; ++bounce) {
            launchTraceLightRays(numPixels);
            launchProcessLightHitsKernel(d_params, numPixels, m_stream);
            CUDA_CHECK(cudaStreamSynchronize(m_stream));
        }

        uint32_t numLV = 0;
        CUDA_CHECK(cudaMemcpy(
            &numLV, wf.numLightVerticesBuffer->getDevicePointer(),
            sizeof(uint32_t), cudaMemcpyDeviceToHost));
        if (wf.numAccumFrames <= 1) {
            printf("[VLR-BDPT] Light vertices generated: %u (multi-bounce)\n", numLV);
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
        // Clear shadow ray counter before processHits enqueues new requests
        if (wf.useBDPT && wf.numShadowRayRequestsBuffer) {
            uint32_t zero = 0;
            CUDA_CHECK(cudaMemcpyAsync(
                wf.numShadowRayRequestsBuffer->getDevicePointer(),
                &zero, sizeof(uint32_t), cudaMemcpyHostToDevice, m_stream));
        }
        launchProcessHits(numActivePaths);
        
        // ?? 3.5: LVC-BPT shadow ray visibility test
        if (wf.useBDPT && wf.numShadowRayRequestsBuffer && wf.shadowRayQueueBuffer) {
            CUDA_CHECK(cudaStreamSynchronize(m_stream));
            uint32_t numShadowReqs = 0;
            CUDA_CHECK(cudaMemcpy(&numShadowReqs,
                wf.numShadowRayRequestsBuffer->getDevicePointer(),
                sizeof(uint32_t), cudaMemcpyDeviceToHost));
            if (numShadowReqs > 0) {
                numShadowReqs = std::min(numShadowReqs, wf.launchParams.maxShadowRayRequests);
                launchTraceShadowRays(numShadowReqs);
                shared::WavefrontLaunchParameters* d_params =
                    static_cast<shared::WavefrontLaunchParameters*>(wf.launchParamsBuffer);
                launchApplyShadowRayResultsKernel(d_params, numShadowReqs, m_stream);
                CUDA_CHECK(cudaStreamSynchronize(m_stream));
            }
        }
        
        // ?? 4: ???? (NEE) + Shadow Ray Visibility
        {
            // Clear shadow ray counter before sampleLights enqueues NEE requests
            if (wf.numShadowRayRequestsBuffer) {
                uint32_t zero = 0;
                CUDA_CHECK(cudaMemcpyAsync(
                    wf.numShadowRayRequestsBuffer->getDevicePointer(),
                    &zero, sizeof(uint32_t), cudaMemcpyHostToDevice, m_stream));
            }
            launchSampleLights(numActivePaths);

            // Trace shadow rays for NEE visibility testing
            if (wf.numShadowRayRequestsBuffer && wf.shadowRayQueueBuffer) {
                CUDA_CHECK(cudaStreamSynchronize(m_stream));
                uint32_t numShadowReqs = 0;
                CUDA_CHECK(cudaMemcpy(&numShadowReqs,
                    wf.numShadowRayRequestsBuffer->getDevicePointer(),
                    sizeof(uint32_t), cudaMemcpyDeviceToHost));
                if (numShadowReqs > 0) {
                    numShadowReqs = std::min(numShadowReqs, wf.launchParams.maxShadowRayRequests);
                    launchTraceShadowRays(numShadowReqs);
                    shared::WavefrontLaunchParameters* d_params =
                        static_cast<shared::WavefrontLaunchParameters*>(wf.launchParamsBuffer);
                    launchApplyShadowRayResultsKernel(d_params, numShadowReqs, m_stream);
                    CUDA_CHECK(cudaStreamSynchronize(m_stream));
                }
            }
        }
        
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
                wf.launchParams.activePathQueue.pathIndices = wf.activePathIndices->getDevicePointer();
                wf.launchParams.nextActivePathQueue.pathIndices = wf.nextActivePathIndices->getDevicePointer();
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
                // Re-upload launchParams to device
                CUDA_CHECK(cudaMemcpyAsync(
                    wf.launchParamsBuffer,
                    &wf.launchParams,
                    sizeof(shared::WavefrontLaunchParameters),
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
            
            // Update launchParams queue pointers after swap
            wf.launchParams.activePathQueue.pathIndices = wf.activePathIndices->getDevicePointer();
            wf.launchParams.nextActivePathQueue.pathIndices = wf.nextActivePathIndices->getDevicePointer();
            
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
            
            // Re-upload launchParams to device so GPU sees updated queue pointers
            CUDA_CHECK(cudaMemcpyAsync(
                wf.launchParamsBuffer,
                &wf.launchParams,
                sizeof(shared::WavefrontLaunchParameters),
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

void Context::launchTraceLightRays(uint32_t numLightPaths) {
    auto& wf = m_optix.wavefrontPathTracing;
    if (!wf.pipeline || !wf.launchParamsBuffer || numLightPaths == 0) return;

    CUDA_CHECK(cudaMemcpy(
        wf.launchParamsBuffer, &wf.launchParams,
        sizeof(shared::WavefrontLaunchParameters), cudaMemcpyHostToDevice));
    CUDA_CHECK(cudaStreamSynchronize(m_stream));

    OptixResult res = optixLaunch(
        wf.pipeline, m_stream, 0, 0,
        &wf.lightSbt, numLightPaths, 1, 1);
    if (res != OPTIX_SUCCESS) {
        fprintf(stderr, "[VLR] Error: optixLaunch (light rays) failed - %s\n", optixGetErrorName(res));
    }
    CUDA_CHECK(cudaStreamSynchronize(m_stream));
}

void Context::launchTraceShadowRays(uint32_t numShadowRays) {
    auto& wf = m_optix.wavefrontPathTracing;
    if (!wf.pipeline || !wf.launchParamsBuffer || numShadowRays == 0) return;

    CUDA_CHECK(cudaMemcpy(
        wf.launchParamsBuffer, &wf.launchParams,
        sizeof(shared::WavefrontLaunchParameters), cudaMemcpyHostToDevice));
    CUDA_CHECK(cudaStreamSynchronize(m_stream));

    OptixResult res = optixLaunch(
        wf.pipeline, m_stream, 0, 0,
        &wf.shadowSbt, numShadowRays, 1, 1);
    if (res != OPTIX_SUCCESS) {
        fprintf(stderr, "[VLR] Error: optixLaunch (shadow rays) failed - %s\n", optixGetErrorName(res));
    }
    CUDA_CHECK(cudaStreamSynchronize(m_stream));
}

void Context::launchProcessHits(uint32_t numActivePaths) {
    auto& wf = m_optix.wavefrontPathTracing;
    if (!wf.launchParamsBuffer) return;
    if (numActivePaths == 0) return;

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

} // namespace vlr
