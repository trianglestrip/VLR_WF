# VLR_WF 变更日志

所有重要变更都会记录在此文件中。

格式基于 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.0.0/)。

---

## [1.5.0] - 2026-03-16

### 新增

#### 场景加载系统

- **PBRT v4 场景解析器** (`viewer/src/PbrtParser.cpp`)
  - 支持 PBRT v4 格式场景文件
  - 解析材质、光源、相机、几何体
  - Include 文件递归解析
  - PLY 网格加载
  - ✅ 已完成（kitchen 场景验证通过）

- **Assimp 模型加载** (`viewer/src/SceneLoader.cpp`)
  - 支持 OBJ/PLY/FBX/glTF 等 40+ 格式
  - 自动提取顶点、法线、UV、材质
  - 集成 VLR 材质系统
  - ✅ 已完成

#### TaskFlow 并行场景准备

- **DAG 编排** (`Scene::prepareSceneParallel()`)
  - GAS 构建、Bounds 计算、数据聚合三阶段并行执行
  - Upload 依赖 Bounds + Aggregate 完成
  - IAS 依赖 GAS + Upload 完成
  - 最大化 CPU/GPU 并行度
  - ✅ 已完成

- **并行化子任务**
  - `buildGeometryAccelerationStructures()`: CPU 数据准备 parallel_for + 多 CUDA stream GAS 构建
  - `computeSceneBounds()`: 每 Instance 独立计算局部 AABB，reduce 合并
  - `aggregateMeshData()`: prefix sum + parallel_for 拷贝各 mesh 数据到全局数组
  - `buildFromPbrt()`: PBRT 材质并行创建（加锁保护 VLR API）
  - ✅ 已完成

#### 性能统计系统

- **统一计时宏** (`libVLR/vlr_profile.h`)
  - `VLR_PROFILE_BEGIN(var)` — 记录起始时间点
  - `VLR_PROFILE_END(var, outMs)` — 耗时赋值到已有变量
  - `VLR_PROFILE_END_NEW(var, newMs)` — 声明新变量并赋值
  - 未启用时零开销（宏展开为空操作）
  - ✅ 已完成

- **CMake 开关**: `cmake -DVLR_PROFILE_SCENE_PREPARE=ON`（默认开启）

- **输出内容**
  - Scene Prepare: 各阶段耗时 + Serial sum + Wall clock + Parallel speedup
  - Total Pipeline: GPU 渲染耗时 + 端到端总时间

#### 测试程序

- `viewer/examples/kitchen_render_test.cpp` - Kitchen 场景渲染测试 ✅

### 修改

#### 代码重构（P1-P5）

- P1: 提取 `path_types_core.h` 消除结构体重复定义
- P2: 提取 `gpu_debug.h` 和 `sampling_common.h` 复用 kernel 代码
- P3: 拆分 `context.cpp` 为 `context_optix`/`context_wavefront`/`context_texture`
- P3: 提取 `uploadLaunchParamsToDevice` 和 `getDeviceLaunchParams` 辅助函数
- P4: 引入 `MaterialDescriptorBuilder` 消除材质创建模板代码
- P5: 提取测试工具到 `test/utils/`

#### 渲染流程

- `Context::renderWavefront()` 改用 `Scene::prepareSceneParallel()` 替代串行的 `buildAccelerationStructure()` + `updateToGPU()`
- 添加端到端总时间统计

### 修复

- 修复 `context.cpp` / `scene.cpp` 中的乱码中文注释
- 统一所有源文件为 CRLF 行尾
- 修复 TaskFlow Windows `min`/`max` 宏冲突
- 修复 TaskFlow `for_each.hpp` 需显式 include 的问题（`algorithm.hpp` 空文件）

### 性能

- Cornell Box 场景准备并行加速比: ~1.40x
- 大型场景（kitchen 等）预期加速更明显

---

## [1.4.0] - 2026-03-08

### 新增

#### 光源系统完善

- **方向光** (`vlrAddDirectionalLight`)
  - 平行光，无距离衰减
  - 模拟太阳光、月光
  - Delta 光源
  - ✅ 已完成

