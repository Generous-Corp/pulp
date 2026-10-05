-- GPU-audio schema-2 lifecycle views. Append one SELECT to a private copy and
-- run `trace_processor query -f COPY TRACE` with the SDK-pinned processor.
-- Session -> Pipeline -> StampedBridge emits independent admission, terminal,
-- callback eligibility, and delivery facts. Recovery is quiescent only.
-- Event timestamps are diagnostic drain time; stage timings are annotations.
-- All joins include Perfetto upid so reused engine IDs cannot join processes.

CREATE OR REPLACE PERFETTO VIEW pulp_gpu_audio_events AS
SELECT s.id AS slice_id, s.ts, s.name, s.dur, t.upid,
  CAST(EXTRACT_ARG(s.arg_set_id, 'debug.schema') AS INT) AS schema,
  CAST(EXTRACT_ARG(s.arg_set_id, 'debug.engine_id') AS INT) AS engine_id,
  CAST(EXTRACT_ARG(s.arg_set_id, 'debug.generation') AS INT) AS generation,
  CAST(EXTRACT_ARG(s.arg_set_id, 'debug.sequence') AS INT) AS sequence,
  CAST(EXTRACT_ARG(s.arg_set_id, 'debug.next_generation') AS INT) AS next_generation,
  CAST(EXTRACT_ARG(s.arg_set_id, 'debug.quiescent') AS INT) AS quiescent,
  CAST(EXTRACT_ARG(s.arg_set_id, 'debug.physical_release_complete') AS INT) AS physical_release_complete,
  CAST(EXTRACT_ARG(s.arg_set_id, 'debug.unresolved_channel_count') AS INT) AS unresolved_channel_count,
  CAST(EXTRACT_ARG(s.arg_set_id, 'debug.gpu_work_admitted') AS INT) AS gpu_work_admitted,
  CAST(EXTRACT_ARG(s.arg_set_id, 'debug.output_eligible') AS INT) AS output_eligible,
  CAST(EXTRACT_ARG(s.arg_set_id, 'debug.lead_blocks') AS INT) AS lead_blocks,
  CAST(EXTRACT_ARG(s.arg_set_id, 'debug.pipeline_depth') AS INT) AS pipeline_depth,
  CAST(EXTRACT_ARG(s.arg_set_id, 'debug.provider_slots') AS INT) AS provider_slots,
  EXTRACT_ARG(s.arg_set_id, 'debug.batch_id') AS batch_id,
  NULLIF(CAST(EXTRACT_ARG(s.arg_set_id, 'debug.microbatch_size') AS INT), -1) AS microbatch_size,
  EXTRACT_ARG(s.arg_set_id, 'debug.model_family') AS model_family,
  EXTRACT_ARG(s.arg_set_id, 'debug.model_hash') AS model_hash,
  EXTRACT_ARG(s.arg_set_id, 'debug.provider_id') AS provider_id,
  EXTRACT_ARG(s.arg_set_id, 'debug.provider_source_hash') AS provider_source_hash,
  EXTRACT_ARG(s.arg_set_id, 'debug.provider_executable_hash') AS provider_executable_hash,
  EXTRACT_ARG(s.arg_set_id, 'debug.predictor_version') AS predictor_version,
  EXTRACT_ARG(s.arg_set_id, 'debug.predictor_calibration') AS predictor_calibration,
  CAST(EXTRACT_ARG(s.arg_set_id, 'debug.claim_batching') AS INT) AS claim_batching,
  CAST(EXTRACT_ARG(s.arg_set_id, 'debug.claim_model_identity') AS INT) AS claim_model_identity,
  CAST(EXTRACT_ARG(s.arg_set_id, 'debug.claim_provider_identity') AS INT) AS claim_provider_identity,
  CAST(EXTRACT_ARG(s.arg_set_id, 'debug.claim_prediction') AS INT) AS claim_prediction,
  CAST(EXTRACT_ARG(s.arg_set_id, 'debug.success_stride') AS INT) AS success_stride,
  CAST(EXTRACT_ARG(s.arg_set_id, 'debug.capture_admissions') AS INT) AS capture_admissions,
  CAST(EXTRACT_ARG(s.arg_set_id, 'debug.gpu_clock_mapped') AS INT) AS gpu_clock_mapped,
  CAST(EXTRACT_ARG(s.arg_set_id, 'debug.gpu_elapsed_available') AS INT) AS gpu_elapsed_available,
  CAST(EXTRACT_ARG(s.arg_set_id, 'debug.admissions_attempted') AS INT) AS admissions_attempted,
  CAST(EXTRACT_ARG(s.arg_set_id, 'debug.admissions_enqueued') AS INT) AS admissions_enqueued,
  CAST(EXTRACT_ARG(s.arg_set_id, 'debug.admissions_dropped') AS INT) AS admissions_dropped,
  CAST(EXTRACT_ARG(s.arg_set_id, 'debug.admissions_drained') AS INT) AS admissions_drained,
  CAST(EXTRACT_ARG(s.arg_set_id, 'debug.attempted') AS INT) AS attempted,
  CAST(EXTRACT_ARG(s.arg_set_id, 'debug.sampled_out') AS INT) AS sampled_out,
  CAST(EXTRACT_ARG(s.arg_set_id, 'debug.invalid') AS INT) AS invalid,
  CAST(EXTRACT_ARG(s.arg_set_id, 'debug.enqueued') AS INT) AS enqueued,
  CAST(EXTRACT_ARG(s.arg_set_id, 'debug.dropped') AS INT) AS dropped,
  CAST(EXTRACT_ARG(s.arg_set_id, 'debug.drained') AS INT) AS drained,
  EXTRACT_ARG(s.arg_set_id, 'debug.gpu_terminal') AS gpu_terminal,
  EXTRACT_ARG(s.arg_set_id, 'debug.delivery') AS delivery,
  EXTRACT_ARG(s.arg_set_id, 'debug.outcome') AS outcome,
  EXTRACT_ARG(s.arg_set_id, 'debug.reason') AS reason,
  EXTRACT_ARG(s.arg_set_id, 'debug.gpu_reason') AS gpu_reason,
  EXTRACT_ARG(s.arg_set_id, 'debug.delivery_reason') AS delivery_reason,
  EXTRACT_ARG(s.arg_set_id, 'debug.cpu_clock') AS cpu_clock,
  EXTRACT_ARG(s.arg_set_id, 'debug.event_time') AS event_time,
  NULLIF(CAST(EXTRACT_ARG(s.arg_set_id, 'debug.callback_ingress_ns') AS INT), -1) AS callback_ingress_ns,
  NULLIF(CAST(EXTRACT_ARG(s.arg_set_id, 'debug.callback_start_ns') AS INT), -1) AS callback_start_ns,
  NULLIF(CAST(EXTRACT_ARG(s.arg_set_id, 'debug.callback_end_ns') AS INT), -1) AS callback_end_ns,
  NULLIF(CAST(EXTRACT_ARG(s.arg_set_id, 'debug.result_visible_ns') AS INT), -1) AS result_visible_ns,
  NULLIF(CAST(EXTRACT_ARG(s.arg_set_id, 'debug.delivery_decision_ns') AS INT), -1) AS delivery_decision_ns,
  NULLIF(CAST(EXTRACT_ARG(s.arg_set_id, 'debug.admission_timestamp_ns') AS INT), -1) AS admission_timestamp_ns,
  NULLIF(CAST(EXTRACT_ARG(s.arg_set_id, 'debug.terminal_ns') AS INT), -1) AS terminal_ns,
  NULLIF(CAST(EXTRACT_ARG(s.arg_set_id, 'debug.retirement_ns') AS INT), -1) AS retirement_ns,
  NULLIF(CAST(EXTRACT_ARG(s.arg_set_id, 'debug.ingress_to_worker_ns') AS INT), -1) AS ingress_to_worker_ns,
  NULLIF(CAST(EXTRACT_ARG(s.arg_set_id, 'debug.worker_to_observed_ns') AS INT), -1) AS worker_to_observed_ns,
  NULLIF(CAST(EXTRACT_ARG(s.arg_set_id, 'debug.admission_ns') AS INT), -1) AS admission_ns,
  NULLIF(CAST(EXTRACT_ARG(s.arg_set_id, 'debug.encode_ns') AS INT), -1) AS encode_ns,
  NULLIF(CAST(EXTRACT_ARG(s.arg_set_id, 'debug.submit_call_ns') AS INT), -1) AS submit_call_ns,
  NULLIF(CAST(EXTRACT_ARG(s.arg_set_id, 'debug.pre_submit_ns') AS INT), -1) AS pre_submit_ns,
  NULLIF(CAST(EXTRACT_ARG(s.arg_set_id, 'debug.submit_to_observed_ns') AS INT), -1) AS submit_to_observed_ns,
  NULLIF(CAST(EXTRACT_ARG(s.arg_set_id, 'debug.scheduled_to_observed_ns') AS INT), -1) AS scheduled_to_observed_ns,
  NULLIF(CAST(EXTRACT_ARG(s.arg_set_id, 'debug.gpu_elapsed_ns') AS INT), -1) AS gpu_elapsed_ns,
  NULLIF(CAST(EXTRACT_ARG(s.arg_set_id, 'debug.predicted_ns') AS INT), -1) AS predicted_ns,
  CAST(EXTRACT_ARG(s.arg_set_id, 'debug.deadline_margin_ns') AS INT) AS deadline_margin_ns
