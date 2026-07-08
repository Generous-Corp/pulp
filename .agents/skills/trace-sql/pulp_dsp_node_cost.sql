-- pulp_dsp_node_cost — per-node DSP cost, aggregated across all blocks.
--
-- Backs the L0 preset `pulp trace dsp-hotspots`. One row per named graph node
-- (category `dsp.node`, name = the node's stable name interned at graph-build
-- time), aggregating every block in which that node ran. This is the view that
-- exposes what a single load-meter scalar hides: a calm mean with one node
-- eating most of every block (see docs/guides/tracing.md use case 3).
--
-- `max_ms` next to `mean_ms` is the tell — a node whose max ≫ mean is the
-- per-block spiker the average is masking. SQLite has no percentile builtin;
-- the trace-analysis skill's p95/p99 discipline computes tail latency from the
-- raw `slice` rows when the mean-vs-max gap is ambiguous.
--
-- Sourced from the offline_process() path (deterministic, RT-hazard-free);
-- Perfetto is never placed on the live audio thread (see trace.hpp).
--
-- Idempotent.

CREATE OR REPLACE PERFETTO VIEW pulp_dsp_node_cost AS
SELECT
  s.name                       AS node_name,
  COUNT(*)                     AS block_count,
  SUM(s.dur) / 1e6             AS total_ms,
  AVG(s.dur) / 1e6             AS mean_ms,
  MAX(s.dur) / 1e6             AS max_ms,
  MIN(s.dur) / 1e6             AS min_ms
FROM slice AS s
WHERE s.category = 'dsp.node'
  AND s.dur >= 0
GROUP BY s.name
ORDER BY total_ms DESC;
