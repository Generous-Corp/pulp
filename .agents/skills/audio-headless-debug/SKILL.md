---
name: audio-headless-debug
description: Reproduce and debug "only happens in a DAW" audio plugin bugs (cutouts, glitches, parameter-change failures) entirely offline — headless Processor scenes for DSP bugs and a standalone AudioUnit host probe for adapter/host-interaction bugs. Use when a plugin misbehaves in Logic/Live/etc. but unit tests are green.
requires:
  - tools/audio/analysis/include/pulp/audio/analysis/audio_metrics.hpp
  - tools/audio/analysis/include/pulp/audio/analysis/audio_assertions.hpp
  - core/format/include/pulp/format/headless.hpp
---

# Debugging "only in the DAW" audio bugs headlessly

When a plugin bug reproduces in a host (Logic, Live, REAPER) but every unit
test is green, you are almost always **testing the wrong layer**. This skill is
the playbook that took a multi-day "audio cuts out when I touch a parameter"
ghost down to a deterministic, no-DAW reproduction in one session.

## The core insight: pick the layer the bug actually lives in

Pulp has two offline harnesses, and they exercise **different code paths**:

| Harness | What it drives | Misses |
|---|---|---|
| `HeadlessHost` (`pulp/format/headless.hpp`) | the `Processor` directly (DSP) | the entire format adapter, the host↔store sync, the render thread, `Globals()` parameter store |
| **Standalone AU host probe** (`AudioComponentInstanceNew` + `AudioUnitRender`) | the **real `.component`** through CoreAudio | nothing — it is the real adapter + render path |

A bug that only shows up in a DAW is, by definition, in something `HeadlessHost`
skips — the adapter, the parameter-sync, threading, or a *dynamic* DSP path the
static scene tests never hit. **Reproduce at the adapter layer first**, then
bisect downward into the Processor once you can trigger it.

## STOP — use the tools that already exist. Do not hand-roll.

These were built deliberately for exactly this. Reaching for a hand-written
`rms_db`/peak loop instead is how a −39 dB ghost ships past a lenient threshold.
**Before writing any audio reproduction or assertion, use one of these:**

| Need | Use (already built) | Where |
|---|---|---|
| Metrics from a rendered buffer (peak/RMS/dBFS, clip, silence, NaN/Inf) | `pulp::audio::analysis::analyze()` → `BufferMetrics` | `pulp/audio/analysis/audio_metrics.hpp` (link `pulp::audio-analysis`) |
| Pass/fail gates in a test | `assert_not_silent`, `assert_no_nan_inf`, `assert_rms_between`, `assert_peak_between`, `assert_frequency_near`, `assert_null_near`, `assert_channels_independent` | `pulp/audio/analysis/audio_assertions.hpp` |
| Fundamental-frequency check | `estimate_frequency()` / `assert_frequency_near()` | `audio_metrics.hpp` / `audio_assertions.hpp` |
| Human-readable buffer report | `summarize(metrics)` | `audio_metrics.hpp` |
| Glitch/artifact + "audio doctor" detection | `audio_artifacts.hpp`, `audio_doctor_artifacts.hpp` | `tools/audio/analysis/` |
| Command-line: render/inspect/compare a WAV without writing C++ | **`pulp audio validate <summarize\|doctor\|compare\|assert>`** (assert checks: `no_nan_inf`, `not_silent`, `silent`, `peak_below`, `frequency_near`) | `tools/cli/cmd_audio_validate.cpp` |
| Live per-callback metering / MIDI log / buffer-underrun capture | `pulp::inspect::AudioInspector` | `inspect/include/pulp/inspect/audio_inspector.hpp` |
| Interactive inspection from an agent session | the **`pulp_inspect_audio`** MCP tool (the Audio Inspector) | MCP |

Rule of thumb: if you typed `std::sqrt`, `* x[i]`, or `20.0 * log10` in a test,
stop — `analyze()` already did it correctly. The only thing you author is the
*stimulus* (what signal, what parameter motion) and the *gate*
(`assert_*(...).passed`).

## Tier 1 — headless Processor scene (DSP bugs)

