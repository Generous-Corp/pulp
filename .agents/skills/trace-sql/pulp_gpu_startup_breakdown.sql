-- GPU startup attribution over backend-neutral trace events. The closed view
-- selects the earliest render-frame lifecycle carrying frame_index = 0 and
-- excludes later frames and lifecycles. Selection is accepted only when every
-- startup candidate has one exact, valid evidence ID; otherwise an uncorrelated
-- long stage could be silently omitted from the ranking. The
-- definition remains idempotent: the CLI and an expert Perfetto session may
-- load it repeatedly against the same flushed trace.
CREATE OR REPLACE PERFETTO VIEW pulp_gpu_startup_breakdown AS
WITH candidates AS (
  SELECT
    ts,
    name,
    dur,
    arg_set_id,
    CASE
      WHEN name GLOB 'gpu_shader_compile*' THEN 'shader-compile'
      WHEN name GLOB 'gpu_pipeline_prepare*' THEN 'pipeline-prepare'
      WHEN name GLOB 'gpu_resource_upload*' THEN 'resource-upload'
      WHEN name GLOB 'gpu_acquire*' THEN 'acquire'
      WHEN name GLOB 'gpu_submit*' THEN 'submit'
      WHEN name GLOB 'gpu_present*' THEN 'present'
      ELSE 'frame'
    END AS stage,
    COALESCE(
      CAST(EXTRACT_ARG(arg_set_id, 'debug.gpu_evidence_id') AS TEXT),
      CAST(EXTRACT_ARG(arg_set_id, 'args.debug.gpu_evidence_id') AS TEXT)) AS evidence_id,
    CAST(COALESCE(
      EXTRACT_ARG(arg_set_id, 'debug.frame_index'),
      EXTRACT_ARG(arg_set_id, 'args.debug.frame_index')) AS INT) AS frame_index
  FROM slice
  WHERE (category GLOB 'gpu*'
      AND (name GLOB 'gpu_shader_compile*'
        OR name GLOB 'gpu_pipeline_prepare*'
        OR name GLOB 'gpu_resource_upload*'
        OR name GLOB 'gpu_acquire*'
        OR name GLOB 'gpu_submit*'
        OR name GLOB 'gpu_present*'))
    OR (category GLOB 'render*' AND name GLOB 'frame*')
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
)
SELECT
  stage,
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
  frame_index,
  dur = -1 AS is_incomplete,
  COALESCE(
    CAST(EXTRACT_ARG(arg_set_id, 'debug.health_state') AS TEXT),
    CAST(EXTRACT_ARG(arg_set_id, 'args.debug.health_state') AS TEXT), '')
    IN ('failed', 'lost') AS is_failure
FROM candidates
JOIN selected_lifecycle USING (evidence_id)
WHERE frame_index IS NULL OR frame_index = 0;
