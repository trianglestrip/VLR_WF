# Assimp 预编译库集成说明

## 快速开始（推荐）

### 自动构建脚本

在PowerShell中运行：

```powershell
cd viewer\external\assimp
.\download_and_build.ps1
```

脚本会自动：
1. 下载Assimp v5.4.3源码
2. 配置CMake（仅启用常用导入器）
3. 编译Release版本
4. 安装到当前目录

**预计时间**: 5-10分钟

---

## 手动方法

### 方法1: 从GitHub下载并编译

```powershell
# 1. 克隆Assimp仓库
git clone --depth 1 --branch v5.4.3 https://github.com/assimp/assimp.git temp_assimp

# 2. 配置CMake
cmake -S temp_assimp -B build_assimp `
    -DCMAKE_INSTALL_PREFIX="viewer/external/assimp" `
    -DBUILD_SHARED_LIBS=ON `
    -DASSIMP_BUILD_TESTS=OFF `
    -DASSIMP_BUILD_ASSIMP_TOOLS=OFF `
    -DASSIMP_BUILD_SAMPLES=OFF `
    -DASSIMP_NO_EXPORT=ON `
    -G "Visual Studio 17 2022" -A x64

# 3. 编译并安装
cmake --build build_assimp --config Release --target install

# 4. 清理
Remove-Item -Recurse temp_assimp, build_assimp
```

### 方法2: 使用vcpkg（如果已安装）

```powershell
vcpkg install assimp:x64-windows

# 然后手动复制文件到此目录：
# vcpkg/installed/x64-windows/include -> viewer/external/assimp/include
# vcpkg/installed/x64-windows/lib -> viewer/external/assimp/lib
# vcpkg/installed/x64-windows/bin -> viewer/external/assimp/bin
```

---

## 所需目录结构

完成后，此目录应包含：

```
viewer/external/assimp/
├── include/           # 头文件
│   └── assimp/
│       ├── Importer.hpp
│       ├── scene.h
│       ├── postprocess.h
│       └── ...
├── lib/               # 静态库/导入库
│   ├── assimp-vc143-mt.lib  (Release)
│   └── assimp-vc143-mtd.lib (Debug, 可选)
└── bin/               # DLL文件
    ├── assimp-vc143-mt.dll  (Release)
    └── assimp-vc143-mtd.dll (Debug, 可选)
```

**注意**: 
- `vc143` 对应 Visual Studio 2022
- 如果使用其他版本的VS，文件名可能不同（如 `vc142` = VS2019）
- CMake会自动查找可用的库文件

---

## 验证安装

检查关键文件是否存在：

```powershell
Test-Path include/assimp/Importer.hpp
Test-Path lib/assimp-vc143-mt.lib
Test-Path bin/assimp-vc143-mt.dll
```

如果都返回 `True`，则安装成功！

---

## 支持的模型格式

启用的导入器：
- ✅ OBJ (Wavefront)
- ✅ FBX (Autodesk)
- ✅ GLTF/GLB (Khronos)
- ✅ COLLADA (.dae)
- ✅ 3DS (3D Studio)
- ✅ Blender (.blend)
- ✅ PLY (Stanford)
- ✅ STL (Stereolithography)

---

## 故障排除

### 问题1: CMake找不到Assimp

**解决**: 确保目录结构正确，`include/assimp/Importer.hpp` 必须存在。

### 问题2: 运行时找不到DLL

**解决**: 
1. 检查 `bin/assimp-vc143-mt.dll` 是否被复制到项目的 `bin/` 目录
2. 或者将 `viewer/external/assimp/bin` 添加到系统PATH

### 问题3: 链接错误 (LNK2019)

**解决**: 
1. 确保使用的库文件与编译器版本匹配
2. 检查运行时库设置（应为 `/MD` 或 `/MDd`）

---

## 更多信息

- Assimp官网: https://www.assimp.org/
- GitHub仓库: https://github.com/assimp/assimp
- 文档: https://assimp-docs.readthedocs.io/
