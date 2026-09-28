# GPU WaveNet shared-provider boundary (2026-09-25)

This note records the code-level boundary found while auditing the current
Pulp and GPU-NAM implementations. It is an implementation prerequisite for
the prepared-program contract; it is not evidence that WaveNet shared
execution exists.

## Current authenticated path

`core/gpu_audio/src/detail/dawn_shared_io_provider.{hpp,cpp}` authenticates
one Dawn provider, creates host-mapped input/output slots, and owns the
serialized completion lifecycle. Its only prepared program is
`DawnSharedIoConvolutionProgram`, selected through
`SharedIoConvolutionProgramSpec`. The provider's Dawn device, queue, pipelines,
and implementation state are private. `SharedIoPreparedProgram` is a generic
arena lifetime interface, but it does not expose a device, queue, encoder, or
provider-owned command submission context.

`core/gpu_audio/src/detail/realtime_gpu_audio_path.cpp` deliberately recognizes
only a prepared `GpuConvolver`. A generic `GpuAudioNode` has no shared-provider
callback seam.

## GPU-NAM's current path

`pulp-gpu-nam/src/gpu_nam_cloud_node.hpp` prepares one standalone
`render::GpuCompute` and calls `GpuNam::forward()` from the transport worker.
That reaches `GpuCompute::wavenet_forward()`, which performs a host upload,
encodes a command buffer, submits it, and blocks on a readback. It is a valid
worker-thread baseline and has a continuously primed CPU fallback, but it is
not shared-host-pointer execution.

The persistent WaveNet state currently lives in the private
`DawnGpuCompute::WavenetPlan` (`core/render/src/gpu_compute.cpp`): resident
weights, per-array activation/history buffers, head accumulators and outputs,
uniforms, bind groups, and a readback buffer. The plan is keyed by block size
and instance so stereo streams keep independent causal histories.

## Why a thin adapter is incorrect

The existing convolution prepared program cannot represent WaveNet's causal
history or its multiple dispatch stages. Reusing it would either reset history
or alias stereo instances. Adopting `GpuCompute::initialize_from_device()` is
also insufficient: the authenticated provider's device is private, and that
API still uses `WriteBuffer` plus a blocking readback. Exposing a Dawn handle,
queue, or callback through the public SDK would bypass the provider identity
and lifecycle contract.

## Required next implementation slice

Implement a provider-owned private `DawnSharedIoWavenetProgram` with a typed
spec containing the validated layer-array metadata, flat weights, head scale,
block size, and stream-instance count. The provider must construct resident
pipelines/resources and per-instance causal history, then encode each slot's
host-mapped input into the output buffer while using the existing terminal
inbox and drain barrier. Add a private node-path adapter analogous to
`GpuConvolver`, plus GPU-NAM receipts that prove shared-provider identity,
zero transfer calls, stereo instance isolation, and CPU fallback behavior.

Until that program and adapter exist, GPU-NAM's capability report must remain
staged-only and no integration should claim persistent shared WaveNet
execution.

## Compile-gated preparation slice

This follow-up adds `DawnSharedIoWavenetProgram` as a private
`SharedIoPreparedProgram`. It validates the typed WaveNet shape before any
provider interaction, copies the flat weights into owned immutable storage,
and allocates per-stream causal-history storage sized from the maximum layer
dilation. Its `prepare()` method validates every provider slot capability and
its `release()` clears the prepared state. `submit()` currently refuses before
submission because no provider-owned Dawn pipeline/device context has been
authenticated for WaveNet yet. The refusal is deliberate and keeps callers on
their prepared CPU fallback path.

This proves the resource and lifecycle boundary only. It does not prove Dawn
execution, zero-copy transfer, realtime safety, or GPU-NAM integration. Those
claims remain blocked on a real provider implementation and receipts for
provider identity, numerical output, stereo isolation, fallback, and terminal
retirement.

## Follow-up audit: why the first authenticated implementation must stay scoped

A provider-owned WaveNet program cannot safely be added by only forwarding
`SharedIoPreparedProgram::submit()`. The submit token identifies a slot,
stream epoch, and sequence, but it does not identify a WaveNet stream instance.
`DawnSharedIoWavenetProgramSpec::stream_instances` therefore cannot select a
causal-history buffer for stereo or other independent streams. Mapping an
instance from `slot % stream_instances` would silently mix histories whenever
the fixed ring is reused and would fail numerical parity.

The smallest safe implementation contract is one of:

* make one prepared session represent exactly one causal stream and require
  `stream_instances == 1`; or
* extend the private submission context with an authenticated instance index
  (and include it in the token/slot lease) before enabling multi-instance
  WaveNet.

The provider also needs a private factory and plan owner, for example
`make_wavenet_program(spec)`, `prepare_wavenet_program(spec, slots)`,
`submit_wavenet_program(resources, token, inbox)`, and
`release_wavenet_program()`. Those methods must retain the provider's Dawn
device and queue internally, build resident weight/activation/history buffers,
and use the same queue-future/error-scope/terminal-inbox machinery as the
existing convolution path. No public raw Dawn handle is required.

Until the instance-routing contract and this provider-owned plan exist, a
WaveNet `submit()` refusal is the only honest behavior. A copy/upload/readback
adapter through `render::GpuCompute` would not test shared-memory execution and
must not be counted as GPU-NAM validation.
