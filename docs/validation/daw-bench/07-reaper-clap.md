# 07 — Reaper (CLAP)

**Target quirks**:

- `reaper_process_while_bypassed` (R1) — applies across CLAP + VST3 + AU.
- `reaper_anticipative_fx_buffer_variability` (R4) — also CLAP-relevant.
- `reaper_midsession_setstate` (R6) — CLAP `state.save/load` round-trip.
- R7 (catalog note) — Reaper CLAP is reference-strict; any drift here
  surfaces bugs other hosts hide.

**Prereqs**: Reaper 7.20+ (CLAP support landed in 7.0 and improved through
7.x), `PulpHostBench.clap` installed under
`~/Library/Audio/Plug-Ins/CLAP/` (macOS) or the platform CLAP folder.

## Steps

1. **Clear stale logs**.

2. **Launch Reaper**, new empty project. Verify CLAP is enabled:
   Options → Preferences → Plug-ins → CLAP → "Enable CLAP support".

3. **Add a track, insert PulpHostBench (CLAP)** from the FX browser. (If you
   have both the VST3 and CLAP variants installed, pick the CLAP filter
   first to be sure.)

4. **Confirm session log shows format=CLAP**:
   ```bash
   head -5 ~/Library/Logs/PulpHostBench/Reaper-CLAP-*.log
   # session_start  host=Reaper  format=CLAP
   ```

5. **Hit Play, let it run a few seconds, Stop**.
   ```bash
   grep "transport_changed\|tempo_changed\|process_is_playing_edge" \
       ~/Library/Logs/PulpHostBench/Reaper-CLAP-*.log
   ```
   → catalog row **R7** sanity: every transport edge produces an event. If a
     play-then-stop yields zero `process_is_playing_edge` events, the CLAP
     adapter is not surfacing transport state correctly.

6. **Bypass via the FX-chain bypass button**, hit Play, observe the bench
   plugin's "Last event" indicator (visually) — it should keep advancing
   `process_*` events even while bypassed if R1 is firing.

7. **Render to file at a different sample rate** (96 kHz):
   ```bash
   grep "prepare\|process_sample_rate_drift\|process_buffer_overrun" \
       ~/Library/Logs/PulpHostBench/Reaper-CLAP-*.log
   ```
   → catalog row **R4**.

8. **Save an FX chain** (right-click bench → Save FX chain), delete + reload.
   ```bash
   grep -E "serialize_plugin_state|deserialize_plugin_state" \
       ~/Library/Logs/PulpHostBench/Reaper-CLAP-*.log
   ```
   → catalog row **R6** — CLAP `clap_plugin_state` round-trip.

9. **Add a second track with a stereo audio file, set its output as a
   sidechain to the bench-plugin track via Reaper's pin connector** (right-
   click bench → Pin Connector → enable sidechain input).
   ```bash
   grep "sidechain_edge" ~/Library/Logs/PulpHostBench/Reaper-CLAP-*.log
   # Expect: connected=true once the sidechain is wired and audio is playing.
   ```

10. **Quit Reaper**.

## Result

| Quirk flag                              | Row | Observed | Notes |
|-----------------------------------------|-----|----------|-------|
| `reaper_process_while_bypassed`         | R1  | <C/R/NT> | <step 6> |
| `reaper_anticipative_fx_buffer_variability` | R4 | <C/R/NT> | <step 7> |
| `reaper_midsession_setstate`            | R6  | <C/R/NT> | <step 8> |
| `silence_unsupported_bus_arrangements`  | #25 | <C/R/NT> | <bench accepted layouts> |
| (CLAP canary — R7)                      | —   | <C/R/NT> | <any new drift visible vs the VST3 run?> |

**Log file(s)**: `<paste>`
**Reaper version**: `<7.x.y>` — **OS**: `<x.y>` — **Date**: `<YYYY-MM-DD>`
