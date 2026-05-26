# 02 — Cubase 12 (VST3)

**Target quirks** (catalog rows in `host_quirks.hpp::HostQuirksMeta`):

- `cubase10_async_view_resize_queue` (row #1) — async view resize timing.
- `cubase10_param_gesture_ordering` (row #2) — automation gesture order.
- `cubase10_fractional_scale_correction` (row #3) — DPI rounding.
- General defaults: bypass synth (#23), latency clamp (#24), bus silence (#25).

> "Cubase 10" was the original catalog evidence — Cubase 11/12/13 retain the
> same behaviors except where Steinberg shipped fixes. If your bench host is
> Cubase 11 or 13, the script still applies; record the actual version in
> the Result section below.

**Prereqs**: Cubase Pro 12.x, `PulpHostBench.vst3` installed under
`~/Library/Audio/Plug-Ins/VST3/` (macOS) or
`C:\Program Files\Common Files\VST3\` (Windows).

## Steps

1. **Clear stale logs**:
   ```bash
   rm -rf ~/Library/Logs/PulpHostBench/           # macOS
   # rmdir /S /Q %LOCALAPPDATA%\PulpHostBench    # Windows
   ```

2. **Launch Cubase 12**, new empty project, 48 kHz / 32-bit float.

3. **Add an audio track**, insert PulpHostBench (Inserts → Pulp → PulpHostBench).

4. **Confirm log file exists + Cubase is detected**:
   ```bash
   head -5 ~/Library/Logs/PulpHostBench/Cubase-VST3-*.log
   # session_start  host=Cubase  format=VST3
   ```

5. **Resize the plugin window** (drag corner slowly).
   ```bash
   grep "view_resized" ~/Library/Logs/PulpHostBench/Cubase-VST3-*.log
   ```
   → catalog row **#1**: count the `view_resized` events. If Cubase emits MORE
     events than discrete drag pauses, the host is firing the async-resize race
     described in the catalog. If the count matches discrete pauses, the host
     coalesces — no async accommodation needed for this version.

6. **Move the bench window between displays** (if you have a multi-monitor setup
   with at least one non-integer scale factor like 125% or 150%).
   ```bash
   grep "view_resized" ~/Library/Logs/PulpHostBench/Cubase-VST3-*.log | tail -10
   ```
   → catalog row **#3**: the dimensions should reflect the destination display's
     scale. Fractional DPI typically shows up as widths/heights that are NOT
     multiples of 100/8.

7. **Right-click the Bench Gain parameter** → Show Automation → write-enable.
   Touch the knob, write a 2-second automation pass, stop. Play back. Touch the
   knob again to overwrite.
   ```bash
   grep "tempo_changed\|transport_changed\|process_is_playing_edge" \
       ~/Library/Logs/PulpHostBench/Cubase-VST3-*.log | tail -5
   ```
   → catalog row **#2**: the bench plugin can't directly see the gesture-edit
     sequence (that lives in the format adapter), but the parameter events
     show up in StateStore and the host-event ordering can be visually
     confirmed: the value should sweep on playback. If the automation lane
     ends up "touched" but value never changes, that's evidence of the
     gesture-ordering bug.

8. **Save project, close, reopen**.
   ```bash
   grep -E "serialize_plugin_state|deserialize_plugin_state" \
       ~/Library/Logs/PulpHostBench/Cubase-VST3-*.log
   ```
   → confirms state round-trip.

9. **Render-in-place / Export Audio Mixdown** with PulpHostBench on the track.
   ```bash
   grep "prepare\|suspend\|resume" ~/Library/Logs/PulpHostBench/Cubase-VST3-*.log | tail -10
   ```
   → tail-time + offline-render: Cubase may call `prepare()` with a different
     buffer size than the live session. If the bench logs `prepare` with a
     different `max_buffer_size`, the host is honoring the offline-render
     prepare contract.

10. **Quit Cubase** or remove the plugin.
    ```bash
    tail -3 ~/Library/Logs/PulpHostBench/Cubase-VST3-*.log
    # Expect: session_end at the bottom.
    ```

## Result

| Quirk flag                                    | Row | Observed | Notes |
|-----------------------------------------------|-----|----------|-------|
| `cubase10_async_view_resize_queue`            | #1  | <C/R/NT> |       |
| `cubase10_param_gesture_ordering`             | #2  | <C/R/NT> |       |
| `cubase10_fractional_scale_correction`        | #3  | <C/R/NT> |       |
| `synthesize_bypass_parameter`                 | #23 | <C/R/NT> |       |
| `clamp_latency_to_nonneg`                     | #24 | <C/R/NT> |       |
| `silence_unsupported_bus_arrangements`        | #25 | <C/R/NT> |       |

**Log file(s)**: `<paste path>`
**Cubase version**: `<Pro 12.x.y>` — **OS**: `<macOS x.y / Win x.y>` — **Date**: `<YYYY-MM-DD>`
