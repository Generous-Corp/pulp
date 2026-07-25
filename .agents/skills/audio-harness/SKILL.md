---
name: audio-harness
description: The measurement surface for ALL Pulp DSP and audio-pipeline work — read it BEFORE writing or gating DSP, not only when something already sounds wrong. Covers the C++ harness (signal generators, metrics, assertions, RenderScenario, contracts), the offline Audio Doctor (magnitude/frequency response, THD/THD+N, phase/group delay), and their Python sibling the Audio Quality Lab (tools/audio/quality-lab — null residual + alignment, LTAS log-spectral distance, spectral flux/centroid, HNR, Theil-Sen drift slope, Kaiser-sinc resampling, license-guarded corpus, regression-net ratchet). TRIGGER on AUTHORING work — "build/design an oscillator/filter/synth/effect", "add a DSP module", "what should the acceptance gate be", "how do I measure aliasing / anti-aliasing / alias floor", "null against a reference", "is this DSP correct", "choose a tolerance", "golden/regression corpus for audio", "measure drift or jitter", "A/B two renders" — AND on DEBUGGING work — "is there sound / no audio / I hear nothing", "does this filter/compressor/synth/delay produce the right signal", "prove the DSP / prove the contract", "measure the frequency response", "what's the THD / is it distorting", "what's the group delay / phase response / measured latency", "magnitude response curve", "render a test tone and assert", "audio regression", "64-frame works but 128 is silent", "sample-rate change pitch-shifted it", "describe what's in this buffer", "audio doctor", "compare before/after a DSP refactor". Reach for this BEFORE hand-rolling any FFT, null test, alias measurement, pitch tracker, or golden-render script — most of it already exists in one of the two lanes. Test/tool layer over HeadlessHost — deterministic, no audio device, no speakers. Off the realtime thread entirely.
---

# Audio harness (observability + validation)

Pulp's agent-first way to turn "I can't hear it" / "does this sound right?" into
**inspectable, deterministic signal evidence** — without a device, speakers, or a
debugger. You are reading this skill because you need to prove, measure, debug, or
regression-guard the audio a Pulp `Processor` emits.

Everything here is the **test/tool layer**, driven by
`pulp::format::HeadlessHost`. Nothing in this skill runs on the realtime audio
thread — measurements analyze buffers that have already left `process()`. The
realtime probe path is a separate concern (see *Roadmap*).

It lives in **two** places, and the split is load-bearing:

- **`tools/audio/analysis/`** — the `pulp::audio-analysis` lib (headers under
  `<pulp/audio/analysis/…>`). The pure buffer/file analysis: metrics,
  assertions, artifacts, and the FFT spectrum analyzers. It knows nothing about
  `Processor`, so the shipped `pulp audio validate …` CLI links it directly
  without pulling in any `test/` library.
- **`test/support/`** — the layers that need a `Processor`: signal generators,
  `RenderScenario`, contracts, and the scenario-driven Doctor wiring. Test-only;
  links the lib above.

Both use namespace `pulp::test::audio`, so the namespace does NOT tell you which
target a symbol comes from — the include path does.

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
- "What's the group delay / latency of this filter?" — a phase + group-delay
  curve, in samples or seconds, per frequency.

## The layering (each layer builds only on the ones below — no back-edges)

```
                    ── pulp::audio-analysis lib (also linked by the pulp CLI) ──
metrics → assertions → artifacts → spectrum
                    ── test/support (needs a Processor) ──
signal generators → scenarios → contracts → doctor
```

