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
auto thd = measure_thd(sc, /*fundamental_hz=*/999.0); // steady bin-coherent sine
// thd.thd_percent(), thd.thd_plus_n, thd.harmonics[...]
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

### Launch the Audio Inspector in the standalone host

`StandaloneApp::run_with_editor()` wires the inspector to the host's
output-boundary probe automatically (behind `PULP_ENABLE_AUDIO_PROBES`, default
ON for dev/examples). At runtime, toggle it with **Cmd/Ctrl+Shift+A**, or open it
on launch by setting `PULP_AUDIO_INSPECTOR` in the environment:

```bash
# Live: feed a test signal so output is non-silent, then open the inspector.
PULP_AUDIO_INSPECTOR=1 ./build/examples/<app>/pulp-<app>
```

CLI shortcuts (resolve the standalone binary + set the env vars for you):

```bash
pulp run --audio-inspector                    # open the live inspector window
pulp run --audio-probe-json /tmp/probe.json   # headless: dump probe JSON + exit
pulp run --audio-scope-json /tmp/scope.json   # headless: dump live sample-window scope JSON + exit
pulp audio scope --input-wav out.wav --json /tmp/scope.json --png /tmp/scope.png
```

For MCP clients, use the existing `pulp-mcp` tools instead of creating a new
MCP server. `pulp_audio_probe_json` is a one-shot wrapper around
`pulp run --audio-probe-json`, accepts optional `target` and `frames`, and
returns scalar probe counters as `structuredContent`. `pulp_audio_scope`
returns `pulp.audio.scope.v1` sample-window acquisition/measurements; live
target mode may open the audio device, while `input_wav` mode is speakerless
offline and can also write a PNG artifact.

Agent triage pattern:

1. Start with `pulp_audio_probe_json` for the target you are debugging.
2. If the tool errors or no JSON is written, report a launch/probe problem, not
   a DSP conclusion.
3. If `callbacks == 0`, increase `frames` or inspect standalone startup/device
   lifecycle.
4. If callbacks advance but `peak_max == 0` and `rms_max == 0`, treat the
   observed output boundary as truly silent; debug routing, input stimulus,
   bypass/mute state, graph wiring, or the processor output branch.
5. If `clip_count`/`clipped_blocks` or `nan_inf_count`/`nan_blocks` are non-zero,
   prioritize DSP/gain/state initialization bugs.
6. If the live snapshot is healthy but the user says the sound is wrong, switch
   to `pulp_audio_scope` / `pulp audio scope --input-wav` for sample-window
   facts, or to an offline render + Audio Doctor for THD/response/residual
   checks. The scalar probe cannot prove THD, response, phase, latency, or
   perceptual quality.

`--audio-probe-json` is the **programmatic readout** for agents: it writes
`output_probe().latest()` (+ the `AudioStats` subset) as a flat JSON object —
`stage`, `sample_rate`, `block_size`, `channel_count`, `sequence_number`,
`peak_max`/`rms_max`, `peak_dbfs`/`rms_dbfs` (null on true silence),
`clip_count`, `nan_inf_count`, `clipped_blocks`, `nan_blocks`,
`silence_run_blocks`, `callbacks`, then exits. The mapping is the pure
`pulp::audio::audio_probe_snapshot_to_json()` helper
(`pulp/audio/audio_probe_json.hpp`); the frame delay reuses `--frames` /
`PULP_FRAMES`. This is the *live* counterpart to the offline
`pulp audio validate` Doctor below. See `docs/guides/audio-inspector.md`.

`PULP_AUDIO_INSPECTOR` also enables the probe's capture ring (sized to the panel
display width), so the inspector paints a live *waveform* and not just meters —
the default probe config is summary-only. The toggle routes through a
shell-owned `CommandRegistry` via `route_global_keys` (the root view's
`on_global_key`), which is independent of the layout inspector's `on_global_click`
(Cmd/Ctrl+I) — both tools coexist on one window without clobbering.

Headless proof: a screenshot run with the inspector open also captures the
inspector's OWN window surface next to the main screenshot:

```bash
# Writes /tmp/x.png (main) AND /tmp/x.audio-inspector.png (the live panel).
PULP_AUDIO_INSPECTOR=1 ./build/examples/<app>/pulp-<app> \
  --screenshot /tmp/x.png --screenshot-frame-delay 90
```

