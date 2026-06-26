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

To add coverage for a new effect, copy the nearest contract fixture in
`test/test_audio_contracts.cpp` (or a Doctor case in `test/test_audio_doctor.cpp`)
and adjust the expectations — don't hand-roll sample loops. For a captured WAV or
an `audio-run/` bundle, reach for the `pulp audio validate` verbs above.

> Live in-app inspection has landed in `/audio-inspect` and
> `pulp run --audio-inspector`. The still-planned harness slices are live
> ring-capture-to-WAV and a scenario-driven `render` verb; until those land,
> use the fixtures, Audio Scope, and the `validate` verbs above.
