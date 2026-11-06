# Appendix A GPU resident spectral chain evidence — 2026-11-06

**Disposition: PARTIAL / NO CHANGE.** The existing resident spectral stack is
healthy on this Apple Silicon host and has a useful CPU reference, but the
Appendix A promotion gate is not met. This evidence does not add a graph
adapter, public API, or runtime path.

## Scope and provenance

| Item | Value |
|---|---|
| Pulp source | merged current `origin/main` `1acc348c2c3c06c9a8f5052c58ec19caf336180d`; evidence head `cb6c07686d90c1d506df5e5e25e9ebded5978458` |
| Worktree | `evidence/gpu-graph-chain-20261106` |
| Host/provider | Apple Silicon macOS; WebGPU backend `Metal`; vendor `apple` |
| Workload | `CpuSpectralStack` vs `GpuSpectralStack`, FFT 2048, hop 512, 16 resident layers, 64 measured hops after 8 warmups |
| Build | Release, `PULP_BENCHMARK=ON`, governed `cmake --build build --target pulp-gpu-spectral-chain-evidence` |
| Probe | `test/test_gpu_spectral_chain_evidence.cpp` (advisory standalone executable) |
| Probe output SHA-256 | `96a6520466fd1d5db47d16b422acbea7cf90239939ac980dd3898d6ae112952f` |

The existing GPU spectral correctness suite also passed from this clean source
(`189 assertions / 13 cases`).

## Matched timing result

| Backend | min us | p50 us | p95 us | p99 us | max us | mean us | jitter SD us |
|---|---:|---:|---:|---:|---:|---:|---:|
| CPU | 121.000 | 125.791 | 131.625 | 133.833 | 136.000 | 126.633 | 2.993 |
| GPU resident | 540.375 | 591.500 | 677.292 | 872.334 | 1098.625 | 611.142 | 79.006 |

The GPU path is about 5.7x slower at the median for this workload. Its
resident spectral payload is 262,144 bytes (magnitude plus phase for 16 × 2048
floats), and its per-hop output readback payload is 16,384 bytes. The result
does not justify promoting this lane at the tested size.

## Controls

- **Positive parity:** CPU/GPU maximum absolute output difference `0.000` for
  the matched deterministic frame; output is non-silent.
- **Planted oversized negative:** preparing a 1,048,576-point, 1,048,576-layer
  shape reports `available=false` rather than admitting an invalid device plan.
- **Planted device-loss negative:** `notify_device_lost()` sets
  `device_lost=true`; the GPU render then returns `false`; the explicit CPU
  fallback render succeeds. This is a synthetic lifecycle control, not physical
  Metal loss evidence.

## Qualification blocker and no-change decision

The Appendix A gate requires measured copy reduction for a matched graph chain,
activation latency, jitter, memory, and device-loss fallback with an explicit
graph-adapter owner and callback no-wait guarantee. This probe supplies timing,
resident payload, deterministic parity, and planted refusal/loss controls. It
cannot supply the copy-count gate: the current `PULP_BENCHMARK` counters cover
the generic async convolution path and do not instrument the spectral stack's
three per-hop `WriteBuffer` calls, its output `CopyBufferToBuffer`, or its
blocking map/readback. There is also no `SignalGraph` GPU adapter in the tested
source, and `GpuSpectralStack::render()` is explicitly blocking; the CPU
fallback is an owner-side choice rather than a callback-safe graph transport.

Therefore the correct decision is **no graph promotion and no runtime/API
change**. Keep the existing resident stack as the base. A future evidence lane
must first instrument these transfer events in a benchmark-only surface, define
the staged graph-chain comparator, and prove callback no-wait fallback before
re-running Appendix A.
