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
or CPU-load result. The paced follow-up below records the exact Release
executable, output, and provider-identity hashes used on the Apple Silicon
host.

## Downstream GPU-NAM audit boundary

The current `origin/main` tree has no checked-in `examples/gpu-nam` consumer.
The GPU-NAM documentation points to the separate `pulp-gpu-nam` repository,
but that downstream checkout was not built or exercised by this matched
screening branch. In particular, this branch has no downstream evidence for
the consumer's fallback priming, provider handoff, teardown, or lifecycle
behavior under a missed or unavailable GPU path.

Installed-SDK/provider identity receipts are also absent from this campaign.
The exact-provider proof used to configure the Pulp build establishes the
framework-side pinned provider only; it does not attest an installed
GPU-NAM SDK, downstream provider revision, or consumer/provider compatibility.
Those receipts must be captured at the downstream repository boundary and
bound to its source and executable before any GPU-NAM result can be promoted.

Therefore downstream GPU-NAM proof remains **not run** and is a separate gate.
The follow-up must build the external consumer against the installed SDK,
record provider and SDK identity, and run explicit fallback-priming and
lifecycle checks. None of those checks may be inferred from the matched
screening output or represented as `pulp.gpu-audio.p4.raw.v1` evidence.

## Observed host screening run (burst diagnostic, 2026-09-20)

The Release publication build completed for
`pulp-gpu-audio-p4-matched-convolution-benchmark`,
`pulp-test-gpu-audio-trace`, and the provider-identity fixture target. The
focused command was:

```text
ctest --test-dir build-gpu-publish/test --output-on-failure -R 'pulp-gpu-audio-p4-matched-convolution-benchmark|GPU audio trace'
```

The trace tests and both exact-provider identity fixtures passed. The direct
matched executable was then run with its stdout and stderr preserved under
`docs/validation/gpu-audio-p4-matched-screening-20260920/`:

```text
build-gpu-publish/test/pulp-gpu-audio-p4-matched-convolution-benchmark \
  > docs/validation/gpu-audio-p4-matched-screening-20260920/receipt-burst-screening.jsonl \
  2> docs/validation/gpu-audio-p4-matched-screening-20260920/run.stderr
exit=1
```

The terminal record is `status: "screening_failed"`,
`performance_verdict: "unassigned"`, with `staged_records: 12`,
`shared_records: 22`, `staged_terminal_records: 12`,
`shared_terminal_records: 2`, `terminal_records_required: 12`,
`staged_misses: 0`, and `shared_misses: 10`.
The receipt SHA-256 is
`56ae50166e8d6ce44803f6cbd966407cd9d0d275da4cc0b4881c1fda4aec8767`;
the preserved stderr SHA-256 is
`9591e15c3c26f12180d2448eae87586ed73e841fd15db149c480b32f7c42c19e`;
the benchmark executable SHA-256 is
`116b4e0ee1221ef8b548dfa63435a9bac4d6e8051cf0c930f5ad7d837a7312a7`.

This is not an unavailable-provider result. The standalone
`pulp-gpu-dawn-shared-io-provider-probe` passed on the same host and reported
an Apple M5 Max Metal adapter (`hardware_model: "Mac17,7"`,
`adapter_name: "Apple M5 Max"`, `process_events_calls: 147`), and the exact
provider identity fixtures passed. The first run exposed a trace-ownership
gap: the normal service hook drained the private trial queue into Perfetto
before the quiescent accessor could read it (`attempted: 22`, `enqueued: 22`,
`drained: 22`, `invalid: 0`, `dropped: 0`). The benchmark now retains that
queue for configured trials and performs a bounded non-RT provider service
drain before reading it. The corrected run proves queue delivery, but only two
of the required twelve shared terminal records completed; the remaining
records are callback/fallback dispositions and the run exits 1. Keep this as a
failed intermediate diagnostic and do not treat it as a completed performance
result or raw receipt.

The corrected capability snapshots identify the staged trial as an eligible
staged path with an unknown generic provider and a prepared CPU fallback, and
the shared trial as an eligible, prepared `SharedMemory` path with provider
`Dawn` and a prepared CPU fallback. The shared path was therefore selected;
its 2/12 terminal completion count is a lifecycle/completion gap, not a
provider-unavailable fallback classification.

### Formatting follow-up rerun (2026-09-20)

