# VLR_WF 材质系统调试指南

> 材质系统 Task 1-5 调试点说明文档

**创建日期**: 2026-03-08  
**状态**: 调试中  
**目标**: 验证材质参数从 CPU 创建到 GPU 渲染的完整流程

---

## 📋 调试任务概览

| Task | 描述 | 调试位置 | 状态 |
|------|------|---------|------|
| Task 1 | CPU端材质参数验证 | `scene.cpp` | ✅ 已添加 |
| Task 2 | CPU→GPU数据传输验证 | `scene.cpp::updateToGPU` | ✅ 已添加 |
| Task 3 | GPU端材质类型读取验证 | `sample_bsdf.cu` | ✅ 已添加 |
| Task 4 | BSDF采样分发验证 | `bsdf_common.h::sampleBSDFWithU2` | ✅ 已添加 |
| Task 5 | BSDF评估和渲染验证 | `bsdf_common.h::evaluateMicrofacetReflectionBSDF`, `sample_lights.cu` | ✅ 已添加 |

---

## 🔍 Task 1: CPU端材质参数验证

### 调试位置
- **文件**: `libVLR/scene.cpp`
- **函数**: `Scene::createMaterialEx()`, `Scene::createMaterialConductor()`

### 调试输出内容

#### createMaterialEx
```
[Material Debug] createMaterialEx: index=<材质索引>, bsdfType=<类型ID>
  Albedo: (<R>, <G>, <B>)
  Roughness: <粗糙度>, Metallic: <金属度>, IOR: <折射率>
  Emission: (<R>, <G>, <B>)
  bsdfProcedureSetIndex: <类型ID>
  Verify data[AlbedoR]: <实际值> (expected <期望值>)
  Verify data[IOR]: <实际值> (expected <期望值>)
```

#### createMaterialConductor
```
[Material Debug] createMaterialConductor: index=<材质索引>, bsdfType=<类型ID> (MicrofacetReflection)
  Eta: (<R>, <G>, <B>)
  Kappa: (<R>, <G>, <B>)
  Roughness: <粗糙度>
  bsdfProcedureSetIndex: <类型ID>
  Verify data[EtaR]: <实际值> (expected <期望值>)
  Verify data[KappaR]: <实际值> (expected <期望值>)
  Verify data[Roughness]: <实际值> (expected <期望值>)
```

### 验证要点
1. ✅ `bsdfProcedureSetIndex` 是否与 `bsdfType` 一致
2. ✅ `data[]` 数组中的 float 值是否正确写入（通过 reinterpret_cast 读回验证）
3. ✅ 导体材质的 eta/kappa 参数是否正确
4. ✅ 透射材质的 IOR 参数是否正确

---

## 🔍 Task 2: CPU→GPU数据传输验证

### 调试位置
- **文件**: `libVLR/scene.cpp`
- **函数**: `Scene::updateToGPU()`

### 调试输出内容
```
[Material Debug] updateToGPU: Uploading <N> materials to GPU
  Material[<索引>]: bsdfProcedureSetIndex=<类型ID>
    Albedo: (<R>, <G>, <B>)
    Roughness: <粗糙度>, IOR: <折射率>
    Eta: (<R>, <G>, <B>)
    Kappa: (<R>, <G>, <B>)
```

### 验证要点
1. ✅ 材质数量是否正确
2. ✅ 每个材质的 `bsdfProcedureSetIndex` 是否与创建时一致
3. ✅ 材质参数在上传前是否完整
4. ✅ `m_materialBuffer->copyToDevice()` 是否成功执行

---

## 🔍 Task 3: GPU端材质类型读取验证

### 调试位置
- **文件**: `libVLR/GPU_kernels/sample_bsdf.cu`
- **Kernel**: `sampleBSDF`

### 调试输出内容
```
[GPU SampleBSDF] pathIndex=<路径索引>, geomInstIndex=<几何实例索引>, materialIndex=<材质索引>
  bsdfProcedureSetIndex=<类型ID>, getBSDFType()=<类型ID>
  Albedo: (<R>, <G>, <B>)
  Roughness: <粗糙度>, IOR: <折射率>
  Eta: (<R>, <G>, <B>)
  Kappa: (<R>, <G>, <B>)
```

