// ============================================================================
// VLR Wavefront - 配置加载器
//
// 从INI文件加载渲染和性能配置参数
//
// 作者：VLR 开发团队
// 创建日期：2026-03-07
// ============================================================================

#pragma once

#include <string>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <cstdint>

namespace vlr {

/// 运行时性能配置（从INI文件加载）
struct RuntimePerformanceConfig {
    // 同步优化
    uint32_t syncInterval = 4;
    
    // 路径压缩优化
    float compressionThreshold = 0.75f;
    uint32_t minPathsForCompression = 2048;
    bool enablePathSorting = true;
    bool enableStreamCompaction = true;
    
    // Kernel线程块配置
    uint32_t generateRaysBlockSize = 256;
    uint32_t processHitsBlockSize = 128;
    uint32_t sampleLightsBlockSize = 256;
    uint32_t sampleBSDFBlockSize = 192;
    uint32_t accumulateBlockSize = 256;
    
    // 早期终止优化
    bool enableEarlyTermination = true;
    float earlyTerminationThreshold = 0.01f;
    uint32_t earlyTerminationMinDepth = 10;
    
    // 内存优化
    bool useRestrictPointers = true;
    bool useMaterialCache = false;
    bool useTextureMemory = false;
    
    // 高级优化
    bool useCudaGraphs = false;
    bool useWarpOptimizations = true;
    bool usePrefetching = false;
    bool useFusedKernels = true;
    bool useDynamicMaxDepth = false;
    bool useAdaptiveSampling = false;
    
    // 调试选项
    bool enableNaNTracking = false;
    bool enablePerfCounters = false;
    bool printKernelTiming = false;
    bool validateQueues = false;
};

/// 渲染配置
struct RenderConfig {
    // 图像参数
    uint32_t width = 512;
    uint32_t height = 512;
    uint32_t samples = 64;
    uint32_t maxDepth = 8;
    float exposure = 1.0f;
    
    // 输出
    std::string outputFilename = "output.png";
    std::string outputFormat = "png";
    
    // 相机
    float cameraPosX = 0.0f;
    float cameraPosY = 1.5f;
    float cameraPosZ = 6.0f;
    float cameraTargetX = 0.0f;
    float cameraTargetY = 1.5f;
    float cameraTargetZ = 0.0f;
    float cameraFOV = 40.0f;
    float lensRadius = 0.0f;
    float focusDistance = 1.0f;
    
    // 性能
    uint32_t deviceID = 0;
    bool verboseLogging = true;
    
    // 性能配置
    RuntimePerformanceConfig perfConfig;
};

/// 简单的INI解析器
class INIParser {
private:
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> data_;
    
    static std::string trim(const std::string& str) {
        size_t first = str.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) return "";
        size_t last = str.find_last_not_of(" \t\r\n");
        return str.substr(first, last - first + 1);
    }
    
public:
    bool load(const std::string& filename) {
        std::ifstream file(filename);
        if (!file.is_open()) {
            return false;
        }
        
        std::string currentSection;
        std::string line;
        
        while (std::getline(file, line)) {
            line = trim(line);
            
            // 跳过空行和注释
            if (line.empty() || line[0] == '#' || line[0] == ';') {
                continue;
            }
            
            // 解析节
            if (line[0] == '[' && line[line.length() - 1] == ']') {
                currentSection = line.substr(1, line.length() - 2);
                continue;
            }
            
            // 解析键值对
            size_t pos = line.find('=');
            if (pos != std::string::npos) {
                std::string key = trim(line.substr(0, pos));
                std::string value = trim(line.substr(pos + 1));
                data_[currentSection][key] = value;
            }
        }
        
        return true;
    }
    
    std::string getString(const std::string& section, const std::string& key, 
                         const std::string& defaultValue = "") const {
        auto secIt = data_.find(section);
        if (secIt == data_.end()) return defaultValue;
        
        auto keyIt = secIt->second.find(key);
        if (keyIt == secIt->second.end()) return defaultValue;
        
        return keyIt->second;
    }
    
