# 06 - Reaper (VST3) Result

**Host**: REAPER 7.74/macOS-arm64
**Format**: VST3
**OS**: macOS 26.5.1 arm64
**Date**: 2026-06-12
**Pulp commit**: `1ed767e69251`
**Bench plugin version**: `1.0.0`

## Run Notes

PulpHostBench VST3 was built from the Release `build-dsp-offline-render`
configuration and installed under the user VST3 plug-in directory. REAPER was
launched with an isolated temporary resource directory, scanned the VST3, and
loaded `VST3: PulpHostBench (Pulp)` on the master FX chain. The session log is
checked in under `logs/REAPER-VST3-20260612T172051Z-pid5104.log`.

The run produced `session_start`, `define_parameters`, repeated
`bus_layout_proposal`, `prepare`, `process_is_playing_edge`,
`process_without_prepare`, `sidechain_edge`, `view_opened`, `view_resized`, and
`serialize_plugin_state` events.

## Capability Evidence

| Capability | Observed | Notes |
|------------|----------|-------|
| Load | Confirmed | Log contains `session_start`, `define_parameters`, and `prepare` events. |
| Params | Confirmed | Log contains `define_parameters` and `serialize_plugin_state` events. |
| Sidechain | Confirmed | Log contains a `sidechain_edge` event. |
| Multi-bus | Confirmed | Log contains repeated `bus_layout_proposal` events with a multi-input arrangement. |

## Result

| Quirk flag | Row | Observed | Notes |
|------------|-----|----------|-------|
| `reaper_vst3_gesture_ordering` | #15 | Confirmed | Log contains `process_is_playing_edge`; this is the existing promotion rule for the VST3 gesture-ordering defense. |
| `reaper_process_while_bypassed` | R1 | Confirmed | Log contains repeated `process_without_prepare` entries after REAPER released the initial prepare state. |
| `reaper_keyboard_passthrough` | R2 | Not Triggered | Keyboard routing was not exercised in this scripted evidence run. |
| `reaper_keyboard_only_space` | (LO) | Not Triggered | Keyboard-only-space behavior was not exercised in this scripted evidence run. |
| `reaper_permissive_bus_arrangements` | R3 | Confirmed | Log contains repeated `bus_layout_proposal` entries for the loaded VST3. |
| `reaper_anticipative_fx_buffer_variability` | R4 | Not Triggered | No `process_buffer_overrun` or `process_sample_rate_drift` event was observed. |
| `reaper_midsession_setstate` | R6 | Not Triggered | No `deserialize_plugin_state` event was observed. |
| Cross-format defaults (#23/#24/#25) | - | Not Triggered | This run did not exercise the cross-format default matrix. |

**Log file(s)**: `logs/REAPER-VST3-20260612T172051Z-pid5104.log`
