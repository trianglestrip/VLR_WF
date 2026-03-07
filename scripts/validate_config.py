#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
VLR Wavefront 配置验证工具

验证配置文件的有效性和合理性

用法:
    python validate_config.py config.ini
    
作者: VLR开发团队
日期: 2026-03-07
"""

import argparse
import configparser
from pathlib import Path
from typing import List, Tuple


class ConfigValidator:
    """配置验证器"""
    
    def __init__(self):
        self.warnings = []
        self.errors = []
        
    def validate(self, config_path: str) -> bool:
        """验证配置文件"""
        print("\n" + "="*60)
        print(f"  验证配置: {config_path}")
        print("="*60)
        
        # 加载配置
        config = configparser.ConfigParser()
        try:
            config.read(config_path, encoding='utf-8')
        except Exception as e:
            self.errors.append(f"无法读取配置文件: {e}")
            return False
        
        # 验证各个节
        self.validate_render(config)
        self.validate_optimization(config)
        self.validate_kernel_config(config)
        self.validate_early_termination(config)
        self.validate_memory(config)
        self.validate_advanced(config)
        
        # 打印结果
        self.print_results()
        
        return len(self.errors) == 0
    
    def validate_render(self, config: configparser.ConfigParser):
        """验证[Render]节"""
        section = 'Render'
        
        # Width
        width = config.getint(section, 'Width', fallback=512)
        if width < 64 or width > 8192:
            self.warnings.append(f"[{section}] Width={width} 超出推荐范围 [64, 8192]")
        if width % 32 != 0:
            self.warnings.append(f"[{section}] Width={width} 不是32的倍数，可能影响性能")
        
        # Height
        height = config.getint(section, 'Height', fallback=512)
        if height < 64 or height > 8192:
            self.warnings.append(f"[{section}] Height={height} 超出推荐范围 [64, 8192]")
        if height % 32 != 0:
            self.warnings.append(f"[{section}] Height={height} 不是32的倍数，可能影响性能")
        
        # Samples
        samples = config.getint(section, 'Samples', fallback=64)
        if samples < 1:
            self.errors.append(f"[{section}] Samples={samples} 必须 >= 1")
        if samples > 10000:
            self.warnings.append(f"[{section}] Samples={samples} 非常大，渲染时间会很长")
        
        # MaxDepth
        max_depth = config.getint(section, 'MaxDepth', fallback=8)
        if max_depth < 1:
            self.errors.append(f"[{section}] MaxDepth={max_depth} 必须 >= 1")
        if max_depth > 32:
            self.warnings.append(f"[{section}] MaxDepth={max_depth} 过大，可能浪费计算")
        
        # Exposure
        exposure = config.getfloat(section, 'Exposure', fallback=1.0)
        if exposure <= 0:
            self.errors.append(f"[{section}] Exposure={exposure} 必须 > 0")
        if exposure > 10.0:
            self.warnings.append(f"[{section}] Exposure={exposure} 过大，图像可能过曝")
    
    def validate_optimization(self, config: configparser.ConfigParser):
        """验证[Optimization]节"""
        section = 'Optimization'
        
        # SyncInterval
        sync_interval = config.getint(section, 'SyncInterval', fallback=4)
        if sync_interval < 1:
            self.errors.append(f"[{section}] SyncInterval={sync_interval} 必须 >= 1")
        if sync_interval > 16:
            self.warnings.append(f"[{section}] SyncInterval={sync_interval} 过大，可能浪费计算")
        
        # CompressionThreshold
        comp_threshold = config.getfloat(section, 'CompressionThreshold', fallback=0.75)
        if comp_threshold < 0.0 or comp_threshold > 1.0:
            self.errors.append(f"[{section}] CompressionThreshold={comp_threshold} 必须在 [0.0, 1.0]")
        if comp_threshold < 0.5:
            self.warnings.append(f"[{section}] CompressionThreshold={comp_threshold} 过小，压缩过于频繁")
        if comp_threshold > 0.95:
            self.warnings.append(f"[{section}] CompressionThreshold={comp_threshold} 过大，压缩不够频繁")
        
        # MinPathsForCompression
        min_paths = config.getint(section, 'MinPathsForCompression', fallback=2048)
        if min_paths < 0:
            self.errors.append(f"[{section}] MinPathsForCompression={min_paths} 必须 >= 0")
        if min_paths < 512:
            self.warnings.append(f"[{section}] MinPathsForCompression={min_paths} 过小，可能频繁压缩小队列")
    
    def validate_kernel_config(self, config: configparser.ConfigParser):
        """验证[KernelConfig]节"""
        section = 'KernelConfig'
        
        kernel_names = [
            'GenerateRaysBlockSize',
            'ProcessHitsBlockSize',
            'SampleLightsBlockSize',
            'SampleBSDFBlockSize',
            'AccumulateBlockSize'
        ]
        
        for kernel_name in kernel_names:
            block_size = config.getint(section, kernel_name, fallback=256)
            
            # BlockSize必须是32的倍数（warp大小）
            if block_size % 32 != 0:
                self.errors.append(f"[{section}] {kernel_name}={block_size} 必须是32的倍数")
            
            # BlockSize范围检查
            if block_size < 32 or block_size > 1024:
                self.errors.append(f"[{section}] {kernel_name}={block_size} 必须在 [32, 1024]")
            
            # 推荐范围
            if block_size < 128 or block_size > 256:
                self.warnings.append(f"[{section}] {kernel_name}={block_size} 超出推荐范围 [128, 256]")
    
    def validate_early_termination(self, config: configparser.ConfigParser):
        """验证[EarlyTermination]节"""
        section = 'EarlyTermination'
        
        # Threshold
        threshold = config.getfloat(section, 'Threshold', fallback=0.01)
        if threshold < 0.0 or threshold > 1.0:
            self.errors.append(f"[{section}] Threshold={threshold} 必须在 [0.0, 1.0]")
        if threshold > 0.1:
            self.warnings.append(f"[{section}] Threshold={threshold} 过大，可能过早终止")
        
        # MinDepth
        min_depth = config.getint(section, 'MinDepth', fallback=10)
        if min_depth < 0:
            self.errors.append(f"[{section}] MinDepth={min_depth} 必须 >= 0")
        
        # 检查与MaxDepth的关系
        max_depth = config.getint('Render', 'MaxDepth', fallback=8)
        if min_depth >= max_depth:
            self.warnings.append(f"[{section}] MinDepth={min_depth} >= MaxDepth={max_depth}，早期终止永远不会触发")
    
    def validate_memory(self, config: configparser.ConfigParser):
        """验证[Memory]节"""
        section = 'Memory'
        
        # UseMaterialCache
        use_cache = config.getboolean(section, 'UseMaterialCache', fallback=False)
        if use_cache:
            # 检查block size是否会导致occupancy下降
            process_block = config.getint('KernelConfig', 'ProcessHitsBlockSize', fallback=128)
            if process_block > 192:
                self.warnings.append(f"[{section}] UseMaterialCache=true 且 ProcessHitsBlockSize={process_block} 可能导致occupancy下降")
    
    def validate_advanced(self, config: configparser.ConfigParser):
        """验证[Advanced]节"""
        section = 'Advanced'
        
        # UseCudaGraphs
        use_graphs = config.getboolean(section, 'UseCudaGraphs', fallback=False)
        if use_graphs:
            self.warnings.append(f"[{section}] UseCudaGraphs=true 对Wavefront动态工作负载效果有限")
        
        # UseFusedKernels
        use_fused = config.getboolean(section, 'UseFusedKernels', fallback=True)
        if use_fused:
            # 检查是否会增加寄存器压力
            process_block = config.getint('KernelConfig', 'ProcessHitsBlockSize', fallback=128)
            if process_block < 192:
                self.warnings.append(f"[{section}] UseFusedKernels=true 可能增加寄存器压力，建议 ProcessHitsBlockSize >= 192")
    
    def print_results(self):
        """打印验证结果"""
        print("\n" + "="*60)
        print("  验证结果")
        print("="*60)
        
        if not self.errors and not self.warnings:
            print("\n✓ 配置文件有效，没有发现问题")
        else:
            if self.errors:
                print(f"\n❌ 发现 {len(self.errors)} 个错误:")
                for i, error in enumerate(self.errors, 1):
                    print(f"  {i}. {error}")
            
            if self.warnings:
                print(f"\n⚠️  发现 {len(self.warnings)} 个警告:")
                for i, warning in enumerate(self.warnings, 1):
                    print(f"  {i}. {warning}")
        
        print("\n" + "="*60)
        
        if self.errors:
            print("  状态: 配置无效，请修复错误")
        elif self.warnings:
            print("  状态: 配置有效，但建议检查警告")
        else:
            print("  状态: 配置完美 ✓")
        print("="*60)


def main():
    parser = argparse.ArgumentParser(description='VLR Wavefront 配置验证')
    parser.add_argument('config', help='要验证的配置文件')
    
    args = parser.parse_args()
    
    # 检查文件是否存在
    config_path = Path(args.config)
    if not config_path.exists():
        print(f"\n[Error] 配置文件不存在: {config_path}")
        return 1
    
    # 创建验证器
    validator = ConfigValidator()
    
    # 验证配置
    is_valid = validator.validate(str(config_path))
    
    return 0 if is_valid else 1


if __name__ == '__main__':
    exit(main())