FROM slice s
LEFT JOIN thread_track tt ON tt.id = s.track_id
LEFT JOIN thread t ON t.utid = tt.utid
WHERE s.category = 'gpu' AND s.name GLOB 'gpu.audio.*';

CREATE OR REPLACE PERFETTO VIEW pulp_gpu_audio_sessions AS
SELECT * FROM pulp_gpu_audio_events WHERE name = 'gpu.audio.session';

CREATE OR REPLACE PERFETTO VIEW pulp_gpu_audio_admissions AS
SELECT * FROM pulp_gpu_audio_events WHERE name = 'gpu.audio.admission';

CREATE OR REPLACE PERFETTO VIEW pulp_gpu_audio_terminals AS
SELECT * FROM pulp_gpu_audio_events WHERE name = 'gpu.audio.terminal';

CREATE OR REPLACE PERFETTO VIEW pulp_gpu_audio_eligible AS
SELECT * FROM pulp_gpu_audio_events WHERE name = 'gpu.audio.eligible';

CREATE OR REPLACE PERFETTO VIEW pulp_gpu_audio_terminal_disposition_counts AS
-- A terminal disposition is first-class: every admitted block must contribute
-- exactly one row to this census. Counts above one identify duplicate terminal
-- records; absence is reported by the lifecycle-violation views below. Keep
-- epoch/sequence in the grouping so reused sequence values cannot merge.
SELECT upid, engine_id, generation, sequence,
       gpu_terminal, outcome, COUNT(*) AS terminal_count
