# 调试渲染模式

## 概述

调试渲染模式用于快速可视化场景的几何、材质和光照属性。**已完成** (2026-03-08)。

## 调试模式列表

| 模式 | 说明 |
|------|------|
| Normal | 正常路径追踪 |
| BaseColor | 材质反照率 |
| GeometricNormal / ShadingNormal | 法线可视化 |
| Depth | 深度（近白远黑） |
| UV | 纹理坐标 (R=U, G=V) |
| Tangent / Bitangent | 切线向量 |
| Roughness / Metallic | 表面参数 |
| MaterialID / InstanceID / PrimitiveID | ID 哈希颜色 |
| DirectLighting / IndirectLighting | 光照分离（占位） |
| DenoiserAlbedo / DenoiserNormal | 降噪器 AOV |

## API

```cpp
vlrSetDebugMode(context, mode);
vlrGetDebugMode(context, &outMode);
vlrSetProbePixel(context, x, y);  // 待完整实现
```

## 使用方法

```cpp
vlrSetDebugMode(context, VLRDebugMode_BaseColor);
vlrRender(context, scene, 512, 512, 1, VLRRenderer_WavefrontPathTracing);
vlrGetPixels(context, pixels.data());
vlrSetDebugMode(context, VLRDebugMode_Normal);  // 恢复正常
```

## 性能

- 单次采样，无多次反弹
- 512×512 通常 < 100ms

## 相关文件

- `libVLR/GPU_kernels/debug_rendering.cu`
- `libVLR/include/vlr/public_types.h`
- `libVLR/include/vlr/vlr.h`
