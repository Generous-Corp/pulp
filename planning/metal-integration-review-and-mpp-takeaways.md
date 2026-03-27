# Metal Integration Review and MPP Takeaways

Date: 2026-03-26

## Purpose

Review Apple's `Metal-Performance-Primitives-Programming-Guide.pdf` and compare its recommendations to Pulp's current Metal integration on macOS and iOS. The goal is to identify what matters now, what is future-looking, and what work should be planned before the Apple rendering story is described as mature.

## Executive Summary

The most important conclusion is that the Apple guide is not primarily about the kind of rendering Pulp is doing today. It is a Metal Performance Primitives guide for tensor-oriented compute kernels, especially GEMM-style work on newer Apple silicon. That makes it highly relevant for future ML or compute-heavy workloads, but only indirectly relevant to the current Dawn + Skia Graphite UI rendering path.

The larger near-term issue is not a missing optimization from the guide. It is that Pulp's current Apple rendering integration appears architecturally incomplete and more loosely wired than the docs imply:

- `GpuSurface` does not currently model a real native presentable surface.
- `SkiaSurface` currently creates its own Dawn device/context and ignores the provided `GpuSurface`.
- iOS has an isolated `CAMetalLayer` helper, but it does not appear to be connected to the active render path.
- No equivalent macOS Metal surface implementation was found in `core/render`.
- The current Graphite path appears offscreen-only, with no visible present path to an Apple drawable.

If this analysis is correct, Pulp should prioritize presentation-path correctness and documentation accuracy before investing in Apple-specific Metal micro-optimizations.

## What The Apple Guide Actually Covers

The reviewed PDF is about Metal Performance Primitives tensor operations, not general-purpose Metal app rendering.

Important themes from the guide:

- The target problem class is tensor/ML-style compute, especially GEMM.
- It emphasizes Apple-silicon-specific tuning rather than generic CUDA-like assumptions.
- It argues that highly optimized GEMM kernels on Apple silicon do not necessarily benefit from manual threadgroup-memory staging.
- It recommends careful benchmarking of tile sizes and access patterns.
- It highlights locality-preserving traversal, static extents, and fused operations to reduce memory traffic.

This is useful guidance if Pulp later adds:

- on-GPU ML inference
- GEMM-heavy DSP kernels
- custom compute pipelines for spectral analysis or AI-assisted features

It is not strong evidence that the current UI renderer is leaving large performance on the table today.

## Current Pulp State On Apple Platforms

### 1. Render docs describe a more integrated stack than the code currently shows

`docs/guides/modules/render.md` describes:

- Dawn providing the GPU device and surface
- `SkiaSurface` wiring Dawn's device into Skia
- `begin_frame()` acquiring the next texture
- `end_frame()` submitting and presenting

But the current implementation does not appear to match that description.

### 2. `GpuSurface` looks like device bootstrap, not a presentable surface abstraction

`core/render/include/pulp/render/gpu_surface.hpp` exposes width, height, `begin_frame()`, and `end_frame()`, but the config and interface do not currently carry any native view, layer, window, or drawable handle. That makes it difficult for the abstraction to actually represent a macOS or iOS presentation target.

In `core/render/src/gpu_surface_dawn.cpp`, the Dawn implementation:

- creates a WebGPU instance
- requests an adapter and device
- stores width and height
- treats `begin_frame()` as an initialization check
- treats `end_frame()` as event processing

There is no visible surface creation, texture acquisition, swap-chain/present step, or native Metal layer hookup in that implementation.

### 3. `SkiaSurface` is offscreen and bypasses `GpuSurface`

`core/render/src/skia_surface.cpp` currently:

- creates its own Dawn instance, adapter, device, and queue
- builds a Skia Graphite context directly from that
- creates an offscreen render target via `SkSurfaces::RenderTarget(...)`
- explicitly ignores the `GpuSurface&` passed into `SkiaSurface::create(...)`

That means the current implementation is not yet a clean "GpuSurface owns presentation; SkiaSurface records into that target" model.

It also means the current render subsystem appears to have two independent GPU ownership paths:

- `GpuSurface` bootstraps one Dawn/WebGPU stack
- `SkiaSurface` bootstraps another and uses that one for real rendering

That is more than a documentation mismatch. It is a structural integration problem that should be resolved before Apple rendering is treated as a coherent subsystem.

### 4. The current Graphite path appears offscreen-only

The current `SkiaSurface` path creates an offscreen `SkSurface`, submits Graphite recordings, and stops there.

This audit did not find a visible path that:

- acquires a drawable from a native Apple layer
- wraps that drawable as the render target for Graphite
- presents the final image to the screen

That means even if the native iOS layer helper were wired in tomorrow, there is still a distinct integration step required to get Graphite output onto an actual presentable surface.

### 5. iOS has foundation code, but it appears unwired

`core/render/src/metal_surface_ios.mm` defines an `IOSMetalSurface` backed by `UIView` + `CAMetalLayer`. It configures:

- Metal device
- BGRA8 pixel format
- drawable size
- display scale
- safe-area accessors

