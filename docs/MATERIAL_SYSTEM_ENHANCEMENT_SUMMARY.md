# 材质系统增强总结

**状态**: ✅ 完成 (2026-03-08)

## 新增 API

| API | 说明 |
|-----|------|
| `vlrCreateMaterialConductorAniso` | 各向异性金属（拉丝、刷纹） |
| `vlrCreateMaterialMultiSurface` | 2-4 层材质混合 |
| `vlrCreateMaterialLambertianScattering` | 次表面散射 |
| `vlrCreateMaterialDisney` | Disney Principled BRDF (11 参数) |

## 材质能力

- **14 种材质类型**：Lambert, GGX, Specular, Glass, MicrofacetReflection/Scattering, MultiSurface, Disney BRDF 等
- **测试**: 7/7 API 通过，4/4 多层材质通过

## 性能

| 材质 | 吞吐量 |
|------|--------|
| LambertianScattering | 91.79 Msamples/s |
| Disney BRDF | 84.95 Msamples/s |
| ConductorAniso | 49.39 Msamples/s |

## 使用示例

```cpp
// 各向异性金属
vlrCreateMaterialConductorAniso(scene, eta, kappa, 0.3f, 0.8f, &mat);

// Disney 塑料
vlrCreateMaterialDisney(scene, baseColor, 0.0f, 0.0f, 0.5f, 0.3f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, &mat);
```

## 相关文件

- `libVLR/shared/bsdf_common.h`
- `libVLR/shared/material_types.h`
- `test/material_test.cpp`, `test/disney_brdf_test.cpp` 等
