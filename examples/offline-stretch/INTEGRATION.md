# OfflineStretch + PulpTempoSampler — integration & status

What the current OfflineStretch + PulpTempoSampler integration provides, how the
pieces fit, and how to build/run them.

## The engine — `pulp::signal::OfflineStretch`

Header-only, in `core/signal/offline_stretch.hpp`. Whole-input in → whole-output
out, **exactly `round(N·time_ratio)` frames** (loop grid-lock). Modes:

- **Repitch (linked)** — `repitch_linked=true`: pure sinc6 resample, pitch follows tempo.
- **Tempo-only** — `time_ratio≠1`, pitch 0: `RealtimePitchTimeProcessor` time_stretch, pitch preserved.
- **Pitch-only** — `time_ratio=1`, pitch≠0: realtime_pitch + formant (follow/preserve/independent).
- **Independent R+S** — follow-formant = single pass (stretch R·P + resample P); preserve/independent = pitch→tempo cascade.
- **STN noise routing** (`route_noise_stn`, off by default; opt in with `--stn`) and **draft `quality=0`** (fast Hann-OLA) for instant preview.

```cpp
pulp::signal::OfflineStretch s;
s.prepare(sample_rate, channels, sizing /* max_time_ratio, max_pitch_semitones */);
pulp::signal::OfflineStretchOptions o; o.time_ratio = host_bpm_ratio; o.pitch_semitones = ...;
const long out = pulp::signal::offline_stretch_output_frames(in_frames, o.time_ratio);
s.process(in, in_frames, out_ptrs, out, o, &err);   // distinct instance per render thread
```

Ratio/routing math: `RATIO-AND-ROUTING.md`. Baseline capture:
`baseline-phase1.json`. Quality refinement gates: `QUALITY-REFINEMENT-NOTES.md`.

## The instrument — `PulpTempoSampler`

`examples/PulpTempoSampler/`. Reuses PulpSampler's voice/store. Flow:

```
load_loop ─▶ BuiltInKeyTempoAnalyzer (BPM) + OnsetDetector (slices)
host tempo change ─▶ background worker renders the whole loop through
                     OfflineStretch at R = loop_bpm/host_bpm ─▶ publishes the
                     stretched buffer to the generation-safe SampleSlotBank
MIDI note ─▶ SamplerVoice plays a slice of the cached (already tempo-matched)
             buffer at native rate, transport-aware
```

Generation-safe publish + audio-ack keeps in-flight voices on their old buffer
(RT lifetime); the worker supersedes stale tempo requests. Params: gain, ADSR,
link, pitch, formant, quality, loop.

## Verification

- Engine: `pulp-test-offline-stretch` — 13 cases / 182k assertions (null, exact
  length, repitch≡reference, pitch ±12 st, independent R+S, STN determinism,
  stereo coherence, safety, draft).
- Instrument: `pulp-tempo-sampler-test` — 4 cases / 45k assertions (load+detect+
  publish, grid-lock, MIDI→audio, render-while-playing race).
- Perf (M-series): tempo q2 **29.5× realtime**, q0 **151×** (targets 20× / 100×).
- Edge: ratios 0.25×–4×, sub-FFT (256-frame) inputs — safe.

## Build / run

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DPULP_ENABLE_GPU=OFF   # engine needs no GPU
cmake --build build --target pulp-test-offline-stretch pulp-tempo-sampler-test \
      PulpTempoSampler_CLAP pulp-tempo-sampler stretchcli
# CLAP plugin:  build/CLAP/PulpTempoSampler.clap
# Standalone:   build/examples/PulpTempoSampler/pulp-tempo-sampler [loop.wav]
# Dev CLI:      build/examples/offline-stretch/stretchcli in.wav out.wav --ratio 1.25 --quality 2
```

## Waveform UX Disposition

Waveform theming and screenshot-diff validation belong to the design-system /
GPU track, not this engine harness. This example intentionally keeps the
engine-focused build runnable with `-DPULP_ENABLE_GPU=OFF`; PulpTempoSampler is
the pilot consumer for the waveform UI work when that design-system path lands.
