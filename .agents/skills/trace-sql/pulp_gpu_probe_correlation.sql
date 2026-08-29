-- Correlate bounded numeric-probe work by its stable evidence identifier. The
-- closed view returns rows only when every candidate has the same exact, valid
-- evidence ID; mixed or uncorrelated traces fail closed as an empty result.
-- Incomplete rows are excluded here and detected across the whole trace by the
-- CLI capture-integrity query. Filesystem paths, shader source, and adapter
-- marketing strings are excluded.
CREATE OR REPLACE PERFETTO VIEW pulp_gpu_probe_correlation AS
WITH candidates AS (
  SELECT
    s.name,
    s.dur,
    s.arg_set_id,
    th.upid AS process_upid,
    p.pid AS process_pid,
    COALESCE(
      CAST(EXTRACT_ARG(s.arg_set_id, 'debug.gpu_evidence_id') AS TEXT),
      CAST(EXTRACT_ARG(s.arg_set_id, 'args.debug.gpu_evidence_id') AS TEXT)) AS evidence_id
  FROM slice AS s
  JOIN thread_track AS tt ON s.track_id = tt.id
  JOIN thread AS th ON tt.utid = th.utid
  JOIN process AS p ON th.upid = p.upid
  WHERE s.category GLOB 'gpu*'
    AND (s.name GLOB 'gpu_probe*' OR s.name GLOB 'gpu_readback*' OR s.name GLOB 'gpu_submit*')
), selected_evidence AS (
  SELECT MIN(evidence_id) AS evidence_id
  FROM candidates
  HAVING COUNT(*) = COUNT(evidence_id)
    AND COUNT(DISTINCT evidence_id) = 1
    AND MIN(length(evidence_id)) = 32
    AND MIN(evidence_id) NOT GLOB '*[^0-9a-f]*'
)
SELECT
  CASE
    WHEN name GLOB 'gpu_readback*' THEN 'readback'
    WHEN name GLOB 'gpu_submit*' THEN 'submit'
    ELSE 'probe'
  END AS stage,
  dur AS duration_ns,
  evidence_id,
  process_upid,
  process_pid,
  COALESCE(
    CAST(EXTRACT_ARG(arg_set_id, 'debug.diagnostic_code') AS TEXT),
    CAST(EXTRACT_ARG(arg_set_id, 'args.debug.diagnostic_code') AS TEXT)) AS diagnostic_code,
  COALESCE(
    CAST(EXTRACT_ARG(arg_set_id, 'debug.health_state') AS TEXT),
    CAST(EXTRACT_ARG(arg_set_id, 'args.debug.health_state') AS TEXT)) AS health_state,
  CAST(COALESCE(
    EXTRACT_ARG(arg_set_id, 'debug.sequence'),
    EXTRACT_ARG(arg_set_id, 'args.debug.sequence')) AS INT) AS sequence,
  CAST(COALESCE(
    EXTRACT_ARG(arg_set_id, 'debug.frame_index'),
    EXTRACT_ARG(arg_set_id, 'args.debug.frame_index')) AS INT) AS frame_index,
  'not-applicable' AS timing_phase,
  NULL AS cpu_running_ns,
  0 AS has_scheduler_evidence,
  0 AS is_incomplete,
  (COALESCE(
     CAST(EXTRACT_ARG(arg_set_id, 'debug.health_state') AS TEXT),
     CAST(EXTRACT_ARG(arg_set_id, 'args.debug.health_state') AS TEXT), '')
     IN ('failed', 'lost')
   OR COALESCE(
     CAST(EXTRACT_ARG(arg_set_id, 'debug.diagnostic_code') AS TEXT),
     CAST(EXTRACT_ARG(arg_set_id, 'args.debug.diagnostic_code') AS TEXT), '')
     IN ('cpu_oracle_mismatch', 'magnitude_dispatch_failed')) AS is_failure
FROM candidates
JOIN selected_evidence USING (evidence_id)
WHERE dur >= 0;