### 验证要点
1. ✅ `bsdfProcedureSetIndex` 是否与 CPU 端一致
2. ✅ `getBSDFType()` 返回值是否正确
3. ✅ 材质参数通过 `getMaterialDataAsFloats()` 读取是否正确
4. ✅ 几何实例的 `materialIndex` 是否指向正确的材质

### 限制
- 仅输出前 10 条路径，避免过多调试信息

---

## 🔍 Task 4: BSDF采样分发验证

### 调试位置

#### 位置 1: `sample_bsdf.cu` - 采样结果
```
[GPU SampleBSDF] pathIndex=<路径索引>: BSDF sampling result
  sampledBSDFType=<采样的BSDF类型>, isDelta=<是否Delta>
  pdf=<概率密度>
  f=(<BSDF值 4个分量>)
  dirLocal=(<采样方向 局部坐标>)
```

#### 位置 2: `bsdf_common.h::sampleBSDFWithU2` - 分支执行
```
[GPU sampleBSDFWithU2] BSDFType=<类型ID>
[GPU sampleBSDFWithU2] Entering MicrofacetReflection branch
  eta=(<R>,<G>,<B>), kappa=(<R>,<G>,<B>), roughness=<粗糙度>
[GPU sampleBSDFWithU2] Entering SpecularTransmission branch
  ior=<折射率>, dispersion=<色散强度>
```

### 验证要点
1. ✅ switch 语句是否进入正确的 case 分支
2. ✅ `MicrofacetReflection` 分支是否被执行（金属材质）
3. ✅ `SpecularTransmission` 分支是否被执行（玻璃材质）
4. ✅ BSDF 采样结果的 `pdf` 是否 > 0
5. ✅ BSDF 采样结果的 `f` 是否非零且有限

### 限制
- `sampleBSDFWithU2` 仅在 `threadIdx.x==0 && blockIdx.x==0` 时输出
- `sample_bsdf.cu` 仅输出前 10 条路径

---

## 🔍 Task 5: BSDF评估和渲染验证

### 调试位置

#### 位置 1: `bsdf_common.h::evaluateMicrofacetReflectionBSDF`
```
[GPU evaluateMicrofacetReflectionBSDF]
  eta=(<R>,<G>,<B>), kappa=(<R>,<G>,<B>), roughness=<粗糙度>
  NdotL=<入射余弦>, NdotV=<出射余弦>
  cosTheta=<半矢量余弦>, D=<GGX分布>, G=<Smith遮蔽>, denom=<分母>
  Fresnel F=(<4个分量>)
  Final BSDF=(<4个分量>)
```

#### 位置 2: `sample_lights.cu` - NEE 中的 BSDF 评估
```
[GPU SampleLights] pathIndex=<路径索引>: evaluateBSDF result
  BSDFType=<类型ID>
  fs=(<BSDF值 4个分量>)
  dirInLocal=(<入射方向>), dirOutLocal=(<出射方向>)
```

### 验证要点
1. ✅ Fresnel 计算是否正确（`FresnelConductor` 函数）
2. ✅ GGX 分布函数 D 是否合理（通常 0.01 ~ 10）
3. ✅ Smith 遮蔽函数 G 是否合理（0 ~ 1）
4. ✅ 最终 BSDF 值是否非零且有限
5. ✅ NEE（Next Event Estimation）中的 BSDF 评估是否正确

### 限制
- `evaluateMicrofacetReflectionBSDF` 仅在 `threadIdx.x==0 && blockIdx.x==0` 时输出
- `sample_lights.cu` 仅输出前 5 条路径

---

## 🚀 运行调试

### 方法 1: 使用调试脚本（推荐）
```powershell
.\debug_material_system.ps1
```

### 方法 2: 手动运行
```powershell
# 编译
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --config Debug -j 8

# 运行测试（小分辨率，快速验证）
.\build\bin\Debug\cornell_box_improved_test.exe -w 256 -h 256 -s 4 -o bin/debug_material.png
```

### 方法 3: 使用配置文件
```powershell
.\build\bin\Debug\cornell_box_improved_test.exe config_presets/preview_scene.ini config_presets/preview_performance.ini
```

---

## 📊 预期调试输出示例

### CPU 端输出（Task 1 & 2）

