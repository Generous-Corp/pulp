# Diagnose stamped WaveNet fallback

The physical NAM run at source `642d9c145bafe25d3602de195f503f8fb6c85ac5`
used SDK `7858d7edb5a2bbb568c03d7e740043e8c897c0ee`. Aggregate fallback counts
cannot identify why a block missed. That SDK and the convolver trace successor
`af30ae3b99e59a46baeaa32d2dbd32278fa764ab` do not wire a recorder into the
WaveNet node. A tracing-enabled rebuild alone does not establish attribution.

The plugin uses one block of transport lead at 512 internal frames. At 48 kHz
that is 10.667 ms. Its reported 1024-frame PDC also includes a separate 512-frame
reblocking delay. The full 21.333 ms is not the GPU completion budget. Hardware
callbacks are 128 frames; delivery counters count 512-frame transport quanta.
The internal session currently submits with deadline_ns=0, so its `late` result
flag is not an audio-deadline measurement. The callback's sequence selection is
the authority for which audio was delivered.

## Opt-in node records

Call `GpuWaveNetRealtimeNode::configure_trace` before preparation. The default
is off. Enable admissions and success_stride=1 for lifecycle acceptance. The
node owns its recorder and each prepared epoch. Callback code publishes fixed
records to bounded queues; the sole worker drains them. Release after both
callers stop records any pending cancellation and drains before destroying the
recorder. Failed physical release retains the recorder for retry. Never drain
from a second consumer or call Perfetto in the audio callback.

Correlate `(upid, engine_id, generation, sequence)`. Engine IDs share an allocator
with convolution producers. Every worker-acquired group has one GPU terminal,
including partial channel rejection and cancellation. Eligibility and actual
CPU/GPU delivery are separate events: an eligible fallback block need not have
been admitted to the GPU worker. A completed/published GPU result does not mean
the callback selected it. One group includes all configured channels.

The canonical SQL is `.agents/skills/trace-sql/pulp_gpu_audio_blocks.sql`.
`callback_ingress_ns` is the observed transport callback entry supplied by its
caller. `ingress_to_worker_ns` measures time until the worker acquired that input.
`worker_to_observed_ns` covers the group until all channel results were observed.
These spans can distinguish delayed CPU admission from later provider/service
work. They do not separate GPU queue scheduling, execution and completion
servicing. The group submission API includes encoding and submission internally;
its call boundaries are not represented as GPU Queue::Submit timestamps.
`schedule`, encode, submit, and native GPU elapsed fields remain unavailable.
Zero ingress is unavailable, not zero delay.

Use `delivery_reason` to classify callback fallback and `gpu_reason` for the
independent GPU result. Input saturation, sequence recovery, provider failure,
teardown and a missing due result have different reasons. A generic
`deadline_exceeded` delivery reason establishes absence of a usable due result,
not the cause of that absence. Likewise an eventual published output does not
measure when the callback discarded an older queued result. Do not infer a
scheduler failure from either field alone.

Before attribution, require zero capture/lifecycle/duplicate issues and zero
reported drops. Otherwise the trace is incomplete. Keep observed counters,
missing timestamps and rejected records visible. Do not interpret an empty
capture as a pass.

## Plugin-owned capture

NAM's diagnostic successor uses `GPU_NAM_TRACE_SHARED_IO=1` together with
`PULP_TRACE_PATH=/absolute/new/capture.pftrace`. It refuses that request on an
SDK lacking the WaveNet trace API or tracing support. The existing CLAP
`ScopedTracingAttachment` owns the single plugin trace runtime and flushes after
the processor and its worker are destroyed. The host must not start a second
statically linked trace runtime. Leave `PULP_TRACE_SECONDS` unset for lifecycle
acceptance so a timer cannot stop capture before final records drain.

Run the physical host's normal stop/restart/destroy path, preserving its existing
external watchdog and explicit physical device selection. A forced process exit
cannot establish final flush or complete terminal accounting. Keep trace runs
separate from uninstrumented performance comparisons.
