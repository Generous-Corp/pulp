-- GPU startup attribution over backend-neutral trace events. This definition is
-- intentionally idempotent: the CLI and an expert Perfetto session may load it
-- repeatedly against the same flushed trace.
CREATE OR REPLACE PERFETTO VIEW pulp_gpu_startup_breakdown AS
WITH candidates AS (
  SELECT
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
    END AS stage
  FROM slice
  WHERE (category GLOB 'gpu*' OR category GLOB 'render*')
    AND (name GLOB 'gpu_shader_compile*'
      OR name GLOB 'gpu_pipeline_prepare*'
      OR name GLOB 'gpu_resource_upload*'
      OR name GLOB 'gpu_acquire*'
      OR name GLOB 'gpu_submit*'
      OR name GLOB 'gpu_present*'
      OR name GLOB 'frame*')
)
SELECT
  stage,
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
FROM candidates;
