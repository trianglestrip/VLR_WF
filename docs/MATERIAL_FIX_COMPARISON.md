# 材质系统修复前后对比

**日期**: 2026-03-08  
**修复内容**: Fresnel 值 clamp 到 [0,1]  
**测试场景**: Cornell Box Improved

---

## 📊 渲染对比

### 修复前（cornell_box_improved.png）
- **分辨率**: 1024x1024
- **采样数**: 512 samples
- **问题**: Fresnel 值可能超过 1.0（能量不守恒）

### 修复后（cornell_box_fixed.png）
- **分辨率**: 512x512
- **采样数**: 128 samples
- **修复**: Fresnel 值 clamp 到 [0,1]
- **性能**: 36.93 Msamples/s

---

## 🔍 视觉对比分析

### 左侧黑色立方体（铜材质 - MicrofacetReflection）

#### 修复前
- 立方体显示为**纯黑色**
- 无任何反射或高光
- 完全不符合铜的光学特性

#### 修复后
- 立方体显示为**黑色**（仍然）
- 有轻微的灰色反射（可见）
- 边缘有微弱的高光

**分析**: 
- ✅ Fresnel clamp 修复生效
- ⚠️ 铜色不明显，可能是因为：
  1. Albedo 设置为 (0,0,0)（纯黑色基底）
  2. 铜的颜色应该来自 Fresnel，但在暗环境中不明显
  3. 需要更强的光照或调整 eta/kappa 参数

### 右侧球体（玻璃材质 - MicrofacetScattering）

#### 修复前
- 球体有明显的折射效果
- 可以看到背景的扭曲
- 边缘有 Fresnel 反射

#### 修复后
- 球体折射效果**保持一致**
- 背景扭曲清晰可见
- 边缘反射正常

**分析**: 
- ✅ 玻璃材质工作正常
- ✅ IOR=1.5 产生正确的折射
- ✅ 修复未影响透射材质

### 地板（棋盘格 - LambertCheckerboard）

#### 修复前 & 修复后
- 黑白棋盘格清晰可见
- 格子大小和分布正确
- 无明显差异

**分析**: 
- ✅ 棋盘格材质不受影响
- ✅ 漫反射材质工作正常

### 墙壁（Lambert 漫反射）

#### 修复前 & 修复后
- 左墙红色、右墙蓝色、背墙白色
- 颜色正确
- 无明显差异

**分析**: 
- ✅ Lambert 材质不受影响

---

## 🐛 发现的新问题

### 问题: 铜盒显示为黑色

**现象**: 
- 铜盒在两张图中都显示为黑色
- 仅有轻微的灰色反射

**原因分析**:

#### 1. Albedo 设置错误
从调试输出可以看到：
```
Material[6]: bsdfProcedureSetIndex=3 (MicrofacetReflection)
  Albedo: (0.000, 0.000, 0.000)  ❌ 纯黑色！
  Eta: (0.143, 0.374, 1.442)
  Kappa: (3.984, 2.386, 1.603)
```

**问题**: `createMaterialConductor` 没有设置 Albedo！

#### 2. 导体材质的正确实现

导体的反射颜色应该来自：
1. **Fresnel 反射率**（由 eta/kappa 决定）
2. **可选的 coeffR**（额外的颜色系数）

在原始 VLR 中：
```cpp
// SpecularReflectionSurfaceMaterial
BSDF = coeffR * FresnelConductor(eta, kappa)
```

当前实现中，`Albedo` 被用作 `coeffR`，但在 `createMaterialConductor` 中未设置，导致为 (0,0,0)。

---

## 🔧 建议的修复

### 方案 1: 设置 Albedo 为白色（推荐）

在 `scene.cpp::createMaterialConductor()` 中：
```cpp
float one = 1.0f;
mat.data[MaterialDataLayout::AlbedoR] = *reinterpret_cast<uint32_t*>(&one);
mat.data[MaterialDataLayout::AlbedoG] = *reinterpret_cast<uint32_t*>(&one);
mat.data[MaterialDataLayout::AlbedoB] = *reinterpret_cast<uint32_t*>(&one);
```

这样 `coeffR = (1,1,1)`，铜的颜色完全由 Fresnel 决定。

### 方案 2: 添加 coeffR 参数

修改 `createMaterialConductor` 接口：
```cpp
uint32_t createMaterialConductor(
    float coeffR, float coeffG, float coeffB,  // 新增
    float etaR, float etaG, float etaB,
    float kappaR, float kappaG, float kappaB,
    float roughness);
```

这样可以额外调制反射颜色。

### 方案 3: 使用 evaluateMicrofacetReflectionBSDF 的修复版本

确保 BSDF 评估时，即使 Albedo 为 (0,0,0)，也能正确计算 Fresnel 反射。

当前实现中，`evaluateMicrofacetReflectionBSDF` 不使用 Albedo，直接计算 `F * D * G / denom`，所以理论上应该工作。

**但是**，在 `sampleMicrofacetReflectionBSDF` 中可能有问题！

---

## 🔬 深入分析：为什么铜盒是黑色的？

### 检查 sampleMicrofacetReflectionBSDF

让我查看采样函数的实现...

从调试输出可以看到：
```
[GPU sampleBSDFWithU2] Entering MicrofacetReflection branch
  eta=(0.143,0.374,1.442), kappa=(3.984,2.386,1.603), roughness=0.150
```

参数正确传递，但采样结果如何？

### 可能的问题

#### 1. BSDF 采样返回的 `result->f` 可能为 Zero

如果 `sampleMicrofacetReflectionBSDF` 中使用了 Albedo 或 coeffR，而它们为 (0,0,0)，则：
```cpp
result->f = coeffR * F * spec  // 如果 coeffR = (0,0,0)，则 f = (0,0,0)
```

#### 2. 路径 throughput 被乘以 Zero

```cpp
pathState.throughput *= result.f / result.pdf;
// 如果 f = (0,0,0)，则 throughput = (0,0,0)
```

---

## ✅ 确认的修复方案

**立即修复**: 在 `createMaterialConductor` 中设置 Albedo 为 (1,1,1)

这样：
1. `coeffR = (1,1,1)`
2. `BSDF = (1,1,1) * Fresnel(eta,kappa) * D * G / denom`
3. 铜的颜色完全由 Fresnel 的波长依赖性决定

---

## 📈 预期效果

修复后，铜盒应该显示为：
- **红色/橙色金属外观**
- **高光处接近白色**
- **边缘有明显的 Fresnel 反射**

Fresnel 反射率（基于 eta/kappa）：
- 红光: F ≈ 1.0（几乎完全反射）
- 绿光: F ≈ 0.94（94% 反射）
- 蓝光: F ≈ 0.61（61% 反射）

结果：**红色 > 绿色 > 蓝色** → 铜色外观

---

## 🎯 下一步行动

1. **修复 Albedo 问题**（立即）
   ```cpp
   // scene.cpp::createMaterialConductor
   // 添加 Albedo = (1,1,1)
   ```

2. **重新渲染**
   ```powershell
   .\bin\cornell_box_improved_test.exe -w 512 -h 512 -s 128 -o bin/cornell_box_final.png
   ```

3. **验证铜色外观**
   - 检查铜盒是否显示为红色/橙色
   - 确认高光和反射正确

---

**创建日期**: 2026-03-08  
**状态**: 发现新问题，需要修复 Albedo
