# 06 — Reaper (VST3)

**Target quirks** (Reaper has the widest catalog footprint — rows 15 + R1–R7):

- `reaper_vst3_gesture_ordering` (#15) — gesture begin/end ordering.
- `reaper_process_while_bypassed` (R1) — Reaper may call `process()` while bypassed.
- `reaper_keyboard_passthrough` (R2) — key events route through plugin first.
- `reaper_permissive_bus_arrangements` (R3) — Reaper accepts arbitrary pin wiring.
- `reaper_anticipative_fx_buffer_variability` (R4) — variable buffer sizes mid-block.
- `reaper_midsession_setstate` (R6) — `setState` called any time.
- `reaper_keyboard_only_space` — REAPER only delivers a well-formed keyMsg for Space.

**Prereqs**: Reaper 7.x, `PulpHostBench.vst3` installed and rescanned
(Reaper → Options → Preferences → Plug-ins → VST → Re-scan).

## Steps

1. **Clear stale logs**.

2. **Launch Reaper**, new empty project, 48 kHz, 32-bit float.

3. **Insert a track**, add PulpHostBench on the FX chain.

4. **Confirm session log**:
   ```bash
   head -5 ~/Library/Logs/PulpHostBench/Reaper-VST3-*.log
   # session_start  host=Reaper  format=VST3
   ```

5. **Bypass the plugin via the FX-chain bypass button**. Hit Play. Stop.
   Unbypass. Hit Play. Stop.
   ```bash
   grep "process_block_count\|suspend\|resume" ~/Library/Logs/PulpHostBench/Reaper-VST3-*.log
   # Also check the plugin UI shows process_block_count > 0 during bypass
   # (you'll need to peek at the bench's "Last event" string for a "process_*" event).
   ```
   → catalog row **R1**: if `process_*` events appear in the log even while
     bypassed, Reaper is honoring "Run FX while bypassed" — confirming the
     quirk. The bench plugin doesn't implement `process_bypassed()` (it
     doesn't exist yet — see catalog), so process() runs as normal.

6. **Right-click the track's FX-chain header** → Send all keyboard input to plugin.
   Open the plugin window. Press Space.
   ```bash
   grep "view_opened\|view_resized" ~/Library/Logs/PulpHostBench/Reaper-VST3-*.log
   # No direct key event — the bench plugin doesn't have a text widget, so
   # the key-routing observation is whether REAPER's transport (Space = play)
   # still works (it should — that's the desired behavior).
   ```
   → catalog row **R2**: if pressing Space starts Reaper's transport, REAPER
     correctly bubbles keys it doesn't see consumed. If it freezes / does
     nothing, the plugin is swallowing keys (which the bench doesn't).
   → catalog row `reaper_keyboard_only_space`: try pressing other keys
     (Tab, Esc, Return) — they should NOT trigger anything in the bench
     plugin (no text widget); record whether REAPER's own shortcuts still fire.

7. **Right-click the bench plugin's FX-chain entry** → Pin Connector. Connect
   stereo output → left only of the next plugin, or to a 5-channel send.
   ```bash
   grep "bus_layout_proposal" ~/Library/Logs/PulpHostBench/Reaper-VST3-*.log
   # Look for: outputs=N where N > 2 if you set up a wider send.
   ```
   → catalog row **R3**: Reaper may propose unusual channel counts. The bench
     plugin's `is_bus_layout_supported` is permissive (accepts 1–8), so as long
     as the count is in range the layout is accepted. If you see "Refused"
     visually in Reaper's FX UI for a layout you tried to wire, that's evidence
     the default-strict policy (rather than the permissive Reaper override) is
     active.

8. **Render the project to disk** (File → Render to File → set Sample Rate
   to 96 kHz, Format = WAV, render).
   ```bash
   grep "prepare\|process_sample_rate_drift\|process_buffer_overrun" \
       ~/Library/Logs/PulpHostBench/Reaper-VST3-*.log
   ```
   → catalog row **R4**: if a new `prepare` event appears with
     `sample_rate=96000` (different from the project's 48 kHz), Reaper is
     correctly re-preparing for the render. If only `process_sample_rate_drift`
     events appear (without a fresh prepare), the offline-render path is
     skipping prepare — that's the anticipative-FX quirk.

9. **In the FX chain, right-click the bench plugin → Save FX chain → save as
   `pulp-bench-test.RfxChain`. Delete the plugin from the track. Drag the
   .RfxChain back from the FX browser onto the track.**
   ```bash
   grep -E "serialize_plugin_state|deserialize_plugin_state" \
       ~/Library/Logs/PulpHostBench/Reaper-VST3-*.log
   ```
   → catalog row **R6**: Reaper just called `setState` on a freshly loaded
     plugin instance, in the middle of an active project. Confirms the
     mid-session-setState path works (and that the bench plugin doesn't
     crash on it).

10. **Save .rpp project**, close, reopen — confirm overall round-trip.

11. **Quit Reaper**.

## Result

| Quirk flag                                       | Row | Observed | Notes |
|--------------------------------------------------|-----|----------|-------|
| `reaper_vst3_gesture_ordering`                   | #15 | <C/R/NT> | <only fully visible via host-side automation lane> |
| `reaper_process_while_bypassed`                  | R1  | <C/R/NT> | <step 5> |
| `reaper_keyboard_passthrough`                    | R2  | <C/R/NT> | <step 6: Space starts transport?> |
| `reaper_keyboard_only_space` (Lesson)            | (LO)| <C/R/NT> | <step 6 other keys> |
| `reaper_permissive_bus_arrangements`             | R3  | <C/R/NT> | <step 7> |
| `reaper_anticipative_fx_buffer_variability`      | R4  | <C/R/NT> | <step 8> |
| `reaper_midsession_setstate`                     | R6  | <C/R/NT> | <step 9> |
| Cross-format defaults (#23/#24/#25)              | —   | <C/R/NT> |       |

**Log file(s)**: `<paste>`
**Reaper version**: `<7.x.y>` — **OS**: `<x.y>` — **Date**: `<YYYY-MM-DD>`