FROM pulp_gpu_audio_terminals
GROUP BY upid, engine_id, generation, sequence, gpu_terminal, outcome;


CREATE OR REPLACE PERFETTO VIEW pulp_gpu_audio_deliveries AS
SELECT * FROM pulp_gpu_audio_events WHERE name = 'gpu.audio.delivery';

-- Lossless lifecycle evidence. Keep one row per emitted event, including
-- duplicate identities, so integrity views can report rather than collapse
-- producer history. Use pulp_gpu_audio_blocks for a joined convenience view.
CREATE OR REPLACE PERFETTO VIEW pulp_gpu_audio_lifecycle_records AS
SELECT upid, engine_id, generation, sequence, name AS lifecycle_event,
       slice_id, ts, dur, gpu_terminal, delivery, outcome, reason,
       gpu_reason, delivery_reason, admission_timestamp_ns, admission_ns,
       terminal_ns, delivery_decision_ns,
       retirement_ns, callback_start_ns, callback_end_ns, result_visible_ns
FROM pulp_gpu_audio_events
WHERE name IN ('gpu.audio.admission', 'gpu.audio.terminal',
               'gpu.audio.eligible', 'gpu.audio.delivery', 'gpu.audio.recovery');

CREATE OR REPLACE PERFETTO VIEW pulp_gpu_audio_recoveries AS
SELECT * FROM pulp_gpu_audio_events WHERE name = 'gpu.audio.recovery';

CREATE OR REPLACE PERFETTO VIEW pulp_gpu_audio_counters AS
SELECT * FROM pulp_gpu_audio_events WHERE name = 'gpu.audio.counters';

-- Counter rows are cumulative snapshots. Use one latest snapshot rather than
-- independent maxima that might never have coexisted in one observation.
CREATE OR REPLACE PERFETTO VIEW pulp_gpu_audio_latest_counters AS
SELECT * FROM (
  SELECT c.*, ROW_NUMBER() OVER (
    PARTITION BY upid, engine_id, generation ORDER BY ts DESC, slice_id DESC
  ) AS latest_rank FROM pulp_gpu_audio_counters c
) WHERE latest_rank = 1;

-- One analysis row per identity. Duplicate raw events are rejected separately;
-- the first record is only a display choice, never an integrity qualification.
CREATE OR REPLACE PERFETTO VIEW pulp_gpu_audio_blocks AS
WITH identities AS (
  SELECT upid, engine_id, generation, sequence FROM pulp_gpu_audio_admissions
  UNION SELECT upid, engine_id, generation, sequence FROM pulp_gpu_audio_eligible
  UNION SELECT upid, engine_id, generation, sequence FROM pulp_gpu_audio_terminals
  UNION SELECT upid, engine_id, generation, sequence FROM pulp_gpu_audio_deliveries
), terminal AS (
  SELECT *, ROW_NUMBER() OVER (
    PARTITION BY upid, engine_id, generation, sequence ORDER BY slice_id
  ) AS identity_rank FROM pulp_gpu_audio_terminals
), delivery AS (
  SELECT *, ROW_NUMBER() OVER (
    PARTITION BY upid, engine_id, generation, sequence ORDER BY slice_id
  ) AS identity_rank FROM pulp_gpu_audio_deliveries
)
SELECT i.*, COALESCE(d.ts, t.ts) AS ts,
       EXISTS (SELECT 1 FROM pulp_gpu_audio_admissions a
               WHERE a.upid = i.upid AND a.engine_id = i.engine_id
                 AND a.generation = i.generation AND a.sequence = i.sequence)
         AS gpu_work_admitted,
       EXISTS (SELECT 1 FROM pulp_gpu_audio_eligible e
               WHERE e.upid = i.upid AND e.engine_id = i.engine_id
                 AND e.generation = i.generation AND e.sequence = i.sequence)
         AS output_eligible,
       t.gpu_terminal, t.gpu_reason, t.outcome, d.delivery, d.delivery_reason,
       COALESCE(NULLIF(d.delivery_reason, 'none'), t.gpu_reason) AS reason,
       COALESCE(t.batch_id, d.batch_id) AS batch_id,
       COALESCE(t.microbatch_size, d.microbatch_size) AS microbatch_size,
       COALESCE(t.model_family, d.model_family) AS model_family,
       COALESCE(t.model_hash, d.model_hash) AS model_hash,
       COALESCE(t.provider_id, d.provider_id) AS provider_id,
       COALESCE(t.provider_source_hash, d.provider_source_hash) AS provider_source_hash,
       COALESCE(t.provider_executable_hash, d.provider_executable_hash) AS provider_executable_hash,
       COALESCE(t.predictor_version, d.predictor_version) AS predictor_version,
       COALESCE(t.predictor_calibration, d.predictor_calibration) AS predictor_calibration,
       COALESCE(t.predicted_ns, d.predicted_ns) AS predicted_ns,
       COALESCE(t.deadline_margin_ns, d.deadline_margin_ns) AS deadline_margin_ns,
       COALESCE(t.claim_batching, d.claim_batching) AS claim_batching,
       COALESCE(t.claim_model_identity, d.claim_model_identity) AS claim_model_identity,
       COALESCE(t.claim_provider_identity, d.claim_provider_identity) AS claim_provider_identity,
       COALESCE(t.claim_prediction, d.claim_prediction) AS claim_prediction,
       t.callback_ingress_ns, t.ingress_to_worker_ns, t.worker_to_observed_ns,
       t.admission_ns, t.encode_ns, t.submit_call_ns, t.pre_submit_ns,
       t.submit_to_observed_ns, t.scheduled_to_observed_ns,
       t.gpu_elapsed_available, t.gpu_elapsed_ns
