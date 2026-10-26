---
breaking = false
summary = "SignalGraph times nodes only once someone reads per-node load: the first node_loads() turns monitoring on, so that first reading is stale"
---

## What changed

- A `SignalGraph` no longer times every node on every block from the start.
  Per-node timing costs two clock reads per node per block, which was about a
  quarter of a small graph's per-block cost on Apple Silicon. Nodes are timed
  once someone reads their load:
  - The first `node_loads()` call turns per-node monitoring on, and it stays on.
    That call returns the measurers' current values: zero for a graph that was
    never monitored, or the values from when monitoring last stopped. From the
    next block on, `callback_count` and the loads advance as before.
  - `set_node_load_monitoring(true)` starts timing without waiting for a first
    read. `set_node_load_monitoring(false)` stops it until the next
    `node_loads()`.
  - Enabling live-DSP telemetry (`set_live_dsp_telemetry_enabled(true)`) times
    the nodes too, because its record reads the same measurers.
- `graph_load()` is unchanged. Whole-graph timing always runs, because the
  live-swap admission gate reads it.
- A `GraphRuntimeExecutor` used directly still times nodes by default. Its new
  `set_node_load_timing(false)` turns that off.

## Migration steps

- None required. A caller that polls `node_loads()` keeps working; its first
  reading may be zero or stale.
- A caller that needs real values on its first read calls
  `set_node_load_monitoring(true)` before processing starts.
