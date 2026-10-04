-- pulp_frame_stage_cost — one row per frame span, its cost split by stage.
--
-- Backs tools/scripts/trace_frame_cost.py (which adds p50/p95/max, scenario
-- grouping and budgets). For each slice named `frame_name` (a host's `frame`
-- or `plugin_editor_frame`, a fixture's own span), every slice nested inside
-- it on the same track is attributed to its category by SELF time -- its
-- duration minus its direct children's -- so a js span wrapping canvas work
-- is not counted twice. Also per frame:
--   repaint_requests  `view_repaint_request` slices: whole-surface repaint
--                     requests (a bounded request takes another path and
--                     emits none), so >0 means the frame dirtied everything
--   layout_passes     `layout_children` slices: Yoga passes the frame ran
--
--   SELECT * FROM pulp_frame_stage_cost('plugin_editor_frame');
--
-- Incomplete slices (dur = -1) are excluded. Idempotent.

CREATE OR REPLACE PERFETTO FUNCTION pulp_frame_stage_cost(frame_name STRING)
RETURNS TABLE(
  frame_id LONG,
  ts LONG,
  frame_ms DOUBLE,
  layout_ms DOUBLE,
  canvas_ms DOUBLE,
  js_ms DOUBLE,
  text_ms DOUBLE,
  state_ms DOUBLE,
  render_ms DOUBLE,
  repaint_requests LONG,
  layout_passes LONG
) AS
WITH frames AS (
  SELECT id, ts, dur, track_id, depth FROM slice
  WHERE name = $frame_name AND dur >= 0
), child_sum AS (
  SELECT parent_id, SUM(dur) AS dur FROM slice
  WHERE dur >= 0 AND parent_id IS NOT NULL GROUP BY parent_id
), inner_slices AS (
  SELECT f.id AS frame_id, s.category, s.name,
         s.dur - COALESCE(cs.dur, 0) AS self_dur
  FROM frames f
  JOIN slice s ON s.track_id = f.track_id AND s.depth > f.depth
              AND s.ts >= f.ts AND s.ts + s.dur <= f.ts + f.dur
  LEFT JOIN child_sum cs ON cs.parent_id = s.id
  WHERE s.dur >= 0
)
SELECT
  f.id AS frame_id, f.ts AS ts, f.dur / 1e6 AS frame_ms,
  COALESCE(SUM(CASE WHEN i.category = 'layout' THEN i.self_dur END), 0) / 1e6 AS layout_ms,
  COALESCE(SUM(CASE WHEN i.category = 'canvas' THEN i.self_dur END), 0) / 1e6 AS canvas_ms,
  COALESCE(SUM(CASE WHEN i.category = 'js' THEN i.self_dur END), 0) / 1e6 AS js_ms,
  COALESCE(SUM(CASE WHEN i.category = 'text' THEN i.self_dur END), 0) / 1e6 AS text_ms,
  COALESCE(SUM(CASE WHEN i.category = 'state' THEN i.self_dur END), 0) / 1e6 AS state_ms,
  COALESCE(SUM(CASE WHEN i.category = 'render' THEN i.self_dur END), 0) / 1e6 AS render_ms,
  COALESCE(SUM(CASE WHEN i.name = 'view_repaint_request' THEN 1 END), 0) AS repaint_requests,
  COALESCE(SUM(CASE WHEN i.name = 'layout_children' THEN 1 END), 0) AS layout_passes
FROM frames f LEFT JOIN inner_slices i ON i.frame_id = f.id
GROUP BY f.id
ORDER BY f.ts;