FROM identities i
LEFT JOIN terminal t USING (upid, engine_id, generation, sequence)
LEFT JOIN delivery d USING (upid, engine_id, generation, sequence)
WHERE (t.identity_rank IS NULL OR t.identity_rank = 1)
  AND (d.identity_rank IS NULL OR d.identity_rank = 1);

CREATE OR REPLACE PERFETTO VIEW pulp_gpu_audio_capture_issues AS
SELECT 'missing_session' AS issue, CAST(NULL AS INT) AS upid, CAST(NULL AS INT) AS engine_id, CAST(NULL AS INT) AS generation
WHERE NOT EXISTS (SELECT 1 FROM pulp_gpu_audio_sessions)
UNION ALL
SELECT 'missing_blocks' AS issue, CAST(NULL AS INT) AS upid, CAST(NULL AS INT) AS engine_id, CAST(NULL AS INT) AS generation
WHERE NOT EXISTS (SELECT 1 FROM pulp_gpu_audio_blocks)
UNION ALL
SELECT 'missing_counters' AS issue, CAST(NULL AS INT) AS upid, CAST(NULL AS INT) AS engine_id, CAST(NULL AS INT) AS generation
WHERE NOT EXISTS (SELECT 1 FROM pulp_gpu_audio_counters)
UNION ALL
SELECT 'invalid_event_identity' AS issue, upid, engine_id, generation FROM pulp_gpu_audio_events
WHERE upid IS NULL OR engine_id IS NULL OR engine_id <= 0
   OR generation IS NULL OR generation <= 0 OR schema IS NOT 2 OR dur < 0
   OR (name != 'gpu.audio.session' AND name != 'gpu.audio.counters' AND name != 'gpu.audio.ownership'
       AND (sequence IS NULL OR sequence < 0))
UNION ALL
SELECT 'unknown_event' AS issue, upid, engine_id, generation FROM pulp_gpu_audio_events
WHERE name NOT IN ('gpu.audio.session', 'gpu.audio.admission', 'gpu.audio.terminal',
                   'gpu.audio.eligible', 'gpu.audio.delivery', 'gpu.audio.recovery',
                   'gpu.audio.counters', 'gpu.audio.ownership')
UNION ALL
SELECT 'missing_session_for_event' AS issue, e.upid, e.engine_id, e.generation FROM pulp_gpu_audio_events e
WHERE NOT EXISTS (SELECT 1 FROM pulp_gpu_audio_sessions s
  WHERE s.upid = e.upid AND s.engine_id = e.engine_id AND s.generation = e.generation)
GROUP BY e.upid, e.engine_id, e.generation
UNION ALL
SELECT 'missing_counters_for_session' AS issue, s.upid, s.engine_id, s.generation FROM pulp_gpu_audio_sessions s
WHERE NOT EXISTS (SELECT 1 FROM pulp_gpu_audio_counters c
  WHERE c.upid = s.upid AND c.engine_id = s.engine_id AND c.generation = s.generation)
GROUP BY s.upid, s.engine_id, s.generation
UNION ALL
SELECT 'invalid_session_policy' AS issue, upid, engine_id, generation FROM pulp_gpu_audio_sessions
WHERE lead_blocks IS NULL OR lead_blocks <= 0
   OR pipeline_depth IS NULL OR pipeline_depth <= lead_blocks
   OR (provider_slots IS NOT NULL AND provider_slots <= 0)
   OR success_stride IS NULL OR success_stride <= 0
   OR capture_admissions IS NULL OR capture_admissions NOT IN (0, 1)
   OR cpu_clock IS NOT 'worker.monotonic' OR event_time IS NOT 'drain'
   OR gpu_clock_mapped IS NULL OR gpu_clock_mapped NOT IN (0, 1)
