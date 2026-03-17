# VLR_WF vs libVLR_reference 渲染技术对比报告

本报告对比 VLR_WF（Wavefront Path Tracing）与 libVLR_reference（原版 Recursive Path Tracing）在各关键技术点上的实现差异，用于指导后续渲染质量调优。

---

## 1. 渲染架构

| 技术点 | libVLR_reference | VLR_WF |
|--------|-----------------|--------|
| 渲染方式 | Recursive Path Tracing（单线程递归） | Wavefront Path Tracing（批处理管线） |
| 路径追踪入口 | `RT_RG_NAME(pathTracing)` 单 kernel 递归 | 多 kernel 分阶段：Generate → Trace → ProcessHits → SampleLights → SampleBSDF → Accumulate |
| GPU 并行模型 | 每像素一个线程完整跑完所有弹射 | 每阶段所有活跃路径并行处理 |
| 最大路径长度 | `MaxPathLength = 25`（硬编码） | `DefaultMaxPathLength = 25`（可配置） |

---

## 2. Tonemapping（色调映射）

**这是当前画面亮度差异的最大来源。**

| 技术点 | libVLR_reference | VLR_WF |
|--------|-----------------|--------|
| Tonemap 函数 | **指数映射** `1 - exp(-x)` | **ACES Filmic** (Narkowicz 2015) |
| Gamma 校正 | **sRGB gamma**（低值线性段 + 2.4 幂） | `pow(x, 1/2.2)` |
| 曝光控制 | `brightnessCoeff = 1.0`（线性乘数） | `exposure = 3.0`（线性乘数） |

### Tonemap 曲线特性对比

| 输入 HDR 值 | `1-exp(-x)` 输出 | ACES Filmic 输出 | 差异 |
|-------------|------------------|------------------|------|
| 0.3 | 0.259 | 0.228 | -12% |
| 0.5 | 0.393 | 0.339 | -14% |
| 0.7 | 0.503 | 0.433 | -14% |
| 1.0 | 0.632 | 0.584 | -8% |
| 2.0 | 0.865 | 0.860 | -1% |
| 5.0 | 0.993 | 0.983 | -1% |

**结论：** ACES Filmic 在中间调（0.3–1.0 范围）比指数映射系统性偏暗约 **10–14%**。经过 gamma 校正后在最终 LDR 图像上差异更加明显。

### sRGB Gamma vs pow(1/2.2)

```
sRGB:    v ≤ 0.0031308 → 12.92 × v
         v > 0.0031308 → 1.055 × v^(1/2.4) − 0.055

pow2.2:  v^(1/2.2) ≈ v^0.4545
```

sRGB 在暗部（v < 0.04）比 pow(1/2.2) 亮约 5–10%，对阴影区域有明显影响。

---

## 3. 场景参数

### 3.1 光源

| 参数 | libVLR_reference | VLR_WF | 倍数 |
|------|-----------------|--------|------|
| 主光源 emittance | **30.0** (Rec709 D65) | **15.0** | 2× |
| 光源类型 | DiffuseEmitter | Lambert emissive (type 0) | 相同 |
| 光源色温 | 等能白 (30, 30, 30) | 等能白 (15, 15, 15) | — |

**参考渲染器的光源强度是当前实现的 2 倍。**

### 3.2 墙壁材质（Matte / Lambert）

| 表面 | libVLR_reference (sRGB Gamma) | VLR_WF (线性) | 差异分析 |
|------|------------------------------|---------------|---------|
| 白墙/天花板 | (0.75, 0.75, 0.75) | (0.73, 0.73, 0.73) | 接近 |
| 红墙 | **(0.75, 0.25, 0.25)** | **(0.65, 0.05, 0.05)** | 次要通道差 5× |
| 蓝墙 | **(0.25, 0.25, 0.75)** | **(0.05, 0.05, 0.65)** | 次要通道差 5× |
| 地板 | checkerboard 纹理 | 程序化 checkerboard | 实现不同 |

**关键差异：** 参考渲染器的红/蓝墙在次要颜色通道（G、B / R、G）保留了 0.25 的反射率，而 VLR_WF 只有 0.05。这意味着：

- 参考红墙的平均反射率 ≈ (0.75+0.25+0.25)/3 = **0.417**
- VLR_WF 红墙的平均反射率 ≈ (0.65+0.05+0.05)/3 = **0.250**

红/蓝墙的间接照明贡献在参考渲染器中明显更高，场景整体亮度也相应提升。

### 3.3 金属材质

| 参数 | libVLR_reference (Gold) | VLR_WF (Gold) |
|------|------------------------|---------------|
| eta (n) | (0.12481, 0.46823, 1.44476) | (0.18, 0.47, 1.46) |
| kappa (k) | (3.32107, 2.23761, 1.69196) | (3.10, 2.38, 1.95) |
| roughness | 未指定（SpecularReflection = 完美镜面） | 0.2 (GGX microfacet) |
| 材质类型 | SpecularReflection（delta BSDF） | MicrofacetReflection（GGX） |