The sibling `<stem>.audio-inspector.png` is only written when the inspector
window is visible and the host has GPU capture (`WindowHost::capture_png()`).

## CLI: `pulp audio validate <verb>` (offline, over WAVs / artifact bundles)

The harness analyzers are also reachable from the shipped CLI, nested under the
existing `pulp audio` command (the `model`/`excerpt-find`/`read-bundle` verbs are
untouched). The CLI is **not** tied to a plugin, so it analyzes captured audio
files and stored `audio-run/` artifacts — not live processor instantiation
(controlled-stimulus render stays the test-side `RenderScenario`). It links the
reusable `pulp::audio-analysis` lib (`tools/audio/analysis`, namespace
`pulp::test::audio`), the same file-analysis code the test harness uses; no
`test/` library is linked into the CLI and no FFT leaks into a runtime build.

```bash
# Agent-readable signal summary (peak/RMS/DC/dominant pitch); --json for machine output
pulp audio validate summarize out.wav [--json]

# Offline Audio Doctor: THD/THD+N and/or spectrum magnitude at checkpoints.
# Writes a schema-versioned JSON curve artifact to the temp dir.
pulp audio validate doctor out.wav --thd [--fundamental 1000]
pulp audio validate doctor out.wav --response 100,1000,8000

# Null/spectral diff verdict (exits nonzero past tolerance)
pulp audio validate compare before.wav after.wav [--mode null|spectral] [--tolerance -120]

# Re-check a stored assertions.json (or an audio-run dir holding one); nonzero on failure
pulp audio validate assert audio-run/assertions.json
```

`assertions.json` schema: `{"schema_version":1,"assertions":[{...}]}` where each
entry has a `check` (`not_silent`, `silent`, `no_nan_inf`, `peak_below`,
`frequency_near`), a `file` (relative to the JSON), and the check's named
tolerance (`min_rms_dbfs`, `ceiling_dbfs`, `expected_hz` + `tolerance_cents`,
...). The `/audio-harness` slash command documents these verbs and points at the
offline render path below.

## Offline plugin render (`pulp audio render`)

`pulp audio render --plugin <bundle> --out <file.wav> (--duration-ms <n> |
--duration-frames <n>)` is the scenario-driven counterpart to `validate`: it
loads an explicit plugin bundle through `pulp::host::PluginSlot` (the generic CLI
has no registered factory, so a bundle is the only offline render source),
drives it block-by-block from declarative flags, writes an int16 WAV, and emits
the same `pulp::audio-analysis` metrics JSON as `validate summarize --json`
(`--manifest <file>` / `--json`). No DAW, no audio device. Drive it with
`--input-signal silence|sine:<hz>[,<dbfs>]` or `--input <file.wav>` (used as-is
at `--sample-rate` — no resampling), `--param <id>=<value>[@frame]`, and
`--midi note:<note>,<vel>,<on>[,<off>]`.

