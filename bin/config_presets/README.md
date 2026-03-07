# 配置预设

本目录包含针对不同使用场景的预设配置文件。

## 📁 预设列表

### preview.ini - 快速预览

**用途**：快速迭代、场景调试、实时预览

**特点**：
- 低分辨率（512×512）
- 低采样数（16）
- 低深度（4）
- 禁用排序优化
- 激进的早期终止

**性能**：~0.2秒

**使用**：
```bash
.\cornell_box_improved_test.exe config_presets\preview.ini
```

---

### high_quality.ini - 高质量渲染

**用途**：最终输出、照片级渲染、展示

**特点**：
- 高分辨率（1920×1080）
- 高采样数（2048）
- 高深度（16）
- 禁用早期终止（确保质量）
- 所有优化启用

**性能**：~120秒

**使用**：
```bash
.\cornell_box_improved_test.exe config_presets\high_quality.ini
```

---

### benchmark.ini - 性能基准测试

**用途**：性能测试、对比分析、优化验证

**特点**：
- 标准分辨率（512×512）
- 标准采样数（1024）
- 启用性能计数器
- 打印详细耗时
- 默认优化配置

**性能**：~8秒

**使用**：
```bash
.\cornell_box_improved_test.exe config_presets\benchmark.ini > benchmark_result.txt
```

---

### debug.ini - 调试配置

**用途**：开发调试、问题排查、验证正确性

**特点**：
- 极小分辨率（256×256）
- 极少采样（4）
- 每次深度都同步
- 启用所有调试选项
- 禁用融合kernel（便于单步调试）

**性能**：~0.05秒

**使用**：
```bash
.\cornell_box_improved_test.exe config_presets\debug.ini
```

---

## 🎯 快速选择

```
我想...                           使用配置
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
快速查看场景效果                  preview.ini
调整场景参数并快速预览            preview.ini
进行性能测试和对比                benchmark.ini
生成最终的高质量图像              high_quality.ini
调试代码或排查问题                debug.ini
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

### 基于预设修改

```bash
# 1. 复制预设
copy config_presets\benchmark.ini my_config.ini

# 2. 编辑参数
notepad my_config.ini

# 3. 使用自定义配置
.\cornell_box_improved_test.exe my_config.ini
```

### 常见修改

**提高质量**：
```ini
Samples = 2048      # 增加采样
MaxDepth = 16       # 增加深度
```

**提高速度**：
```ini
SyncInterval = 8                    # 减少同步
CompressionThreshold = 0.80         # 减少压缩
EnableEarlyTermination = true       # 启用早期终止
Threshold = 0.02                    # 更激进的终止
```

**调试特定像素**：
```ini
Width = 1
Height = 1
Samples = 1
[Debug]
EnableNaNTracking = true
PrintKernelTiming = true
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
