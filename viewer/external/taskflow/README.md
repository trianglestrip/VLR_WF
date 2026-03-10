# Taskflow 集成说明

Taskflow 是一个现代 C++ 并行编程库，用于实现高性能的任务图和并行处理。

## 版本信息

- **版本**: v3.8.0
- **类型**: Header-Only（仅头文件库）
- **要求**: C++17 或更高（推荐 C++20）
- **官网**: https://taskflow.github.io/
- **GitHub**: https://github.com/taskflow/taskflow

## 安装方式

### 方式 1: 自动下载脚本（推荐）

运行提供的 PowerShell 脚本：

```powershell
cd viewer/external/taskflow
.\download_taskflow.ps1
```

脚本会自动：
1. 下载 Taskflow v3.8.0 源码
2. 解压并提取头文件
3. 安装到 `include/taskflow/` 目录

### 方式 2: 手动下载

如果自动脚本失败，可以手动操作：

1. **下载源码**
   ```
   https://github.com/taskflow/taskflow/archive/refs/tags/v3.8.0.zip
   ```

2. **解压**
   解压 zip 文件到任意位置

3. **复制头文件**
   将解压后的 `taskflow-3.8.0/taskflow/` 目录复制到：
   ```
   viewer/external/taskflow/include/taskflow/
   ```

4. **验证安装**
   确认以下文件存在：
   ```
   viewer/external/taskflow/include/taskflow/taskflow.hpp
   viewer/external/taskflow/include/taskflow/core/executor.hpp
   viewer/external/taskflow/include/taskflow/algorithm/...
   ```

### 方式 3: 使用 Git Clone

```bash
git clone https://github.com/taskflow/taskflow.git temp
mkdir -p include
cp -r temp/taskflow include/
rm -rf temp
```

## 目录结构

安装完成后，目录结构应该是：

```
viewer/external/taskflow/
├── README.md                    # 本文件
├── download_taskflow.ps1        # 自动下载脚本
└── include/
    └── taskflow/
        ├── taskflow.hpp         # 主头文件
        ├── core/
        │   ├── executor.hpp
        │   ├── taskflow.hpp
        │   └── ...
        ├── algorithm/
        │   ├── for_each.hpp
        │   ├── reduce.hpp
        │   └── ...
        └── ...
```

## 使用方法

在代码中包含头文件：

```cpp
#include <taskflow/taskflow.hpp>

int main() {
    tf::Executor executor;
    tf::Taskflow taskflow;
    
    // 创建任务
    auto [A, B, C, D] = taskflow.emplace(
        []() { /* Task A */ },
        []() { /* Task B */ },
        []() { /* Task C */ },
        []() { /* Task D */ }
    );
    
    // 定义依赖关系
    A.precede(B, C);  // A -> B, A -> C
    D.succeed(B, C);  // B -> D, C -> D
    
    // 执行任务图
    executor.run(taskflow).wait();
    
    return 0;
}
```

## 编译

Taskflow 是 header-only 库，无需编译。只需：

1. 包含头文件路径
2. 使用 C++17 或更高标准
3. 链接 pthread（Linux/macOS）

CMake 示例：

```cmake
target_include_directories(MyTarget PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/external/taskflow/include
)

target_compile_features(MyTarget PRIVATE cxx_std_20)
```

## 功能特性

### 1. 静态任务图

```cpp
tf::Taskflow taskflow;
auto [A, B, C] = taskflow.emplace(
    []() { std::cout << "Task A\n"; },
    []() { std::cout << "Task B\n"; },
    []() { std::cout << "Task C\n"; }
);
A.precede(B, C);  // A 必须在 B 和 C 之前执行
```

### 2. 动态任务（Subflow）

```cpp
taskflow.emplace([](tf::Subflow& subflow) {
    auto [A, B, C] = subflow.emplace(
        []() { /* ... */ },
        []() { /* ... */ },
        []() { /* ... */ }
    );
    A.precede(B, C);
});
```

### 3. 并行循环

```cpp
taskflow.for_each(data.begin(), data.end(), [](auto& item) {
    // 并行处理每个元素
    process(item);
});
```

### 4. 条件任务

```cpp
auto condition = taskflow.emplace([&]() {
    return shouldContinue ? 0 : 1;
});

condition.precede(taskA, taskB);
```

### 5. 工作窃取调度器

Taskflow 使用高效的工作窃取调度器，自动负载均衡，支持数百万到数十亿个任务。

## 在 VLR_WF 项目中的应用

### ParallelSceneLoader

使用 Taskflow 实现并行场景加载：

```cpp
#include "ParallelSceneLoader.h"

ParallelSceneLoader::ParallelConfig config;
config.numThreads = 0;  // 自动检测CPU核心数
config.enableTaskGraph = true;  // 启用任务图优化

ParallelSceneLoader loader(renderer, config);
loader.loadSceneParallel("model.obj");
```

### 任务图结构

```
[材质1] [材质2] ... [材质N]  <- 并行加载
   ↓       ↓           ↓
[网格1] [网格2] ... [网格M]  <- 并行处理（依赖材质）
   ↓       ↓           ↓
      [最终化场景]            <- 汇总
```

## 性能优势

- **自动并行化**: 无需手动管理线程池
- **依赖管理**: 自动处理任务间依赖关系
- **负载均衡**: 工作窃取算法自动平衡负载
- **零开销**: Header-only，无运行时开销
- **可视化**: 支持导出 DOT 格式任务图

## 故障排除

### 问题 1: 找不到 taskflow.hpp

**解决方案**: 确认 `viewer/external/taskflow/include/taskflow/taskflow.hpp` 存在

### 问题 2: 编译错误 "requires C++17"

**解决方案**: 在 CMakeLists.txt 中设置：
```cmake
target_compile_features(MyTarget PRIVATE cxx_std_20)
```

### 问题 3: 链接错误（Linux）

**解决方案**: 添加 pthread 链接：
```cmake
target_link_libraries(MyTarget PRIVATE pthread)
```

## 参考资源

- [官方文档](https://taskflow.github.io/taskflow/index.html)
- [快速入门](https://taskflow.github.io/taskflow/QuickStart.html)
- [示例代码](https://github.com/taskflow/taskflow/tree/master/examples)
- [API 参考](https://taskflow.github.io/taskflow/classtf_1_1Taskflow.html)

## 许可证

Taskflow 使用 MIT 许可证，可以自由用于商业和开源项目。