UNION ALL
SELECT 'invalid_counter' AS issue, upid, engine_id, generation FROM pulp_gpu_audio_counters
WHERE admissions_enqueued IS NULL OR admissions_enqueued < 0
   OR admissions_dropped IS NULL OR admissions_dropped < 0
   OR admissions_drained IS NULL OR admissions_drained < 0
   OR attempted IS NULL OR attempted < 0
   OR sampled_out IS NULL OR sampled_out < 0
   OR invalid IS NULL OR invalid < 0
   OR enqueued IS NULL OR enqueued < 0
   OR dropped IS NULL OR dropped < 0
   OR drained IS NULL OR drained < 0
UNION ALL
SELECT 'invalid_prediction_or_identity_metadata' AS issue, upid, engine_id, generation
FROM pulp_gpu_audio_events
WHERE (batch_id IS NOT NULL AND (microbatch_size IS NULL OR microbatch_size <= 0))
   OR (batch_id IS NULL AND microbatch_size IS NOT NULL)
   OR ((model_family IS NULL) != (model_hash IS NULL))
   OR ((provider_id IS NULL) != (provider_source_hash IS NULL)
       OR (provider_id IS NULL) != (provider_executable_hash IS NULL))
   OR ((predicted_ns IS NULL) != (deadline_margin_ns IS NULL)
       OR (predicted_ns IS NULL) != (predictor_version IS NULL)
       OR (predicted_ns IS NULL) != (predictor_calibration IS NULL))
   OR predicted_ns < 0
GROUP BY upid, engine_id, generation
UNION ALL
SELECT 'missing_batch_metadata' AS issue, s.upid, s.engine_id, s.generation
FROM pulp_gpu_audio_sessions s
WHERE s.claim_batching = 1
  AND EXISTS (
    SELECT 1 FROM pulp_gpu_audio_blocks b
    WHERE b.upid = s.upid AND b.engine_id = s.engine_id
      AND b.generation = s.generation
      AND (b.batch_id IS NULL OR b.microbatch_size IS NULL OR b.microbatch_size <= 0))
GROUP BY s.upid, s.engine_id, s.generation
UNION ALL
SELECT 'missing_model_identity' AS issue, s.upid, s.engine_id, s.generation
FROM pulp_gpu_audio_sessions s
WHERE s.claim_model_identity = 1
  AND EXISTS (
    SELECT 1 FROM pulp_gpu_audio_blocks b
    WHERE b.upid = s.upid AND b.engine_id = s.engine_id
      AND b.generation = s.generation
      AND (b.model_family IS NULL OR b.model_hash IS NULL))
GROUP BY s.upid, s.engine_id, s.generation
UNION ALL
SELECT 'missing_provider_identity' AS issue, s.upid, s.engine_id, s.generation
FROM pulp_gpu_audio_sessions s
WHERE s.claim_provider_identity = 1
  AND EXISTS (
    SELECT 1 FROM pulp_gpu_audio_blocks b
    WHERE b.upid = s.upid AND b.engine_id = s.engine_id
      AND b.generation = s.generation
      AND (b.provider_id IS NULL
           OR b.provider_source_hash IS NULL
           OR b.provider_executable_hash IS NULL))
GROUP BY s.upid, s.engine_id, s.generation
UNION ALL
SELECT 'missing_prediction_metadata' AS issue, s.upid, s.engine_id, s.generation
FROM pulp_gpu_audio_sessions s
WHERE s.claim_prediction = 1
  AND EXISTS (
    SELECT 1 FROM pulp_gpu_audio_blocks b
    WHERE b.upid = s.upid AND b.engine_id = s.engine_id
      AND b.generation = s.generation
      AND (b.predicted_ns IS NULL
           OR b.deadline_margin_ns IS NULL
           OR b.predictor_version IS NULL
           OR b.predictor_calibration IS NULL))
