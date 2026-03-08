# 调试渲染模式使用说明

## 验证日期
2026-03-08

## 概述

调试渲染模式用于快速可视化场景的几何、材质和光照属性，便于调试和优化渲染管线。调试模式下仅处理 primary ray，单次采样即可输出，渲染速度 < 100ms (512×512)。

## API 函数

### vlrSetDebugMode
```cpp
VLR_API VLRResult vlrSetDebugMode(VLRContext context, uint32_t mode);
```
- **功能**: 设置调试渲染模式
- **参数**: `mode` 为 `VLRDebugMode` 枚举值 (0-16)
- **调用时机**: 在 `vlrRender()` 之前调用

### vlrGetDebugMode
```cpp
VLR_API VLRResult vlrGetDebugMode(VLRContext context, uint32_t* outMode);
```
- **功能**: 获取当前调试模式
- **参数**: `outMode` 输出当前模式值

### vlrSetProbePixel
```cpp
VLR_API VLRResult vlrSetProbePixel(VLRContext context, int32_t x, int32_t y);
```
- **功能**: 设置探针像素（单像素详细调试）
- **参数**: `x=-1` 表示禁用探针
- **状态**: 待完整实现

## 调试模式枚举 (VLRDebugMode)

| 值 | 模式 | 说明 |
|----|------|------|
| 0 | Normal | 正常路径追踪 |
| 1 | BaseColor | 材质反照率 |
| 2 | GeometricNormal | 几何法线 |
| 3 | ShadingNormal | 着色法线 |
| 4 | Depth | 深度可视化 |
| 5 | UV | 纹理坐标 |
| 6 | Tangent | 切线向量 |
| 7 | Bitangent | 副切线向量 |
| 8 | Roughness | 表面粗糙度 |
| 9 | Metallic | 金属度 |
| 10 | MaterialID | 材质 ID |
| 11 | InstanceID | 实例 ID |
| 12 | PrimitiveID | 图元 ID |
| 13 | DirectLighting | 直接光照（占位） |
| 14 | IndirectLighting | 间接光照（占位） |
| 15 | DenoiserAlbedo | 降噪器反照率 |
| 16 | DenoiserNormal | 降噪器法线 |

## 使用示例

```cpp
#include <vlr/vlr.h>

// 设置调试模式
vlrSetDebugMode(context, VLRDebugMode_BaseColor);

// 渲染（调试模式自动使用 1 sample，无需多次反弹）
vlrRender(context, scene, 512, 512, 1, VLRRenderer_WavefrontPathTracing);

// 获取结果
vlrGetPixels(context, pixels.data());

// 恢复正常渲染
vlrSetDebugMode(context, VLRDebugMode_Normal);
```

## 测试建议

1. **单模式测试**: 修改 `cornell_box_improved_test.cpp`，在渲染前调用 `vlrSetDebugMode(context, mode)`
2. **批量导出**: 循环 mode 0-16，保存为 `debug_mode_XX.png`
3. **验证**: 调试模式只需 1 sample，渲染应 < 100ms

## 相关文件

- `libVLR/include/vlr/public_types.h` - VLRDebugMode 枚举
- `libVLR/include/vlr/vlr.h` - 公共 API
- `libVLR/GPU_kernels/debug_rendering.cu` - GPU 实现
- `docs/DEBUG_MODE_IMPLEMENTATION.md` - 完整实现文档
