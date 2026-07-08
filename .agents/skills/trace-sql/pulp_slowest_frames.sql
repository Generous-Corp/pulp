-- pulp_slowest_frames — frame spans ordered slowest-first.
--
-- Backs the L0 preset `pulp trace slowest-frames`. One row per completed
-- frame span (category `render`, name `frame`), with its duration in
-- milliseconds and the thread it ran on.
--
-- Incomplete slices (dur = -1, emitted by an unterminated PULP_TRACE_BEGIN or
-- a span still open when the ring was flushed) are excluded — a negative
-- duration is "not finished", not "instantaneous".
--
-- Idempotent: safe to re-run in the same trace_processor session.

CREATE OR REPLACE PERFETTO VIEW pulp_slowest_frames AS
SELECT
  s.id                         AS slice_id,
  s.ts                         AS ts,
  s.dur                        AS dur_ns,
  s.dur / 1e6                  AS dur_ms,
  thread.name                  AS thread_name,
  thread.tid                   AS tid,
  EXTRACT_ARG(s.arg_set_id, 'debug.frame_index') AS frame_index
FROM slice AS s
JOIN thread_track AS tt ON s.track_id = tt.id
JOIN thread            ON thread.utid = tt.utid
WHERE s.category = 'render'
  AND s.name = 'frame'
  AND s.dur >= 0
ORDER BY s.dur DESC;
