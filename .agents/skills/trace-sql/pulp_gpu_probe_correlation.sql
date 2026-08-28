-- Correlate bounded numeric-probe work by its stable evidence identifier.
-- Filesystem paths, shader source, and adapter marketing strings are excluded.
CREATE OR REPLACE PERFETTO VIEW pulp_gpu_probe_correlation AS
SELECT
  CASE
    WHEN name GLOB 'gpu_readback*' THEN 'readback'
    WHEN name GLOB 'gpu_submit*' THEN 'submit'
    ELSE 'probe'
  END AS stage,
  CASE WHEN dur = -1 THEN 0 ELSE dur END AS duration_ns,
  COALESCE(
    CAST(EXTRACT_ARG(arg_set_id, 'debug.gpu_evidence_id') AS TEXT),
    CAST(EXTRACT_ARG(arg_set_id, 'args.debug.gpu_evidence_id') AS TEXT)) AS evidence_id,
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
    CAST(EXTRACT_ARG(arg_set_id, 'debug.diagnostic_code') AS TEXT),
    CAST(EXTRACT_ARG(arg_set_id, 'args.debug.diagnostic_code') AS TEXT), '') != '' AS is_failure
FROM slice
WHERE category GLOB 'gpu*'
  AND (name GLOB 'gpu_probe*' OR name GLOB 'gpu_readback*' OR name GLOB 'gpu_submit*')
  AND COALESCE(
    CAST(EXTRACT_ARG(arg_set_id, 'debug.gpu_evidence_id') AS TEXT),
    CAST(EXTRACT_ARG(arg_set_id, 'args.debug.gpu_evidence_id') AS TEXT)) IS NOT NULL;
