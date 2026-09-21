# P4 raw campaign implementation handoff

Status: the staged producer seam is now available behind a quiescent-only
drain, while the strict raw campaign remains blocked on callback/result timing
and campaign context. This note is an implementation handoff, not a
performance result. It records why the current probe cannot honestly emit
`pulp.gpu-audio.p4.raw.v1` and the bounded changes needed before running the
campaign on Apple Silicon.

## What exists on `origin/main`

`test/test_gpu_shared_io_paced_convolution_probe.cpp` is the only physical
probe currently registered as
`pulp-gpu-shared-io-paced-convolution-probe` (registration in
`test/cmake/render_gpu_surface_tests.cmake`). It exercises the automatically
selected shared Dawn path, uses a sleeping non-RT callback driver, and emits
`pulp.gpu-audio-paced-convolution.v1`. It has no staged trial and no per-block
worker/submit/completion receipt.

The runtime already has useful lifecycle instrumentation:

* `detail::SharedIoTraceRecord` carries a sequence, terminal disposition,
  delivery disposition, and worker-side encode/submit/completion timestamps
  (`core/gpu_audio/src/detail/shared_io_trace.hpp:64-98`).
* The session stamps admission, encode, submit, and completion stages
  (`core/gpu_audio/src/detail/shared_io_convolution_session.cpp:114-177` and
  `342-429`).
* The trace drain is intentionally non-RT and emits Perfetto events with the
  sequence and terminal/delivery fields
  (`core/gpu_audio/src/detail/shared_io_trace.cpp:251-300`).

This is enough to improve diagnostics, but it is not enough to manufacture a
strict P4 raw receipt. In particular, the current session initializes both
`Scheduled` and `WorkerEntry` from the worker clock
(`shared_io_convolution_session.cpp:124-128`), and the callback does not stamp
its own clock. The resulting values cannot be represented as direct
`callback_cpu` or `scheduled -> admitted` timing.

## Exact validator requirements that are currently unmet

`tools/scripts/gpu_audio_p4_evidence.py` requires the raw schema
`pulp.gpu-audio.p4.raw.v1`, with `staged_sync`, `staged_async`, and
`shared_async` trials. Normal async trials must contain eligible GPU delivery,
positive direct/correlated `submit_to_completion`, and complete CPU timing
fields. Confirmation/default campaigns additionally require every verdict
timing to be direct/correlated and require long matched campaigns. A receipt
from the existing probe, or a relabelled historical paired receipt, must not be
accepted as P4 evidence.

The current public transport API only exposes `prepare`, `process`, `pump`,
`release`, `stats`, and `capability_report`
(`core/gpu_audio/include/pulp/gpu_audio/gpu_audio_transport.hpp`). It does not
select a transport per trial or expose the per-block trace queue. The current
`GpuConvolver::prepare()` always chooses shared Dawn when the expected Dawn
revision is available and otherwise falls back to the legacy staged provider
(`core/gpu_audio/src/gpu_convolver.cpp:121-190`).

## Bounded implementation slice

The next implementation should be one private, host-only benchmark seam, not a
public SDK redesign:

1. Add a private `GpuConvolver` preparation configuration consumed only before
   `prepare()`: requested path (`RequireStaged` or `RequireSharedHostPointer`),
   completion policy, trace enabled, and capture-admissions enabled. The
   configuration must be quiescent-only and have no callback-time allocation or
   locking.
2. Add a quiescent observation accessor for the authenticated terminal and
   delivery dispositions. The benchmark must use this accessor to write one
   terminal outcome for every admitted `(generation, sequence)`.
3. Add callback-side timestamps only if they can be captured with the same
   monotonic clock and without violating the RT contract. Otherwise emit an
   explicit `unavailable` timing and keep the campaign in screening; do not
   infer it from worker timestamps.
4. Add a dedicated benchmark target that runs separate staged and shared
   trials on one prepared device, writes an explicitly intermediate JSONL
   record, and leaves `performance_verdict` unassigned. Keep the existing
   paced probe unchanged.
5. Add unit tests for configuration isolation, exact terminal accounting, and
   negative-control rejection. Physical performance acceptance still requires a
   Release build and Shipyard/TartCI execution on Apple Silicon.

The first producer/runtime slice is complete: `drain_gpu_convolver_trial_records`
now drains the staged ledger only after `StagedAsyncTrialState::quiescent()`
proves that no request remains pending. A request admitted before a failed
submit is closed as `CancelledTeardown`, preserving exactly-once terminal
accounting. The accessor returns authenticated intermediate records and does
not emit `pulp.gpu-audio.p4.raw.v1`; callback/result-visible timing and matched
trial context are still required for that format.

The old paired-provider worktree (`f92a9b0b52`) contains names for much of this
seam (`configure_gpu_convolver_service_for_next_prepare`,
`configure_gpu_convolver_trace_for_next_prepare`,
`reprepare_gpu_convolver_for_trial`, `gpu_convolver_trial_observation`, and
`last_gpu_convolver_delivery`), but it also includes a broad private provider
stack and emits `pulp.gpu-audio-paced-convolution-paired.v1`. Do not cherry-pick
that stack wholesale or relabel its receipts. Port only the controls above after
reconciling them with the current merged P5/P6 implementation.

## Matched screening slice (`7304159271`)

The private target `pulp-gpu-audio-p4-matched-convolution-benchmark` now runs
the same 2-channel, 32-frame, 48 kHz, 257-tap input/IR pair through separate
`RequireStaged` and `RequireSharedHostPointer` `GpuConvolver` preparations. It
drives both with a caller-owned `GpuAudioTransport`, stops the transport before
draining records, checks every authenticated record, and carries one shared
pair ID plus input/IR context fingerprints into the output. Staged records now
carry their configured preparation generation so they can be matched against
shared records without manufacturing identity.

The target emits `pulp.gpu-audio.p4.matched.v1` only. Callback and
publish-to-consumable timing, transfer counters, and source/kernel/plan
digests remain explicitly unavailable; `performance_verdict` is always
`unassigned`, and the summary records `raw_receipt: "not_emitted"`. This is a
screening diagnostic, not a P4 raw receipt or a realtime, latency, throughput,
or CPU-load result. The producer worktree has no configured runtime/build, so
the target has not been executed on a physical Apple Silicon provider here.

## Stop condition

Until the seam and benchmark exist, the honest status is **screening only**:
shared-memory correctness and lifecycle/Perfetto diagnostics are available;
strict staged-vs-shared P4 latency or realtime suitability is not yet measured.

## Raw-writer contract gap

The new quiescent drain returns authenticated `SharedIoTraceRecord` values, but
it is intentionally not a P4 writer. A trace record can provide the engine,
generation, sequence, CPU stage timestamps, GPU terminal, and delivery
disposition. It does not carry the raw schema's trial metadata (`trial_id`,
`pair_id`, path, load, block geometry, deadline/watchdog), transfer counters,
or a direct/correlated timestamp provenance for callback and result-visible
boundaries. It also cannot identify the staged provider, because that provider
does not emit these records.

Consequently a pure writer that serialized these records as
`pulp.gpu-audio.p4.raw.v1` would either invent fields or mark required verdict
timings as direct when they are not. The next safe writer task must accept an
explicit trial context and transfer/timing provenance from the benchmark, then
reject records whose required fields are unavailable. Until that context is
defined, keep the drain as the authenticated intermediate representation and
do not add a relabelling serializer.
