#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
VLR Wavefront 配置生成器

根据场景类型和GPU架构生成优化配置

用法:
    python generate_config.py --scene complex --gpu ampere --output my_config.ini
    
作者: VLR开发团队
日期: 2026-03-07
"""

import argparse
from pathlib import Path
from typing import Dict


class ConfigGenerator:
    """配置生成器"""
    
    # 场景预设
    SCENE_PRESETS = {
        'simple': {
            'name': '简单场景',
            'description': '单一材质，小光源，简单几何',
            'sync_interval': 8,
            'compression_threshold': 0.80,
            'enable_path_sorting': False,
            'process_hits_block': 256,
            'sample_lights_block': 256,
            'sample_bsdf_block': 256,
            'early_term_threshold': 0.05,
            'early_term_min_depth': 8
        },
        'medium': {
            'name': '中等场景',
            'description': '3-5种材质，多个光源',
            'sync_interval': 4,
            'compression_threshold': 0.75,
            'enable_path_sorting': True,
            'process_hits_block': 128,
            'sample_lights_block': 256,
            'sample_bsdf_block': 192,
            'early_term_threshold': 0.01,
            'early_term_min_depth': 10
        },
        'complex': {
            'name': '复杂场景',
            'description': '10+种材质，大量几何',
            'sync_interval': 4,
            'compression_threshold': 0.60,
            'enable_path_sorting': True,
            'process_hits_block': 128,
            'sample_lights_block': 128,
            'sample_bsdf_block': 192,
            'early_term_threshold': 0.005,
            'early_term_min_depth': 12
        }
    }
    
    # GPU架构预设
    GPU_PRESETS = {
        'turing': {
            'name': 'Turing (RTX 20系列)',
            'compute': '7.5',
            'process_hits_block': 128,
            'sample_lights_block': 128,
            'sample_bsdf_block': 192,
            'use_material_cache': False
        },
        'ampere': {
            'name': 'Ampere (RTX 30系列)',
            'compute': '8.0/8.6',
            'process_hits_block': 256,
            'sample_lights_block': 256,
            'sample_bsdf_block': 256,
            'use_material_cache': True
        },
        'ada': {
            'name': 'Ada (RTX 40系列)',
            'compute': '8.9',
            'process_hits_block': 256,
            'sample_lights_block': 256,
            'sample_bsdf_block': 256,
            'use_material_cache': True
        }
    }
    
    def generate(self, scene_type: str, gpu_arch: str, 
                 width: int = 512, height: int = 512,
                 samples: int = 64, max_depth: int = 8) -> str:
        """生成配置文件内容"""
        
        scene = self.SCENE_PRESETS.get(scene_type, self.SCENE_PRESETS['medium'])
        gpu = self.GPU_PRESETS.get(gpu_arch, self.GPU_PRESETS['ampere'])
        
        # 合并场景和GPU配置（GPU配置优先）
        process_block = gpu['process_hits_block']
        sample_lights_block = gpu['sample_lights_block']
        sample_bsdf_block = gpu['sample_bsdf_block']
        
        config_content = f"""# VLR_WF 自动生成配置
# 场景类型: {scene['name']} ({scene['description']})
# GPU架构: {gpu['name']} (Compute {gpu['compute']})
# 生成日期: 2026-03-07

[Render]
Width = {width}
Height = {height}
Samples = {samples}
MaxDepth = {max_depth}
Exposure = 1.0

[Output]
Filename = output.png
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

[Performance]
DeviceID = 0
VerboseLogging = true

# ============================================================================
# 优化参数 - 针对 {scene['name']} 优化
# ============================================================================

[Optimization]
SyncInterval = {scene['sync_interval']}
CompressionThreshold = {scene['compression_threshold']}
MinPathsForCompression = 2048
EnablePathSorting = {str(scene['enable_path_sorting']).lower()}
EnableStreamCompaction = true

# ============================================================================
# Kernel配置 - 针对 {gpu['name']} 优化
# ============================================================================

