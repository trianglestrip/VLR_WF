# 配置文件使用示例

> **分离配置 vs 合并配置的实际使用案例**

---

## 配置文件架构

### 分离配置（推荐）

```
场景配置 (scene_config.ini)     性能配置 (vlr_performance.ini)
┌─────────────────────────┐     ┌─────────────────────────┐
│ [Render]                │     │ [Optimization]          │
│   Width, Height         │     │   SyncInterval          │
│   Samples, MaxDepth     │     │   CompressionThreshold  │
│                         │     │   EnablePathSorting     │
│ [Output]                │     │                         │
│   Filename, Format      │     │ [KernelConfig]          │
│                         │     │   BlockSizes            │
│ [Camera]                │     │                         │
│   Position, FOV         │     │ [EarlyTermination]      │
│                         │     │   Threshold, MinDepth   │
└─────────────────────────┘     └─────────────────────────┘
         │                               │
         └───────────┬───────────────────┘
                     ▼
            ┌─────────────────┐
            │  VLR Renderer   │
            └─────────────────┘
```

**优点**：
- ✅ 关注点分离（场景 vs 性能）
- ✅ 复用性能配置（多个场景共享同一性能配置）
- ✅ 团队协作（场景设计师 vs 性能工程师）
- ✅ 便于调优（只修改性能配置，不影响场景）

---

## 使用场景

### 场景1: 多场景共享性能配置

```bash
# 场景设计师创建多个场景
scene1.ini  # Cornell Box
scene2.ini  # Glass Spheres
scene3.ini  # Dragon Model

# 性能工程师创建一个优化的性能配置
rtx3080_optimized.ini

# 渲染所有场景，使用相同的性能配置
.\cornell_box_test.exe scene1.ini rtx3080_optimized.ini
.\glass_spheres_test.exe scene2.ini rtx3080_optimized.ini
.\dragon_test.exe scene3.ini rtx3080_optimized.ini
```

**优势**：性能配置只需调优一次，所有场景受益

---

### 场景2: 同一场景不同性能配置

```bash
# 场景配置（固定）
my_scene.ini

# 不同的性能配置
preview_perf.ini      # 快速预览
benchmark_perf.ini    # 性能测试
final_perf.ini        # 最终渲染

# 快速预览
.\test.exe my_scene.ini preview_perf.ini

# 性能测试
.\test.exe my_scene.ini benchmark_perf.ini

# 最终渲染
.\test.exe my_scene.ini final_perf.ini
```

**优势**：场景不变，只切换性能配置

---

### 场景3: GPU特化性能配置

```bash
# 场景配置（通用）
cornell_box_scene.ini

# 不同GPU的性能配置
rtx2060_performance.ini   # Turing架构
rtx3080_performance.ini   # Ampere架构
rtx4090_performance.ini   # Ada架构

# 在RTX 2060上运行
.\test.exe cornell_box_scene.ini rtx2060_performance.ini

# 在RTX 4090上运行
.\test.exe cornell_box_scene.ini rtx4090_performance.ini
```

**优势**：针对不同GPU优化，无需修改场景

---

### 场景4: 团队协作

```
团队结构:
├── 场景设计师 (Artist)
│   └── 负责: 场景配置（分辨率、相机、输出）
│
└── 性能工程师 (Engineer)
    └── 负责: 性能配置（同步、压缩、kernel）

工作流:
1. 场景设计师创建 my_scene.ini
2. 提交到Git
3. 性能工程师创建 optimized_performance.ini
4. 提交到Git
5. 两者独立迭代，互不影响
```

**示例**：

场景设计师：
```bash
# 创建场景配置
copy config_presets\benchmark_scene.ini my_scene.ini

# 修改相机和输出
notepad my_scene.ini
# 修改: PositionZ = 8.0, Samples = 128

# 提交
git add my_scene.ini
git commit -m "Add new camera angle"
```

