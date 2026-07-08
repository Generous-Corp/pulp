# DSP hints — `dsp` / `dsp.node` spans

Domain knowledge for investigating audio-processing cost in a Pulp trace.
Ground every claim in a query; this file tells you *what to look for* and
*which trap you are probably in*.

## First, the scope rule (read this before anything else)

Perfetto is **never on the live audio thread**. D1 proved Perfetto's
`TRACE_EVENT` locks a mutex on chunk rollover, which is not real-time-safe
(`core/runtime/include/pulp/runtime/trace.hpp` states this). So:

- **`dsp` / `dsp.node` spans come from the OFFLINE render path**
  (`offline_process()` in `core/audio/include/pulp/audio/offline_processor.hpp`),
  not from a live in-DAW `process()`. This is a feature: offline has no
  deadline and no under-load buffer drop, so the capture is **deterministic and
  reproducible** — the right substrate for a docs/CI answer.
- **Live, in-DAW DSP observability** is Pulp's own fixed-slot per-node
  telemetry (a `TripleBuffer` drained off-thread, surfaced like
  `AudioProcessLoadMeasurer`), **not** Perfetto. If the question is "why does it
  stutter *live* in Logic under load", Perfetto is the wrong instrument — say
  so and point at the load-measurer / telemetry path.

## The headline trap: "the load meter said 40% but one node dominates"

`AudioProcessLoadMeasurer` produces a single EMA scalar — one number for the
whole graph. It **cannot** say which node, which block, or why. A calm ~40%
mean routinely hides one node eating ~60% of every block (a per-voice
oversampler, an over-large FFT convolver on one layer). This is the canonical
`examples/trace-demo` reveal.

Query `pulp_dsp_node_cost` and read **`max_ms` next to `mean_ms`**:

- A node whose `total_ms` dominates is the flat hotspot.
- A node whose `max_ms` ≫ `mean_ms` is the **per-block spiker the average hid** —
  this is the p95/p99 discipline in action. The mean is calm; the tail is not.

Then confirm on the timeline: in the flamegraph the guilty node is one **fat
span** per block while its siblings are slivers.

## Per-node cost and graph topology

Because Pulp owns the signal graph (`core/host/include/pulp/host/signal_graph.hpp`),
`dsp.node` span names are the graph's **stable node names** (interned at
graph-build time — a non-RT operation). That lets you reason about *structure*,
not just flat spans:

- **Critical path** — sum cost along the serial chain; the longest path bounds
  the block.
- **Per-voice vs per-bus** — a cost that scales with polyphony (per-voice) reads
  very differently from a fixed per-bus cost. Group by node name and correlate
  with voice count.
- **"These could parallelize"** — siblings with no data dependency that each eat
  a chunk of the block are the §6.1 parallel-graph candidates. Per-node timing
  is exactly the input that work needs.

## xruns / deadline-miss attribution

`pulp_xruns` surfaces xrun / deadline-miss **instant events** (zero-duration
marks, keyed by name `GLOB 'xrun*'`/`'deadline_miss*'`, not by `dur`), carrying
`block_index` and `load_fraction` args where the emit site set them. Correlate
a miss's `block_index` with the `dsp.node` spans on that block to attribute the
overrun to a node. (These come from the offline/analysis path — live xruns are
the telemetry fallback's job, per the scope rule above.)

## Jitter and denormals

- **Jitter** is a distribution question: `SELECT dur FROM slice WHERE
  category='dsp' AND name='process' AND dur>=0` gives the per-block duration
  histogram. A wide spread (not a fat mean) is jitter — a node whose cost
  depends on signal content or branch behavior.
- **Denormals** show up as a block whose cost balloons when a decaying tail
  reaches subnormal range (reverb/filter tails). Look for a `dsp.node` whose
  `max_ms` spikes on the *quiet* blocks after a note-off, not the loud ones —
  counterintuitive, and a strong denormal tell. The fix is flush-to-zero, not
  optimization.

## Common queries

```sql
-- Per-node ranking (the hotspot list):
SELECT * FROM pulp_dsp_node_cost;         -- load pulp_dsp_node_cost.sql first

-- Per-block duration spread (jitter):
SELECT dur/1e6 AS ms FROM slice
WHERE category='dsp' AND name='process' AND dur>=0 ORDER BY ms DESC;

-- Nodes active on the block that missed its deadline:
SELECT node.name, node.dur/1e6 AS ms
FROM slice node
WHERE node.category='dsp.node' AND node.dur>=0
  AND node.ts BETWEEN (SELECT ts FROM pulp_xruns LIMIT 1)
                  AND (SELECT ts FROM pulp_xruns LIMIT 1) + 1e6
ORDER BY ms DESC;
```

## Traps

- **Don't diagnose a live stutter from an offline trace.** Offline is
  deterministic *because* it has no deadline — it will never show the
  buffer-size-dependent dropout the user hears live. Reframe to the offline
  "which node is expensive" question, or point at the live telemetry path.
- **Mean is not max.** Never conclude "DSP is fine, 40%" from a mean. Always
  check the per-node `max_ms`.
- **`dsp` ≠ `dsp.node`.** `WHERE category='dsp'` is the per-block wrapper span;
  `'dsp.node'` is the per-node breakdown. Use `GLOB 'dsp*'` only to get both.