    int getInt(const std::string& section, const std::string& key, int defaultValue = 0) const {
        std::string value = getString(section, key);
        if (value.empty()) return defaultValue;
        return std::stoi(value);
    }
    
    float getFloat(const std::string& section, const std::string& key, float defaultValue = 0.0f) const {
        std::string value = getString(section, key);
        if (value.empty()) return defaultValue;
        return std::stof(value);
    }
    
    bool getBool(const std::string& section, const std::string& key, bool defaultValue = false) const {
        std::string value = getString(section, key);
        if (value.empty()) return defaultValue;
        
        // 支持多种布尔值表示
        if (value == "true" || value == "True" || value == "TRUE" || value == "1" || value == "yes" || value == "Yes") {
            return true;
        }
        if (value == "false" || value == "False" || value == "FALSE" || value == "0" || value == "no" || value == "No") {
            return false;
        }
        
        return defaultValue;
    }
};

/// 配置加载器
class ConfigLoader {
public:
    static bool loadRenderConfig(const std::string& filename, RenderConfig& config) {
        INIParser parser;
        if (!parser.load(filename)) {
            return false;
        }
        
        // [Render]
        config.width = parser.getInt("Render", "Width", 512);
        config.height = parser.getInt("Render", "Height", 512);
        config.samples = parser.getInt("Render", "Samples", 64);
        config.maxDepth = parser.getInt("Render", "MaxDepth", 8);
        config.exposure = parser.getFloat("Render", "Exposure", 1.0f);
        
        // [Output]
        config.outputFilename = parser.getString("Output", "Filename", "output.png");
        config.outputFormat = parser.getString("Output", "Format", "png");
        
        // [Camera]
        config.cameraPosX = parser.getFloat("Camera", "PositionX", 0.0f);
        config.cameraPosY = parser.getFloat("Camera", "PositionY", 1.5f);
        config.cameraPosZ = parser.getFloat("Camera", "PositionZ", 6.0f);
        config.cameraTargetX = parser.getFloat("Camera", "TargetX", 0.0f);
        config.cameraTargetY = parser.getFloat("Camera", "TargetY", 1.5f);
        config.cameraTargetZ = parser.getFloat("Camera", "TargetZ", 0.0f);
        config.cameraFOV = parser.getFloat("Camera", "FOV", 40.0f);
        config.lensRadius = parser.getFloat("Camera", "LensRadius", 0.0f);
        config.focusDistance = parser.getFloat("Camera", "FocusDistance", 1.0f);
        
        // [Performance]
        config.deviceID = parser.getInt("Performance", "DeviceID", 0);
        config.verboseLogging = parser.getBool("Performance", "VerboseLogging", true);
        
        // [Optimization]
        auto& perf = config.perfConfig;
        perf.syncInterval = parser.getInt("Optimization", "SyncInterval", 4);
        perf.compressionThreshold = parser.getFloat("Optimization", "CompressionThreshold", 0.75f);
        perf.minPathsForCompression = parser.getInt("Optimization", "MinPathsForCompression", 2048);
        perf.enablePathSorting = parser.getBool("Optimization", "EnablePathSorting", true);
        perf.enableStreamCompaction = parser.getBool("Optimization", "EnableStreamCompaction", true);
        
        // [KernelConfig]
        perf.generateRaysBlockSize = parser.getInt("KernelConfig", "GenerateRaysBlockSize", 256);
        perf.processHitsBlockSize = parser.getInt("KernelConfig", "ProcessHitsBlockSize", 128);
        perf.sampleLightsBlockSize = parser.getInt("KernelConfig", "SampleLightsBlockSize", 256);
        perf.sampleBSDFBlockSize = parser.getInt("KernelConfig", "SampleBSDFBlockSize", 192);
        perf.accumulateBlockSize = parser.getInt("KernelConfig", "AccumulateBlockSize", 256);
        
        // [EarlyTermination]
        perf.enableEarlyTermination = parser.getBool("EarlyTermination", "EnableEarlyTermination", true);
        perf.earlyTerminationThreshold = parser.getFloat("EarlyTermination", "Threshold", 0.01f);
        perf.earlyTerminationMinDepth = parser.getInt("EarlyTermination", "MinDepth", 10);
        
        // [Memory]
        perf.useRestrictPointers = parser.getBool("Memory", "UseRestrictPointers", true);
        perf.useMaterialCache = parser.getBool("Memory", "UseMaterialCache", false);
        perf.useTextureMemory = parser.getBool("Memory", "UseTextureMemory", false);
        
        // [Advanced]
        perf.useCudaGraphs = parser.getBool("Advanced", "UseCudaGraphs", false);
        perf.useWarpOptimizations = parser.getBool("Advanced", "UseWarpOptimizations", true);
        perf.usePrefetching = parser.getBool("Advanced", "UsePrefetching", false);
        perf.useFusedKernels = parser.getBool("Advanced", "UseFusedKernels", true);
        perf.useDynamicMaxDepth = parser.getBool("Advanced", "UseDynamicMaxDepth", false);
        perf.useAdaptiveSampling = parser.getBool("Advanced", "UseAdaptiveSampling", false);
        
        // [Debug]
        perf.enableNaNTracking = parser.getBool("Debug", "EnableNaNTracking", false);
        perf.enablePerfCounters = parser.getBool("Debug", "EnablePerfCounters", false);
        perf.printKernelTiming = parser.getBool("Debug", "PrintKernelTiming", false);
        perf.validateQueues = parser.getBool("Debug", "ValidateQueues", false);
        
        return true;
    }
    
