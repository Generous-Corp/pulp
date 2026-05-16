---
name: threejs-bridge
description: Build or iterate on Pulp's native Dawn-backed Three.js workflow using the real three.webgpu.js renderer, focused bridge tests, and native demo capture.
---

# Three.js Bridge

Use this skill when a task involves Pulp's native Three.js bridge or the
`examples/threejs-native-demo` workflow.

## Truthful Position

Pulp now supports a **native** Three.js lane built on:

- the real MIT `three.webgpu.js` renderer
- Pulp's V8-backed JS engine
- the Dawn-backed native `GPUCanvasContext` bridge
- a visible native `WindowHost` path with no browser or WebView

Supported now:

- native demo modes in `examples/threejs-native-demo/main.cpp`
  - `cube`
  - `spectrum`
  - `particles`
  - `ribbon`
  - `reverb`
- real `THREE.WebGPURenderer` initialization
- native pointer drag, wheel zoom, and trackpad pinch on the demo canvas
- real `OrbitControls` addon import/init on the native path
- focused bridge validation in `test/web-compat/test_threejs_bridge.cpp`
- native one-shot screenshot capture via `--capture`

Not supported by this skill:

- broad browser compatibility
- a full DOM implementation
- claims that all Three.js APIs or addons are already covered
- cross-platform live-parity claims beyond what the branch has actually proven

## Critical Build Requirements

1. **V8 engine required** — Three.js needs typed arrays, promises, and full ES module support. Configure with:
   ```bash
   cmake -S . -B build -DPULP_JS_ENGINE=v8 \
     -DV8_INCLUDE_DIR=/opt/homebrew/opt/node/include/node \
     -DV8_LIB_DIR=/opt/homebrew/opt/node/lib \
     -DV8_LIBRARY_PATH=/opt/homebrew/opt/node/lib/libnode.141.dylib \
     -DPULP_ENABLE_GPU=ON -DPULP_BUILD_TESTS=ON
   ```

2. **gpu_surface MUST be passed to WidgetBridge** — The native GPU bridge only initializes when WidgetBridge receives a non-null GpuSurface pointer. Without it, Three.js gets no WebGPU device and the 3D canvas renders black. This is the `attach_gpu_surface()` call in the demo.

3. **Three.js fetched via FetchContent** — `PULP_HAS_THREEJS` is set automatically when GPU + tests are enabled.

## Failure Modes To Recognize

These three failures all present as "demo says `status: 'ready'` but the canvas is solid black/blank" — Three.js's `renderer.init()` uses `new Promise(async (resolve, reject) => {…})` which silently swallows any throw inside the executor, so errors do NOT propagate as rejections. Whenever you see a black canvas, suspect one of these:

1. **`createNativeAdapter` / `describeNativeAdapter` missing from `window.pulp.gpu`** (regression introduced by ad7175be7 in April 2026). `web-compat-canvas.js`'s `__ensurePulpGpuHelpers` calls `window.pulp.gpu.createNativeAdapter()`; if undefined, the entire native bridge silently falls back to no-op mocks. Fix: keep the `describeNativeAdapter` / `createNativeAdapter` accessors in `web-compat-document.js` alongside the mock-adapter accessors, and keep the native-aware `navigator.gpu.requestAdapter` that prefers `__describeNativeAdapterImpl` over the mock.

2. **`globalThis.requestAnimationFrame` recursing on `globalThis.window.requestAnimationFrame`** (RangeError: Maximum call stack size exceeded). In V8 the script-top `var window = {…}` becomes a property of globalThis, so a wrapper that lives on globalThis and calls `globalThis.window.X` resolves back to itself. Bind directly to the inner `__requestFrame__` helper (or capture `window.requestAnimationFrame` once at install time and call that local).

3. **`var depthStencil` missing in the `__createMockGPURenderPassEncoder` closure** in `web-compat-gpu-buffered.js`. `encoder.draw` / `encoder.drawIndexed` reference `depthStencil` to forward to `createBufferedDrawPayload`; if undeclared in the closure, the reference resolves as a free variable and throws ReferenceError as soon as the bridge is engaged. Add `var depthStencil = descriptor.depthStencilAttachment || null;` next to the other closure vars.

Diagnosing tools:
- Wrap `renderer.init`'s async-executor body in your own try/catch + reject (in the cached `three.webgpu.js` from FetchContent) to surface the otherwise-silent throw.
- Check `globalThis.__phase13BufferedSkips` after a render — empty array means draws are landing through the native bridge; non-empty means the buffered draw encoder gave up because `attachmentView._nativeBridge` was false.
- Probe `context._nativeBridge`, `context._configured`, `device._nativeBridge`, and `context.getCurrentTexture().createView()._nativeBridge` to bisect which layer dropped the bridge metadata.

## Core Files

Main workflow files:

- `examples/threejs-native-demo/main.cpp`
- `examples/threejs-native-demo/README.md`
- `test/web-compat/test_threejs_bridge.cpp`

Bridge/runtime files often involved:

- `core/view/src/widget_bridge.cpp`
- `core/view/js/web-compat.js`
- `core/view/js/web-compat-canvas.js`
- `core/view/js/web-compat-document.js`
- `core/view/js/web-compat-element.js`
- `core/render/src/gpu_surface_dawn.cpp`
- `core/canvas/src/skia_canvas.cpp`

Truth/status docs to keep aligned:

- `planning/v3-phase14-gap-closure-status.md`
- `planning/v3-verification-report.md`

## Recommended Workflow

### 1. Keep the native path honest

Default to the real native stack:

- real `three/webgpu`
- real native `GPUCanvasContext`
- real native host presentation
- no browser/WebView fallback unless the user explicitly asks for that lane