The initial receipt above is preserved as superseded evidence. The four source
files were then normalized with the repository clang-format so the changed-line
format gate can pass. The focused rerun produced the same screening result and
the same benchmark executable bytes:

```text
source_sha256:
  core/gpu_audio/src/detail/staged_async_trace_ledger.hpp 4685dfcc77fd58556e381fb4b4835f8b62aab60e08d67d4a5965eb6ca42e2c28
  core/gpu_audio/src/gpu_convolver.cpp 27e2e5fdf766bd37a96f2b013261908088ae967d33ae7c4ee9c3bd64124a839c
  test/test_gpu_audio_p4_matched_convolution_benchmark.cpp 6b794cc992035de7d36ed522b96efd5dae5c5be02a62eb82a36f0d46000dd542
  test/test_gpu_audio_trace.cpp e2e1e53cae9b0d2163be9495559be9f976236dd72d4a773e29c86a1a8b884b7a
receipt_sha256: 48f2fd9a5e327e2ecd34149c8b589900835ea1d69ce27ef00b6286d9d6076d91
stderr_sha256: 9591e15c3c26f12180d2448eae87586ed73e841fd15db149c480b32f7c42c19e
executable_sha256: 116b4e0ee1221ef8b548dfa63435a9bac4d6e8051cf0c930f5ad7d837a7312a7
exit: 1
status: screening_failed
staged_terminal_records: 12
shared_terminal_records: 2
terminal_records_required: 12
```

The focused CTest run remains 8/9: all trace and exact-provider identity
fixtures pass, while the matched benchmark intentionally fails its incomplete
2/12 shared terminal-record gate. The formatted rerun is still a diagnostic;
it does not emit a raw receipt or a performance verdict.

### Paced screening rerun (2026-09-20)

The benchmark-only follow-up waits one declared 32-frame block period
(`32 / 48000 s`) after each callback. This removes the artificial burst that
filled the fixed ingress queue while leaving runtime/provider code unchanged.
The paced JSONL is the canonical receipt at
`docs/validation/gpu-audio-p4-matched-screening-20260920/receipt.jsonl`; the
prior formatted burst receipt remains at
`receipt-burst-screening.jsonl`.

The direct exact-provider executable completed screening:

```text
status: screening_complete
performance_verdict: unassigned
staged_records: 12
shared_records: 32
staged_terminal_records: 12
shared_terminal_records: 12
terminal_records_required: 12
staged_misses: 0
shared_misses: 10
raw_receipt: not_emitted
```

Receipt and identity bindings for this run are:

```text
source_commit: d619e6c6e05c247739a029a2b70a262b19966ebe
receipt_sha256: 84d39fd73f146ef28b8a89e831b53a801829ad371cb43e0bd50570e1085dd127
stderr_sha256: 9591e15c3c26f12180d2448eae87586ed73e841fd15db149c480b32f7c42c19e
benchmark_executable_sha256: 63deef5ba87037cc4ca74188bd8f5aca64f4214b2be1491d0fb795a3e68e0d11
provider_identity_receipt_sha256: 6f25b897e618813a6e4578d86f736e184ea982994f0b73fc83a94e67deb94288
provider_identity_fixture_executable_sha256: bb28e02b3897da2dd9d2fa09ed3531c913b46f98c6b802bbe980346cb4b71ca3
provider_asset_sha256: 0ebfe03a209ceefe47edfeae70c3cc6c499583b74f35a26140ea55bad7f1e5a9
linked_dawn_archive_sha256: 73727ddf86ffc34eea6fb6392d8d688f44e317ff37b3bb421f8223c8b8815dc9
dawn_revision: f91da75afe31d4d6f47a6da307e1fbabd1b1691a
```

The provider identity fixture was passed for the same configured build. The
standalone benchmark was run directly because the generic CTest invocation
requested an unavailable provider-path dependency (`provider_path_missing`);
that CTest result is not used as screening evidence. The paced result proves
complete terminal-record observation with ten shared realtime deadline misses;
it does not assign a performance verdict, emit `pulp.gpu-audio.p4.raw.v1`, or
prove strict realtime suitability.

## Stop condition

The seam and benchmark now produce a complete paced screening receipt:
shared-memory terminal records are observable, but strict staged-vs-shared P4
latency, realtime suitability, and raw-campaign verdict statistics remain
unmeasured. Keep the status **screening only** until a strict raw writer has
honest callback/result timing and transfer provenance.

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
