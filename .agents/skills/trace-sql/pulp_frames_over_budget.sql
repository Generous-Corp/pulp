-- pulp_frames_over_budget — frames that missed the vsync budget.
--
-- Backs the `pulp trace query --preset frames-over-budget` lane. One row per
-- frame span (category `render`, name `frame`) whose duration exceeded the
-- frame budget, worst overrun first.
--
-- The budget is expressed as a PerfettoSQL function so the caller can pass the
-- real refresh interval (a 60 Hz surface = 16.667 ms; 120 Hz = 8.333 ms). The
-- view defaults to the 60 Hz budget; query the function directly for other
-- refresh rates:
--   SELECT * FROM pulp_frames_over_budget_ms(8.333);
--
-- Incomplete slices (dur = -1) are excluded.
--
-- Idempotent.

CREATE OR REPLACE PERFETTO FUNCTION pulp_frames_over_budget_ms(budget_ms DOUBLE)
RETURNS TABLE(
  slice_id LONG,
  ts LONG,
  dur_ms DOUBLE,
  over_ms DOUBLE,
  thread_name STRING
) AS
SELECT
  s.id,
  s.ts,
  s.dur / 1e6                  AS dur_ms,
  (s.dur / 1e6) - $budget_ms   AS over_ms,
  thread.name                  AS thread_name
FROM slice AS s
JOIN thread_track AS tt ON s.track_id = tt.id
JOIN thread            ON thread.utid = tt.utid
WHERE s.category = 'render'
  AND s.name = 'frame'
  AND s.dur >= 0
  AND (s.dur / 1e6) > $budget_ms
ORDER BY over_ms DESC;

CREATE OR REPLACE PERFETTO VIEW pulp_frames_over_budget AS
SELECT * FROM pulp_frames_over_budget_ms(16.667);
