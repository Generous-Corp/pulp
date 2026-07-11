# GPU audio runtime (developer guide)

> Status: experimental, default-OFF. Requires a Skia/Dawn GPU build
> (`-DPULP_ENABLE_GPU=ON`); every path has a CPU fallback so plugins keep
> working on unsupported devices.

This is **not** "run your audio on the GPU." It's a runtime that lets a plugin
selectively accelerate *computationally expensive* DSP on the GPU **while
preserving real-time audio guarantees and seamless CPU compatibility** — the
audio thread is never blocked on the GPU, and anything the GPU can't do (or
can't keep up with) falls back to the CPU. This guide is the developer surface:
the compute primitives, the real-time transport, and the ready-made processors.

## The model in one paragraph

The bottleneck isn't GPU compute — it's **CPU↔GPU communication**, so the whole
runtime is built around that. The audio thread **never talks to the GPU**: it
hands work to a non-real-time worker and reads results a fixed number of blocks
later (reported to the host as plugin delay compensation). To make that pay off,
batch lots of work together, keep intermediates GPU-resident, do **one** readback
instead of many, and add predictable latency instead of blocking. Small/
low-latency DSP stays on the CPU; the GPU does the coarse, heavy, batched,
latency-tolerant work — and a CPU fallback always covers it.

## Honest tradeoffs (read this first)

GPU audio is easy to oversell, so here are the hard parts up front — three things
to know before reaching for it:

1. **The round-trip tax is real and we don't hide it.** Every block crosses
   CPU→GPU and back. That transfer + the GPU's own dispatch/readback latency is
   pure overhead, and for small or scalar work it *dwarfs* the compute — the GPU
   is then **slower** than the CPU, full stop. We measured it on our own
   convolution path: at a 256-sample block the per-call readback dominates, so
   the CPU wins there. GPU only pays off when the per-block compute is large
   enough to swamp the round-trip (long IRs, many voices/IRs batched, big FFTs).
   That's why SuperConvolver **defaults to the CPU engine** and the GPU engine is
   opt-in, aimed at the heavy regime.

2. **It is NOT a free speed-up for any plugin.** If your DSP is gain, biquads,
   a compressor, a small delay, an envelope follower, or MIDI — keep it on the
   CPU; the GPU will only make it slower. The win is narrow and specific (see the
   list below). We'd rather tell you that than sell you a GPU mode that
   regresses your plugin.

3. **Compatibility is opt-out-safe: there is always a CPU fallback.** A common
   disappointment with GPU audio is a plugin that won't even run on a given card.
   Pulp's contract avoids that: **no path hard-requires a GPU.**
   If the GPU is absent, unsupported, or fails to initialize, the node logs it
   and runs its `signal::*` CPU reference — the plugin still loads and produces
   correct audio, just unaccelerated. On a *miss* mid-stream (the worker didn't
   finish in time) the transport's `CpuFallback` policy fills the block on the
   CPU, seamlessly — no dropout. `capabilities().backend` tells you at runtime
   which backend you actually got ("Metal"/"D3D12"/"Vulkan"), and audio paths are
   only **validated on Apple Silicon / Metal** today (see the matrix below) —
   everything else is experimental-but-falls-back. We won't claim a card works
   until we've tested it.

Bottom line: GPU audio here is a **latency-tolerant accelerator for heavy,
batched, parallel DSP, with a guaranteed CPU fallback** — not a magic
across-the-board speed-up, and never a hard GPU dependency.

## What belongs on the GPU (and what doesn't)

The GPU only pays off for work that is **heavy, parallel, and latency-tolerant** —
enough arithmetic per block to dwarf the CPU↔GPU round-trip. Match the workload.

**The value case — two ways GPU audio is genuinely worth it:**
1. **Things that are otherwise impossible / infeasible on CPU.** A single plugin
   whose compute a CPU can't sustain in real time: large-scale physical
   modeling, thousands of simultaneous convolutions/partials, room/spatial
   modeling that touches enormous sample counts, real-time neural inference. The
   most compelling case is a plugin that simply *couldn't exist* on a CPU at all
   — that's where the GPU earns its keep.
2. **Headroom — running more heavy work at once.** Offloading demanding,
   batchable DSP to the otherwise-idle GPU so the session can carry more heavy
   instances/voices than the CPU alone would allow.

