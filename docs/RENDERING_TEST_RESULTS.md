# 渲染测试结果

**测试日期**: 2026-03-08

## 测试总览

| 测试程序 | 材质 | 性能 | 状态 |
|---------|------|------|------|
| anisotropic_test | ConductorAniso | 49.39 Msamples/s | ✅ |
| disney_brdf_test | Disney BRDF | 84.95 Msamples/s | ✅ |
| lambertian_scattering_test | LambertianScattering | 91.79 Msamples/s | ✅ |
| multi_surface_test | MultiSurface | - | ✅ |
| cornell_box_improved_test | Glass + Metal | - | ✅ |

## 场景配置建议

```cpp
// Cornell Box 3×3×3
float lightEmission[] = { 30.0f, 30.0f, 30.0f };
float exposure = 0.5f;
// 相机: (0, 1.5, 6) 看向 (0, 1.5, 0)
// 不创建前墙（相机在 z=6）
```

## 已知限制

- Disney subsurface 为简化实现
- LambertianScattering 适用于薄材质（纸、皮肤），厚实体需 BSSRDF
