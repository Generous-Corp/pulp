# Diagnosing GPU audio with Perfetto

GPU-audio tracing is an opt-in developer instrument. Build a tracing SDK and
capture a bounded session with the ordinary Pulp trace controls:

```sh
pulp build --trace
# Produce the clean and planted-invalid controls from the tracing build.
PULP_GPU_AUDIO_TRACE_TEST_DIR=/tmp \
  ./build/test/pulp-test-gpu-audio-trace '[gpu_audio][trace][perfetto]'
python3 tools/scripts/validate_gpu_audio_trace_captures.py \
  --processor "$HOME/.pulp/tools/trace-processor/v57.2/<platform>/trace_processor_shell" \
  --definitions .agents/skills/trace-sql/pulp_gpu_audio_blocks.sql \
  --positive-trace /tmp/pulp-gpu-audio-positive.pftrace \
  --negative-trace /tmp/pulp-gpu-audio-planted-duplicate.pftrace
```

Install the pinned processor with `pulp trace fetch`, replace `<platform>` with
the installed platform directory. The tracing test writes bounded positive and
planted-negative controls; the validator appends its constant query to a private
temporary copy of the checked-in definitions, and verifies that a missing GPU
timestamp is SQL `NULL`, never zero. For an ad hoc investigation, start a
short session with `PULP_TRACE_RING_KB=262144 pulp trace start --categories
gpu,dsp,render`, reproduce the workload, and stop it. Then make your own
temporary copy of the definitions and append one final `SELECT`; Perfetto v57.2
does not compose `-q` with `--query-string`, and `pulp trace query` does not
currently preload these views. Use a short, dense capture with admission
capture enabled and success sampling stride 1 when reconstructing a complete
sequence lifecycle. Start tracing before the generation's first admission. Stop
and join the callback, service, and diagnostic drain callers, then call the
session's explicit quiescent `release()`. A successful release closes pending
admissions and performs the final drain before destroying its recorder. Only
then stop tracing; do not race release with a second diagnostic consumer. For a long tail,
keep the raw benchmark receipt and use the trace for causal correlation; the
default stride is deliberately bounded and cannot prove a miss rate.

The authoritative path is `SharedIoConvolutionSession` → `SharedIoConvolutionPipeline` →
`SharedIoStampedBridge`. The session records actual worker admission and GPU
terminal facts; the bridge records callback eligibility and the audio delivered.
These facts use separate fixed, trivially-copyable records and bounded SPSC
queues. The callback never calls Perfetto, logs, formats strings, allocates,
waits for the worker, or drains a trace queue. A non-realtime diagnostic thread
drains the queues and emits `gpu.audio.*` events. Overflow is observable loss
and never backpressure on audio. No parallel execution controller predicts or
replays the runtime's decisions.

Schema 2 emits `gpu.audio.admission`, `gpu.audio.terminal`,
`gpu.audio.eligible`, and `gpu.audio.delivery` independently. The SQL joins
these observations into `pulp_gpu_audio_blocks` by process `upid` plus
`(engine_id, generation, sequence)`. Each worker admission requires exactly one
GPU terminal; each callback eligibility marker requires exactly one delivery.
Callback output can be silence without a corresponding worker admission, and
teardown can close admitted GPU work that never became callback-eligible.
Schema-1 combined `gpu.audio.block` captures do not qualify under these views.

`gpu.audio.recovery` records an actual quiescent drain/reprime with its old
`generation`, `next_generation`, and the next epoch's initial `sequence`.
Recovery requires callback, service, and diagnostic drain callers to be stopped
and joined; this instrumentation does not add live recovery. The private session reports
`silence_delivered` when no ready GPU output exists. Public
`GpuAudioTransport`/`GpuConvolver` callback routing, continuously primed
CPU fallback, and live recovery remain unresolved integration gates outside
this private session. A trace must not infer `cpu_fallback_delivered` from a
missing GPU output.

`gpu.audio.session` identifies the engine generation (the stream epoch), path,
block shape, algorithmic lead, logical pipeline depth, physical provider-slot
count, sampling policy, and clock provenance. `pipeline_depth` is the logical
bridge/completion-record capacity and must exceed `lead_blocks` so the callback
currently being published has its own record. `provider_slots` is the physical
provider-arena slot count; completed slots can be reused within that logical
window, so it is not another spelling of pipeline depth. Earlier schema-2
captures predate `provider_slots` and expose SQL `NULL`, meaning unavailable,
never zero. Terminal and delivery events carry the exact `(engine_id,
generation, sequence)` identity; `sequence` is the block sequence. Terminal
events carry the worker stage durations:

