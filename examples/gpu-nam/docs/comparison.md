# GPU NAM (Pulp) vs the reference NeuralAmpModelerPlugin

An honest, factual comparison of this demo against the open-source
[NeuralAmpModelerPlugin](https://github.com/sdatkinson/NeuralAmpModelerPlugin)
(iPlug2) whose editor it faithfully recreates. The goal is a fair capability
map — what each does, where they differ, and specifically what runs on the GPU.
It is not a knock on the reference plugin, which is excellent and is the ground
truth this demo measures itself against.

## What each is

| | NeuralAmpModelerPlugin (reference) | GPU NAM (this demo) |
|---|---|---|
| UI framework | iPlug2 `IGraphics` (NanoVG) | Pulp view/canvas (Skia Graphite on Dawn) |
| Neural inference | NeuralAmpModelerCore (Eigen), **CPU** | Clean-room WaveNet: **CPU oracle + opt-in GPU engine** |
| `.nam` model support | Yes (trainer + player) | Player only (loads `.nam` WaveNet captures) |
| License posture | iPlug2 (NOASSERTION), eigen (MPL-2.0) | MIT-releasable, no copyleft deps |

## Rendering — GPU in both, different stacks

The reference UI is **not** CPU-rasterized: iPlug2's default `IGraphics` backend
is NanoVG, which is GPU-accelerated (OpenGL/Metal). So the honest statement is
that **both editors are GPU-rendered** — the reference via NanoVG, this demo via
Skia Graphite on Dawn (WebGPU). We do not claim a rendering advantage on the UI;
the recreation aims for visual parity, and the montage in this folder shows it.

Where Pulp's stack adds value is uniformity: the same view/canvas tree is
GPU-rendered live in a host **and** headless-renderable for tests and
screenshots (the montage and the `gpu-nam-ui-test` fixture are the same code
path), which is how this demo keeps its editor under visual-regression cover.

## Audio — this is where "on GPU" is real

The reference runs its WaveNet **entirely on the CPU** (NeuralAmpModelerCore is
an Eigen/CPU implementation). GPU NAM runs a clean-room WaveNet with **two
interchangeable engines**:

- **CPU oracle** (default) — the exact NAM forward inline on the audio thread.
  Always available; the fallback when no GPU device exists.
- **GPU engine** (opt-in, in the settings gear) — one fused `nam_forward` per
  channel executed on the GPU via `GpuNamCloudNode` on a `GpuAudioTransport`,
  and **validated bit-for-bit against the CPU oracle** (`gpu-nam-gpu-test`).

So: **the neural amp inference can run on the GPU in this demo, which the
reference does not do.** Both engines re-block the host stream into fixed
512-sample blocks through one shared FIFO at one fixed, PDC-correct latency, so
switching engines is seamless and phase-aligned (`gpu-nam-plugin-test` proves
the switch is live at fixed latency, and that the amped output is identical
regardless of host block size).

## Signal-face parity

| Control | Reference | GPU NAM | Notes |
|---|---|---|---|
| Input / Output gain | ✅ | ✅ | Same ranges (−20..20 / −40..40 dB) |
| Noise gate (threshold + toggle) | ✅ | ✅ | Downward expander on the input drive |
| Bass / Middle / Treble tone stack | ✅ | ✅ | Low-shelf / peak / high-shelf; 5 = flat |
| EQ enable toggle | ✅ | ✅ | |
| Input / Output meters | ✅ | ✅ | Idle-dark with an accent baseline |
| Model (`.nam`) slot | ✅ | ✅ | Loads WaveNet captures |
| Engine CPU/GPU switch | ✕ (CPU only) | ✅ | The point of the GPU demo |

## Honest gaps (where the reference does more, today)

- **Impulse-response (IR) convolution** — the reference loads and applies a
  cabinet IR. GPU NAM shows the IR slot for layout faithfulness but **does not
  apply an IR yet**; it is a UI placeholder, not wired to a convolver. (Pulp has
  a partitioned convolver — see SuperConvolver — so this is a wiring task, not a
  missing capability.)
- **Output mode / calibration** — the reference offers Raw / Normalized /
  Calibrated output and input-level calibration. GPU NAM has plain output gain
  only.
- **Model training** — out of scope by design; GPU NAM is a player. Train with
  the [NAM trainer](https://github.com/sdatkinson/neural-amp-modeler).

## Summary

On the faithful signal face and the editor, GPU NAM reaches parity with the
reference. Its distinct contribution is running the **neural amp inference on
the GPU** (opt-in, bit-exact against the CPU), which the CPU-only reference does
not offer. It trails the reference on IR convolution and output calibration,
both of which are known follow-ups rather than architectural limits.
