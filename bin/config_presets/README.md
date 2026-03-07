# 配置预设

本目录包含针对不同使用场景的预设配置文件。

## 📋 配置文件结构

配置文件分为两类：

### 1. 场景配置（*_scene.ini）
包含测试程序相关的参数：
- 图像分辨率、采样数、深度
- 输出文件名和格式
- 相机位置和参数
- 场景描述

### 2. 性能配置（*_performance.ini）
包含渲染器核心的性能参数：
- CPU-GPU同步间隔
- 路径压缩和排序
- Kernel线程块大小
- 早期终止参数
- 内存和高级优化

### 3. 兼容配置（*.ini）
为了向后兼容，保留了合并的配置文件。

---

## 📁 预设列表

### preview - 快速预览

**用途**：快速迭代、场景调试、实时预览

**特点**：
- 低分辨率（512×512）
- 低采样数（16）
- 低深度（4）
- 禁用排序优化
- 激进的早期终止

**性能**：~0.2秒

**使用方式**：

```bash
# 方式1: 使用分离的配置（推荐）
.\cornell_box_improved_test.exe config_presets\preview_scene.ini config_presets\preview_performance.ini

# 方式2: 使用合并的配置（向后兼容）
.\cornell_box_improved_test.exe config_presets\preview.ini
```

---

### benchmark - 性能基准测试

**用途**：性能测试、对比分析、优化验证

**特点**：
- 标准分辨率（512×512）
- 标准采样数（1024）
- 启用性能计数器
- 打印详细耗时
- 默认优化配置

**性能**：~8秒

**使用方式**：

```bash
# 方式1: 使用分离的配置（推荐）
.\cornell_box_improved_test.exe config_presets\benchmark_scene.ini config_presets\benchmark_performance.ini

# 方式2: 使用合并的配置（向后兼容）
.\cornell_box_improved_test.exe config_presets\benchmark.ini > benchmark_result.txt
```

---

### high_quality - 高质量渲染

**用途**：最终输出、照片级渲染、展示

**特点**：
- 高分辨率（1920×1080）
- 高采样数（2048）
- 高深度（16）
- 禁用早期终止（确保质量）
- 所有优化启用

**性能**：~120秒

**使用方式**：

```bash
# 方式1: 使用分离的配置（推荐）
.\cornell_box_improved_test.exe config_presets\high_quality_scene.ini config_presets\high_quality_performance.ini

# 方式2: 使用合并的配置（向后兼容）
.\cornell_box_improved_test.exe config_presets\high_quality.ini
```

---

### debug - 调试配置

**用途**：开发调试、问题排查、验证正确性

**特点**：
- 极小分辨率（256×256）
- 极少采样（4）
- 每次深度都同步
- 启用所有调试选项
- 禁用融合kernel（便于单步调试）

**性能**：~0.05秒

**使用方式**：

```bash
# 方式1: 使用分离的配置（推荐）
.\cornell_box_improved_test.exe config_presets\debug_scene.ini config_presets\debug_performance.ini

# 方式2: 使用合并的配置（向后兼容）
.\cornell_box_improved_test.exe config_presets\debug.ini
```

---

## 🎯 快速选择

```
我想...                           场景配置                    性能配置
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
快速查看场景效果                  preview_scene.ini          preview_performance.ini
调整场景参数并快速预览            preview_scene.ini          preview_performance.ini
进行性能测试和对比                benchmark_scene.ini        benchmark_performance.ini
生成最终的高质量图像              high_quality_scene.ini     high_quality_performance.ini
调试代码或排查问题                debug_scene.ini            debug_performance.ini
```

### 配置文件组合

```
预设名称          场景配置                    性能配置                      合并配置（兼容）
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
preview          preview_scene.ini          preview_performance.ini       preview.ini
benchmark        benchmark_scene.ini        benchmark_performance.ini     benchmark.ini
high_quality     high_quality_scene.ini     high_quality_performance.ini  high_quality.ini
debug            debug_scene.ini            debug_performance.ini         debug.ini
```

## 📊 性能对比

```
场景: Cornell Box

配置          分辨率      采样    时间      质量
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
preview       512×512     16      0.2秒     噪声大
默认          512×512     64      0.8秒     轻微噪声
benchmark     512×512     1024    8.0秒     清晰
high_quality  1920×1080   2048    120秒     照片级
```

## 🔧 自定义配置

### 方式1: 分离配置（推荐）

```bash
# 1. 复制场景配置
copy config_presets\benchmark_scene.ini my_scene.ini

# 2. 编辑场景参数（分辨率、采样、相机等）
notepad my_scene.ini

# 3. 选择或创建性能配置
copy config_presets\benchmark_performance.ini my_performance.ini
notepad my_performance.ini

# 4. 使用自定义配置
.\cornell_box_improved_test.exe my_scene.ini my_performance.ini
```

**优点**：
- 场景和性能参数分离
- 可以复用性能配置
- 便于团队协作（场景设计师 vs 性能工程师）

### 方式2: 合并配置（向后兼容）

```bash
# 1. 复制预设
copy config_presets\benchmark.ini my_config.ini

# 2. 编辑参数
notepad my_config.ini

# 3. 使用自定义配置
.\cornell_box_improved_test.exe my_config.ini
```

### 常见修改

**提高质量**（修改场景配置）：
```ini
# my_scene.ini
[Render]
Samples = 2048      # 增加采样
MaxDepth = 16       # 增加深度
```

**提高速度**（修改性能配置）：
```ini
# my_performance.ini
[Optimization]
SyncInterval = 8                    # 减少同步
CompressionThreshold = 0.80         # 减少压缩

[EarlyTermination]
EnableEarlyTermination = true       # 启用早期终止
Threshold = 0.02                    # 更激进的终止
```

**调试特定像素**（修改场景配置）：
```ini
# debug_pixel.ini
[Render]
Width = 1
Height = 1
Samples = 1
```

**针对RTX 4090优化**（修改性能配置）：
```ini
# rtx4090_performance.ini
[KernelConfig]
ProcessHitsBlockSize = 256          # Ada架构，更多寄存器
SampleLightsBlockSize = 256
SampleBSDFBlockSize = 256

[Memory]
UseMaterialCache = true             # 更多Shared memory
```

## 📖 详细文档

完整的配置参数说明和调优指南，请参考：

**docs/CONFIGURATION_GUIDE.md**

包含：
- 所有参数的详细说明
- 性能影响分析
- 调优流程和建议
- 不同GPU架构的推荐配置

## 💡 提示

1. **从预设开始**：选择最接近需求的预设，再微调
2. **逐个调整**：一次只改一个参数，观察影响
3. **记录结果**：保存性能数据，便于对比
4. **保存配置**：找到最优配置后，保存为自己的预设

---

**更新日期**: 2026-03-07  
**维护者**: VLR开发团队
