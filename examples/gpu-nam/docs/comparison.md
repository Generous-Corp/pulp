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
| Neural inference | NeuralAmpModelerCore (Eigen), CPU | Clean-room WaveNet: CPU oracle + opt-in GPU engine |
| `.nam` model support | Yes (trainer + player) | Player only (loads `.nam` WaveNet captures) |
| Purpose | A complete, shipping product | A Pulp GPU-audio showcase built around the same UX |

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

## Performance — what is actually measured

A claim of "faster" is only worth making with a number behind it, so here is
exactly what has been measured and what it does and does not say.

`gpu-nam-gpu-test` times the two engines on a **deliberately large synthetic
model** (48 channels, two 16-layer arrays, 4096-sample block) on this machine:

| Engine | µs / block (that model) |
|---|---|
| CPU oracle (serial, per-sample) | ≈ 7.81 × 10⁶ |
| GPU engine (block-parallel, one submit) | ≈ 5.3 × 10⁴ |

The GPU engine runs the whole block in parallel; the CPU oracle is strictly
serial per sample. The gap widens with model size, which is the point the GPU
engine is there to demonstrate.

**What this does not claim.** This measures GPU block-parallelism against *this
demo's own* CPU oracle, which is a straightforward serial reference (it even
reallocates per sample). It is **not** a benchmark against the reference
plugin's optimized Eigen CPU inference, and we do not claim to beat that — no
such benchmark has been run. For a typical `.nam` capture the CPU oracle is
already comfortably real-time, which is exactly why it is the default engine.
The honest, measured takeaway is narrow and true: **our GPU engine scales to
model sizes where our serial CPU oracle cannot**, and it reproduces the CPU
oracle bit-for-bit while doing so.

## Signal-face parity

| Control | Reference | GPU NAM | Notes |
|---|---|---|---|
| Input / Output gain | ✅ | ✅ | Same ranges (−20..20 / −40..40 dB) |
| Noise gate (threshold + toggle) | ✅ | ✅ | Downward expander on the input drive |
| Bass / Middle / Treble tone stack | ✅ | ✅ | Low-shelf / peak / high-shelf; 5 = flat |
| EQ enable toggle | ✅ | ✅ | |
| Input / Output meters | ✅ | ✅ | Idle-dark with an accent baseline |
| Model (`.nam`) slot | ✅ | ✅ | Loads WaveNet captures |
| Cabinet IR convolution | ✅ | ✅ | Partitioned convolver, after the tone stack; 0-latency |
| Engine CPU/GPU switch | ✕ (CPU only) | ✅ | The point of the GPU demo |

## Scope differences

- **Impulse-response (IR) convolution** — parity. GPU NAM loads a cabinet IR
  (WAV/AIFF/FLAC) and applies it through Pulp's `PartitionedConvolver` after the
  tone stack, matching the reference's chain order. The IR is decoded and built
  off the audio thread and swapped in RT-safely; it is 0-latency (overlap-save),
  so dry/wet stay aligned. The file-decode→mono→resample→normalize loader is a
  shared core helper (`pulp::audio::read_impulse_response`) used by both this
  demo and SuperConvolver.
- **Output mode (Raw / Normalized / Calibrated)** — the reference offers
  loudness-normalized and calibrated output. GPU NAM offers plain output gain;
  loudness-normalized output from the model's embedded metadata is a candidate
  follow-up, while full digital-analog calibration depends on host calibration
  and is out of scope for a demo.

Model training is intentionally out of scope here — GPU NAM is a player; train
with the [NAM trainer](https://github.com/sdatkinson/neural-amp-modeler).

## Summary

On the faithful signal face and the editor, GPU NAM reaches parity with the
reference. Its distinct contribution is running the **neural amp inference on
the GPU** (opt-in, bit-exact against the CPU oracle) — the reference runs its
inference on the CPU. Both editors are GPU-rendered; the audio path is where the
GPU work is unique to this demo.
