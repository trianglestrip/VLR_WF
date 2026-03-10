# Viewer 快速开始指南

## 🚀 5分钟上手

### 步骤1: 验证安装

确认以下文件存在：

```powershell
# 检查Assimp
Test-Path viewer\external\assimp\include\assimp\Importer.hpp  # 应返回 True
Test-Path viewer\external\assimp\lib\assimp-vc143-mt.lib      # 应返回 True
Test-Path viewer\external\assimp\bin\assimp-vc143-mt.dll      # 应返回 True

# 检查编译输出
Test-Path bin\Viewer.lib                  # 应返回 True
Test-Path bin\simple_viewer_test.exe      # 应返回 True
Test-Path bin\assimp-vc143-mt.dll         # 应返回 True
```

如果Assimp文件不存在，运行：
```powershell
cd viewer\external\assimp
.\download_and_build.ps1
cd ..\..\..
```

### 步骤2: 编译项目（如果还没编译）

```powershell
# 配置
cmake -B build -S . -G "Visual Studio 17 2022" -A x64

# 编译Viewer库
cmake --build build --config Release --target Viewer

# 编译测试程序
cmake --build build --config Release --target simple_viewer_test
```

### 步骤3: 运行测试

```powershell
# 渲染sphere模型
.\bin\simple_viewer_test.exe test\resources\sphere\sphere.obj sphere_output.png

# 查看输出
start sphere_output.png
```

## 📖 使用示例

### 示例1: 加载OBJ模型

```cpp
#include "VLRRenderer.h"
#include "SceneLoader.h"

int main() {
    // 创建渲染器
    viewer::VLRRenderer renderer;
    viewer::VLRRenderer::Config config;
    config.width = 1920;
    config.height = 1080;
    config.maxSamples = 1024;
    renderer.initialize(config);

    // 创建场景
    VLRScene scene = renderer.createScene();
    renderer.setScene(scene);

    // 加载模型
    viewer::SceneLoader loader(renderer);
    loader.loadScene("path/to/model.obj");

    // 设置相机
    float pos[3], target[3], up[3] = {0, 1, 0};
    loader.getSuggestedCameraPosition(pos, target);
    renderer.setCamera(pos, target, up, 45.0f, 16.0f/9.0f);

    // 渲染
    renderer.render("output.png");
    return 0;
}
```

### 示例2: 自定义加载选项

```cpp
viewer::SceneLoader::LoadOptions options;
options.flipUVs = true;           // 翻转UV坐标
options.triangulate = true;       // 三角化所有面
options.generateSmoothNormals = true;  // 生成平滑法线
options.optimizeMeshes = true;    // 优化网格性能
options.scale = 2.0f;             // 放大2倍

loader.loadScene("model.fbx", options);
```

### 示例3: 自定义相机

```cpp
// 手动设置相机位置
float position[3] = {0.0f, 2.0f, 5.0f};
float target[3] = {0.0f, 0.0f, 0.0f};
float up[3] = {0.0f, 1.0f, 0.0f};
float fov = 60.0f;  // 度
float aspect = 16.0f / 9.0f;

renderer.setCamera(position, target, up, fov, aspect);
```

## 🎨 支持的格式

| 格式 | 扩展名 | 说明 |
|------|--------|------|
| Wavefront | .obj | 最常用的3D格式 |
| FBX | .fbx | Autodesk格式，支持动画 |
| glTF | .gltf, .glb | 现代3D传输格式 |
| COLLADA | .dae | XML格式，支持动画 |
| 3D Studio | .3ds | 经典格式 |
| Blender | .blend | Blender原生格式 |
| PLY | .ply | 点云和网格 |
| STL | .stl | 3D打印格式 |

## 🔧 配置选项

### VLRRenderer::Config

```cpp
struct Config {
    uint32_t width = 1024;        // 渲染宽度
    uint32_t height = 768;        // 渲染高度
    uint32_t maxSamples = 1024;   // 每像素采样数
    uint32_t maxBounces = 8;      // 光线最大反弹次数
    bool enableDenoiser = true;   // 启用降噪器
};
```

### SceneLoader::LoadOptions

```cpp
struct LoadOptions {
    bool flipUVs = true;              // 翻转UV坐标
    bool triangulate = true;          // 三角化网格
    bool generateNormals = false;     // 生成法线
    bool generateSmoothNormals = true; // 生成平滑法线
    bool optimizeMeshes = true;       // 优化网格
    float scale = 1.0f;               // 缩放因子
};
```

## 📝 添加新测试

在 `viewer/examples/` 创建新文件，然后在 `CMakeLists.txt` 添加：

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

重新配置并编译：

```powershell
cmake -B build -S .
cmake --build build --config Release --target my_test
```

## ⚡ 性能提示

1. **采样数**: 从低采样（64-128）开始测试，确认场景正确后再提高
2. **分辨率**: 开发时使用较低分辨率（512x512）加快迭代
3. **网格优化**: 启用 `optimizeMeshes` 可显著提升性能
4. **反弹次数**: 对于简单场景，4-6次反弹通常足够

## 🐛 常见问题

### Q: 编译时找不到Assimp

**A**: 运行安装脚本：
```powershell
cd viewer\external\assimp
.\download_and_build.ps1
```

### Q: 运行时找不到DLL

**A**: 确保DLL在bin目录：
```powershell
Copy-Item viewer\external\assimp\bin\assimp-vc143-mt.dll bin\
```

### Q: 加载模型失败

**A**: 检查：
1. 文件路径是否正确
2. 模型格式是否支持
3. 模型文件是否损坏

### Q: 渲染结果全黑

**A**: 可能原因：
1. 场景没有光源
2. 相机位置不正确
3. 材质设置问题

## 📚 更多资源

- **Viewer完整文档**: `viewer/README.md`
- **Assimp文档**: https://assimp-docs.readthedocs.io/
- **VLR API参考**: `libVLR/include/vlr/vlr.h`

## 🎯 下一步

1. 尝试加载不同格式的模型
2. 实验不同的渲染参数
3. 创建自定义测试场景
4. 扩展VLRRenderer添加更多功能

Happy Rendering! 🎨
