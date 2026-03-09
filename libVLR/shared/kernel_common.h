// ============================================================================
// VLR 内核公共头文件
//
// 本文件包含 CUDA/OptiX 内核的公共头文件和定义。
//
// 作者：VLR 开发团队
// 创建时间：2026-03-07
// 环境：CUDA 13.1，OptiX 8.0.0，VS2022
// ============================================================================

#pragma once

// ============================================================================
// 调试开关
// ============================================================================
// 全局调试开关：取消注释以启用所有GPU内核调试输出
// #define VLR_ENABLE_GPU_DEBUG 1

#ifdef VLR_ENABLE_GPU_DEBUG
    #define VLR_DEBUG_PRINTF(...) printf(__VA_ARGS__)
#else
    #define VLR_DEBUG_PRINTF(...) ((void)0)
#endif

// 取消注释以启用材质系统调试输出（Task 1-5）
// #define VLR_DEBUG_MATERIAL

// 取消注释以启用详细的 BSDF 调试输出
// #define VLR_DEBUG_BSDF_VERBOSE

// 包含基本类型
#include "../include/vlr/basic_types.h"

// 注意：OptiX 头文件由各内核文件按需包含

// CUDA 头文件（无条件包含，本文件仅被 .cu 使用；避免条件包含导致 C1020）
#include <cuda_runtime.h>

// 标准库头文件
#include <cstdint>
#include <cstring>

namespace vlr {
namespace shared {

// 将基本类型导入 shared 命名空间
using ::vlr::Vector3D;
using ::vlr::Point3D;
using ::vlr::Normal3D;
using ::vlr::Vector2D;
using ::vlr::TexCoord2D;
using ::vlr::ReferenceFrame;
using ::vlr::SampledSpectrum;
using ::vlr::DiscretizedSpectrum;
using ::vlr::SpectrumStorage;
using ::vlr::WavelengthSamples;
using ::vlr::DirectionType;
using ::vlr::KernelRNG;
using ::vlr::SurfacePoint;
using ::vlr::CameraDescriptor;
using ::vlr::CameraType;
using ::vlr::GeometryType;
using ::vlr::Triangle;
using ::vlr::GeometryInstance;
using ::vlr::Instance;
using ::vlr::NodeProcedureSet;
using ::vlr::SmallNodeDescriptor;
using ::vlr::MediumNodeDescriptor;
using ::vlr::LargeNodeDescriptor;
using ::vlr::BSDFProcedureSet;
using ::vlr::EDFProcedureSet;
using ::vlr::IDFProcedureSet;
using ::vlr::SurfaceMaterialDescriptor;
using ::vlr::TransportMode;
using ::vlr::BSDF;
using ::vlr::BSDFQuery;
using ::vlr::BSDFSample;
using ::vlr::EDF;
using ::vlr::EDFQuery;
using ::vlr::LensPosSample;
using ::vlr::IDFSample;
using ::vlr::LightPosSample;
using ::vlr::DiscreteDistribution1D;
using ::vlr::SceneBounds;
using ::vlr::NumSpectralSamples;

// OptiX 工具命名空间
namespace optixu = ::vlr::optixu;

// ============================================================================
// 配置常量
// ============================================================================
#ifndef VLR_NUM_MATERIAL_CATEGORIES_DEFINED
#define VLR_NUM_MATERIAL_CATEGORIES_DEFINED
constexpr uint32_t NumMaterialCategories = 6;
#endif
constexpr uint32_t MaxPathLength = 64;



// ============================================================================
// 全局启动参数指针
// ============================================================================

// 前向声明
struct WavefrontLaunchParameters;

// 用于启动参数的全局常量内存
// 注意：应在使用它的每个内核文件中定义
// #ifdef __CUDACC__
//     extern "C" __constant__ WavefrontLaunchParameters wlp;
// #endif

// 访问启动参数的便捷宏
// #define WLP wlp

}  // namespace shared
}  // namespace vlr
