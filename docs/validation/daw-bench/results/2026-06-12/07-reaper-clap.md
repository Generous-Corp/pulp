# 07 - Reaper (CLAP) Result

**Host**: REAPER 7.74/macOS-arm64
**Format**: CLAP
**OS**: macOS 26.5.1 arm64
**Date**: 2026-06-12
**Pulp commit**: `e941153f0e7e`
**Bench plugin version**: `1.0.0`

## Run Notes

PulpHostBench CLAP was built from the Release `build-cov` configuration and
installed under the user CLAP plug-in directory. REAPER was launched with a
temporary ReaScript that opened a throwaway project, inserted
`CLAP: PulpHostBench (Pulp)` on a track, played briefly, saved the project, and
quit. The session log is checked in under
`logs/REAPER-CLAP-20260612T211307Z-pid73106.log`.

The run produced `session_start`, `define_parameters`, `prepare`,
`serialize_plugin_state`, `process_is_playing_edge`, `sidechain_edge`,
`release`, and `session_end` events. It did not exercise bypass toggling,
sample-rate-drift rendering, FX-chain reload/deserialization, MIDI input, or
editor UI behavior.

## Result

| Quirk flag | Row | Observed | Notes |
|------------|-----|----------|-------|
| `reaper_process_while_bypassed` | R1 | Not Triggered | Bypass toggling was not exercised by this scripted CLAP evidence run. |
| `reaper_anticipative_fx_buffer_variability` | R4 | Not Triggered | No `process_buffer_overrun` or `process_sample_rate_drift` event was observed. |
| `reaper_midsession_setstate` | R6 | Not Triggered | The log contains `serialize_plugin_state`, but no `deserialize_plugin_state` event was observed. |
| `reaper_clap_transport_edges` | R7 | Confirmed | The log contains `process_is_playing_edge` events for both play and stop. |

**Log file(s)**: `logs/REAPER-CLAP-20260612T211307Z-pid73106.log`