```
[Material Debug] createMaterialEx: index=0, bsdfType=1
  Albedo: (0.800, 0.800, 0.800)
  Roughness: 0.000, Metallic: 0.000, IOR: 0.000
  Emission: (0.000, 0.000, 0.000)
  bsdfProcedureSetIndex: 1
  Verify data[AlbedoR]: 0.800 (expected 0.800)
  Verify data[IOR]: 0.000 (expected 0.000)

[Material Debug] createMaterialConductor: index=5, bsdfType=3 (MicrofacetReflection)
  Eta: (0.200, 0.920, 1.100)
  Kappa: (3.680, 2.460, 2.140)
  Roughness: 0.050
  bsdfProcedureSetIndex: 3
  Verify data[EtaR]: 0.200 (expected 0.200)
  Verify data[KappaR]: 3.680 (expected 3.680)
  Verify data[Roughness]: 0.050 (expected 0.050)

[Material Debug] updateToGPU: Uploading 8 materials to GPU
  Material[0]: bsdfProcedureSetIndex=1
    Albedo: (0.800, 0.800, 0.800)
    Roughness: 0.000, IOR: 0.000
    Eta: (0.000, 0.000, 0.000)
    Kappa: (0.000, 0.000, 0.000)
  ...
```

### GPU 端输出（Task 3, 4, 5）

```
[GPU SampleBSDF] pathIndex=0, geomInstIndex=2, materialIndex=5
  bsdfProcedureSetIndex=3, getBSDFType()=3
  Albedo: (0.000, 0.000, 0.000)
  Roughness: 0.050, IOR: 0.000
  Eta: (0.200, 0.920, 1.100)
  Kappa: (3.680, 2.460, 2.140)

[GPU sampleBSDFWithU2] BSDFType=3
[GPU sampleBSDFWithU2] Entering MicrofacetReflection branch
  eta=(0.200,0.920,1.100), kappa=(3.680,2.460,2.140), roughness=0.050

[GPU SampleBSDF] pathIndex=0: BSDF sampling result
  sampledBSDFType=3, isDelta=0
  pdf=0.123456
  f=(0.234567, 0.345678, 0.456789, 0.345678)
  dirLocal=(0.123, 0.456, 0.789)

[GPU evaluateMicrofacetReflectionBSDF]
  eta=(0.200,0.920,1.100), kappa=(3.680,2.460,2.140), roughness=0.050
  NdotL=0.866, NdotV=0.707
  cosTheta=0.987, D=1.234567, G=0.567890, denom=2.441406
  Fresnel F=(0.678901,0.789012,0.890123,0.786012)
  Final BSDF=(0.345678,0.456789,0.567890,0.456786)

[GPU SampleLights] pathIndex=0: evaluateBSDF result
  BSDFType=3
  fs=(0.345678, 0.456789, 0.567890, 0.456786)
  dirInLocal=(0.123,0.456,0.789), dirOutLocal=(0.234,0.567,0.890)
```

---

## 🔧 调试点详细说明

### Task 1: CPU端参数验证

**目的**: 确认材质参数在 CPU 端创建时正确写入 `SurfaceMaterialDescriptor`

**关键检查**:
1. `bsdfProcedureSetIndex` 是否等于 `bsdfType`
2. `data[]` 数组通过 `reinterpret_cast<uint32_t*>` 写入的 float 值是否正确
3. 导体材质的 eta/kappa 是否按 RGB 分量正确存储
4. 透射材质的 IOR 是否正确存储

**常见问题**:
- ❌ `reinterpret_cast` 使用错误，导致数据损坏
- ❌ `MaterialDataLayout` 索引错误，参数写入错误位置
- ❌ 未初始化的字段（应使用 `memset(&mat, 0, sizeof(mat))`）

---

### Task 2: CPU→GPU数据传输验证

**目的**: 确认材质数据在上传到 GPU 前完整无误

**关键检查**:
1. `m_materials.size()` 是否与场景中的材质数量一致
2. 每个材质的 `bsdfProcedureSetIndex` 是否保持不变
3. 材质参数在 CPU 端的最终状态（上传前）

**常见问题**:
- ❌ 材质索引错误，几何实例引用了错误的材质
- ❌ 材质数据在上传前被意外修改
- ❌ `copyToDevice` 失败但未检测到

---

### Task 3: GPU端材质类型读取验证

