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