If the task is about the original Phase 13 acceptance set, prefer extending the
existing native demo modes rather than inventing detached throwaway samples.

### 2. Build the narrowest targets first

```bash
cmake --build build --target pulp-threejs-native-demo pulp-test-threejs-bridge -j8
```

If the task is deeper in the bridge layer, also use the lower-level focused
proofs as needed:

```bash
./build/test/pulp-test-web-compat-prelude "[webcompat][canvas][gpu]"
./build/test/pulp-test-canvas-widget "[canvas_widget][gpu]"
./build/test/pulp-test-skia-surface "[render][skia][readback]"
```

### 3. Run the narrowest truthful test slice

Examples:

```bash
./build/test/pulp-test-threejs-bridge "[threejs][gpu][phase13]"
./build/test/pulp-test-threejs-bridge "[threejs][gpu][phase13][spectrum]"
./build/test/pulp-test-threejs-bridge "[threejs][gpu][phase13][particles]"
./build/test/pulp-test-threejs-bridge "[threejs][gpu][phase13][ribbon]"
./build/test/pulp-test-threejs-bridge "[threejs][gpu][phase13][reverb]"
```

Do not rerun broad unrelated suites when a focused bridge/demo tag is enough.

### 4. Always capture the real native demo for visible changes

```bash
./build/examples/threejs-native-demo/pulp-threejs-native-demo --demo spectrum --capture /tmp/pulp-threejs-spectrum.png
./build/examples/threejs-native-demo/pulp-threejs-native-demo --demo particles --capture /tmp/pulp-threejs-particles.png
./build/examples/threejs-native-demo/pulp-threejs-native-demo --demo ribbon --capture /tmp/pulp-threejs-ribbon.png
./build/examples/threejs-native-demo/pulp-threejs-native-demo --demo reverb --capture /tmp/pulp-threejs-reverb.png
```

Use screenshots to confirm the result is visibly truthful, not just test-green.

### 5. Prefer contract-driven bridge work

When something breaks, do not guess at generic browser APIs.

Instead:

- run the real Three.js path
- take the first concrete failure
- implement only the missing contract that the current renderer path actually needs
- rerun the focused test + capture loop

That keeps the bridge aligned to real Three.js usage instead of drifting toward
an unfocused browser shim.

## What To Check

For native Three.js work, verify:

- `THREE.WebGPURenderer` still initializes
- the native canvas presents non-empty output
- input still reaches the demo canvas when relevant
- audio-reactive modes use data coming from the C++ side truthfully
- docs do not overclaim DOM/addon/runtime parity
- screenshot output still matches the claimed visible result

## When Updating Docs

Keep these in sync when the workflow meaningfully changes:

- `examples/threejs-native-demo/README.md`
- `planning/v3-phase14-gap-closure-status.md`
- `planning/v3-verification-report.md`

If the workflow grows stable enough for broader reuse, keep this skill aligned
with the actual shipped demo modes and focused validation commands.

## Benchmark Mode (Slice 0.5 of #516)

When `PULP_BENCHMARK=ON`, `pulp-threejs-native-demo` exposes a
headless benchmark that drives the JS→GPU upload path without a
visible window:

```bash
pulp-threejs-native-demo --benchmark-seconds=10 --widget=particles \
                         --particle-count=10000 --target-fps=60 \
                         --output=planning/bench/particles-N10000.json
```

The benchmark bypasses the full `three.webgpu.js` module loader and
calls `__gpuQueueDrawBufferedImpl` directly with a vertex-buffer
payload shaped like `THREE.BufferGeometry.setAttribute('position',
new THREE.BufferAttribute(new Float32Array(count * 3), 3))`. It
exercises the exact `widget_bridge.cpp` WriteBuffer path a real
Three.js particles scene would hit.

Gotchas:

- **V8 + a complete Skia tree are both required.** The benchmark
  depends on the native GPU bridge paths which are gated on
  `PULP_HAS_SKIA`. If `external/skia-build/` only contains `include/`
  + `modules/` without `build/mac-gpu/lib/`, `cmake` silently sets
  `PULP_HAS_SKIA=FALSE` and every native WebGPU call in
  `widget_bridge.cpp` short-circuits on its `#ifndef PULP_HAS_SKIA
  return false` guard. The benchmark harness detects this and fails
  fast rather than emitting zero counters.
- **The full `three.webgpu.js` module loader has been seen to hang
  at `status: 'starting'` headless on some hosts** (module runs to
  completion with empty error, but top-level state never transitions
  to `'ready'`). The benchmark works around this by using a minimal
  JS harness; the in-window demo may still hit the hang when invoked
  headlessly via `--capture`. If that happens, fix the loader, don't
  paper over it in the harness.
- **`base64_decode_us` will be zero for the particle benchmark** —
  that counter only fires on the `__gpuComputeDispatchImpl`
  `bufferDataBase64` lane (#535), not the vertex-buffer lane. Don't
  read a zero there as a bug.

## Decision 1 status (#516)

The zero-copy JS↔GPU initiative (#516) ran Decision 1 twice:

1. Slice 0 (PRs #518, #526) measured `ui-preview`'s oscilloscope +
   spectrogram — wrong workload, C++-driven, never exercises the
   upload path.
2. Slice 0.5 measured this benchmark on Three.js particles — honest
   workload, 0.036% → 0.26% of frame budget at 1K → 100K points.

Both landed NO-GO; Slice 0.5 supersedes Slice 0's verdict. See
`planning/zero-copy-decision-1-re-evaluation-2026-04-20.md`. The
new `PerfCounters` fields (`base64_decode_total_us`,
`gpu_buffer_upload_count`, `gpu_buffer_bytes_resident_peak`) stay
merged for future workload-specific re-evaluations.
