# 配置快速参考

> **一页纸速查表 - 打印后放在桌面**

---

## 🚀 快速开始

```bash
# 使用预设配置
.\cornell_box_improved_test.exe config_presets\preview.ini      # 快速预览 (~0.2s)
.\cornell_box_improved_test.exe config_presets\benchmark.ini    # 性能测试 (~8s)
.\cornell_box_improved_test.exe config_presets\high_quality.ini # 高质量 (~120s)
.\cornell_box_improved_test.exe config_presets\debug.ini        # 调试 (~0.05s)
```

---

## ⚙️ 核心参数速查

### 同步优化

| 参数 | 范围 | 默认 | 说明 | 性能影响 |
|------|------|------|------|---------|
| SyncInterval | 1-16 | 4 | CPU-GPU同步间隔（深度数） | +5-10% |

```ini
[Optimization]
SyncInterval = 4    # 平衡 ✓
SyncInterval = 8    # 更快，简单场景
SyncInterval = 1    # 调试用
```

---

### 路径压缩

| 参数 | 范围 | 默认 | 说明 | 性能影响 |
|------|------|------|------|---------|
| CompressionThreshold | 0.5-0.95 | 0.75 | 压缩阈值（路径下降比例） | +2-5% |
| MinPathsForCompression | 512-8192 | 2048 | 最小压缩路径数 | +1-3% |
| EnablePathSorting | true/false | true | 按材质排序路径 | +10-20% |
| EnableStreamCompaction | true/false | true | 移除终止路径 | +5-10% |

```ini
[Optimization]
CompressionThreshold = 0.75         # 平衡 ✓
CompressionThreshold = 0.60         # 复杂场景，更激进
CompressionThreshold = 0.80         # 简单场景，更少压缩

EnablePathSorting = true            # 多材质场景必须启用 ✓
EnablePathSorting = false           # 单一材质可禁用
```

---

### Kernel线程块

| Kernel | 推荐值 | 说明 |
|--------|--------|------|
| GenerateRaysBlockSize | 256 | 简单初始化 |
| ProcessHitsBlockSize | 128-256 | 寄存器压力大 |
| SampleLightsBlockSize | 128-256 | 计算密集 |
| SampleBSDFBlockSize | 192-256 | 平衡型 |
| AccumulateBlockSize | 256 | 简单累加 |

```ini
[KernelConfig]
# Turing (RTX 20系列)
ProcessHitsBlockSize = 128
SampleLightsBlockSize = 128
SampleBSDFBlockSize = 192

# Ampere/Ada (RTX 30/40系列)
ProcessHitsBlockSize = 256
SampleLightsBlockSize = 256
SampleBSDFBlockSize = 256
```

**规则**：BlockSize必须是32的倍数

---

### 早期终止

| 参数 | 范围 | 默认 | 说明 | 性能影响 |
|------|------|------|------|---------|
| EnableEarlyTermination | true/false | true | 是否启用 | +10-20% |
| Threshold | 0.001-0.1 | 0.01 | 终止阈值（活跃路径比例） | - |
| MinDepth | 0-32 | 10 | 最小深度 | - |

```ini
[EarlyTermination]
EnableEarlyTermination = true
Threshold = 0.01                    # 1%路径时终止 ✓
MinDepth = 10                       # 至少执行10次深度 ✓

# 更激进（复杂场景）
Threshold = 0.005                   # 0.5%路径时终止
MinDepth = 12

# 最终渲染（确保质量）
EnableEarlyTermination = false      # 禁用
```

---

## 📊 场景类型推荐

### 简单场景（单一材质）

```ini
[Optimization]
SyncInterval = 8
CompressionThreshold = 0.80
EnablePathSorting = false           # 不需要排序
[KernelConfig]
ProcessHitsBlockSize = 256          # 大block
[EarlyTermination]
Threshold = 0.05
MinDepth = 8
```

### 中等场景（3-5种材质）

```ini
[Optimization]
SyncInterval = 4
CompressionThreshold = 0.75
EnablePathSorting = true            # 启用排序 ✓
[KernelConfig]
ProcessHitsBlockSize = 128          # 默认
[EarlyTermination]
Threshold = 0.01
MinDepth = 10
```

### 复杂场景（10+种材质）

