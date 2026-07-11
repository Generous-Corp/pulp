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
  image-collapse/phase damage on `(N,2)` stereo arrays; and the EXPERIMENTAL `onset_drift`
  — timing/groove drift via event-time residual after common-latency removal, advisory-only
  until promoted), and
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
- **Maturity gate:** each detector has `maturity` (`experimental`/`beta`/`stable`). An
  `experimental` detector runs and reports under the report's `advisory` block but its
  `fired`/`low_coverage` are excluded from the `verdict` AND `engine_baseline` — route
  every verdict/gate path through `schema.detectors_for_verdict` /
  `detectors_for_engine_baseline`, never re-implement the filter. New detectors ship
  `experimental`. (Plan: planning/2026-06-29-audio-quality-lab-postmvp-detectors-maturity-gate.md.)
- **Perceptual models** (`perceptual.py`, opt-in, license-fenced): full-reference,
  music/general-audio quality predictors, each behind its OWN env-path and each
  `skipped`-independently when absent — set only the ones you want. `evaluate()` consults
  all: ViSQOL (`PULP_VISQOL_BIN`, Apache-2.0, MOS-LQO), PEAQ (`PULP_PEAQ_BIN`, GPL, ITU-R
  BS.1387 ODG), AQUA-Tk (`PULP_AQUATK_BIN`, GPL-3.0, ODG). Results land under
  `report["perceptual"]`, ADVISORY ONLY. Deliberately excludes speech/telephony (PESQ,
  POLQA) and no-reference neural speech (DNSMOS, NISQA) metrics — wrong domain/contract.
- **MIR structural oracle** (`mir.py`, opt-in, license-fenced): aubio (`PULP_AUBIO_BIN`,
  GPL-3.0) — a *feature extractor*, NOT a quality metric, so it is a SEPARATE layer from
  `perceptual.py`. Gives an independent onset-count cross-check under
  `report["advisory"]["mir_oracles"]` to non-circularly validate `onset_drift`. ADVISORY,
  never a gate/baseline. **License fence for both layers:** GPL tools (PEAQ/AQUA-Tk/aubio)
  are reached ONLY via a developer-set env-path over a process boundary, never bundled,
  fetched, linked, or added to `tool-registry.json` — so no copyleft obligation attaches
  and no runtime warning is needed (consistent with how the lab already treats PEAQ).
- **Advisory reviewer** (`reviewer.py`, opt-in `PULP_QLAB_REVIEWER_CMD` subprocess, `run
  --review`): an LLM/multimodal model names artifacts in plain language under
  `advisory.reviewers`. ADVISORY ONLY — never changes the verdict/gate; skip-when-absent;
  no network by default. Validate with `reviewer.score_agreement` (precision/recall vs the
  synthetic answer key) before trusting it.
- **Tuning loop** (`loop.py`, `quality-lab loop`, EXPERIMENTAL): scores candidates with the
  gate-participating detectors (experimental detectors excluded), ranks them, and writes
  PROPOSAL-ONLY label updates to `corpus/LABEL_PROPOSALS.json` — never `MANIFEST.json`,
  never auto-promotes. `goodhart_guard` refuses a candidate that games one detector while
  regressing another (normalized Pareto + held-out slice + NEEDS-EAR).
