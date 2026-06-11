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
no probe to read, so `--audio-inspector` is a no-op and `--audio-probe-json`
writes nothing.

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

## Live vs offline at a glance

| | Live Audio Inspector | Offline Audio Doctor |
|---|---|---|
| Surface | `pulp run --audio-inspector` / `--audio-probe-json` | `pulp audio validate ...` |
| Input | A running audio host (RT probe) | A rendered WAV file |
| Output | Floating window + JSON snapshot | THD/spectrum/compare/assert reports |
| Gate | `PULP_ENABLE_AUDIO_PROBES` (dev-on/ship-off) | Always available |

See the [CLI reference](../reference/cli.md#run) for the full `pulp run`
flag list.