That is useful foundation work. But a repo-wide search only found this implementation and the iOS CMake entry that compiles it. No active call sites were found connecting it to `GpuSurface`, `SkiaSurface`, or a public iOS render factory.

### 6. macOS Metal presentation appears absent or undocumented

`core/render/CMakeLists.txt` includes only the iOS Metal surface helper. No matching macOS Metal layer or view host implementation was found in `core/render` during this audit.

This is significant because the planning and docs story describes a Metal-backed Apple rendering stack across macOS and iOS, while the visible implementation is stronger on offscreen Graphite setup than on actual Apple-window presentation.

## Important Takeaways

## What matters now

1. Presentation-path completeness matters more than Apple-specific tuning.

Before optimizing Metal behavior, Pulp needs a trustworthy model for:

- native surface ownership
- drawable acquisition
- resize and scale-factor handling
- present timing / vsync behavior
- Skia-to-surface integration

2. The docs should distinguish "offscreen Graphite context exists" from "onscreen Metal presentation is production-ready."

Right now, the code suggests Pulp has made real progress on GPU context bring-up, but not yet a fully integrated Apple presentation path.

3. The render gap is not just missing native glue. It is also missing a visible end-to-end present path.

Unifying ownership is necessary, but it is not sufficient on its own. Pulp also needs an explicit way for Graphite output to land on a presentable Apple drawable rather than an orphaned offscreen texture.

4. iOS and macOS should be described separately.

iOS appears to have more foundational work in place because there is at least a native `CAMetalLayer` helper. macOS did not show an equivalent presentation helper in this audit.

## What matters later

If Pulp introduces custom Metal compute or ML acceleration, the Apple guide suggests several principles worth adopting:

1. Do not assume threadgroup-memory staging is automatically best on Apple silicon.
2. Benchmark tile shapes and occupancy before hand-optimizing.
3. Prefer locality-preserving dispatch patterns for large tensor problems.
4. Use static extents where practical to reduce overhead.
5. Fuse operations when doing so reduces round-trips to memory.

Those are future compute-kernel concerns, not the first-order issue in today's Apple render path.

## Suggested Work Items

### Work item 1: Apple render reality check

Produce a short implementation note that explicitly answers:

- What is offscreen only today?
- What is actually presentable on macOS?
- What is actually presentable on iOS?
- Which code path is used by current examples/tests?
- What should the support matrix say right now?

This should drive doc corrections before broader claims are made.

### Work item 2: Unify render surface ownership

Define a single ownership model for:

- native view/layer/window host
- Dawn/WebGPU device and surface
- Skia Graphite recorder/context
- frame acquire / record / submit / present lifecycle

Today these responsibilities appear split in a way that makes the abstraction story harder to trust.

### Work item 3: Define the actual present path

Make the presentation model explicit:

- where the native Apple drawable comes from
- how Dawn/WebGPU owns or wraps that presentable surface
- how Skia Graphite targets it
- where submit and present occur

Today the planning risk is that surface ownership gets cleaned up conceptually, but the renderer still has no explicit on-screen destination.

### Work item 4: Add Apple presentation validation

Add a lightweight validation plan for Apple render integration:

- window-backed rendering on macOS
- view-backed rendering on iOS
- resize / Retina scale handling
- safe-area behavior on iOS
- frame pacing / vsync behavior
- suspend/resume / backgrounding behavior on iOS

Without this, even a completed integration will be hard to describe confidently.

### Work item 5: Separate "render integration" from "Metal compute"

Keep these as distinct roadmap tracks:

- Track A: complete Dawn/Skia presentation on Apple platforms
- Track B: optional future Metal compute / MPP exploration

This avoids distracting the render roadmap with MPP/tensor work that is not yet needed.

### Work item 6: Create a future compute adoption rubric

If Pulp eventually adds Apple-specific compute acceleration, require an explicit justification:

- What workload needs it?
- Why is WebGPU compute insufficient?
- Is it Apple-only or cross-platform?
- Is the win large enough to justify platform divergence?

That will prevent premature complexity in `render` or `signal`.

## Recommendations

### Recommendation 1

Treat the Apple MPP guide as a future optimization reference, not as evidence that the current Metal renderer needs immediate low-level tuning.

### Recommendation 2

Prioritize honest docs and a complete Apple presentation model over performance work.

The current risk is credibility, not raw speed.

### Recommendation 3

Do not describe the Apple render path as mature until:

- `GpuSurface` and `SkiaSurface` are aligned around one frame lifecycle
- Graphite output has an explicit path to a presentable native surface
- macOS and iOS native presentation are both explicit and testable
- the support matrix and render docs match reality

### Recommendation 4

If future Pulp work includes ML inference, spectral compute, or large matrix kernels, revisit this Apple guide and create a separate compute plan. That work should likely live outside the core UI presentation milestone.

## Next Milestone

The next milestone for the Apple render path is now:

[apple-render-surface-integration-spec.md](/Users/danielraffel/Code/pulp/planning/apple-render-surface-integration-spec.md)

That spec turns this review into an implementation target covering:

- desired Apple render architecture
- native surface APIs for macOS and iOS
- ownership boundaries between platform, render, and view
- validation criteria
- honest status language for docs and support manifests
