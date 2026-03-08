# 场景设置调试指南

## 问题概述

`disney_brdf_test.cpp` 和 `lambertian_scattering_test.cpp` 渲染结果全黑，即使替换为已知工作的 Lambert 材质也无法渲染出任何内容。

## 已完成的修复

### 1. Disney BRDF 参数槽位重叠问题

**问题**: Disney BRDF 参数槽位 32-40 与 MultiSurface 槽位 16-40 重叠

**修复**: 将 Disney BRDF 槽位移动到 41-49

**影响文件**:
- `libVLR/vlr.cpp` (第 450-458 行)
- `libVLR/materials.cuh` (第 83-91 行)

### 2. Disney BRDF 函数签名参数错位

**问题**: `sampleDisneyBRDF` 和 `getDisneyBRDFPDF` 函数缺少 `anisotropic` 参数

**修复**: 在函数签名中添加 `anisotropic` 参数

**影响文件**:
- `libVLR/bsdf.cuh` (第 1007 行和第 1160 行)

### 3. 光源强度和曝光提升

**修改**:
- 光源强度: 15/40 → 80
- 曝光值: 0.6/1.0 → 1.2/1.5

**影响文件**:
- `test/disney_brdf_test.cpp` (第 140 行和第 301 行)
- `test/lambertian_scattering_test.cpp` (第 163 行和第 140 行)

## 问题诊断

### 已排除的原因

1. ✅ **材质实现错误**: 替换为 Lambert 材质后仍全黑
2. ✅ **参数槽位冲突**: 已修复 Disney BRDF 槽位重叠
3. ✅ **函数签名错误**: 已修复 Disney BRDF 函数参数
4. ✅ **光源强度过低**: 已提升到 80
5. ✅ **曝光值过低**: 已提升到 1.2/1.5

### 根本原因

**场景设置有问题**，可能的原因包括：

1. **相机位置错误**: 相机可能在几何体内部或看向错误的方向
2. **光源位置错误**: 光源可能在场景外或被遮挡
3. **几何体位置错误**: 几何体可能不在相机视野内
4. **几何体法线错误**: 几何体可能是反向的（背面朝向相机）
5. **场景尺度问题**: 场景可能太大或太小

## 调试步骤

### 1. 对比工作正常的测试程序

参考以下工作正常的测试程序：

#### `cornell_box_improved_test.cpp` (推荐)

**场景设置**:
```cpp
// Cornell Box 尺寸: 3x3x3
// 地板: y=0
// 天花板: y=3
// 墙壁: x=±1.5, z=±1.5

// 光源位置: y=2.9 (接近天花板)
float lightVerts[] = {
    -0.5f, 2.9f, 0.5f,  -0.5f, 2.9f, -0.5f,
    0.5f, 2.9f, -0.5f,  0.5f, 2.9f, 0.5f
};

// 相机位置: (0, 1.5, 5) 看向 (0, 1, 0)
camera.position[0] = 0.0f;
camera.position[1] = 1.5f;
camera.position[2] = 5.0f;
camera.direction[0] = 0.0f;
camera.direction[1] = -0.196f;
camera.direction[2] = -0.981f;
```

#### `anisotropic_test.cpp`

**场景设置**:
```cpp
// 简化场景: 地板 + 背景墙 + 球体

// 地板: y=0, 4x4
float floorVerts[] = {
    -2, 0, -2,  2, 0, -2,  2, 0, 2,  -2, 0, 2
};

// 背景墙: z=2, 2x2
float backVerts[] = {
    -2, 0, 2,  2, 0, 2,  2, 2, 2,  -2, 2, 2
};

// 光源: y=1.95, 1x1
float lightVerts[] = {
    -0.5f, 1.95f, 0.5f,  0.5f, 1.95f, 0.5f,
    0.5f, 1.95f, -0.5f,  -0.5f, 1.95f, -0.5f
};

// 相机位置: (0, 1, 3.5) 看向 (0, 1, 0)
camera.position[0] = 0.0f;
camera.position[1] = 1.0f;
camera.position[2] = 3.5f;
camera.direction[0] = 0.0f;
camera.direction[1] = -0.2f;
camera.direction[2] = -1.0f;
```

### 2. 检查 `disney_brdf_test.cpp` 的场景设置

**当前设置**:
```cpp
// 地板: y=0, 4x4
float floorVerts[] = { -2, 0, -2,  2, 0, -2,  2, 0, 2,  -2, 0, 2 };

// 背景墙: z=2, 2x2
float backVerts[] = { -2, 0, 2,  2, 0, 2,  2, 2, 2,  -2, 2, 2 };

// 光源: y=1.95, 1x1
float lightVerts[] = {
    -0.5f, 1.95f, 0.5f,  0.5f, 1.95f, 0.5f,
    0.5f, 1.95f, -0.5f,  -0.5f, 1.95f, -0.5f
};

// 球体: y=0.25, radius=0.25, 5个球体排成一行
createSphere(sphereVerts, sphereInds, -1.2f, y, z, radius);  // x=-1.2
createSphere(sphereVerts, sphereInds, -0.6f, y, z, radius);  // x=-0.6
createSphere(sphereVerts, sphereInds, 0.0f, y, z, radius);   // x=0.0
createSphere(sphereVerts, sphereInds, 0.6f, y, z, radius);   // x=0.6
createSphere(sphereVerts, sphereInds, 1.2f, y, z, radius);   // x=1.2

// 相机位置: (0, 1, 3.5) 看向 (0, 1, 0)
camera.position[0] = 0.0f;
camera.position[1] = 1.0f;
camera.position[2] = 3.5f;
camera.direction[0] = 0.0f;
camera.direction[1] = -0.2f;
camera.direction[2] = -1.0f;
```