| Layer | Header | Target | What it gives you |
|-------|--------|--------|-------------------|
| Metrics | `<pulp/audio/analysis/audio_metrics.hpp>` | lib | `analyze()` → `BufferMetrics`: peak, RMS, DC, NaN/Inf, clip count, silence-run; `estimate_frequency()` (zero-crossing, documented limits); `to_dbfs`; `summarize()` (agent-readable signal description). |
| Assertions | `<pulp/audio/analysis/audio_assertions.hpp>` | lib | `assert_no_nan_inf / not_clipped / silent / not_silent / peak_between / rms_between / frequency_near / null_near / channels_independent` — each returns `CheckResult{passed,message}` with dBFS/Hz/cents messages, never a bare float. |
| Artifacts | `<pulp/audio/analysis/audio_artifacts.hpp>` | lib | `BufferMetrics` → JSON (`schema_version` + provenance) for failing CI/local runs. |
| Spectrum | `<pulp/audio/analysis/audio_spectrum.hpp>` | lib | The FFT-bearing core of the Doctor, over **already-rendered buffers** (no `Processor`): `response_relative_to_input(input, output, …)`, `magnitude_spectrum_curve()`, `measure_thd(signal, …)`, `measure_group_delay(input, output, …)` (phase + group delay), plus the shared `ResponseCurve` / `ThdResult` / `PhaseCurve` / `Window` types. This is what the `pulp audio validate doctor` CLI runs over decoded WAVs — and what you call directly when your audio came from somewhere other than a scenario (e.g. rendered through a real format adapter). |
| Doctor artifacts | `<pulp/audio/analysis/audio_doctor_artifacts.hpp>` | lib | `write_response_artifact()` / `write_thd_artifact()` / `write_phase_artifact()` + the JSON serializers (`phase_curve_to_json`); `kDoctorCurveSchemaVersion`. |
| Latency evidence | `<pulp/audio/analysis/latency_evidence.hpp>` | lib | Delayed-null + impulse-marker latency policies as pure functions (see *Proving reported latency*). |
| Generators | `"support/audio_test_signals.hpp"`, `"support/audio_signal_generators.hpp"` | test | Deterministic stimulus: sine/square/saw, impulse(+train), step, DC, multi-sine, swept sine, seeded white/pink/brown noise, stepped automation + MIDI note scripts. No clocks, no `random_device`. |
| Reverb | `"support/reverb_metrics.hpp"` | test | `t60_schroeder` / `band_t60` (Schroeder backward integration, T20-extrapolated), `echo_density_curve` / `mixing_time_seconds` (Abel & Huang), `range_rms`, `band_energy`. Header-only; measures a reverberant DECAY, which is why it is not in the analysis lib (see *Measuring a reverb* below). |
| Scenarios | `"support/render_scenario.hpp"` | test | `RenderScenario` builder over HeadlessHost (factory, sample rate, block size, channels, duration, input/MIDI/param scripts); `render()` → `ScenarioResult`. `run_matrix()` (SR × block sweeps) + `assert_block_partition_invariant()`. |
| Contracts | `"support/audio_contracts.hpp"` | test | `AudioContract` — a named claim + scenario + accumulated `CheckResult`s; failures read `contract '<name>': ...`. Family helpers `expect_{passthrough,silence_preserved,tone,finite_and_unclipped}`. |
| Doctor (offline) | `"support/audio_doctor.hpp"` | test | The **scenario-driven** entry points: `response_relative_to_input(scenario, …)`, `measure_thd(scenario, …)`, and `measure_group_delay(scenario, …)` (its results gated by `defined_at(hz)` for group delay and the stricter `phase_defined_at(hz)` for phase) synthesize the stimulus, drive the `Processor`, and delegate the math to `audio_spectrum.hpp`. It re-exports that header's result types, so a test that includes it needs nothing else. |

Two traps this table exists to prevent:

- **`response_relative_to_input` and `measure_thd` are each overloaded across
  both targets.** The scenario overloads (`audio_doctor.hpp`) drive a
  `Processor` and synthesize their own stimulus, overriding the scenario's
  input/duration. The buffer overloads (`audio_spectrum.hpp`) analyze audio you
  already rendered and synthesize nothing. Same names, different layers — pick
  by where your audio comes from.
- **The heavy FFT lives in `pulp-audio-analysis` and is never linked into a
  runtime/plugin build.** That is enforced by a CMake target boundary, so a
  violation is a link error rather than a review comment. Do not add FFT code
  anywhere else.


Read `test/support/README.md` for the authoritative layering contract.

## Run the proofs

