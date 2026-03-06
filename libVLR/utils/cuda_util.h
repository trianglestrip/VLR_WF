// ============================================================================
// CUDA 工具
//
// 本文件提供 CUDA 工具类和函数。
//
// 作者：VLR 开发团队
// 创建：2026-03-07
// 环境：CUDA 13.1、OptiX 8.0.0、VS2022
// ============================================================================

#pragma once

#include <cuda_runtime.h>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace vlr {
namespace cudau {

// ============================================================================
// 错误处理
// ============================================================================

inline void checkError(cudaError_t error, const char* expr, const char* file, int line) {
    if (error != cudaSuccess) {
        char msg[1024];
        snprintf(msg, sizeof(msg), "CUDA Error at %s:%d\n  %s\n  Error: %s (%d)",
                 file, line, expr, cudaGetErrorString(error), error);
        throw std::runtime_error(msg);
    }
}

#define CUDA_CHECK(call) ::vlr::cudau::checkError(call, #call, __FILE__, __LINE__)


// ============================================================================
// CUDA 上下文
// ============================================================================

class Context {
public:
    Context() {
        int deviceCount;
        CUDA_CHECK(cudaGetDeviceCount(&deviceCount));
        if (deviceCount == 0) {
            throw std::runtime_error("No CUDA devices found");
        }
        
        CUDA_CHECK(cudaSetDevice(0));
        CUDA_CHECK(cudaFree(0));  // 初始化 CUDA 上下文
    }
    
    ~Context() {
        // CUDA 上下文会自动清理
    }
    
    int getDeviceCount() const {
        int count;
        CUDA_CHECK(cudaGetDeviceCount(&count));
        return count;
    }
    
    void synchronize() {
        CUDA_CHECK(cudaDeviceSynchronize());
    }
};


// ============================================================================
// 缓冲区类型
// ============================================================================

enum class BufferType {
    Device,      // GPU 显存
    Host,        // CPU 内存（可分页）
    HostPinned,  // CPU 内存（固定/页锁定）
    Managed      // 统一内存
};


// ============================================================================
// 通用缓冲区类
// ============================================================================

template <typename T>
class Buffer {
public:
    Buffer() 
        : m_data(nullptr)
        , m_size(0)
        , m_capacity(0)
        , m_type(BufferType::Device)
    {}
    
    ~Buffer() {
        finalize();
    }
    
    void initialize(Context* context, BufferType type, size_t size) {
        if (m_data) {
            finalize();
        }
        
        m_type = type;
        m_size = size;
        m_capacity = size;
        
        size_t bytes = size * sizeof(T);
        
        switch (type) {
            case BufferType::Device:
                CUDA_CHECK(cudaMalloc(&m_data, bytes));
                break;
                
            case BufferType::Host:
                m_data = new T[size];
                break;
                
            case BufferType::HostPinned:
                CUDA_CHECK(cudaMallocHost(&m_data, bytes));
                break;
                
            case BufferType::Managed:
                CUDA_CHECK(cudaMallocManaged(&m_data, bytes));
                break;
        }
    }
    
    void finalize() {
        if (!m_data) return;
        
        switch (m_type) {
            case BufferType::Device:
                cudaFree(m_data);
                break;
                
            case BufferType::Host:
                delete[] static_cast<T*>(m_data);
                break;
                
            case BufferType::HostPinned:
                cudaFreeHost(m_data);
                break;
                
            case BufferType::Managed:
                cudaFree(m_data);
                break;
        }
        
        m_data = nullptr;
        m_size = 0;
        m_capacity = 0;
    }
    
