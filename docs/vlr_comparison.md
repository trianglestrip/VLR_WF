# VLR vs VLR_WF 功能对比分析

## 当前问题总结

### 渲染问题
1. **天花板和后墙有明显的棋盘格反射** - 这是最严重的问题
2. **金属盒子颜色不对** - 应该是金色，但显示为粉红色/黑色
3. **整体过曝** - 白色区域过亮，细节丢失
4. **反射过于强烈** - Lambert 材质不应该有如此明显的镜面反射

### 参考图 vs 当前图对比
| 特征 | 参考图 (VLR) | 当前图 (VLR_WF) |
|------|-------------|----------------|
| 天花板 | 纯白色，柔和 | 棋盘格反射明显 |
| 后墙 | 白色，均匀 | 棋盘格反射明显 |
| 金属盒子 | 金色，明亮 | 粉红/黑色 |
| 玻璃球 | 清晰折射 | 清晰折射 ✓ |
| 地板 | 棋盘格清晰 | 棋盘格清晰 ✓ |
| 整体亮度 | 适中 | 过曝 |

---

## 1. 架构对比

### 1.1 渲染模式

| 项目 | VLR (参考) | VLR_WF (当前) |
|------|-----------|--------------|
| **渲染模式** | Megakernel Path Tracing | Wavefront Path Tracing |
| **内核结构** | 单个大内核处理整条路径 | 多个小内核分阶段处理 |
| **主要文件** | `path_tracing.cu` | `generate_rays.cu`, `process_hits.cu`, `sample_lights.cu`, `sample_bsdf.cu`, `accumulate.cu` |
| **优势** | 代码简单，易调试 | GPU 利用率高，适合复杂场景 |
| **劣势** | 分支发散影响性能 | 调试复杂，需要仔细同步 |

**关键差异**：
- VLR 使用递归光线追踪（通过 OptiX 调用栈）
- VLR_WF 使用迭代方式（显式路径状态管理）

---

## 2. 材质系统对比

### 2.1 Lambert BSDF

#### VLR (参考实现)
```cpp
// 位置：libVLR/shared/bsdf_common.h (推测)
SampledSpectrum evaluateLambertBSDF(
    const SampledSpectrum& albedo,
    const Vector3D& dirIn,
    const Vector3D& dirOut) {
    return albedo * VLR_M_INV_PI;
}
```

#### VLR_WF (当前实现)
```cpp
// 位置：libVLR/shared/bsdf_common.h:48-59
CUDA_DEVICE_FUNCTION CUDA_INLINE SampledSpectrum evaluateLambertBSDF(
    const SampledSpectrum& albedo,
    const Vector3D& dirInLocal,
    const Vector3D& dirOutLocal,
    const Normal3D& geomNormalLocal) {

    float cosOut = dot(dirOutLocal, geomNormalLocal);
    if (cosOut <= 0.0f)
        return SampledSpectrum::Zero();

    return albedo * VLR_M_INV_PI;
}
```

**差异**：
- ✓ 实现基本一致
- ✓ 都使用 `albedo / π` 公式
- **问题**：Lambert 材质不应该产生镜面反射，但当前图片中天花板/后墙有明显的棋盘格反射

**可能原因**：
1. 不是 BSDF 本身的问题
2. 可能是路径追踪流程中的累积计算问题
3. 可能是间接光照计算过强

---

### 2.2 GGX BSDF (金属材质)

#### VLR_WF 当前实现（刚修复）
```cpp
// 位置：libVLR/shared/bsdf_common.h:247-260
float VdotH = dot(dirOutLocal, halfVec);

float denom = 4.0f * NdotL * NdotV;
if (denom < 1e-7f)
    return SampledSpectrum::Zero();

float spec = D * G / denom;

// Use reflectance as F0 for metallic materials (Schlick Fresnel with colored F0)
SampledSpectrum result;
for (int i = 0; i < NumSpectralSamples; ++i) {
    float F0 = reflectance.values[i];
    float F = F0 + (1.0f - F0) * powf(1.0f - VdotH, 5.0f);
    result.values[i] = F * spec;
}
```

**最近修复**：
- ✓ 修复了菲涅尔项，从固定的 `F0=0.04` 改为使用 `reflectance` 作为 F0
- ✓ 使用彩色菲涅尔（Schlick approximation with colored F0）

**问题**：
- 金属盒子显示为粉红色/黑色，而不是金色
- 可能是 baseColor 传递问题，或者光照不足

---