性能工程师：
```bash
# 创建性能配置
copy config_presets\benchmark_performance.ini optimized.ini

# 调优参数
python scripts\auto_tune.py --config optimized.ini --output optimized.ini

# 提交
git add optimized.ini
git commit -m "Optimize for RTX 3080"
```

---

## 代码示例

### 使用分离配置

```cpp
#include "config_loader.h"

int main(int argc, char* argv[]) {
    vlr::RenderConfig config;
    
    if (argc >= 3) {
        // 方式1: 分离配置（推荐）
        std::string sceneConfig = argv[1];
        std::string perfConfig = argv[2];
        
        if (!vlr::ConfigLoader::loadSplitConfig(sceneConfig, perfConfig, config)) {
            fprintf(stderr, "[Error] Failed to load configs\n");
            return 1;
        }
        
        printf("[Info] Loaded scene config: %s\n", sceneConfig.c_str());
        printf("[Info] Loaded performance config: %s\n", perfConfig.c_str());
        
    } else if (argc == 2) {
        // 方式2: 合并配置（向后兼容）
        std::string configFile = argv[1];
        
        if (!vlr::ConfigLoader::loadRenderConfig(configFile, config)) {
            fprintf(stderr, "[Error] Failed to load config: %s\n", configFile.c_str());
            return 1;
        }
        
        printf("[Info] Loaded config: %s\n", configFile.c_str());
        
    } else {
        // 使用默认配置
        printf("[Info] Using default configuration\n");
    }
    
    // 打印配置摘要
    vlr::ConfigLoader::printConfigSummary(config);
    
    // 创建上下文并渲染
    VLRContext ctx = nullptr;
    vlrCreateContext(nullptr, config.deviceID, &ctx);
    vlrSetPerformanceConfig(ctx, &config.perfConfig);
    
    // ... 渲染 ...
    
    return 0;
}
```

---

## 实际案例

### 案例1: 快速预览 → 高质量渲染

```bash
# 第一步: 快速预览，调整场景
.\test.exe config_presets\preview_scene.ini config_presets\preview_performance.ini
# 输出: preview.png (~0.2秒)

# 查看结果，调整相机
notepad config_presets\preview_scene.ini
# 修改: PositionZ = 8.0

# 再次预览
.\test.exe config_presets\preview_scene.ini config_presets\preview_performance.ini

# 第二步: 满意后，切换到高质量渲染
# 场景配置不变，只修改采样和深度
copy config_presets\preview_scene.ini final_scene.ini
notepad final_scene.ini
# 修改: Samples = 2048, MaxDepth = 16

# 使用高质量性能配置
.\test.exe final_scene.ini config_presets\high_quality_performance.ini
# 输出: high_quality.png (~120秒)
```

---

### 案例2: 性能调优

```bash
# 场景固定
my_scene.ini  # 512×512, 1024采样

# 测试不同性能配置
perf_v1.ini  # SyncInterval=4
perf_v2.ini  # SyncInterval=8
perf_v3.ini  # SyncInterval=4 + EnablePathSorting=true

# 对比性能
python scripts\compare_configs.py \
  --scene my_scene.ini \
  perf_v1.ini perf_v2.ini perf_v3.ini

# 输出:
# perf_v1.ini: 8.0秒
# perf_v2.ini: 7.9秒 (+1.3%)
# perf_v3.ini: 7.2秒 (+10%)  ← 最优
```

---

### 案例3: 多GPU环境

```bash
# 场景配置（通用）
scene.ini

# GPU特化性能配置
gpu0_performance.ini  # DeviceID=0, RTX 3080
gpu1_performance.ini  # DeviceID=1, RTX 2060

# 在不同GPU上渲染
.\test.exe scene.ini gpu0_performance.ini  # 使用RTX 3080
.\test.exe scene.ini gpu1_performance.ini  # 使用RTX 2060
```

---

### 案例4: 批量渲染

```bash
# 创建批处理脚本 render_batch.bat

@echo off
set PERF_CONFIG=config_presets\benchmark_performance.ini

echo Rendering scene 1...
cornell_box_test.exe scene1.ini %PERF_CONFIG%

echo Rendering scene 2...
glass_spheres_test.exe scene2.ini %PERF_CONFIG%

echo Rendering scene 3...
dragon_test.exe scene3.ini %PERF_CONFIG%

echo All scenes rendered!
```