**When it is NOT worth it (we'll say it plainly):** if your plugin is light DSP —
a basic synth, an EQ, a delay — leave the GPU out; there's little to gain and the
round-trip will only make it slower. And because the GPU path is **latency-
tolerant by design** (it adds fixed plugin-delay-compensated latency), it suits
mixing, sound-design and rendering — *not* zero-latency live tracking/monitoring.

The crossover, concretely: GPU wins once per-block compute ≫ the round-trip, and
the win grows as you scale size (longer IRs, more voices, bigger FFTs/banks).
Below that line the CPU is faster — so pick per workload:

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
| Batched convolution | `prepare_convolution_batch` + `convolve_batch` | many blocks/instances in one submit+readback (amortizes per-dispatch + readback overhead; measured ~2× at batch 2 / stereo — the multiplier grows with batch size until it becomes bandwidth- or readback-bound) |
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

- `GpuConvolver` — FFT overlap-add convolution with a fixed IR; a continuously-fed, zero-latency `signal::PartitionedConvolver` CPU fallback that stays latency-aligned so a GPU miss is filled seamlessly.
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

### A minimal GPU node, end to end

A `GpuAudioNode` is the whole integration surface. Implement four methods — what
you are, how to set up, the worker-side work, and an RT-safe fallback — then let
the transport schedule it:

```cpp
#include <pulp/gpu_audio/gpu_audio_node.hpp>
#include <pulp/gpu_audio/gpu_audio_transport.hpp>
#include <pulp/render/gpu_compute.hpp>

using namespace pulp;

// 1. The node: heavy, batched work that runs on the transport's worker thread.
class MyGpuNode : public gpu_audio::GpuAudioNode {
public:
    MyGpuNode(uint32_t channels, uint32_t block, uint32_t sr)
        : channels_(channels), block_(block), sr_(sr) {}

    gpu_audio::GpuAudioNodeDescriptor descriptor() const override {
        gpu_audio::GpuAudioNodeDescriptor d;
        d.name = "MyGpuNode";
        d.input_channels = channels_;
        d.output_channels = channels_;
        d.block_size = block_;
        d.sample_rate = sr_;
        d.latency_blocks = 1;                              // fixed PDC the host gets
        d.miss_policy = gpu_audio::MissPolicy::CpuFallback; // seamless if the worker is late
        d.supports_cpu_fallback = true;
        return d;
    }

    // Non-RT setup: create the device + upload static data here, never in process_block.
    bool prepare() override {
        gpu_ = render::GpuCompute::create();
        if (!gpu_ || !gpu_->initialize_standalone()) { gpu_.reset(); return false; }
        // ... prepare Layer-1 plans (FFTs, convolution, matmul) on gpu_ ...
        return true;
    }

    // Worker thread: may touch the GPU and block on readback. NEVER the audio thread.
    void process_block(const audio::BufferView<const float>& in,
                       audio::BufferView<float>& out, uint32_t n) override {
        // ... call gpu_->fft_forward / convolve / matmul, write `out` ...
    }

    // RT-safe: runs on the audio thread for a missed block. No alloc/lock/block.
    void process_cpu_fallback(const audio::BufferView<const float>& in,
                              audio::BufferView<float>& out, uint32_t n) noexcept override {
        // ... your signal::* CPU path; the default is a dry passthrough ...
    }
private:
    uint32_t channels_, block_, sr_;
    std::unique_ptr<render::GpuCompute> gpu_;
};

// 2. Wire it into your Processor. prepare() once; process() per block, RT-safe.
class MyPlugin : public format::Processor {
    MyGpuNode node_{2, 256, 48000};
    gpu_audio::GpuAudioTransport transport_;

    void prepare(const format::PrepareContext&) {
        node_.prepare();
        transport_.prepare(&node_, {/*ring_blocks*/ 8, /*run_worker_thread*/ true});
        // Report transport_.latency_samples() to the host for plugin-delay comp.
    }
    void process(audio::BufferView<float>& out,
                 const audio::BufferView<const float>& in, /*...*/) {
        transport_.process(in, out, out.num_frames());  // writes input, reads delayed output — never blocks
    }
};
```

That is the entire contract: the audio thread only ever calls `transport_.process()`
(lock-free), the GPU work happens on the worker a fixed number of blocks behind,
and a missed block degrades via your `MissPolicy` instead of glitching.

### Worked examples

Both ship as buildable plugins (VST3/AU/CLAP/Standalone) with a CPU default and
an opt-in GPU engine, a CPU-reference correctness test, and a benchmark:

- **`examples/super-convolver`** — multi-room convolution reverb. Shows batching
  *many distinct IRs* into one GPU submit (`GpuMultiConvolver`), the live CPU↔GPU
  engine switch, and a live status line (backend, blocks, µs/block, real-time
  headroom). The clearest read for the transport + live-rebuild lifecycle.
- **`examples/spectral-lab`** — N-layer spectral freeze/morph "cloud". Shows
  authoring your own `GpuAudioNode` (`gpu_spectral_cloud_node.hpp`) over a
  GPU-resident spectral stack, and reusing the same status-line + headroom
  telemetry. The clearest read for writing a node from scratch.

## Building against an installed Pulp SDK

Everything above works whether Pulp is a source submodule or an installed SDK.
A standalone plugin repo that consumes Pulp through `find_package(Pulp)` (rather
than `add_subdirectory`) gets the GPU audio runtime from the SDK's exported
targets:

| You link | You get |
|---|---|
| `pulp::gpu-audio` | Layer 2 transport + Layer 3 ready-made processors (`GpuAudioTransport`, `GpuConvolver`, `GpuStft`, `GpuSpectralFreeze`/`Morph`) and their headers |
| `pulp::render` | Layer 1 primitives (`GpuCompute`) — present only in a GPU-enabled SDK build |

`pulp::gpu-audio` is always exported: it's GPU-agnostic and carries its own CPU
fallback, so a non-GPU SDK still exposes the transport (it just runs the
`signal::*` reference path). `pulp::render` exists only when the SDK was built
`-DPULP_ENABLE_GPU=ON`, so gate any GPU-primitive code on it:

```cmake
find_package(Pulp CONFIG REQUIRED)

pulp_add_plugin(MyPlugin
    FORMATS      VST3 AU CLAP Standalone
    SOURCES      my_plugin.cpp
    PLUGIN_NAME  "MyPlugin"
    # ...
)

target_link_libraries(MyPlugin_Core PUBLIC pulp::gpu-audio)
if(TARGET pulp::render)
    target_link_libraries(MyPlugin_Core PUBLIC pulp::render)  # Layer-1 GPU primitives
endif()
```

`find_package(Pulp)` resolves the transport's private dependencies for you
(`Threads`, and `ICU` on Linux) — a static library propagates its private link
deps into its exported interface, so `PulpConfig` re-finds them; you don't list
them yourself.

### Ship a self-contained GPU bundle

A GPU plugin loads `libwgpu_native.dylib` at runtime. From a source build the
WebGPU dependency copies that dylib next to each format binary automatically; an
installed-SDK consumer has no such upstream copy step, so a `find_package(Pulp)`
plugin must place the dylib itself and bake an `@loader_path` rpath — otherwise
the bundle passes every check on the build machine and fails to load
("Library not loaded: @rpath/libwgpu_native.dylib") on any other Mac. The SDK
exports two helpers for exactly this. `find_package(Pulp)` already defines them —
no extra `include()` — so call them directly:

```cmake
foreach(_fmt MyPlugin_CLAP MyPlugin_VST3 MyPlugin_AU MyPlugin_Standalone)
    if(TARGET ${_fmt})
        pulp_make_bundle_relocatable(${_fmt})       # copy dylib + bake @loader_path
        pulp_validate_bundle_relocatable(${_fmt})   # POST_BUILD guard: fail the build if not
    endif()
endforeach()
```

`pulp_validate_bundle_relocatable` runs at build time and fails hard if a bundle
still references a build-machine-absolute path, so the footgun is caught in CI
instead of on a customer's machine. Apply it to single-bundle formats
(Standalone/CLAP/VST3/AU); an AUv3 `.appex` is intentionally not relocatable in
isolation — its dylib and framework live in the container app's
`Contents/Frameworks` and resolve by `@rpath` only inside that container, so the
container app (not the standalone appex) is the relocatability unit.

> In one line: **Pulp lets plugin developers selectively accelerate
> computationally expensive DSP on the GPU while preserving real-time audio
> guarantees and seamless CPU compatibility** — rather than "plugins run on the
> GPU." That's the accurate, and more valuable, framing.
