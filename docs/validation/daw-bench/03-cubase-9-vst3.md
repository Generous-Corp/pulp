# 03 — Cubase 9 (VST3)

**Target quirks**:

- `cubase9_state_blob_size_validation` (catalog row #4) — Cubase 9 stream-size
  bug where `IBStream::read` reports the wrong remaining-byte count.

> Cubase 9 is a legacy benchmark. Skip this script unless you have a working
> Cubase 9 (Pro 9.0.40 was the last 9.x release). The bench plugin works
> identically; the value is in observing the state-blob path under the
> older host.

**Prereqs**: Cubase 9 Pro, `PulpHostBench.vst3` installed.

## Steps

1. **Clear stale logs**.

2. **Launch Cubase 9**, new empty project, 48 kHz.

3. **Add an audio track**, insert PulpHostBench.

4. **Confirm session start**:
   ```bash
   head -5 ~/Library/Logs/PulpHostBench/Cubase-VST3-*.log
   # session_start  host=Cubase
   ```
   (Cubase 9 reports as `Cubase` — same detection as Cubase 12. The bench
   script's value is in the per-step *behavior* differences, not the host
   tag.)

5. **Wiggle the Bench Gain knob** (turn it from 0 dB to +12 dB).

6. **Save the project as a .cpr file**.

7. **Close the project**. Quit Cubase. Relaunch. Reopen the .cpr.
   ```bash
   ls -lt ~/Library/Logs/PulpHostBench/ | head -5
   # A NEW LogicPro/Cubase log file should appear — the relaunch creates
   # a fresh PID, so a fresh log.

   tail -20 ~/Library/Logs/PulpHostBench/Cubase-VST3-*.log
   # Look for: deserialize_plugin_state  bytes=18  marker_ok=true
   ```
   → catalog row **#4**: if the deserialize log line shows `bytes=18` and
     `marker_ok=true`, Cubase 9 correctly conveyed the blob. If `bytes` is
     less than 18 or `marker_ok=false`, the host truncated the blob, which
     is exactly the bug the quirk flag exists to handle.

8. **In the reopened project**, the Bench Gain knob should still read +12 dB.
   If it's back to 0 dB, the state didn't round-trip — confirming the
   `cubase9_state_blob_size_validation` quirk is needed for this version.

9. **Quit Cubase** to flush the log.

## Result

| Quirk flag                            | Row | Observed | Notes |
|---------------------------------------|-----|----------|-------|
| `cubase9_state_blob_size_validation`  | #4  | <C/R/NT> | <e.g. "deserialize bytes=18, knob restored OK"> |
| Cross-format defaults (#23–#25)       | —   | <C/R/NT> |       |

**Log file(s)**: `<paste>`
**Cubase version**: `Pro 9.0.x` — **OS**: `<macOS x.y / Win x.y>` — **Date**: `<YYYY-MM-DD>`