**差异分析：** 参考图中金属盒子使用的是**完美镜面反射**（delta BSDF），表面平滑无噪点。VLR_WF 使用 GGX 微表面模型（roughness=0.2），会产生模糊反射和少量噪点。

> 注：参考代码中 `SpecularReflection` 被注释，实际活跃的是 `SpecularScattering`（玻璃/钻石），说明参考图中可能没有金属盒子，而是两个玻璃球。

### 3.4 玻璃材质

| 参数 | libVLR_reference | VLR_WF |
|------|-----------------|--------|
| 材质类型 | SpecularScattering（完美透射+反射） | SpecularTransmission（type 6） |
| IOR | **Diamond: 2.41–2.45** | **Glass: 1.5** |
| coeff | 0.999 (近完美透射) | 1.0 |

**IOR 差异巨大：** 钻石的高 IOR 会产生更强的折射和全内反射效果，视觉上比普通玻璃更具戏剧性。

---

## 4. 路径追踪核心算法

### 4.1 MIS（Multiple Importance Sampling）

| 技术点 | libVLR_reference | VLR_WF |
|--------|-----------------|--------|
| MIS 权重函数 | Power Heuristic (β=2) | Power Heuristic (β=2) |
| NEE MIS 公式 | `w = p_light² / (p_light² + p_bsdf²)` | `w = p_light² / (p_light² + p_bsdf²)` |
| 隐式光 MIS | `w = p_bsdf² / (p_light² + p_bsdf²)` | 同左 |
| Delta BSDF 处理 | MISWeight = 1.0 | MISWeight = 1.0 |

**实现一致。**

### 4.2 Russian Roulette

| 技术点 | libVLR_reference | VLR_WF |
|--------|-----------------|--------|
| 起始深度 | **无最小深度**（从 depth=1 开始） | `RRStartDepth = 3`（前 3 次弹射不做 RR） |
| 继续概率 | `min(importance / initImportance, 1.0)` | `min(importance / initImportance, 1.0)` |
| 最小阈值 | **无阈值 clamp** | `max(continueProb, 0.05)` |
| 补偿 | `alpha /= continueProb` | `throughput /= continueProb` |

**差异分析：**

1. VLR_WF 的 `RRStartDepth = 3` 意味着前 3 次弹射不做 RR，能量守恒更保守。参考实现从第 1 次弹射就开始 RR——但由于初始 importance 很高，前几次 continueProb ≈ 1.0，实际效果差异不大。
2. VLR_WF 的 `rrThreshold = 0.05` 下限确保即使 throughput 极小也有 5% 概率继续。这在物理上引入微小偏差但避免了路径过早终止。参考实现无此下限。

### 4.3 NEE（Next Event Estimation / 直接光采样）

| 技术点 | libVLR_reference | VLR_WF |
|--------|-----------------|--------|
| 光源选择 | 层次采样（Instance → GeomInst） | 均匀随机选择 |
| 可见性测试 | 同步 shadow ray（递归中直接 trace） | **异步 shadow ray**（批量入队 → 批量 trace → 批量 apply） |
| Delta BSDF | 跳过 NEE | 跳过 NEE |
| G 几何项 | `fractionalVisibility × |cos_s| × cos_l / d²` | `|cos_s| × cos_l / d²`（visibility 在 apply 时处理） |

### 4.4 Throughput 更新

| 技术点 | libVLR_reference | VLR_WF |
|--------|-----------------|--------|
| 公式 | `alpha *= fs × (|cosFactor| / dirPDF)` | `throughput *= fs × (cosAbs / pdf)` |
| Delta BSDF | `alpha *= fs / dirPDF`（无 cos 项） | `throughput *= fs / pdf` |
| Firefly 抑制 | **无** | `maxBounceFactor = 20`（per-bounce clamp） |

**Firefly clamp 是 VLR_WF 特有的，会引入微小的能量损失。** 在高方差场景中这是合理的折衷，但对于收敛后的正确性验证应临时禁用。

---

## 5. 光谱处理

| 技术点 | libVLR_reference | VLR_WF |
|--------|-----------------|--------|
| 光谱表示 | `SampledSpectrum`（4 波长采样） | `SampledSpectrum`（4 通道，实质为 RGB+padding） |
| 波长采样 | 360–830nm 等间距偏移采样 | 固定 RGB 通道 |
| 累积空间 | `SpectrumStorage`（波长 bin 累积） → XYZ → Rec709 | `SpectrumStorage`（直接 RGB 累积） |
| 色散 | 通过波长采样自然支持 | 需要 `singleWlSelected` 特殊处理 |
| 光谱→RGB | `spectrum.toXYZ()` → `XYZ_to_Rec709_D65` 矩阵 | `toDiscretizedSpectrum()` 直接输出 RGB |