## 3. 路径追踪流程对比

### 3.1 VLR (Megakernel)

```
pathTracing (RayGen)
  ├─> 生成相机光线
  ├─> trace() - 递归追踪
  │    ├─> ClosestHit
  │    │    ├─> 计算表面交点
  │    │    ├─> 评估 BSDF
  │    │    ├─> 显式光源采样 (NEE)
  │    │    ├─> BSDF 采样下一跳
  │    │    └─> 递归 trace()
  │    └─> Miss
  │         └─> 环境光
  └─> 累积到输出缓冲区
```

### 3.2 VLR_WF (Wavefront)

```
主循环 (CPU)
  ├─> generateRays() - 生成初始光线
  ├─> 迭代直到所有路径终止：
  │    ├─> traceRays() - OptiX 光线追踪
  │    ├─> processHits() - 处理命中点
  │    │    ├─> 计算表面交点
  │    │    ├─> 评估隐式光源采样 (MIS)
  │    │    ├─> 写入 Denoiser 缓冲区
  │    │    └─> 标记路径状态
  │    ├─> sampleLights() - 显式光源采样 (NEE)
  │    ├─> sampleBSDF() - BSDF 采样
  │    ├─> compactPaths() - 压缩活跃路径
  │    └─> sortByMaterial() - 按材质排序
  └─> accumulate() - 累积到输出缓冲区
```

**关键差异**：
- VLR 在 ClosestHit 中完成所有工作
- VLR_WF 分阶段处理，需要在多个内核间传递状态

---

## 4. 光照计算对比

### 4.1 直接光照 (NEE - Next Event Estimation)

#### VLR (参考)
```cpp
// 在 ClosestHit 中
// 1. 选择光源
// 2. 采样光源表面点
// 3. 计算光源贡献
// 4. 评估 BSDF
// 5. 计算 MIS 权重
// 6. 追踪阴影光线
```

#### VLR_WF (当前)
```cpp
// 位置：libVLR/GPU_kernels/sample_lights.cu
// 1. 从 processHits 传递的状态读取
// 2. 选择光源 (selectLight)
// 3. 采样光源表面点
// 4. 计算光源贡献 (evaluateLightEmission)
// 5. 评估 BSDF
// 6. 计算 MIS 权重
// 7. 追踪阴影光线
```

**差异**：
- ✓ 逻辑基本一致
- VLR_WF 需要显式管理路径状态

---

### 4.2 间接光照 (BSDF 采样)

#### VLR (参考)
```cpp
// 在 ClosestHit 中
// 1. 采样 BSDF 获取下一跳方向
// 2. 计算 throughput *= f * cos / pdf
// 3. 递归 trace()
```

#### VLR_WF (当前)
```cpp
// 位置：libVLR/GPU_kernels/sample_bsdf.cu
// 1. 从状态读取 BSDF 参数
// 2. 采样 BSDF 获取下一跳方向
// 3. 更新 throughput *= f * cos / pdf
// 4. 写回路径状态，等待下一次 traceRays()
```

**差异**：
- ✓ 逻辑基本一致
- VLR_WF 需要在内核间传递 throughput

---

## 5. 累积计算对比

### 5.1 VLR (参考)
```cpp
// 在 RayGen 结束时
// contribution 已经累积了整条路径的贡献
outputBuffer[pixelIndex] += contribution;
```

### 5.2 VLR_WF (当前)
```cpp
// 位置：libVLR/GPU_kernels/accumulate.cu
// 每次命中都累积
if (pathState.hasEmission) {
    SampledSpectrum contribution = pathState.throughput * pathState.emission;
    atomicAdd(&outputBuffer[pixelIndex], contribution);
}
```

**关键差异**：
- VLR 在路径结束时一次性累积
- VLR_WF 在每次命中时累积（如果有发光）

**潜在问题**：
- 多次 atomicAdd 可能导致数值误差
- 累积顺序不同可能影响结果

---

## 6. 问题根因分析

### 6.1 天花板/后墙的棋盘格反射问题

**现象**：Lambert 材质（白色天花板/后墙）显示出明显的棋盘格反射

**可能原因**：

#### 原因 1：间接光照过强
```cpp
// 当前实现中，间接光照的 throughput 可能没有正确衰减
// 检查点：libVLR/GPU_kernels/sample_bsdf.cu

// 正确的 throughput 更新应该是：
throughput *= (bsdf_f * cos_theta) / pdf;

// 如果缺少 cos_theta 或 pdf 不正确，会导致过强的反射
```

