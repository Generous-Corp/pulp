---
name: audio-inspect
description: Open the developer Audio Inspector window — live meters, probe-stage status, copied waveform, L/R level-match + balance, and a device/runtime summary
---

The Audio Inspector is a separate developer tool window that shows what audio is
flowing right now, built on the realtime audio probe. It is a sibling of the
layout inspector (`Cmd+I` / `Ctrl+I`), not a tab inside it — the two tools own
independent state models but share the same command-routing and window
primitives.

## Opening it

The window opens/focuses through a rebindable command registered with the
shell's `CommandRegistry`:

- Default chord: `Cmd+Shift+A` (macOS) / `Ctrl+Shift+A` (Windows/Linux).
- Command id: `AudioInspectorWindow::kToggleAudioInspector` (ASCII `PLAI`),
  distinct from the layout inspector's `PLPI`.
- Rebind it like any other command via the `KeyMappingEditor`; the binding
  persists through `ShortcutMap`.
- Env var: launch with `PULP_AUDIO_INSPECTOR=1` to open the window at startup.
- CLI: `pulp run --audio-inspector` (forwards the flag + sets the env var).
  Composes with `--screenshot` to also capture the panel as
  `<stem>.audio-inspector.png` in a headless run.

### Programmatic readout (agents / CI)

When you can't look at a window, read the probe as JSON:

```bash
pulp run --audio-probe-json /tmp/probe.json   # headless one-shot dump + exit
```

This writes `output_probe().latest()` (+ the `AudioStats` subset) as a flat
JSON object — `stage`, `sample_rate`, `block_size`, `channel_count`,
`sequence_number`, `peak_max`/`rms_max`, `peak_dbfs`/`rms_dbfs` (null on true
silence), `clip_count`, `nan_inf_count`, `clipped_blocks`, `nan_blocks`,
`silence_run_blocks`, `callbacks`, `underruns`, `device_xruns`,
`cpu_overloads`. The mapping is the pure
`pulp::audio::audio_probe_snapshot_to_json()` helper
(`pulp/audio/audio_probe_json.hpp`), so it is unit-tested without a device.
Both flags are gated by `PULP_ENABLE_AUDIO_PROBES` (dev-on, ship-off). See
`docs/guides/audio-inspector.md`.

A shell wires it once:

```cpp
pulp::view::CommandRegistry registry;
pulp::view::route_global_keys(root, registry);   // single sanctioned key path

pulp::view::AudioInspectorWindow audio_inspector;
audio_inspector.set_probe(&my_audio_probe);       // optional; null → "no probe"
audio_inspector.register_command_handler(registry);
```

Then poll it once per UI tick (e.g. from the editor's frame timer):

```cpp
audio_inspector.set_device_stats(device_manager.stats());  // xruns / overloads
audio_inspector.poll();                                     // reads probe once
```

`poll()` reads `AudioProbe::latest()` exactly once per call (single-consumer
TripleBuffer) and refreshes the panel. No FFT, allocation, logging, or file I/O
touches the realtime audio thread — the probe guarantees that and the window
only consumes the published summary.

## What it shows

- Probe-stage status: processor output, standalone boundary, meter bridge,
  device callback, or graph node — whichever stage the wired probe observes.
- Level meters: peak / RMS (dBFS), clip count, NaN/Inf count, silence-run blocks.
- A time-domain waveform over the most recent fixed-capacity COPIED snapshot
  (capture must be enabled on the probe; otherwise the trace is empty, honestly).
- L/R level-match ratio + channel balance, derived from
  per-channel RMS.
- Device/runtime summary: sample rate, block size, channel count, callbacks,
  mirrored device xruns and CPU overloads.

## Honest unavailable state

When no probe is wired, or the probe's sequence number stops advancing (the
audio thread is idle), the window shows an explicit "no probe" / "stale" state
rather than faked zeros — so a silent stage reads as silent, not as broken.

This slice ships meters + status + waveform only. Live spectrum and
freeze/export are deferred to later slices (they need an off-callback FFT path
and a stable artifact schema). For controlled-stimulus / offline analysis, use
the `audio-harness` skill and the Audio Doctor analyzers.
