# 05 — Bitwig Studio (VST3)

**Target quirks**:

- `bitwig_vst3_linux_repaint_after_resize` (row #8) — Linux-only repaint bug.
- `bitwig_vst3_setbusarrangements_while_active` (row #9) — Bitwig calls
  `setBusArrangements` while the plugin is active (spec violation).

**Prereqs**: Bitwig Studio 5+, `PulpHostBench.vst3` installed and Bitwig has
rescanned plugins.

## Steps

1. **Clear stale logs**.

2. **Launch Bitwig Studio**, new empty project, 48 kHz.

3. **Add an audio track**, drag PulpHostBench from the device browser into
   the device chain.

4. **Confirm session log**:
   ```bash
   head -5 ~/Library/Logs/PulpHostBench/BitwigStudio-VST3-*.log
   # session_start  host=BitwigStudio  format=VST3
   ```
   (If the file is named `Bitwig-` instead, that's the same — the exact spelling
   depends on `host_type_name(HostType::Bitwig)`.)

5. **Drag the plugin window's corner** to resize it (Bitwig 5+ supports
   plugin-window resize through the "expand" toggle in the device header).
   ```bash
   grep "view_resized" ~/Library/Logs/PulpHostBench/Bitwig*-VST3-*.log
   ```
   → catalog row **#8** (Linux only): after a `view_resized` event, the
     plugin should repaint. Visually verify the bench plugin's
     "Last event" string updates on screen. If it freezes / shows stale
     content, the Linux repaint-after-resize quirk is firing and the
     flag should stay enabled.

6. **Change the track input from stereo to mono** (right-click track → I/O →
   change input format).
   ```bash
   grep "bus_layout_proposal\|process_without_prepare" ~/Library/Logs/PulpHostBench/Bitwig*-VST3-*.log
   ```
   → catalog row **#9**: if `bus_layout_proposal` events arrive WITHOUT a
     fresh `prepare` event between them — i.e. the host is changing
     layout without deactivating — that's evidence of the spec-violation
     quirk. The bench's `is_bus_layout_supported` always accepts, but the
     pattern in the log is the evidence.

7. **Hit Play**, let it loop for a few seconds, Stop.
   ```bash
   grep "transport_changed\|tempo_changed" ~/Library/Logs/PulpHostBench/Bitwig*-VST3-*.log
   ```

8. **Save .bwproject, close, reopen** to confirm state round-trip.

9. **Quit Bitwig**.

## Result

| Quirk flag                                       | Row | Observed | Notes |
|--------------------------------------------------|-----|----------|-------|
| `bitwig_vst3_linux_repaint_after_resize` (Linux) | #8  | <C/R/NT> | <visual repaint check from step 5> |
| `bitwig_vst3_setbusarrangements_while_active`    | #9  | <C/R/NT> | <step 6 pattern> |
| `silence_unsupported_bus_arrangements`           | #25 | <C/R/NT> | <bench accepted; plugin stayed loaded> |

**Log file(s)**: `<paste>`
**Bitwig version**: `<5.x.y>` — **OS**: `<x.y>` — **Date**: `<YYYY-MM-DD>`
