#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
VLR Wavefront 配置拆分工具

将合并的配置文件拆分为场景配置和性能配置

用法:
    python split_config.py input.ini scene_output.ini performance_output.ini
    
作者: VLR开发团队
日期: 2026-03-07
"""

import argparse
import configparser
from pathlib import Path


class ConfigSplitter:
    """配置拆分器"""
    
    # 场景相关的节
    SCENE_SECTIONS = ['Render', 'Output', 'Camera', 'Scene']
    
    # 性能相关的节
    PERFORMANCE_SECTIONS = [
        'Optimization', 'KernelConfig', 'EarlyTermination',
        'Memory', 'Advanced', 'Debug', 'Device', 'Performance'
    ]
    
    def split(self, input_file: str, scene_file: str, perf_file: str):
        """拆分配置文件"""
        print("\n" + "="*60)
        print("  VLR Wavefront 配置拆分工具")
        print("="*60)
        print(f"  输入: {input_file}")
        print(f"  场景配置: {scene_file}")
        print(f"  性能配置: {perf_file}")
        print("="*60)
        
        # 加载输入配置
        config = configparser.ConfigParser()
        try:
            config.read(input_file, encoding='utf-8')
        except Exception as e:
            print(f"\n[Error] 无法读取配置文件: {e}")
            return False
        
        # 创建场景配置
        scene_config = configparser.ConfigParser()
        scene_count = 0
        
        for section in self.SCENE_SECTIONS:
            if config.has_section(section):
                scene_config.add_section(section)
                for key, value in config.items(section):
                    if key != 'DEFAULT':  # 跳过默认节
                        scene_config.set(section, key, value)
                        scene_count += 1
        
        # 创建性能配置
        perf_config = configparser.ConfigParser()
        perf_count = 0
        
        for section in self.PERFORMANCE_SECTIONS:
            if config.has_section(section):
                perf_config.add_section(section)
                for key, value in config.items(section):
                    if key != 'DEFAULT':
                        perf_config.set(section, key, value)
                        perf_count += 1
        
        # 特殊处理：[Performance]节中的DeviceID和VerboseLogging移到[Device]节
        if config.has_section('Performance'):
            if not perf_config.has_section('Device'):
                perf_config.add_section('Device')
            
            if config.has_option('Performance', 'DeviceID'):
                device_id = config.get('Performance', 'DeviceID')
                perf_config.set('Device', 'DeviceID', device_id)
            
            if config.has_option('Performance', 'VerboseLogging'):
                verbose = config.get('Performance', 'VerboseLogging')
                perf_config.set('Device', 'VerboseLogging', verbose)
        
        # 保存场景配置
        try:
            with open(scene_file, 'w', encoding='utf-8') as f:
                f.write(f"# VLR_WF 场景配置\n")
                f.write(f"# 从 {Path(input_file).name} 拆分\n")
                f.write(f"# 生成日期: 2026-03-07\n\n")
                scene_config.write(f)
            print(f"\n✓ 场景配置已保存: {scene_file} ({scene_count} 个参数)")
        except Exception as e:
            print(f"\n[Error] 无法保存场景配置: {e}")
            return False
        
        # 保存性能配置
        try:
            with open(perf_file, 'w', encoding='utf-8') as f:
                f.write(f"# VLR_WF 性能配置\n")
                f.write(f"# 从 {Path(input_file).name} 拆分\n")
                f.write(f"# 生成日期: 2026-03-07\n\n")
                perf_config.write(f)
            print(f"✓ 性能配置已保存: {perf_file} ({perf_count} 个参数)")
        except Exception as e:
            print(f"\n[Error] 无法保存性能配置: {e}")
            return False
        
        # 打印摘要
        self.print_summary(scene_config, perf_config)
        
        return True
    
    def print_summary(self, scene_config, perf_config):
        """打印拆分摘要"""
        print("\n" + "="*60)
        print("  拆分摘要")
        print("="*60)
        
        print("\n场景配置包含:")
        for section in scene_config.sections():
            params = len(scene_config.items(section))
            print(f"  [{section}]: {params} 个参数")
        
        print("\n性能配置包含:")
        for section in perf_config.sections():
            params = len(perf_config.items(section))
            print(f"  [{section}]: {params} 个参数")
        
        print("\n" + "="*60)
        print("  拆分完成 ✓")
        print("="*60)
        
        print("\n使用拆分后的配置:")
        print("  .\\cornell_box_improved_test.exe <scene_file> <perf_file>")


def main():
    parser = argparse.ArgumentParser(
        description='VLR Wavefront 配置拆分工具',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
示例:
  # 拆分合并的配置文件
  python split_config.py render_config.ini my_scene.ini my_performance.ini
  
  # 拆分预设配置
  python split_config.py config_presets\\benchmark.ini benchmark_scene.ini benchmark_perf.ini
        """
    )
    
    parser.add_argument('input', help='输入的合并配置文件')
    parser.add_argument('scene_output', help='输出的场景配置文件')
    parser.add_argument('performance_output', help='输出的性能配置文件')
    
    args = parser.parse_args()
    
    # 检查输入文件
    if not Path(args.input).exists():
        print(f"\n[Error] 输入文件不存在: {args.input}")
        return 1
    
    # 创建拆分器
    splitter = ConfigSplitter()
    
    # 执行拆分
    success = splitter.split(args.input, args.scene_output, args.performance_output)
    
    return 0 if success else 1


if __name__ == '__main__':
    exit(main())
