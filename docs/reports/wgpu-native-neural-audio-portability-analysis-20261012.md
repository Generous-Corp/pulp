# wgpu-native portability analysis for GPU neural/audio

**Review head:** `9da93648a19ef1d849cf0c4d531f4e5796509c92` (protected
`origin/main`, 2026-10-12)

**Scope:** bounded architecture review of the current GPU neural/audio path.
This report does not claim a wgpu-native implementation, product support, or
provider qualification.

## Findings

The existing audio lifecycle is already suitable as a backend-neutral seam.
`SharedIoArenaProvider` defines slot allocation, submit, polling, terminal
retirement, device loss, drain, provider identity, capabilities, and optional
GPU timing without naming Dawn or WebGPU (`core/gpu_audio/src/detail/shared_io_arena.hpp:108-251`).
`SharedIoExecutionContract` likewise describes CPU/staged/shared-host-pointer
paths, lead, pipeline depth, slots, fallback, and typed failure reasons without
native handles (`core/gpu_audio/src/detail/shared_io_execution_contract.hpp:20-135`).
These are the correct facts for a future provider adapter.

The concrete neural implementation is Dawn-specific in four places:

1. `DawnSharedIoProvider` includes `dawn/dawn_proc.h`,
   `dawn/dawn_version.h`, `dawn/native/DawnNative.h`, and
   `webgpu/webgpu_cpp.h` (`core/gpu_audio/src/detail/dawn_shared_io_provider.cpp:10-18`).
2. Creation authenticates a Dawn header/proc/native revision, installs one
   process-global Dawn proc table, and optionally delegates bootstrap to the
   Vellum Dawn coordinator (`.../dawn_shared_io_provider.cpp:67-173`).
3. The provider owns Dawn-native device/queue/command resources and WGSL
   program preparation; the private WaveNet and convolution programs are
   factories for this provider, not generic WebGPU programs.
4. CMake enables the provider only when Apple desktop, Skia, and a Dawn
   archive are present, and otherwise retains the CPU/transport path
   (`core/gpu_audio/CMakeLists.txt:77-83`, `176-195`).

The render device layer is further along toward a WebGPU provider boundary.
Its public `pulp-gpu-device-api` exposes opaque handles and names no provider;
the implementation consumes Dawn/WebGPU privately (`core/render/CMakeLists.txt:1-20`,
`22-63`). `gpu_compute.cpp` is explicitly written against `webgpu/webgpu_cpp.h`
and has a native Dawn instance arm plus an Emscripten arm
(`core/render/src/gpu_compute.cpp:1-22`). CMake already links the `webgpu`
target when no Skia Dawn archive is present, while avoiding duplicate symbols
when Skia supplies Dawn (`core/render/CMakeLists.txt:175-212`). This is the
existing wgpu-native delivery seam, not a neural provider seam.

## Dawn versus WebGPU versus provider-neutral

| Surface | Classification | Portability consequence |
| --- | --- | --- |
| WGSL kernels, bind-group layouts, storage/uniform buffers, queue submit semantics | WebGPU | Reusable by wgpu-native if limits/features and callback behavior are proven. |
| `webgpu_cpp.h` object wrappers | WebGPU C++ binding, implementation-dependent ABI | Reusable only with the exact wgpu-native headers/library tuple; avoid crossing installed public ABI. |
| `dawn_proc.h`, `DawnNative.h`, `dawn_version.h`, `dawnProcSetProcs`, native adapter/device creation | Dawn-specific | Must be isolated behind an adapter; wgpu-native has no equivalent Dawn proc-table contract. |
| Vellum Dawn bootstrap and exact Dawn revision checks | Installed Dawn provider policy | Cannot be reused for wgpu-native; requires a separate authenticated provider identity/bootstrap policy. |
| `SharedIoArenaProvider`, terminal inbox, slot tokens, drain/reprime, execution contract | Provider-neutral private lifecycle | Candidate shared seam for Dawn and wgpu-native adapters. |
| `GpuCompute` public API and opaque handles | Pulp provider-neutral API | Suitable consumer surface; implementation currently still links one selected provider. |
| Metal layer helpers and native surface creation | Platform/window integration | Out of scope for audio compute; wgpu-native neural work can avoid render surfaces. |

## Minimal spike feasibility

A low-risk spike is feasible as a private, non-production adapter test. It
should use the existing `SharedIoArenaProvider`/`SharedIoProgramSession`
lifecycle and a tiny WGSL kernel (for example one WaveNet-equivalent
elementwise layer), while keeping all wgpu-native headers, instance creation,
limits/features, and callbacks in a new private adapter translation unit. The
spike must not expose raw handles or replace the current Dawn provider.

The smallest useful acceptance packet is:

* compile/link against the pinned `webgpu` target on a platform where the
  runtime is available;
* authenticate the selected library/header/runtime tuple and record adapter
  identity, backend, limits, and feature enablement;
* prove one fixed-slot submit/retire/drain/reprime cycle through the existing
  provider-neutral lifecycle;
* compare the kernel with a CPU oracle and exercise a planted device-loss or
  completion-failure control;
* record that the audio callback performs no WebGPU calls, allocations, waits,
  or readbacks.

This is not currently a one-file substitution. wgpu-native does not provide
the Dawn proc-table/native bootstrap used by `DawnSharedIoProvider`; its
callback and device-loss behavior must be mapped to the existing terminal
contract, and its shared-host-pointer/imported-buffer support must be proven
separately. The current CMake condition also excludes the Dawn audio provider
on Linux/Windows and GPU-off builds, so cross-platform acceptance would need a
new opt-in build target and exact runtime packaging evidence.

## Blockers and recommendation

The principal blockers are provider authentication/provenance, equivalent
buffer-import semantics for the shared-host-pointer path, serialized completion
and device-loss behavior, and real hardware evidence. The existing support
matrix explicitly says web DSP is CPU-only today and that WebGPU compute would
run in a worker with a SharedArrayBuffer ring; this is a distinct browser
architecture from native realtime audio (`docs/status/support-matrix.yaml:41-60`).

Recommendation: keep the current Dawn/Vellum neural path unchanged and open a
separate private wgpu-native feasibility spike. Do not update the public SDK,
claim neural portability, or classify wgpu-native evidence as product proof
until the packet above has exact source/runtime hashes, authenticated provider
identity, CPU parity, fallback/deadline evidence, and physical host results.