#### 原因 2：MIS 权重计算错误
```cpp
// 位置：libVLR/GPU_kernels/process_hits.cu:30-50
// 隐式光源采样的 MIS 权重可能不正确
// 导致间接光照贡献过大

float computeImplicitLightMISWeight(...) {
    // 如果这里返回的权重过大，会导致反射过强
    // 需要检查 Power Heuristic 的实现
}
```

#### 原因 3：累积次数错误
```cpp
// 如果同一个贡献被累积多次，会导致过曝
// 检查 accumulate.cu 中的累积逻辑
```

**验证方法**：
1. 临时禁用间接光照（只保留直接光照）
2. 检查 throughput 的数值范围
3. 添加 GPU printf 输出累积值

---

### 6.2 金属盒子颜色错误

**现象**：金属盒子显示为粉红色/黑色，而不是金色

**可能原因**：

#### 原因 1：baseColor 传递错误
```cpp
// 检查材质创建时的颜色值
// 测试代码：test/cornell_box_improved_test.cpp:304
float goldColor[] = { 1.0f, 0.782f, 0.344f };

// 验证这个颜色是否正确传递到 GPU
// 检查 SurfaceMaterialDescriptor 的数据布局
```

#### 原因 2：光照不足
```cpp
// 金属材质需要足够的光照才能显示颜色
// 当前光照强度：250.0f
// 曝光：8.0f

// 可能需要进一步增加光照或调整金属材质的粗糙度
```

#### 原因 3：菲涅尔项过强
```cpp
// 刚修复的彩色菲涅尔可能导致颜色偏移
// 检查 F0 的值是否正确

float F0 = reflectance.values[i];  // 应该是 (1.0, 0.782, 0.344)
float F = F0 + (1.0f - F0) * powf(1.0f - VdotH, 5.0f);
```

---

### 6.3 整体过曝问题

**现象**：白色区域过亮，细节丢失

**原因**：
```cpp
// 当前曝光设置过高
// test/cornell_box_improved_test.cpp:447
savePNG(outputFile, width, height, outputBuffer, numSamples, 8.0f);
//                                                             ^^^^
//                                                             过高
```

**建议**：
- 降低曝光到 2.0-3.0
- 或者降低光照强度到 100-150

---

## 7. 修复优先级

### 优先级 1：天花板/后墙反射问题（最严重）
1. **检查间接光照的 throughput 计算**
   - 文件：`libVLR/GPU_kernels/sample_bsdf.cu`
   - 验证 `throughput *= (f * cos) / pdf` 是否正确
   
2. **检查 MIS 权重计算**
   - 文件：`libVLR/GPU_kernels/process_hits.cu:30-50`
   - 验证 `computeImplicitLightMISWeight` 的实现
   
3. **检查累积逻辑**
   - 文件：`libVLR/GPU_kernels/accumulate.cu`
   - 确保每个贡献只累积一次

### 优先级 2：金属盒子颜色
1. **验证 baseColor 传递**
   - 添加 GPU printf 输出 `reflectance` 的值
   - 检查 `SurfaceMaterialDescriptor` 的数据布局
   
2. **调整光照/曝光**
   - 降低曝光到 2.0-3.0
   - 或调整光照强度

### 优先级 3：整体曝光
1. **降低曝光参数**
   - 从 8.0 降到 2.0-3.0
   
2. **或降低光照强度**
   - 从 250.0 降到 100-150

---

## 8. 对比参考实现的关键代码

### 8.1 需要对比的文件

| 功能 | VLR (参考) | VLR_WF (当前) |
|------|-----------|--------------|
| **路径追踪主循环** | `path_tracing.cu` | `process_hits.cu`, `sample_bsdf.cu` |
| **BSDF 实现** | `shared/bsdf_common.h` (推测) | `shared/bsdf_common.h` |
| **光源采样** | `path_tracing.cu` (内联) | `sample_lights.cu` |
| **累积** | `path_tracing.cu` (内联) | `accumulate.cu` |
| **材质数据** | `shared/material_types.h` (推测) | `shared/material_types.h` |

### 8.2 关键检查点

1. **Throughput 更新公式**
   ```cpp
   // VLR 参考（推测）
   throughput *= bsdf_f * abs(cos_theta) / pdf;
   
   // VLR_WF 当前 - 需要验证
   // 位置：sample_bsdf.cu
   ```