For sidechain scenes, use `process_with_sidechain()` or
`render_offline_with_sidechain()`; main and sidechain remain independent buses,
and ordinary `process()` clears stale sidechain state. Use
`try_prepare_bus_layout()` for a declared layout. `BackgroundTaskLane<T>` is a
fixed-capacity prewarmed lane with ordered/latest policies and exception
containment; `AudioTap` uses the planar SPSC ring and drops whole aligned frames.

Fast, deterministic, no Apple frameworks. Use `HeadlessHost` + the offline audio
analysis helpers. Pattern (see `tests/test_scenes.cpp` in a plugin repo):

```cpp
#include <pulp/format/headless.hpp>
#include <pulp/audio/analysis/audio_metrics.hpp>
#include <pulp/audio/analysis/audio_assertions.hpp>

pulp::format::HeadlessHost host(create_my_processor);
host.prepare(48000.0, 512);
// render blocks, optionally mutating params per block via host.state().set_value(...)
auto m = pulp::audio::analysis::analyze(out_view, 48000.0);
REQUIRE(pulp::audio::analysis::assert_not_silent(m).passed);   // the gate that matters
```

For double-precision callback bugs, `HeadlessHost` has matching
`process_f64(...)` overloads and `ValidationHarness::process_buffer_f64(...)`.
Non-f64 processors still run through the default f64-to-f32 compatibility
fallback, so a headless f64 scene proves host-boundary behavior; it does not
prove the plugin has native double-precision DSP unless the descriptor opts into
`supports_f64_audio` and the processor overrides `process_f64`.

**Always use `pulp/audio/analysis` — never hand-roll `rms_db`.** `analyze()` →
`BufferMetrics`; assert with `assert_not_silent`, `assert_no_nan_inf`,
`assert_rms_between`, `assert_frequency_near`, `assert_null_near`,
`assert_channels_independent`. These encode the right thresholds (e.g. the
silence floor) so a test can't "pass" on a −39 dB ghost the way a lenient
hand-written `> -50 dB` check did.

## Tier 2 — standalone AU host probe (adapter / host-interaction / threading bugs)

When Tier 1 is green but the DAW still fails, load the **real component** and
drive it like a host. The reference implementation is
`bendr-pulp/tools/au_host_probe.cpp` — copy and adapt it. It:

1. `AudioComponentFindNext` by `{type, subtype, manufacturer}` (read these from
   the built bundle: `PlistBuddy -c "Print :AudioComponents:0:..."`).
2. `AudioComponentInstanceNew` → `AudioUnitInitialize` with a stream format +
   `MaximumFramesPerSlice` + an input render callback that supplies a tone.
3. Renders on a **background thread** (the host's render thread) via
   `AudioUnitRender` while the **main thread** mutates a parameter — either via
   `AudioUnitSetParameter` (host path) or, to faithfully mimic the plugin's own
   editor, by fetching the `StateStore` through the editor-context property
   (`kPulpEditorContextProperty` = `'PuEd'`, struct `{Processor*, StateStore*}`)
   and running a real `pulp::state::ParameterEdit` gesture.
4. Measures per-block output peak and flags it if output dies after a change.

Build/run (the probe does **offline render to memory — no speakers**, so no
audio-etiquette concern):

```bash
cp -R build/AU/MyPlug.component ~/Library/Audio/Plug-Ins/Components/
rm -rf ~/Library/Caches/AudioUnitCache; killall -9 AudioComponentRegistrar
./build/au-host-probe   # exits non-zero if the bug reproduces
```

This is the tool to reach for whenever someone says "can we automate this
instead of me testing in Logic." It works on any Pulp AU plugin.

## Step 0 — check the crash logs BEFORE any bisect

In Logic, multiple AUs share one `AUHostingServiceXPC` process. If touching
plugin A reliably kills plugin B's audio (e.g. the upstream software
instrument dies until reopened), suspect a **crash of the shared process**,
not a stall — and the guilty binary can be a *different plugin* than the one
being touched. **Before bisecting anything:**

```bash
ls -t ~/Library/Logs/DiagnosticReports/ | head    # AUHostingServiceXPC_*.ips
```

