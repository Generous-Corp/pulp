# GPU-audio hints — `gpu.audio.*` spans

Schema 2 observes the real `SharedIoConvolutionSession` → `SharedIoConvolutionPipeline` →
`SharedIoStampedBridge` path. Worker admission and terminal facts are independent
of callback eligibility and delivery. Producers publish fixed records to
bounded SPSC queues; only the diagnostic consumer emits Perfetto events.

`gpu.audio.session` establishes engine/generation, provider path, block shape,
lead, physical depth, sampling stride, and clock provenance.
`gpu.audio.admission` and `gpu.audio.terminal` identify admitted worker work;
`gpu.audio.eligible` and `gpu.audio.delivery` identify callback output.
`gpu.audio.recovery` records the old/new generation and initial next sequence
only after a quiescent drain/reprime. It is not live-recovery support.
`gpu.audio.counters` has separate admission counts and aggregate counts for
terminal, eligible, delivery, and recovery records, including queue losses.

Use the checked-in `pulp_gpu_audio_blocks.sql` views. Its joined block view
reconciles actual observations by stable process `upid` and exact
`(engine_id, generation, sequence)`; it does not synthesize terminal decisions.
Raw terminal and delivery views preserve duplicate events for independent
exactly-once checks. Schema-1 combined block events fail closed.

Zero integrity rows mean nothing without positive session, terminal, and
delivery controls. Require zero `pulp_gpu_audio_capture_issues`, zero
`pulp_gpu_audio_full_lifecycle_issues`, zero violation/duplicate rows, and one
`pulp_gpu_audio_full_lifecycle_generations` row per captured generation.
Full-lifecycle capture requires admission capture, stride 1, tracing before
first admission, and no queue or Perfetto loss. Stop and join callback, service,
and diagnostic drain callers before quiescent recovery or explicit session
release. Release performs the final drain before destroying the recorder. A second diagnostic
consumer must never race that lifetime boundary. Counters cover the immutable
generation, so loss before capture fails closed too. A missing annotation must
not vanish through SQL NULL comparisons; required fields are checked explicitly.

The private session delivers silence when GPU output is unavailable. It leaves
public `GpuAudioTransport`/`GpuConvolver` callback routing, continuously primed
CPU fallback, and live recovery as unresolved integration gates. Never label
that silence as `cpu_fallback_delivered`. A late terminal and callback fallback retain separate
`gpu_reason` and `delivery_reason`. GPU delivery requires an accepted terminal
with the same identity. Recovery ordering needs capture-procedure evidence;
an old/new epoch annotation alone cannot prove callbacks were stopped.

Perfetto v57.2 does not compose a definitions `-q` file with a separate
`--query-string`. Copy the definitions, append one investigation `SELECT`, and
run `trace_processor query -f TEMP TRACE`. Keep the checked-in definitions
unchanged. Missing GPU timestamps remain SQL NULL; CPU submit-to-observation
latency must not be relabelled as GPU execution time. Rank tails over raw
terminal durations, not differences between latest atomic snapshots. Missing
timing does not erase otherwise complete lifecycle evidence. Sampling and loss
make long-run miss rates unavailable; preserve raw benchmark receipts.

Session timings start at the worker's physical input lease admission.
`encode_ns` measures input packing; actual provider command encoding can occur
inside `submit_call_ns`. Callback-to-worker queue delay has no timestamp in
this path and must remain unavailable.