```bash
# Build + run the whole harness (Release — Debug is meaningless for DSP timing/levels)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
tools/ci/governed-build.sh cmake --build build --target \
  pulp-test-audio-support pulp-test-render-scenario pulp-test-audio-contracts \
  pulp-test-audio-doctor pulp-test-adapter-audio-parity pulp-test-golden \
  pulp-test-audio-matrix pulp-test-audio-tone-regression pulp-test-latency-contract
ctest --test-dir build -R 'audio|golden|render|contract|doctor' --output-on-failure
```

That ctest regex matches on test-case NAMES, so it is deliberately wide — it
sweeps in several hundred unrelated `render`/`contract` cases from other
subsystems. It is the right net when you want "did I break anything audio-
shaped"; when you want just the harness, run the built binaries directly
(`./build/test/pulp-test-audio-doctor`) or filter by tag (`-R` won't do tags —
pass `'[doctor]'` to the binary itself).

The `/audio-harness` slash command wraps this. JSON metric/curve artifacts (on failure or
on demand) land under a temp `pulp-audio-metrics/` dir and are INFO-logged.

## Measuring a reverb — two traps that make a working engine look broken

Both of these cost real debugging time on the multirate FDN reverb, and both
produce a *plausible* wrong number rather than an obvious one.

**Measure decay IN A BAND, not broadband.** A broadband Schroeder integration
folds the top two octaves into the number, and any fractional-delay read in a
feedback loop — which is what delay modulation requires — costs real
high-frequency energy per pass. On that engine the cost is 19% of the 10 kHz
decay against 3% at 1 kHz, so the broadband T60 moved 10.6% when modulation was
switched on and modulation looked like a decay control. It is not one. Use
`band_t60(ir, fs, 1000.0)`; state the probe frequency in the test.

**A proportional band's energy is not a level — normalize by bin count.** A
+/-12% band at 8 kHz is 32x wider than the same band at 250 Hz, so summing its
bins builds a ~15 dB tilt into any band-to-band comparison and a perfectly flat
tail reads as 16 dB of spread. `band_energy` returns the RMS across its bins (a
density) for exactly this reason. If you hand-roll a band measure, do the same.

A third, cheaper one: **use a band, not a bin, for anything granular.** A
granular pitch shifter spreads its output into grain-rate sidebands, so a single
880 Hz bin under-reports a working octave-up shifter by an order of magnitude
and can read as zero.

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

Measure it like Plugin Doctor (offline Doctor, scenario-driven):

```cpp
auto curve = response_relative_to_input(sc, {50.0, 8000.0});
REQUIRE(curve.attenuation_db_at(8000.0) >= 20.0);   // "drops ≥20 dB at 8 kHz"
auto thd = measure_thd(sc, /*fundamental_hz=*/999.0); // steady bin-coherent sine
// thd.thd_percent(), thd.thd_plus_n, thd.harmonics[...]

// Phase / group delay. ALWAYS gate first: a stopband has no phase to measure,
// so the analyzer reports it undefined and the accessors return NaN rather than
// a number read out of the noise floor. TWO gates, and they are not the same —
// defined_at() qualifies group delay, phase_defined_at() qualifies phase.
auto gd = measure_group_delay(sc, {100.0, 8000.0});
if (gd.defined_at(100.0))
    REQUIRE(gd.group_delay_samples_at(100.0) < 8.0); // "≤ 8 samples latency"
if (gd.phase_defined_at(100.0))                      // STRICTER — see below
    REQUIRE(gd.phase_radians_at(100.0) < 0.0);
// gd.group_delay_seconds_at(hz)
// gd.magnitude_db_rel_peak_at(hz) — peak-relative, NOT the absolute out/in
// ratio that ResponseCurve::magnitude_db_at returns. The two curves use
// different names for this reason; a +12 dB passband reads +12 there and 0 here.
```

Measure audio that did NOT come from a scenario — e.g. rendered through a real
format adapter, or decoded from a WAV. Drop to the buffer overloads in
`audio_spectrum.hpp`; you supply the stimulus AND the render, so nothing is
synthesized for you:

