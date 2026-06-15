# 06 - Reaper (VST3) Result

**Host**: REAPER 7.74/macOS-arm64
**Format**: VST3
**OS**: macOS 26.5.1 arm64
**Date**: 2026-06-15
**Pulp commit**: `1e4348a16376`
**Bench plugin version**: `1.0.0`

## Run Notes

Autonomous re-validation on current `main` (after the #4060/#4061 Windows
CI fixes merged). `PulpHostBench` VST3 was rebuilt from this checkout
(`PulpHostBench_VST3`), installed under the user VST3 directory, and exercised
headlessly via `tools/scripts/run_reaper_hostbench_smoke.py --format vst3`,
which launches REAPER with a generated ReaScript (new project → insert
`VST3: PulpHostBench (Pulp)` → set a parameter → transport play/stop → bypass
toggle → quit). The smoke reported `ok=true`, `returncode=0`, and no missing
required events. The session log is checked in under
`logs/REAPER-VST3-20260615T044210Z-pid28846.log`.

The run produced `session_start`, `define_parameters`, `prepare` (×2),
repeated `bus_layout_proposal` (×8, including an unsupported layout accepted via
the silence accommodation), `process_without_prepare` (×83), `sidechain_edge`,
`serialize_plugin_state`, `release`, and `session_end` events.

This is a scripted smoke: it does not drive real-device playback or the plugin
GUI, so transport-playing edges, keyboard routing, sample-rate drift, and
session reopen are recorded as Not Triggered (see the 2026-06-12 manual run for
`reaper_vst3_gesture_ordering` confirmation).

## Capability Evidence

| Capability | Observed | Notes |
|------------|----------|-------|
| Load | Confirmed | Log contains `session_start`, `define_parameters`, and `prepare`. |
| Params | Confirmed | Log contains `define_parameters` and `serialize_plugin_state`. |
| Sidechain | Confirmed | Log contains a `sidechain_edge` event. |
| Multi-bus | Confirmed | Log contains repeated `bus_layout_proposal` events with multi-input arrangements. |

## Result

| Quirk flag | Row | Observed | Notes |
|------------|-----|----------|-------|
| `reaper_process_while_bypassed` | R1 | Confirmed | Repeated `process_without_prepare` entries after REAPER released the initial prepare state. |
| `reaper_permissive_bus_arrangements` | R3 | Confirmed | Repeated `bus_layout_proposal` entries for the loaded VST3. |
| `reaper_vst3_gesture_ordering` | #15 | Not Triggered | No `process_is_playing_edge`; headless smoke has no active playback device. Confirmed in the 2026-06-12 manual run. |
| `reaper_keyboard_passthrough` | R2 | Not Triggered | Keyboard routing not exercised by the scripted smoke. |
| `reaper_anticipative_fx_buffer_variability` | R4 | Not Triggered | No `process_buffer_overrun` / `process_sample_rate_drift`. |
| `reaper_midsession_setstate` | R6 | Not Triggered | No `deserialize_plugin_state` (smoke does not reopen a session). |

**Log file(s)**: `logs/REAPER-VST3-20260615T044210Z-pid28846.log`
**Reaper version**: `7.74/macOS-arm64` — **macOS**: `26.5.1` — **Date**: `2026-06-15`
