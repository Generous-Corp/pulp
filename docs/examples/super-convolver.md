# SuperConvolver — GPU convolution reverb

SuperConvolver is a convolution reverb / impulse processor built on Pulp. The
live audio path is an RT-safe CPU partitioned convolver
(`signal::PartitionedConvolver`) with lock-free runtime impulse-response swap; the
GPU engine ([`pulp::gpu-audio`](../reference/modules.md#gpu_audio)'s
`GpuConvolver`) is the accelerator for very long IRs and many simultaneous rooms.
The native GPU UI shows the live IR waveform, a frequency display, and controls.

It lives in-tree under `examples/super-convolver/` and is one of the two worked
examples for the [GPU audio runtime](../reference/modules.md#gpu_audio).

## What the GPU buys you (honest version)

A convolution reverb is the honest case for GPU audio: with long impulse responses
or a bank of many rooms, the frequency-domain multiply-accumulate is wide, regular,
and parallel — exactly the shape a GPU wins at. The plugin runs CPU by default and
switches to the GPU engine when the room count or IR length makes it worthwhile;
the UI shows which engine is live.

The caveat, stated in the repo's README: a static-pan room bank is mathematically
reducible, so the headline many-rooms benchmark measures parallel-FFT throughput,
not proof that the *sound* requires the GPU. The GPU path exists for genuinely
large workloads, not to inflate a number.

Offline bounces render the GPU convolution **synchronously** so an export matches
what you hear live (the GPU worker can't keep up with a faster-than-real-time
render otherwise). The CPU engine always bounced correctly.

## Install

Download the signed, notarized macOS installer from the release repo's
[Releases](https://github.com/danielraffel/pulp-superconvolver/releases) page. The
installer's Customize pane picks formats (AU / VST3 / CLAP / Standalone).

## Build from source

SuperConvolver builds as part of the standard Pulp build when the GPU stack is
present (it gates on `pulp::render`):

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target SuperConvolver_Standalone SuperConvolver_CLAP \
      SuperConvolver_VST3 SuperConvolver_AU -j$(sysctl -n hw.ncpu)
```

Formats: VST3, AU v2, CLAP, Standalone. There is also a headless audio-validation
test (`super-convolver-test`) and an honest CPU-vs-GPU benchmark
(`super-convolver-bench`, run by hand).

## See also

- [GPU audio module](../reference/modules.md#gpu_audio) — the runtime this is built on
- [GPU Audio SDK guide](../guides/gpu-audio-sdk.md) — building your own GPU node
- [Spectral Lab](spectral-lab.md) — the other in-tree GPU-audio example
