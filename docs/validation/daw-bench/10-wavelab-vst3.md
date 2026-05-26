# 10 — Wavelab (VST3)

**Target quirks**:

- `wavelab_vst3_defer_activation` (row #10) — `setBusArrangements` called
  re-entrantly inside `prepareToPlay` when the plugin invokes `setLatencySamples`.
- `wavelab_state_blob_fallback` (row #11) — Wavelab tolerates state-blob
  read failures; strict failure causes session loss.
- `tolerate_state_read_nontrue_status` (LessonOnly) — `IBStream::read`
  may return non-`kResultTrue` while still having populated the buffer.

**Prereqs**: Steinberg Wavelab 11.1+ (or earlier — the bench will confirm
behavior on whichever version you have), `PulpHostBench.vst3` installed.

## Steps

1. **Clear stale logs**.

2. **Launch Wavelab**, open an Audio Editor with any source file
   (Wavelab is mastering-centric — most users will start in the Audio
   Editor window rather than a Montage).

3. **Insert PulpHostBench in the Master Section** (Master Section panel →
   any insert slot → choose PulpHostBench).

4. **Confirm session log**:
   ```bash
   head -5 ~/Library/Logs/PulpHostBench/Wavelab-VST3-*.log
   # session_start  host=Wavelab  format=VST3
   ```

5. **Hit Play in the Audio Editor**.
   ```bash
   grep -E "prepare|bus_layout_proposal|process_without_prepare" \
       ~/Library/Logs/PulpHostBench/Wavelab-VST3-*.log | head -20
   ```
   → catalog row **#10**: count the order of `prepare` and `bus_layout_proposal`
     events. If `bus_layout_proposal` interleaves WITH or INSIDE the prepare
     sequence (i.e. multiple proposals between processor_construct and the
     first process block), Wavelab's re-entrant negotiation pattern is firing.

6. **Save the Audio Editor's plugin chain as a preset**, close the file,
   reopen the saved preset.
   ```bash
   grep -E "serialize_plugin_state|deserialize_plugin_state" \
       ~/Library/Logs/PulpHostBench/Wavelab-VST3-*.log
   ```
   → catalog rows **#11** / `tolerate_state_read_nontrue_status`: the bench's
     `marker_ok` field on the deserialize line confirms whether Wavelab read
     back the exact bytes. If `bytes < 18` or `marker_ok=false`, the Wavelab-
     style truncation / non-true-status path is the cause and the quirk is
     warranted.

7. **Try a Montage workflow** (File → New → Audio Montage), drop a clip in,
   put PulpHostBench on the clip's effect chain. Play.
   ```bash
   ls -lt ~/Library/Logs/PulpHostBench/ | head -3
   # A new log file (different PID) typically appears for Montage's instance.
   ```

8. **Render the Montage to file** (Workspace → Render).
   ```bash
   grep "prepare\|suspend\|resume" ~/Library/Logs/PulpHostBench/Wavelab-VST3-*.log | tail -10
   ```

9. **Quit Wavelab**.

## Result

| Quirk flag                                   | Row | Observed | Notes |
|----------------------------------------------|-----|----------|-------|
| `wavelab_vst3_defer_activation`              | #10 | <C/R/NT> | <step 5 ordering pattern> |
| `wavelab_state_blob_fallback`                | #11 | <C/R/NT> | <step 6 marker_ok value> |
| `tolerate_state_read_nontrue_status` (Lesson)| (LO)| <C/R/NT> | <step 6 bytes value vs 18> |
| `synthesize_bypass_parameter`                | #23 | <C/R/NT> |       |

**Log file(s)**: `<paste>`
**Wavelab version**: `<11.x.y>` — **OS**: `<x.y>` — **Date**: `<YYYY-MM-DD>`
