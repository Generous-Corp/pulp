-- ui_jank.sql: definitions for finding UI jank in a Pulp editor trace.
--
-- Definitions only: append ONE final SELECT and run with
--   trace_processor_shell -q <file> <trace.pftrace>
-- (Perfetto v57.2 refuses a -q file with more than one row-returning
-- statement). Walkthrough: references/ui_jank_playbook.md.
--
-- Idempotent (CREATE OR REPLACE PERFETTO), GLOB not LIKE, and incomplete
-- slices (dur = -1, still open at flush) are excluded from every duration.
-- Frame INTERVALS use slice start timestamps, so an incomplete last frame
-- still bounds the interval before it.
--
-- Slice names used, all emitted by Pulp core:
--   render/frame, canvas/paint, gpu/gpu_acquire         (window host)
--   js/frame_callback_pump, js/raf_flush                (widget bridge)
--   js/js_native + arg debug.fn                         (every JS->C++ call)
--   js/dom_event_dispatch, js/dom_event_evaluate        (DOM event delivery)
--   state/native_drag_dispatch, state/pointer_coalescer_flush (macOS host)

-- 0. Health: any row here condemns the capture. Run it first.
CREATE OR REPLACE PERFETTO VIEW jank_health AS
SELECT name, idx, severity, value
FROM stats
WHERE value > 0
  AND (severity = 'data_loss'
       OR name GLOB 'traced_buf_*wrap*'
       OR name GLOB 'traced_buf_*dropped*'
       OR name = 'packet_skipped_seq_needs_incremental_state_invalid');

-- Positive controls: counts that must be non-zero for a live-editor capture.
CREATE OR REPLACE PERFETTO VIEW jank_controls AS
SELECT
  (SELECT COUNT(*) FROM slice WHERE category = 'render' AND name = 'frame')                AS frames,
  (SELECT COUNT(*) FROM slice WHERE category = 'canvas' AND name = 'paint')                AS paints,
  (SELECT COUNT(*) FROM slice WHERE category = 'gpu'    AND name = 'gpu_acquire')          AS gpu_acquires,
  (SELECT COUNT(*) FROM slice WHERE category = 'js'     AND name = 'dom_event_dispatch')   AS dom_events,
  (SELECT COUNT(*) FROM slice WHERE category = 'state'  AND name = 'native_drag_dispatch') AS drag_events,
  (SELECT COUNT(*) FROM slice WHERE category = 'js'     AND name = 'js_native')            AS bridge_calls;

-- 1. The editor thread is the one that owns the most frame slices.
-- (thread.is_main_thread can be NULL in an in-process capture; do not rely on it.)
CREATE OR REPLACE PERFETTO TABLE jank_ui_thread AS
SELECT tt.utid AS utid, COUNT(*) AS frames
FROM slice s JOIN thread_track tt ON s.track_id = tt.id
WHERE s.category = 'render' AND s.name = 'frame'
GROUP BY tt.utid ORDER BY frames DESC LIMIT 1;

-- Interval between consecutive frame starts on that thread.
CREATE OR REPLACE PERFETTO TABLE jank_frame_gap AS
SELECT ts, next_ts, (next_ts - ts) / 1e6 AS gap_ms
FROM (
  SELECT s.ts, LEAD(s.ts) OVER (ORDER BY s.ts) AS next_ts
  FROM slice s JOIN thread_track tt ON s.track_id = tt.id
  WHERE s.category = 'render' AND s.name = 'frame'
    AND tt.utid = (SELECT utid FROM jank_ui_thread))
WHERE next_ts IS NOT NULL;

-- 2. Strokes: native_drag_dispatch clusters; a gap > 150 ms starts a new
-- stroke, and a cluster of <= 10 events is noise, not a gesture.
CREATE OR REPLACE PERFETTO TABLE jank_stroke AS
WITH d AS (
  SELECT ts, ts - LAG(ts) OVER (ORDER BY ts) AS since_prev
  FROM slice WHERE category = 'state' AND name = 'native_drag_dispatch'),
n AS (
  SELECT ts,
         SUM(CASE WHEN since_prev IS NULL OR since_prev > 150000000 THEN 1 ELSE 0 END)
           OVER (ORDER BY ts ROWS UNBOUNDED PRECEDING) AS stroke_id
  FROM d)
SELECT stroke_id, MIN(ts) AS start_ts, MAX(ts) AS end_ts, COUNT(*) AS events
FROM n GROUP BY stroke_id HAVING COUNT(*) > 10;

