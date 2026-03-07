#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
VLR Wavefront 自动调优脚本

自动搜索最优性能参数配置

用法:
    python auto_tune.py --config benchmark.ini --output optimized.ini
    
作者: VLR开发团队
日期: 2026-03-07
"""

import subprocess
import re
import time
import argparse
from pathlib import Path
from typing import Dict, List, Tuple
import configparser


class PerformanceTuner:
    """性能自动调优器"""
    
    def __init__(self, exe_path: str, base_config: str):
        self.exe_path = Path(exe_path)
        self.base_config = Path(base_config)
        self.best_config = None
        self.best_time = float('inf')
        self.results = []
        
    def load_config(self, config_path: str) -> configparser.ConfigParser:
        """加载INI配置"""
        config = configparser.ConfigParser()
        config.read(config_path, encoding='utf-8')
        return config
        
    def save_config(self, config: configparser.ConfigParser, output_path: str):
        """保存INI配置"""
        with open(output_path, 'w', encoding='utf-8') as f:
            config.write(f)
            
    def run_benchmark(self, config_path: str) -> float:
        """运行基准测试，返回渲染时间（秒）"""
        try:
            result = subprocess.run(
                [str(self.exe_path), str(config_path)],
                capture_output=True,
                text=True,
                timeout=300
            )
            
            # 解析输出，提取渲染时间
            output = result.stdout
            
            # 匹配 "Total time: X.XXXs" 或 "Render complete! X.XXXs"
            time_match = re.search(r'Total time:\s*([\d.]+)s', output)
            if not time_match:
                time_match = re.search(r'Render complete!\s*([\d.]+)s', output)
            
            if time_match:
                return float(time_match.group(1))
            else:
                print(f"[Warning] Could not parse time from output")
                return float('inf')
                
        except subprocess.TimeoutExpired:
            print(f"[Error] Benchmark timeout")
            return float('inf')
        except Exception as e:
            print(f"[Error] Benchmark failed: {e}")
            return float('inf')
    
    def tune_parameter(self, param_section: str, param_name: str, 
                       values: List, config: configparser.ConfigParser) -> Tuple[any, float]:
        """调优单个参数"""
        print(f"\n{'='*60}")
        print(f"调优参数: [{param_section}] {param_name}")
        print(f"{'='*60}")
        
        best_value = None
        best_time = float('inf')
        
        for value in values:
            # 设置参数
            config.set(param_section, param_name, str(value))
            
            # 保存临时配置
            temp_config = "temp_tune.ini"
            self.save_config(config, temp_config)
            
            # 运行基准测试
            print(f"  测试 {param_name}={value}...", end=' ', flush=True)
            render_time = self.run_benchmark(temp_config)
            
            if render_time < float('inf'):
                print(f"{render_time:.3f}s", end='')
                
                if render_time < best_time:
                    best_time = render_time
                    best_value = value
                    print(f" ✓ 最优")
                else:
                    improvement = ((best_time - render_time) / best_time) * 100
                    print(f" ({improvement:+.1f}%)")
            else:
                print("失败")
            
            # 记录结果
            self.results.append({
                'section': param_section,
                'param': param_name,
                'value': value,
                'time': render_time
            })
        
        print(f"\n  最优值: {param_name}={best_value} ({best_time:.3f}s)")
        
        # 恢复最优值
        config.set(param_section, param_name, str(best_value))
        
        return best_value, best_time
    
    def auto_tune(self) -> configparser.ConfigParser:
        """自动调优所有参数"""
        print("\n" + "="*60)
        print("  VLR Wavefront 自动调优")
        print("="*60)
        print(f"  基准配置: {self.base_config}")
        print(f"  测试程序: {self.exe_path}")
        print("="*60)
        
        # 加载基准配置
        config = self.load_config(str(self.base_config))
        
        # 1. 调优同步间隔
        self.tune_parameter('Optimization', 'SyncInterval', 
                           [1, 2, 4, 8], config)
        
        # 2. 调优压缩阈值
        self.tune_parameter('Optimization', 'CompressionThreshold',
                           [0.50, 0.60, 0.70, 0.75, 0.80, 0.90], config)
        
        # 3. 调优ProcessHits BlockSize
        self.tune_parameter('KernelConfig', 'ProcessHitsBlockSize',
                           [128, 192, 256], config)
        
        # 4. 调优SampleLights BlockSize
        self.tune_parameter('KernelConfig', 'SampleLightsBlockSize',
                           [128, 192, 256], config)
        
        # 5. 调优SampleBSDF BlockSize
        self.tune_parameter('KernelConfig', 'SampleBSDFBlockSize',
                           [128, 192, 256], config)
        
        # 6. 调优早期终止阈值
        self.tune_parameter('EarlyTermination', 'Threshold',
                           [0.005, 0.01, 0.02, 0.05], config)
        
        # 7. 调优早期终止最小深度
        self.tune_parameter('EarlyTermination', 'MinDepth',
                           [8, 10, 12, 14], config)
        
        return config
    
    def print_summary(self):
        """打印调优摘要"""
        print("\n" + "="*60)
        print("  调优完成")
        print("="*60)
        
        # 按参数分组
        by_param = {}
        for result in self.results:
            key = f"[{result['section']}] {result['param']}"
            if key not in by_param:
                by_param[key] = []
            by_param[key].append((result['value'], result['time']))
        
        # 打印每个参数的最优值
        print("\n最优参数:")
        for param, values in by_param.items():
            best = min(values, key=lambda x: x[1])
            print(f"  {param}: {best[0]} ({best[1]:.3f}s)")
        
        print("="*60)


def main():
    parser = argparse.ArgumentParser(description='VLR Wavefront 自动调优')
    parser.add_argument('--exe', default='../bin/cornell_box_improved_test.exe',
                       help='测试程序路径')
    parser.add_argument('--config', default='../bin/config_presets/benchmark.ini',
                       help='基准配置文件')
    parser.add_argument('--output', default='optimized.ini',
                       help='输出配置文件')
    
    args = parser.parse_args()
    
    # 创建调优器
    tuner = PerformanceTuner(args.exe, args.config)
    
    # 运行自动调优
    start_time = time.time()
    optimized_config = tuner.auto_tune()
    elapsed = time.time() - start_time
    
    # 保存最优配置
    tuner.save_config(optimized_config, args.output)
    
    # 打印摘要
    tuner.print_summary()
    
    print(f"\n总耗时: {elapsed:.1f}秒")
    print(f"最优配置已保存到: {args.output}")
    print("\n使用最优配置:")
    print(f"  .\\cornell_box_improved_test.exe {args.output}")


if __name__ == '__main__':
    main()