2. **MIS 权重计算**
   ```cpp
   // Power Heuristic
   float mis_weight = (pdf_a * pdf_a) / (pdf_a * pdf_a + pdf_b * pdf_b);
   
   // 需要验证 VLR_WF 的实现是否正确
   ```

3. **累积时机**
   ```cpp
   // VLR: 路径结束时一次性累积
   // VLR_WF: 每次命中时累积（如果有发光）
   
   // 需要确认这个差异是否导致问题
   ```

---

## 9. 调试建议

### 9.1 添加调试输出
```cpp
// 在 process_hits.cu 中添加
if (pixelIndex == 256 * 256 + 256) {  // 中心像素
    printf("Path %d: throughput = (%f, %f, %f), emission = (%f, %f, %f)\n",
           pathState.pathLength,
           pathState.throughput.values[0],
           pathState.throughput.values[1],
           pathState.throughput.values[2],
           pathState.emission.values[0],
           pathState.emission.values[1],
           pathState.emission.values[2]);
}
```

### 9.2 简化测试场景
```cpp
// 创建一个只有白色盒子和一个光源的简单场景
// 验证 Lambert 材质是否正确

// 预期：白色墙面应该均匀，没有明显的反射图案
```

### 9.3 逐步启用功能
```cpp
// 1. 只启用直接光照（禁用间接光照）
// 2. 只启用 Lambert 材质（禁用 GGX）
// 3. 逐步增加复杂度

// 通过对比找出问题所在
```

---

## 10. 修复方案

### 已完成的修复
1. ✓ **降低曝光** - 从 8.0 降到 2.0（改善过曝问题）
2. ✓ **修复 GGX 菲涅尔** - 使用彩色 F0 而不是固定的 0.04

### 待修复问题

#### 问题 1：天花板/后墙的棋盘格反射（最严重）

**根因分析**：
Lambert 材质不应该有镜面反射，但当前渲染中天花板/后墙显示出明显的棋盘格反射图案。这说明：
1. **不是 BSDF 本身的问题**（Lambert BSDF 实现是正确的）
2. **是间接光照传播的问题**

**可能原因**：
1. **路径长度过长** - 当前 `maxPathLength` 可能设置过大，导致过多的间接反弹
2. **俄罗斯轮盘赌阈值过高** - `RRThreshold` 过高导致低贡献路径仍然继续
3. **间接光照权重过大** - throughput 衰减不够

**修复方案**：
```cpp
// 方案 1：降低最大路径长度
// 位置：test/cornell_box_improved_test.cpp
// 当前：可能是 8-12
// 建议：改为 4-6（Cornell Box 场景不需要很深的路径）

// 方案 2：调整俄罗斯轮盘赌参数
// 位置：libVLR/shared/path_types.h 或 WavefrontConfig
// RRStartDepth: 3 → 2（更早开始 RR）
// RRThreshold: 当前值 → 0.5（更激进地终止低贡献路径）

// 方案 3：检查 Lambert BSDF 的反照率
// 位置：test/cornell_box_improved_test.cpp
// 白色材质：(0.522, 0.522, 0.522)
// 建议：降低到 (0.4, 0.4, 0.4) 或 (0.3, 0.3, 0.3)
// 这会减少间接光照的强度
```

#### 问题 2：金属盒子颜色错误

**根因分析**：
金属盒子应该是金色 `(1.0, 0.782, 0.344)`，但显示为粉红色/黑色。

**可能原因**：
1. **baseColor 传递错误** - 材质数据布局问题
2. **光照不足** - 金属材质需要足够的光照才能显示颜色
3. **菲涅尔计算错误** - 彩色 F0 的实现可能有问题

**修复方案**：
```cpp
// 方案 1：添加调试输出验证 baseColor
// 位置：libVLR/GPU_kernels/sample_bsdf.cu 或 process_hits.cu
if (pathIndex == 256 * 256 + 200) {  // 金属盒子附近的像素
    printf("GGX material: reflectance = (%f, %f, %f)\n",
           reflectance.values[0],
           reflectance.values[1],
           reflectance.values[2]);
}

// 方案 2：增加光照强度
// 位置：test/cornell_box_improved_test.cpp
// 当前：250.0f
// 建议：尝试 300.0f 或 400.0f

// 方案 3：降低金属粗糙度
// 位置：test/cornell_box_improved_test.cpp
// 当前：0.10f
// 建议：尝试 0.05f（更光滑，更明亮的反射）

// 方案 4：检查材质数据布局
// 验证 SurfaceMaterialDescriptor 中的 baseColor 是否正确存储
// 可能需要对比 VLR 的材质数据结构
```

