# Generalized Dawn phase E: smallest non-WaveNet proof receipt (2026-10-02)

## Candidate and decision

The smallest non-WaveNet workload that can reuse the existing shared-I/O
transport is a **single-channel, short FIR/convolution block**:

* one prepared `DawnSharedIoProvider` convolution program;
* one or two physical slots (one is sufficient for the minimum proof);
* planar input/output packed into the existing complex FFT slot layout;
* the existing overlap-add executor to preserve cross-block history.

This is smaller than WaveNet because it has no model manifest, weight upload,
rechannel stage, multi-instance identity, or neural state. It also exercises the
same provider/arena/plan/terminal lifecycle that a generalized workload needs.
No GraphNode field or public SDK ABI is needed.

The private implementation already provides this path through
`DawnSharedIoProvider::make_convolution_program`, `SharedIoComputePlan`,
`SharedIoConvolutionExecutor`, and `SharedIoConvolutionSession`. The intended
first proof is mono, one slot, `fft_size >= block_size + ir_length - 1`, with a
direct FIR oracle.

## Executed focused evidence

All seven currently built convolution safety/behavior tests passed:

```text
ctest --test-dir build --output-on-failure -R \
  'GPU convolution (route fails closed|route rejects malformed|route clears output|configuration setters|valid configuration)|GpuConvolver matches direct convolution|GpuAudioTransport fallback stream matches reference convolution'

7/7 passed (0.37 s)
```

This proves the public route fails closed without a provider, rejects malformed
geometry, preserves release/idempotence, matches direct convolution, and keeps
the CPU fallback stream aligned.

## Required private proof and current build boundary

> **Historical wording:** the `NOT_BUILT` labels below describe the earlier
> aggregate CTest inventory. They are superseded by the direct executable
> receipt [`2026-10-02-dawn-phase-e-direct-executables.md`](2026-10-02-dawn-phase-e-direct-executables.md).

The provider-backed proof source is
`test/test_gpu_shared_io_private_convolution_probe.cpp`; the session proof source
is `test/test_gpu_shared_io_convolution_session_probe.cpp`. They emit private
JSON receipts with submission counts, transfer counters, output oracle status,
retirement/failure data, and trace information. Their current CTest entries are
`pulp-test-gpu-shared-io-convolution-pipeline_NOT_BUILT-b12d07c` (#757) and
`pulp-test-gpu-shared-io-convolution-session_NOT_BUILT-b12d07c` (#758), so the
aggregate CTest inventory did not claim Dawn execution in this historical
receipt. Direct execution is recorded by the superseding receipt linked above.

The supporting ledger binary is also unavailable:
`pulp-test-gpu-shared-io-slot-ledger_NOT_BUILT-b12d07c` (#730). The source tests
nevertheless define the acceptance evidence that must be run when built:

* **late result:** `SharedIoConvolutionExecutor` records a late success only
after advancing OLA history, then suppresses delivery for the already-closed
callback position;
* **device loss:** failed terminal completion fences the executor, rejects
subsequent old-epoch results, and requires `fence_and_reprime()` before the new
epoch can publish;
* **ledger safety:** expiry does not reuse storage before physical GPU
completion; retirement rejects new acquisitions; stale epochs and duplicate
terminal claims are rejected; reset is permitted only after quiescence;
* **provider ownership:** the arena retains terminal credit and storage until a
late retirement is observed, including failed drain/release barriers.

These are private lifecycle guarantees, not public plugin callbacks.

## Public boundary and GraphNode safety

The proof remains entirely below the public SDK boundary. Plugin developers
continue to use `GpuAudioNode`/`GpuAudioTransport` and receive capability,
delivery, miss, resync, fallback, and worker-wall-time evidence. Dawn proc
tables, provider handles, queue futures, slot tokens, ledger epochs, device-loss
callbacks, and GPU timestamp mappings remain private detail types.

`GraphNode` remains unchanged. Its only transport-related state is the existing
prepare-stable `transport_sensitive` bit used by anticipation partitioning and
routed binding. No provider pointer, async result, slot token, or Dawn type is
added.

## Precise blocker

The historical next action was to build the two `NOT_BUILT` convolution probes
and the slot-ledger target in a Dawn-capable build. That action is superseded by
the direct executable receipt; the remaining limitation is that the direct
tests still do not claim the live adapter probe or hardware Dawn submission.
No implementation or public ABI change is made in this receipt.
