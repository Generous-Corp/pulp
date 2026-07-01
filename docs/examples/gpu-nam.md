# GPU NAM — Neural Amp Modeler support

GPU NAM is a Pulp example that loads open-source [Neural Amp Modeler](https://github.com/sdatkinson/neural-amp-modeler)
(`.nam`) capture files and runs them as a guitar-amp plugin (VST3 / AU / CLAP /
Standalone). It exists to showcase Pulp's GPU audio runtime around a faithful
recreation of the reference NeuralAmpModeler editor, and it doubles as the
reference for **how to add NAM inference to your own Pulp project**.

The neural inference is **clean-room** — written from the model math and the
`.nam` file format, not ported from any existing implementation.

## What a `.nam` file is

A `.nam` file is JSON: a model `architecture`, a `config` describing the network
shape, a flat `weights` array, and the capture `sample_rate`. GPU NAM reads it,
builds the matching network, and streams audio through it sample-by-sample.

## Supported architectures

| Architecture | Status | Notes |
|---|---|---|
| **WaveNet (A1)** | Supported | The original/standard NAM capture. Runs on the CPU oracle and, opt-in, on the fused GPU engine. |
| **LSTM** | Supported | The recurrent NAM option. CPU engine; validated bit-approximately (max abs diff ~7e-8 over the reference vector) against NeuralAmpModelerCore's own output. |
| **NAM Architecture 2 (A2)** | Not modeled | NAM's next-gen architecture (the default for new captures on TONE3000). It departs from A1 — LeakyReLU activations, a convolutional head, mixed kernel sizes per layer array, grouped convolutions, and packed-slimmable Full/Lite sizing — so it needs its own engine. GPU NAM rejects A2 files cleanly rather than mis-rendering. Download **A1 – Legacy** models to use with GPU NAM today (see below). |
| ConvNet, Linear | Not modeled | Rare in practice; not yet implemented. |
| Parametric / conditioned WaveNet (`condition_size > 1`) | Not modeled | Its extra condition channels come from host controls that aren't wired; rejected rather than mis-rendered. |
| Experimental WaveNet variants (grouped convolutions, FiLM conditioning, `head1x1`, bottlenecks) and `SlimmableContainer` | Not modeled | Research-stage architectures from the training tool; not what shipping A1 captures use. GPU NAM rejects them with a clear message rather than producing wrong audio. |

An unsupported model fails to load with a specific reason (surfaced in the log
and left as a dry pass-through) — it never silently mis-renders.

### Where to get loadable models

NAM's model library at [TONE3000](https://www.tone3000.com/search) now defaults
new captures to **A2**, which GPU NAM does not yet load. To download captures
that work today, filter for the legacy A1 architecture:

> Filters → Technical → **A1 – Legacy**, then download the A1 models from a tone
> pack's *Models* section.

A1 (WaveNet) and LSTM captures load and run; A2 captures are rejected with a
clear message until GPU NAM grows a dedicated A2 engine. A2 is fully MIT-licensed
open source, so a clean-room A2 engine is a plausible future addition.

## CPU oracle + opt-in GPU engine

- **CPU engine (default).** The exact NAM forward runs inline on the audio
  thread, re-blocked into fixed internal blocks. Always available and RT-safe.
- **GPU engine (opt-in, WaveNet only).** The `Engine` control routes the same
  fixed blocks through Pulp's GPU audio runtime (one fused GPU forward per
  channel on a non-real-time worker, with a CPU fallback on any worker miss).
  LSTM is recurrent and runs CPU-only; selecting a GPU device for an LSTM model
  keeps the CPU engine.

Both engines report one fixed latency for the prepared lifetime, so switching
engines live keeps the host's delay compensation correct and the dry/wet blend
phase-aligned.

## Signal chain

Input → **noise gate** (on the drive) → **neural model** → **tone stack**
(Bass / Middle / Treble) → **cabinet IR** (optional convolution) → output, with a
matched dry-path delay so a dry/wet blend stays aligned. The cabinet IR is loaded
from a WAV / AIFF / FLAC file, summed to mono, resampled to the session rate, and
unit-energy normalized (see [`read_impulse_response`](../reference/modules.md)).

## Using it

- **Load a model:** click the model slot in the editor and choose a `.nam` file,
  or set `GPU_NAM_MODEL=/path/to/model.nam` before launch to override the
  bundled default.
- **Load a cabinet IR:** click the IR slot and choose an impulse-response file.
- **Switch engines:** toggle `Engine` (CPU ↔ GPU) in the settings page.

## Adding NAM inference to your own Pulp project

The inference is self-contained and depends only on `choc` (JSON) plus the
standard library — no Eigen, no external SDK. Three headers under
`examples/gpu-nam/` are the reusable substrate:

- `nam_model.hpp` — the WaveNet loader + CPU inference.
- `nam_lstm.hpp` — the LSTM loader + CPU inference.
- `nam_runtime.hpp` — `NamRuntime`, which peeks the `architecture` field and
  presents one architecture-agnostic surface: `load_nam_runtime(path, rt)`, then
  `rt.reset()` / `rt.process(in, out, n)` / `rt.process_sample(x)`.

A minimal integration inside a `Processor`:

```cpp
#include "nam_runtime.hpp"
using pulp::examples::nam::NamRuntime;

NamRuntime nam;
std::string err;
if (!load_nam_runtime(path, nam, &err))
    /* log err; pass dry */;

// In process(), per channel (keep one NamRuntime per channel for state):
nam.process(drive, wet, num_samples);   // WaveNet or LSTM, transparently
```

`NamRuntime::gpu_eligible()` tells you whether the fused GPU path applies (only
WaveNet today); `wavenet()` returns the concrete model for the GPU node to upload.
Everything else — the noise gate, tone stack, cabinet IR, dry/wet delay, meters,
and the GPU engine wiring — lives in `gpu_nam_processor.hpp` as a worked example
you can lift patterns from.

## Honesty notes

- The rendering is GPU-accelerated in **both** GPU NAM (Skia Graphite on Dawn)
  and the reference plugin (iPlug2's NanoVG backend); the recreation aims for
  visual parity, not a rendering-speed claim. See
  [`comparison.md`](../../examples/gpu-nam/docs/comparison.md) in the example.
- GPU NAM is a **player**: it runs captures. Training `.nam` models is the job of
  the [Neural Amp Modeler trainer](https://github.com/sdatkinson/neural-amp-modeler).
- Performance claims are made only where measured; the comparison page documents
  the method and the numbers.