-- Phases. Windows of one phase may overlap (a stroke starting < 2 s after the
-- previous release); jank_phase_gap de-duplicates so no interval counts twice.
--   warmup        first 8 s of frames: excluded from every verdict
--   mid_stroke    stroke start + 300 ms .. last drag event
--   after_release last drag event .. + 2 s (deferred commits land here)
--   steady        3 s after the last stroke (or after warmup) .. end
--   all           everything after warmup (catches gaps no named phase owns)
CREATE OR REPLACE PERFETTO TABLE jank_phase AS
SELECT 'warmup' AS phase,
       (SELECT MIN(ts) FROM jank_frame_gap) AS ts_start,
       (SELECT MIN(ts) FROM jank_frame_gap) + 8000000000 AS ts_end
UNION ALL
SELECT 'mid_stroke', start_ts + 300000000, end_ts FROM jank_stroke
UNION ALL
SELECT 'after_release', end_ts, end_ts + 2000000000 FROM jank_stroke
UNION ALL
SELECT 'steady',
       COALESCE((SELECT MAX(end_ts) FROM jank_stroke) + 3000000000,
                (SELECT MIN(ts) FROM jank_frame_gap) + 8000000000),
       (SELECT MAX(next_ts) FROM jank_frame_gap)
UNION ALL
SELECT 'all',
       (SELECT MIN(ts) FROM jank_frame_gap) + 8000000000,
       (SELECT MAX(next_ts) FROM jank_frame_gap);

CREATE OR REPLACE PERFETTO TABLE jank_phase_gap AS
SELECT DISTINCT p.phase, g.ts, g.next_ts, g.gap_ms
FROM jank_frame_gap g
JOIN jank_phase p ON g.next_ts > p.ts_start AND g.next_ts <= p.ts_end;

-- 3. Frame-interval statistics per phase: never a single whole-run mean.
CREATE OR REPLACE PERFETTO VIEW jank_phase_stats AS
WITH r AS (
  SELECT phase, gap_ms,
         ROW_NUMBER() OVER (PARTITION BY phase ORDER BY gap_ms) AS rn,
         COUNT(*)     OVER (PARTITION BY phase)                 AS n
  FROM jank_phase_gap)
SELECT phase,
       MAX(n)                                                                  AS n,
       ROUND(1000.0 / AVG(gap_ms), 1)                                          AS fps,
       ROUND(AVG(gap_ms), 1)                                                   AS avg_ms,
       ROUND(MAX(CASE WHEN rn = CAST(n * 0.50 AS INT) + 1 THEN gap_ms END), 1) AS p50_ms,
       ROUND(MAX(CASE WHEN rn = MIN(n, CAST(n * 0.95 AS INT) + 1) THEN gap_ms END), 1) AS p95_ms,
       ROUND(MAX(gap_ms), 1)                                                   AS max_ms,
       SUM(gap_ms > 50)                                                        AS over_50ms,
       SUM(gap_ms > 100)                                                       AS over_100ms
FROM r GROUP BY phase;

-- 4. The worst frame intervals after warmup, each classified by the longest
-- top-level (depth <= 1) editor-thread slice overlapping it, the longest
-- JS->native call overlapping it (its debug.fn names the entry point), and
-- the longest gpu_acquire overlapping it. ui_busy_ms is the editor thread's
-- top-level slice time inside the gap: near gap_ms means the thread was busy
-- (a compute problem); far below it means the thread sat outside any slice
-- (waiting, or work that emits no span) -- follow the blocker instead.
CREATE OR REPLACE PERFETTO TABLE jank_worst_frames AS
WITH w AS (
  SELECT g.ts, g.next_ts, g.gap_ms
  FROM jank_frame_gap g
  WHERE g.ts >= (SELECT ts_end FROM jank_phase WHERE phase = 'warmup')
    AND g.gap_ms > 33
  ORDER BY g.gap_ms DESC LIMIT 15),
