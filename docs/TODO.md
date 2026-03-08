# VLR_WF 开发路线图

**最后更新**: 2026-03-08 | **当前版本**: 1.3

---

## 功能对比

| 模块 | 原版 VLR | VLR_WF | 状态 |
|------|---------|--------|------|
| 基础渲染 | ✅ | ✅ | - |
| Wavefront 架构 | ❌ | ✅ | - |
| 材质系统 | 8种 | 14种 | ✅ 完成 |
| 光源系统 | 4种 | 3种 | 🔴 高 |
| 纹理系统 | 完整 | 基础 | 🟡 中 |
| 调试模式 | 12种 | 17种 | ✅ 完成 |
| 降噪 | OptiX | API已实现 | 🟡 中 |

---

## 近期优先级

### 高优先级
1. **光源系统** (2-3周): DirectionalEmitter, EnvironmentEmitter 重要性采样

### 中优先级
3. **降噪与后处理** (3-4周): OptiX Denoiser, AOV 系统
4. **调试增强**: ProbePixel 完整实现
5. **Shared Memory 缓存** (2-3周)

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
- ✅ 性能优化阶段 1-5 (3.07x 加速)
