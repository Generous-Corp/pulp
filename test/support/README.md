# test/support — audio harness layers

Shared test-only support code for the audio observability harness. Nothing
here ships in runtime builds; everything runs off the audio thread.

## Layering rule

```
signals  (audio_test_signals, audio_signal_generators)   — deterministic stimulus + event scripts
   ↓
metrics  (audio_metrics)                                 — pure-arithmetic facts about buffers
   ↓
assertions (audio_assertions)                            — CheckResult pass/fail over metrics/buffers
   ↓
artifacts (audio_artifacts)                              — JSON serialization of metrics + provenance
   ↓
scenarios (render_scenario)                              — HeadlessHost block-loop renders + matrix sweeps
```

**No back-edges.** A layer may include layers above it in this list, never
below. Generators must not measure; metrics must not render; the artifact
writer must not know what a scenario is beyond its provenance string. When
adding a helper, place it in the lowest layer that can express it.

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
