// ============================================================================
// VLR GPU 调试工具
//
// 统一所有 GPU kernel 的调试宏和计数器声明。
// kernel 文件只需 #include 此文件即可获得调试能力。
// ============================================================================

#pragma once

// 全局调试开关：取消注释以启用所有 GPU 内核调试输出
// #define VLR_ENABLE_GPU_DEBUG 1

#ifndef VLR_DEBUG_PRINTF
#ifdef VLR_ENABLE_GPU_DEBUG
    #define VLR_DEBUG_PRINTF(...) printf(__VA_ARGS__)
#else
    #define VLR_DEBUG_PRINTF(...) ((void)0)
#endif
#endif

// 取消注释以启用各子系统的调试
// #define VLR_DEBUG_MATERIAL
// #define VLR_DEBUG_BSDF_VERBOSE
// #define VLR_DEBUG_NAN_TRACKING

// 探测像素坐标（用于 per-pixel 调试输出）
#ifndef VLR_DEBUG_PROBE_PIX_X
#define VLR_DEBUG_PROBE_PIX_X 256
#endif
#ifndef VLR_DEBUG_PROBE_PIX_Y
#define VLR_DEBUG_PROBE_PIX_Y 166
#endif

// 限制 debug printf 输出数量的工具宏
#ifdef __CUDACC__
#define VLR_DEBUG_PRINTF_LIMITED(counter, limit, ...) \
    do { \
        unsigned int _idx = atomicAdd(&(counter), 1u); \
        if (_idx < (limit)) { printf(__VA_ARGS__); } \
    } while(0)
#else
#define VLR_DEBUG_PRINTF_LIMITED(counter, limit, ...) ((void)0)
#endif