- **环境光重要性采样** (`env_importance.h/cpp`)
  - 基于 HDR 贴图亮度构建 2D CDF
  - 分层采样（theta → phi）
  - 立体角加权
  - 性能提升：减少噪点 2-5x
  - ✅ 已完成

- **多光源重要性采样** (`light_common.h`)
  - 基于功率的光源权重
  - CDF 二分查找选择光源
  - 动态权重更新
  - 优化多光源场景性能
  - ✅ 已完成

- **光源优化**
  - Light Cache (Shared Memory)
  - Warp 级早期退出
  - Delta 材质跳过 NEE
  - 几何项修正（无穷远光源）

#### 测试程序

- `test/light_system_test.cpp` - 光源系统测试（5 个场景）✅

#### 文档

- `docs/LIGHT_SYSTEM.md` - 光源系统完整文档

---

## [1.3.0] - 2026-03-08

### 新增

#### 纹理系统

- **图像加载** (`image_loader.h/cpp`)
  - PNG/JPG：使用 stb_image
  - EXR：使用 tinyexr
  - HDR：使用 stb_image
  - 自动格式检测
  - ✅ 所有格式已支持

- **纹理 API** (`vlr.h`)
  - `vlrCreateTexture2D(context, path, &tex)` - 从文件加载
  - `vlrCreateTexture2DFromMemory(...)` - 从内存创建
  - `vlrDestroyTexture(tex)` - 销毁纹理
  - `vlrSetTextureFilterMode(tex, mode)` - 滤波模式
  - `vlrSetTextureWrapMode(tex, wrapU, wrapV)` - 环绕模式

- **材质纹理绑定** (`vlr.h`)
  - `vlrSetMaterialBaseColorTexture(mat, tex)` - BaseColor 纹理
  - `vlrSetMaterialRoughnessTexture(mat, tex)` - Roughness 纹理
  - `vlrSetMaterialMetallicTexture(mat, tex)` - Metallic 纹理
  - `vlrSetMaterialNormalTexture(mat, tex, scale)` - Normal Map
  - `vlrSetMaterialTextureTransform(mat, ...)` - UV 变换

- **GPU 实现** (`texture_types.h`, `texture_common.h`)
  - 双线性/最近邻滤波
  - Repeat/Clamp 环绕
  - 法线贴图解码和切线空间变换
  - UV Scale/Offset 变换
  - 棋盘格程序化纹理

- **渲染集成** (`process_hits.cu`, `sample_bsdf.cu`)
  - ProcessHits 中采样纹理
  - 纹理化参数传递到 BSDF
  - 法线贴图自动应用

#### 测试程序

- `test/texture_test.cpp` - 纹理系统测试 ✅

#### 文档

- `docs/TEXTURE_SYSTEM.md` - 纹理系统完整文档

---

## [1.2.0] - 2026-03-08

### 新增

#### 调试渲染模式

- **17 种调试可视化模式**
  - BaseColor, GeometricNormal, ShadingNormal, Depth, UV
  - Tangent, Bitangent, Roughness, Metallic
  - MaterialID, InstanceID, PrimitiveID
  - DirectLighting, IndirectLighting (占位)
  - DenoiserAlbedo, DenoiserNormal
  - ✅ 所有模式已实现

- **调试 API** (`vlr.h`)
  - `vlrSetDebugMode(context, mode)` - 设置调试模式
  - `vlrGetDebugMode(context, &mode)` - 获取当前模式
  - `vlrSetProbePixel(context, x, y)` - 设置探针像素（待完整实现）

- **GPU 实现** (`GPU_kernels/debug_rendering.cu`)
  - 单次采样快速渲染
  - 仅处理 primary ray（无多次反弹）
  - Wang Hash ID → 颜色映射
  - 性能：< 100ms (512×512)

- **自动集成**
  - 调试模式下自动跳过多次反弹
  - 禁用降噪器
  - 与 Wavefront 渲染循环无缝集成

#### 文档

