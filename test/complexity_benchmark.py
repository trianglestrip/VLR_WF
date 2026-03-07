#!/usr/bin/env python3
# Scene Complexity Performance Benchmark
# Test rendering performance with different scene complexities

import subprocess
import time

def run_test(exe_path, samples, name):
    """Run a single test and extract timing information"""
    print(f"\n{'='*60}")
    print(f"Testing: {name} ({samples} samples)")
    print(f"{'='*60}")
    
    start_time = time.time()
    
    try:
        result = subprocess.run(
            [exe_path, "-s", str(samples)],
            capture_output=True,
            text=True,
            timeout=120,
            cwd="."
        )
        
        elapsed = time.time() - start_time
        
        if result.returncode != 0:
            print(f"ERROR: Test failed with return code {result.returncode}")
            print(result.stderr)
            return None
        
        print(result.stdout)
        
        return {
            'name': name,
            'samples': samples,
            'elapsed': elapsed,
            'throughput': samples / elapsed
        }
        
    except subprocess.TimeoutExpired:
        print("ERROR: Test timed out")
        return None
    except Exception as e:
        print(f"ERROR: {e}")
        return None

def main():
    print("="*60)
    print("VLR Wavefront Scene Complexity Benchmark")
    print("="*60)
    
    # Test configurations: (executable, samples, name)
    tests = [
        ("minimal_test.exe", 128, "Minimal (2 meshes)"),
        ("simple_render_test.exe", 128, "Simple (1 triangle)"),
        ("multi_material_test.exe", 96, "Multi-Material (5 meshes)"),
        ("cornell_box_test.exe", 96, "Cornell Box (8 meshes)"),
        ("glass_spheres_test.exe", 64, "Glass Spheres (6 meshes, 3456 tris)"),
    ]
    
    results = []
    
    for exe, samples, name in tests:
        result = run_test(exe, samples, name)
        if result:
            results.append(result)
        else:
            print(f"WARNING: Test failed for {name}")
    
    # Print summary
    print("\n" + "="*60)
    print("SCENE COMPLEXITY BENCHMARK SUMMARY")
    print("="*60)
    print(f"\n{'Scene':<30} {'Samples':<10} {'Time(s)':<10} {'Samp/s':<12}")
    print("-"*65)
    
    for r in results:
        print(f"{r['name']:<30} {r['samples']:<10} {r['elapsed']:<10.2f} {r['throughput']:<12.2f}")
    
    print("\n" + "="*60)
    print("Notes:")
    print("- Samp/s: Samples per second (higher is better)")
    print("- Scene complexity affects memory usage and kernel occupancy")
    print("="*60)

if __name__ == "__main__":
    main()