GROUP BY s.upid, s.engine_id, s.generation
UNION ALL
SELECT 'metadata_identity_mismatch' AS issue, t.upid, t.engine_id, t.generation
FROM pulp_gpu_audio_terminals t
JOIN pulp_gpu_audio_deliveries d USING (upid, engine_id, generation, sequence)
JOIN pulp_gpu_audio_sessions s USING (upid, engine_id, generation)
WHERE (t.batch_id IS NOT NULL AND d.batch_id IS NOT NULL AND t.batch_id != d.batch_id)
   OR (s.claim_batching = 1 AND ((t.batch_id IS NULL) != (d.batch_id IS NULL)))
   OR (t.microbatch_size IS NOT NULL AND d.microbatch_size IS NOT NULL
       AND t.microbatch_size != d.microbatch_size)
   OR (s.claim_batching = 1 AND ((t.microbatch_size IS NULL) != (d.microbatch_size IS NULL)))
   OR (t.model_family IS NOT NULL AND d.model_family IS NOT NULL
       AND t.model_family != d.model_family)
   OR (s.claim_model_identity = 1 AND ((t.model_family IS NULL) != (d.model_family IS NULL)))
   OR (t.model_hash IS NOT NULL AND d.model_hash IS NOT NULL
       AND t.model_hash != d.model_hash)
   OR (s.claim_model_identity = 1 AND ((t.model_hash IS NULL) != (d.model_hash IS NULL)))
   OR (t.provider_id IS NOT NULL AND d.provider_id IS NOT NULL
       AND t.provider_id != d.provider_id)
   OR (s.claim_provider_identity = 1 AND ((t.provider_id IS NULL) != (d.provider_id IS NULL)))
   OR (t.provider_source_hash IS NOT NULL AND d.provider_source_hash IS NOT NULL
       AND t.provider_source_hash != d.provider_source_hash)
   OR (s.claim_provider_identity = 1
       AND ((t.provider_source_hash IS NULL) != (d.provider_source_hash IS NULL)))
   OR (t.provider_executable_hash IS NOT NULL AND d.provider_executable_hash IS NOT NULL
       AND t.provider_executable_hash != d.provider_executable_hash)
   OR (s.claim_provider_identity = 1
       AND ((t.provider_executable_hash IS NULL) != (d.provider_executable_hash IS NULL)))
   OR (t.predictor_version IS NOT NULL AND d.predictor_version IS NOT NULL
       AND t.predictor_version != d.predictor_version)
   OR (s.claim_prediction = 1
       AND ((t.predictor_version IS NULL) != (d.predictor_version IS NULL)))
   OR (t.predictor_calibration IS NOT NULL AND d.predictor_calibration IS NOT NULL
       AND t.predictor_calibration != d.predictor_calibration)
   OR (s.claim_prediction = 1
       AND ((t.predictor_calibration IS NULL) != (d.predictor_calibration IS NULL)))
   OR (t.predicted_ns IS NOT NULL AND d.predicted_ns IS NOT NULL
       AND t.predicted_ns != d.predicted_ns)
   OR (s.claim_prediction = 1 AND ((t.predicted_ns IS NULL) != (d.predicted_ns IS NULL)))
   OR (t.deadline_margin_ns IS NOT NULL AND d.deadline_margin_ns IS NOT NULL
       AND t.deadline_margin_ns != d.deadline_margin_ns)
   OR (s.claim_prediction = 1
       AND ((t.deadline_margin_ns IS NULL) != (d.deadline_margin_ns IS NULL)))
GROUP BY t.upid, t.engine_id, t.generation
UNION ALL
SELECT 'queue_dropped' AS issue, upid, engine_id, generation FROM pulp_gpu_audio_counters WHERE dropped > 0
UNION ALL
SELECT 'admission_queue_dropped' AS issue, upid, engine_id, generation FROM pulp_gpu_audio_counters WHERE admissions_dropped > 0
UNION ALL
SELECT 'invalid_record' AS issue, upid, engine_id, generation FROM pulp_gpu_audio_counters WHERE invalid > 0
UNION ALL
SELECT 'queue_not_drained' AS issue, upid, engine_id, generation FROM pulp_gpu_audio_latest_counters
WHERE enqueued IS NOT drained
UNION ALL
SELECT 'admission_queue_not_drained' AS issue, upid, engine_id, generation FROM pulp_gpu_audio_latest_counters
WHERE admissions_enqueued IS NOT admissions_drained
UNION ALL
SELECT 'invalid_gpu_timing' AS issue, upid, engine_id, generation FROM pulp_gpu_audio_terminals
WHERE gpu_elapsed_available IS NULL OR gpu_elapsed_available NOT IN (0, 1)
   OR (gpu_elapsed_available = 0 AND gpu_elapsed_ns IS NOT NULL)
   OR (gpu_elapsed_available = 1 AND (gpu_elapsed_ns IS NULL OR gpu_elapsed_ns < 0))
UNION ALL
SELECT 'processor_data_loss' AS issue, CAST(NULL AS INT) AS upid, CAST(NULL AS INT) AS engine_id, CAST(NULL AS INT) AS generation
WHERE EXISTS (SELECT 1 FROM stats WHERE value > 0 AND (severity = 'data_loss'
  OR name IN ('traced_buf_write_wrap_count', 'traced_buf_bytes_overwritten',
              'traced_buf_incremental_sequences_dropped',
              'packet_skipped_seq_needs_incremental_state_invalid')));

CREATE OR REPLACE PERFETTO VIEW pulp_gpu_audio_duplicate_terminals AS
SELECT upid, engine_id, generation, sequence, COUNT(*) AS identity_rows
FROM pulp_gpu_audio_terminals
GROUP BY upid, engine_id, generation, sequence HAVING COUNT(*) > 1;

CREATE OR REPLACE PERFETTO VIEW pulp_gpu_audio_duplicate_deliveries AS
SELECT upid, engine_id, generation, sequence, COUNT(*) AS identity_rows
FROM pulp_gpu_audio_deliveries
GROUP BY upid, engine_id, generation, sequence HAVING COUNT(*) > 1;

CREATE OR REPLACE PERFETTO VIEW pulp_gpu_audio_duplicate_admissions AS
SELECT upid, engine_id, generation, sequence, COUNT(*) AS identity_rows
FROM pulp_gpu_audio_admissions
GROUP BY upid, engine_id, generation, sequence HAVING COUNT(*) > 1;

CREATE OR REPLACE PERFETTO VIEW pulp_gpu_audio_duplicate_eligible AS
SELECT upid, engine_id, generation, sequence, COUNT(*) AS identity_rows
FROM pulp_gpu_audio_eligible
GROUP BY upid, engine_id, generation, sequence HAVING COUNT(*) > 1;

