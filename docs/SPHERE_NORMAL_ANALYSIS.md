# 球体法线方向分析

## 问题描述

玻璃球看起来不够透明，可能是因为法线方向错误。

## createSphere 函数分析

### 顶点生成（第 109-127 行）

```cpp
for (int lat = 0; lat <= rings; ++lat) {
    float phi = PI * float(lat) / float(rings);  // 0 -> PI（从北极到南极）
    float sinPhi = std::sin(phi);
    float cosPhi = std::cos(phi);
    
    for (int lon = 0; lon <= segments; ++lon) {
        float theta = TWO_PI * float(lon) / float(segments);  // 0 -> 2PI（绕 Y 轴）
        float sinTheta = std::sin(theta);
        float cosTheta = std::cos(theta);
        
        float x = cx + radius * sinPhi * cosTheta;
        float y = cy + radius * cosPhi;
        float z = cz + radius * sinPhi * sinTheta;
        
        vertices.push_back(x);
        vertices.push_back(y);
        vertices.push_back(z);
    }
}
```

**顶点顺序**:
- `lat=0`: 北极（y = cy + radius）
- `lat=rings`: 南极（y = cy - radius）
- `lon=0 -> segments`: 绕 Y 轴，从 +X 轴开始逆时针

### 三角形生成（第 129-145 行）

```cpp
for (int lat = 0; lat < rings; ++lat) {
    for (int lon = 0; lon < segments; ++lon) {
        uint32_t first = lat * (segments + 1) + lon;
        uint32_t second = first + segments + 1;
        
        if (lat != 0) {
            indices.push_back(first);
            indices.push_back(second);
            indices.push_back(first + 1);
        }
        if (lat != rings - 1) {
            indices.push_back(first + 1);
            indices.push_back(second);
            indices.push_back(second + 1);
        }
    }
}
```

### 三角形卷绕顺序分析

以第一个四边形为例（lat=1, lon=0）:

```
顶点布局:
  first (lat=1, lon=0)         first+1 (lat=1, lon=1)
       *-------------------------*
       |                         |
       |                         |
       |                         |
       *-------------------------*
  second (lat=2, lon=0)      second+1 (lat=2, lon=1)
```

**上三角形**: `(first, second, first+1)`
- 从上方看（沿 -Y 方向）：逆时针
- **法线方向**：使用右手定则，法线指向**外部**（+Y 分量）

**下三角形**: `(first+1, second, second+1)`
- 从上方看：逆时针
- **法线方向**：指向**外部**

## 结论

✅ **`createSphere` 生成的法线指向外部**（outward normals）

这对于玻璃球是**正确的**！

## 那么问题在哪里？

### 可能的原因

#### 1. SpecularTransmission 是 Delta BSDF

`SpecularTransmission` 只在**完美反射/折射方向**上有贡献：
- `isDelta = true`
- 只能通过路径追踪看到
- NEE（直接光照采样）无法评估 delta BSDF

**问题**: 如果光源采样时尝试评估 delta BSDF，会返回 Zero！

#### 2. 应该使用 MicrofacetScattering（GGX 玻璃）

对于**真实的玻璃**，应该使用 `MicrofacetScattering`（roughness > 0）：
- 支持粗糙表面的折射
- 可以被 NEE 评估
- 更真实的外观

**对比**:
- `SpecularTransmission`: 完美镜面玻璃（roughness = 0）
- `MicrofacetScattering`: 真实玻璃（roughness > 0）

#### 3. 检查 sphere.obj

让我检查 `bin/resources/sphere/sphere.obj` 的法线方向。

## 建议的修复

### 方案 1: 使用 MicrofacetScattering（推荐）

```cpp
// 替换为 MicrofacetScattering
VLRMaterial matGlass = nullptr;
res = vlrCreateMaterialMicrofacetScattering(scene, 1.5f, 0.05f, &matGlass);
```

这样玻璃球会有：
- 轻微的粗糙度（roughness=0.05）
- 更真实的外观
- 可以被 NEE 正确评估

### 方案 2: 检查法线方向

如果使用 `sphere.obj`，需要确认：
- 法线是否指向外部
- 是否有法线数据（如果没有，VLR 会自动计算）

### 方案 3: 增加采样数

`SpecularTransmission` 需要更多采样才能收敛：
- 当前：128 samples
- 建议：512-1024 samples

## 下一步

1. **检查 sphere.obj 的法线方向**
2. **尝试使用 MicrofacetScattering**
3. **增加采样数测试**