Parse the `.ips`: the `asiBacktraces` field holds the original throw-site
backtrace with the guilty image name. Two gotchas: macOS **throttles duplicate
crash reports** (repeat wedges may not write new files), and an uncaught
ObjC exception in `drawRect:` is **fatal to the whole process** (AppKit
`_crashOnException`) — one plugin's paint bug silences every plugin in the
process. A multi-day "wedge" hunt across params/DSP/GPU was solved in minutes
once the crash log was read. (SDK hardening from that incident: UTF-8-safe
`text_x_for_byte`, `ns_string_never_nil` in the CG canvas, and a `@try/@catch`
guard around the plugin-view paint.)

## The decisive measurement, not a guess

Once reproduced, the single most localizing fact for a cutout is: **is the host
still calling render, and is input vs output silent?**
- render still called + **input full, output silent** → the *plugin* is emitting
  silence (DSP or adapter), not the host muting it.
- render stops being called → the host disabled the AU (a returned error, a
  render-thread stall).

`os_log`/`runtime::log` does **not** surface from Logic's sandboxed
AUHostingService process. To watch the live render path in-host, write a
throttled trace to a **file** (`/tmp/...`) from `ProcessBufferLists` and
`tail -f` it over SSH. Better: reproduce in the probe and read state directly.

## Sweep the parameter and measure LEVEL — the move that cracks DSP bugs

The single highest-leverage diagnostic when "audio breaks when I touch a
control": **render at a sweep of STATIC values across the parameter's full
range, measure the output LEVEL at each, and toggle each related mode on/off.**
The differential pattern localizes the broken stage in one table:

```cpp
for (bool mode_on : {true, false})
  for (float v : {0.f, 3.f, 6.f, 9.f, 12.f}) {
    HeadlessHost host(create_my_processor); host.prepare(48000, 512);
    host.state().set_value(kSomeMode, mode_on); host.state().set_value(kParam, v);
    auto out = render(host, tone);
    auto m = pulp::audio::analysis::analyze(out_view, 48000);
    printf("mode=%d param=%.1f -> %.2f dBFS\n", mode_on, v, m.max_rms());
  }
```

A real worked example: "audio cuts out when I change a parameter" was NOT a
threading/adapter bug and NOT a rate-of-change latch (two wrong theories that
cost hours). The level sweep showed instantly:

```
preserve=1 pitch= 0 st -> -9 dB     preserve=0 pitch= 0 st -> -9 dB
preserve=1 pitch= 6 st -> -34 dB    preserve=0 pitch= 6 st -> -9 dB
preserve=1 pitch=7.2 st -> -39 dB   preserve=0 pitch=7.2 st -> -9 dB
```

→ a **static** level collapse confined to the formant-preserve path,
progressive with pitch. That pinned it to `SpectralEnvelopeShifter` (the
envelope correction wasn't energy-preserving, so a narrow-band tone was scaled
by the falling envelope above its peak). One sweep replaced days of guessing.

**Measure LEVEL, never only frequency.** The bug above rendered the *correct
frequency* the whole time — every existing scene checked `peak_hz` / frequency
and passed at −39 dB. `assert_frequency_near` alone is a trap; pair it with
`assert_rms_between` / `assert_not_silent`.

## Also test the rate and the toggles

Beyond the static sweep, for every continuous parameter add a **rapid-jump**
scene (change every 1–2 blocks, then settle and assert recovery) and a
**rapid-toggle** scene for booleans — these catch genuine *latching* state
(an edge handler or accumulator that self-sustains). Run the **AU host probe in
CI** gating on output staying alive across scripted parameter automation; it
exercises the adapter+render path no headless scene can.

If you discover a failure mode, add the scene to `test_scenes.cpp` AND extend
the probe so it can never regress.

## What this caught (worked examples)

- **Formant-preserve pitch-up silenced the output** (the headline bug): static
  level collapse in `SpectralEnvelopeShifter`, found by the level sweep above,
  fixed with energy-preserving normalization. Frequency-only tests missed it.
- **Param edits reverting in-host** (XY snap-back, type-in not taking): the AU
  adapter's per-block `GetParameter → store` pull clobbered UI writes. Tier-2;
  fixed by a host↔store reconcile. See `au-param-host-store-clobber`.
