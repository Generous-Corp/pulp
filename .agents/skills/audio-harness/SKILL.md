---
name: audio-harness
description: Prove and debug what a Pulp processor actually emits — the audio observability harness (signal generators, metrics, assertions, RenderScenario, effect contracts) plus the offline Audio Doctor analyzers (magnitude/frequency response, THD/THD+N). TRIGGER on phrases like "is there sound / no audio / I hear nothing", "does this filter/compressor/synth/delay produce the right signal", "prove the DSP / prove the contract", "measure the frequency response", "what's the THD / is it distorting / aliasing", "render a test tone and assert", "audio regression", "64-frame works but 128 is silent", "sample-rate change pitch-shifted it", "describe what's in this buffer", "audio doctor", "magnitude response curve", "compare before/after a DSP refactor". Test/tool layer over HeadlessHost — deterministic, no audio device, no speakers. Off the realtime thread entirely.
---

# Audio harness (observability + validation)

Pulp's agent-first way to turn "I can't hear it" / "does this sound right?" into
**inspectable, deterministic signal evidence** — without a device, speakers, or a
debugger. You are reading this skill because you need to prove, measure, debug, or
regression-guard the audio a Pulp `Processor` emits.

Everything here is the **test/tool layer** (`test/support/audio_*`), driven by
`pulp::format::HeadlessHost`. Nothing in this skill runs on the realtime audio
thread — measurements analyze buffers that have already left `process()`. The
realtime probe path is a separate concern (see *Roadmap*).

## When to rope this skill in

- "There's no sound / it sounds wrong" — render the path offline and read the
  facts (peak/RMS/silence/frequency/NaN) instead of guessing.
- Building or changing a filter / oscillator / compressor / delay / sampler — state
  the contract and prove it (frequency response, delay time, decay, bypass-nulls).
- A regression hunt — "64 frames is fine but 128 is silent", "changing the sample
  rate chipmunked it", "the test tone toggle produces nothing".
- A DSP refactor — compare old vs new renders (exact / numeric / spectral).
- "How distorted is this?" — THD / THD+N and a harmonic breakdown.
- "What does this lowpass actually do at 8 kHz?" — a magnitude-response curve.

## The layering (each layer builds only on the ones below — no back-edges)

```
signal generators → metrics → assertions → artifacts → scenarios → contracts → doctor
```

| Layer | Header (`test/support/`) | What it gives you |
|-------|--------------------------|-------------------|
| Generators | `audio_test_signals.hpp`, `audio_signal_generators.hpp` | Deterministic stimulus: sine/square/saw, impulse(+train), step, DC, multi-sine, swept sine, seeded white/pink/brown noise, stepped automation + MIDI note scripts. No clocks, no `random_device`. |
| Metrics | `audio_metrics.hpp` | `analyze()` → `BufferMetrics`: peak, RMS, DC, NaN/Inf, clip count, silence-run; `estimate_frequency()` (zero-crossing, documented limits); `to_dbfs`; `summarize()` (agent-readable signal description). |
| Assertions | `audio_assertions.hpp` | `assert_no_nan_inf / not_clipped / silent / not_silent / peak_between / rms_between / frequency_near / null_near / channels_independent` — each returns `CheckResult{passed,message}` with dBFS/Hz/cents messages, never a bare float. |
| Artifacts | `audio_artifacts.hpp` | `BufferMetrics` → JSON (`schema_version` + provenance) for failing CI/local runs. |
| Scenarios | `render_scenario.hpp` | `RenderScenario` builder over HeadlessHost (factory, sample rate, block size, channels, duration, input/MIDI/param scripts); `render()` → `ScenarioResult`. `run_matrix()` (SR × block sweeps) + `assert_block_partition_invariant()`. |
| Contracts | `audio_contracts.hpp` | `AudioContract` — a named claim + scenario + accumulated `CheckResult`s; failures read `contract '<name>': ...`. Family helpers `expect_{passthrough,silence_preserved,tone,finite_and_unclipped}`. |
| Doctor (offline) | `audio_doctor.hpp`, `audio_doctor_artifacts.hpp` | Plugin-Doctor-style measurements: `response_relative_to_input()` (magnitude/frequency response curve + `attenuation_db_at(hz)`), `measure_thd()` (THD / THD+N + harmonic breakdown), curve JSON artifacts. FFT lives test-side only — never `core/view`/runtime. |

