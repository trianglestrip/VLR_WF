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

// 包含基本类型
#include "../include/vlr/basic_types.h"

// OptiX 头文件（仅在 OptiX 可用时包含）
#if defined(__CUDACC__) && defined(VLR_USE_OPTIX)
    #include <optix.h>
    #include <optix_device.h>
#endif

// CUDA 头文件
#ifdef __CUDACC__
    #include <cuda_runtime.h>
#endif

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