**可能的问题**:

1. **光源位置**: y=1.95 可能太低，球体 y=0.25 + radius=0.25 = 0.5，光源应该更高
2. **相机方向**: 相机看向 (0, 1, 0)，但球体在 y=0.25，可能看不到
3. **背景墙位置**: z=2，但相机在 z=3.5，背景墙在相机后面
4. **球体 z 坐标**: z=0，可能在相机视野外

### 3. 检查 `lambertian_scattering_test.cpp` 的场景设置

**当前设置**:
```cpp
// Cornell Box 3x3x3
// 地板: y=0
// 天花板: y=3
// 墙壁: x=±1.5, z=±1.5

// 光源: y=2.9, 1x1
float kLightVertices[] = {
    -0.5f, 2.9f, 0.5f,  -0.5f, 2.9f, -0.5f,
    0.5f, 2.9f, -0.5f,  0.5f, 2.9f, 0.5f
};

// 球体: (0, 1, 0), radius=0.5
createSphere(sphereVerts, sphereInds, 0.0f, 1.0f, 0.0f, 0.5f, 64, 48);

// 相机位置: (0, 1.5, 5) 看向 (0, 1, 0)
camera.position[0] = 0.0f;
camera.position[1] = 1.5f;
camera.position[2] = 5.0f;
camera.direction[0] = 0.0f;
camera.direction[1] = -0.196f;
camera.direction[2] = -0.981f;
```

**可能的问题**:

1. **几何体顶点顺序**: 检查所有几何体的顶点顺序是否正确（逆时针）
2. **光源法线方向**: 光源面片的法线应该朝下（向场景内部）
3. **相机方向归一化**: 检查相机方向向量是否正确归一化

### 4. 建议的调试方法

#### 方法 1: 使用最简场景

创建一个最简场景，逐步添加元素：

```cpp
// 1. 只有地板 + 光源
// 2. 添加一个球体
// 3. 添加背景墙
// 4. 添加更多球体
```

#### 方法 2: 使用 Cornell Box 模板

直接复制 `cornell_box_improved_test.cpp` 的场景设置，只替换材质：

```cpp
// 1. 复制 Cornell Box 场景设置
// 2. 将中心球体材质替换为 Disney BRDF
// 3. 验证渲染结果
// 4. 添加更多球体
```

#### 方法 3: 添加调试输出

在渲染前输出场景信息：

```cpp
printf("Camera: pos=(%.2f, %.2f, %.2f), dir=(%.2f, %.2f, %.2f)\n",
       camera.position[0], camera.position[1], camera.position[2],
       camera.direction[0], camera.direction[1], camera.direction[2]);

printf("Light: y=%.2f, size=%.2fx%.2f\n", lightY, lightSize, lightSize);

printf("Spheres: y=%.2f, radius=%.2f\n", sphereY, sphereRadius);
```

#### 方法 4: 检查几何体法线

添加代码检查几何体的法线方向：

```cpp
// 计算三角形法线
float v0[3] = { vertices[indices[0]*3+0], vertices[indices[0]*3+1], vertices[indices[0]*3+2] };
float v1[3] = { vertices[indices[1]*3+0], vertices[indices[1]*3+1], vertices[indices[1]*3+2] };
float v2[3] = { vertices[indices[2]*3+0], vertices[indices[2]*3+1], vertices[indices[2]*3+2] };

float e1[3] = { v1[0]-v0[0], v1[1]-v0[1], v1[2]-v0[2] };
float e2[3] = { v2[0]-v0[0], v2[1]-v0[1], v2[2]-v0[2] };

float normal[3] = {
    e1[1]*e2[2] - e1[2]*e2[1],
    e1[2]*e2[0] - e1[0]*e2[2],
    e1[0]*e2[1] - e1[1]*e2[0]
};

printf("Normal: (%.2f, %.2f, %.2f)\n", normal[0], normal[1], normal[2]);
```

## 推荐的修复方案

### 方案 1: 使用 Cornell Box 模板（推荐）

**优点**:
- 场景设置已验证正确
- 相机、光源、几何体位置关系清晰
- 易于调试

**步骤**:
1. 复制 `cornell_box_improved_test.cpp` 的场景设置
2. 将中心球体材质替换为 Disney BRDF 或 LambertianScattering
3. 验证渲染结果
4. 根据需要添加更多球体或调整场景

### 方案 2: 修复现有场景设置

**优点**:
- 保留原有的场景设计
- 学习如何调试场景设置问题

**步骤**:
1. 检查相机位置和方向
2. 检查光源位置和法线方向
3. 检查几何体位置和法线方向
4. 逐步添加几何体，验证每一步的渲染结果

## 参考资料

### 工作正常的测试程序

- `test/cornell_box_improved_test.cpp` - Cornell Box 场景
- `test/anisotropic_test.cpp` - 简化场景

### 相关文档

- `docs/MATERIAL_SYSTEM_ENHANCEMENT_SUMMARY.md` - 材质系统实现报告
- `CHANGELOG.md` - 变更日志

## 后续步骤

1. 选择修复方案（推荐方案 1）
2. 修改 `disney_brdf_test.cpp` 和 `lambertian_scattering_test.cpp`
3. 重新编译和测试
4. 验证渲染结果
5. 更新文档和 CHANGELOG

## 联系信息

如有问题，请参考以下资源：

- 项目路线图: `docs/TODO.md`
- 材质系统报告: `docs/MATERIAL_SYSTEM_ENHANCEMENT_SUMMARY.md`
- 成果展示: `docs/MATERIAL_ENHANCEMENT_RESULTS.md`