Read `test/support/README.md` for the authoritative layering contract.

## Run the proofs

```bash
# Build + run the whole harness (Release — Debug is meaningless for DSP timing/levels)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(sysctl -n hw.ncpu) --target \
  pulp-test-audio-support pulp-test-render-scenario pulp-test-audio-contracts \
  pulp-test-audio-doctor pulp-test-golden pulp-test-audio-matrix pulp-test-audio-tone-regression
ctest --test-dir build -R 'audio|golden|render|contract|doctor' --output-on-failure
```

The `/audio-harness` slash command wraps this. JSON metric/curve artifacts (on failure or
on demand) land under a temp `pulp-audio-metrics/` dir and are INFO-logged.

## Copy-this patterns

Describe / debug a render (the "no sound" workflow):

```cpp
auto m = analyze(rendered, 48000.0);
INFO(summarize(m, estimate_frequency(rendered.channel(0), 48000.0)));
REQUIRE(assert_not_silent(m, -60.0).passed);   // which stage went silent?
```

State and prove a DSP contract:

```cpp
auto sc = RenderScenario(create_my_lowpass)
    .name("mylp.attenuates_8k").sample_rate(48000.0).block_size(128)
    .input(Sine{.hz = 8000.0f, .dbfs = -12.0f}).set_param(kCutoff, 200.0f);
AudioContract c("mylp.attenuates_8k", sc);
c.expect(expect_finite_and_unclipped(c.result()))
 .expect(assert_block_partition_invariant(sc, {64,128,256}));
REQUIRE(c.verify().passed);     // failure says `contract 'mylp.attenuates_8k': ...`
```

Measure it like Plugin Doctor (offline Doctor):

```cpp
auto curve = response_relative_to_input(sc, {50.0, 8000.0});
REQUIRE(curve.attenuation_db_at(8000.0) >= 20.0);   // "drops ≥20 dB at 8 kHz"
auto thd = measure_thd(/* steady bin-coherent sine through the processor */);
// thd.thd_percent, thd.thd_plus_n, thd.harmonics[...]
```

## Discipline that keeps it trustworthy

- **Release only.** A Debug DSP/UI build mismeasures levels and timing.
- **Analyzer Determinism Contract** — every spectral/estimator assertion declares
  its stimulus, window (rectangular for a bin-coherent tone or an impulse; Hann
  for broadband — a Hann window annihilates an impulse at n=0), warm-up trim,
  estimator, seed, sample rate, and tolerance *class*. State them in the test so a
  red is a real DSP change, not an analyzer artifact.
- **Named tolerances**, never magic numbers — see the plan's Threshold Policy.
- **Layering is one-way.** Doctor may use scenarios + FFT; nothing below may
  include `audio_doctor`/`audio_contracts`.

## Live inspection (Audio Inspector window) — landed

For *live* signal inspection while a standalone app / hosted graph runs, there is
a separate developer tool window: `pulp::view::AudioInspectorWindow`
(`core/view/.../audio_inspector_window.hpp`). It is a sibling of the layout
inspector, not a tab — open it via its `CommandRegistry` command
`kToggleAudioInspector` (default Cmd/Ctrl+Shift+A, rebindable) or the
`/audio-inspect` slash command. It shows meters (peak/RMS/clip/NaN-Inf/silence),
the observed probe stage, a copied fixed-capacity waveform, channel balance + an
L/R level-match ratio, and a device/runtime summary — all polled once per UI tick
from a realtime-safe `AudioProbeSnapshot` (it never touches the audio thread). It
honestly shows a "no probe" / "stale" state rather than faking zeros. Live data
requires the standalone output-boundary tap, which is gated behind
`PULP_ENABLE_AUDIO_PROBES`. NOTE: the "L/R match" is a level ratio, not a
phase/Pearson correlation (the RT snapshot carries no inter-sample L*R term yet).

This is for *watching what is currently flowing*; controlled-stimulus measurement
(response/THD/etc.) is the offline Doctor above.

## Roadmap (planned — do NOT instruct using this until it lands)

- **`pulp audio validate <verb>` CLI** (render / assert / summarize / compare /
  doctor over these analyzers) — nests under the existing `pulp audio` command.
  Update the `/audio-harness` command to wrap it when it ships.