    void resize(size_t newSize) {
        if (newSize <= m_capacity) {
            m_size = newSize;
            return;
        }
        
        // 需要重新分配
        void* oldData = m_data;
        size_t oldSize = m_size;
        
        m_capacity = newSize;
        m_size = newSize;
        
        size_t bytes = newSize * sizeof(T);
        
        switch (m_type) {
            case BufferType::Device:
                CUDA_CHECK(cudaMalloc(&m_data, bytes));
                if (oldData && oldSize > 0) {
                    CUDA_CHECK(cudaMemcpy(m_data, oldData, oldSize * sizeof(T), cudaMemcpyDeviceToDevice));
                    cudaFree(oldData);
                }
                break;
                
            case BufferType::Host:
                m_data = new T[newSize];
                if (oldData && oldSize > 0) {
                    memcpy(m_data, oldData, oldSize * sizeof(T));
                    delete[] static_cast<T*>(oldData);
                }
                break;
                
            case BufferType::HostPinned:
                CUDA_CHECK(cudaMallocHost(&m_data, bytes));
                if (oldData && oldSize > 0) {
                    memcpy(m_data, oldData, oldSize * sizeof(T));
                    cudaFreeHost(oldData);
                }
                break;
                
            case BufferType::Managed:
                CUDA_CHECK(cudaMallocManaged(&m_data, bytes));
                if (oldData && oldSize > 0) {
                    memcpy(m_data, oldData, oldSize * sizeof(T));
                    cudaFree(oldData);
                }
                break;
        }
    }
    
    void clear(cudaStream_t stream = 0) {
        if (m_type == BufferType::Device || m_type == BufferType::Managed) {
            CUDA_CHECK(cudaMemsetAsync(m_data, 0, m_size * sizeof(T), stream));
        } else {
            memset(m_data, 0, m_size * sizeof(T));
        }
    }
    
    // 访问器
    T* getDevicePointer() { return static_cast<T*>(m_data); }
    const T* getDevicePointer() const { return static_cast<const T*>(m_data); }
    
    T* getDevicePointerAt(size_t index) { 
        return static_cast<T*>(m_data) + index; 
    }
    
    T* getHostPointer() { return static_cast<T*>(m_data); }
    const T* getHostPointer() const { return static_cast<const T*>(m_data); }
    
    size_t size() const { return m_size; }
    size_t capacity() const { return m_capacity; }
    size_t sizeInBytes() const { return m_size * sizeof(T); }
    
    BufferType type() const { return m_type; }
    
    // 数据传输
    void copyToDevice(const T* hostData, size_t count, cudaStream_t stream = 0) {
        CUDA_CHECK(cudaMemcpyAsync(m_data, hostData, count * sizeof(T), 
                                   cudaMemcpyHostToDevice, stream));
    }
    
    void copyToHost(T* hostData, size_t count, cudaStream_t stream = 0) const {
        CUDA_CHECK(cudaMemcpyAsync(hostData, m_data, count * sizeof(T), 
                                   cudaMemcpyDeviceToHost, stream));
    }

private:
    void* m_data;
    size_t m_size;
    size_t m_capacity;
    BufferType m_type;
};


// ============================================================================
// 内核启动辅助函数
// ============================================================================

inline dim3 computeGridSize(uint32_t numThreads, uint32_t blockSize) {
    return dim3((numThreads + blockSize - 1) / blockSize);
}

inline dim3 computeGridSize2D(uint32_t width, uint32_t height, uint32_t blockWidth, uint32_t blockHeight) {
    return dim3(
        (width + blockWidth - 1) / blockWidth,
        (height + blockHeight - 1) / blockHeight
    );
}


// ============================================================================
// 设备属性
// ============================================================================

inline void printDeviceProperties(int deviceId = 0) {
    cudaDeviceProp prop;
    CUDA_CHECK(cudaGetDeviceProperties(&prop, deviceId));
    
    printf("=== CUDA Device %d ===\n", deviceId);
    printf("Name: %s\n", prop.name);
    printf("Compute Capability: %d.%d\n", prop.major, prop.minor);
    printf("Total Global Memory: %.2f GB\n", prop.totalGlobalMem / (1024.0 * 1024.0 * 1024.0));
    printf("Multiprocessors: %d\n", prop.multiProcessorCount);
    printf("Max Threads Per Block: %d\n", prop.maxThreadsPerBlock);
    printf("Max Threads Per Multiprocessor: %d\n", prop.maxThreadsPerMultiProcessor);
    printf("Warp Size: %d\n", prop.warpSize);
    int memClockKHz = 0;
    cudaDeviceGetAttribute(&memClockKHz, cudaDevAttrMemoryClockRate, deviceId);
    printf("Memory Clock Rate: %.2f GHz\n", (memClockKHz > 0) ? (memClockKHz / 1e6f) : 0.0f);
    printf("Memory Bus Width: %d bits\n", prop.memoryBusWidth);
    printf("L2 Cache Size: %.2f MB\n", prop.l2CacheSize / (1024.0 * 1024.0));
}

} // namespace cudau
} // namespace vlr
