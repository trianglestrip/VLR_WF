# Cornell Box 渲染问题分析与修复方案

## 问题1：玻璃和金属材质泛白

### 根本原因分析

#### 1.1 MicrofacetReflection PDF 公式错误

**位置**: `libVLR/shared/bsdf_common.h` - `getMicrofacetReflectionBSDFPDF` (第532-559行)

**问题**: GGX VNDF 采样的 PDF 应使用 **观察方向 (V)** 的 G1 项，而非采样方向。根据 Walter 2007 和 Heitz 2017：

```
p(ωo) = D(H) * G1(V) / (4 * V·H)
```

其中 V = 入射方向 (dirInLocal)，即路径追踪中的观察方向。

**当前错误实现**:
```cpp
float G1_v = GGX_G1(NdotV, alpha2);  // 错误：使用了采样方向 NdotV
return D * G1_v * NdotV / (4.0f * VdotHSafe);
```

**正确实现**: 应使用 `G1(NdotL)`，即观察方向与法线的点积。

**影响**: PDF 偏小会导致 `f * cosθ / pdf` 被高估，尤其在掠射角附近产生过亮反射，造成泛白。

#### 1.2 MicrofacetScattering PDF 同样错误

**位置**: `getMicrofacetScatteringBSDFPDF` 反射分支 (第556-559行)

反射分量的 PDF 同样错误地使用了 `G1(NdotV)` 而非 `G1(NdotL)`。

#### 1.3 其他可能因素

- **Fresnel**: `FresnelDielectric` 和 `FresnelConductor` 实现正确
- **能量守恒**: 反射 F + 透射 (1-F) 正确
- **铝材质参数**: eta=[1.28, 0.94, 0.57], kappa=[7.3, 6.3, 5.2] 符合物理参考值

### 修复建议

1. **修正 PDF 公式**: 将 `G1(NdotV)` 改为 `G1(NdotL)` 在 MicrofacetReflection 和 MicrofacetScattering 的 PDF 计算中
2. **参数建议**:
   - 玻璃: IOR 1.5 合理；roughness 0.1 可尝试 0.05（更锐利）或 0.15（更柔和）
   - 金属: roughness 0.15 合理；若仍泛白可降至 0.08

---

## 问题2：地面棋盘格 UV 不均匀

### 根本原因分析

#### 2.1 UV 映射逻辑问题

**位置**: `libVLR/shared/material_types.h` - `getLambertAlbedoCheckerboard` (第131-180行)

**当前逻辑** (水平面 ny > 0.9):
```cpp
u = surfPt->position.x;
v = surfPt->position.z;
u = u - floor(u);  // frac，周期为 1
v = v - floor(v);
```

**问题**: 地面范围 x,z ∈ [-1.5, 1.5]（3×3），使用 `frac(position)` 会产生 **周期为 1** 的重复模式：
- 每 1 单位重复一次，3×3 地面上出现 3×3 = 9 个周期
- 配合 gridSize=5，每个周期有 5 个格子，共 15×15 格子
- 用户期望的是 **整个 3×3 地面** 上均匀的 **5×5** 格子

**正确映射**: 应将 [-1.5, 1.5] 线性映射到 [0, 1]：
```cpp
u = (position.x + 1.5f) / 3.0f;  // [0, 1]
v = (position.z + 1.5f) / 3.0f;
```

#### 2.2 缺少可配置的 extent 参数

当前实现无法区分"周期重复"与"整面均匀"两种模式。Cornell Box 地面需要后者。

### 修复建议

1. **添加 CheckerboardExtent 参数**: 当 extent > 0 时，使用 `(position + extent) / (2*extent)` 归一化到 [0,1]
2. **默认行为**: extent=0 时保持现有 frac 逻辑（向后兼容）
3. **Cornell Box 场景**: 设置 extent=1.5，使 5×5 格子均匀覆盖 3×3 地面

---

## 修复清单

| 文件 | 修改内容 |
|------|----------|
| bsdf_common.h | getMicrofacetReflectionBSDFPDF: G1(NdotV)→G1(NdotL) |
| bsdf_common.h | getMicrofacetScatteringBSDFPDF 反射分支: 同上 |
| bsdf_common.h | sampleMicrofacetReflectionBSDF: PDF 公式同步修正 |
| material_types.h | getLambertAlbedoCheckerboard: 添加 extent 支持 |
| material_types.h | MaterialDataLayout: 添加 CheckerboardExtent 槽位 |
| scene.cpp | createMaterialCheckerboard: 支持 extent 参数 |
| vlr.cpp / vlr.h | vlrCreateMaterialCheckerboard: API 扩展 |