**优势**：所有场景使用相同的性能配置，便于管理

---

## 配置文件模板

### 场景配置模板

```ini
# my_scene.ini
[Render]
Width = 512
Height = 512
Samples = 64
MaxDepth = 8
Exposure = 1.0

[Output]
Filename = my_output.png
Format = png

[Camera]
PositionX = 0.0
PositionY = 1.5
PositionZ = 6.0
TargetX = 0.0
TargetY = 1.5
TargetZ = 0.0
FOV = 40.0
LensRadius = 0.0
FocusDistance = 1.0

[Scene]
SceneType = medium
Description = My custom scene
```

### 性能配置模板

```ini
# my_performance.ini
[Optimization]
SyncInterval = 4
CompressionThreshold = 0.75
MinPathsForCompression = 2048
EnablePathSorting = true
EnableStreamCompaction = true

[KernelConfig]
GenerateRaysBlockSize = 256
ProcessHitsBlockSize = 128
SampleLightsBlockSize = 256
SampleBSDFBlockSize = 192
AccumulateBlockSize = 256

[EarlyTermination]
EnableEarlyTermination = true
Threshold = 0.01
MinDepth = 10

[Memory]
UseRestrictPointers = true
UseMaterialCache = false
UseTextureMemory = false

[Advanced]
UseCudaGraphs = false
UseWarpOptimizations = true
UseFusedKernels = true

[Debug]
EnableNaNTracking = false
EnablePerfCounters = false
PrintKernelTiming = false
ValidateQueues = false

[Device]
DeviceID = 0
VerboseLogging = true
```

---

## 最佳实践

### 1. 场景配置命名

```
场景名称_scene.ini

示例:
cornell_box_scene.ini
glass_spheres_scene.ini
dragon_scene.ini
outdoor_scene.ini
```

### 2. 性能配置命名

```
GPU型号_场景类型_performance.ini

示例:
rtx3080_simple_performance.ini
rtx3080_complex_performance.ini
rtx4090_general_performance.ini
```

### 3. 目录组织

```
my_project/
├── scenes/                     # 场景配置
│   ├── scene1.ini
│   ├── scene2.ini
│   └── scene3.ini
├── performance/                # 性能配置
│   ├── rtx3080_optimized.ini
│   ├── rtx2060_optimized.ini
│   └── debug.ini
└── outputs/                    # 输出图像
    ├── scene1.png
    ├── scene2.png
    └── scene3.png
```

### 4. Git版本控制

```bash
# 场景配置应该提交
git add scenes/*.ini

# 性能配置也应该提交（团队共享）
git add performance/*.ini

# 输出图像可以忽略（太大）
echo "outputs/" >> .gitignore
```

---

## 迁移指南

### 从合并配置迁移到分离配置

```bash
# 假设你有一个合并的配置
my_old_config.ini

# 步骤1: 创建场景配置
# 复制 [Render], [Output], [Camera] 节
copy my_old_config.ini my_scene.ini
# 手动删除性能相关的节

# 步骤2: 创建性能配置
# 复制 [Optimization], [KernelConfig] 等节
copy my_old_config.ini my_performance.ini
# 手动删除场景相关的节

# 步骤3: 验证
python scripts\validate_config.py my_scene.ini
python scripts\validate_config.py my_performance.ini

# 步骤4: 测试
.\test.exe my_scene.ini my_performance.ini
```

**自动化脚本**（可选）：

