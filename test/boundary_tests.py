#!/usr/bin/env python3
# Boundary Case Tests
# Test edge cases and extreme configurations

import subprocess
import time

def run_test(exe_path, args, name):
    """Run a single test"""
    print(f"\n{'='*60}")
    print(f"Testing: {name}")
    print(f"Command: {exe_path} {' '.join(args)}")
    print(f"{'='*60}")
    
    start_time = time.time()
    
    try:
        result = subprocess.run(
            [exe_path] + args,
            capture_output=True,
            text=True,
            timeout=180,
            cwd="."
        )
        
        elapsed = time.time() - start_time
        
        if result.returncode != 0:
            print(f"FAILED: Return code {result.returncode}")
            print(result.stderr[-500:] if len(result.stderr) > 500 else result.stderr)
            return {'name': name, 'status': 'FAILED', 'elapsed': elapsed}
        
        # Print last 20 lines of output
        lines = result.stdout.split('\n')
        for line in lines[-20:]:
            print(line)
        
        return {'name': name, 'status': 'PASSED', 'elapsed': elapsed}
        
    except subprocess.TimeoutExpired:
        print("TIMEOUT: Test exceeded time limit")
        return {'name': name, 'status': 'TIMEOUT', 'elapsed': 180}
    except Exception as e:
        print(f"ERROR: {e}")
        return {'name': name, 'status': 'ERROR', 'elapsed': 0}

def main():
    print("="*60)
    print("VLR Wavefront Boundary Case Tests")
    print("="*60)
    
    # Test configurations
    tests = [
        # Low resolution
        ("cornell_box_test.exe", ["-w", "256", "-h", "256", "-s", "32"], "Low Resolution (256x256)"),
        
        # High resolution
        ("cornell_box_test.exe", ["-w", "3840", "-h", "2160", "-s", "16"], "High Resolution (4K, 16 samples)"),
        
        # Many samples (stress test)
        ("minimal_test.exe", ["-s", "512"], "Many Samples (512)"),
        
        # Different scenes with various complexities
        ("simple_render_test.exe", ["-s", "256"], "Simple Scene (256 samples)"),
        ("multi_material_test.exe", ["-s", "128"], "Multi-Material (128 samples)"),
        ("glass_spheres_test.exe", ["-s", "128"], "Glass Spheres (128 samples)"),
    ]
    
    results = []
    
    for exe, args, name in tests:
        result = run_test(exe, args, name)
        if result:
            results.append(result)
    
    # Print summary
    print("\n" + "="*60)
    print("BOUNDARY TESTS SUMMARY")
    print("="*60)
    print(f"\n{'Test':<40} {'Status':<10} {'Time(s)':<10}")
    print("-"*60)
    
    passed = 0
    failed = 0
    for r in results:
        print(f"{r['name']:<40} {r['status']:<10} {r['elapsed']:<10.2f}")
        if r['status'] == 'PASSED':
            passed += 1
        else:
            failed += 1
    
    print("\n" + "="*60)
    print(f"Results: {passed} PASSED, {failed} FAILED")
    print("="*60)
    
    return 0 if failed == 0 else 1

if __name__ == "__main__":
    exit(main())