CREATE OR REPLACE PERFETTO VIEW pulp_gpu_audio_lifecycle_violations AS
SELECT 'missing_gpu_terminal' AS violation, a.upid, a.engine_id, a.generation, a.sequence FROM pulp_gpu_audio_admissions a
WHERE NOT EXISTS (SELECT 1 FROM pulp_gpu_audio_terminals b
  WHERE b.upid = a.upid AND b.engine_id = a.engine_id
    AND b.generation = a.generation AND b.sequence = a.sequence)
UNION ALL
SELECT 'orphan_terminal' AS violation, a.upid, a.engine_id, a.generation, a.sequence FROM pulp_gpu_audio_terminals a
WHERE NOT EXISTS (SELECT 1 FROM pulp_gpu_audio_admissions b
  WHERE b.upid = a.upid AND b.engine_id = a.engine_id
    AND b.generation = a.generation AND b.sequence = a.sequence)
UNION ALL
SELECT 'missing_delivery' AS violation, a.upid, a.engine_id, a.generation, a.sequence FROM pulp_gpu_audio_eligible a
WHERE NOT EXISTS (SELECT 1 FROM pulp_gpu_audio_deliveries b
  WHERE b.upid = a.upid AND b.engine_id = a.engine_id
    AND b.generation = a.generation AND b.sequence = a.sequence)
UNION ALL
SELECT 'delivery_without_eligibility' AS violation, a.upid, a.engine_id, a.generation, a.sequence FROM pulp_gpu_audio_deliveries a
WHERE NOT EXISTS (SELECT 1 FROM pulp_gpu_audio_eligible b
  WHERE b.upid = a.upid AND b.engine_id = a.engine_id
    AND b.generation = a.generation AND b.sequence = a.sequence)
UNION ALL
SELECT 'invalid_gpu_terminal' AS violation, upid, engine_id, generation, sequence FROM pulp_gpu_audio_terminals
WHERE gpu_work_admitted IS NOT 1 OR output_eligible IS NOT 0
   OR gpu_terminal IS NULL OR gpu_terminal NOT IN ('completed_accepted', 'stale_rejected',
       'late_rejected', 'provider_failed', 'device_lost', 'cancelled_teardown')
   OR outcome IS NULL OR outcome NOT IN ('success', 'submission_rejected',
       'completion_failed', 'deadline_exceeded', 'stale_rejected', 'late_rejected',
       'device_lost', 'cancelled')
   OR gpu_reason IS NULL
UNION ALL
SELECT 'terminal_outcome_mismatch' AS violation, upid, engine_id, generation, sequence
FROM pulp_gpu_audio_terminals
WHERE (gpu_terminal = 'provider_failed' AND outcome NOT IN ('submission_rejected', 'completion_failed'))
   OR (gpu_terminal = 'late_rejected' AND outcome IS NOT 'late_rejected')
   OR (gpu_terminal = 'stale_rejected' AND outcome IS NOT 'stale_rejected')
   OR (gpu_terminal = 'device_lost' AND outcome IS NOT 'device_lost')
   OR (gpu_terminal = 'cancelled_teardown' AND outcome IS NOT 'cancelled')
UNION ALL
SELECT 'invalid_delivery' AS violation, upid, engine_id, generation, sequence FROM pulp_gpu_audio_deliveries
WHERE (gpu_work_admitted IS NOT NULL AND gpu_work_admitted != 0)
   OR output_eligible IS NOT 1 OR delivery_reason IS NULL
   OR delivery IS NULL OR delivery NOT IN ('gpu_delivered', 'cpu_fallback_delivered',
       'silence_delivered', 'passthrough_delivered', 'priming', 'invalid_rejected')
UNION ALL
SELECT 'stale_acceptance' AS violation, upid, engine_id, generation, sequence FROM pulp_gpu_audio_terminals
WHERE gpu_terminal = 'completed_accepted' AND outcome IS NOT 'success'
UNION ALL
SELECT 'gpu_delivery_without_acceptance' AS violation, d.upid, d.engine_id, d.generation, d.sequence FROM pulp_gpu_audio_deliveries d
WHERE delivery = 'gpu_delivered' AND NOT EXISTS (
  SELECT 1 FROM pulp_gpu_audio_terminals t WHERE t.upid = d.upid
    AND t.engine_id = d.engine_id AND t.generation = d.generation
    AND t.sequence = d.sequence AND t.gpu_terminal = 'completed_accepted'
    AND t.outcome = 'success')
UNION ALL
SELECT 'invalid_recovery' AS violation, upid, engine_id, generation, sequence FROM pulp_gpu_audio_recoveries
WHERE next_generation IS NULL OR next_generation <= generation OR quiescent IS NOT 1
UNION ALL
SELECT 'unresolved_physical_ownership' AS violation, upid, engine_id, generation, NULL AS sequence
FROM pulp_gpu_audio_events WHERE name = 'gpu.audio.ownership'
  AND (physical_release_complete IS NOT 1 OR unresolved_channel_count IS NOT 0);

CREATE OR REPLACE PERFETTO VIEW pulp_gpu_audio_admission_violations AS
SELECT CASE violation WHEN 'missing_gpu_terminal' THEN 'missing_terminal_for_admission'
                      ELSE 'terminal_without_admission' END AS violation,
       upid, engine_id, generation, sequence
