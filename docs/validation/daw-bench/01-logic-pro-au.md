# 01 — Logic Pro (AU v2)

**Target quirks** (from `core/format/include/pulp/format/host_quirks.hpp::HostQuirksMeta`):

- `logic_au_channel_probe_cap` (catalog row #19) — Logic hangs probing >8 channels.
- `logic_au_tail_time_conversion` (catalog row #20) — AU tail time depends on cached sample rate.
- General lifecycle: `prepare` → `process` → `release` ordering.

**Prereqs**: macOS 13+, Logic Pro 10.7+, `PulpHostBench.component` installed under `~/Library/Audio/Plug-Ins/Components/` and re-validated by Logic (Logic Pro → Settings → Audio → Audio Units Manager → "Reset & Rescan Selection").

## Steps

1. **Clear stale logs** in a terminal:
   ```bash
   rm -rf ~/Library/Logs/PulpHostBench/
   ```

2. **Launch Logic Pro**, create a new empty project (Audio template, 48 kHz, 1 audio track).

3. **Insert PulpHostBench** on track 1 (Audio FX slot → Audio Units → Pulp → PulpHostBench).

4. **Confirm session log appears**:
   ```bash
   ls -lt ~/Library/Logs/PulpHostBench/ | head -3
   # Expect: LogicPro-AU-<timestamp>-pid<NNNN>.log
   ```

5. **Open the log** and confirm the **header**:
   ```bash
   head -5 ~/Library/Logs/PulpHostBench/LogicPro-AU-*.log
   # Look for: session_start  host=LogicPro  format=AU
   # Look for: prepare        sample_rate=48000  max_buffer_size=<N>
   ```
   → confirms Logic detected via `HostType::LogicPro` (catalog row #22 cross-host detection).

6. **Hit Play, let one second go by, hit Stop**.
   ```bash
   grep -E "transport_changed|process_is_playing_edge" ~/Library/Logs/PulpHostBench/LogicPro-AU-*.log
   ```
   → confirms `on_host_transport_changed()` fires and `ProcessContext::is_playing` flips.

7. **Switch the project's sample rate** to 44.1 kHz (File → Project Settings → Audio).
   Hit Play once again.
   ```bash
   grep "process_sample_rate_drift\|prepare" ~/Library/Logs/PulpHostBench/LogicPro-AU-*.log | tail -5
   # Expect either: a NEW prepare event (sample_rate=44100), OR
   #                process_sample_rate_drift entries.
   ```
   → catalog row **#20**: if a fresh `prepare` event arrives, the AU adapter is
     correctly re-cached; if `process_sample_rate_drift` events appear, that's
     evidence the AU adapter is NOT re-preparing on sample-rate change (so
     `logic_au_tail_time_conversion` should stay Speculative pending a fix).

8. **Try inserting PulpHostBench on a multi-output bus** (right-click track →
   Configure Track Header → enable Output → set Output to Surround 5.1 if your
   audio interface supports it, otherwise the largest channel count Logic allows).
   ```bash
   grep "bus_layout_proposal" ~/Library/Logs/PulpHostBench/LogicPro-AU-*.log
   # Look for: inputs=N  outputs=M  where M reflects Logic's probe.
   ```
   → catalog row **#19**: if Logic does NOT probe channels >8 before stalling /
     crashing, that's evidence the existing cap is doing its job.

9. **Save the project**, close it, reopen it.
   ```bash
   grep -E "serialize_plugin_state|deserialize_plugin_state" ~/Library/Logs/PulpHostBench/LogicPro-AU-*.log
   ```
   → confirms state round-trip; `marker_ok=true` on the deserialize line is the
     blob-integrity proof.

10. **Resize the plugin window** (drag the corner).
    ```bash
    grep "view_resized" ~/Library/Logs/PulpHostBench/LogicPro-AU-*.log
    ```
    → confirms `on_view_resized()` fires with the new dimensions.

11. **Quit Logic** (or just delete the plugin from the track).
    ```bash
    tail -5 ~/Library/Logs/PulpHostBench/LogicPro-AU-*.log
    # Expect: release, then session_end.
    ```

## Result

Paste this section into the follow-up PR (one row per catalog flag):

| Quirk flag                            | Catalog row | Observed | Notes                                              |
|---------------------------------------|-------------|----------|----------------------------------------------------|
| `logic_au_channel_probe_cap`          | #19         | <Confirmed/Refuted/Not Triggered> | <e.g. "Logic never probed above 8 ch in step 8."> |
| `logic_au_tail_time_conversion`       | #20         | <C/R/NT> | <e.g. "prepare re-fired on SR change in step 7."> |
| `synthesize_bypass_parameter` (general) | #23       | <C/R/NT> | <e.g. "Logic shows a bypass control regardless."> |
| `clamp_latency_to_nonneg` (general)   | #24         | <C/R/NT> | <Logic queried latency at prepare; see log.>      |
| `silence_unsupported_bus_arrangements` (general) | #25 | <C/R/NT> | <e.g. "step 8 layout was accepted; no crash.">    |

**Log file(s)**: `<paste relative path under docs/validation/daw-bench/results/.../logs/>`
**Logic Pro version**: `<x.y.z>` — **macOS**: `<x.y.z>` — **Date**: `<YYYY-MM-DD>`
