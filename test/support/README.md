# test/support — audio harness layers

Shared test-only support code for the audio observability harness. Nothing
here ships in runtime builds; everything runs off the audio thread.

The pure file-analysis layers (metrics, assertions, artifacts, and the
buffer-level FFT spectrum analyzers) live in the reusable
`pulp::audio-analysis` lib at `tools/audio/analysis/`
(`<pulp/audio/analysis/…>` headers, namespace `pulp::test::audio`). They were
promoted out of this directory so the shipped `pulp audio validate …` CLI can
analyze decoded WAVs without linking any `test/` library. The
`Processor`-driven layers (signals, scenarios, contracts, and the
scenario-driven Doctor wiring) stay test-only here and link that lib.

## Layering rule

```
file-analysis lib — pulp::audio-analysis (tools/audio/analysis), also linked by the CLI:
metrics    (audio_metrics)      — pure-arithmetic facts about buffers
assertions (audio_assertions)   — CheckResult pass/fail over metrics/buffers
artifacts  (audio_artifacts)    — JSON serialization of metrics + provenance
spectrum   (audio_spectrum)     — buffer-level FFT response + THD/THD+N
           (audio_doctor_artifacts) — JSON curve artifacts for response/THD

test-only (test/support), links pulp::audio-analysis:
reverb     (reverb_metrics)     — T60 / band T60 / echo density / band level, header-only
signals    (audio_test_signals, audio_signal_generators) — deterministic stimulus + event scripts
   ↓
scenarios  (render_scenario)    — HeadlessHost block-loop renders + matrix sweeps
   ↓
contracts  (audio_contracts)    — named claims over one rendered scenario
   ↓
doctor     (audio_doctor)       — scenario-driven response/THD (delegates to audio_spectrum)
```

`reverb_metrics` sits beside the generators rather than in the analysis lib on
purpose. It needs no `Processor`, so the layering rule would allow it there —
but it measures a reverberant DECAY (it needs an impulse response and a decay
model, not a spectrum), and the analysis lib is linked into the shipped
`pulp audio validate` CLI, which has no use for a T60. Anything spectral a
reverb test needs is already in `audio_spectrum` and must be taken from there.

**No back-edges.** A layer may include layers above it in this list, never
below. Generators must not measure; metrics must not render; the artifact
writer must not know what a scenario is beyond its provenance string. The
file-analysis lib must never depend on the test-only layers (it has no
`Processor`/scenario knowledge). When adding a helper, place it in the lowest
layer that can express it.

Determinism is the harness contract: every generator documents its exact
expression and seed handling (no `std::random_device`, no clocks), and
every assertion takes an explicit, named tolerance.

## Copy this scenario

```cpp
auto result = RenderScenario(pulp::examples::create_pulp_gain)
    .name("pulpgain.minus6")                 // provenance for artifacts
    .sample_rate(48000.0)
    .block_size(128)
    .input(make_sine(2, 24000, 440.0f, 48000.0, 0.25f))
    .set_param(pulp::examples::kOutputGain, -6.0f)
    .render();
INFO(summarize(result.metrics));
CHECK(assert_not_silent(result.metrics).passed);
CHECK(assert_frequency_near(result.output.channel(0), 48000.0, 440.0, 5.0).passed);
```

Instruments use `.channels(0, 2)`, a `duration_ms(...)`, and
`.midi(make_note_script(...))` instead of `.input(...)`. For sample-rate
sweeps pass a generator: `.input([](double sr, int ch, std::int64_t n) {
return make_sine(ch, int(n), 440.0f, sr); })`, then `run_matrix(...)`.
Partition checks: `assert_block_partition_invariant(scenario, {64, 128, 256})`.

## Copy this contract

A contract names the claim so failures are self-describing — the verdict
message carries `contract '<name>':`, the scenario facts, and a metrics
artifact path. New effects copy a fixture from `test_audio_contracts.cpp`:

```cpp
AudioContract contract("myeffect.bypass", scenario);   // renders once
contract.expect(expect_passthrough(contract.result(), input))
        .expect(expect_finite_and_unclipped(contract.result()))
        .expect(assert_block_partition_invariant(scenario, {64, 128, 256}));
const auto verdict = contract.verify();
INFO(verdict.message);
CHECK(verdict.passed);
```

Family helpers: `expect_passthrough` (unity/bypass), `expect_silence_preserved`
(silence-in-silence-out, silent-without-MIDI), `expect_tone` (instrument
pitch + level over a held window), `expect_finite_and_unclipped` (hygiene).
Partition invariance is the existing `assert_block_partition_invariant`.
Anything else: `.expect({condition, "message"})` — expectations are plain
`CheckResult`s.

## Doctor (offline)

The Audio Doctor sits on top of scenarios: it drives a processor through a
`RenderScenario` and runs an offline FFT (`core/signal`) to answer "is this
DSP behaving correctly?" — magnitude/frequency response and THD/THD+N in this
slice. Heavy FFT/analyzer code lives only here (test/support); nothing below
scenarios may include `audio_doctor.hpp`. Each analyzer states its Analyzer
Determinism Contract (window, FFT length, stimulus, coherence) in its header
and echoes those fields into a schema-versioned JSON curve artifact.

```cpp
auto scenario = RenderScenario(create_pulp_effect)   // input/duration overridden
    .name("lowpass.response").sample_rate(48000.0).block_size(256)
    .set_param(kFrequency, 200.0f).set_param(kFilterType, 0.0f);
const double checkpoints[] = {50.0, 8000.0};
auto curve = response_relative_to_input(scenario, checkpoints, {.fft_length = 16384});
CHECK(curve.attenuation_db_at(8000.0) >= 20.0);      // direct response claim
write_response_artifact(curve, "lowpass.response");  // reviewable JSON

auto thd = measure_thd(scenario, /*fundamental_hz=*/999.0, {.fft_length = 16384});
CHECK(thd.thd < 0.001);                              // clean tone, near-zero THD
```