```cpp
#include <pulp/audio/analysis/audio_spectrum.hpp>
const auto impulse  = /* your stimulus  */;
const auto rendered = /* what came back */;
const double checkpoints[] = {50.0, 1000.0, 8000.0};
auto curve = response_relative_to_input(impulse.view(), rendered.view(),
                                        48000.0, checkpoints, {.fft_length = 16384});
```

`test/test_adapter_audio_parity.cpp` is the worked example of driving the real
adapter: it renders one Processor through `clap_process()` and through
HeadlessHost and requires the bits to match.

**Measure the path the code under test actually runs.** Driving `clap_process()`
is not by itself proof that you measured the adapter. A `clap_process_t proc{}`
leaves `out_events` null, and the whole output-parameter publication phase sits
behind `if (out_events)` — so a test that never sets it renders audio through the
adapter while silently skipping the param path, and stays green no matter how
badly that path breaks. Before trusting an adapter measurement, check that the
lines you care about are reachable from your fixture, then break them on purpose
and confirm your test goes red.

**Two things that will bite you when measuring an adapter path:**

- **THD is blind to anything non-harmonic.** It sums energy at integer multiples
  of the fundamental only. Chop each 256-frame block in half and you amplitude-
  modulate the tone at 187.5 Hz; the sidebands land *between* harmonics, so THD
  reads a serene −170 dB while the signal is visibly destroyed. `thd_plus_n`
  counts every non-fundamental bin and reads ~0 dB (100%). Assert THD+N against
  a stated floor, not just `thd_plus_n >= thd` — that comparison is a tautology
  and proves nothing.
- **Response and THD are both RATIOS, so they cannot see a broadband gain
  error.** A wrong-by-0.5 dB adapter path passes every Doctor check. Level is a
  null test's job.

**Group delay is a derivative, so the estimator matters.** `measure_group_delay`
uses the ramped-signal Fourier identity (`τ = Re{X_r·conj(X)}/|X|²`, `X_r =
FFT(n·x[n])`) rather than differencing an unwrapped phase curve: it needs no
unwrapping, has no frequency step to tune, and is exact for any impulse response
that fits inside the rectangular analysis window. Budget `fft_length` so the IR
has decayed within it — a truncated IR is the one real error source, and the
analyzer will faithfully report the delay of the *truncated* signal.

**`phase_defined` is stricter than `defined`, and you want the strict one for
phase.** Reported `phase_rad` IS unwrapped, and unwrapping is a walk from DC
upward — so a bin's phase is only as trustworthy as every bin *below* it, and
past a stopband null or a gap in the reference spectrum the accumulated offset
lands on an unresolved 2πk branch. `defined` cannot see that: it asks only
whether *this* bin has energy, which is the whole precondition for group delay
(estimated per bin, never consulting the unwrap) and not nearly enough for
phase. So a bin can be `defined` — carrying a real, correct group delay — while
its phase is unreachable. `phase_defined` is the gate that says so, and
`phase_radians_at()` is NaN wherever it is false. Expect exactly this on a
band-limited or gapped reference: full group delay across the band, phase only
up to the first gap. (The separate aliasing bound — phase unwraps correctly only
while `group_delay < fft_length / 2` — is *not* gated: per bin the analyzer
cannot tell an aliased phase from a true one, so it is your budgeting duty.)

**`defined` is not only a stopband test — it also rejects a near-silent
output.** The estimator divides by `|X|²`, so it has an energy floor of its own
below which a bin has no phase to differentiate. A processor whose output has
collapsed (a dead gain stage, a degenerate filter) can still produce a
*perfectly shaped* impulse response, just at −160 dBFS: its peak-relative
magnitude looks like a clean 0 dB passband, but every bin is under the
estimator's floor. Those bins are `defined = false`. If a group-delay curve
comes back entirely undefined for a processor you expected to measure, check the
output's absolute level before suspecting the analyzer — an all-undefined curve
usually means the processor emitted (almost) nothing, which is the finding.

## Discipline that keeps it trustworthy