| Annotation | Meaning |
| --- | --- |
| `pre_submit_ns` | physical input lease admission through provider submission begin |
| `encode_ns` | worker input packing interval; excludes provider command encoding |
| `submit_call_ns` | provider submission call, including command encoding performed inside it |
| `submit_to_observed_ns` | submit through completion observation; not GPU execution |
| `scheduled_to_observed_ns` | worker admission through completion observation on the CPU clock |
| `gpu_elapsed_ns` | provider-authentic GPU timer only; `-1`/unavailable otherwise |

The callback never reads a diagnostic clock. These intervals begin at the
worker's physical input lease admission, so callback-to-worker queue delay is
unavailable. The field `encode_ns` retains its schema name but measures the
session's input packing, not an isolated GPU command encoder.

`outcome`, `reason`, `gpu_reason`, `delivery_reason`, `gpu_terminal`, and
`delivery` are stable text labels in
the trace. They are not enum ordinals, so captures remain readable if the C++
enum layout changes. GPU reasons belong to terminal events and delivery reasons
belong to callback delivery events.

The GPU terminal disposition is a first-class enum answering what happened to
admitted work: `completed_accepted`, `stale_rejected`, `late_rejected`,
`provider_failed`, `device_lost`, or `cancelled_teardown`. The delivery
disposition answers what audio was delivered: `gpu_delivered`, `cpu_fallback_delivered`,
`silence_delivered`, `passthrough_delivered`, `priming`, or `invalid_rejected`
(the callback rejected an invalid output shape). A late GPU result
and fallback therefore remain two independent facts joined by identity. `gpu_reason`
explains the GPU terminal disposition and `delivery_reason` explains the audio
delivery; `reason` is the compatibility projection and must not be used to
reconstruct both causes. Completion, late/stale rejection, provider/device
failure, or explicit quiescent teardown closes each traced worker admission.
The independent callback delivery event closes its eligibility marker. Integrity
views flag missing, duplicate, orphaned, or stale-accepted dispositions.

Interpretation examples:

```sql
-- Admission / submit tails (NULL means the stage was not observed).
SELECT sequence, pre_submit_ns/1000.0 AS pre_submit_us,
       submit_to_observed_ns/1000.0 AS observed_us
FROM pulp_gpu_audio_blocks ORDER BY pre_submit_ns DESC LIMIT 25;

-- Silence delivered while the worker result was unavailable.
SELECT sequence, gpu_terminal, gpu_reason, delivery, delivery_reason
FROM pulp_gpu_audio_blocks
WHERE delivery = 'silence_delivered';

-- Find rendering activity around record drain time. This is context, not a
-- causal join to the original block timestamps.
SELECT b.sequence, b.ts, b.scheduled_to_observed_ns/1e6 AS observed_ms,
       r.name, r.dur/1e6 AS render_ms
FROM pulp_gpu_audio_blocks b
JOIN thread t ON t.upid = b.upid
JOIN thread_track tt ON tt.utid = t.utid
JOIN slice r ON r.track_id = tt.id AND r.category = 'render' AND r.dur >= 0
  AND r.ts BETWEEN b.ts - 5000000 AND b.ts + 5000000
ORDER BY b.ts;
```

Do not treat an empty result as zero cost. First confirm the capture has
`gpu.audio.session`, positive terminal and delivery rows, no processor data-loss
stats, and zero rows in `pulp_gpu_audio_capture_issues`. Exactly-once lifecycle
proof additionally requires a qualified row for each captured generation in
`pulp_gpu_audio_full_lifecycle_generations` and zero rows in
`pulp_gpu_audio_full_lifecycle_issues`, `pulp_gpu_audio_admission_violations`,
and all duplicate/lifecycle violation views. Those checks require admission
capture, stride 1, a capture begun before the first admission, explicit
quiescent release after all producers and diagnostic consumers have stopped,
the release-owned final drain, and no loss
from any producer queue. The aggregate record counters count terminal, eligible,
delivery, and recovery records; admission counters count worker admissions
separately. Recorder counters are
cumulative for one immutable engine generation; an overflow before capture is
therefore conservatively visible and makes that capture unavailable. Missing categories, dropped records, or a compiled-out tracing build produce
an **unavailable** lifecycle finding. Priming-only captures cannot establish
GPU execution behavior. Missing stage timestamps or unavailable GPU timers
make those timing measurements unavailable without invalidating otherwise
complete lifecycle evidence. A clean integrity query is necessary evidence only; it
does not prove realtime determinism.

The trace is intended to explain where a deadline was lost: CPU admission,
encoding/submission, completion observation, device loss, or delivery fallback.
It cannot establish a hard scheduling guarantee, and it cannot infer GPU
execution duration from a CPU completion interval.
