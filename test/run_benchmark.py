#!/usr/bin/env python3
"""
VLR Wavefront Optimization Benchmark Script

This script runs the simple_render_test multiple times with different
optimization configurations to measure performance improvements.

Author: VLR Development Team
Created: 2026-03-07
"""

import subprocess
import time
import statistics
import sys
import os

# ============================================================================
# Configuration
# ============================================================================

EXECUTABLE = "bin/simple_render_test.exe"
NUM_RUNS = 3  # Number of runs per configuration for averaging

# Test configurations (modify Context defaults before each run)
CONFIGS = [
    {"name": "Baseline (Simple Swap)",     "sorting": False, "compaction": False},
    {"name": "Path Sorting Only",          "sorting": True,  "compaction": False},
    {"name": "Stream Compaction Only",     "sorting": False, "compaction": True},
]

# ============================================================================
# Helper Functions
# ============================================================================

def run_test():
    """Run the simple_render_test and measure execution time."""
    start = time.time()
    
    try:
        result = subprocess.run(
            [EXECUTABLE],
            cwd="bin",
            capture_output=True,
            text=True,
            timeout=60
        )
        
        elapsed = time.time() - start
        
        if result.returncode != 0:
            print(f"[Error] Test failed with exit code {result.returncode}")
            print(f"stderr: {result.stderr}")
            return -1.0
        
        return elapsed * 1000.0  # Convert to milliseconds
        
    except subprocess.TimeoutExpired:
        print("[Error] Test timed out")
        return -1.0
    except Exception as e:
        print(f"[Error] Exception: {e}")
        return -1.0


def modify_config(use_sorting, use_compaction):
    """
    Modify the Context default configuration by editing shared/path_types.h
    This is a hack for benchmarking; in production, use API functions.
    """
    config_file = "libVLR/shared/path_types.h"
    
    try:
        with open(config_file, 'r', encoding='utf-8') as f:
            content = f.read()
        
        # Find and replace UsePathSorting
        import re
        content = re.sub(
            r'(constexpr bool UsePathSorting\s*=\s*)(true|false)',
            r'\1' + ('true' if use_sorting else 'false'),
            content
        )
        
        # Find and replace UseStreamCompaction
        content = re.sub(
            r'(constexpr bool UseStreamCompaction\s*=\s*)(true|false)',
            r'\1' + ('true' if use_compaction else 'false'),
            content
        )
        
        with open(config_file, 'w', encoding='utf-8') as f:
            f.write(content)
        
        print(f"[Config] Set UsePathSorting={use_sorting}, UseStreamCompaction={use_compaction}")
        return True
        
    except Exception as e:
        print(f"[Error] Failed to modify config: {e}")
        return False


def rebuild():
    """Rebuild the VLR library and test executable."""
    print("[Build] Rebuilding VLR...")
    
    try:
        result = subprocess.run(
            ["cmake", "--build", "build", "--config", "Release", "--target", "VLR"],
            capture_output=True,
            text=True,
            timeout=120
        )
        
        if result.returncode != 0:
            print(f"[Error] VLR build failed: {result.stderr}")
            return False
        
        result = subprocess.run(
            ["cmake", "--build", "build", "--config", "Release", "--target", "simple_render_test"],
            capture_output=True,
            text=True,
            timeout=120
        )
        
        if result.returncode != 0:
            print(f"[Error] Test build failed: {result.stderr}")
            return False
        
        print("[Build] Build successful")
        return True
        
    except Exception as e:
        print(f"[Error] Build exception: {e}")
        return False


# ============================================================================
# Main Benchmark
# ============================================================================

def main():
    print("=" * 60)
    print("VLR Wavefront Optimization Benchmark")
    print("=" * 60)
    print(f"Executable: {EXECUTABLE}")
    print(f"Runs per config: {NUM_RUNS}")
    print(f"Configurations: {len(CONFIGS)}")
    print()
    
    if not os.path.exists(EXECUTABLE):
        print(f"[Error] Executable not found: {EXECUTABLE}")
        print("Please build the project first.")
        return 1
    
    results = []
    
    for config in CONFIGS:
        print("\n" + "=" * 60)
        print(f"Testing: {config['name']}")
        print("=" * 60)
        
        # Modify configuration
        if not modify_config(config["sorting"], config["compaction"]):
            print("[Error] Failed to modify configuration")
            return 1
        
        # Rebuild
        if not rebuild():
            print("[Error] Build failed")
            return 1
        
        # Run multiple times
        times = []
        for run in range(NUM_RUNS):
            print(f"\n[Run {run + 1}/{NUM_RUNS}]")
            elapsed = run_test()
            
            if elapsed < 0:
                print(f"[Error] Run {run + 1} failed")
                return 1
            
            times.append(elapsed)
            print(f"[Result] Time: {elapsed:.2f} ms")
        
        # Calculate statistics
        avg_time = statistics.mean(times)
        std_dev = statistics.stdev(times) if len(times) > 1 else 0.0
        
        results.append({
            "name": config["name"],
            "avg": avg_time,
            "std": std_dev,
            "times": times
        })
        
        print(f"\n[Summary] Average: {avg_time:.2f} ms, StdDev: {std_dev:.2f} ms")
    
    # Print final summary
    print("\n" + "=" * 60)
    print("BENCHMARK SUMMARY")
    print("=" * 60)
    print(f"{'Configuration':<30} | {'Avg (ms)':>10} | {'StdDev':>8} | {'Speedup':>8}")
    print("-" * 60)
    
    baseline = results[0]["avg"]
    for r in results:
        speedup = baseline / r["avg"]
        print(f"{r['name']:<30} | {r['avg']:>10.2f} | {r['std']:>8.2f} | {speedup:>8.2f}x")
    
    print("=" * 60)
    print(f"Baseline: {baseline:.2f} ms")
    
    best = min(results, key=lambda x: x["avg"])
    best_speedup = baseline / best["avg"]
    print(f"Best: {best['name']} - {best['avg']:.2f} ms ({best_speedup:.2f}x speedup)")
    
    print("\n=== Benchmark Complete ===")
    return 0


if __name__ == "__main__":
    sys.exit(main())