**目的**: 确认 GPU 端能正确读取材质数据

**关键检查**:
1. `geomInst.materialIndex` 是否指向正确的材质
2. `matDesc.bsdfProcedureSetIndex` 是否与 CPU 端一致
3. `getBSDFType(matDesc)` 返回值是否正确
4. `getMaterialDataAsFloats()` 读取的参数是否与 CPU 端一致

**常见问题**:
- ❌ 材质索引越界或错误
- ❌ GPU 端读取到的数据与 CPU 端不一致（内存损坏）
- ❌ `getBSDFType()` 逻辑错误

---

### Task 4: BSDF采样分发验证

**目的**: 确认 `sampleBSDFWithU2` 的 switch 语句正确分发到对应的 BSDF 类型

**关键检查**:
1. switch 语句是否进入正确的 case 分支
2. `MicrofacetReflection` 分支是否被执行（金属材质）
3. `SpecularTransmission` 分支是否被执行（玻璃材质）
4. 参数提取函数（`getMicrofacetReflectionParams` 等）是否返回正确值
5. BSDF 采样结果是否有效（`pdf > 0`, `f != Zero`）

**常见问题**:
- ❌ switch 语句进入 default 分支（材质类型未识别）
- ❌ 参数提取函数返回错误值（如 eta=0, kappa=0）
- ❌ BSDF 采样函数返回无效结果（pdf=0 或 f=Zero）

---

### Task 5: BSDF评估和渲染验证

**目的**: 确认 BSDF 评估函数（用于 NEE）正确计算反射率

**关键检查**:
1. `evaluateMicrofacetReflectionBSDF` 的中间计算值
   - NdotL, NdotV（余弦值，应 > 0）
   - D（GGX 分布，通常 0.01 ~ 10）
   - G（Smith 遮蔽，0 ~ 1）
   - F（Fresnel，0 ~ 1）
2. 最终 BSDF 值是否合理（通常 0.001 ~ 1.0）
3. NEE 中 `evaluateBSDF` 的返回值是否与采样一致

**常见问题**:
- ❌ Fresnel 计算错误（eta/kappa 参数问题）
- ❌ GGX 分布或遮蔽函数返回 NaN/Inf
- ❌ 分母接近 0 导致除零错误
- ❌ BSDF 值过大或过小（能量不守恒）

---

## 🎯 已知问题和修复

### 问题 1: 金属材质渲染为黑色

**症状**: 金属盒子在渲染结果中显示为黑色

**可能原因**:
1. ❌ eta/kappa 参数未正确传递到 GPU
2. ❌ `getBSDFType()` 返回错误类型
3. ❌ `sampleBSDFWithU2` 未进入 `MicrofacetReflection` 分支
4. ❌ Fresnel 计算返回 0
5. ❌ BSDF 采样或评估返回 Zero

**调试步骤**:
1. 检查 Task 1 输出，确认 CPU 端 eta/kappa 正确
2. 检查 Task 2 输出，确认上传到 GPU 的数据正确
3. 检查 Task 3 输出，确认 GPU 端读取的 eta/kappa 正确
4. 检查 Task 4 输出，确认进入 `MicrofacetReflection` 分支
5. 检查 Task 5 输出，确认 Fresnel 和 BSDF 值非零

---

### 问题 2: 玻璃材质渲染为黑色

**症状**: 玻璃球在渲染结果中显示为黑色

**可能原因**:
1. ❌ IOR 参数未正确传递
2. ❌ `getBSDFType()` 返回错误类型（应为 `SpecularTransmission`）
3. ❌ `sampleBSDFWithU2` 未进入 `SpecularTransmission` 分支
4. ❌ 折射方向计算错误
5. ❌ 色散逻辑错误触发（`dispersionStrength` 应为 0）

**调试步骤**:
1. 检查 Task 1 输出，确认 IOR 正确（应为 2.4）
2. 检查 Task 3 输出，确认 GPU 端读取的 IOR 正确
3. 检查 Task 4 输出，确认进入 `SpecularTransmission` 分支
4. 检查 `dispersionStrength` 是否为 0（避免错误触发色散）

---

## 📈 调试流程

