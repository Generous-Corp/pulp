---
name: audio-harness
description: Prove and inspect what a Pulp processor emits — run the audio observability harness (metrics, scenarios, contracts) and the offline Audio Doctor analyzers (frequency response, THD)
---

Turn "is there sound / does this sound right?" into deterministic signal evidence,
offline — no audio device, no speakers. This wraps the **audio-harness** skill;
read it (`.agents/skills/audio-harness/SKILL.md`) for the full vocabulary and the
copy-this patterns.

Build Release (Debug mismeasures DSP levels/timing) and run the proofs:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(sysctl -n hw.ncpu) --target \
  pulp-test-audio-support pulp-test-render-scenario pulp-test-audio-contracts \
  pulp-test-audio-doctor pulp-test-golden pulp-test-audio-matrix pulp-test-audio-tone-regression
ctest --test-dir build -R 'audio|golden|render|contract|doctor' --output-on-failure
```

What you get:

- **Signal facts** — `analyze()` + `summarize()`: peak/RMS/DC/NaN/clip/silence-run
  and a dominant-pitch estimate, so "no sound" becomes "which stage went silent".
- **Scenarios + contracts** — `RenderScenario` renders a processor deterministically
  across sample rates / block sizes; `AudioContract` states a named claim and a
  failure reads `contract '<name>': expected … actual …`, never a raw sample index.
- **Audio Doctor (offline)** — `response_relative_to_input()` for a magnitude /
  frequency-response curve (`attenuation_db_at(hz)`), `measure_thd()` for THD /
  THD+N + harmonic breakdown. Curves serialize to schema-versioned JSON.
- **`pulp audio validate <verb>` CLI** — the same analyzers over captured audio
  files / `audio-run/` bundles, no plugin instantiation:
  - `pulp audio validate summarize out.wav [--json]` — signal summary
  - `pulp audio validate doctor out.wav --thd [--fundamental <hz>]` / `--response f1,f2,...`
  - `pulp audio validate compare a.wav b.wav [--mode null|spectral] [--tolerance <dbfs>]`
  - `pulp audio validate assert audio-run/assertions.json` — re-check stored assertions, nonzero on failure
- **Third-party plugin interrogation** — discover the host API, then render an
  explicit plugin through `PluginSlot`, no DAW or audio device:
  - `pulp audio plugin-inspect --plugin <bundle> --format <format>` (always JSON)
  - `pulp audio render --plugin <bundle> --out out.wav --duration-ms 1000`
  - both commands isolate vendor code behind a child-process timeout
  - drive renders with `--input-signal`, `--input`, repeatable `--param`, and repeatable `--midi`
  - use `--warmup-ms`, `--initial-param`, and `--settle-ms` before capture;
    use `--tail-ms` and `--wav-format float32` for analysis-quality captures
  - `--param <id>=<value>[@frame]` values are the PLAIN native parameter domain,
    not normalized; `@frame` is **sample-accurate** (block-rate on LV2 by its
    control-port nature, and on any plugin that reads its params once per block)
  - exposed as `pulp_audio_plugin_inspect` and `pulp_audio_render` MCP tools
    (render takes single initial/automation/MIDI tokens; use the CLI for multiple)
- **Latency proof (`--latency-report`)** — prove a plugin's `latency_samples()`
  against the delay actually in its rendered audio. The host slides the whole
  track by that number and nothing else checks it, so a wrong one comb-filters
  every parallel/multi-mic mix silently.

  ```bash
  # Needs a pass-through/dry mode (arrange with --param) and a broadband,
  # aperiodic stimulus — never silence or a sine.
  pulp audio render --plugin My.clap --input-signal noise --duration-frames 32768 \
      --param <dry-mix-id>=0 --out /tmp/o.wav --latency-report /tmp/lat.json
  ```

  It **refuses rather than guesses**: an unprovable claim exits nonzero exactly
  like a disproven one. Read `null_depth_db` / `ambiguity_margin_db` in the
  artifact — a pass with a small margin is a finding, not a clean bill of health.
  Add `--latency-expect <n>` to pin the value the plugin is *supposed* to have;
  without it the proof is self-consistency only, so a plugin whose true delay AND
  report both grew still passes. Same evidence over MCP via `latency: true`.

  **Most plugins do not need this** — of Pulp's 24 example plugins only two ever
  report nonzero latency. It is for convolution / FFT / lookahead / oversampling /
  neural DSP, where the number is *derived* and a refactor moves it. See
  `docs/guides/latency-proof.md` for when NOT to use it.

To add coverage for a new effect, copy the nearest contract fixture in
`test/test_audio_contracts.cpp` (or a Doctor case in `test/test_audio_doctor.cpp`)
and adjust the expectations — don't hand-roll sample loops. For a captured WAV or
an `audio-run/` bundle, reach for the `pulp audio validate` verbs above. For a
plugin bundle that needs a deterministic offline scenario, use `pulp audio render`.

> Live in-app inspection has landed in `/audio-inspect` and
> `pulp run --audio-inspector`. Live capture-to-WAV has landed in two modes:
> `pulp run --audio-capture-wav` (earliest window, int16 — presence/level/clip)
> and `pulp run --audio-capture-rolling` (last/steady-state window, float, or
> `--audio-capture-rolling-format int24` — the window `doctor`/`compare` want).
> Pick among the fixtures, Audio Scope, the two live captures, `pulp audio
> validate`, and `pulp audio render` according to the window/source you need.