- **Release only.** A Debug DSP/UI build mismeasures levels and timing.
- **Analyzer Determinism Contract** — every spectral/estimator assertion declares
  its stimulus, window (rectangular for a bin-coherent tone or an impulse; Hann
  for broadband — a Hann window annihilates an impulse at n=0), warm-up trim,
  estimator, seed, sample rate, and tolerance *class*. State them in the test so a
  red is a real DSP change, not an analyzer artifact.
- **Named tolerances**, never magic numbers — see the plan's Threshold Policy.
- **Layering is one-way.** Doctor may use scenarios + spectrum; nothing below
  scenarios may include `audio_doctor`/`audio_contracts`, and the
  `pulp::audio-analysis` lib may never depend on the test-only layers (it has no
  `Processor`/scenario knowledge).
- **Prove the assertion can fail.** A green measurement is not evidence until
  you have seen it go red. Mutate the thing under test — drop the param event,
  zero the output, route the id wrong — rebuild, and confirm the check you are
  relying on actually turns red. This is cheap and it repeatedly catches
  tolerances that are too loose to detect the bug they were written for: a
  200 Hz lowpass check at 8 kHz looks decisive but still passes with the filter
  left at its 1000 Hz default, because 8 kHz is ~36 dB down either way. Only the
  1 kHz checkpoint separates them.
- **A fixed sample-rate converter does not anti-alias a variable playback
  rate.** Tempo maps, varispeed, and other runtime projections can advance the
  source position by more than the asset-rate/timeline-rate ratio prepared at
  compile time. Derive the reconstruction cutoff from the effective source
  frames per output frame, prepare a bounded kernel bank off the audio thread,
  and select from it without allocation. Prove the path with both a passband
  control and a bright source that aliases under linear point sampling.
- **Compiling a harness TU by hand? Copy `CXX_DEFINES`, not just `CXX_FLAGS`
  and `CXX_INCLUDES`.** Both live in the target's `build/**/flags.make`.
  Adapter structs are gated on defines (`PULP_CLAP_GUI` adds members to
  `PulpClapPlugin` — the header says so at the top), so a TU built without them
  disagrees with the prebuilt `libpulp-format.a` about the struct's size and
  `clap_init` writes off the end of your object. The symptom is a
  `__stack_chk_fail` abort *after* every assertion has already printed PASSED,
  and it appears/disappears with unrelated stack layout changes — it will read
  as a product bug and it is not one.

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

> **Do not hand-roll WAV analysis.** If you are about to `soundfile.read()` two
> WAVs and compare them, these verbs already do it. They are registered in
> **`docs/status/tools.yaml`**, whose digest is generated into CLAUDE.md, and a
> PostToolUse hook names them if you start writing the script anyway. The design
> tools learned this the hard way: guidance that exists but never fires at the
> moment of need is the same as no guidance.

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

For streamed sampler assets, build the production mip sidecar with
`pulp audio sampler-mip build <source.wav|source.aiff>`. It accepts strict
ranged WAV and uncompressed AIFF/AIFF-C, uses the sampler's shared 140 dB
decimator, publishes source-and-payload-hash-addressed float32 WAV levels, and
atomically publishes the `.pulpmip` manifest last. `--levels 1|2`,
`--max-source-bytes`, and `--max-output-bytes` bound offline production.
`--json` emits `ok`, `source`, `manifest`, `payloads`, and on failure `error`;
the command exits 0 on success and 1 on failure. Sidecar production does not
waive PulpSampler's runtime limit of one or two channels and at most 192 kHz.

### In-tree render → WAV bridge (no plugin bundle)

