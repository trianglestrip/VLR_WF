# VLR_WF 开发路线图

**最后更新**: 2026-03-11 | **当前版本**: 1.5

---

## 紧急任务（Bug 修复）

### 🔴 关键：SpecularTransmission 透射路径失效问题
**状态**: 调试中 - 已识别关键问题  
**优先级**: P0（阻塞玻璃材质正常工作）

#### 问题描述
- Cornell Box 场景中的玻璃球渲染为黑色
- 镜面反射路径正常（`BSDFType_Specular` 工作正常）
- 透射路径完全失效（`BSDFType_SpecularTransmission` 黑色）

#### 最新进展 (2026-03-11)

**已识别的关键问题**：
1. ✅ **transmittance 参数错误**: 玻璃材质使用 `getEffectiveLambertAlbedo` 获取 transmittance，但应该固定为 (1,1,1)
   - 已修正：在 `sampleBSDFWithU2` 的 `BSDFType_SpecularTransmission` case 中强制 `transmittance = (1,1,1)`
   
2. ✅ **BSDF 公式缺少 `/|cos|` 项**: 对于 delta BSDF，`result.f` 必须包含 `1/|cos(θ)|` 项
   - PBRT 规则：`throughput *= f / pdf`（不额外乘 cos）
   - 反射 BSDF: `f = R * transmittance / |cos(θ_i)|`
   - 折射 BSDF: `f = T * transmittance * η² / |cos(θ_t)|`
   - 已修正：在 `sampleDielectricBSDF_PBRT` 中添加了 `/absCosI` 和 `/absCosT` 项

3. ✅ **实际调用函数识别**: 发现 `BSDFType_SpecularTransmission` 调用的是 `sampleDielectricBSDF_PBRT`，而不是 `sampleSpecularTransmissionBSDF`

**问题依然存在**：
- ⚠️ 即使修正了上述问题，玻璃球仍然黑色
- 可能原因：
  - `refractVector` 函数实现有误（方向计算、符号问题）
  - Local/World 坐标转换问题
  - Ray offset 导致 self-intersection
  - Fresnel 系数计算异常
  - 其他未发现的逻辑错误

#### 已完成的诊断
1. ✅ 修复了材质分类错误（`BSDFType_SpecularTransmission` → `MaterialCategory_Transmissive`）
2. ✅ 添加了 `cosAbs` 安全检查防止除零
3. ✅ 移除了错误的双重几何修正（`cosGeometric/cosShading`）
4. ✅ 启用了反射/透射随机选择（修复 `if(false&&u0<F)` bug）
5. ✅ Lambert 测试证明球体几何和碰撞检测正常

#### 下一步调试计划
1. **深入检查 `refractVector` 函数**
   - 验证 Snell 定律的实现是否正确
   - 检查方向约定（`wo` vs `-wo`）
   - 验证 `cosThetaI` 的符号处理
   - 测试已知输入的输出是否符合预期

2. **使用参考实现**
   - 考虑使用完整的、经过验证的 Glass BSDF 实现（来自 PBRT/Mitsuba）
   - 或临时使用 `BSDFType_MicrofacetScattering` (roughness=0.01) 作为玻璃材质

3. **添加详细的 GPU 调试输出**
   - 在 `sampleDielectricBSDF_PBRT` 中添加 printf
   - 打印 `entering`, `eta`, `Fr`, `wi`, `result.f`, `result.pdf`
   - 在 `sample_bsdf.cu` 中打印 throughput 更新前后的值

4. **验证光线传播**
   - 检查透射光线的起点偏移
   - 验证透射光线是否正确击中后续表面
   - 检查 throughput 是否被 Russian Roulette 过早终止

#### 相关文件
- `libVLR/shared/bsdf_common.h`: 
  - `sampleDielectricBSDF_PBRT` (行 1457-1529): 实际被调用的 Glass BSDF 实现 ✅ 已修正
  - `refractVector` (行 1380-1407): Snell 定律折射计算 ⚠️ 待验证
  - `FresnelDielectric` (行 320-335): 菲涅尔系数计算
  - `sampleBSDFWithU2` BSDFType_SpecularTransmission case (行 2631-2650): BSDF 调度 ✅ 已修正
- `libVLR/GPU_kernels/sample_bsdf.cu`: BSDF 采样和 throughput 更新 (行 237-265)
- `test/cornell_box_improved_test.cpp`: 测试场景（玻璃：IOR=1.5, Color=(0.999,0.999,0.999)）

#### 测试结果记录
- ✅ `mirror_test.png`: 临时使用镜面材质 (`matGlass = matGold`) - 球体正常渲染，证明几何和材质绑定正确
- ❌ `glass_black_v1.png`: 初始版本 - 玻璃球完全黑色
- ❌ `glass_black_v2.png`: 修正 transmittance=(1,1,1) + 添加 `/|cos|` 项 - 玻璃球仍然黑色
- ⚠️ `glass_force_transmission.png`: 强制 100% 透射测试 - 渲染卡死（无进度输出）

#### 关键发现
1. **镜面反射工作正常** (`BSDFType_Specular`): 说明 integrator pipeline、throughput 更新、ray offset 都是正确的
2. **问题集中在折射逻辑**: `refractVector` 或相关的方向/坐标处理可能有根本性错误
3. **强制透射导致卡死**: 可能是 `refractVector` 返回了无效方向，导致光线陷入无限循环或立即终止

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
2. **SpecularTransmission Bug 修复** 🔴 **进行中** (2026-03-10 ~ 2026-03-11)
   - ✅ 修正 transmittance 参数（应为 1,1,1 而非 albedo）
   - ✅ 修正 BSDF 公式（添加 `/|cos|` 项）
   - ⚠️ 问题依然存在，需要深入调试 `refractVector` 或使用参考实现

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

