# 纹理系统实现文档

## 概述

VLR_WF 纹理系统提供完整的 2D 纹理加载、采样和材质绑定功能，支持多种图像格式和纹理参数。

## 实现状态

✅ **已完成** (2026-03-08)

## 功能特性

### 支持的图像格式

| 格式 | 加载器 | 用途 | 色彩空间 |
|------|--------|------|----------|
| **PNG** | stb_image | BaseColor, Roughness, Metallic | sRGB / Linear |
| **JPG** | stb_image | BaseColor | sRGB |
| **EXR** | tinyexr | HDR 纹理, Normal Map | Linear |
| **HDR** | stb_image | 环境贴图 | Linear |

### 纹理类型

| 类型 | 格式 | 说明 |
|------|------|------|
| **RGBA8** | 8-bit | LDR 纹理（BaseColor, Mask） |
| **RGB32F** | 32-bit float | HDR 纹理，Normal Map |
| **RGBA32F** | 32-bit float | HDR + Alpha |

### 纹理参数

| 参数 | 选项 | 说明 |
|------|------|------|
| **FilterMode** | Nearest, Linear | 纹理滤波 |
| **WrapMode** | Repeat, Clamp | UV 环绕 |
| **UV Transform** | Scale, Offset | 纹理坐标变换 |
| **Normal Scale** | 0.0 - 2.0 | 法线强度 |

## API 参考

### 纹理管理

```cpp
// 从文件创建纹理
VLRResult vlrCreateTexture2D(
    VLRContext context,
    const char* imagePath,
    VLRTexture* outTexture
);

// 从内存创建纹理
VLRResult vlrCreateTexture2DFromMemory(
    VLRContext context,
    const void* data,
    uint32_t width,
    uint32_t height,
    uint32_t format,  // 0=RGBA8, 1=RGB32F, 2=RGBA32F
    VLRTexture* outTexture
);

// 销毁纹理
VLRResult vlrDestroyTexture(VLRTexture texture);

// 设置滤波模式
VLRResult vlrSetTextureFilterMode(
    VLRTexture texture,
    uint32_t filterMode  // 0=Nearest, 1=Linear
);

// 设置环绕模式
VLRResult vlrSetTextureWrapMode(
    VLRTexture texture,
    uint32_t wrapU,  // 0=Repeat, 1=Clamp
    uint32_t wrapV
);
```

### 材质纹理绑定

```cpp
// 绑定 BaseColor 纹理
VLRResult vlrSetMaterialBaseColorTexture(
    VLRMaterial material,
    VLRTexture texture  // NULL 清除绑定
);

// 绑定 Roughness 纹理
VLRResult vlrSetMaterialRoughnessTexture(
    VLRMaterial material,
    VLRTexture texture
);

// 绑定 Metallic 纹理
VLRResult vlrSetMaterialMetallicTexture(
    VLRMaterial material,
    VLRTexture texture
);

// 绑定 Normal Map
VLRResult vlrSetMaterialNormalTexture(
    VLRMaterial material,
    VLRTexture texture,
    float normalScale  // 法线强度，默认 1.0
);

// 设置纹理坐标变换
VLRResult vlrSetMaterialTextureTransform(
    VLRMaterial material,
    float scaleU,
    float scaleV,
    float offsetU,
    float offsetV
);
```

## 使用示例

### 基础纹理使用

```cpp
VLRContext context;
vlrCreateContext(nullptr, 1, &context);

VLRScene scene;
vlrCreateScene(context, &scene);

// 加载纹理
VLRTexture albedoTex, roughnessTex, normalTex;
vlrCreateTexture2D(context, "textures/wood_albedo.png", &albedoTex);
vlrCreateTexture2D(context, "textures/wood_roughness.png", &roughnessTex);
vlrCreateTexture2D(context, "textures/wood_normal.png", &normalTex);

// 设置纹理参数
vlrSetTextureFilterMode(albedoTex, 1);  // Linear
vlrSetTextureWrapMode(albedoTex, 0, 0); // Repeat

// 创建材质
VLRMaterial material;
vlrCreateMaterialDisney(scene, 0.8f, 0.8f, 0.8f, /* ... */, &material);

// 绑定纹理
vlrSetMaterialBaseColorTexture(material, albedoTex);
vlrSetMaterialRoughnessTexture(material, roughnessTex);
vlrSetMaterialNormalTexture(material, normalTex, 1.0f);

// 纹理坐标变换（2x tiling）
vlrSetMaterialTextureTransform(material, 2.0f, 2.0f, 0.0f, 0.0f);

// 渲染...

// 清理
vlrDestroyTexture(albedoTex);
vlrDestroyTexture(roughnessTex);
vlrDestroyTexture(normalTex);
vlrDestroyScene(scene);
vlrDestroyContext(context);
```

### 程序化纹理

```cpp
// 创建棋盘格材质（无需纹理文件）
VLRMaterial checkerMat;
vlrCreateMaterialCheckerboard(
    scene,
    0.9f, 0.9f, 0.9f,  // color0 (白色)
    0.1f, 0.1f, 0.1f,  // color1 (黑色)
    8,                  // gridSize (8x8)
    &checkerMat
);
```

## 技术细节

### 纹理采样流程

1. **UV 变换**：`(u, v) → (u * scaleU + offsetU, v * scaleV + offsetV)`
2. **环绕处理**：根据 WrapMode 处理边界
3. **滤波采样**：Nearest 或 Bilinear
4. **格式转换**：RGBA8 → float, RGB32F → RGBA

### 法线贴图处理

1. **采样**：从法线贴图读取 RGB
2. **解码**：`[0,1] → [-1,1]` 切线空间法线
3. **缩放**：应用 `normalScale` 参数
4. **变换**：切线空间 → 世界空间（使用 TBN 矩阵）
5. **校正**：确保法线与几何法线同侧

### 渲染管线集成

```
ProcessHits:
  1. 采样 BaseColor 纹理 → pathTexturedParamsBuffer
  2. 采样 Roughness 纹理 → pathTexturedParamsBuffer
  3. 采样 Metallic 纹理 → pathTexturedParamsBuffer
  4. 应用法线贴图 → 更新 shadingFrame

SampleBSDF:
  1. 读取 pathTexturedParamsBuffer
  2. 使用纹理化参数计算 BRDF
```

## 性能特性

- **GPU 加速**：所有纹理数据存储在 GPU 显存
- **高效采样**：双线性插值，硬件加速
- **内存优化**：纹理共享，避免重复加载

## 测试程序

`test/texture_test.cpp` 提供完整的纹理系统测试：
- 程序化纹理（棋盘格）
- 纹理坐标变换
- 多种参数组合

运行测试：

```powershell
cd f:\project\VLR_WF
.\bin\texture_test.exe
```

## 相关文件

- `libVLR/include/vlr/vlr.h` - 公共 API
- `libVLR/shared/texture_types.h` - 纹理类型定义
- `libVLR/shared/texture_common.h` - 采样函数
- `libVLR/image_loader.h/cpp` - 图像加载
- `libVLR/scene.h/cpp` - 纹理管理
- `libVLR/GPU_kernels/process_hits.cu` - 纹理采样集成
- `test/texture_test.cpp` - 测试程序

## 版本历史

- **v1.0** (2026-03-08)
  - 初始实现：纹理加载、采样、材质绑定
  - 支持 PNG, JPG, EXR, HDR
  - 支持 BaseColor, Roughness, Metallic, Normal Map
  - UV 变换和法线缩放