- **Agent compare report** (`compare.py`, `quality-lab compare before.wav after.wav`): the
  agent-facing **measure → compare → judge** surface. Level-matches, runs one curated **axis**
  (`--profile`), and emits a typed evidence envelope + an action-oriented verdict
  (`regression_suspected` / `material_change_detected` / `no_material_change_detected` /
  `inconclusive` / `invalid`). Six axes today — a new axis is one `_AXES` registry entry in
  `compare.py`, the shared `_measure`/`_verdict` machinery does the rest:

  | `--profile` | axis | measures | bad direction (`bad_sign`) |
  |-------------|------|----------|----------------------------|
  | `tonal-balance` | `tonal_balance` | LTAS spectral-centroid shift | duller (−1) |
  | `added-hf` | `added_hf` | band-relative ≥8 kHz fraction ratio (dB) | added fizz (+1) |
  | `noise-roughness` | `noise_roughness` | harmonic-to-noise ratio drop (dB) | rougher/noisier (−1) |
  | `graininess` | `graininess` | relative spectral-flux increase | grainier (+1) |
  | `stereo-width` | `stereo_width` | RMS(side)/RMS(mid) width + interchannel correlation | narrower/collapsed (−1) |
  | `transient-integrity` | `transient_integrity` | per-onset attack-smear deficit (onset-aligned) | softer/smeared (+1) |

  `noise-roughness` (HNR, pitch range 40–1000 Hz so bass fundamentals count) and `graininess`
  (relative `mean_spectral_flux` increase) are meaningful on tonal/sustained material — a
  caller-declared contract (the caller picks the profile), carried as a standing summary caveat +
  both per-signal scalars in the payload, NOT an automatic tonal/percussive classifier in the
  honesty path (which would just be a tunable threshold on the same statistic). Only mathematically
  degenerate inputs go `not_applicable` (a flux reference below an epsilon floor — the div-by-zero
  edge). **`stereo-width`** is the one axis fed the ORIGINAL 2-channel signal (via
  `audio_io.load_wav_multichannel`, the `_Axis.needs_stereo` flag, and the `_measure_stereo` path)
  rather than the mono downmix; mono input on either side is `not_applicable`, its metric is
  level-invariant (no level-match), and a candidate whose correlation goes negative is flagged out
  of phase (mono-incompatible) in the summary. **`transient-integrity`** (the `_Axis.needs_onsets`
  flag) is "did my DSP soften/smear the attacks?" — it detects+matches onsets (`align.detect_onsets`
  / `map_onsets`), locks each attack (`dsp.local_align`), and compares the high-band attack rise
  (`dsp.attack_rise`, the SAME primitive the `transient_sharpness` detector uses — extracted to dsp
  so there is no forked DSP). One-directional (a softening is the regression; a sharper candidate
  reads no change); needs onset-bearing material (< 3 matched onsets → `not_applicable`); self-aligns
  per onset so `--align` is a no-op and the global sample-domain advisory is suppressed. That takes
  compare to **6 of the lab's 7 detectors** reachable from the agent-facing surface (up from 2).
  `compare.STEREO_PROFILES` / `ONSET_PROFILES` mark the capability-specific axes, and
  `NET_DEFAULT_PROFILES` (the regression net's default) excludes them so they don't emit spurious
  `not_applicable` rows on the wrong material — a suite opts in. `compare.MONO_PROFILES` /
  `compare.STEREO_PROFILES` split which axes need 2-channel input — the regression net defaults to
  the mono set so a mono comparison doesn't emit a spurious not_applicable stereo-width row.

  The `added-hf` axis measures the **ratio of the ≥8 kHz energy fraction** (`10·log10(frac_cand /
  frac_ref)`, default ±3 dB), NOT the absolute fraction *delta* — the absolute delta is
  signal-dependent and effectively blind on a bass-heavy source (an amp render's HF fraction is
  ~1e-4, so even a clearly harsh addition barely moves it; only the null-residual corroboration
  caught it before). The ratio is invariant to a broadband gain / the level-match, so it flags a
  real harshness change on a dark source that the absolute delta missed. Both band fractions are
  floor-clamped, so a zero-HF reference (fizz on a dark source) or candidate (brickwall low-pass)
  gives a large *finite* dB delta, never `inf` → a false `invalid`. It flags HF *loss* as material
  too (a negative dB delta) — only *added* fizz is the bad direction. A large EQ move outside the
  band (e.g. a big LF shelf) still leaves some residual sensitivity — far less than the absolute
  metric, not zero. The threshold is a dB magnitude with its own per-axis range, so the shipped
  CLI / MCP pass any positive threshold through to the Python registry (the single source of truth
  for the valid range) rather than a hardcoded fraction bound.

  **Intent-safe:** `regression_suspected` needs `--reference-role golden` AND a change in the
  axis's bad direction (a duller candidate can't trip the `added-hf` axis; added fizz can't trip
  it on a `peer` compare); otherwise the change is the neutral `material_change_detected`.
  **Advisory:** exits non-zero only on `invalid` (couldn't measure), never for a judgment — the
  gate primitive stays `pulp audio validate compare`. Each axis has its own materiality default
  (`--threshold` overrides). Report shape (`quality_lab.compare.v1`) is owned by `schema.py`;
  every envelope carries `status`/`applicable`/`materiality`/`provenance`. Use this when an agent
  must weigh in on a DSP change with cited evidence rather than a bare pass/fail.

  **Corroboration + raw comparators (`report["advisory"]`, off-gate).** When the primary
  measurement succeeds, the report also carries an `advisory` namespace: a deterministic
  `null_residual` raw comparator (level-matched sample-domain residual RMS relative to the
  reference, `db_rel_reference`; lower = more identical) and a `corroboration` block. Corroboration
  is a **materiality cross-check — NOT a trust score**: it reports only whether that independent,
  algorithm-agnostic residual *also* registers a material change (`corroborated` /
  `not_corroborated`), under the same level-matched global contract. Both are `maturity:
  experimental`, `participates_in_verdict: false` — they NEVER move the verdict. The valuable
  signal is disagreement: `not_corroborated` with `axis_exceeds:false, raw_material:true` means a
  real sample-domain difference this axis can't see (e.g. a pure delay — try another profile);
  `axis_exceeds:true, raw_material:false` means a marginal/phase-only change to treat with more
  caution. **That disagreement is now PROMOTED to the headline** (it used to sit buried in the
  advisory): a top-level `report["headline_flags"]` (always present, empty when there's nothing to
  flag) carries a structured `{flag, detail, expected_for}` — `uncaptured_material_difference` or
  `axis_change_without_residual` — and a matching sentence is appended to `summary`. The flag is
  STRUCTURED, not prose, precisely so the known false-alarm class is machine-suppressible: every
  time/pitch-variant A/B (chorus, phaser, bendr, any modulated effect) legitimately reads
  `not_corroborated` forever because the phase-sensitive residual always disagrees with a tonal
  axis, so `uncaptured_material_difference` carries `expected_for:["time_variant_processing"]` — a
  caller doing time-variant work filters on that rather than treating the flag as noise. It still
  NEVER moves the verdict. Length is handled honestly: the candidate is level-matched over the **common region**
  and the shorter signal **zero-padded** to the longer length, so a dropped/added tail *with
  content* raises the residual on its own (reads material) while trailing *silence* contributes
  ~0 (stays immaterial) — a truncated render can never masquerade as a near-identity match, and
  the sample counts are reported in the comparator's `detail`. The residual is **phase/delay-sensitive and alignment-free by contract**, so it
  corroborates *materiality*, never audibility. Cite it — never treat agreement as a confidence
  score (the plan's explicit non-goal: agreement ≠ trust). Deliberately NOT built: the GPL aubio /
  AGPL Essentia feature-extractor menu — feature extractors are never verdicts, and an env-path
  license fence is disproportionate surface for the value; the pure-numpy residual carries the
  corroboration story license-free.

  **Time-alignment (`--align latency`).** The axes are alignment-free, so a constant delay/offset
  (delay plugin, tempo-sampler start offset) doesn't move the tonal verdict but makes the
  phase-sensitive null-residual false-alarm `not_corroborated`. `--align latency` estimates one
  constant lag (`dsp.estimate_global_lag` — normalized cross-correlation of the attack envelopes),
  trims to a common time base (`dsp.apply_lag_trim`), and measures the aligned pair: a pure delay
  then reads `no_material_change` + corroborated, and a real change *behind* a delay is measured
  cleanly. The outcome is disclosed on the envelope (`alignment: {policy: "fixed-latency-trim",
  lag_samples, confidence, applied}`) and the summary. It **refuses** below a confidence floor
  (records `policy: "not_aligned"`, measures unaligned) — a wrong alignment is worse than none.
  Default `--align none` = zero behavior change. `--align` threads through `compare_files` → CLI →
  MCP (`pulp_audio_compare`'s `align` param) → the `/audio-compare` slash command.

  The `--align` grammar + per-mode dispatch live in **`alignment.py`** (extracted from compare.py in
  Tier 3 / T3.1): `alignment.parse("mode[:param]") → AlignSpec` and `alignment.apply(spec, ref, cand,
  sr, *, needs_stereo, needs_onsets) → (ref, cand, record)`, dispatched through a `_HANDLERS` dict so
  a new warp class is one entry. `alignment.py` imports `schema` + `dsp` (never `compare` — capability
  facts arrive as booleans; no import cycle); the record shape stays in `schema.py`; the prose lives in
  `alignment.describe(record)`. Not to be confused with `align.py` (onset primitives) which the
  onset-anchored warp handlers consume.

  **`--align varispeed:R`** (Tier 3 / T3.1) — a caller-**declared** tape-style speed change (candidate
  = reference resampled by R, pitch+time coupled). A resample is EXACT for this class, so the handler
  resamples the candidate back to the reference length (`dsp.resample_to_length` — a pure-numpy
  Kaiser-windowed-sinc resampler, no scipy) and the WHOLE alignment-free pipeline including the sample
  residual applies (clean varispeed → `no_material_change` + corroborated). It **verifies** the
  declared ratio against the observed duration ratio (±2%) and **refuses** (`not_aligned`) otherwise —
  never resampling to a wrong length. Anti-masking holds (a defect behind the varispeed still flags).
  Honest physical limit: a speed-up (R<1) bandlimits to a lower Nyquist, so near-Nyquist energy is
  legitimately lost (correct varispeed behavior, matches an ideal brickwall to ~1% — not a resampler
  artifact).

  **`--align stretch:R`** (Tier 3 / T3.2) — a declared PITCH-PRESERVING time-stretch (PV/bendr). No
  exact inverse exists, so the axes measure the UNWARPED pair directly (LTAS centroid / HF fraction /
  HNR / width are time-averages → warp-invariant). Two axes are warp-normalized off the ratio, threaded
  via the `AlignSpec`: graininess is `warp_aware` (`_Axis.warp_aware`) and measures the candidate flux
  at `mean_spectral_flux(..., hop_scale=R)` (`spectral_flux.v2-warp` — else a clean stretch reads a
  false "smoother"); corroboration binds to `dsp.ltas_log_spectral_distance_db` (a phase-blind,
  length-independent `ltas_residual` comparator) because the sample residual is invalid across a
  stretch (the null residual is still emitted, marked not-a-corroborator). `_align_stretch` returns the
  pair UNCHANGED after verifying §6.1 (duration within ±3% of R, `R∈[0.25,4]`) and §6.3 that a single
  uniform ratio fits — onset material: MAD of the per-onset residual from the `ref_t·R` prediction
  ≤ 15 ms with coverage ≥ 0.5; sustained: ratio-mapped `smooth_energy_env` correlation ≥ floor — else
  REFUSE (`not_aligned`, "non-uniformly warped"). The `_WARP_MODES` set gates the capability
  short-circuit (a warp is applied/routed for onset/stereo axes, not skipped like a constant lag);
  `_MEASURE_UNWARPED_MODES` = {stretch, pitch} drives the per-axis warp normalization + corroborator
  swap (varispeed is NOT in it — it resamples back, so the standard pipeline applies verbatim).

  **`--align pitch:S`** (Tier 3 / T3.3) — a declared DURATION-PRESERVING pitch shift (S semitones,
  parsed with an optional `st`/`semitones` suffix; `spec.param` is the signed semitones). Time base
  unchanged, so the axes measure the pair directly; **tonal-balance** (also `warp_aware`) compensates
  the LTAS centroid via `dsp.pitch_compensated_centroid_shift` (a perfect shift moves the centroid by
  the pitch ratio → the axis reports deviation-from-expected, `spectral_centroid.v2-pitch`), and the
  corroborator shifts the reference LTAS to its expected position first (`dsp.ltas_logfreq_shift`, via
  the `ref_shift_ratio` arg on `ltas_log_spectral_distance_db`). `_align_pitch` verifies `|S| ≤ 24 st`
  + duration preserved (a length change ⇒ not a pure pitch shift) and refuses otherwise. The warp-aware
  kernels take the accepted `AlignSpec` (`spec=`) and each reads only its own class off `spec.mode`/
  `spec.param` — the generalized threading that replaced T3.2's single `flux_hop_scale` param. Known
  advisory imperfection: the log-frequency LTAS compensation carries bin-interpolation error on
  discrete partials, so a clean shift can read `not_corroborated` (advisory only, never the verdict).
  a non-constant warp / DTW stays on the engine path, not compare.

  **`--align ratio:auto`** (Tier 3 / T3.4) — ESTIMATE a uniform stretch ratio, double-gated (§6.2):
  two independent estimators — the duration ratio and the onset-time slope (`dsp.theil_sen_slope` over
  `map_onsets` pairs, ≥ 6) — must agree within 2%, else REFUSE (`not_aligned`, "ratio estimators
  disagree" / "needs ≥ 6 matched onsets"). On agreement `_align_ratio_auto` delegates to
  `_align_stretch` with the onset-slope estimate (interior evidence) and tags the record
  `estimated=True` + both estimator values. The estimated ratio reaches the warp-aware axes via
  `alignment.effective_spec(spec, record)`, which resolves a `ratio:auto` spec to `stretch:<estimated>`
  so graininess hop-scales off the ACCEPTED transform, not the "auto" request string. Reliable on
  onset-bearing uniform EXPANSIONS; compressions (map_onsets drift) + non-uniform + sustained refuse
  (conservative — declare stretch:R). This is the last declared-warp class; a non-constant warp / DTW
  stays on the engine path, not compare.

  **Honesty disclosures (what compare admits it can't see).** Every axis EXCEPT `stereo-width`
  mean-**downmixes** to mono, so on those a stereo/spatial change (widener, panner, M/S) is
  invisible. When a file was multichannel the report says so on the mono axes:
  `provenance.ref_channels`/`cand_channels`, a `downmix` note on the measurement envelope, and a
  one-line summary clause — a stereo-widener whose mid is unchanged reads `no_material_change` on
  `tonal-balance` but the report never hides that imaging was discarded, and pointing the same pair
  at `--profile stereo-width` catches the change directly (the downmix note is correctly suppressed
  there, since that axis DID read the stereo image). The **`added-hf`** axis reports
  `not_applicable` (→ `inconclusive`) at low sample rates where the ≥8 kHz band collapses to a
  degenerate handful of LTAS bins (e.g. sr = 16 kHz is the single Nyquist bin) instead of a
  confident "no change" over nothing. A **DC offset** concentrates energy in LTAS bin 0 and drags
  the centroid down — reading as false tonal "dulling" — so the advisory carries a `dc_offset`
  raw comparator (per-signal means + `present` flag) and, when present, a summary sentence telling
  you to high-pass before comparing rather than chase phantom dullness. All three are
  presentation/advisory only — like corroboration, they never move the verdict.
- **Shipped surfaces (same measurement, three entry points):** the Python `quality-lab compare`
  is the dev-loop surface; **`pulp audio compare <ref> <cand> [--profile …] [--reference-role …]
  [--json …]`** is the shipped CLI verb (thin delegator — `tools/cli/cmd_audio_compare.cpp` locates
  the opt-in managed tool and forwards, so no DSP links into the CLI); **`pulp_audio_compare`** is
  the MCP tool mirroring it (returns the report JSON). All three run the identical axis logic. The
  `/audio-compare` slash command wraps the CLI. Install the tool once: `pulp tool install
  audio-quality-lab`.
- **Golden-render regression net (`quality_lab.regression_net`, `quality-lab regression-net
  --manifest net.json`).** The daily-driver loop: keep a golden render per plugin/preset, `pulp
  audio render --plugin …` the candidate, and `compare` across every wired axis, emitting a table
  (plugin | profile | verdict | corroboration | flags). **Fail policy is the contract: the fail
  signal keys off axis verdicts only — exit 1 iff an axis reports `regression_suspected`; a pair
  that couldn't be measured (missing/corrupt render → `invalid`, or a bad manifest) is a separate
  ERROR (exit 2), never greenlit as clean; exit 0 = all measured, none regressed. The corroboration
  column is informational and never affects the exit code** — the modulated family
  (chorus/phaser/flanger/tremolo/ring-mod) is time-variant
  so it reads `not_corroborated` forever (the `expected_for:["time_variant_processing"]` flag). This
  is advisory reporting attached to a change; gating proper stays `pulp audio validate compare`. The
  module is the reusable REFERENCE — a plugin suite (pulp-classic-effects, bendr, GPU NAM) wires its
  own renders into `run_net` and commits a portable manifest (paths resolve relative to it). Read
  the change class, not the backend: timbral + modulated changes are valid through the net; bendr
  *fixed-ratio* A/Bs are valid, but bendr ratio changes / tempo-sampler offsets are time-misaligned
  (the residual false-alarms) → use the reference-free engine path instead. See the guide's
  "Golden-render regression net" section.
- **Live plugin before/after (measure a real plugin, then judge the change).** `compare` needs two
  WAVs; capture them from a live/hosted plugin with the existing steady-state tap, then judge:
  ```bash
  # capture the KNOWN-GOOD baseline (announce audio + cap duration per the local-dev etiquette)
  pulp run --plugin before.clap --audio-capture-rolling before.wav --headless   # tear down promptly
  # …apply the DSP change, rebuild…
  pulp run --plugin after.clap  --audio-capture-rolling after.wav  --headless
  pulp audio compare before.wav after.wav --reference-role golden               # base = golden
  ```
  Use `--audio-capture-rolling` (last-N steady-state window — the region `compare` wants), NOT
  `--audio-capture-wav` (earliest int16 window). `--audio-capture-rolling` opens the live audio
  device: announce before launching, cap the wall-clock, and tear down (see the local-dev audio
  etiquette in CLAUDE.md). For a fully offline capture of an explicit bundle, `pulp audio render
  --plugin <bundle> --out cand.wav …` writes a WAV without a live device — feed that to `compare`
  the same way. This is the "measure a plugin with the scope/inspector and compare findings" path.
- Guide: `docs/guides/audio-quality-lab.md`; module map + deferred-detector status:
  `tools/audio/quality-lab/README.md`.

### Compare/measure design rationale & non-goals (why we built it this way / what we skipped)

Durable "why / watch-out-for" notes so this isn't re-litigated (rationale, not workflow history):

- **A vertical agent report before a shared measurement record.** The harness + lab + inspector
  already measure; the gap was orchestration + a *defended verdict*. A universal `{ref,cand,delta}`
  scalar is too lossy to defend a judgment, so measurements are typed *evidence envelopes* and
  curves/THD stay first-class. (Two independent adversarial reviews converged on this.)
- **Cross-tool "agreement = trust" is NOT a signal** and is deliberately absent — different
  algorithms disagree for legitimate reasons; treating agreement as trust manufactures
  overconfidence. If ever added, it reads as "corroborated under a comparable-axis contract."
- **Doctor `--response`/`--thd` are NOT valid on arbitrary before/after WAVs** — `--response` is a
  peak-normalized self-spectrum (not a transfer response); THD is gate-grade only for a steady
  bin-coherent sine. They need a controlled stimulus, so they're excluded from `compare` and join
  later behind stimulus-supplying profiles.
- **Alignment is `not_required` for both current axes** — global LTAS metrics (centroid shift,
  HF fraction) are alignment-free; the envelope records the *policy*, we don't build general
  alignment for them. A future transients/timing axis will need a real alignment policy first.
- **Axes are a per-profile registry, added one entry at a time** — each axis declares its
  `axis`/`tool`/`bad_sign`/`default_threshold`/`kernel`/`summarize`; the shared `_measure` does
  applicability + level-match + materiality and `_verdict` does the intent-safe mapping. `bad_sign`
  is the sign of the delta that is a regression against a golden reference (−1 = a drop is bad,
  +1 = an increase is bad). Only axes on the *same contract* (global, delta-in-(0,1), self-
  applicability) belong here — stimulus/alignment-dependent measures do NOT.
- **`regression_suspected` requires `--reference-role golden`** — "worse" assumes the reference is
  the known-good baseline; without that, a change is neutral. It ALSO requires the change to be in
  the axis's bad direction (`bad_sign`), so a legitimately-brighter or HF-reduced candidate reads
  as `material_change_detected`, not a false regression.
- **Axis thresholds are per-axis and unit-bound to (0, 1)** — every current axis's delta is a
  dimensionless ratio in (0, 1), so the threshold guard is `0 < t < 1`. A dB-unit axis (e.g. HNR
  drop) is deliberately deferred until the threshold validation is made per-axis; don't shove a
  dB metric onto the (0,1) contract.
- **Speech/perceptual & feature-extractor scope is bounded** — PESQ/POLQA/DNSMOS/NISQA are speech/
  telephony (out of scope for full-reference music); aubio/librosa/Essentia are feature extractors,
  not MOS predictors (advisory raw-comparators behind profiles, never verdicts); scalar-only where
  the scalar is the calibrated deliverable (PEAQ/AQUA-Tk ODG). GPL/AGPL tools stay behind the
  env-path license fence.
- **`audio_io.load_wav` mono-downmixes** — fine for tonal balance; a future stereo-image axis must
  NOT use that loader.

## Proving reported latency

`expect_reported_latency(result)` measures the delay actually present in a
render and compares it against `Processor::latency_samples()`. This is the
contract for any processor with nonzero latency: the host slides the whole track
by the number the plugin reports, and nothing else checks it. `RenderScenario`
collects the report facts on **every** render (`ScenarioResult::latency`), so a
contract can ask for the proof without re-rendering.

The schema and both evaluators live in `pulp::audio-analysis`
(`latency_evidence.hpp`) as pure functions over buffers, so the test harness, the
`pulp` CLI, and MCP all produce the same verdict. Add a policy there, never in
`test/support`, or the surfaces drift.

**Feed it a broadband, aperiodic stimulus.** `make_white_noise` for the
delayed-null policy, `make_impulse` for the marker policy.

**The refusals are the feature, not a limitation.** A stimulus that cannot pin
the delay down returns `not_measurable` / `inconclusive`, and an inconclusive
result that was *asked for* fails the gate — an unprovable claim is a failed
claim, never a silent pass. Expect a refusal when:

- the stimulus is silent, or the output is not a delayed copy of the input (the
  delayed-null policy needs the processor in a declared identity / bypass /
  fully-dry mode — set that up with `set_param`);
- the stimulus is **periodic on an integer sample count**. This is the one that
  bites. Tile-a-cell noise with period 100 against a true delay of 512 makes
  delays 12, 112, 212… bit-identical; the sweep's argmin walks upward and would
  confidently return **12**, falsely accusing an honest processor of misreporting
  by 500 samples. The guard catches competing minima and refuses.
  Note this is about *integer-sample* periodicity, not tonality — a 440 Hz sine at
  48 kHz has a 109.09-sample period, so its delay IS recoverable and is proven;
- the processor's report *moved* mid-render, including moving away and back. The
  harness watches `consume_latency_changed_flag()` as well as polling the value,
  because a host notified mid-render has already acted on the intermediate number.
  (Consuming the flag is safe here: a headless render has no adapter to steal it
  from.)
