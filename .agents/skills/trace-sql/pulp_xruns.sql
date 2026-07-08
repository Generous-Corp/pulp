-- pulp_xruns — audio xruns / deadline misses as instant events.
--
-- Backs the L0 preset `pulp trace xruns`. One row per xrun / deadline-miss
-- instant event (category `dsp`, name GLOB `xrun*` OR `deadline_miss*`),
-- emitted from the same overload detection that feeds AudioProcessLoadMeasurer.
--
-- These are Perfetto *instant* events (zero-duration marks), so this view keys
-- off the name pattern, not `dur`. GLOB (not LIKE) is used deliberately:
-- PerfettoSQL GLOB is case-sensitive and honours `*`/`?` the way trace tooling
-- expects, whereas LIKE is case-insensitive and uses `%`/`_`.
--
-- Args carried on each mark (present when the emit site set them):
--   block_index    — the processing block the miss occurred on
--   load_fraction  — the load-measurer reading at that block
--
-- Note: because Perfetto is off the live audio thread (trace.hpp), these marks
-- come from the offline/analysis path; in-DAW live xruns are covered by the
-- fixed-slot telemetry fallback, not this view.
--
-- Idempotent.

CREATE OR REPLACE PERFETTO VIEW pulp_xruns AS
SELECT
  s.ts                         AS ts,
  s.name                       AS kind,
  EXTRACT_ARG(s.arg_set_id, 'debug.block_index')   AS block_index,
  EXTRACT_ARG(s.arg_set_id, 'debug.load_fraction') AS load_fraction,
  thread.name                  AS thread_name
FROM slice AS s
JOIN thread_track AS tt ON s.track_id = tt.id
JOIN thread            ON thread.utid = tt.utid
WHERE s.category = 'dsp'
  AND (s.name GLOB 'xrun*' OR s.name GLOB 'deadline_miss*')
ORDER BY s.ts ASC;
