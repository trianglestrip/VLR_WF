#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
VLR Wavefront 配置对比工具

对比不同配置文件的性能差异

用法:
    python compare_configs.py config1.ini config2.ini config3.ini
    
作者: VLR开发团队
日期: 2026-03-07
"""

import subprocess
import re
import argparse
from pathlib import Path
from typing import Dict, List
import configparser


class ConfigComparator:
    """配置对比器"""
    
    def __init__(self, exe_path: str):
        self.exe_path = Path(exe_path)
        self.results = []
        
    def run_benchmark(self, config_path: str, runs: int = 3) -> Dict:
        """运行基准测试，返回性能数据"""
        print(f"\n测试配置: {config_path}")
        
        times = []
        throughputs = []
        
        for i in range(runs):
            print(f"  运行 {i+1}/{runs}...", end=' ', flush=True)
            
            try:
                result = subprocess.run(
                    [str(self.exe_path), str(config_path)],
                    capture_output=True,
                    text=True,
                    timeout=300
                )
                
                output = result.stdout
                
                # 提取渲染时间
                time_match = re.search(r'Total time:\s*([\d.]+)s', output)
                if time_match:
                    render_time = float(time_match.group(1))
                    times.append(render_time)
                    print(f"{render_time:.3f}s", end='')
                
                # 提取吞吐量
                throughput_match = re.search(r'Throughput:\s*([\d.]+)\s*Msamp/s', output)
                if throughput_match:
                    throughput = float(throughput_match.group(1))
                    throughputs.append(throughput)
                    print(f" ({throughput:.1f} Msamp/s)")
                else:
                    print()
                    
            except subprocess.TimeoutExpired:
                print("超时")
            except Exception as e:
                print(f"失败: {e}")
        
        if not times:
            return None
        
        # 计算平均值
        avg_time = sum(times) / len(times)
        avg_throughput = sum(throughputs) / len(throughputs) if throughputs else 0
        
        # 加载配置信息
        config = configparser.ConfigParser()
        config.read(config_path, encoding='utf-8')
        
        return {
            'config_path': config_path,
            'avg_time': avg_time,
            'min_time': min(times),
            'max_time': max(times),
            'avg_throughput': avg_throughput,
            'sync_interval': config.getint('Optimization', 'SyncInterval', fallback=4),
            'compression_threshold': config.getfloat('Optimization', 'CompressionThreshold', fallback=0.75),
            'path_sorting': config.getboolean('Optimization', 'EnablePathSorting', fallback=True),
            'early_termination': config.getboolean('EarlyTermination', 'EnableEarlyTermination', fallback=True),
            'samples': config.getint('Render', 'Samples', fallback=64),
            'resolution': f"{config.getint('Render', 'Width', fallback=512)}×{config.getint('Render', 'Height', fallback=512)}"
        }
    
    def compare(self, config_paths: List[str], runs: int = 3):
        """对比多个配置"""
        print("\n" + "="*70)
        print("  VLR Wavefront 配置性能对比")
        print("="*70)
        
        # 运行所有配置
        for config_path in config_paths:
            result = self.run_benchmark(config_path, runs)
            if result:
                self.results.append(result)
        
        if not self.results:
            print("\n[Error] 没有成功的测试结果")
            return
        
        # 找到最快的配置
        fastest = min(self.results, key=lambda x: x['avg_time'])
        
        # 打印对比表
        self.print_comparison_table(fastest)
        
    def print_comparison_table(self, baseline):
        """打印对比表"""
        print("\n" + "="*70)
        print("  性能对比结果")
        print("="*70)
        
        # 表头
        print(f"\n{'配置文件':<30} {'时间':<12} {'吞吐量':<15} {'相对性能':<10}")
        print("-" * 70)
        
        # 按性能排序
        sorted_results = sorted(self.results, key=lambda x: x['avg_time'])
        
        for result in sorted_results:
            config_name = Path(result['config_path']).name
            time_str = f"{result['avg_time']:.3f}s"
            throughput_str = f"{result['avg_throughput']:.1f} Msamp/s"
            
            # 计算相对性能
            speedup = baseline['avg_time'] / result['avg_time']
            if speedup >= 1.0:
                speedup_str = f"{speedup:.2f}x ✓"
            else:
                speedup_str = f"{speedup:.2f}x"
            
            print(f"{config_name:<30} {time_str:<12} {throughput_str:<15} {speedup_str:<10}")
        
        # 打印关键参数对比
        print("\n" + "="*70)
        print("  关键参数对比")
        print("="*70)
        
        print(f"\n{'配置':<20} {'同步间隔':<10} {'压缩阈值':<12} {'路径排序':<10} {'早期终止':<10}")
        print("-" * 70)
        
        for result in sorted_results:
            config_name = Path(result['config_path']).stem
            sync = result['sync_interval']
            comp = result['compression_threshold']
            sort_enabled = "启用" if result['path_sorting'] else "禁用"
            early_term = "启用" if result['early_termination'] else "禁用"
            
            print(f"{config_name:<20} {sync:<10} {comp:<12.2f} {sort_enabled:<10} {early_term:<10}")
        
        print("\n" + "="*70)
        print(f"  最快配置: {Path(baseline['config_path']).name}")
        print(f"  渲染时间: {baseline['avg_time']:.3f}s")
        print(f"  吞吐量: {baseline['avg_throughput']:.1f} Msamp/s")
        print("="*70)


def main():
    parser = argparse.ArgumentParser(description='VLR Wavefront 配置对比')
    parser.add_argument('configs', nargs='+', help='要对比的配置文件')
    parser.add_argument('--exe', default='../bin/cornell_box_improved_test.exe',
                       help='测试程序路径')
    parser.add_argument('--runs', type=int, default=3,
                       help='每个配置运行次数（取平均）')
    
    args = parser.parse_args()
    
    # 创建对比器
    comparator = ConfigComparator(args.exe)
    
    # 运行对比
    comparator.compare(args.configs, args.runs)


if __name__ == '__main__':
    main()
