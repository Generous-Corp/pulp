-- GPU startup attribution over backend-neutral trace events. The closed view
-- selects the earliest render-frame lifecycle carrying frame_index = 0 (or the
-- single identified legacy lifecycle) and keeps both its cold/setup work and
-- later indexed steady-state work separate. Selection is accepted only when
-- every startup candidate has one exact, valid evidence ID.
--
-- `duration_ns` is wall-clock time. `cpu_running_ns` is populated only when
-- Perfetto thread_state intervals cover the complete slice on its stable utid;
-- partial or absent coverage remains NULL because it cannot distinguish
-- blocking from CPU work. Unindexed work is cold only when it begins before a
-- correlated indexed frame-zero completes; later unindexed work is unknown.
-- Incomplete slices are excluded here and detected across the whole trace by
-- the CLI's capture-integrity query, so they fail closed instead of ranking as
-- zero-duration work.
CREATE OR REPLACE PERFETTO VIEW pulp_gpu_startup_breakdown AS
WITH candidates AS (
  SELECT
    s.ts,
    s.name,
    s.dur,
    s.arg_set_id,
    s.track_id,
    CASE
      WHEN s.name GLOB 'gpu_shader_compile*' THEN 'shader-compile'
      WHEN s.name GLOB 'gpu_pipeline_prepare*' THEN 'pipeline-prepare'
      WHEN s.name GLOB 'gpu_resource_upload*' THEN 'resource-upload'
      WHEN s.name GLOB 'gpu_acquire*' THEN 'acquire'
      WHEN s.name GLOB 'gpu_submit*' THEN 'submit'
      WHEN s.name GLOB 'gpu_present*' THEN 'present'
      ELSE 'frame'
    END AS stage,
    COALESCE(
      CAST(EXTRACT_ARG(s.arg_set_id, 'debug.gpu_evidence_id') AS TEXT),
      CAST(EXTRACT_ARG(s.arg_set_id, 'args.debug.gpu_evidence_id') AS TEXT)) AS evidence_id,
    CAST(COALESCE(
      EXTRACT_ARG(s.arg_set_id, 'debug.frame_index'),
      EXTRACT_ARG(s.arg_set_id, 'args.debug.frame_index')) AS INT) AS frame_index
  FROM slice AS s
  WHERE s.dur >= 0
    AND ((s.category GLOB 'gpu*'
      AND (s.name GLOB 'gpu_shader_compile*'
        OR s.name GLOB 'gpu_pipeline_prepare*'
        OR s.name GLOB 'gpu_resource_upload*'
        OR s.name GLOB 'gpu_acquire*'
        OR s.name GLOB 'gpu_submit*'
        OR s.name GLOB 'gpu_present*'))
      OR (s.category GLOB 'render*' AND s.name GLOB 'frame*'))
), first_indexed_anchor AS (
  SELECT evidence_id
  FROM candidates
  WHERE stage = 'frame'
    AND frame_index = 0
  ORDER BY ts, evidence_id
  LIMIT 1
), first_indexed_lifecycle AS (
  SELECT evidence_id
  FROM first_indexed_anchor
  WHERE length(evidence_id) = 32
    AND evidence_id NOT GLOB '*[^0-9a-f]*'
), singleton_unindexed_lifecycle AS (
  SELECT MIN(evidence_id) AS evidence_id
  FROM candidates
  HAVING COUNT(*) = COUNT(evidence_id)
    AND COUNT(DISTINCT evidence_id) = 1
    AND MIN(length(evidence_id)) = 32
    AND MIN(evidence_id) NOT GLOB '*[^0-9a-f]*'
), all_candidates_identified AS (
  SELECT 1 AS valid
  WHERE NOT EXISTS (
    SELECT 1
    FROM candidates
    WHERE evidence_id IS NULL
      OR length(evidence_id) != 32
      OR evidence_id GLOB '*[^0-9a-f]*'
  )
), selected_lifecycle AS (
  SELECT evidence_id
  FROM first_indexed_lifecycle
  JOIN all_candidates_identified
  UNION ALL
  SELECT evidence_id
  FROM singleton_unindexed_lifecycle
  JOIN all_candidates_identified
  WHERE NOT EXISTS (SELECT 1 FROM first_indexed_anchor)
), selected_rows AS (
  SELECT
    c.*,
    tt.utid,
    th.upid,
    p.pid,
    (
      SELECT MAX(anchor.ts + anchor.dur)
      FROM candidates AS anchor
      WHERE anchor.evidence_id = c.evidence_id
        AND anchor.stage = 'frame'
        AND anchor.frame_index = 0
    ) AS cold_frame_end_ts
  FROM candidates AS c
  JOIN selected_lifecycle USING (evidence_id)
  LEFT JOIN thread_track AS tt ON c.track_id = tt.id
  LEFT JOIN thread AS th ON tt.utid = th.utid
  LEFT JOIN process AS p ON th.upid = p.upid
), attributed AS (
  SELECT
    selected_rows.*,
    (
      SELECT SUM(
        MIN(state.ts + state.dur, selected_rows.ts + selected_rows.dur)
        - MAX(state.ts, selected_rows.ts))
      FROM thread_state AS state
      WHERE state.utid = selected_rows.utid
        AND state.dur >= 0
        AND state.ts < selected_rows.ts + selected_rows.dur
        AND state.ts + state.dur > selected_rows.ts
    ) AS measured_state_coverage_ns,
    (
      SELECT SUM(
        MIN(state.ts + state.dur, selected_rows.ts + selected_rows.dur)
        - MAX(state.ts, selected_rows.ts))
      FROM thread_state AS state
      WHERE state.utid = selected_rows.utid
        AND state.state = 'Running'
        AND state.dur >= 0
        AND state.ts < selected_rows.ts + selected_rows.dur
        AND state.ts + state.dur > selected_rows.ts
    ) AS measured_cpu_running_ns
  FROM selected_rows
)
SELECT
  stage,
  dur AS duration_ns,
  evidence_id,
  upid AS process_upid,
  pid AS process_pid,
  COALESCE(
    CAST(EXTRACT_ARG(arg_set_id, 'debug.diagnostic_code') AS TEXT),
    CAST(EXTRACT_ARG(arg_set_id, 'args.debug.diagnostic_code') AS TEXT)) AS diagnostic_code,
  COALESCE(
    CAST(EXTRACT_ARG(arg_set_id, 'debug.health_state') AS TEXT),
    CAST(EXTRACT_ARG(arg_set_id, 'args.debug.health_state') AS TEXT)) AS health_state,
  CAST(COALESCE(
    EXTRACT_ARG(arg_set_id, 'debug.sequence'),
    EXTRACT_ARG(arg_set_id, 'args.debug.sequence')) AS INT) AS sequence,
  frame_index,
  CASE
    WHEN frame_index = 0 THEN 'cold'
    WHEN frame_index > 0 THEN 'steady'
    WHEN frame_index IS NULL
      AND cold_frame_end_ts IS NOT NULL
      AND ts < cold_frame_end_ts THEN 'cold'
    ELSE 'unknown'
  END AS timing_phase,
  CASE
    WHEN measured_state_coverage_ns = dur THEN COALESCE(measured_cpu_running_ns, 0)
    ELSE NULL
  END AS cpu_running_ns,
  COALESCE(measured_state_coverage_ns = dur, 0) AS has_scheduler_evidence,
  0 AS is_incomplete,
  COALESCE(
    CAST(EXTRACT_ARG(arg_set_id, 'debug.health_state') AS TEXT),
    CAST(EXTRACT_ARG(arg_set_id, 'args.debug.health_state') AS TEXT), '')
    IN ('failed', 'lost') AS is_failure
FROM attributed;