c AS (
  SELECT w.*,
    (SELECT s.id FROM slice s JOIN thread_track tt ON s.track_id = tt.id
      WHERE tt.utid = (SELECT utid FROM jank_ui_thread)
        AND s.dur >= 0 AND s.depth <= 1
        AND s.name NOT IN ('frame', 'frame_callback_pump')
        AND s.ts < w.next_ts AND s.ts + s.dur > w.ts
      ORDER BY s.dur DESC LIMIT 1) AS culprit_id,
    (SELECT s.id FROM slice s
      WHERE s.category = 'js' AND s.name = 'js_native' AND s.dur >= 0
        AND s.ts < w.next_ts AND s.ts + s.dur > w.ts
      ORDER BY s.dur DESC LIMIT 1) AS native_id,
    (SELECT MAX(s.dur) / 1e6 FROM slice s
      WHERE s.category = 'gpu' AND s.name = 'gpu_acquire' AND s.dur >= 0
        AND s.ts < w.next_ts AND s.ts + s.dur > w.ts) AS gpu_acquire_ms,
    (SELECT SUM(MIN(s.ts + s.dur, w.next_ts) - MAX(s.ts, w.ts)) / 1e6
       FROM slice s JOIN thread_track tt ON s.track_id = tt.id
      WHERE tt.utid = (SELECT utid FROM jank_ui_thread)
        AND s.depth = 0 AND s.dur >= 0
        AND s.ts < w.next_ts AND s.ts + s.dur > w.ts) AS busy_ms
  FROM w)
SELECT ROUND(c.gap_ms, 1)                     AS gap_ms,
       ROUND(c.busy_ms, 1)                    AS ui_busy_ms,
       ROUND((c.ts - trace_start()) / 1e9, 2) AS at_s,
       COALESCE((SELECT p.phase FROM jank_phase_gap p
                  WHERE p.ts = c.ts AND p.phase NOT IN ('warmup', 'all')
                  ORDER BY p.phase LIMIT 1), 'between_phases') AS phase,
       cs.name                                AS culprit,
       ROUND(cs.dur / 1e6, 1)                 AS culprit_ms,
       EXTRACT_ARG(cs.arg_set_id, 'debug.fn') AS culprit_fn,
       EXTRACT_ARG(ns.arg_set_id, 'debug.fn') AS longest_native_fn,
       ROUND(ns.dur / 1e6, 1)                 AS longest_native_ms,
       ROUND(c.gpu_acquire_ms, 1)             AS gpu_acquire_ms
FROM c
LEFT JOIN slice cs ON cs.id = c.culprit_id
LEFT JOIN slice ns ON ns.id = c.native_id
ORDER BY c.gap_ms DESC;

-- 5. Editor-thread top-level cost per phase, in ms of wall time per second
-- of phase. Answers "what is the main thread doing while it feels bad".
CREATE OR REPLACE PERFETTO TABLE jank_phase_span AS
SELECT phase, SUM(ts_end - ts_start) / 1e9 AS seconds FROM jank_phase GROUP BY phase;

CREATE OR REPLACE PERFETTO VIEW jank_ui_breakdown AS
WITH top AS (
  SELECT DISTINCT p.phase, s.id, s.name, s.dur
  FROM slice s
  JOIN thread_track tt ON s.track_id = tt.id
  JOIN jank_phase p ON s.ts >= p.ts_start AND s.ts < p.ts_end
  WHERE tt.utid = (SELECT utid FROM jank_ui_thread)
    AND s.depth = 0 AND s.dur >= 0)
SELECT top.phase, top.name, COUNT(*) AS n,
       ROUND(SUM(top.dur) / 1e6 / ps.seconds, 1) AS ms_per_s,
       ROUND(MAX(top.dur) / 1e6, 1)              AS max_ms
FROM top JOIN jank_phase_span ps USING (phase)
GROUP BY top.phase, top.name;

-- 6. Content cadence: how often a named data-delivery slice fires. The frame
-- rate can be a steady 60 Hz while the content it draws updates at 23 Hz.
CREATE OR REPLACE PERFETTO FUNCTION jank_cadence(slice_glob STRING)
RETURNS TABLE(name STRING, n LONG, per_s DOUBLE, max_gap_ms DOUBLE, gaps_over_150ms LONG) AS
WITH e AS (
  SELECT name, ts, ts - LAG(ts) OVER (PARTITION BY name ORDER BY ts) AS since_prev
  FROM slice
  WHERE name GLOB $slice_glob
    AND ts >= (SELECT ts_end FROM jank_phase WHERE phase = 'warmup'))
SELECT name, COUNT(*) AS n,
       ROUND(COUNT(*) * 1e9 / NULLIF(MAX(ts) - MIN(ts), 0), 1) AS per_s,
       ROUND(MAX(since_prev) / 1e6, 1)                         AS max_gap_ms,
       SUM(since_prev > 150000000)                             AS gaps_over_150ms
FROM e GROUP BY name;