`pulp audio render` and `pulp run --audio-capture-*` both need a built plugin
bundle / standalone. To get an **in-tree** `Processor` (or a raw oscillator
wrapped in one) onto disk for the Python lane WITHOUT a bundle, use the
scenario-side bridge in `"support/wav_bridge.hpp"`:
`write_scenario_wav(result, path, WavBitDepth::Float32)` (or the
`write_buffer_wav(buffer, sample_rate, …)` overload) deinterleaves a
`ScenarioResult` / `Buffer<float>` and calls `write_wav_file`. Float32 is the
default so the file carries the exact rendered samples; the deterministic
harness makes the bytes reproducible. The `pulp-osc-render-wav` test tool
(`test/osc_render_wav.cpp`) is the argv surface: it renders the in-tree
`VcoOscillator` (via `"support/osc_wav_scenario.hpp"`) to a WAV
(`--shape/--freq/--sr/--dur-ms/--channels/--drift-cents/--jitter-cents/--bits`),
so an offline Quality-Lab script can analyze in-tree oscillator output with no
bundle in the loop. The pytest `tools/audio/quality-lab/tests/test_osc_wav_bridge.py`
is the end-to-end readability proof (soundfile loads it, fundamental matches).

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
- **`corpus.seed()`** ships FIVE synthetic families, not just the two stretch-oriented ones:
  `synthetic_drumbreak` / `synthetic_tonalpad` (time-stretch / tonal) plus three oscillator
  families (`family: "oscillator"`, `material_class: "synth"`) built from `osc_fixtures.py`'s
  corpus-render helpers (`render_static_shapes`, `render_sync_sweep`, `render_tzfm_grid`) —
  `synthetic_osc_static_shapes` (sine/saw/square/triangle at a few pitches), `synthetic_osc_sync_sweep`
  (`hard_synced_saw` swept across master frequencies), `synthetic_osc_tzfm_grid` (`tzfm_sine` over a
  mod-rate/index grid). They gate on the SAME `regression_net` ratchet as any other family — no
  bespoke oscillator ratchet — typically via the `added-hf` axis (aliasing reads as added HF energy)
  and `dsp.null_residual_db`'s bit-identical clamp (`-160.0`) for the determinism check. See
  `tests/test_corpus_osc.py`.
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
- **`detectors/click.py` refuses a REAL BLEP/BLIT oscillator at a fractional period — do not
  read that refusal as a click.** The detector's comb self-reference (delay one period, subtract)
  nulls a bandlimited oscillator to the interpolator floor ONLY when successive periods are
  identical up to the fractional delay. That holds for an exact additive fixture (sinc-periodic),
  which is exactly why a click floor measured on a synthetic fixture does NOT transfer to a real
  oscillator: on a polyBLEP saw/square, `sr/f0` is fractional, so the discontinuity lands at a
  different sub-sample phase every period and the comb leaves a per-edge *approximation* residual
  that reads a false −20 to −30 dB "click" even when the render's alias floor is −55 dB or lower.
  `detect()` discriminates that regime with two measurements — `template_novelty_db` and
  `edge_concentration` — and REFUSES (`low_coverage`, `fired=False`, a *third* refusal
  precondition alongside low period-confidence and pitch drift) rather than false-firing.
  `template_novelty_db` aligns the comb residual into per-period frames at the fitted fractional
  period, builds the median-frame TEMPLATE, and scores the worst period's departure from it over
  the median period's — a one-off seam is the single period that does NOT match the recurring
  template (a large outlier), while the smear repeats the same per-edge shape every period so it
  barely departs. The departure is scored against a HIGH percentile (90th) of the per-period
  departures, NOT the median — because at a HALF-INTEGER period (`sr/f0` frac ~0.5) the BLEP edge
  alternates sub-sample phase A/B/A/B (true residual period 2P), so HALF the periods depart from
  the per-P template: a median denominator reads that recurring half as a lone outlier and
  FALSE-FIRES on a clean oscillator, flipping with period-count parity (render length). A high
  percentile puts any recurring cluster over ~10% of periods (2P halves, 3P thirds) INTO the
  denominator, so only a genuine one-off seam stays a large ratio. It is a per-period RATIO, so it
  is **render-length INDEPENDENT** (a longer clip only adds more template-matching periods — it
  moves neither the worst nor the percentile): the same near-floor seam that a global statistic
  dropped on a multi-second render now fires at any length, at integer AND half-integer periods.
  `edge_concentration` (cosine similarity of `|residual|` to `|diff(y)|`) is kept, but ONLY to
  split smear (rides the waveform's own edges) from a block-rate zipper (off the edges) — novelty
  alone reads a stationary zipper as low-novelty and would wrongly refuse it. A refusal is NOT a pass:
  the honest gate for a discontinuous real oscillator is a **frozen-reference null**
  (`dsp.null_residual_db` — render the same patch with the offending parameter held frozen), not
  the comb self-reference. When you add a click fixture for a new oscillator, grow a REAL polyBLEP
  render (`osc_fixtures.py`), not just a synthetic sum — the synthetic-only fixture is precisely
  the blind spot that let the false-fire ship.

## Proving reported latency

`expect_reported_latency(result)` measures the delay actually present in a
render and compares it against `Processor::latency_samples()`. This is the
contract for any processor with nonzero latency: the host slides the whole track
by the number the plugin reports, and nothing else checks it. `RenderScenario`
collects the report facts on **every** render (`ScenarioResult::latency`), so a
contract can ask for the proof without re-rendering.

**Do not reach for this on a processor that has no latency.** Most don't: gain,
EQ, compressors without lookahead, waveshapers, delay *effects*, and ordinary
synth paths correctly report zero, and rendering them to prove `0 == 0` is
ceremony. PulpSampler is the sampler exception: enabling its synthetic heritage
two-leg SRC produces derived, profile-dependent latency, while an all-bypassed
profile reports zero. That heritage path belongs in the same measured-latency
proof category as FFT size, partition scheme, lookahead window, or oversampling
ratio because a refactor can silently change the number. User-facing rationale,
including when NOT to use it: `docs/guides/latency-proof.md`.

## The spectral analyzers fail closed — a refusal is the answer, not a bug

`measure_thd`, `measure_aliasing`, and `magnitude_spectrum_curve` throw rather
than return a number they cannot stand behind: silence (no fundamental to be
relative to), a capture shorter than `fft_length` (zero-padding would measure
the truncation edge), a fundamental at or above Nyquist, and — the one that
surprises people — an alias series that never reaches Nyquist. **Size
`num_harmonics` from the sample rate**: the default 64 models no alias site at
all below ~375 Hz at 48 kHz, and that used to read as *clean* on a maximally
aliased saw, because "no aliases among the zero places I looked" is technically
true. Through the CLI a refusal is exit 2, distinct from a failed check.

Every one of those paths previously returned a confident number that passed a
gate. If a refusal surprises you, the input is usually wrong — not the tool.

## Measuring an oscillator's pitch — `estimate_pitch`, not `estimate_frequency`

`estimate_frequency` (audio_metrics.hpp) is a zero-crossing detector and its own
doc disclaims harmonically-dense material. A real oscillator IS dense: on a
bright tone it locks to a harmonic (measured ~1900 cents high on a formant
waveform), so it cannot measure a VCO's drift or a DCO's few-cent quantization
error. Reach for **`estimate_pitch` / `track_pitch`** (`pitch_track.hpp`)
instead: a coarse `magnitude_spectrum_curve` peak seeds a golden-section refine
over the shipped `fit_tone` projection, so it is leakage-free and sub-cent
(<0.002 cent on a clean tone, coherent or not) and FFT-backend-stable. It
**refuses** silence and noise rather than octave-guessing — honor the confidence
gate. `track_pitch` gives the `f0(t)` trajectory; the drift/jitter statistics over
it (Allan deviation, Theil-Sen slope) stay in the Quality Lab per the C++/Python
split. A steep glide needs the window short relative to the sweep.

**Harmonic-dominant material — don't assume the loudest bin is f0.** A rolled-off
or resonant oscillator can carry MORE energy in an upper partial than in the
fundamental, so the loudest FFT bin lands an octave/twelfth/higher above the true
pitch. `estimate_pitch` recovers f0: it ranks the coarse peak against its
subharmonics (to coarse/8) and adopts the lowest root that BOTH explains the
segment (harmonic-comb energy within a margin of the best) AND carries real energy
at its OWN fundamental tooth. That own-tooth test is load-bearing — explained
energy alone cannot tell f0 from f0/N (a near-pure tone lets any subharmonic refine
one of its harmonics onto the tone), and only the TRUE root has a partial sitting
at the root itself. **The tooth floor is LEAKAGE-AWARE, and it has to be:** a fixed
floor is defeated by rectangular-window LSQ leakage from the loud coarse peak into
the f0/2 fit, and that leakage GROWS as the analysis window shortens (energy ~
`(1/(π·Δf·T))²`). With a fixed floor a plain bass saw over the DEFAULT window holds
only a handful of periods and reads a confident octave (or two) DOWN — the ordinary
case, worse than the octave-up it was meant to fix. So a candidate's tooth must
clear `max(absolute-noise-floor, expected coarse-peak leakage at its Δf over this
window)`; a genuine fundamental far below a loud harmonic (Δf large) or over a long
window (T large) still clears it, a leaky saw subharmonic over a few-period window
does not. Corollary: **a genuinely faint fundamental is not resolvable below the
leakage floor at a short window — use a longer analysis window** (the DCO/VCO
quantization/drift fixtures already do, n≈2^15) rather than trusting a low bass note
over a 4096-sample frame. Do NOT "fix" a wrong octave by lowering the floor — it
re-opens the octave trap. **One honest residual, NOT a refusal:** a genuine
missing-fundamental comb (energy only at 2·f0/3·f0, nothing at f0) has no tooth at
f0 to lock, so the estimate stays on the loudest PRESENT partial (2·f0) — the
frequency actually there, never a fabricated virtual f0; likewise the descent stops
at coarse/8, so a 9th-harmonic-dominant signal (rare) reports a lower harmonic.
Both return a real present frequency, never fail closed. (Regressions fixed
2026-07: octave-UP on harmonic-dominant material — old guard stopped at coarse/4 on
a fixed 10% cliff; then octave-DOWN on a plain short-window saw — fixed-floor
leakage, closed with the leakage-aware floor above.)

## Never gate on `detection_floor_db`

It is a ~2σ bound that assumes a **white** residual. Aliases are discrete tones,
so it is wrong exactly where it is most tempting, and asserting it is the
analyzer grading its own homework. Its one legitimate use is deciding whether a
reading is conclusive at all — a gate means something only while the measured
value clears the floor.

**Prove a floor with a negative control**: a fixture whose alias content is zero
by construction must read collapsed, and injected impurities of known level must
come back at that level. Prefer `tone_residual_db` (least-squares projection) to
reading a windowed spectrum near a loud tone — it sidesteps leakage entirely.
Window floors are measured, not assumed: `flat_top` is amplitude-accurate at
about −93 dB with a flat skirt and **cannot** gate −100 dBc; only `kaiser` at
β=14 resolves it, and even then not at bins 1–3, where the DC-removal pedestal
dominates regardless of window.

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

**Read the confidence numbers, not just the verdict.** The evidence carries
`null_depth_db` (how completely the delayed input explained the output) and
`ambiguity_margin_db` (how much worse the best competing delay scored). Measured
references: a pure delay line nulls to the -200 dB floor with a 203 dB margin;
`SpectralFrameEngine` in identity mode — real STFT, real overlap-add rounding —
nulls to -137 dB with a 140 dB margin. Both are nowhere near the -60 dB floor or
the 12 dB ambiguity bar. A pass with a *small* margin technically cleared the bar
and is still a coin flip waiting to happen — treat it as a finding and say so.

**A pass proves self-consistency, not the right value.** The audio is delayed by
exactly what the processor reports — which is all the host needs, and is why it
sounds correct. It does NOT prove the latency is what it should be: double an FFT
size and the processor honestly reports its new, larger delay, passes the
contract, and has silently added 43 ms to every session. `apply_expected_samples`
(C++) / `--latency-expect` (CLI) / `latency_expect` (MCP) pins the intended value
so that drift fails too. It never masks a real mismatch — an audio-vs-report
disagreement is still reported as *that*.

**Dogfood reference.** `test/test_latency_contract.cpp` proves the contract
against `SpectralFrameEngine` (true latency `fft_size + analysis_hop` = 2560),
not just against a delay line, and a companion case deliberately misreports by
one analysis hop to confirm the catch. Copy that shape when covering real DSP.
