# 11 — FL Studio (VST3)

**Target quirks**:

- `fl_studio_setactive_process_mutex` (row #13) — FL's Patcher calls
  `process()` before `setActive(true)` and concurrently with
  `setBusArrangements()`.
- `fl_studio_state_reader_skip` (row #14) — FL's state blob layout differs
  from VST3 reference; strict `MemoryStream` reader fails.

**Prereqs**: Image-Line FL Studio 21+, `PulpHostBench.vst3` installed and
FL has rescanned (Options → Manage Plugins → Find Plugins).

## Steps

1. **Clear stale logs**.

2. **Launch FL Studio**, new empty project, 48 kHz.

3. **Open the Mixer**, select Insert 1, click the "+" slot →
   PulpHostBench from the plugin list. Click "Add to insert".

4. **Confirm session log**:
   ```bash
   head -5 ~/Library/Logs/PulpHostBench/FLStudio-VST3-*.log
   # session_start  host=FLStudio  format=VST3
   ```

5. **Open the Patcher** (Channels → Add One → Patcher). Drop PulpHostBench
   into the Patcher.
   ```bash
   grep -E "process_without_prepare|bus_layout_proposal|prepare" \
       ~/Library/Logs/PulpHostBench/FLStudio-VST3-*.log | head -20
   ```
   → catalog row **#13**: if any `process_without_prepare` events appear
     in the log, FL's Patcher is the prime suspect — it is documented as
     calling `process()` before `setActive(true)`. The bench plugin
     captures this as a first-class log line specifically because of
     this row.

6. **Hit Play, Stop, repeatedly toggle the Patcher's bypass**.
   ```bash
   grep "process_without_prepare\|process_sample_rate_drift\|process_buffer_overrun" \
       ~/Library/Logs/PulpHostBench/FLStudio-VST3-*.log
   ```

7. **Change the project sample rate** (Options → Audio Settings → Sample Rate
   → e.g. 96000). Hit Play again.
   ```bash
   grep "prepare\|process_sample_rate_drift" ~/Library/Logs/PulpHostBench/FLStudio-VST3-*.log
   ```

8. **Save the .flp project, close, reopen**.
   ```bash
   grep -E "serialize_plugin_state|deserialize_plugin_state" \
       ~/Library/Logs/PulpHostBench/FLStudio-VST3-*.log
   ```
   → catalog row **#14**: `marker_ok=true` on the deserialize line means
     FL conveyed the blob correctly via its own reader path. If
     `marker_ok=false`, the `fl_studio_state_reader_skip` quirk is needed.

9. **Quit FL Studio**.

## Result

| Quirk flag                                | Row | Observed | Notes |
|-------------------------------------------|-----|----------|-------|
| `fl_studio_setactive_process_mutex`       | #13 | <C/R/NT> | <count of process_without_prepare events> |
| `fl_studio_state_reader_skip`             | #14 | <C/R/NT> | <step 8 marker_ok value> |
| Cross-format defaults (#23/#24/#25)       | —   | <C/R/NT> |       |

**Log file(s)**: `<paste>`
**FL Studio version**: `<21.x.y>` — **OS**: `<x.y>` — **Date**: `<YYYY-MM-DD>`
