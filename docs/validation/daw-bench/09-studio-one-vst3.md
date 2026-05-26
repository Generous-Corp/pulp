# 09 — Studio One (VST3)

**Target quirks**:

- Studio One is not in the dense per-host catalog rows (1–22), but it is the
  primary target of the cross-format defaults — `synthesize_bypass_parameter`
  (#23), `clamp_latency_to_nonneg` (#24), `silence_unsupported_bus_arrangements`
  (#25) — and the bench plugin confirms those behaviors hold in PreSonus's
  host.

**Prereqs**: PreSonus Studio One 6.x, `PulpHostBench.vst3` installed and
Studio One has rescanned plugins (Studio One → Preferences → Locations →
VST Plug-Ins → Reset Blocklist & Rescan).

## Steps

1. **Clear stale logs**.

2. **Launch Studio One**, new empty Song, 48 kHz.

3. **Add an audio track**, insert PulpHostBench (drag from Browser → Effects).

4. **Confirm session log**:
   ```bash
   head -5 ~/Library/Logs/PulpHostBench/StudioOne-VST3-*.log
   # session_start  host=StudioOne  format=VST3
   ```

5. **Drag the Bench Gain knob** from 0 dB to +6 dB; touch it again to go back
   to 0 dB.

6. **Hit Play, Stop**.
   ```bash
   grep "transport_changed\|process_is_playing_edge" ~/Library/Logs/PulpHostBench/StudioOne-VST3-*.log
   ```

7. **Toggle the channel-strip bypass** (the channel-level bypass, not the
   plugin's own Bypass param).
   → catalog row **#23**: Studio One should successfully bypass the plugin
     regardless of whether the plugin declared its own bypass param. The
     bench plugin DOES declare one, so this is more about confirming no
     duplicate "Bypass" entries appear in the param list (which would
     indicate row #23 over-synthesizing).

8. **Try a multi-bus / sidechain configuration**: right-click the bench plugin
   header → Sidechain → pick another track's output as the sidechain input.
   ```bash
   grep "bus_layout_proposal\|sidechain_edge" ~/Library/Logs/PulpHostBench/StudioOne-VST3-*.log
   ```

9. **Save the Song, close, reopen**.
   ```bash
   grep -E "serialize_plugin_state|deserialize_plugin_state" \
       ~/Library/Logs/PulpHostBench/StudioOne-VST3-*.log
   ```

10. **Quit Studio One**.

## Result

| Quirk flag                                | Row | Observed | Notes |
|-------------------------------------------|-----|----------|-------|
| `synthesize_bypass_parameter`             | #23 | <C/R/NT> | <step 7> |
| `clamp_latency_to_nonneg`                 | #24 | <C/R/NT> | <bench reports latency=0; no negative> |
| `silence_unsupported_bus_arrangements`    | #25 | <C/R/NT> | <step 8> |
| (host detection)                          | —   | <C/R/NT> | <session_start host= ?> |

**Log file(s)**: `<paste>`
**Studio One version**: `<6.x.y>` — **OS**: `<x.y>` — **Date**: `<YYYY-MM-DD>`