FROM pulp_gpu_audio_lifecycle_violations
WHERE violation IN ('missing_gpu_terminal', 'orphan_terminal');

-- Full-lifecycle proof requires tracing before admission, producers quiesced,
-- explicit Session release while its recorder lives, and final queue drain.
-- These views check the trace evidence; the capture procedure proves ordering.
CREATE OR REPLACE PERFETTO VIEW pulp_gpu_audio_full_lifecycle_issues AS
SELECT 'missing_admissions' AS issue, s.upid, s.engine_id, s.generation
FROM pulp_gpu_audio_sessions s
WHERE NOT EXISTS (SELECT 1 FROM pulp_gpu_audio_admissions a
  WHERE a.upid = s.upid AND a.engine_id = s.engine_id AND a.generation = s.generation)
UNION ALL
SELECT 'sampled_records' AS issue, upid, engine_id, generation
FROM pulp_gpu_audio_counters WHERE sampled_out > 0
UNION ALL
SELECT 'admission_capture_disabled' AS issue, upid, engine_id, generation FROM pulp_gpu_audio_sessions WHERE capture_admissions IS NOT 1
UNION ALL
SELECT 'success_sampling_enabled' AS issue, upid, engine_id, generation FROM pulp_gpu_audio_sessions WHERE success_stride IS NOT 1
UNION ALL
SELECT 'admission_attempt_mismatch' AS issue, upid, engine_id, generation FROM pulp_gpu_audio_latest_counters
WHERE admissions_attempted IS NOT admissions_enqueued
UNION ALL
SELECT 'record_attempt_mismatch' AS issue, upid, engine_id, generation FROM pulp_gpu_audio_latest_counters
WHERE attempted IS NOT enqueued
UNION ALL
SELECT 'admission_row_count_mismatch' AS issue, c.upid, c.engine_id, c.generation FROM pulp_gpu_audio_latest_counters c
WHERE c.admissions_drained IS NOT (
  SELECT COUNT(*) FROM (
    SELECT upid, engine_id, generation FROM pulp_gpu_audio_admissions
  ) r WHERE r.upid = c.upid AND r.engine_id = c.engine_id AND r.generation = c.generation)
UNION ALL
SELECT 'record_row_count_mismatch' AS issue, c.upid, c.engine_id, c.generation FROM pulp_gpu_audio_latest_counters c
WHERE c.drained IS NOT (
  SELECT COUNT(*) FROM (
    SELECT upid, engine_id, generation FROM pulp_gpu_audio_terminals
    UNION ALL SELECT upid, engine_id, generation FROM pulp_gpu_audio_eligible
    UNION ALL SELECT upid, engine_id, generation FROM pulp_gpu_audio_deliveries
    UNION ALL SELECT upid, engine_id, generation FROM pulp_gpu_audio_recoveries
  ) r WHERE r.upid = c.upid AND r.engine_id = c.engine_id AND r.generation = c.generation)
UNION ALL
SELECT 'duplicate_terminals_identity' AS issue, upid, engine_id, generation FROM pulp_gpu_audio_duplicate_terminals
UNION ALL
SELECT 'duplicate_deliveries_identity' AS issue, upid, engine_id, generation FROM pulp_gpu_audio_duplicate_deliveries
UNION ALL
SELECT 'duplicate_admissions_identity' AS issue, upid, engine_id, generation FROM pulp_gpu_audio_duplicate_admissions
UNION ALL
SELECT 'duplicate_eligible_identity' AS issue, upid, engine_id, generation FROM pulp_gpu_audio_duplicate_eligible
UNION ALL
SELECT violation AS issue, upid, engine_id, generation
FROM pulp_gpu_audio_lifecycle_violations;

CREATE OR REPLACE PERFETTO VIEW pulp_gpu_audio_full_lifecycle_generations AS
SELECT DISTINCT s.upid, s.engine_id, s.generation FROM pulp_gpu_audio_sessions s
WHERE NOT EXISTS (SELECT 1 FROM pulp_gpu_audio_capture_issues)
  AND NOT EXISTS (SELECT 1 FROM pulp_gpu_audio_full_lifecycle_issues i
    WHERE i.upid = s.upid AND i.engine_id = s.engine_id AND i.generation = s.generation);

-- Frequencies describe observed callback deliveries only. Sampling or loss
-- makes a long-run miss-rate denominator unavailable; retain raw receipts.
CREATE OR REPLACE PERFETTO VIEW pulp_gpu_audio_fallback_frequency AS
SELECT upid, engine_id, generation, COUNT(*) AS output_blocks,
       SUM(CASE WHEN delivery = 'cpu_fallback_delivered' THEN 1 ELSE 0 END) AS cpu_fallback_blocks,
       SUM(CASE WHEN delivery IN ('silence_delivered', 'passthrough_delivered') THEN 1 ELSE 0 END) AS other_fallback_blocks,
       SUM(CASE WHEN delivery = 'priming' THEN 1 ELSE 0 END) AS priming_blocks
FROM pulp_gpu_audio_deliveries GROUP BY upid, engine_id, generation;
