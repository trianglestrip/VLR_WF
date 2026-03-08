# 玻璃球透明度修复报告

**日期**: 2026-03-08  
**问题**: 玻璃球看起来不够透明  
**根本原因**: 球体法线方向错误（指向内部而非外部）  
**修复**: 反转三角形卷绕顺序

---

## 🔍 问题分析

### 现象
- 玻璃球看起来不透明
- 无法清晰看到背后的墙壁
- 折射效果不明显

### 根本原因

#### 1. 法线方向错误

程序化生成的球体（`createSphere` 函数）使用了错误的三角形卷绕顺序，导致：
- **几何法线指向内部**（inward normals）
- 光线从外部击中球体时，被认为是"从内部离开"
- 折射计算使用了错误的 `etaI/etaT` 比值

#### 2. SpecularTransmission 的方向判断

在 `sampleSpecularTransmissionBSDF` 中（`bsdf_common.h:1037`）:

```cpp
bool entering = (cosThetaI >= 0.0f);
if (entering) {
    etaRatio = etaI / etaT_eff;  // 1.0 / 1.5 = 0.667（进入玻璃）
} else {
    etaRatio = etaT_eff / etaI;  // 1.5 / 1.0 = 1.5（离开玻璃）
}
```

**问题**:
- 如果法线指向内部，`cosThetaI` 的符号会反转
- 进入时被误判为离开，`etaRatio = 1.5`（错误！）
- 离开时被误判为进入，`etaRatio = 0.667`（错误！）

#### 3. 折射角度错误

使用错误的 `etaRatio` 会导致：
- 折射角度计算错误
- 光线路径错误
- 玻璃球看起来不透明

---

## 🔧 修复方案

### 修复前的三角形卷绕顺序

```cpp
// 错误的卷绕顺序（法线指向内部）
if (lat != 0) {
    indices.push_back(first);
    indices.push_back(second);        // ← 顺序错误
    indices.push_back(first + 1);
}
if (lat != rings - 1) {
    indices.push_back(first + 1);
    indices.push_back(second);        // ← 顺序错误
    indices.push_back(second + 1);
}
```

### 修复后的三角形卷绕顺序

```cpp
// 正确的卷绕顺序（法线指向外部）
if (lat != 0) {
    indices.push_back(first);
    indices.push_back(first + 1);     // ← 反转
    indices.push_back(second);
}
if (lat != rings - 1) {
    indices.push_back(first + 1);
    indices.push_back(second + 1);    // ← 反转
    indices.push_back(second);
}
```

### 卷绕顺序的影响

使用**右手定则**计算几何法线：
```
Normal = (v1 - v0) × (v2 - v0)
```

- **修复前**: `(second - first) × (first+1 - first)` → 法线指向**内部**
- **修复后**: `(first+1 - first) × (second - first)` → 法线指向**外部**

---

## 📊 修复效果对比

### 修复前（cornell_box_final.png）
- ❌ 玻璃球不透明
- ❌ 无法看到背后的墙壁
- ❌ 折射效果微弱
- ❌ 球体内部看起来是实心的

### 修复后（cornell_box_sphere_fixed.png）
- ✅ 玻璃球透明
- ✅ 可以清晰看到背后的白色墙壁
- ✅ 折射效果明显（背景扭曲）
- ✅ 边缘有正确的 Fresnel 反射
- ✅ 球体内部有正确的光线传播

---

## 🎯 技术细节

### 折射率设置
- **IOR = 1.5**（标准玻璃）
- **Transmittance = (0.999, 0.999, 0.999)**（几乎无吸收）

### 正确的折射行为

#### 从外部进入（Air → Glass）
- `cosThetaI > 0`（入射方向与外向法线同向）
- `entering = true`
- `etaRatio = 1.0 / 1.5 = 0.667`
- 折射角度**减小**（光线向法线弯曲）

#### 从内部离开（Glass → Air）
- `cosThetaI < 0`（入射方向与外向法线反向）
- `entering = false`
- `etaRatio = 1.5 / 1.0 = 1.5`
- 折射角度**增大**（光线远离法线）

### Fresnel 反射

在玻璃表面（IOR=1.5）：
- **垂直入射**: F ≈ 4%（大部分透射）
- **掠射角**: F → 100%（全反射）

这解释了为什么球体边缘有明显的高光（Fresnel 反射）。

---

## 🐛 其他发现

### 铜盒仍然是黑色

虽然已经：
1. ✅ 设置 `Albedo = (1,1,1)`
2. ✅ 修改 BSDF 函数使用 `coeffR`
3. ✅ 添加 Fresnel clamp

但铜盒仍然显示为黑色。

**可能的原因**:
1. **光照不足**: 铜的 Fresnel 反射率虽高，但在暗环境中不明显
2. **粗糙度过高**: `roughness = 0.15` 可能导致反射过于分散
3. **相机角度**: 从正面看，反射的光线可能没有指向相机

**建议测试**:
- 降低粗糙度到 0.05
- 增加环境光强度
- 调整相机角度

---

## ✅ 总结

### 已修复
1. ✅ **Fresnel 值 clamp 到 [0,1]**（能量守恒）
2. ✅ **添加 coeffR 支持**（MicrofacetReflection BSDF）
3. ✅ **球体法线方向修复**（反转卷绕顺序）
4. ✅ **调试代码条件化**（使用预处理宏）

### 效果验证
- ✅ 玻璃球透明度**显著改善**
- ✅ 折射效果**正确**
- ✅ Fresnel 反射**正确**
- ⚠️ 铜盒仍然偏暗（需要进一步调整）

### 性能
- **渲染速度**: 38.36 Msamples/s
- **256 samples**: 收敛良好

---

## 📝 下一步建议

### 1. 优化铜盒外观
```cpp
// 降低粗糙度
float roughness = 0.05f;  // 从 0.15 改为 0.05

// 或者调整 eta/kappa（使用更亮的金属）
float etaGold[] = { 0.47f, 0.37f, 1.44f };  // 金
float kappaGold[] = { 2.82f, 2.15f, 1.79f };
```

### 2. 增加环境光
```cpp
// 增加天空光强度
float lightEmission[] = { 50.0f, 50.0f, 50.0f };  // 从 30 增加到 50
```

### 3. 测试不同材质参数
- 铜（Copper）: eta=(0.143, 0.374, 1.442), kappa=(3.984, 2.386, 1.603)
- 金（Gold）: eta=(0.47, 0.37, 1.44), kappa=(2.82, 2.15, 1.79)
- 银（Silver）: eta=(0.155, 0.116, 0.138), kappa=(4.82, 3.12, 2.14)

---

**创建日期**: 2026-03-08  
**状态**: 玻璃球透明度问题已修复 ✅
