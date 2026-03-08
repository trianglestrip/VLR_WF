# VLR Wavefront 文档

> VLR 高性能 GPU 路径追踪渲染器的技术文档

---

## 📚 核心文档

| 文档 | 说明 | 适合读者 |
|-----|------|---------|
| **[GETTING_STARTED.md](GETTING_STARTED.md)** | 快速开始指南 | 新用户 |
| **[CONFIGURATION_GUIDE.md](CONFIGURATION_GUIDE.md)** | 配置系统完整指南 | 开发者 |
| **[WAVEFRONT_API.md](WAVEFRONT_API.md)** | API 参考手册 | 开发者 |
| **[TODO.md](TODO.md)** | 开发路线图 | 项目规划者 |
| **[OPTIMIZATION_SUMMARY.md](OPTIMIZATION_SUMMARY.md)** | 性能优化总结 | 性能工程师 |

**功能文档**: [DEBUG_MODE_IMPLEMENTATION.md](DEBUG_MODE_IMPLEMENTATION.md) | [MATERIAL_SYSTEM_ENHANCEMENT_SUMMARY.md](MATERIAL_SYSTEM_ENHANCEMENT_SUMMARY.md) | [RENDERING_TEST_RESULTS.md](RENDERING_TEST_RESULTS.md) | [DENOISER_INTEGRATION.md](DENOISER_INTEGRATION.md)

---

## 🚀 快速开始

### 新用户
1. 阅读 [GETTING_STARTED.md](GETTING_STARTED.md)
2. 查看 [../README.md](../README.md) 了解项目概览
3. 运行示例程序开始渲染

### 开发者
1. 阅读 [WAVEFRONT_API.md](WAVEFRONT_API.md) 了解 API
2. 阅读 [CONFIGURATION_GUIDE.md](CONFIGURATION_GUIDE.md) 了解配置系统
3. 查看 [OPTIMIZATION_SUMMARY.md](OPTIMIZATION_SUMMARY.md) 了解优化技术

### 项目规划
1. 阅读 [TODO.md](TODO.md) 了解开发路线图
2. 查看功能对比和优先级
3. 了解性能优化方向

---

## 📊 项目状态

**当前版本**: 1.0  
**性能**: 3.07x 加速（相比传统递归式路径追踪）  
**吞吐量**: 40.6 Msamples/s (RTX 2060 SUPER)  

**已完成优化**:
- ✅ 阶段 1: CPU-GPU 同步优化 + 路径压缩 (37.0% 提升)
- ✅ 阶段 2-3: 内存访问优化 (0.7% 提升)
- ✅ 阶段 4: 早期终止优化 (16.5% 提升)
- ✅ 阶段 5: RNG + Warp + CUB 优化 (2.3% 提升)

**下一步**: 查看 [TODO.md](TODO.md) 了解未来开发计划

---

## 🔗 相关资源

- **主项目**: [../README.md](../README.md)
- **配置预设**: [../bin/config_presets/](../bin/config_presets/)
- **源代码**: [../libVLR/](../libVLR/)
- **测试程序**: [../test/](../test/)
- **工具文档**: [../tools/](../tools/)

---

**最后更新**: 2026-03-07  
**维护者**: VLR 开发团队
