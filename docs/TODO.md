# VLR_WF 开发路线图

**最后更新**: 2026-03-10 | **当前版本**: 1.5

---

## 紧急任务（Bug 修复）

### 🔴 关键：SpecularTransmission 透射路径失效问题
**状态**: 正在调试  
**优先级**: P0（阻塞玻璃材质正常工作）

#### 问题描述
- Cornell Box 场景中的玻璃球渲染为黑色
- 反射路径正常（可见高光）
- 透射路径完全失效（即使强制所有路径走透射，球体仍然黑色）

#### 已完成的诊断
1. ✅ 修复了材质分类错误（`BSDFType_SpecularTransmission` → `MaterialCategory_Transmissive`）
2. ✅ 添加了 `cosAbs` 安全检查防止除零
3. ✅ 移除了错误的双重几何修正（`cosGeometric/cosShading`）
4. ✅ 启用了反射/透射随机选择（修复 `if(false&&u0<F)` bug）
5. ✅ Lambert 测试证明球体几何和碰撞检测正常

#### 下一步调试计划
1. **验证 transmittance 参数值**
   - 在 `sampleSpecularTransmissionBSDF_FrontFace` 中添加 printf
   - 确认 `getEffectiveLambertAlbedo` 返回的是 (0.999, 0.999, 0.999) 而不是 (0, 0, 0)
   - 检查材质创建流程：`vlrCreateMaterialEx` → `Scene::createMaterialEx` → GPU 材质数据

2. **验证 BSDF 值计算**
   - 检查 `result.f` 的实际值（应该是 transmittance * (1-F) * squeezeFactor / cosTAbs）
   - 验证 Fresnel 系数 F 的计算（IOR=1.5，正面入射 F≈0.04）
   - 验证 squeezeFactor = (etaI/etaT)² 的计算
   - 确认透射方向 `wt` 的计算使用正确的 Snell 定律

3. **验证光线传播**
   - 检查透射光线的起点偏移（`offsetRayOriginForNextBounce`，cosFactor < 0 应向内偏移）
   - 验证透射光线是否正确击中后续表面（可能需要在 `process_hits.cu` 中添加 printf）
   - 检查透射路径的 throughput 衰减是否过快导致被 Russian Roulette 终止

4. **对比参考实现**
   - 详细对比 `D:\gitProject\OfflineRenderer\libVLR_reference` 中的 SpecularBSDF::sample 实现
   - 检查是否有其他隐藏的处理逻辑（如 adjoint BSDF、non-symmetric scattering 处理）

5. **检查特殊情况**
   - 验证球体内部的二次反弹（从内部向外透射）
   - 检查 `isFrontFace` 标志是否正确切换
   - 验证 `etaI` 和 `etaT` 在进入/离开玻璃时是否正确交换

#### 相关文件
- `libVLR/shared/bsdf_common.h`: SpecularTransmission BSDF 实现 (行 2710-2830)
- `libVLR/GPU_kernels/sample_bsdf.cu`: BSDF 采样和 throughput 更新 (行 237-262)
- `libVLR/shared/material_types.h`: 材质数据布局和访问函数
- `test/cornell_box_improved_test.cpp`: 测试场景设置（玻璃材质：IOR=1.5, Color=(0.999,0.999,0.999)）

#### 测试结果记录
- `glass_lambert_test.png`: Lambert 材质测试 ✅ 球体可见（白色）
- `glass_with_reflection.png`: 启用反射的 SpecularTransmission ⚠️ 只有高光，球体黑色
- `glass_force_transmission.png`: 强制透射 ❌ 球体仍然黑色

---

## 功能对比

| 模块 | 原版 VLR | VLR_WF | 状态 |
|------|---------|--------|------|
| 基础渲染 | ✓ | ✓ | - |
| Wavefront 架构 | ✗ | ✓ | - |
| 材质系统 | 8种 | 14种 | ⚠️ SpecularTransmission 有 bug |
| 光源系统 | 4种 | 3种 | ✅ 完成 |
| 纹理系统 | 完整 | 基础 | 🟡 中 |
| 调试模式 | 12种 | 17种 | ✅ 完成 |
| 降噪 | OptiX | API已实现 | 🟡 中 |

---

## 近期优先级

### 高优先级
1. ~~**光源系统**~~ ✅ **已完成** (2026-03-08)
2. **SpecularTransmission Bug 修复** 🔴 **进行中** (2026-03-10)

### 中优先级
3. **降噪与后处理**: OptiX Denoiser, AOV 系统
4. **调试增强**: ProbePixel 完整实现
5. **Shared Memory 缓存**

### 低优先级
6. 多渲染器 (Light Tracing, BPT)
7. 相机增强 (Equirectangular, 运动模糊)
8. 交互式查看器

---

## 性能优化路线

| 阶段 | 目标 | 关键优化 |
|------|------|----------|
| 短期 | 50 Msamples/s | Shared Memory, 材质优化 |
| 中期 | 65 Msamples/s | CUDA Graphs, Warp 聚合 |
| 长期 | 100+ Msamples/s | SoA 重构, ReSTIR |

**当前**: 40.6 Msamples/s (3.07x 加速)

---

## 已完成

- ✅ 材质系统完善 (MicrofacetReflection/Scattering, MultiSurface, Disney BRDF, 各向异性)
- ✅ 调试渲染模式 (17 种)
- ✅ 纹理系统 (Image2D 加载、材质绑定、UV 变换、法线贴图)
- ✅ 光源系统完善 (方向光、环境光重要性采样、多光源优化)
- ✅ 性能优化阶段 1-5 (3.07x 加速)

