# 材质系统调试快速参考

> Task 1-5 关键发现速查表

---

## ✅ 验证通过

### CPU 端（Task 1-2）
```
✓ createMaterialConductor: eta/kappa/roughness 正确写入
✓ createMaterialEx: albedo/ior/roughness 正确写入
✓ updateToGPU: 7 个材质正确上传
✓ 数据验证: 所有 "expected" 值匹配
```

### GPU 端（Task 3-5）
```
✓ getBSDFType(): 返回正确类型 (0,1,3,4)
✓ sampleBSDFWithU2: 进入正确分支
✓ MicrofacetReflection: eta/kappa 正确读取
✓ MicrofacetScattering: ior 正确读取
✓ evaluateBSDF: 返回合理值
✓ Fresnel: (1.004,0.943,0.613) 符合铜的光学特性
```

---

## 🔍 关键数值

### 铜材质（Material[6]）
```
Type: MicrofacetReflection (BSDFType=3)
Eta:   (0.143, 0.374, 1.442)
Kappa: (3.984, 2.386, 1.603)
Roughness: 0.150

Fresnel F: (1.004, 0.943, 0.613)  // 红>绿>蓝
BSDF:      (3.063, 2.876, 1.869)  // 红色金属
```

### 玻璃材质（Material[5]）
```
Type: MicrofacetScattering (BSDFType=4)
IOR: 1.500
Roughness: 0.000
Albedo: (0.999, 0.999, 0.999)
```

---

## ⚠️ 发现的问题

### Fresnel 略大于 1.0
```
F=(1.004, 1.018, 1.031, 1.043, 1.056)
```

**修复**:
```cpp
// bsdf_common.h:453
F.values[i] = vlr_min(1.0f, FresnelConductor(...));
```

---

## 🚀 运行调试

```powershell
# 快速调试（推荐）
.\debug_material_system.ps1

# 或手动运行
.\bin\cornell_box_improved_test.exe -w 128 -h 128 -s 16 -o bin/debug_material.png
```

---

## 📁 输出文件

- `bin/debug_material.png`: 渲染结果
- `docs/MATERIAL_DEBUG_GUIDE.md`: 详细指南
- `docs/MATERIAL_DEBUG_REPORT.md`: 完整报告
- `MATERIAL_DEBUG_SUMMARY.md`: 总结

---

## 🔧 移除调试代码

搜索并删除所有包含以下标记的代码：
```
[DEBUG Task 1]
[DEBUG Task 2]
[DEBUG Task 3]
[DEBUG Task 4]
[DEBUG Task 5]
```

或使用预处理宏条件化：
```cpp
#define VLR_DEBUG_MATERIAL  // 取消注释启用调试
```

---

**完成日期**: 2026-03-08
