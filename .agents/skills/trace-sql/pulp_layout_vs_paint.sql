-- pulp_layout_vs_paint — frame-pipeline cost split, one row per category.
--
-- Backs the L0 preset `pulp trace layout-vs-paint`. Aggregates the wall time
-- spent in each frame-pipeline stage across the whole capture, so a hitch can
-- be attributed to layout vs. text shaping vs. paint vs. GPU submit at a
-- glance. This is the "where did the frame budget go?" summary that turns a
-- flamegraph into a one-screen answer.
--
-- Stages, by Pulp category:
--   layout  — Yoga layout pass
--   text    — TextShaper::prepare / layout (the PreText cheap-vs-expensive split)
--   canvas  — 2D drawing / dirty-walk
--   render  — paint + present bookkeeping
--   gpu     — Dawn submit / Graphite record / per-pass GPU time
--
-- `self_ms` is the sum of slice durations in that category; nested child
-- slices are counted in their own category rows, so the column is a per-stage
-- total, not an exclusive self-time. Incomplete slices (dur = -1) are excluded.
--
-- Idempotent.

CREATE OR REPLACE PERFETTO VIEW pulp_layout_vs_paint AS
SELECT
  s.category                   AS stage,
  COUNT(*)                     AS slice_count,
  SUM(s.dur) / 1e6             AS total_ms,
  AVG(s.dur) / 1e6             AS mean_ms,
  MAX(s.dur) / 1e6             AS max_ms
FROM slice AS s
WHERE s.category IN ('layout', 'text', 'canvas', 'render', 'gpu')
  AND s.dur >= 0
GROUP BY s.category
ORDER BY total_ms DESC;