```python
# split_config.py
import configparser

def split_config(input_file, scene_file, perf_file):
    config = configparser.ConfigParser()
    config.read(input_file)
    
    # 场景相关节
    scene_sections = ['Render', 'Output', 'Camera', 'Scene']
    scene_config = configparser.ConfigParser()
    for section in scene_sections:
        if config.has_section(section):
            scene_config.add_section(section)
            for key, value in config.items(section):
                scene_config.set(section, key, value)
    
    # 性能相关节
    perf_sections = ['Optimization', 'KernelConfig', 'EarlyTermination', 
                     'Memory', 'Advanced', 'Debug', 'Device']
    perf_config = configparser.ConfigParser()
    for section in perf_sections:
        if config.has_section(section):
            perf_config.add_section(section)
            for key, value in config.items(section):
                perf_config.set(section, key, value)
    
    # 保存
    with open(scene_file, 'w') as f:
        scene_config.write(f)
    with open(perf_file, 'w') as f:
        perf_config.write(f)

# 使用
split_config('my_old_config.ini', 'my_scene.ini', 'my_performance.ini')
```

---

## 高级用法

### 1. 环境变量支持

```bash
# 设置默认性能配置
set VLR_PERFORMANCE_CONFIG=rtx3080_optimized.ini

# 只指定场景配置
.\test.exe my_scene.ini
# 程序自动使用 %VLR_PERFORMANCE_CONFIG%
```

### 2. 配置继承

```ini
# base_performance.ini (基础配置)
[Optimization]
SyncInterval = 4
CompressionThreshold = 0.75
EnablePathSorting = true

# rtx4090_performance.ini (继承并覆盖)
# 包含: base_performance.ini
[KernelConfig]
ProcessHitsBlockSize = 256  # 覆盖默认值
```

### 3. 配置验证钩子

```bash
# pre-commit hook
#!/bin/bash
for config in *.ini; do
    python scripts/validate_config.py "$config" || exit 1
done
```

---

## 性能对比

### 分离配置 vs 合并配置

| 方面 | 分离配置 | 合并配置 |
|------|---------|---------|
| 文件数量 | 2个 | 1个 |
| 复用性 | ⭐⭐⭐⭐⭐ | ⭐⭐ |
| 可维护性 | ⭐⭐⭐⭐⭐ | ⭐⭐⭐ |
| 团队协作 | ⭐⭐⭐⭐⭐ | ⭐⭐ |
| 简单性 | ⭐⭐⭐ | ⭐⭐⭐⭐⭐ |
| 向后兼容 | ✅ | ✅ |

---

## 常见问题

### Q1: 必须使用分离配置吗？

**A**: 不是必须的。两种方式都支持：

```bash
# 分离配置（推荐）
.\test.exe scene.ini performance.ini

# 合并配置（向后兼容）
.\test.exe combined.ini
```

### Q2: 如何选择使用哪种方式？

**A**: 根据项目规模：

```
小项目（1-3个场景）:
  → 使用合并配置（简单）

中型项目（5-10个场景）:
  → 使用分离配置（复用性能配置）

大型项目（10+场景，多GPU）:
  → 必须使用分离配置（便于管理）
```

### Q3: 性能配置可以跨场景使用吗？

**A**: 可以，但需要注意场景类型：

```
简单场景 → simple_performance.ini
  EnablePathSorting = false

复杂场景 → complex_performance.ini
  EnablePathSorting = true
  CompressionThreshold = 0.60
```

建议为不同复杂度的场景创建不同的性能配置。

### Q4: 如何快速创建配置？

**A**: 使用生成器脚本：

```bash
# 生成场景配置
python scripts\generate_config.py --scene medium --output my_scene.ini

# 生成性能配置（针对GPU）
python scripts\generate_config.py --gpu ampere --output rtx3080_perf.ini
```

---

## 参考文档

- **[CONFIGURATION_GUIDE.md](CONFIGURATION_GUIDE.md)** - 完整配置指南
- **[CONFIG_QUICK_REFERENCE.md](CONFIG_QUICK_REFERENCE.md)** - 快速参考
- **[CONFIG_SYSTEM_OVERVIEW.md](CONFIG_SYSTEM_OVERVIEW.md)** - 系统架构
- **[../scripts/README.md](../scripts/README.md)** - 工具脚本

---

**版本**: 1.0  
**更新日期**: 2026-03-07  
**作者**: VLR开发团队
