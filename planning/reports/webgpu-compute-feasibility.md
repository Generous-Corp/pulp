# WebGPU Compute for Audio: Feasibility Report

**Phase:** 11 (WebGPU/audio exploration)
**Date:** 2026-03-31
**Status:** Complete

## Summary

This report documents a non-realtime WebGPU compute feasibility spike for
audio-related batch processing. The experiment uses Dawn's WebGPU compute
pipeline (WGSL compute shaders) on a shared device with Skia Graphite.

**Recommendation: Conditional Go** — GPU compute is viable for large batch
workloads (>64K elements) but not for small real-time buffers.

---

## What Was Tested

### Compute Experiments

Two WGSL compute shaders were implemented and benchmarked:

1. **Spectral Magnitude** — compute `sqrt(re^2 + im^2)` from interleaved
   complex float pairs. Workgroup size 256.

2. **Complex Multiply** — element-wise `(a_re + a_im*i) * (b_re + b_im*i)`
   for frequency-domain convolution. Workgroup size 256.

Both were tested at sizes from 256 to 1,048,576 elements.

### Device Sharing (Phase 13 Compatibility)

Verified that a second consumer (compute pipeline) can:
- Obtain the Dawn wgpu::Device from GpuSurface
- Create storage/compute buffers on the shared device
- Submit command buffers to the shared queue
- Allocate 32 MB of GPU memory alongside Skia Graphite

**Result: All checks passed on macOS/Metal.**

---

## Benchmark Results

### Magnitude Spectrum: CPU vs GPU

| Bins | CPU (us) | GPU (us) | Speedup | Winner |
|------|----------|----------|---------|--------|
| 256 | 1.3 | 323.3 | 0.00x | CPU |
| 1,024 | 5.2 | 317.7 | 0.02x | CPU |
| 4,096 | 21.8 | 603.0 | 0.04x | CPU |
| 16,384 | 82.6 | 484.4 | 0.17x | CPU |
| 65,536 | 329.2 | 341.0 | 0.97x | CPU |
| 262,144 | 1,313.4 | 337.1 | 3.90x | GPU |
| 1,048,576 | 5,328.3 | 1,993.8 | 2.67x | GPU |

### Complex Multiply: CPU vs GPU

| Elements | CPU (us) | GPU (us) | Speedup | Winner |
|----------|----------|----------|---------|--------|
| 256 | 2.8 | 429.8 | 0.01x | CPU |
| 1,024 | 10.5 | 767.1 | 0.01x | CPU |
| 4,096 | 33.5 | 313.6 | 0.11x | CPU |
| 16,384 | 131.6 | 469.3 | 0.28x | CPU |
| 65,536 | 524.0 | 534.3 | 0.98x | CPU |
| 262,144 | 2,090.8 | 919.7 | 2.27x | GPU |
| 1,048,576 | 8,358.0 | 4,055.7 | 2.06x | GPU |

**Test platform:** macOS (Apple Silicon), Metal backend, Dawn via Skia Graphite build.

---

## Analysis

### CPU vs GPU Crossover Points

- **Magnitude:** Crossover at ~65K bins. Below that, upload/readback latency
  dominates. At 262K bins, GPU is ~4x faster.
- **Complex multiply:** Same crossover at ~65K elements. GPU is ~2x faster
  at 262K+.

### Upload/Readback Overhead

GPU dispatch overhead is approximately 300-500 microseconds per operation
(buffer creation + write + dispatch + copy + map readback). This fixed cost
makes GPU compute uneconomical for:

- Per-buffer audio processing (typical buffer: 64-4096 samples)
- Real-time FFT at standard sizes (256-8192 bins)
- Any operation in the audio callback

### Where GPU Compute Makes Sense

- **Offline batch processing:** Spectrogram generation over thousands of
  FFT frames (e.g., 4096 bins x 1000 frames = 4M elements per dispatch)
- **Background analysis:** Large convolution reverbs, offline spectral
  processing
- **Visualization pre-computation:** Generating spectrogram textures for
  display (thousands of FFT frames at once)

### Where GPU Compute Does NOT Make Sense

- **Real-time audio processing:** Upload/readback latency (~300+ us) exceeds
  typical buffer durations at standard sample rates
- **Small FFT buffers:** Below 64K elements, CPU is faster
- **Per-frame operations:** Per-frame buffer creation + dispatch has too much
  overhead for 60fps use

### Platform Stability

- **macOS Metal:** Stable. All tests pass. Device sharing works.
- **Windows D3D12:** Not tested in this spike (requires cross-platform CI).
  Dawn supports D3D12 compute; expect similar behavior.
- **Vulkan/Linux:** Not tested. Dawn's Vulkan backend supports compute.
- **iOS:** Metal compute supported but not tested.

### Failure Modes Observed

1. **Buffer usage flags:** Storage buffers written via `WriteBuffer` must
   include `CopyDst` usage. Dawn validates this strictly (validation error,
   not crash).
2. **Readback buffer lifecycle:** After `MapAsync` + `Unmap`, a readback
   buffer cannot be reused as a copy destination. Must create a new buffer.
3. **No silent failures:** Dawn's error callback catches all misuse before
   corruption occurs. This is a strength of the WebGPU validation layer.

---

## Device Sharing Verification (Phase 13)

Phase 13 (Three.js bridge) needs a shared Dawn device between Skia Graphite
and a WebGPU JS bridge. This phase verified:

| Check | Result |
|-------|--------|
| Device obtained from GpuSurface | Pass |
| Second consumer creates buffers | Pass |
| Command submission on shared queue | Pass |
| 2x 16MB GPU memory allocation | Pass |
| Backend identified | Metal |

**Approach for Phase 13:** The existing `GpuSurface::dawn_device_handle()`
API provides the shared device. A Three.js bridge would receive this device
and create its own render targets. The react-native-webgpu PlatformContext
pattern (device owner shares with consumers) maps directly to our
GpuSurface ownership model.

---

## Recommendation

### Conditional Go

GPU compute is viable and worth pursuing for **batch/offline** audio
workloads. It is NOT viable for **real-time per-buffer** processing.

**Proceed with:**
- Batch spectrogram generation (offline, background thread)
- Large convolution pre-computation
- Visualization texture generation
- Phase 13 shared-device Three.js bridge (device sharing verified)

**Do NOT pursue:**
- GPU-based real-time audio processing
- Per-frame compute dispatches at audio buffer sizes
- Replacing CPU FFT with GPU FFT for standard sizes

### Next Steps

1. Phase 13 can proceed with shared-device architecture (verified here)
2. An offline spectrogram generator could use batch_magnitude for bulk
   processing
3. No changes needed to the real-time audio pipeline — CPU FFT remains
   the right choice for standard buffer sizes

---

## Code Deliverables

| File | Purpose |
|------|---------|
| `core/render/include/pulp/render/gpu_compute.hpp` | Public API |
| `core/render/src/gpu_compute.cpp` | WGSL shaders + Dawn compute pipeline |
| `test/test_gpu_compute.cpp` | Correctness + benchmark tests |
| `docs/reports/webgpu-compute-feasibility.md` | This report |
