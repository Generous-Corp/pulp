# Native Metal comparator boundary

Status: measurement design only. This note does not claim that native Metal or
Metal 4 is faster, deterministic, or safe to call from an audio callback.

## Current Pulp boundary

At `origin/main` (`78e52792a3`, checked 2026-09-25), the authenticated shared
audio provider is Dawn. `DawnSharedIoProvider::submit_impl()` creates a Dawn
command encoder, encodes the persistent convolution plan, submits it through a
Dawn queue, and reports completion through the existing terminal inbox
(`core/gpu_audio/src/detail/dawn_shared_io_provider.cpp:1347-1450`). The
session's trace is deliberately CPU-side: it records encode and submit stages
from the service clock, then records completion observation
(`core/gpu_audio/src/detail/shared_io_convolution_session.cpp:345-455`). The
trace schema keeps `gpu_elapsed_ns` unavailable unless a provider supplies an
authentic GPU timer (`core/gpu_audio/src/detail/shared_io_trace.hpp:88-99`).

The current Metal target is only the capability probe
`pulp-test-native-metal-compute` (`test/test_metal_native_compute.mm`). There is
no native Metal provider in `GpuAudioTransport`, and the public SDK must not
expose a Metal queue, command buffer, residency set, or completion callback.
That keeps the comparator a test-only provider control until a measured result
justifies a backend decision.

## What Metal 4 can measure

The macOS 27 SDK documents the relevant APIs:

* `MTLDevice::newMTL4CommandQueue` and `newCommandAllocator` are available from
  macOS 26 (`Metal.framework/Headers/MTLDevice.h:1379-1402`).
* An `MTL4CommandBuffer` must begin with an allocator and end before commit;
  ending permits allocator reuse (`MTL4CommandBuffer.h:54-103`).
* `MTL4CommandQueue::commit:count:options:` accepts commit feedback
  (`MTL4CommandQueue.h:237-259`).
* `MTL4CommitFeedback` reports `GPUStartTime` and `GPUEndTime` in host seconds,
  and invokes its handler after the workload completes
  (`MTL4CommitFeedback.h:19-40`).
* `MTLResidencySet` can hold the persistent allocations and be requested
  resident (`MTLResidencySet.h:40-89`).

These APIs support a useful decomposition, but they do not provide a deadline
or priority contract for audio. A feedback callback is completion observation,
not a guarantee about admission latency or realtime scheduling.

## Bounded comparator design

A valid comparator should use the same no-copy host allocations, kernel shape,
block geometry, and warmup/trial policy for both providers. Each trial records
these CPU-clock intervals separately:

1. command encode start and end;
2. commit call start and return;
3. completion feedback observed;
4. result validation and output visibility.

Metal 4 additionally records the provider-authentic GPU interval from
`GPUStartTime` to `GPUEndTime`. Ordinary Metal can use
`MTLCommandBuffer.GPUStartTime` and `GPUEndTime` after completion on supported
OS versions, but the receipt must mark those fields unavailable when the API
returns zero or otherwise fails validation. Neither provider may wait on the
audio callback. Completion callbacks belong to a non-realtime service, and
results must be matched to a sequence before delivery or fallback.

The persistent form of the experiment must prepare pipelines, argument tables,
residency sets, imported shared buffers, and one allocator per in-flight slot
before the timed loop. Allocating a command buffer or allocator per trial is a
valid construction smoke test, but it is not evidence for the persistent audio
architecture. The output receipt should carry provider identity, OS/device,
source and executable hashes, no-copy status, trial count, all percentiles and
maxima, and `performance_verdict: "unassigned"` until matched Pulp/Dawn and
native runs pass the same correctness and lifecycle gates.

## Existing unmerged control and why it is not evidence yet

The local branch `audit/native-metal-comparison-20260924` contains a useful
standalone ordinary-Metal control and a Metal 4 control (`5ab954b125`,
`701502a1e6`, `1a1d5174d6`). Both use shared buffers and validate a complex
multiply. The Metal 4 control currently waits for feedback with a 100 microsecond
`NSThread` sleep. Its `submit_feedback_*` values therefore include observer
sleep and cannot be compared with ordinary Metal's blocking completion values.
It also allocates a fresh allocator and command buffer on every trial. On an M5
Max running macOS 26.6.2, a manual run produced ordinary p50 159.250 us and
Metal 4 feedback p50 333.916 us, with GPU p50 7.292 us. These are construction
smoke measurements only, not a backend result; the polling and per-trial
allocation differences invalidate a performance comparison.

Before merging that control, replace the timed sleep with a semaphore or
condition signaled by the feedback handler, and add a persistent-resource mode.
Keep the current output schema explicit about CPU observation versus provider
GPU time. The same test should run as `unavailable` on pre-macOS 26 or when the
Metal 4 queue cannot be created, using CTest's skip return code.

## Trigger for a real comparator

The native comparator becomes actionable only after all of these are true:

* Pulp's exact Dawn provider identity and shared-I/O lifecycle gate is green.
* The ordinary and Metal 4 controls use matched persistent resources and an
  equivalent completion-observation mechanism.
* A Release Apple Silicon run produces complete correctness and exactly-once
  terminal receipts for every trial, with no audio callback call into Metal.
* The result is correlated with the Pulp/Dawn decomposition. A standalone
  control cannot establish a Pulp scheduling or realtime claim.

Until then, keep the native code isolated under tests and leave the SDK/backend
selection unchanged. The open question is whether the native submission path
changes the pre-submit or completion tail, not whether Metal 4 is intrinsically
better.