```ini
[Optimization]
SyncInterval = 4
CompressionThreshold = 0.60         # 更激进
EnablePathSorting = true            # 必须启用 ✓
[KernelConfig]
ProcessHitsBlockSize = 128          # 小block
SampleLightsBlockSize = 128
[EarlyTermination]
Threshold = 0.005                   # 更激进
MinDepth = 12
[Memory]
UseMaterialCache = true             # 启用缓存
```

---

## 🎯 调优速查

### 问题：渲染太慢

```ini
# 1. 减少采样和深度
[Render]
Samples = 16                        # 64 → 16
MaxDepth = 4                        # 8 → 4

# 2. 增加同步间隔
[Optimization]
SyncInterval = 8                    # 4 → 8

# 3. 启用早期终止
[EarlyTermination]
EnableEarlyTermination = true
Threshold = 0.02                    # 更激进
```

### 问题：GPU占用率低

```ini
# 1. 更激进的压缩
[Optimization]
CompressionThreshold = 0.60         # 0.75 → 0.60
EnableStreamCompaction = true       # 确保启用

# 2. 增大BlockSize
[KernelConfig]
ProcessHitsBlockSize = 256          # 128 → 256
```

### 问题：分支效率低

```ini
# 启用路径排序
[Optimization]
EnablePathSorting = true            # 必须启用
```

### 问题：图像有噪声

```ini
# 增加采样数
[Render]
Samples = 1024                      # 64 → 1024

# 禁用早期终止
[EarlyTermination]
EnableEarlyTermination = false
```

---

## 🔧 工具速查

### 验证配置

```bash
python scripts\validate_config.py my_config.ini
```

### 生成配置

```bash
# 为RTX 3080生成复杂场景配置
python scripts\generate_config.py --scene complex --gpu ampere --output my.ini
```

### 对比配置

```bash
python scripts\compare_configs.py config1.ini config2.ini
```

### 自动调优

```bash
python scripts\auto_tune.py --config benchmark.ini --output optimized.ini
```

---

## 📈 性能影响速查

| 优化 | 性能提升 | 内存开销 | 推荐 |
|------|---------|---------|------|
| SyncInterval: 4→8 | +5% | 0 | ✓ |
| CompressionThreshold: 0.75→0.60 | +2% | 0 | ✓ |
| EnablePathSorting | +10-20% | +1MB | ✓✓ |
| EnableStreamCompaction | +5-10% | +0.3MB | ✓ |
| EarlyTermination | +10-20% | 0 | ✓✓ |
| ProcessHitsBlockSize: 128→256 | -5% ~ +5% | 0 | GPU相关 |

---

## 💡 调优技巧

### 1. 从预设开始

```bash
# 选择最接近的预设
copy config_presets\benchmark.ini my_config.ini

# 微调参数
notepad my_config.ini
```

### 2. 逐个调整

```
一次只改一个参数 → 测试 → 记录结果 → 保留最优
```

### 3. 使用脚本

```bash
# 自动找最优配置
python scripts\auto_tune.py --config my_config.ini --output optimized.ini
```

---

## 🐛 调试速查

### 图像有NaN

```ini
[Debug]
EnableNaNTracking = true
ValidateQueues = true
```

### 性能分析

```ini
[Debug]
EnablePerfCounters = true
PrintKernelTiming = true
```

### 验证正确性

```ini
[Debug]
ValidateQueues = true
[EarlyTermination]
EnableEarlyTermination = false      # 禁用早期终止
```

---

## 📚 完整文档

- **配置指南**: [CONFIGURATION_GUIDE.md](CONFIGURATION_GUIDE.md)
- **脚本文档**: [../scripts/README.md](../scripts/README.md)
- **优化技术**: [../tools/06_optimizations.md](../tools/06_optimizations.md)

---

## 🎓 记住这些

1. **BlockSize必须是32的倍数**（warp大小）
2. **SyncInterval推荐4-8**（平衡性能和控制）
3. **多材质场景必须启用PathSorting**（+10-20%性能）
4. **最终渲染禁用EarlyTermination**（确保质量）
5. **修改配置无需重新编译**（直接运行）

---

**打印提示**：建议打印本页并放在桌面，方便随时查阅

---

**版本**: 1.0  
**更新日期**: 2026-03-07  
**作者**: VLR开发团队
