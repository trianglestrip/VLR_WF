# 环境光照（IBL）实现指南

## 验证日期
2026-03-08

## 概述

Image-Based Lighting (IBL) 使用HDR环境贴图作为光源，提供真实的全局照明效果。

## 架构设计

### 1. 图像加载模块

**文件**: `libVLR/image_loader.h` + `libVLR/image_loader.cpp`

**支持格式**:
- `.exr` - 使用tinyexr库加载
- `.hdr` - 使用stb_image库加载

**核心结构**:
```cpp
struct HDRImage {
    float* data;           // RGB浮点数据（线性空间）
    int width;
    int height;
    int channels;          // 通常为3（RGB）
};
```

**API**:
- `HDRImage loadEXR(const char* path)` - 加载EXR文件
- `HDRImage loadHDR(const char* path)` - 加载HDR文件
- `HDRImage loadHDRImage(const char* path)` - 自动识别格式

### 2. 环境光参数

**文件**: `libVLR/scene.h`

**扩展的EnvironmentLightParams**:
```cpp
struct EnvironmentLightParams {
    float color[3];              // 常量颜色（无纹理时使用）
    float intensity;             // 强度倍增器
    
    // IBL 纹理支持
    float* textureData;          // HDR纹理数据（RGB float数组）
    int textureWidth;            // 纹理宽度
    int textureHeight;           // 纹理高度
    float rotation;              // 环境旋转角度（弧度）
};
```

### 3. VLR C API

**文件**: `libVLR/include/vlr/vlr.h` + `libVLR/vlr.cpp`

**新增API**:
```cpp
// 设置常量颜色环境光
VLRResult vlrSetEnvironmentLight(VLRScene scene, const float color[3]);

// 从图像文件加载环境光
VLRResult vlrSetEnvironmentLightFromImage(
    VLRScene scene, 
    const char* imagePath, 
    float rotation
);
```

### 4. GPU端实现

**文件**: `libVLR/GPU_kernels/sample_lights.cu`

**核心功能**:
- 球面坐标映射（世界空间方向 → UV坐标）
- 环境贴图采样（双线性插值）
- 重要性采样（基于亮度构建CDF）
- 环境旋转支持（旋转矩阵）

## 使用示例

### 基础用法

```cpp
// 1. 加载环境贴图
VLRResult res = vlrSetEnvironmentLightFromImage(
    scene, 
    "resources/environments/WhiteOne.exr", 
    0.0f  // 旋转角度
);

// 2. 错误处理 - 回退到常量颜色
if (res != VLRResult_Success) {
    float envColor[] = { 0.05f, 0.05f, 0.05f };
    vlrSetEnvironmentLight(scene, envColor);
}
```

### 环境旋转

```cpp
// 旋转90度
float rotation = M_PI / 2.0f;
vlrSetEnvironmentLightFromImage(scene, "env.exr", rotation);
```

## 技术细节

### 球面坐标映射

世界空间方向向量 → UV坐标：
```
u = atan2(dir.x, -dir.z) / (2π) + 0.5
v = acos(dir.y) / π
```

### 重要性采样

1. 计算每个像素的亮度（luminance）
2. 构建2D累积分布函数（CDF）
3. 采样时根据CDF选择方向
4. 计算对应的PDF

### 环境旋转

使用Y轴旋转矩阵：
```
[cos(θ)  0  sin(θ)]
[  0     1    0   ]
[-sin(θ) 0  cos(θ)]
```

## 资源文件管理

### 测试资源位置
- **开发时**: `test/resources/environments/`
- **运行时**: `bin/resources/environments/`

### 资源复制
运行测试前，确保资源已复制到`bin/`：
```powershell
Copy-Item -Recurse test/resources bin/ -Force
```

## 已知问题和解决方案

### 问题1: tinyexr编译错误（miniz相关）

**症状**: 
- LNK2019: 无法解析 `mz_compress`, `mz_uncompress`
- C2086: miniz内部符号重定义

**解决方案**:
使用STB的zlib实现代替miniz：
```cpp
#define TINYEXR_USE_MINIZ 0
#define TINYEXR_USE_STB_ZLIB 1
```

### 问题2: stb_image链接错误

**症状**:
- LNK2019: 无法解析 `stbi_loadf`, `stbi_zlib_compress`

**解决方案**:
在`tinyexr_impl.cpp`中统一定义：
```cpp
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
```

### 问题3: 运行时找不到环境贴图

**症状**:
```
[Warning] Failed to load environment map
```

**解决方案**:
1. 检查资源路径是否正确
2. 确保`bin/resources/`目录存在
3. 使用相对路径：`resources/environments/WhiteOne.exr`

## 性能考虑

### 内存占用
- 4K环境贴图 (4096x2048 RGB float): ~96MB
- 2K环境贴图 (2048x1024 RGB float): ~24MB
- 1K环境贴图 (1024x512 RGB float): ~6MB

### 优化建议
1. 使用合适分辨率的环境贴图（1K-2K通常足够）
2. 实现重要性采样减少噪点
3. 考虑MIP-mapping减少采样开销

## 下一步扩展

### 计划中的功能
1. **多重重要性采样（MIS）**: 结合BSDF和光源采样
2. **环境光遮蔽**: 预计算可见性
3. **HDR色调映射**: 自动曝光调整
4. **环境光缓存**: 避免重复加载相同贴图

## 参考实现

参考项目: `F:\project\OfflineRenderer\VLR\HostProgram\scene.cpp`
- 使用`loadImage2D`加载环境贴图
- 使用`createShaderNode("EnvironmentTexture")`创建纹理节点
- 使用`createSurfaceMaterial("EnvironmentEmitter")`创建环境材质
- 使用`setEnvironment(matEnv, rotation)`设置环境光
