# Audio Inspector Guide

The **Audio Inspector** is a floating developer window in the standalone host
that observes the realtime output-boundary probe — peak/RMS levels, dBFS,
clip and NaN/Inf counts, silence runs, and callback counts. It is the audio
sibling of the `Cmd+I` layout inspector: a dev-only overlay you open while a
plugin or app is running to answer "is sound actually flowing, and what does
it look like?"

It is **live**: it reads metrics from a running audio host. This is distinct
from the offline **Audio Doctor** (`pulp audio validate ...`), which analyses
an already-rendered WAV file offline. Use the Inspector to watch a live host;
use the Doctor to assert on a captured render in CI.

## Audio Inspector vs Audio Scope vs Spectrum Analyzer

Three related but distinct things share similar vocabulary; pick by the
question you're asking:

| Tool | What it is | Scope of signal | Frequency domain? |
|---|---|---|---|
| **Audio Inspector** | A floating **developer** window in the standalone host (this guide). | Pulp's *own* running plugin/standalone, at the output-boundary probe. | No — time-domain levels/counters/waveform only. |
| **Audio Scope** | A **mode of the Audio Inspector** (see "Signal mode vs Scope mode" below), not a separate tool. | Same probe as the Inspector. | No — triggered, measurable time-domain trace. |
| **Spectrum Analyzer** | A **shippable FX example plugin** (clean-room, MIT, in its own repo: [pulp-spectrum-analyzer](https://github.com/danielraffel/pulp-spectrum-analyzer)) with a GPU spectrogram and a cross-instance before/after spectral **diff**. | *Any* signal in a host chain, including third-party plugins — not limited to Pulp's own plugins. | Yes — FFT-based spectrum, computed off the audio callback. |

The Audio Inspector is deliberately a time-domain developer diagnostic and does
**not** do frequency-domain analysis; the Spectrum Analyzer example is the
frequency-domain, end-user-facing counterpart. They are different deployments
answering different questions: the Inspector debugs *your* Pulp plugin while you
build it; the Spectrum Analyzer is an insert you ship that can compare *any*
chain. The Spectrum Analyzer is an independent clean-room implementation built on
Pulp SDK primitives (FFT, lock-free `TripleBuffer`/`SpscQueue` publication, the
`SpectrumView`/`SpectrogramView` widgets); see Pulp's licensing and attribution
notes at <https://www.generouscorp.com/pulp/licensing.html>.

## When to use it

Use the Audio Inspector when the question is about a running host, not an
offline render:

- "Is this processor producing audio at all?"
- "Did my parameter, MIDI, host graph, or importer change accidentally silence
  the standalone output?"
- "Are we clipping, producing NaN/Inf samples, or underrunning while the UI is
  open?"
- "Is the standalone host configured with the sample rate, block size, and
  channel count I expected?"
- "Can an agent get a quick machine-readable health check before deciding which
  deeper audio test to run?"

It is deliberately shallow and cheap. It does not prove frequency response,
THD, phase, latency, tuning, or golden-file parity. For those, render a WAV or
artifact bundle and use the offline Audio Doctor.

## Signal mode vs Scope mode

The Inspector window has two related modes:

| Mode | Use it when | What it optimizes for |
|---|---|---|
| **Signal** | You need to know whether the live host is healthy. | Literal buffer observability: levels, clip/NaN counters, silence, callbacks, device stress, and a raw rolling waveform. |
| **Scope** | You need to understand the shape of a signal. | Repeatable sample-window acquisition: trigger alignment, grid/readouts, and measurements such as frequency, peak-to-peak, RMS, DC offset, and crest factor. |

Signal mode is the most diagnostic view. It shows what the probe just copied
from the live output boundary, with no invented samples, interpolation,
temporal smoothing, or persistence. That makes it the right mode for debugging
silence, clipping, NaN/Inf output, DC offsets, discontinuities, and dropouts.
A periodic signal may appear to slide around because each UI tick can land at a
different phase; that is expected, and it is part of why Signal mode is useful.

Scope mode is more readable, not more "smoothed." It still uses real copied
samples, but it chooses a bounded acquisition window and can align the display
to a rising zero crossing when one exists. That makes a healthy oscillator or
periodic waveform look stable enough to inspect, and it gives agents and humans
measurements they can compare. If no trigger is found, Scope reports that fact
and falls back to the available sample window rather than fabricating a cleaner
trace.

New users start in Signal mode. If you switch to Scope, the Inspector remembers
that preference for the app, so reopening the window returns to the mode you
last used.

## Quick developer smoke check

For a human, the fastest path is the live window:

```bash
pulp run pulp-audio-inspector-demo --audio-inspector
```

For a quick terminal check, ask for JSON and inspect the counters:

```bash
pulp run pulp-audio-inspector-demo \
  --audio-probe-json /tmp/pulp-audio-inspector-demo.json \
  --frames 90

jq '{callbacks, peak_max, rms_max, peak_dbfs, rms_dbfs, clip_count, nan_inf_count}' \
  /tmp/pulp-audio-inspector-demo.json
```

For an app/plugin expected to output audio, a healthy smoke result usually has
`callbacks > 0`, non-zero `peak_max` and `rms_max`, and zero `clip_count` /
`nan_inf_count`. For an intentionally silent plugin, non-zero callbacks plus
zero levels is still useful: it says the host is running and the observed stage
is silent.

## What it observes

The probe taps the standalone host at the *output boundary* — immediately
after `Processor::process(...)` and before the device callback returns. Each
block it publishes an `AudioProbeSnapshot` (see
`pulp/audio/audio_probe_snapshot.hpp`) carrying:

- Identity: `stage`, `sample_rate`, `block_size`, `channel_count`,
  `sequence_number` (monotonic; a gap means snapshots were dropped between
  reads).
- Level: `peak_max`, `rms_max` (linear) plus `peak_dbfs` / `rms_dbfs`
  (`20*log10`).
- Content events: `clip_count`, `nan_inf_count`, `clipped_blocks`,
  `nan_blocks`, `silence_run_blocks`, `callbacks`.

The waveform is a diagnostic signal inspector, not a full oscilloscope. By
default it draws the copied buffer exactly as received, with no invented
samples, interpolation, smoothing, or persistence. That makes it useful for
spotting silence, clipping, discontinuities, DC offsets, and dropouts. It is
not meant to provide cursors, phosphor persistence, triggered measurements, or
frequency-domain analysis; use Audio Doctor or a purpose-built render for those.

For readability during live visual checks, the window supports display-only
environment knobs:

- `PULP_AUDIO_INSPECTOR_TRIGGER=rising-zero` starts the visible trace at the
  first rising zero crossing when one exists and leaves raw mode otherwise.
- `PULP_AUDIO_INSPECTOR_GRID=0` hides the subtle waveform grid.
- `PULP_AUDIO_INSPECTOR_SCALE=<n>` zooms the horizontal view over the copied
  real samples (`1.0` shows the full captured buffer).

## Opening it

There are three ways to open the live Inspector:

1. **In-app chord** — press `Cmd+Shift+A` (macOS) / `Ctrl+Shift+A`
   (Windows/Linux) in a running standalone window.
2. **Environment variable** — set `PULP_AUDIO_INSPECTOR=1` before launching;
   the host opens the window at startup.
3. **CLI flag** — `pulp run --audio-inspector` (forwards
   `--audio-inspector` and sets `PULP_AUDIO_INSPECTOR=1` for the child).

```bash
# Open the live Audio Inspector alongside the running app
pulp run --audio-inspector
```

`--audio-inspector` does **not** imply `--headless` (a dev usually wants the
visible window). It composes with `--screenshot`, which is headless: a CI run
can capture the panel for visual regression.

```bash
# Headless: capture the main UI AND the inspector panel
pulp run --audio-inspector --screenshot ui.png
# → ui.png and ui.audio-inspector.png
```

## Dev-on / ship-off gating

The probe and Inspector are gated behind the `PULP_ENABLE_AUDIO_PROBES` CMake
option, which is **ON by default for dev/example builds**. A release/ship
standalone configures it OFF so the shipped binary carries no probe and no
inspector surface:

```bash
# Ship build: strip the probe + inspector
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DPULP_ENABLE_AUDIO_PROBES=OFF
```

When the option is OFF, the CLI flags still parse, but the standalone host has
no probe to read. `--audio-inspector` is a no-op, and
`--audio-probe-json` is unsupported in that binary: the standalone launch fails
before the window opens, logs `PULP_ENABLE_AUDIO_PROBES=OFF`, and does not
write JSON.

## Programmatic readout: `--audio-probe-json`

A visible window is useless to an agent or a CI job — they need the scalar
facts as text. `pulp run --audio-probe-json PATH` runs the host headlessly,
lets the audio path produce a few blocks, writes the probe's latest snapshot
as a flat JSON object to `PATH`, then exits.

```bash
pulp run --audio-probe-json /tmp/probe.json
cat /tmp/probe.json
```

`--audio-probe-json` implies `--headless` (one-shot dump + exit, like
`--screenshot`). It is forwarded as `--audio-probe-json <file>` and via
`PULP_AUDIO_PROBE_JSON=<file>`. The frame delay reuses the same
`--frames` / `PULP_FRAMES` mechanism as the screenshot capture.

Headless here means no visible UI, not no audio device. The standalone host
still runs live so the output-boundary probe can observe real callback/device
state, and that may make audible sound if the target emits signal through the
default output. Agents and scripts should announce before using this live path
locally, cap the run duration, and prefer Audio Doctor / `HeadlessHost` when the
question can be answered offline without speakers.

The JSON shape:

```json
{
  "stage": "standalone_output_boundary",
  "sample_rate": 48000.0,
  "block_size": 256,
  "channel_count": 2,
  "sequence_number": 42,
  "peak_max": 0.5,
  "rms_max": 0.25,
  "peak_dbfs": -6.0206,
  "rms_dbfs": -12.0412,
  "clip_count": 0,
  "nan_inf_count": 0,
  "clipped_blocks": 0,
  "nan_blocks": 0,
  "silence_run_blocks": 0,
  "callbacks": 99,
  "underruns": 0,
  "device_xruns": 0,
  "cpu_overloads": 0
}
```

`peak_dbfs` / `rms_dbfs` are `null` when the corresponding linear value is 0,
so a reader can distinguish true silence from a finite low level (JSON has no
infinity literal). The snapshot→JSON mapping is the pure
`pulp::audio::audio_probe_snapshot_to_json()` helper
(`pulp/audio/audio_probe_json.hpp`).

## Interpreting common JSON states

| Result | Likely meaning | Next useful step |
|---|---|---|
| `callbacks == 0` | The host did not process audio before the dump. | Increase `--frames`; check standalone launch/device setup. |
| `callbacks > 0`, `peak_max == 0`, `rms_max == 0`, dBFS fields are `null` | The observed output stage is truly silent. | If silence is unexpected, check MIDI/input routing, graph connections, bypass/mute params, or the processor's `process()` branch. |
| `peak_max > 0`, `rms_max > 0`, zero error counters | Audio is flowing at the standalone output boundary. | Use the live window for visual inspection or Audio Doctor for deeper signal quality checks. |
| `clip_count > 0` or `clipped_blocks > 0` | At least one sample hit/exceeded the clipping threshold. | Inspect gain staging and run an offline render if exact clipping location matters. |
| `nan_inf_count > 0` or `nan_blocks > 0` | The DSP produced invalid floating-point samples. | Treat as a bug; inspect DSP math/state initialization near the changed code. |
| `device_xruns > 0` or `cpu_overloads > 0` | The runtime/device reported stress while the host was running. | Recheck in Release, then profile or reduce realtime work. |
| `sequence_number` stops advancing in the live window | The probe is stale; audio processing is idle or the host stopped publishing snapshots. | Check transport/device lifecycle before debugging DSP. |

The JSON is a snapshot, not a log. If you need history, run a longer scenario
or add a purpose-built trace around the suspected path.

### MCP access

The Claude/Codex-facing path uses the existing `pulp-mcp` binary. Call
`pulp_audio_probe_json` to run the same `pulp run --audio-probe-json` one-shot
flow and receive the probe object as `structuredContent`:

```json
{
  "target": "pulp-audio-inspector-demo",
  "frames": 90
}
```

`target` is optional and maps to `pulp run <target>`. `frames` is optional and
defaults to 90. This is not a second MCP server and not the future persistent
live socket; it is the agent-friendly wrapper for the JSON dump that exists
today.

The MCP JSON path reports scalar probe facts only. It does not return waveform
pixels or apply the live window's trigger/grid/scale controls.

### Agent workflow

An agent working on audio software should treat `pulp_audio_probe_json` as the
first live-health check, not as a replacement for audio analysis:

1. Call `pulp_audio_probe_json` with the standalone target under investigation.
2. If the tool returns an error, report the launch/probe problem before making
   DSP claims.
3. If callbacks are zero, adjust the run length (`frames`) or inspect host
   startup.
4. If levels are silent but callbacks advance, debug routing, input stimulus,
   bypass state, graph wiring, or the processor output path.
5. If clipping or NaN/Inf counters are non-zero, prioritize DSP/gain/state bugs.
6. If the snapshot is healthy but the user reports "sounds wrong", switch to
   Audio Doctor or a scenario render; the live probe cannot diagnose spectral or
   perceptual quality.

## End-to-end demo harness

The in-tree `pulp-audio-inspector-demo` example is the recommended live proof
target. It uses `StandaloneApp::run_with_editor()` and emits a continuous sine
tone from `Processor::process()`, so the output-boundary probe has non-silent
data without a DAW, MIDI input, or a physical audio input.

```bash
# Programmatic proof: JSON should show non-zero peak/RMS and callbacks.
pulp run pulp-audio-inspector-demo \
  --audio-probe-json /tmp/pulp-audio-inspector-demo.json \
  --frames 90

# Visual proof: writes the main UI plus the inspector panel sibling capture.
pulp run pulp-audio-inspector-demo \
  --audio-inspector \
  --screenshot /tmp/pulp-audio-inspector-demo.png \
  --frames 90
# -> /tmp/pulp-audio-inspector-demo.audio-inspector.png
```

The committed reference captures live under
`examples/audio-inspector-demo/fixtures/` as `editor-main.png` and
`audio-inspector-panel.png`.

Use this demo for GUI end-to-end checks before changing the Inspector window,
the `pulp run` flags, or the future live MCP socket path.

## Live vs offline at a glance

| | Live Audio Inspector | Offline Audio Doctor |
|---|---|---|
| Surface | `pulp run --audio-inspector` / `--audio-probe-json` | `pulp audio validate ...` |
| Input | A running audio host (RT probe) | A rendered WAV file |
| Output | Floating window + JSON snapshot | THD/spectrum/compare/assert reports |
| Gate | `PULP_ENABLE_AUDIO_PROBES` (dev-on/ship-off) | Always available |

See the [CLI reference](../reference/cli.md#run) for the full `pulp run`
flag list.
