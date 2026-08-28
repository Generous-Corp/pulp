# Drum whole-voice math validation

**Status:** Implemented and landed in [Pulp PR #7869](https://github.com/Generous-Corp/pulp/pull/7869)

**Scope:** `TomVoice` only; no kick, snare, hat, clap, cymbal, membrane, string, zap, FM6, or FM8 behavior changed

**Canonical execution plan:** [Drum Whole-Voice Math Profiling and Bounded Optimization](https://github.com/danielraffel/pulp-planning/blob/main/2026-08-27-drum-whole-voice-math-profiling-execution.md)

Pulp chooses drum math alternatives from complete production-voice evidence,
not from isolated transcendental benchmarks. The 2026-08-27 census measured 13
non-FM percussion scenarios at 44.1, 48, and 96 kHz with block sizes 32, 64,
128, and 512. Sine, exponential, and power families in kick, snare, cymbal,
membrane, string, and zap voices could not plausibly produce a greater-than-10%
whole-voice gain, so those voices remain on their reference paths.

The generic and SDS-V-family toms were the sole exception: their noise ladder's
exact `tanhf` accounted for 37.30% and 34.00% of sampled whole-voice time.
Selecting the existing `FastMath::tanh` implementation at that consumer produced
the following complete-hit Release results across all 24 tom rate/block cells.
Each cell reports the median paired gain from independent processes; each process
itself uses 11 trials of three complete hits. Executable order was counterbalanced
to limit temperature and frequency-order bias. The generic voice used three
processes, while the narrower SDS-V-family margin was confirmed with five.

| Voice | Minimum gain | Median gain | Maximum gain |
|---|---:|---:|---:|
| Generic tom | 14.87% | 16.84% | 19.19% |
| SDS-V-family tom | 10.14% | 16.07% | 17.41% |

No declared configuration lost after cross-process median aggregation. The
narrowest cell was the SDS-V-family voice at 96 kHz with 32-frame blocks, so
that cell's margin is the reopen trigger if compiler, architecture, or voice
lifetime behavior changes.

The 0.75-second acceptance corpus covered all eight tom presets, velocities
0.2/0.82/1.0, all three rates, and 32/512 block partitions. Across 144 cells,
the worst candidate/reference changes were 0.0000393% peak, 0.00000343% RMS,
0.000246% for a transient/tail window, and 0.0000654% for the spectral-probe
signature. Every render was finite and allocation-free; repeated renders and
block partitions were bit-exact within each profile.

`TomVoice::LadderMathProfile::reference` remains the default. Select
`realtime_efficient` only while the voice is idle; the setter rejects a change
during an active hit so one hit never crosses saturation curves.

## Enable the measured Tom profile

Include `<pulp/signal/drum/tom.hpp>` and link `Pulp::signal`. Select the profile
during setup or between hits:

```cpp
pulp::signal::drum::TomVoice tom;
tom.prepare(sample_rate);

const bool accepted = tom.set_ladder_math_profile(
    pulp::signal::drum::TomVoice::LadderMathProfile::realtime_efficient);
```

`accepted` is `false` if a hit is active. The selection applies to the noise
ladder inside every `TomVoice` preset, including the generic and SDS-V-family
presets. It does not alter other drum voice classes. Omit the setter, or select
`LadderMathProfile::reference`, to retain reference math. Use
`ladder_math_profile()` to inspect the current selection.

The earlier FM6 and FM8 conclusions are unchanged. FM6 remains a measured
whole-voice performance NO-GO, and FM8 remains a quality NO-GO under its
predeclared peak-change gate.
