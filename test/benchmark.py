#!/usr/bin/env python3
# Performance Benchmark Script
# Compare rendering performance with different optimization settings

import subprocess
import re
import time
import sys

def run_test(exe_path, samples, output_name):
    """Run a single test and extract timing information"""
    print(f"\nRunning: {exe_path} -s {samples}")
    print("=" * 60)
    
    start_time = time.time()
    
    try:
        result = subprocess.run(
            [exe_path, "-s", str(samples)],
            capture_output=True,
            text=True,
            timeout=120,
            cwd="bin"
        )
        
        elapsed = time.time() - start_time
        
        if result.returncode != 0:
            print(f"ERROR: Test failed with return code {result.returncode}")
            print(result.stderr)
            return None
        
        print(result.stdout)
        print(f"Total elapsed time: {elapsed:.2f}s")
        
        return {
            'samples': samples,
            'elapsed': elapsed,
            'output': result.stdout
        }
        
    except subprocess.TimeoutExpired:
        print("ERROR: Test timed out")
        return None
    except Exception as e:
        print(f"ERROR: {e}")
        return None

def main():
    print("=" * 60)
    print("VLR Wavefront Performance Benchmark")
    print("=" * 60)
    
    # Test configurations
    tests = [
        ("simple_render_test.exe", 32, "baseline"),
        ("cornell_box_test.exe", 64, "cornell_box"),
    ]
    
    results = []
    
    for exe, samples, name in tests:
        result = run_test(exe, samples, name)
        if result:
            results.append((name, result))
    
    # Print summary
    print("\n" + "=" * 60)
    print("BENCHMARK SUMMARY")
    print("=" * 60)
    
    for name, result in results:
        print(f"\n{name}:")
        print(f"  Samples: {result['samples']}")
        print(f"  Time: {result['elapsed']:.2f}s")
        print(f"  Throughput: {result['samples'] / result['elapsed']:.2f} samples/s")
    
    print("\n" + "=" * 60)

if __name__ == "__main__":
    main()