```
┌─────────────────────────────────────────────────────────────┐
│ Task 1: CPU端创建材质                                        │
│ ✓ createMaterialConductor(eta, kappa, roughness)           │
│ ✓ createMaterialEx(bsdfType, albedo, ior, ...)             │
│ ✓ 验证 SurfaceMaterialDescriptor 数据写入                   │
└─────────────────────┬───────────────────────────────────────┘
                      │
                      ▼
┌─────────────────────────────────────────────────────────────┐
│ Task 2: CPU→GPU数据传输                                      │
│ ✓ updateToGPU() 上传 m_materials 到 m_materialBuffer       │
│ ✓ 验证材质数量和参数完整性                                   │
└─────────────────────┬───────────────────────────────────────┘
                      │
                      ▼
┌─────────────────────────────────────────────────────────────┐
│ Task 3: GPU端读取材质                                        │
│ ✓ sample_bsdf.cu: 从 materialDescriptorBuffer 读取         │
│ ✓ getBSDFType(matDesc) 返回正确类型                         │
│ ✓ getMaterialDataAsFloats() 读取参数                        │
└─────────────────────┬───────────────────────────────────────┘
                      │
                      ▼
┌─────────────────────────────────────────────────────────────┐
│ Task 4: BSDF采样分发                                         │
│ ✓ sampleBSDFWithU2 switch 进入正确分支                      │
│ ✓ MicrofacetReflection: 提取 eta/kappa/roughness           │
│ ✓ SpecularTransmission: 提取 ior/dispersion                │
│ ✓ 采样结果有效（pdf > 0, f != Zero）                        │
└─────────────────────┬───────────────────────────────────────┘
                      │
                      ▼
┌─────────────────────────────────────────────────────────────┐
│ Task 5: BSDF评估和渲染                                       │
│ ✓ evaluateMicrofacetReflectionBSDF: Fresnel + GGX          │
│ ✓ evaluateBSDF 在 sample_lights.cu 中正确调用               │
│ ✓ 最终渲染结果正确（金属有反射，玻璃有折射）                  │
└─────────────────────────────────────────────────────────────┘
```

---

## 🔍 关键数据结构

### SurfaceMaterialDescriptor
```cpp
struct SurfaceMaterialDescriptor {
    uint32_t bsdfProcedureSetIndex;  // BSDF 类型 ID
    uint32_t edfProcedureSetIndex;   // EDF 类型 ID (0xFFFFFFFF = 无发光)
    uint32_t data[32];               // 材质参数（以 uint32_t 存储，按 float 解释）
};
```

### MaterialDataLayout（data[] 数组索引）
```cpp
namespace MaterialDataLayout {
    constexpr int BSDFType = 0;             // data[0]: BSDF 类型
    constexpr int AlbedoR = 1;              // data[1-3]: 反照率 RGB
    constexpr int Roughness = 4;            // data[4]: 粗糙度
    constexpr int Metallic = 5;             // data[5]: 金属度
    constexpr int IOR = 6;                  // data[6]: 折射率
    constexpr int EmissionR = 7;            // data[7-9]: 发光 RGB
    constexpr int EtaR = 10;                // data[10-12]: 导体 eta RGB
    constexpr int KappaR = 13;              // data[13-15]: 导体 kappa RGB
}
```

### BSDFType 枚举
```cpp
enum BSDFType : uint32_t {
    BSDFType_Lambert = 0,
    BSDFType_LambertCheckerboard = 1,
    BSDFType_GGX = 2,
    BSDFType_MicrofacetReflection = 3,      // 导体微表面反射
    BSDFType_MicrofacetScattering = 4,      // 电介质微表面散射
    BSDFType_Specular = 5,                  // 完美镜面
    BSDFType_SpecularTransmission = 6,      // 完美透射
    BSDFType_GGXTransmission = 7,
    BSDFType_FresnelBlend = 8,
    BSDFType_UE4BRDF = 9,
    BSDFType_FrostbiteBRDF = 10,
    BSDFType_MixedBSDF = 11,
};
```

---

## 🧪 测试场景

### Cornell Box Improved Test
- **左侧**: 玻璃球（IOR=2.4, SpecularTransmission）
- **右侧**: 金属盒（Copper eta/kappa, MicrofacetReflection, roughness=0.05）
- **地板**: 棋盘格（LambertCheckerboard）
- **墙壁**: Lambert 漫反射
- **光源**: 发光矩形（Emission）

