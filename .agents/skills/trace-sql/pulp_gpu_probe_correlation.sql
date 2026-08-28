-- Correlate bounded numeric-probe work by its stable evidence identifier. The
-- closed view returns rows only when every candidate has the same exact, valid
-- evidence ID; mixed or uncorrelated traces fail closed as an empty result.
-- Filesystem paths, shader source, and adapter marketing strings are excluded.
CREATE OR REPLACE PERFETTO VIEW pulp_gpu_probe_correlation AS
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
    AND (name GLOB 'gpu_probe*' OR name GLOB 'gpu_readback*' OR name GLOB 'gpu_submit*')
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
  COALESCE(
    CAST(EXTRACT_ARG(arg_set_id, 'debug.health_state') AS TEXT),
    CAST(EXTRACT_ARG(arg_set_id, 'args.debug.health_state') AS TEXT), '')
    IN ('failed', 'lost') AS is_failure
FROM candidates
JOIN selected_evidence USING (evidence_id);