**`--param` values are PLAIN domain** (the parameter's native `min..max`), **not
normalized `[0,1]`** — matching `PluginSlot::set_parameter` /
`ParameterEvent::value`. Parameters are delivered **sample-accurately**: the
stepper windows each block's events with per-block sample offsets and the queue
is forwarded straight to `PluginSlot::process`, which every loader applies at the
event offset (CLAP/VST3/AU sample-accurate; LV2 block-rate by its control-port
contract). We do NOT also call `set_parameter` — that would double-apply each
change (once at offset 0, once at its real offset). MIDI is likewise
sample-accurate. (A plugin that reads its own params once per block still steps
at block boundaries — that is the plugin's rate, not the CLI's.) The block
stepper is a deliberate, callback-driven parallel to `OfflineRenderHost::render`
(PluginSlot has no `ProcessContext`, so it can't reuse the core renderer
directly); a block-partition-invariance test guards the two against drift.

## Live capture-to-WAV — two modes, both LANDED

`pulp run` taps the standalone's output boundary into a WAV the offline `pulp
audio validate` verbs read, then exits. Pick the mode by which window you need:

- **`--audio-capture-wav <file>` (earliest, int16).** Dumps the EARLIEST window
  after the stream starts (drop-on-full FIFO). Robust for `validate summarize` /
  `assert` (presence / level / clip / NaN); the wrong window for steady-state
  `doctor` and quantization-limited for `compare`. `--audio-capture-frames <n>`
  sets the window.
- **`--audio-capture-rolling <file>` (last-N, float or int24).** Keeps the LAST
  (steady-state) window in a `RollingAudioCaptureBuffer` and writes a **float**
  WAV (no int16 floor) — the window `doctor` (THD/response) and `compare`
  (sub-−96 dBFS residuals) actually want. `--audio-capture-rolling-frames <n>`
  sets the window; `--audio-capture-rolling-format int24` swaps the float WAV for
  int24 (≈ −144 dBFS floor, smaller, universal DAW compatibility). Uses the hold
  protocol so the off-RT materialize is safe while the audio thread is still
  appending. One capture mode per invocation (mutually exclusive with
  `--audio-inspector` / `--audio-scope-json` / `--audio-capture-wav`).

WAV writing is `pulp::audio::write_wav_file(path, data, WavBitDepth)` —
`Int16` (default overload), `Int24`, or `Float32`.

## Roadmap

The Phase-7 offline-render and live-capture slices have all landed: `pulp audio
render` (offline plugin render, sample-accurate `--param @frame`), `pulp run
--audio-capture-wav` (earliest-window int16), and `pulp run
--audio-capture-rolling` (last-N, float or int24). The live realtime output tap
they read from is gated behind `PULP_ENABLE_AUDIO_PROBES` (see *Live inspection*
above). The harness's offline/live capture surface is feature-complete; further
work is open-ended (e.g. additional analysis verbs), not a tracked backlog.

## Sibling: the Audio Quality Lab (reference-vs-candidate perceptual artifacts)

This skill covers presence / level / THD / response. For **reference-vs-candidate
perceptual artifact** detection — "did this DSP change make it sound *worse*?"
(transient smear, dulling, metallic fizz, graininess) — there is a separate **opt-in**
developer/CI tool, `tools/audio/quality-lab/` (Python, numpy + soundfile). It is
additive: it does NOT change anything here and is never required to run the basic
harness or `ctest`.

- **Install (managed):** `pulp tool install audio-quality-lab` — provisions an isolated
  venv under `~/.pulp/tools/` (the same `pulp tool` lane as `ffmpeg`/`uv`/importers;
  `pulp tool list` shows it). Then `pulp tool run audio-quality-lab -- <args>`. A plain
  `cd tools/audio/quality-lab && pip install -r requirements.txt` venv works identically.
- **Subcommands** (after `python -m quality_lab.cli` or `pulp tool run audio-quality-lab --`):
  `run --case {drum,tonal} --degradation <kind>` (synthetic case + listenable clips),
  `engine [--input <wav>] --character <c>` (validate the REAL stretch engine, reference-free
  on a dry input), `engine-baseline` (regression gate: did an engine change make it worse?),
  `corpus list|add` (versioned, license-guarded corpus).
- Aligns a candidate to a reference (onset-map + local cross-correlation), runs the
  detectors (`transient_sharpness`, `spectral_centroid`, `hf_fizz`, `spectral_flux`, `hnr` —
  tonal noise/roughness via autocorrelation HNR; plus the standalone `stereo_width` for
  image-collapse/phase damage, called directly on `(N,2)` stereo arrays), and
  writes a `report.json` with per-onset localization, coverage/confidence, and provenance.
- Credibility: detectors are validated against an *independent* textbook phase vocoder
  (`reference_pv.py`) AND the real product engine, not just their own synthetic degradation.
- **`engine`/`engine-baseline` need a built `stretchcli`.** `engine.resolve()` finds it via,
  in order: the `PULP_STRETCHCLI` env-path, a `build/examples/offline-stretch/stretchcli`
  found by **walking up from the current directory** (so the engine path works even when the
  lab is `pulp tool install`-ed into a managed venv, as long as you run it from a Pulp
  checkout that built stretchcli), then the package-relative repo build. Absent → a clean,
  actionable `skipped` (build command + env-path), never a failure. Build it with
  `cmake --build build --target stretchcli`.
- Guide: `docs/guides/audio-quality-lab.md`; module map + deferred-detector status:
  `tools/audio/quality-lab/README.md`.