- **Audio-thread `AUEventListenerNotify`** stalling Logic's render thread on
  every param change. Tier-2; never call it from `ProcessBufferLists`.

## Reaching typed processor state in a headless scene

`HeadlessHost` hides the wrapped `Processor` behind the generic interface, so a
scene can normally only observe the output buffers + the param store. When a bug
lives in internal state the output doesn't directly expose — a gain-reduction
meter, an event-count/ring, a voice-allocation table — use
`HeadlessHost::processor_as<T>()` to recover the concrete pointer and assert that
state after a scripted block:

```cpp
auto* comp = host.processor_as<MyCompressor>();
REQUIRE(comp);                       // null if the dynamic type doesn't match
run_scene(host, scene);
REQUIRE(comp->gain_reduction_db() > 0.0f);   // internal state, not just output
```

`processor()` returns the base `Processor*`; `processor_as<T>()` is a checked
`dynamic_cast` (null on mismatch). Lets a scene pin a regression at the source
instead of inferring it from a downstream silence.

## The StateStore must outlive the Processor

`HeadlessHost` declares its `state::StateStore` before its
`std::unique_ptr<Processor>`, so the store is destroyed *after* the Processor.
That order is load-bearing, not incidental: `Processor::state()` dereferences a
pointer into the host, and a Processor may follow it from its destructor and from
any worker thread that destructor is about to `join()`.

If you write a headless scene around a Processor that owns a background thread,
this is what keeps `~Processor` from joining that thread against a freed store. It
crashes only on teardown, only sometimes, so a scene that "passes" a hundred times
can still be wrong. `test/test_store_lifetime.cpp` pins the ordering.

## Latency evidence lives in the analysis lib, not the scene

If a headless scene needs to prove a processor's *reported* latency against the
delay in its output, do not hand-roll the measurement. `pulp::audio-analysis`
(`latency_evidence.hpp`) has the pure evaluators — delayed-null and marker-offset
— plus the evidence schema that the test harness, the `pulp` CLI, and MCP all
serialize. Calling them keeps a scene's verdict identical to what CI and an agent
would conclude from the same audio.

They refuse rather than guess: silence, an output that is not a delayed copy of
the input, an integer-sample-periodic stimulus, and a report that moved
mid-render all come back `inconclusive`. See the `audio-harness` skill for the
full list and the stimulus each policy needs.

## The spectral analyzers throw — and the throw is the feature

`measure_aliasing` and `measure_thd` reject inputs they would otherwise report a
confident number for. Catch the exception or size the arguments correctly; do not
read the refusal as a tooling bug.

The one that surprises people is the bass wall. `measure_aliasing` only models an
alias site where a *requested* harmonic crosses Nyquist, so with the default
`num_harmonics = 64` at 48 kHz, a fundamental below ~375 Hz models no alias site
at all. It used to answer "clean" — a naive, maximally aliased saw read −200 dB
and passed a −100 dBc gate, because "no aliases among the zero places I looked"
is technically true. **Size `num_harmonics` from the sample rate**, not from a
default: it needs `fundamental_hz * num_harmonics >= nyquist`.

`measure_thd` refuses silence (a dead processor otherwise reads THD 0 and passes
`thd < 1%`), a buffer shorter than `fft_length` (the old zero-pad made a clean
sine read −48.7 dB against a truth of −176.7, and recorded `coherent = true`),
and a fundamental at or above Nyquist.

## Never gate on `detection_floor_db`

It is a ~2σ bound that assumes a **white** residual. Aliases are discrete tones,
so the assumption fails exactly where the number is most tempting, and asserting
it is the analyzer grading its own homework. Its one legitimate use is deciding
whether a reading is conclusive at all — a gate is trustworthy only while the
measured value clears the floor.

**Prove a floor with a negative control instead**: a fixture whose alias content
is zero by construction must read collapsed, and injected impurities of known
level must come back at that level. That is evidence; the derived number is an
assumption. `tone_residual_db` is the projection path and sidesteps window
leakage entirely — prefer it to reading a windowed spectrum near a loud tone, and
note that kaiser's deep floor holds near the tone but not at bins 1–3, where the
DC-removal pedestal dominates regardless of window.
