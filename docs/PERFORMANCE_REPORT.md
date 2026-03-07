# VLR Wavefront Performance Optimization Report

## Executive Summary

Successfully implemented and debugged CUB-based path sorting and stream compaction optimizations for the VLR Wavefront renderer. The key issue was **memory alignment** - CUB requires 16-byte aligned buffer addresses, which was not initially satisfied in our implementation.

## Optimization Features Implemented

### 1. Path Sorting by Material (CUB RadixSort)
- **Purpose**: Group paths by material category to reduce BSDF evaluation divergence
- **Implementation**: Uses `cub::DeviceRadixSort::SortPairs` to sort path indices by their 8-bit material category
- **Status**: ✅ **WORKING**

### 2. Stream Compaction (CUB DeviceSelect)
- **Purpose**: Remove terminated paths from the active queue to reduce wasted computation
- **Implementation**: Uses `cub::DeviceSelect::Flagged` to filter active paths
- **Status**: ✅ **WORKING**

## Critical Bug Fix: Memory Alignment

### Problem
The renderer crashed with `unspecified launch failure` and `misaligned address` errors when CUB functions were called.

### Root Cause
CUB temporary storage buffers were partitioned incorrectly:
```cpp
// INCORRECT - causes misalignment
void* d_cubTemp = d_tempStorage;
uint32_t* d_keys = reinterpret_cast<uint32_t*>(
    static_cast<uint8_t*>(d_tempStorage) + cubTempBytes);
```

When `cubTempBytes` (e.g., 6655 bytes) is not 16-byte aligned, `d_keys` ends up at a misaligned address like `0x...9FF`.

### Solution
Align the CUB temporary storage size to 16-byte boundaries before calculating subsequent buffer offsets:

```cpp
// CORRECT - ensures alignment
void* d_cubTemp = d_tempStorage;
size_t alignedCubTempBytes = (cubTempBytes + 15) & ~15;  // Round up to 16-byte boundary
uint32_t* d_keys = reinterpret_cast<uint32_t*>(
    static_cast<uint8_t*>(d_tempStorage) + alignedCubTempBytes);
```

This fix was applied to:
- `sortPathsByMaterial()` in `compact.cu`
- `sortPathsByMaterialTempStorageBytes()` in `compact.cu`
- `compactPathsCUB()` in `compact.cu`
- `compactPathsCUBTempStorageBytes()` in `compact.cu`

## Debugging Process

### 1. Initial Investigation
- Added extensive debug prints to identify crash location
- Confirmed CUB library itself works correctly via standalone test (`cub_test.exe`)

### 2. Isolation Testing
- Created minimal test cases to isolate the problem
- Tested with simplified kernels (e.g., `keys[idx] = idx % 256`)
- Confirmed the issue persisted even with trivial kernel logic

### 3. Root Cause Discovery
- Added detailed pointer logging: `[CUB] Keys: in=0000000B052019FF, out=0000000B052019FF`
- Noticed the address `0x...9FF` is not aligned (should end in `0x...000` or similar)
- Identified the `misaligned address` CUDA error via stream synchronization

### 4. Fix Implementation
- Applied 16-byte alignment to all CUB buffer partitions
- Verified fix with both simple and Cornell Box test scenes

## Performance Results

### Test Configuration
- GPU: NVIDIA GeForce RTX 2060 SUPER (Compute 7.5)
- Resolution: 512x512 (262,144 paths)
- CUDA: 13.1
- OptiX: 8.0.0

### Benchmark Results

#### Simple Test Scene
- Samples: 32
- Time: 0.33s
- Throughput: 96.65 samples/s

#### Cornell Box Scene
- Samples: 64
- Time: 0.55s
- Throughput: 115.76 samples/s

### Memory Usage
- Path State Buffer: ~62 MB (262,144 paths)
- CUB Sort Temp Storage: 1048.50 KB
- CUB Compact Temp Storage: 258.50 KB

## Code Changes Summary

### Modified Files
1. `libVLR/GPU_kernels/compact.cu`
   - Fixed memory alignment in `sortPathsByMaterial()`
   - Fixed memory alignment in `compactPathsCUB()`
   - Updated temp storage size calculation functions
   - Added `__host__` qualifiers for host-callable functions

2. `libVLR/GPU_kernels/compact.h`
   - Added function declarations for CUB operations

3. `libVLR/shared/path_types.h`
   - Enabled `UsePathSorting = true`
   - Enabled `UseStreamCompaction = true`

4. `libVLR/context.cpp`
   - Integrated CUB sort and compact into render loop
   - Added conditional logic for depth > 0 (material categories only valid after first bounce)

### Test Files Created
1. `test/cub_test.cu` - Standalone CUB library validation
2. `test/cornell_box_test.cpp` - Cornell Box reference scene
3. `test/benchmark.py` - Performance benchmarking script

## Lessons Learned

1. **Memory Alignment is Critical**: CUB (and many GPU libraries) require proper memory alignment. Always round buffer sizes up to alignment boundaries (typically 16 bytes).

2. **Systematic Debugging**: When facing "unspecified launch failure" errors:
   - First verify the library works in isolation
   - Add detailed pointer logging
   - Check for misaligned addresses
   - Use `cudaStreamSynchronize()` to catch delayed errors

3. **CUDA Architecture Matching**: Ensure compiled code matches the target GPU architecture (sm_75 for RTX 2060 SUPER, not sm_89).

## Future Work

### Deferred Optimizations
- **SoA Memory Layout**: Requires extensive refactoring of data structures
- **Material-Specialized Kernels**: Requires deep BSDF implementation knowledge

### Potential Improvements
- Adaptive sorting threshold (only sort when material diversity is high)
- Multi-level compaction (compact at multiple bounce depths)
- Hybrid sorting (use simpler methods for small path counts)

## Conclusion

The VLR Wavefront renderer now successfully implements GPU-accelerated path sorting and stream compaction using NVIDIA CUB. The critical memory alignment bug has been resolved, and both optimizations are functioning correctly. The renderer can now efficiently handle complex scenes like Cornell Box with proper material-based coherence optimization.

---

**Date**: 2026-03-07  
**Status**: ✅ **COMPLETE AND WORKING**