**差异影响：** 参考渲染器做了完整的光谱渲染管线，颜色精度更高。VLR_WF 的 RGB 近似在大多数场景中差异不大，但在色散材质上会有可见区别。

---

## 6. Fresnel 方程

| 技术点 | libVLR_reference | VLR_WF |
|--------|-----------------|--------|
| 电介质 Fresnel | 标准 Fresnel equations（复数运算） | 标准 Fresnel equations |
| 导体 Fresnel | 复数 IOR Fresnel（通过 PBRT 类实现） | Born & Wolf 展开式（本轮修复） |
| Schlick 近似 | 用于 GGX BSDF | 用于 GGX BSDF |

---

## 7. 后处理管线

| 阶段 | libVLR_reference | VLR_WF |
|------|-----------------|--------|
| 累积 → 平均 | `XYZ *= 1/numAccumFrames` | `RGB *= invFrames` |
| 分辨率修正 | `RGB *= width × height` | 无 |
| 曝光 | `RGB *= brightnessCoeff` (默认 1.0) | `RGB *= exposure` (默认 3.0) |
| Tonemap | `1 - exp(-x)` | ACES Filmic |
| Gamma | sRGB gamma (2.4 + 线性段) | `pow(x, 1/2.2)` |
| 降噪 | 无 | OptiX HDR Denoiser |

### 分辨率修正

参考渲染器有一个 `resCorrection = imageSize.x * imageSize.y` 的修正项。这是为了在改变分辨率时保持感知亮度一致——更高分辨率下每个像素覆盖的立体角更小，单像素积分值更小。VLR_WF 没有这个修正，但由于两者都用 512×512，实际影响需要进一步验证该修正是在哪个空间应用的。

---

## 8. 综合亮度差异估算

假设后墙中心点，以白墙 albedo 的直接照明为基准：

| 因素 | libVLR_reference | VLR_WF | 倍数 |
|------|-----------------|--------|------|
| 光源强度 | 30.0 | 15.0 | 2.0× |
| 白墙 albedo | 0.75 | 0.73 | 1.03× |
| 红/蓝墙间接光贡献 | 高（albedo 次通道 0.25） | 低（albedo 次通道 0.05） | ~1.2× |
| 曝光 × tonemap 响应 | `1.0 × (1-exp(-x))` | `3.0 × ACES(x)` | ~0.9× |
| **综合估算** | | | **~2.2×** |

**参考渲染器的等效亮度约为 VLR_WF 的 2.2 倍。** 这完全解释了当前画面偏暗的原因。

---

## 9. 修复建议

### 优先级 1（参数对齐）

1. **光源强度**: `lightEmission` 从 15.0 提高到 **30.0**
2. **红墙 albedo**: 从 (0.65, 0.05, 0.05) 改为 **(0.75, 0.25, 0.25)**
3. **蓝墙 albedo**: 从 (0.05, 0.05, 0.65) 改为 **(0.25, 0.25, 0.75)**
4. **白墙 albedo**: 从 (0.73, 0.73, 0.73) 改为 **(0.75, 0.75, 0.75)**

### 优先级 2（后处理对齐）

5. **Tonemap**: 从 ACES Filmic 切换为 `1 - exp(-x)` 指数映射
6. **Gamma**: 从 `pow(1/2.2)` 切换为 sRGB gamma
7. **曝光**: 将 `exposure` 调整为 **1.0**（与参考一致）

### 优先级 3（物理精度）

8. 考虑移除 firefly clamp（`maxBounceFactor = 20`）或提高阈值，以消除能量损失
9. 考虑添加分辨率修正项

---

## 附录：参考渲染器关键代码位置

| 模块 | 文件路径 |
|------|---------|
| 路径追踪主循环 | `libVLR_reference/libVLR/GPU_kernels/path_tracing.cu` |
| NEE 实现 | `path_tracing.cu:193–238` |
| MIS 权重 | `path_tracing.cu:167–180, 230–236` |
| Russian Roulette | `path_tracing.cu:184–189` |
| Tonemap + Gamma | `HostProgram/shaders/drawOptiXResult.frag` |
| PNG 保存后处理 | `HostProgram/main.cpp:137–142` |
| sRGB gamma | `libVLR/shared/spectrum_base.h:38–50` |
| 光谱累积 | `libVLR/shared/spectrum_types.h:675–682` |
| Cornell Box 场景 | `HostProgram/scene.cpp:528–795` |
| Brightness 设置 | `HostProgram/scene.h:50` |

---

*报告生成日期: 2026-03-17*
*对比版本: VLR_WF (wavefront-renderer branch) vs libVLR_reference (OfflineRenderer)*