    /// 保存配置到文件
    static bool saveRenderConfig(const std::string& filename, const RenderConfig& config) {
        std::ofstream file(filename);
        if (!file.is_open()) {
            return false;
        }
        
        file << "# VLR_WF Render Configuration\n";
        file << "# 自动生成的配置文件\n\n";
        
        file << "[Render]\n";
        file << "Width = " << config.width << "\n";
        file << "Height = " << config.height << "\n";
        file << "Samples = " << config.samples << "\n";
        file << "MaxDepth = " << config.maxDepth << "\n";
        file << "Exposure = " << config.exposure << "\n\n";
        
        file << "[Output]\n";
        file << "Filename = " << config.outputFilename << "\n";
        file << "Format = " << config.outputFormat << "\n\n";
        
        file << "[Camera]\n";
        file << "PositionX = " << config.cameraPosX << "\n";
        file << "PositionY = " << config.cameraPosY << "\n";
        file << "PositionZ = " << config.cameraPosZ << "\n";
        file << "TargetX = " << config.cameraTargetX << "\n";
        file << "TargetY = " << config.cameraTargetY << "\n";
        file << "TargetZ = " << config.cameraTargetZ << "\n";
        file << "FOV = " << config.cameraFOV << "\n";
        file << "LensRadius = " << config.lensRadius << "\n";
        file << "FocusDistance = " << config.focusDistance << "\n\n";
        
        file << "[Performance]\n";
        file << "DeviceID = " << config.deviceID << "\n";
        file << "VerboseLogging = " << (config.verboseLogging ? "true" : "false") << "\n\n";
        
        const auto& perf = config.perfConfig;
        
        file << "[Optimization]\n";
        file << "SyncInterval = " << perf.syncInterval << "\n";
        file << "CompressionThreshold = " << perf.compressionThreshold << "\n";
        file << "MinPathsForCompression = " << perf.minPathsForCompression << "\n";
        file << "EnablePathSorting = " << (perf.enablePathSorting ? "true" : "false") << "\n";
        file << "EnableStreamCompaction = " << (perf.enableStreamCompaction ? "true" : "false") << "\n\n";
        
        file << "[KernelConfig]\n";
        file << "GenerateRaysBlockSize = " << perf.generateRaysBlockSize << "\n";
        file << "ProcessHitsBlockSize = " << perf.processHitsBlockSize << "\n";
        file << "SampleLightsBlockSize = " << perf.sampleLightsBlockSize << "\n";
        file << "SampleBSDFBlockSize = " << perf.sampleBSDFBlockSize << "\n";
        file << "AccumulateBlockSize = " << perf.accumulateBlockSize << "\n\n";
        
        file << "[EarlyTermination]\n";
        file << "EnableEarlyTermination = " << (perf.enableEarlyTermination ? "true" : "false") << "\n";
        file << "Threshold = " << perf.earlyTerminationThreshold << "\n";
        file << "MinDepth = " << perf.earlyTerminationMinDepth << "\n\n";
        
        file << "[Memory]\n";
        file << "UseRestrictPointers = " << (perf.useRestrictPointers ? "true" : "false") << "\n";
        file << "UseMaterialCache = " << (perf.useMaterialCache ? "true" : "false") << "\n";
        file << "UseTextureMemory = " << (perf.useTextureMemory ? "true" : "false") << "\n\n";
        
        file << "[Advanced]\n";
        file << "UseCudaGraphs = " << (perf.useCudaGraphs ? "true" : "false") << "\n";
        file << "UseWarpOptimizations = " << (perf.useWarpOptimizations ? "true" : "false") << "\n";
        file << "UsePrefetching = " << (perf.usePrefetching ? "true" : "false") << "\n";
        file << "UseFusedKernels = " << (perf.useFusedKernels ? "true" : "false") << "\n";
        file << "UseDynamicMaxDepth = " << (perf.useDynamicMaxDepth ? "true" : "false") << "\n";
        file << "UseAdaptiveSampling = " << (perf.useAdaptiveSampling ? "true" : "false") << "\n\n";
        
        file << "[Debug]\n";
        file << "EnableNaNTracking = " << (perf.enableNaNTracking ? "true" : "false") << "\n";
        file << "EnablePerfCounters = " << (perf.enablePerfCounters ? "true" : "false") << "\n";
        file << "PrintKernelTiming = " << (perf.printKernelTiming ? "true" : "false") << "\n";
        file << "ValidateQueues = " << (perf.validateQueues ? "true" : "false") << "\n";
        
        return true;
    }
    