### 预期结果
- ✅ 金属盒应有铜色反射
- ✅ 玻璃球应有折射和反射
- ✅ 地板应有黑白棋盘格
- ✅ 墙壁应有正确的颜色（红、绿、白）

---

## 📝 调试日志分析

### 正常情况
1. **Task 1**: 所有 "Verify data[X]" 输出应与 "expected" 值一致
2. **Task 2**: 上传的材质数据应与 Task 1 创建的一致
3. **Task 3**: GPU 端读取的数据应与 Task 2 上传的一致
4. **Task 4**: switch 语句应进入正确的 case 分支，采样结果有效
5. **Task 5**: BSDF 评估结果应非零且有限

### 异常情况

#### 情况 A: Task 1 验证失败
```
Verify data[EtaR]: 0.000 (expected 0.200)  ❌
```
**原因**: `reinterpret_cast` 使用错误或 `MaterialDataLayout` 索引错误  
**修复**: 检查 `scene.cpp` 中的数据写入逻辑

#### 情况 B: Task 3 读取错误
```
GPU: Eta: (0.000, 0.000, 0.000)  ❌
CPU: Eta: (0.200, 0.920, 1.100)  ✓
```
**原因**: GPU 端读取位置错误或数据未正确上传  
**修复**: 检查 `getMicrofacetReflectionParams()` 实现

#### 情况 C: Task 4 进入错误分支
```
[GPU sampleBSDFWithU2] BSDFType=0  ❌ (应为 3)
```
**原因**: `getBSDFType()` 返回错误或 `bsdfProcedureSetIndex` 被覆盖  
**修复**: 检查 `getBSDFType()` 实现和材质创建逻辑

#### 情况 D: Task 5 Fresnel 为 0
```
Fresnel F=(0.000000,0.000000,0.000000,0.000000)  ❌
```
**原因**: eta/kappa 参数错误或 `FresnelConductor()` 计算错误  
**修复**: 检查 Fresnel 公式和参数传递

---

## 🛠️ 移除调试代码

调试完成后，可以通过以下方式移除调试输出：

### 方法 1: 使用预处理宏（推荐）
在 `kernel_common.h` 中定义：
```cpp
// #define VLR_DEBUG_MATERIAL  // 取消注释以启用材质调试
```

然后将所有调试 printf 包裹在：
```cpp
#ifdef VLR_DEBUG_MATERIAL
    printf(...);
#endif
```

### 方法 2: 手动删除
搜索并删除所有包含 `[DEBUG Task X]` 的代码块

---

## 📚 参考资料

### 相关文件
- `libVLR/scene.cpp`: 材质创建和上传
- `libVLR/shared/material_types.h`: 材质类型定义和参数提取
- `libVLR/shared/bsdf_common.h`: BSDF 采样和评估
- `libVLR/GPU_kernels/sample_bsdf.cu`: BSDF 采样 kernel
- `libVLR/GPU_kernels/sample_lights.cu`: NEE 中的 BSDF 评估

### 相关文档
- `docs/TODO.md`: 材质系统完善计划
- `.cursor/rules/coding-standards.md`: 编码规范
- `libVLR/README.md`: 库文档

---

## ✅ 验证清单

### CPU 端（Task 1-2）
- [ ] `createMaterialConductor` 输出正确的 eta/kappa/roughness
- [ ] `createMaterialEx` 输出正确的 albedo/ior/roughness
- [ ] `updateToGPU` 输出所有材质的完整参数
- [ ] 材质索引连续且无重复

### GPU 端（Task 3-5）
- [ ] `sample_bsdf.cu` 读取的材质参数与 CPU 端一致
- [ ] `getBSDFType()` 返回正确的 BSDF 类型
- [ ] `sampleBSDFWithU2` 进入正确的 case 分支
- [ ] BSDF 采样结果有效（pdf > 0, f != Zero）
- [ ] `evaluateMicrofacetReflectionBSDF` 返回非零值
- [ ] Fresnel 计算结果合理（0 ~ 1）

### 渲染结果
- [ ] 金属盒子有铜色反射
- [ ] 玻璃球有折射效果
- [ ] 无黑色或异常亮的区域

---

**创建日期**: 2026-03-08  
**维护者**: VLR 开发团队  
**状态**: 调试中