#### 问题 3：整体亮度仍需微调

**修复方案**：
```cpp
// 当前配置：
// - 光照：250.0f
// - 曝光：2.0f
// - 采样：1024

// 建议配置：
// - 光照：150-200（降低以减少间接光照）
// - 曝光：2.0-2.5
// - 采样：512-1024
```

---

## 11. 具体修复步骤

### 步骤 1：降低路径长度和调整 RR 参数
```cpp
// 文件：test/cornell_box_improved_test.cpp
// 找到 maxPathLength 设置（可能在 render 调用中）
// 修改为：
maxPathLength = 5;  // 从 8-12 降到 5

// 文件：libVLR/shared/path_types.h 或相关配置
// 修改 RR 参数：
RRStartDepth = 2;    // 从 3 改为 2
RRThreshold = 0.5;   // 更激进的终止
```

### 步骤 2：降低白色材质的反照率
```cpp
// 文件：test/cornell_box_improved_test.cpp
// 当前：
float whiteColor[] = { 0.522f, 0.522f, 0.522f };

// 修改为：
float whiteColor[] = { 0.35f, 0.35f, 0.35f };  // 降低反照率
```

### 步骤 3：调整光照强度
```cpp
// 文件：test/cornell_box_improved_test.cpp
// 当前：
float lightEmission[] = { 250.0f, 250.0f, 250.0f };

// 修改为：
float lightEmission[] = { 180.0f, 180.0f, 180.0f };  // 降低光照
```

### 步骤 4：添加调试输出（验证金属材质）
```cpp
// 文件：libVLR/GPU_kernels/process_hits.cu
// 在 processHits 函数中添加：
if (pathIndex == 256 * 256 + 256 && pathState.pathLength == 1) {
    const SurfaceMaterialDescriptor& matDesc = wlp.materialDescriptorBuffer[geomInst.materialIndex];
    BSDFType type = getBSDFType(matDesc);
    if (type == BSDFType_GGX) {
        SampledSpectrum reflectance;
        float roughness;
        getGGXParams(matDesc, &reflectance, &roughness);
        printf("GGX hit: reflectance=(%f,%f,%f), roughness=%f\n",
               reflectance.values[0],
               reflectance.values[1],
               reflectance.values[2],
               roughness);
    }
}
```

### 步骤 5：测试并对比
1. 编译并运行测试
2. 对比新旧渲染结果
3. 根据结果微调参数
4. 重复直到达到参考图效果

---

## 12. 总结

### 当前状态（修复后）
- ✓ 基本架构正确（Wavefront Path Tracing）
- ✓ BSDF 实现基本正确（Lambert, GGX）
- ✓ 光源采样实现（NEE）
- ✓ 曝光已修复（从 8.0 降到 2.0）
- ✓ GGX 菲涅尔已修复（使用彩色 F0）
- ❌ 间接光照仍然过强（天花板/后墙反射问题）
- ❌ 金属材质颜色仍不对

### 下一步行动
1. **立即执行**：步骤 1-3（降低路径长度、反照率、光照）
2. **调试验证**：步骤 4（添加 GPU printf）
3. **迭代测试**：步骤 5（测试并微调）
4. **对比参考**：如果有 VLR 源码，直接对比材质数据结构

### 预期结果
修复后应该达到：
- 天花板/后墙：纯白色，柔和，无明显反射
- 金属盒子：金色，明亮，有清晰的反射
- 整体亮度：适中，细节清晰
- 玻璃球：清晰折射，无异常

---

## 附录：文件清单

### VLR_WF (当前项目)
```
libVLR/
├── GPU_kernels/
│   ├── generate_rays.cu
│   ├── trace_rays.cu
│   ├── process_hits.cu
│   ├── sample_lights.cu
│   ├── sample_bsdf.cu
│   ├── compact.cu
│   └── accumulate.cu
├── shared/
│   ├── path_types.h
│   ├── bsdf_common.h
│   ├── material_types.h
│   ├── light_common.h
│   └── geometry_common.h
└── context.cpp

test/
└── cornell_box_improved_test.cpp
```

### VLR (参考项目)
```
libVLR/
├── GPU_kernels/
│   ├── path_tracing.cu          # 主要路径追踪内核
│   ├── materials.cu
│   ├── light_tracing.cu
│   └── ...
└── shared/
    └── ... (推测结构类似)
```
