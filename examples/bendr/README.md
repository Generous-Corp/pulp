# Bendr

A small pitch / time / freeze effect built **entirely from public
`pulp::signal` primitives** — a worked example of composing Pulp's own DSP
into a real plugin with a native GPU UI. No third-party DSP.

## What it does

- Realtime pitch shift (±12 semitones) with independent or pitch-linked
  formant control and a preserve mode.
- **Freeze** — hold the current spectrum and play it chromatically over MIDI.
- A tempo-syncable pitched feedback delay (a second pitch processor inside the
  feedback loop).
- Dry/wet mix with latency compensation.

## Primitives it composes (all in `core/signal/`)

| Primitive | Role |
|-----------|------|
| `RealtimePitchTimeProcessor` | pitch/time shift + built-in `FreezeHold` + `TransientPhasePolicy` |
| `PitchedFeedbackDelay` | tempo-syncable delay with in-loop pitch |
| `DryWetMixer` | latency-compensated dry/wet |

## Build

```sh
cmake --build build --target Bendr_AU Bendr_Standalone Bendr_CLAP Bendr_VST3
```

Formats: VST3, AU, CLAP, Standalone.

## Layout

- `src/reference_processor.hpp` — the `Processor`, composing the primitives.
- `src/reference_ui.hpp` — native GPU UI (XY pad, spectrum, freeze, delay HUD).
- `src/reference_view.cpp` — out-of-line `create_view()`.

MIT-licensed, same as the rest of Pulp.
