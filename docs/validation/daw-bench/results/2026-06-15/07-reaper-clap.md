# 07 - Reaper (CLAP) Result

**Host**: REAPER 7.74/macOS-arm64
**Format**: CLAP
**OS**: macOS 26.5.1 arm64
**Date**: 2026-06-15
**Pulp commit**: `1e4348a16376`
**Bench plugin version**: `1.0.0`

## Run Notes

Autonomous re-validation on current `main` (after the #4060/#4061 Windows
CI fixes merged). `PulpHostBench` CLAP was rebuilt from this checkout
(`PulpHostBench_CLAP`), installed under the user CLAP directory, and exercised
headlessly via `tools/scripts/run_reaper_hostbench_smoke.py --format clap`
(new project → insert `CLAP: PulpHostBench (Pulp)` → set a parameter →
transport play/stop → bypass toggle → quit). The smoke reported `ok=true`,
`returncode=0`, and no missing required events. The session log is checked in
under `logs/REAPER-CLAP-20260615T044212Z-pid29054.log`.

The run produced `session_start`, `define_parameters`, `prepare` (×2),
`sidechain_edge`, `serialize_plugin_state`, `release`, and `session_end`
events.

This is a scripted smoke: it does not drive real-device playback or the plugin
GUI, so process-while-bypassed, buffer variability, session reopen, and
transport edges are recorded as Not Triggered.

## Capability Evidence

| Capability | Observed | Notes |
|------------|----------|-------|
| Load | Confirmed | Log contains `session_start`, `define_parameters`, and `prepare`. |
| Params | Confirmed | Log contains `define_parameters` and `serialize_plugin_state`. |
| Sidechain | Confirmed | Log contains a `sidechain_edge` event. |

## Result

| Quirk flag | Row | Observed | Notes |
|------------|-----|----------|-------|
| `reaper_process_while_bypassed` | R1 | Not Triggered | No `process_without_prepare`; no active playback device in the headless smoke. |
| `reaper_anticipative_fx_buffer_variability` | R4 | Not Triggered | No `process_buffer_overrun` / `process_sample_rate_drift`. |
| `reaper_midsession_setstate` | R6 | Not Triggered | No `deserialize_plugin_state` (smoke does not reopen a session). |
| `reaper_clap_transport_edges` | R7 | Not Triggered | No transport-playing edge; headless smoke has no active playback device. |

**Log file(s)**: `logs/REAPER-CLAP-20260615T044212Z-pid29054.log`
**Reaper version**: `7.74/macOS-arm64` — **macOS**: `26.5.1` — **Date**: `2026-06-15`
