# GPU NAM — Neural Amp Modeler player (separate repo)

GPU NAM is a [Neural Amp Modeler](https://github.com/sdatkinson/neural-amp-modeler)
(`.nam`) player that runs amp captures on the GPU, built on Pulp. It used to live
here under `examples/gpu-nam`. It has grown into a full plugin — its own `.nam` /
Keras format loaders, a reference-faithful CPU oracle for five capture
architectures, a recreated editor, format packaging, and bundled models — so it
now lives in its **own repository** and consumes Pulp as an SDK:

> **https://github.com/danielraffel/pulp-gpu-nam**

Keeping it out of the framework repo lets Pulp stay focused on reusable
capabilities rather than one large, domain-specific plugin. It also makes GPU NAM
a worked example of *building a real plugin against the Pulp SDK* — Pulp is
vendored there as a git submodule, and the plugin depends only on Pulp's public
targets (`pulp::render`, `pulp::gpu-audio`, `pulp::signal`, `pulp::view`,
`pulp::canvas`, `pulp::runtime`).

## Install

Download the signed, notarized macOS installer from the repo's
[Releases](https://github.com/danielraffel/pulp-gpu-nam/releases) page. It offers a
Customize pane to pick formats (AU / VST3 / CLAP / Standalone). A default capture
ships in the bundle, so it makes sound immediately; load your own `.nam` for real
amp tones.

## Build from source

```bash
git clone https://github.com/danielraffel/pulp-gpu-nam.git
cd pulp-gpu-nam
git submodule update --init --recursive
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target GpuNam_Standalone GpuNam_CLAP GpuNam_VST3 GpuNam_AU \
      -j$(sysctl -n hw.ncpu)
```

Full build, test, packaging, and architecture-support details are in that repo's
README and `docs/nam-support.md`.

## What stayed in Pulp: the GPU WaveNet inference primitive

The one framework capability GPU NAM relies on remains part of the Pulp SDK: a
fused, block-parallel, **conditioned-WaveNet GPU forward** —
`pulp::render::GpuCompute::prepare_wavenet` / `wavenet_forward` (see
[`core/render/include/pulp/render/gpu_compute.hpp`](https://github.com/danielraffel/pulp/blob/main/core/render/include/pulp/render/gpu_compute.hpp)).
It is a general neural-inference primitive — a sequence of gated, dilated, causal
1-D conv layer-arrays computed GPU-resident with the CPU↔GPU round-trip paid once
per block — not specific to any capture format. GPU NAM's repo owns the `.nam`
translation onto it.

For the design rationale of running neural-amp inference on the GPU (why the naive
per-sample approach loses and the fused block-parallel approach wins as models
grow), see the honest write-up in the GPU NAM repo's README.