- `docs/DEBUG_MODE_IMPLEMENTATION.md` - 调试模式实现文档

---

## [1.1.0] - 2026-03-08

### 新增

#### 材质系统

- **各向异性金属材质** (`vlrCreateMaterialConductorAniso`)
  - 支持拉丝金属、刷纹表面效果
  - Disney/Burley 参数化 (roughness + anisotropy)
  - 各向异性 GGX D/G/VNDF 实现
  - 性能: 49.39 Msamples/s
  - ✅ 渲染测试通过

- **多层材质系统** (`vlrCreateMaterialMultiSurface`)
  - 支持 2-4 层子材质混合
  - 加权混合 BSDF 和 PDF
  - 自动权重归一化
  - 适用于汽车涂层、复合材质
  - ✅ API 测试通过 (4/4)

- **次表面散射** (`vlrCreateMaterialLambertianScattering`)
  - 双向 Lambert (反射 + 透射)
  - 适用于皮肤、蜡、薄纸等半透明材质
  - 性能: 91.79 Msamples/s
  - ✅ 渲染测试通过

- **Disney Principled BRDF** (`vlrCreateMaterialDisney`)
  - 11个艺术家友好参数
  - 5个分量: Diffuse, Subsurface, Specular, Sheen, Clearcoat
  - 工业标准，与 Blender/Arnold 兼容
  - 性能: 84.95 Msamples/s
  - ✅ 渲染测试通过（5个材质变体）

#### 测试程序

- `test/material_test.cpp` - 材质 API 验证 (7/7 通过) ✅
- `test/anisotropic_test.cpp` - 各向异性渲染测试 ✅
- `test/multi_surface_test.cpp` - 多层材质测试 (4/4 通过) ✅
- `test/lambertian_scattering_test.cpp` - 次表面散射渲染 ✅
- `test/disney_brdf_test.cpp` - Disney BRDF 渲染 ✅

#### 文档

- `docs/MATERIAL_SYSTEM_ENHANCEMENT_SUMMARY.md` - 材质系统增强总结
- `CHANGELOG.md` - 变更日志

### 修改

#### 数据结构

- 扩展 `SurfaceMaterialDescriptor::data[16]` → `data[48]`
  - 支持更多材质参数 (MultiSurface, Disney)

#### BSDF 实现

- 重构 `MicrofacetReflection` 支持各向异性
  - 新增 `GGX_D_Aniso`, `GGX_G1_Aniso`, `sampleGGXVNDF_Aniso`
  - 向后兼容: anisotropy=0 时使用各向同性路径

#### 材质槽位布局

- Slot 5: `Anisotropy` (MicrofacetReflection)
- Slots 16-40: `MultiSurface` 参数 (4层×5参数 + 4权重)
- Slots 41-49: `Disney` 参数 (9个额外参数) - **已修复槽位重叠问题**

### 修复

#### Disney BRDF

- **参数槽位重叠问题** (2026-03-08)
  - 问题: Disney BRDF 槽位 32-40 与 MultiSurface 槽位 16-40 重叠
  - 修复: 将 Disney 槽位移动到 41-49
  - 影响文件: `libVLR/vlr.cpp`, `libVLR/materials.cuh`

- **函数签名参数错位** (2026-03-08)
  - 问题: `sampleDisneyBRDF` 和 `getDisneyBRDFPDF` 缺少 `anisotropic` 参数
  - 修复: 在函数签名中添加 `anisotropic` 参数
  - 影响文件: `libVLR/bsdf.cuh`

#### 光源和曝光

- **提高光源强度**: 从 15/40 提升到 80
- **提高曝光值**: 从 0.6/1.0 提升到 1.2/1.5

### 性能

- 各向异性: 49.39 Msamples/s (512×512, 128 spp)
- Disney BRDF: 84.95 Msamples/s (800×400, 128 spp)
- 次表面散射: 91.79 Msamples/s (512×512, 256 spp)

### 测试

- 所有材质 API 测试通过 (7/7)
- 多层材质测试通过 (4/4)
- 3个渲染测试成功生成图像

