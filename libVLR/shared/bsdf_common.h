// ============================================================================
// VLR 材质系统 - BSDF 公共函数（聚合头文件）
//
// 本文件作为聚合入口，包含所有 BSDF 子模块。
// 各子模块按功能拆分在 bsdf/ 子目录中：
//   bsdf_types.h      — 工具函数 (safeSqrt, roughnessToAlpha)
//   fresnel.h         — Fresnel 方程（Schlick、电介质、导体）
//   microfacet.h      — GGX 微表面分布（各向同性 + 各向异性）
//   lambert.h          — Lambert / LambertianScattering
//   ggx_reflection.h   — GGX 镜面反射 + MicrofacetReflection（导体）
//   scattering.h       — MicrofacetScattering（反射+折射）
//   specular.h         — 完美镜面 + PBRT Dielectric (Glass)
//   transmission.h     — GGX 粗糙透射
//   advanced_brdf.h    — FresnelBlend, UE4, Frostbite, Disney
//   composite.h        — MixedBSDF, MultiSurface
//   bsdf_dispatch.h    — evaluateBSDF / sampleBSDF / getBSDFPDF 统一分发
//   edf.h              — EDF 发光评估 + FrontFace specular transmission
//
// 作者：VLR 开发团队
// 创建日期：2026-03-07
// 环境：CUDA 13.1, OptiX 8.0.0, VS2022
// ============================================================================

#pragma once

#include "bsdf/bsdf_types.h"
#include "bsdf/fresnel.h"
#include "bsdf/microfacet.h"
#include "bsdf/lambert.h"
#include "bsdf/ggx_reflection.h"
#include "bsdf/scattering.h"
#include "bsdf/specular.h"
#include "bsdf/transmission.h"
#include "bsdf/advanced_brdf.h"
#include "bsdf/composite.h"
#include "bsdf/bsdf_dispatch.h"
#include "bsdf/edf.h"
