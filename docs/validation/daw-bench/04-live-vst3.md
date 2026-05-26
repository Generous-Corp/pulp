# 04 — Ableton Live (VST3)

**Target quirks**:

- `live_vst3_canresize_ignore` (row #5) — Live probes `checkSizeConstraint`
  even when `canResize == false`.
- `live_vst3_windows_dpi_defer` (row #6) — Windows-only DPI/window-handle
  timing race on per-monitor-V2 DPI.
- `double_string_buffer_for_live_10_1_13` (row added 2026-05-25) — Live
  10.1.13 over-reads from `IInfoListener::getString` channel-name buffers.

**Prereqs**: Ableton Live 11 or 12 Suite (or Standard), `PulpHostBench.vst3`
installed and Live rescan completed (Preferences → Plug-Ins → Rescan).

## Steps

1. **Clear stale logs**.

2. **Launch Live**, new empty project, 48 kHz.

3. **Drop PulpHostBench on Audio 1** (Plug-Ins browser → Pulp → PulpHostBench).

4. **Confirm session start**:
   ```bash
   head -5 ~/Library/Logs/PulpHostBench/AbletonLive-VST3-*.log
   # session_start  host=AbletonLive  format=VST3
   ```

5. **Open the plugin window**. Click the header to toggle the editor open / closed
   three times.
   ```bash
   grep -c "view_opened\|view_closed" ~/Library/Logs/PulpHostBench/AbletonLive-VST3-*.log
   # Expect 6 events total (3 open + 3 close).
   ```

6. **Resize the plugin window** by dragging the corner (Live 11+ supports
   plugin window resize via the gear icon → "Show plug-in window" toggle).
   ```bash
   grep "view_resized\|bus_layout_proposal" ~/Library/Logs/PulpHostBench/AbletonLive-VST3-*.log
   ```
   → catalog row **#5**: Live calling `view_resized` even on plugins that
     advertise non-resizable size is the symptom. The bench plugin's bounds
     are 400×300 with no explicit aspect; if Live forces a different size
     (other than your drag), record that here.

7. **(Windows only)** Move the Live window between displays with different DPI
   (e.g. 100% and 150%). Watch for editor-render glitches on the first frame.
   This is the qualitative check for row #6 — the log file won't show DPI
   directly, but a `view_resized` event with new dimensions is the trigger
   point where the bug shows up visually.

8. **Switch the Audio 1 routing** from "Ext. Out 1/2" stereo to "Ext. Out 1/2"
   mono via the small triangle next to the routing dropdown.
   ```bash
   grep "bus_layout_proposal" ~/Library/Logs/PulpHostBench/AbletonLive-VST3-*.log
   # Look for: inputs=1  outputs=1  (or inputs=2  outputs=1)
   ```
   → confirms Live re-proposes a layout when the user changes channel format,
     and that the bench plugin's permissive `is_bus_layout_supported`
     accepts it. If the plugin disappears from the chain, that's evidence
     row #25 (`silence_unsupported_bus_arrangements`) is not being honored.

9. **Set the Audio 1 sidechain input** to "Audio 2" (right-click on the
   plugin → Sidechain Input).
   ```bash
   grep "sidechain_edge" ~/Library/Logs/PulpHostBench/AbletonLive-VST3-*.log
   # Expect: connected=true after wiring the sidechain.
   ```

10. **Hit Play for a few seconds, then Stop**.
    ```bash
    grep "transport_changed\|process_is_playing_edge" ~/Library/Logs/PulpHostBench/AbletonLive-VST3-*.log
    ```

11. **Save the .als project**, close, reopen — confirm state round-trip
    (the Bench Gain knob value should persist).

12. **Quit Live** to flush.

## Result

| Quirk flag                                    | Row | Observed | Notes |
|-----------------------------------------------|-----|----------|-------|
| `live_vst3_canresize_ignore`                  | #5  | <C/R/NT> | <step 6 observations> |
| `live_vst3_windows_dpi_defer` (Win only)      | #6  | <C/R/NT> | <step 7 observations or N/A on macOS> |
| `double_string_buffer_for_live_10_1_13`       | (LessonOnly) | <C/R/NT> | <set Refuted unless on 10.1.13 specifically> |
| `silence_unsupported_bus_arrangements`        | #25 | <C/R/NT> | <step 8 observations> |
| `synthesize_bypass_parameter`                 | #23 | <C/R/NT> | <Live shows a "bypass" toggle in the device header> |

**Log file(s)**: `<paste>`
**Live version**: `<11.x.y / 12.x.y>` — **OS**: `<x.y>` — **Date**: `<YYYY-MM-DD>`
