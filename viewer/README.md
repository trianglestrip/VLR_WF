# VLR Viewer - 3D场景查看器和渲染前端

## 概述

Viewer是VLR_WF项目的前端模块，负责：
- 使用Assimp加载各种3D模型格式
- 封装libVLR的调用，提供简化的渲染接口
- 为测试程序提供便捷的场景加载和渲染功能

## 架构设计

```
viewer/
├── include/              # 公共头文件
│   ├── VLRRenderer.h    # VLR渲染器封装类
│   └── SceneLoader.h    # 场景加载器（Assimp）
├── src/                 # 实现文件
│   ├── VLRRenderer.cpp
│   └── SceneLoader.cpp
├── examples/            # 示例程序
│   └── simple_viewer_test.cpp
└── external/            # 外部依赖
    └── assimp/          # Assimp预编译库
```

## 核心类

### VLRRenderer

封装libVLR的渲染功能，提供简化接口：

```cpp
viewer::VLRRenderer renderer;
viewer::VLRRenderer::Config config;
config.width = 1024;
config.height = 768;
config.maxSamples = 512;

renderer.initialize(config);
VLRScene scene = renderer.createScene();
renderer.setScene(scene);
renderer.setCamera(position, target, up, fov, aspect);
renderer.render("output.png");
```

### SceneLoader

使用Assimp加载3D模型：

```cpp
viewer::SceneLoader loader(renderer);
viewer::SceneLoader::LoadOptions options;
options.triangulate = true;
options.flipUVs = true;

loader.loadScene("model.obj", options);
```

## 支持的模型格式

通过Assimp支持以下格式：
- ✅ **OBJ** - Wavefront Object
- ✅ **FBX** - Autodesk FBX
- ✅ **GLTF/GLB** - Khronos glTF
- ✅ **COLLADA** (.dae)
- ✅ **3DS** - 3D Studio
- ✅ **Blender** (.blend)
- ✅ **PLY** - Stanford Polygon
- ✅ **STL** - Stereolithography

## 构建说明

### 1. 安装Assimp

首先需要安装Assimp预编译库：

```powershell
cd viewer\external\assimp
.\download_and_build.ps1
```

这会自动下载、编译并安装Assimp到正确的位置。

### 2. 配置项目

```powershell
cmake -B build -S . -G "Visual Studio 17 2022" -A x64
```

### 3. 编译

```powershell
# 编译Viewer库
cmake --build build --config Release --target Viewer

# 编译示例程序
cmake --build build --config Release --target simple_viewer_test
```

## 使用示例

### 基本用法

```cpp
#include "VLRRenderer.h"
#include "SceneLoader.h"

int main() {
    // 1. 创建渲染器
    viewer::VLRRenderer renderer;
    viewer::VLRRenderer::Config config;
    config.width = 1024;
    config.height = 768;
    config.maxSamples = 512;
    
    renderer.initialize(config);
    
    // 2. 创建场景
    VLRScene scene = renderer.createScene();
    renderer.setScene(scene);
    
    // 3. 加载模型
    viewer::SceneLoader loader(renderer);
    loader.loadScene("path/to/model.obj");
    
    // 4. 设置相机（自动计算）
    float pos[3], target[3], up[3] = {0, 1, 0};
    loader.getSuggestedCameraPosition(pos, target);
    float aspect = (float)config.width / config.height;
    renderer.setCamera(pos, target, up, 45.0f, aspect);
    
    // 5. 渲染
    renderer.render("output.png");
    
    return 0;
}
```

### 运行示例程序

```powershell
# 渲染sphere模型
.\bin\simple_viewer_test.exe test\resources\sphere\sphere.obj output.png

# 渲染kitchen场景
.\bin\simple_viewer_test.exe bin\resources\models\kitchen\scene.obj kitchen.png
```

## API参考

### VLRRenderer::Config

| 参数 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| width | uint32_t | 1024 | 渲染宽度 |
| height | uint32_t | 768 | 渲染高度 |
| maxSamples | uint32_t | 1024 | 最大采样数 |
| maxBounces | uint32_t | 8 | 最大反弹次数 |
| enableDenoiser | bool | true | 启用降噪器 |

### SceneLoader::LoadOptions

| 参数 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| flipUVs | bool | true | 翻转UV坐标 |
| triangulate | bool | true | 三角化网格 |
| generateNormals | bool | false | 生成法线 |
| generateSmoothNormals | bool | true | 生成平滑法线 |
| optimizeMeshes | bool | true | 优化网格 |
| scale | float | 1.0f | 缩放因子 |

## 依赖关系

```
simple_viewer_test
    └── Viewer (静态库)
        ├── VLR (动态库)
        │   └── CUDA Runtime
        └── Assimp (动态库)
```

## 开发指南

### 添加新的测试场景

1. 在 `viewer/examples/` 创建新的cpp文件
2. 在主 `CMakeLists.txt` 中添加可执行文件目标：

```cmake
if(TARGET Viewer)
    add_executable(my_test viewer/examples/my_test.cpp)
    target_include_directories(my_test PRIVATE
        ${CMAKE_SOURCE_DIR}/viewer/include
        ${CMAKE_SOURCE_DIR}/libVLR/include
    )
    target_link_libraries(my_test PRIVATE Viewer VLR CUDA::cudart)
endif()
```

### 扩展VLRRenderer

VLRRenderer类提供了基础的渲染功能，可以扩展：
- 添加材质创建辅助函数
- 添加光源管理
- 添加交互式渲染支持
- 添加实时预览功能

### 扩展SceneLoader

SceneLoader可以扩展以支持：
- 更复杂的材质转换
- 纹理加载
- 光源导入
- 相机动画

## 故障排除

### 问题1: 找不到assimp-vc143-mt.dll

**解决**: 
```powershell
# 手动复制DLL
Copy-Item viewer\external\assimp\bin\assimp-vc143-mt.dll bin\
```

### 问题2: 链接错误

**解决**: 确保Assimp已正确安装：
```powershell
Test-Path viewer\external\assimp\include\assimp\Importer.hpp
Test-Path viewer\external\assimp\lib\assimp-vc143-mt.lib
```

### 问题3: 运行时崩溃

**解决**: 检查CUDA和OptiX环境是否正确配置。

## 性能优化

- 使用 `LoadOptions::optimizeMeshes = true` 优化网格
- 对于大型场景，考虑使用实例化
- 调整 `maxSamples` 和 `maxBounces` 平衡质量和速度

## 许可证

本模块遵循VLR_WF项目的许可证。

Assimp使用BSD 3-Clause许可证，详见 `external/assimp/LICENSE`。
