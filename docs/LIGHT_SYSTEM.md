# 光源系统实现文档

## 概述

VLR_WF 光源系统提供完整的光源类型支持和高效的重要性采样，包括区域光、点光源、方向光和环境光。

## 实现状态

✅ **已完成** (2026-03-08)

## 支持的光源类型

| 光源类型 | 特性 | 用途 |
|---------|------|------|
| **Area Light** | 基于几何表面，有面积 | 室内照明、柔和阴影 |
| **Point Light** | Delta 光源，单点发射 | 灯泡、火焰 |
| **Directional Light** | 平行光，无穷远 | 太阳光、月光 |
| **Environment Light** | IBL，球面贴图 | 室外照明、反射 |

## API 参考

### 区域光

```cpp
// 将实例标记为区域光
VLRResult vlrAddAreaLight(
    VLRScene scene,
    VLRInstance instance
);
```

使用：发光材质 + 几何体 + `vlrAddAreaLight`

### 点光源

```cpp
// 添加点光源
VLRResult vlrAddPointLight(
    VLRScene scene,
    float posX, float posY, float posZ,
    float intensityR, float intensityG, float intensityB
);
```

特性：
- 全向发射
- 距离平方衰减
- Delta 光源（无面积）

### 方向光

```cpp
// 添加方向光
VLRResult vlrAddDirectionalLight(
    VLRScene scene,
    float dirX, float dirY, float dirZ,
    float radianceR, float radianceG, float radianceB
);
```

特性：
- 平行光线
- 无距离衰减
- Delta 光源
- 模拟太阳光

### 环境光

```cpp
// 设置常量环境光
VLRResult vlrSetEnvironmentLight(
    VLRScene scene,
    const float color[3]
);

// 从 HDR 贴图设置环境光
VLRResult vlrSetEnvironmentLightFromImage(
    VLRScene scene,
    const char* imagePath,
    float rotation  // 旋转角度（弧度）
);
```

特性：
- 无穷远球面
- 支持 HDR 贴图（EXR, HDR）
- 自动重要性采样
- 支持旋转

## 使用示例

### 基础场景设置

```cpp
VLRContext context;
vlrCreateContext(nullptr, 1, &context);

VLRScene scene;
vlrCreateScene(context, &scene);

// 创建发光材质
VLRMaterial lightMat;
vlrCreateMaterial(scene, 1.0f, 1.0f, 1.0f, 80.0f, 80.0f, 80.0f, &lightMat);

// 创建光源几何（区域光）
VLRInstance lightInst;
vlrCreateInstance(scene, lightMesh, lightMat, &lightInst);
vlrAddAreaLight(scene, lightInst);

// 添加点光源
vlrAddPointLight(scene, 0.0f, 2.5f, 0.0f, 50.0f, 50.0f, 50.0f);

// 添加方向光（模拟太阳）
vlrAddDirectionalLight(scene, 0.3f, -0.9f, 0.2f, 8.0f, 8.0f, 8.0f);

// 设置环境光
vlrSetEnvironmentLightFromImage(scene, "env.exr", 0.0f);

// 渲染...
```

### 多光源场景

```cpp
// 创建多个区域光
for (int i = 0; i < 4; i++) {
    VLRInstance light;
    vlrCreateInstance(scene, lightMesh, lightMat, &light);
    // 设置位置...
    vlrAddAreaLight(scene, light);
}

// 添加多个点光源
vlrAddPointLight(scene, -1.0f, 2.0f, 0.0f, 30.0f, 10.0f, 10.0f);
vlrAddPointLight(scene, 1.0f, 2.0f, 0.0f, 10.0f, 30.0f, 10.0f);

// 环境光作为全局照明
vlrSetEnvironmentLight(scene, new float[3]{0.5f, 0.6f, 0.7f});
```

## 技术特性

### 重要性采样

#### 环境光重要性采样

- **Importance Map**：基于 HDR 贴图亮度构建 2D CDF
- **分层采样**：先采样 theta（天顶角），再采样 phi（方位角）
- **立体角加权**：`weight = luminance × sin(theta)`
- **性能提升**：减少噪点 2-5x（相同采样数）

#### 多光源重要性采样

- **基于功率的权重**：
  - 区域光：`power = luminance(Le) × area`
  - 点光源：`power = luminance(intensity)`
  - 方向光：`power = luminance(radiance)`
- **CDF 采样**：二分查找选择光源
- **动态更新**：支持运行时重新计算权重

### MIS (Multiple Importance Sampling)

- **BSDF 采样 + NEE**：Power Heuristic 组合
- **权重计算**：`w = (pdf_bsdf)² / ((pdf_bsdf)² + (pdf_light)²)`
- **适用场景**：所有非 Delta 材质

### 性能优化

- **Light Cache**：Shared Memory 缓存常用光源
- **Warp 优化**：早期退出非活跃路径
- **Delta 材质跳过**：完美镜面不执行 NEE

## 光源采样流程

```
SampleLights Kernel:
  1. selectLight(uLight) → 选择光源（基于重要性）
  2. sampleLight(u0, u1, u2) → 采样光源位置
  3. evaluateLightEmission() → 计算辐射度
  4. evaluateBSDF() → 计算 BSDF 响应
  5. computeGeometryTerm() → 计算几何项
  6. computeMISWeight() → 计算 MIS 权重
  7. 累加贡献到 throughput
```

## 测试程序

`test/light_system_test.cpp` 提供完整的光源系统测试：

```powershell
cd f:\project\VLR_WF
.\bin\light_system_test.exe
```

输出 5 张测试图像到 `bin/` 目录。

## 相关文件

- `libVLR/include/vlr/vlr.h` - 公共 API
- `libVLR/shared/light_types.h` - 光源类型定义
- `libVLR/shared/light_common.h` - 光源采样函数
- `libVLR/shared/env_importance.h` - 环境光重要性采样
- `libVLR/env_importance.cpp` - Importance Map 生成
- `libVLR/GPU_kernels/sample_lights.cu` - NEE Kernel
- `test/light_system_test.cpp` - 测试程序

## 版本历史

- **v1.0** (2026-03-08)
  - 完整实现：4 种光源类型
  - 环境光重要性采样
  - 多光源重要性采样
  - MIS 集成