    /// 打印配置摘要
    static void printConfigSummary(const RenderConfig& config) {
        printf("\n");
        printf("═══════════════════════════════════════════════════════════\n");
        printf("  VLR Wavefront 渲染配置\n");
        printf("═══════════════════════════════════════════════════════════\n");
        printf("  图像: %ux%u, %u samples, max depth %u\n", 
               config.width, config.height, config.samples, config.maxDepth);
        printf("  输出: %s\n", config.outputFilename.c_str());
        printf("───────────────────────────────────────────────────────────\n");
        printf("  优化配置:\n");
        printf("    同步间隔: %u 深度\n", config.perfConfig.syncInterval);
        printf("    压缩阈值: %.2f\n", config.perfConfig.compressionThreshold);
        printf("    路径排序: %s\n", config.perfConfig.enablePathSorting ? "启用" : "禁用");
        printf("    流压缩: %s\n", config.perfConfig.enableStreamCompaction ? "启用" : "禁用");
        printf("    早期终止: %s (阈值=%.2f%%, 最小深度=%u)\n", 
               config.perfConfig.enableEarlyTermination ? "启用" : "禁用",
               config.perfConfig.earlyTerminationThreshold * 100.0f,
               config.perfConfig.earlyTerminationMinDepth);
        printf("───────────────────────────────────────────────────────────\n");
        printf("  Kernel配置:\n");
        printf("    ProcessHits: %u threads/block\n", config.perfConfig.processHitsBlockSize);
        printf("    SampleLights: %u threads/block\n", config.perfConfig.sampleLightsBlockSize);
        printf("    SampleBSDF: %u threads/block\n", config.perfConfig.sampleBSDFBlockSize);
        printf("═══════════════════════════════════════════════════════════\n");
        printf("\n");
    }
};

}  // namespace vlr