[KernelConfig]
GenerateRaysBlockSize = 256
ProcessHitsBlockSize = {process_block}
SampleLightsBlockSize = {sample_lights_block}
SampleBSDFBlockSize = {sample_bsdf_block}
AccumulateBlockSize = 256

# ============================================================================
# 早期终止 - 针对 {scene['name']} 优化
# ============================================================================

[EarlyTermination]
EnableEarlyTermination = true
Threshold = {scene['early_term_threshold']}
MinDepth = {scene['early_term_min_depth']}

# ============================================================================
# 内存优化 - 针对 {gpu['name']} 优化
# ============================================================================

[Memory]
UseRestrictPointers = true
UseMaterialCache = {str(gpu['use_material_cache']).lower()}
UseTextureMemory = false

# ============================================================================
# 高级优化
# ============================================================================

[Advanced]
UseCudaGraphs = false
UseWarpOptimizations = true
UsePrefetching = false
UseFusedKernels = true
UseDynamicMaxDepth = false
UseAdaptiveSampling = false

# ============================================================================
# 调试选项
# ============================================================================

[Debug]
EnableNaNTracking = false
EnablePerfCounters = false
PrintKernelTiming = false
ValidateQueues = false
"""
        return config_content
    
    def save_config(self, content: str, output_path: str):
        """保存配置文件"""
        with open(output_path, 'w', encoding='utf-8') as f:
            f.write(content)
        print(f"\n✓ 配置已保存到: {output_path}")


def main():
    parser = argparse.ArgumentParser(
        description='VLR Wavefront 配置生成器',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
示例:
  # 为RTX 3080生成复杂场景配置
  python generate_config.py --scene complex --gpu ampere --output my_config.ini
  
  # 为RTX 2060生成简单场景配置
  python generate_config.py --scene simple --gpu turing --output simple.ini
  
  # 高质量渲染配置
  python generate_config.py --scene medium --gpu ampere --samples 2048 --output hq.ini

场景类型:
  simple  - 简单场景（单一材质，小光源）
  medium  - 中等场景（3-5种材质，多个光源）
  complex - 复杂场景（10+种材质，大量几何）

GPU架构:
  turing  - RTX 20系列 (Compute 7.5)
  ampere  - RTX 30系列 (Compute 8.0/8.6)
  ada     - RTX 40系列 (Compute 8.9)
        """
    )
    
    parser.add_argument('--scene', choices=['simple', 'medium', 'complex'],
                       default='medium', help='场景类型')
    parser.add_argument('--gpu', choices=['turing', 'ampere', 'ada'],
                       default='ampere', help='GPU架构')
    parser.add_argument('--width', type=int, default=512, help='图像宽度')
    parser.add_argument('--height', type=int, default=512, help='图像高度')
    parser.add_argument('--samples', type=int, default=64, help='采样数')
    parser.add_argument('--max-depth', type=int, default=8, help='最大深度')
    parser.add_argument('--output', default='generated_config.ini', help='输出文件')
    
    args = parser.parse_args()
    
    # 创建生成器
    generator = ConfigGenerator()
    
    # 显示配置信息
    scene = generator.SCENE_PRESETS[args.scene]
    gpu = generator.GPU_PRESETS[args.gpu]
    
    print("\n" + "="*60)
    print("  VLR Wavefront 配置生成器")
    print("="*60)
    print(f"  场景类型: {scene['name']}")
    print(f"  场景描述: {scene['description']}")
    print(f"  GPU架构: {gpu['name']} (Compute {gpu['compute']})")
    print(f"  分辨率: {args.width}×{args.height}")
    print(f"  采样数: {args.samples}")
    print(f"  最大深度: {args.max_depth}")
    print("="*60)
    
    # 生成配置
    config_content = generator.generate(
        args.scene, args.gpu,
        args.width, args.height,
        args.samples, args.max_depth
    )
    
    # 保存配置
    generator.save_config(config_content, args.output)
    
    print("\n使用生成的配置:")
    print(f"  .\\cornell_box_improved_test.exe {args.output}")


if __name__ == '__main__':
    main()
