# OfflineStretch — Ratio Derivation & Component Routing

Design contract for the pitch/time/formant paths. Reflects what
`core/signal/offline_stretch.hpp` actually does.

## Symbols

- `R` = `time_ratio` — output duration / input duration.
- `S` = `pitch_semitones`; `P = 2^(S/12)` — pitch ratio (frequency multiplier).
- `N` = input frames; output is always **exactly** `round(N·R)` frames.

## Mode routing (process())

| Condition | Path | Pitch | Formant |
|---|---|---|---|
| `repitch_linked` | sinc6 resample, read input at `i/R` | follows `1/R` (vinyl) | follows |
| `S=0, R=1` | exact copy | — | — |
| `S=0, R≠1` | **tempo** — `time_stretch` engine, whole-file, content-aligned, exact trim | preserved | — |
| `R=1, S≠0` | **pitch** — `realtime_pitch` engine, latency-trimmed | shifted `P` | per mode |
| `R≠1, S≠0, formant=follow` | **single-pass**: tempo by `R·P`, then resample by `P` | shifted `P` | follows |
| `R≠1, S≠0, formant=preserve/independent` | **cascade**: pitch (formant-correct) → tempo `R` | shifted `P` | fixed / `formant_semitones` |

## Ratio math for the single-pass independent path

Goal: duration `R`, pitch `×P`, in one phase-vocoder pass.

1. Tempo-stretch by `R·P` (pitch **preserved**) → length `round(N·R·P)`.
2. Resample reading at step `P` (`out[j] = sinc6(inter, j·P)`) → length `≈ round(N·R)`,
   pitch `×P`, formants dragged along (**follow**).

One PV pass + one interpolation — not two cascaded PV passes. The tempo engine is
sized at `prepare()` for `max_time_ratio · 2^(max_pitch/12)` so `R·P` stays in
bounds. A true engine-internal single-pass for **preserved** formants would
avoid the cascade's second PV pass and remains a future optimization.

## Component routing matrix (STN)

`route_noise_stn` is off by default. Opt in before prepare/sizing
(`stretchcli --stn`) to set the engine's `noise_morphing`, which runs STN
(sines/transients/noise) decomposition:

| Component | Pitch | Time | Formant | Transient | Recombine |
|---|---|---|---|---|---|
| Tonal (sines) | PV phase-propagation | yes | envelope (SpectralEnvelopeShifter) | no | phase-locked |
| Transient | reset (Röbel) | reposition | no | phase reset; optional verbatim relocation on tempo-only renders | sharp |
| Noise / residual | morph | morph (NoiseMorpher) | optional | no | morph, no phase prop |

The tonal/transient/noise split + recombination lives inside the engine
(`StnDecomposer` + `NoiseMorpher` + `TransientPhasePolicy` +
`MultichannelPhaseCoordinator`); OfflineStretch selects it via config and is
verified deterministic on noise input.

## Verified Behavior

- Pitch ±12 st doubles/halves a sine; duration unchanged.
- Independent follow (R=1.5,+12 st) → 1 kHz at exact length; cascade
  (R=0.75,+7 st, preserve) → ~749 Hz at exact length.
- STN noise routing: finite + byte-identical across runs.
