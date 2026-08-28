-- GPU health state and device-loss evidence. The closed view returns rows only
-- when every candidate has the same exact, valid evidence ID; mixed or
-- uncorrelated traces fail closed as an empty result. Generic framework
-- producers may move to Vellum; these backend-neutral names and columns remain
-- the consumer contract.
CREATE OR REPLACE PERFETTO VIEW pulp_gpu_health_transitions AS
WITH candidates AS (
  SELECT
    name,
    dur,
    arg_set_id,
    COALESCE(
      CAST(EXTRACT_ARG(arg_set_id, 'debug.gpu_evidence_id') AS TEXT),
      CAST(EXTRACT_ARG(arg_set_id, 'args.debug.gpu_evidence_id') AS TEXT)) AS evidence_id
  FROM slice
  WHERE category GLOB 'gpu*'
    AND (name GLOB 'gpu_health_transition*' OR name GLOB 'gpu_device_loss*')
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
    WHEN name GLOB 'gpu_device_loss*' THEN 'device-loss'
    ELSE 'health-transition'
  END AS stage,
  CASE WHEN dur = -1 THEN 0 ELSE dur END AS duration_ns,
  evidence_id,
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
  dur = -1 AS is_incomplete,
  (name GLOB 'gpu_device_loss*'
    OR COALESCE(
         CAST(EXTRACT_ARG(arg_set_id, 'debug.health_state') AS TEXT),
         CAST(EXTRACT_ARG(arg_set_id, 'args.debug.health_state') AS TEXT), '')
       IN ('failed', 'lost'))
      AS is_failure
FROM candidates
JOIN selected_evidence USING (evidence_id);