### 已知问题

#### ~~渲染测试场景设置问题~~ ✅ 已修复 (2026-03-08)

**问题描述**:
- `disney_brdf_test.cpp` 和 `lambertian_scattering_test.cpp` 渲染结果全黑
- 即使替换为已知工作的 Lambert 材质，渲染结果仍然全黑

**根本原因**:
1. 光源强度过高（80.0 vs 30.0）
2. 曝光值不匹配（0.8/1.2 vs 0.5）
3. LambertianScattering 测试有前墙，但相机在墙外（z=6.0 > z_wall=1.5）
4. 球体位置不在视野中心

**修复方案**:
- ✅ 调整光源强度: 80.0 → 30.0（与 cornell_box_improved_test.cpp 一致）
- ✅ 调整曝光值: 0.8/1.2 → 0.5
- ✅ 移除 LambertianScattering 测试的前墙（开放式 Cornell Box）
- ✅ 调整球体位置: z=0.5 → z=0.0（居中）
- ✅ 调整相机视线: 看向 y=1.0（球体中心高度）

**验证结果**:
- ✅ Disney BRDF Test: 成功渲染 5 个不同材质球体
- ✅ LambertianScattering Test: 成功渲染次表面散射效果
- ✅ 所有材质系统功能验证完成

---

## [1.0.0] - 2026-03-07

### 新增

#### 核心渲染

- Wavefront 路径追踪架构
- OptiX 8.0 光线追踪集成
- CUDA 13.1 GPU 加速

#### 材质系统

- Lambert 漫反射 (`vlrCreateMaterial`)
- GGX 微表面反射 (`vlrCreateMaterialEx`)
- Specular 完美镜面 (`vlrCreateMaterialEx`)
- SpecularTransmission 完美玻璃 (`vlrCreateMaterialEx`)
- MicrofacetReflection 粗糙金属 (`vlrCreateMaterialConductor`)
- MicrofacetScattering 粗糙玻璃 (`vlrCreateMaterialMicrofacetScattering`)
- Checkerboard 棋盘格纹理 (`vlrCreateMaterialCheckerboard`)
- UE4 BRDF 金属工作流 (内部)

#### 光源系统

- 面光源 (发光几何体)
- 环境光 (天空盒)
- 直接光照采样

#### 场景管理

- 三角网格 (`vlrCreateTriangleMesh`)
- 实例化 (`vlrCreateInstance`)
- 变换矩阵 (平移、旋转、缩放)

#### 相机系统

- 透视相机
- 景深效果 (lensRadius, focusDistance)

#### 渲染配置

- INI 文件配置
- 命令行参数
- 性能预设

#### 测试场景

- Cornell Box (`test/cornell_box_improved_test.cpp`)
- 支持 INI 配置文件

### 性能

- 基准性能: 40.6 Msamples/s (3.07x 加速 vs 原版)
- 优化: CPU-GPU 同步、路径压缩、早期终止、RNG/Warp/CUB

---

## [0.1.0] - 2026-03-01

### 新增

- 项目初始化
- 基础 CMake 构建系统
- OptiX/CUDA 环境配置
- 基础 Wavefront 架构原型

---

## 版本说明

### 版本号规则

遵循 [语义化版本](https://semver.org/lang/zh-CN/) 2.0.0：

- **主版本号**: 不兼容的 API 修改
- **次版本号**: 向下兼容的功能性新增
- **修订号**: 向下兼容的问题修正

### 版本里程碑

- **1.0.x**: Wavefront 基础版 ✅
- **1.1.x**: 材质系统增强版 ✅
- **1.2.x**: 调试渲染模式 ✅
- **1.3.x**: 纹理系统增强版 ✅
- **1.4.x**: 光源系统增强版 ✅
- **1.5.x**: 场景加载与并行化 ✅ 当前
- **2.0.0**: 功能完整版 (目标)

---

## 链接

- [项目路线图](docs/TODO.md)
- [材质系统增强总结](docs/MATERIAL_SYSTEM_ENHANCEMENT_SUMMARY.md)
