# GPU Audio SDK (developer guide)

> Status: experimental, default-OFF. Built on `develop/gpu-audio-runtime`; not in
> a public release until human-reviewed. Requires a Skia/Dawn GPU build
> (`-DPULP_ENABLE_GPU=ON`); every path has a CPU fallback so plugins degrade on
> unsupported devices.

Pulp's GPU audio runtime lets you run heavy, block-based DSP on the GPU without
touching the audio thread. This guide is the developer-facing surface: the
primitives, the real-time transport, and the ready-made processors.

## The model in one paragraph

GPU compute is fast but the CPU↔GPU round-trip is slow, so **the audio thread
never talks to the GPU**. A non-real-time worker runs GPU work and the audio
thread reads results a fixed number of blocks later (plugin delay compensation).
Keep intermediates GPU-resident, read back once, and amortize the round-trip by
batching. Small/low-latency DSP stays on the CPU; the GPU does the coarse, heavy,
batched, latency-tolerant work.

## What belongs on the GPU (and what doesn't)

The GPU only pays off for work that is **heavy, parallel, and latency-tolerant** —
enough arithmetic per block to dwarf the CPU↔GPU round-trip. Match the workload:

**Good GPU candidates** (big parallel math, or batched across instances/voices):
- FFTs / iFFTs
- Partitioned & long-IR convolution (and many IRs at once)
- Spectral EQ, spectral freeze, spectral morphing
- Large filter banks / beamforming
- ML model inference (NAM / CLAP-style neural models)
- Waveform & spectrogram generation; large additive / sinewave banks

**Keep on the CPU** (too small to beat the round-trip; latency-sensitive):
- Simple gain, biquad filters, compressors
- Small delays, envelope followers, MIDI processing

Rule of thumb: if it's a tight per-sample scalar op, leave it on the CPU. If it's
a block-wide transform or a bank you can batch, the GPU can win — and the
further you push size (long IRs, many voices, big FFTs), the bigger the win.
SuperConvolver's GPU mode targets the long-IR / many-IR regime for this reason;
at short IRs the CPU path is selected because it's faster there.

## Which GPUs / platforms

GPU compute runs through the WebGPU layer (Dawn / wgpu-native), so it follows
WebGPU's backends. Every path has a CPU fallback, so a plugin still works with no
compatible GPU — it just runs the `signal::*` reference path.

| Platform | Backend | GPUs |
|---|---|---|
| macOS — **Apple Silicon (M1–M5)** | Metal | integrated Apple GPU ✅ *(validated)* |
| macOS — Intel | Metal | Intel / AMD GPUs |
| Windows 10+ | D3D12 (or Vulkan) | NVIDIA, AMD, Intel |
| Linux | Vulkan | NVIDIA, AMD, Intel |
| no compatible GPU | — | CPU fallback (correct, not accelerated) |

`capabilities().backend` reports the live backend at runtime ("Metal" / "D3D12" /
"Vulkan"), plus limits and optional features (timestamp-query, f16). Audio paths
are currently **validated on Apple Silicon / Metal**; the other backends are
supported by the WebGPU layer but not yet audio-validated — treat them as
experimental until benchmarked, and rely on the CPU fallback meanwhile.

## Layer 1 — compute primitives (`pulp::render::GpuCompute`)

Create with `GpuCompute::create()` then `initialize_standalone()` (own device,
headless) or `initialize_from_surface(GpuSurface&)` (share the UI device for
zero-copy DSP↔UI buffers). `capabilities()` reports backend, limits, and optional
features (timestamp-query, f16). All are validated against CPU references.

| Primitive | Method | Use |
|---|---|---|
| FFT / iFFT | `fft_forward` / `fft_inverse` (+ `fft_forward_timed`) | spectral analysis, fast convolution |
| Fused convolution | `prepare_convolution` + `convolve` | one-readback FFT convolution with a resident IR |
| Batched convolution | `prepare_convolution_batch` + `convolve_batch` | many blocks/instances in one submit+readback (≈16× at batch 16) |
| Magnitude / complex-mul | `compute_magnitude`, `complex_multiply`, `batch_magnitude` | spectral building blocks |
| Dense matmul | `matmul` | neural layers, matrix DSP (ambisonics, mixing) |
| Dense layer | `dense_tanh` | neural inference (NAM dense / LSTM gates) |
| Additive synth | `additive_synth` | sum of sinusoidal partials |
| Modal synth | `modal_strike` | struck/plucked resonant-mode banks |
| Granular | `granular_cloud` | windowed pitch-shifted grain clouds |

These are **not real-time-safe** (they block on readback); call them from the
worker (Layer 2) or offline.

## Layer 2 — real-time transport (`pulp::gpu_audio`)

`GpuAudioTransport` bridges the audio thread and a non-RT worker. Implement a
`GpuAudioNode` (descriptor with channels/block/latency/miss-policy; `prepare()`;
`process_block()` on the worker; `process_cpu_fallback()` RT-safe). `prepare()`
the transport with the node and `Config{ ring_blocks, run_worker_thread }`; the
audio callback calls `process()` — it writes the input ring and reads the
latency-delayed output, never blocking. On a miss the node's `MissPolicy`
(silence / dry passthrough / CPU fallback) fills the block. Report
`latency_samples()` to the host.

## Layer 3 — ready-made processors (`pulp::gpu_audio`)

- `GpuConvolver` — FFT overlap-add convolution with a fixed IR; `signal::Convolver` CPU fallback.
- `GpuStft` — STFT/ISTFT (windowed analyze / synthesize) — the spectral toolkit base.
- `GpuSpectralFreeze` — capture + sustain a spectral frame (infinite pad).
- `GpuSpectralMorph` — blend between two captured spectra (t ∈ [0,1]).

## Authoring pattern

1. Pick the layer: a ready processor (Layer 3), or your own `GpuAudioNode` whose
   `process_block` calls Layer-1 primitives.
2. Always implement a CPU fallback (`signal::*`) — it's your degrade path on
   no-GPU devices and your miss policy.
3. Keep small/low-latency DSP on the CPU; send only coarse, batched, heavy work
   to the GPU, and batch across instances/voices to amortize the round-trip.
4. Test against a CPU reference (golden / frequency-domain) and assert RT-safety
   (no alloc/lock/block on the audio thread).

See `planning/2026-06-27-gpu-audio-compute-runtime.md` for the architecture and
`...-demo-plugins.md` for the flagship example plugins (SuperConvolver, GPU NAM,
Spectral Lab).
