# Spectral Lab — N-layer GPU spectral freeze / morph

Spectral Lab is a spectral "cloud" instrument/effect built on Pulp: it freezes and
morphs many layers of spectral content into an evolving pad. The live audio path is
an RT-bounded CPU spectral stack + freeze framer by default, with an opt-in GPU
engine ([`pulp::gpu-audio`](../reference/modules.md#gpu_audio)'s spectral stack)
that carries big clouds — many frozen layers — for far less per-block cost than the
serial CPU path. The native GPU UI shows the layer cloud, the output spectrum, and
the controls.

It lives in-tree under `examples/spectral-lab/` and is one of the two worked
examples for the [GPU audio runtime](../reference/modules.md#gpu_audio).

## What the GPU buys you

Freezing and morphing N independent spectral layers is embarrassingly parallel:
each layer is an STFT frame evolved with its own per-hop phase behavior, and the
GPU computes them together. At high layer counts the GPU spectral stack matches the
CPU reference output and wins on per-block cost; at low counts the CPU path is fine
and stays the default. The engine switch is live and stays at fixed latency.

**Freeze** sustains the actual spectral content rather than looping one FFT period:
per-hop phase wander (**Jitter**) decorrelates the repetition so a freeze becomes a
smooth, evolving pad. Lower Jitter toward 0 for a more static, tonal freeze.

## Install

Download the signed, notarized macOS installer from the release repo's
[Releases](https://github.com/danielraffel/pulp-spectral-lab/releases) page. The
installer's Customize pane picks formats (AU / VST3 / CLAP / Standalone).

## Build from source

Spectral Lab builds as part of the standard Pulp build when the GPU stack is
present (it gates on `pulp::render`):

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target SpectralLab_Standalone SpectralLab_CLAP \
      SpectralLab_VST3 SpectralLab_AU -j$(sysctl -n hw.ncpu)
```

Formats: VST3, AU v2, CLAP, Standalone. There is also a spectral-core benchmark
(`spectral-lab-bench`) and tests (`spectral-lab-test`,
`spectral-lab-processor-test`) that assert the GPU stack matches the CPU reference
and that the live engine switch stays finite at fixed latency.

## See also

- [GPU audio module](../reference/modules.md#gpu_audio) — the runtime this is built on
- [GPU Audio SDK guide](../guides/gpu-audio-sdk.md) — building your own GPU node
- [SuperConvolver](super-convolver.md) — the other in-tree GPU-audio example
