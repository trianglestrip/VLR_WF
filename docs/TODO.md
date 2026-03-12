# VLR_WF 开发路线图

**最后更新**: 2026-03-12 | **当前版本**: 1.6

---

## 当前任务：LVC-BPT 双向路径追踪

### 🟡 LVC-BPT 框架已搭建，光路追踪待完成
**状态**: 框架已实现，需要完善光路追踪  
**优先级**: P0

#### 背景
- 单向路径追踪（Unidirectional PT）无法高效采样 caustics（光线穿过 delta 表面聚焦到 diffuse 表面的效果）
- 玻璃球的 "暗球" 问题本质是算法限制，而非 BSDF 实现错误
- 参考 `libVLR_reference` 的 Light Vertex Cache BPT (LVC-BPT) 方案

#### 已完成 (2026-03-11 ~ 2026-03-12)
1. ✅ **数据结构定义**
   - `LightPathVertex`: 存储光路顶点的位置、法线、flux、方向、材质等信息
   - `LightPathState`: 光路追踪状态（flux、方向、RNG、波长等）
   - 扩展 `WavefrontLaunchParameters`（字段追加在末尾，保持已有偏移不变）

2. ✅ **光路生成 kernel** (`light_path.cu`)
   - `generateLightPaths`: 从场景灯光采样位置和发射方向，生成 pathLength=0 的光顶点
   - `processLightHits`: 处理光路第一次命中，生成 pathLength=1 的光顶点
   - flux 正确归一化：`alpha = Le / (numLightPaths * lightAreaPDF)`

3. ✅ **Eye Path Vertex Connection** (`process_hits.cu`)
   - 在 eye path 命中非 delta 表面时，随机连接一个光路顶点
   - 计算几何项 G、BSDF 评估、EDF 方向因子（Lambertian: 1/π）
   - 贡献公式：`contrib = throughput * fsE * G * lv.flux * edfFactor / vertexProb`

4. ✅ **Host 端整合** (`context.cpp` / `context.h`)
   - 分配 LVC-BPT 缓冲区（lightVertexCache、lightPathState 等，共约 136 MB）
   - 渲染循环：先生成光路 → 同步 → 再执行 eye path 渲染

5. ✅ **编译测试通过**
   - 场景正确渲染（红墙、蓝墙、棋盘地板、光源正常）
   - 光顶点生成：259071 个（来自 262144 条光路）
   - 无 overexposure 或全黑问题

#### 待完成
1. 🔴 **光路 OptiX 追踪** — 当前光路只生成 pathLength=0 的顶点（在光源表面），没有实际追踪光线
   - 需要让光路也通过 OptiX 进行光线追踪（hit/miss）
   - 方案 A：在 `trace_rays.cu` 中增加光路模式分支
   - 方案 B：为光路创建独立的 OptiX trace pipeline
   - **这是 caustics 出现的关键**：只有光路穿过玻璃球折射到达 diffuse 表面，才会产生 caustic 光顶点

2. 🔴 **Shadow Ray（可见性测试）** — vertex connection 目前没有做遮挡检测
   - eye path 顶点与 light vertex 之间可能被其他几何体遮挡
   - 需要在 connection 之前用 shadow ray 测试可见性

3. 🟡 **多 bounce 光路** — 当前只支持单次 bounce（pathLength ≤ 1）
   - 参考实现支持多次 bounce（光线在场景中多次反射/折射后存入缓存）
   - 需要在 `processLightHits` 中采样 BSDF、继续追踪

4. 🟡 **MIS 权重** — 当前没有在 vertex connection 和直接光照之间做 MIS
   - 需要 Power Heuristic 平衡 unidirectional 贡献和 BDPT connection 贡献

---

## 已知问题：SpecularTransmission（玻璃 BSDF）

### 状态：搁置 — 等待 BDPT 光路追踪完成后重新评估
**原因**: 单向 PT 下的 "暗球" 主要是算法限制，BDPT 实现后可重新评估

#### 已修正的 BSDF 问题
- ✅ transmittance 参数：强制 (1,1,1) 而非 albedo
- ✅ BSDF 公式：包含 `/|cos|` 项，匹配 PBRT delta BSDF 规则
- ✅ PDF 中的 `eta^2` 修正：`result.pdf = Ft * etaRatio2`
- ✅ 函数调用链确认：`BSDFType_SpecularTransmission` → `sampleDielectricBSDF_PBRT`

#### 仍需验证（BDPT 完成后）
- `refractVector` 函数的方向约定和实现
- 折射光线的 ray origin offset 是否导致 self-intersection
- Local/World 坐标系转换的正确性

---

## 功能对比

| 模块 | 原版 VLR | VLR_WF | 状态 |
|------|---------|--------|------|
| 基础渲染 | ✓ | ✓ | ✅ |
| Wavefront 架构 | ✗ | ✓ | ✅ |
| 材质系统 | 8种 | 14种 | ⚠️ SpecularTransmission 待验证 |
| 光源系统 | 4种 | 3种 | ✅ 完成 |
| 纹理系统 | 完整 | 基础 | 🟡 中 |
| 调试模式 | 12种 | 17种 | ✅ 完成 |
| 降噪 | OptiX | API已实现 | 🟡 中 |
| **双向路径追踪** | **✗** | **框架已搭建** | **🟡 光路追踪待完成** |

---

## 近期优先级

### 高优先级
1. ~~**光源系统**~~ ✅ **已完成** (2026-03-08)
2. ~~**SpecularTransmission Bug 修复**~~ 🟡 **搁置** — 等待 BDPT 完善
3. **LVC-BPT 光路追踪** 🔴 **进行中** (2026-03-12 ~)
   - ✅ 框架搭建（数据结构、光路生成、vertex connection）
   - 🔴 光路 OptiX 追踪（让光线穿过玻璃球产生 caustic 光顶点）
   - 🔴 Shadow ray 可见性测试
   - 🟡 多 bounce 光路
   - 🟡 MIS 权重

### 中优先级
4. **降噪与后处理**: OptiX Denoiser, AOV 系统
5. **调试增强**: ProbePixel 完整实现
6. **Shared Memory 缓存**

### 低优先级
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
- ✅ LVC-BPT 双向路径追踪框架 (2026-03-12): 数据结构、光路生成、vertex connection

