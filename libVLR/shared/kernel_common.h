// ============================================================================
// VLR Kernel Common
// 
// This file includes common headers and definitions for CUDA/OptiX kernels.
// 
// Author: VLR Development Team
// Created: 2026-03-07
// Environment: CUDA 13.1, OptiX 8.0.0, VS2022
// ============================================================================

#pragma once

// Include basic types
#include "../include/vlr/basic_types.h"

// OptiX headers (only when OptiX is available)
#if defined(__CUDACC__) && defined(VLR_USE_OPTIX)
    #include <optix.h>
    #include <optix_device.h>
#endif

// CUDA headers
#ifdef __CUDACC__
    #include <cuda_runtime.h>
#endif

// Standard headers
#include <cstdint>
#include <cstring>

namespace vlr {
namespace shared {

// Import basic types into shared namespace
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

// OptiX utility namespace
namespace optixu = ::vlr::optixu;

// ============================================================================
// Global Launch Parameters Pointer
// ============================================================================

// Forward declaration
struct WavefrontLaunchParameters;

// Global constant memory for launch parameters
// Note: This should be defined in each kernel file that uses it
// #ifdef __CUDACC__
//     extern "C" __constant__ WavefrontLaunchParameters wlp;
// #endif

// Convenience macro for accessing launch parameters
// #define WLP wlp

} // namespace shared
} // namespace vlr
