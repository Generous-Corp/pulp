-- pulp_motion_join — frames joined to the motion trace_id that drove them.
--
-- Backs the `pulp trace query --preset motion-join` lane and the §4 "what
-- changed × where the time went" correlation. When a motion trace is active
-- while tracing captures, frame-pipeline spans carry the motion `trace_id` as
-- a typed span argument; this view surfaces every frame that was attributed to
-- a motion trace, so a `CostSample` ("frame 412 = 9 ms, trace_id=knob-sweep")
-- leads straight to the flamegraph of that frame.
--
-- Frames with no motion trace active (arg absent → NULL) are dropped, so the
-- result is exactly the frames a gesture/animation is on the hook for.
--
-- Idempotent.

CREATE OR REPLACE PERFETTO VIEW pulp_motion_join AS
SELECT
  EXTRACT_ARG(s.arg_set_id, 'debug.motion.trace_id') AS motion_trace_id,
  s.id                         AS slice_id,
  s.ts                         AS ts,
  s.dur / 1e6                  AS dur_ms,
  EXTRACT_ARG(s.arg_set_id, 'debug.frame_index')     AS frame_index
FROM slice AS s
WHERE s.category = 'render'
  AND s.name = 'frame'
  AND s.dur >= 0
  AND EXTRACT_ARG(s.arg_set_id, 'debug.motion.trace_id') IS NOT NULL
ORDER BY dur_ms DESC;
