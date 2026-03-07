#!/usr/bin/env python3
# Resolution Performance Benchmark
# Test rendering performance at different resolutions

import subprocess
import re
import time
import sys
import os

def run_test(exe_path, width, height, samples):
    """Run a single test and extract timing information"""
    print(f"\n{'='*60}")
    print(f"Testing: {width}x{height}, {samples} samples")
    print(f"{'='*60}")
    
    start_time = time.time()
    
    try:
        result = subprocess.run(
            [exe_path, "-w", str(width), "-h", str(height), "-s", str(samples)],
            capture_output=True,
            text=True,
            timeout=300,
            cwd="."
        )
        
        elapsed = time.time() - start_time
        
        if result.returncode != 0:
            print(f"ERROR: Test failed with return code {result.returncode}")
            print(result.stderr)
            return None
        
        print(result.stdout)
        
        return {
            'width': width,
            'height': height,
            'samples': samples,
            'pixels': width * height,
            'elapsed': elapsed,
            'throughput_samples': samples / elapsed,
            'throughput_pixels': (width * height * samples) / elapsed
        }
        
    except subprocess.TimeoutExpired:
        print("ERROR: Test timed out")
        return None
    except Exception as e:
        print(f"ERROR: {e}")
        return None

def main():
    print("="*60)
    print("VLR Wavefront Resolution Performance Benchmark")
    print("="*60)
    
    # Test configurations: (width, height, samples)
    # Lower samples for higher resolutions to keep test time reasonable
    resolutions = [
        (512, 512, 128),     # Baseline
        (720, 480, 96),      # 720p (16:9)
        (1280, 720, 64),     # HD 720p
        (1920, 1080, 48),    # Full HD 1080p
        (2560, 1440, 32),    # QHD 1440p
    ]
    
    results = []
    
    for width, height, samples in resolutions:
        result = run_test("cornell_box_test.exe", width, height, samples)
        if result:
            results.append(result)
        else:
            print(f"WARNING: Test failed for {width}x{height}")
    
    # Print summary
    print("\n" + "="*60)
    print("RESOLUTION BENCHMARK SUMMARY")
    print("="*60)
    print(f"\n{'Resolution':<15} {'Pixels':<12} {'Samples':<10} {'Time(s)':<10} {'Samp/s':<12} {'MPix*Samp/s':<15}")
    print("-"*80)
    
    for r in results:
        res_str = f"{r['width']}x{r['height']}"
        mpix_samp = (r['pixels'] * r['samples']) / 1e6
        mpix_samp_per_sec = mpix_samp / r['elapsed']
        print(f"{res_str:<15} {r['pixels']:<12} {r['samples']:<10} {r['elapsed']:<10.2f} "
              f"{r['throughput_samples']:<12.2f} {mpix_samp_per_sec:<15.2f}")
    
    print("\n" + "="*60)
    print("Notes:")
    print("- Samp/s: Samples per second (higher is better)")
    print("- MPix*Samp/s: Million pixels * samples per second (throughput)")
    print("="*60)

if __name__ == "__main__":
    main()
