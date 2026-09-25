# Provider-owned WaveNet plan boundary (2026-09-25)

This branch is based on Pulp PR #8843 (`fceedb0f5a6`). It records the next
implementation boundary for GPU-NAM without claiming that shared WaveNet
execution exists.

## What #8843 provides

`<pulp/gpu_audio/gpu_audio_program.hpp>` provides a typed,
backend-neutral `GpuAudioProgramDescriptor` and fail-closed validation against
`GpuAudioCapabilityReport`. It records a neural workload, path, provider
identity, lead, pipeline depth, provider-slot count, provider-owned resources,
and CPU-fallback preparation. It intentionally contains no Dawn or Metal
handles and no execution methods.

That is the correct SDK boundary for consumers. GPU-NAM can describe a future
WaveNet program with it, but cannot use it to submit work.

## Existing private lifecycle

`SharedIoPreparedProgram` in `core/gpu_audio/src/detail/shared_io_arena.hpp`
already owns the provider-paired prepare, submit, and release lifecycle. The
arena also owns fixed slot leases, terminal inboxes, stale-token rejection, and
drain barriers.

The only concrete session,
`SharedIoConvolutionSession`, is not generic in practice:

- `SharedIoConvolutionPipeline` assumes FFT-sized complex payloads and overlap
  add state.
- `SharedIoConvolutionSession::pack_input()` and its completion path assume
  convolution frame layout.
- `DawnSharedIoProvider` only constructs
  `DawnSharedIoConvolutionProgram`; its Dawn device, queue, pipelines, and
  implementation state remain private.

Therefore a WaveNet node cannot safely reuse that session with a different
payload shape. Doing so would either reinterpret audio blocks as complex FFT
frames or bypass the provider's authenticated terminal lifecycle.

## Smallest safe next seam

Add a private, backend-neutral `SharedIoProgramSession` that owns only:

1. a provider/program pair;
2. fixed input/output byte sizes and slot count;
3. the existing `SharedIoArena` and `SharedIoComputePlan` lifecycle;
4. a typed terminal/completion queue and epoch fence.

Its program-specific adapter should provide only `pack_input`,
`unpack_output`, and a provider-owned `SharedIoPreparedProgram`. A future
`DawnSharedIoWavenetProgram` can then use this seam to own resident weights,
per-stream causal-history buffers, uniforms, bind groups, and pipelines while
remaining private. The first implementation should validate and prepare those
resources and prove release/reprime behavior; it should not advertise realtime
execution until the adapter has a real submit path and GPU-NAM has numerical,
stereo-isolation, zero-transfer, fallback, and terminal-receipt evidence.

## Current blocker

No existing Pulp API supplies the provider's private Dawn context to a neural
program, and no existing session supplies generic block packing. Adding a
public raw-handle accessor or a no-op WaveNet provider would violate the
#8843 contract. The next implementation must therefore introduce the private
generic session first, then add the provider-owned WaveNet plan behind it.

Until those pieces land, GPU-NAM remains a valid staged worker integration with
CPU fallback and must not report `SharedMemory` or provider-owned WaveNet
resources.

## Implementation slice

This branch adds the private `SharedIoProgramSession` wrapper over
`SharedIoComputePlan`. It owns the provider/program pair, rejects incomplete
or zero-sized preparations, forwards fixed-slot input, submit, service,
completion, output, cancellation, expiry, discard, and reprime operations, and
keeps the provider alive until the plan's physical release barrier succeeds.
The focused lifecycle test exercises a fake provider and program through a
complete input/submit/retire/output/release cycle, including the requirement
that program release precedes provider slot retirement.

This is still a lifecycle seam, not WaveNet execution. The next evidence gate
is a real provider-owned neural program with authenticated provider identity,
zero-transfer receipts, numerical and stereo-isolation checks, fallback, and
terminal dispositions in GPU-NAM.

## CI follow-up for the authenticated slice

PR #8858 exposed two portability requirements that are now part of this
boundary. The Linux GPU-off build compiled the WaveNet test but correctly
omitted the Dawn provider implementation, so an unconditional authenticated
case produced undefined references to `DawnSharedIoProvider`. The test now
keeps its metadata and validation cases in GPU-off builds and compiles the
authenticated Dawn case only when `PULP_HAS_SKIA` defines
`PULP_GPU_AUDIO_WAVENET_RUNTIME`. This preserves useful CPU-side validation
without pretending that Dawn exists on Linux GPU-off configurations.

The same CI run also found that the test used Catch2's `Approx` through a
transitive include. It now includes `catch2/catch_approx.hpp` explicitly.
The focused Release build remains green with 21 assertions in 5 cases. The
remaining CI failures were consumption-census drift and negative-contract
checks from the pre-guard test target; they should be re-evaluated against the
fresh head rather than treated as WaveNet runtime failures.

## ABI and private-boundary audit

The GPU-NAM model already has the data needed to describe a WaveNet plan:
`pulp-gpu-nam/src/nam_model.hpp::NamModel` exposes `arrays()`
(`LayerArrayConfig`), `weights_data()`, `weights_size()`, and `head_scale()`.
`pulp-gpu-nam/src/gpu_nam.hpp::GpuNam::prepare_with` translates those arrays
to `render::GpuCompute::WavenetLayerArraySpec`, retaining the NAM flat-weight
order and per-channel instance selection. The public
`GpuAudioProgramDescriptor` is intentionally only scheduling/capability
metadata; it carries no model weights or native resources.

The smallest Pulp-owned shared proof therefore stays private. Build a
one-channel adapter test around `DawnSharedIoWavenetProgramSpec` and
`DawnSharedIoProvider` that accepts one array with one dilation layer,
translates a `NamModel`-equivalent fixture, prepares one persistent shared-I/O
slot, submits a block, and compares the terminal output with the prewarmed CPU
reference. The receipt must include provider identity, zero `WriteBuffer` /
readback activity for the audio slot, numeric parity, terminal retirement, and
CPU fallback continuity. This is a bounded provider proof, not full GPU-NAM
support.

The private implementation contract is explicit in
`core/gpu_audio/src/detail/dawn_shared_io_wavenet_spec.hpp`,
`dawn_shared_io_wavenet_program.hpp`, and `dawn_shared_io_provider.hpp`:
resident weights, uniforms, pipelines, per-slot activations/head/history
buffers, imported input/output buffers, and one authenticated submit path. The
current token has no stream-instance field, so the first proof must require
`stream_instances == 1`. Stereo, multiple arrays, and multiple dilation layers
need an instance-qualified private submission context before they can be
enabled safely.

GPU-NAM cannot consume this path today because those headers live under
`core/gpu_audio/src/detail` and are not installed SDK headers. It must not
include them or reach through raw Dawn handles. After the private proof is
green, expose a narrow Pulp-owned public adapter/node contract and keep
GPU-NAM's existing staged `GpuAudioNode` plus continuously primed CPU fallback
as the fallback integration until that public seam has receipts.

## Baseline build timeout receipt

An exact baseline attempt was stopped during dependency/build preparation, before
the GPU-NAM executable existed:

```text
timeout 300 pulp/tools/ci/governed-build.sh cmake --build build --target gpu-nam-gpu-test -j2
rc=124 (timeout wrapper)
governed child rc=143 (terminated after the timeout)
progress: 41% while compiling SDL/Catch2
host load: 49/64/69 -> 40/45/58 on 18 cores
```

The `-j2` invocation was the leaseless floor. No executable or runtime receipt
was produced, so this attempt says nothing about GPU correctness, latency, or
GPU-NAM integration. It is evidence only that the first dependency build did
not complete within five minutes under the observed host load.
