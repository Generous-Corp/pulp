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
  - `gltf-box`
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

1. **V8 engine required** — Three.js needs typed arrays, promises, and full ES module support. Use Homebrew `node@24` on macOS for this lane. The unversioned `node` formula may point at a newer V8 ABI; `libnode.147.dylib` has compiled but aborted during embedded V8 / Three.js evaluation. Configure with:
   ```bash
   cmake -S . -B build -DPULP_JS_ENGINE=v8 \
     -DV8_INCLUDE_DIR=/opt/homebrew/opt/node@24/include/node \
     -DV8_LIB_DIR=/opt/homebrew/opt/node@24/lib \
     -DV8_LIBRARY_PATH=/opt/homebrew/opt/node@24/lib/libnode.137.dylib \
     -DPULP_ENABLE_GPU=ON -DPULP_BUILD_TESTS=ON
   ```
   Verify the linked dylib before trusting a local Three.js failure:
   ```bash
   otool -L build/test/web-compat/pulp-test-threejs-bridge | grep libnode
   ```

2. **gpu_surface MUST be passed to WidgetBridge** — The native GPU bridge only initializes when WidgetBridge receives a non-null GpuSurface pointer. Without it, Three.js gets no WebGPU device and the 3D canvas renders black. This is the `attach_gpu_surface()` call in the demo.

3. **Three.js fetched via FetchContent** — `PULP_HAS_THREEJS` is set automatically when GPU + tests are enabled.

## Failure Modes To Recognize

These three failures all present as "demo says `status: 'ready'` but the canvas is solid black/blank" — Three.js's `renderer.init()` uses `new Promise(async (resolve, reject) => {…})` which silently swallows any throw inside the executor, so errors do NOT propagate as rejections. Whenever you see a black canvas, suspect one of these:

1. **`createNativeAdapter` / `describeNativeAdapter` missing from `window.pulp.gpu`** (regression introduced by ad7175be7 in April 2026). `web-compat-canvas.js`'s `__ensurePulpGpuHelpers` calls `window.pulp.gpu.createNativeAdapter()`; if undefined, the entire native bridge silently falls back to no-op mocks. Fix: keep the `describeNativeAdapter` / `createNativeAdapter` accessors in `web-compat-document.js` alongside the mock-adapter accessors, and keep the native-aware `navigator.gpu.requestAdapter` that prefers `__describeNativeAdapterImpl` over the mock.

2. **`globalThis.requestAnimationFrame` recursing on `globalThis.window.requestAnimationFrame`** (RangeError: Maximum call stack size exceeded). In V8 the script-top `var window = {…}` becomes a property of globalThis, so a wrapper that lives on globalThis and calls `globalThis.window.X` resolves back to itself. Bind directly to the inner `__requestFrame__` helper (or capture `window.requestAnimationFrame` once at install time and call that local).

3. **`var depthStencil` missing in the `__createMockGPURenderPassEncoder` closure** in `web-compat-gpu-buffered.js`. `encoder.draw` / `encoder.drawIndexed` reference `depthStencil` to forward to `createBufferedDrawPayload`; if undeclared in the closure, the reference resolves as a free variable and throws ReferenceError as soon as the bridge is engaged. Add `var depthStencil = descriptor.depthStencilAttachment || null;` next to the other closure vars.

4. **Geometry arrives all-zero → the mesh collapses to a degenerate point.** Three.js's WebGPUBackend uploads vertex/index buffers with `createBuffer({mappedAtCreation:true})` → `new T(buf.getMappedRange()).set(array)` → `buf.unmap()`. The mock `__createMockGPUBuffer` in `web-compat-document-gpu-mock.js` previously returned `_bytes.buffer.slice(...)` from `getMappedRange()` (an independent COPY) with a no-op `unmap()`, so every mapped write was silently dropped — `buffer._bytes` stayed zero. The buffered-draw serializer ships `buffer._bytes` to native (`web-compat-canvas-gpu.js`), so the native draw received all-zero positions+indices and rasterized nothing while the render-pass clear still showed. Uniforms survived only because they use `queue.writeBuffer`, which writes `_bytes` directly. Fix: `getMappedRange()` hands back a standalone ArrayBuffer seeded from `_bytes`, records it, and `unmap()` copies each recorded range back into `_bytes`. Regression test: `[webcompat][gpu][mock][issue-3217]` round-trips a `mappedAtCreation` write through `_bytes`. **Tell-tale:** a render-pass clear color is visible but geometry is not, and a per-draw dump of the vertex/index buffer head bytes is all `00`.

5. **JS-guessed bind-group layout silently mismatches Three's `layout:"auto"` pipeline → the vertex stage reads zeroed uniforms.** The buffered path (`__gpuQueueDrawBufferedImpl`) used to build an EXPLICIT `BindGroupLayout` from visibility/types guessed in JS (`web-compat-gpu-buffered.js inferVisibilityFromShaders` regex-scans the WGSL), then create the pipeline with an explicit pipeline layout. Under the iOS-Sim `skip_validation` toggle, a layout mismatch does NOT raise an error — it just leaves bindings unfulfilled, so the vertex shader reads zero matrices and the cube degenerates even though every uniform byte uploaded correctly. Fix (mirrors the immediate `__gpuQueueDrawImpl` path): create the pipeline with `layout = nullptr` (auto), then build each bind group from `pipeline.GetBindGroupLayout(group_index)`. This makes the layout come from the real shader interface. The same fix repaired Three's sRGB output-conversion/composite pass, which has the same bind-group shape (sampler + sampled `texture_2d`). **Whenever you replay a serialized WebGPU pipeline, prefer auto layout + `GetBindGroupLayout()` over reconstructing the layout from a JS guess.**

### iOS AUv3 live-present gotchas

The iOS AUv3 GPU path (`PulpMetalPluginView` in `core/view/platform/ios/plugin_view_host_ios.mm`) is **not on `main`** — `main` routes iOS AUv3 to the CPU host — so it has no CI coverage. Hard-won facts:

- **The out-of-process `.appex` swallows logs.** `OS_LOG_DEFAULT` info/debug records are not persisted for extensions, so `simctl log show/stream` saw nothing. Pulp now routes `runtime::log_*` through a named os_log subsystem (`dev.pulp.runtime`); capture with `xcrun simctl spawn <UDID> log stream --level debug --predicate 'subsystem == "dev.pulp.runtime"'`. This is the only way to read an AUv3 extension's diagnostics. Env vars do NOT reach the appex — gate any temporary per-draw dump on a static frame counter, not `getenv`.
- **"Black canvas" was NOT the offscreen-Skia-fallback theory.** `SkiaSurface::begin_frame()` reports `presentable: yes` and all three offscreen-fallback branches fire 0 times; the HUD renders through the same presentable drawable, proving the present path works. Don't chase the offscreen fallback — verify it with the log first.
- **`skip_validation` on the Simulator is load-bearing.** It is enabled (with `allow_unsafe_apis`) on `TARGET_OS_SIMULATOR` in `gpu_surface_dawn.cpp` because Skia Graphite's own per-frame instanced draws emit `firstInstance>0` / `Invalid CommandBuffer` that the Sim's Metal SoftwareRenderer rejects, poisoning the queue. Removing it turns the WHOLE editor black, so you cannot use "remove skip_validation to surface errors" as a clean diagnostic — it kills Skia too. The downside is it ALSO suppresses real cube-pipeline errors (see failure mode 5).
- **iOS build gate:** Apple host-classification impls (`host_type_mac.mm`, `host_version_mac.mm`) must be gated `$<PLATFORM_ID:Darwin,iOS,tvOS,watchOS>`, not just `Darwin` — under an `iphonesimulator`/`iphoneos` cross-build `$<PLATFORM_ID>` is `iOS`, so a `Darwin`-only gate drops them and the iOS AUv3 fails to link `detect_host_version` / `current_auv3_wrapper_identifier`.
- **Repro loop:** build `PulpThreeJsDemo_HostApp_Embed` (`-DPULP_ENABLE_GPU=ON -DPULP_REQUIRE_GPU_FOR_SDK=ON`, iphonesimulator arm64), `simctl install` the `.app`, `simctl launch …threejsdemo.host` (auto-presents the editor), then `simctl io <UDID> screenshot`. The cube `.appex` resource `threejs/scene.js` is a plain bundle file you can hot-patch in the installed `.app` for JS-only experiments without a C++ rebuild. **Always visually inspect the screenshot** — the HUD frame counter changes the PNG bytes every frame, so an md5 diff is a false "it changed" signal.

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

For the full focused CTest slice, build the resource-test target first so
Catch2 does not leave a `NOT_BUILT` placeholder in the selected test set:

```bash
cmake --build build --target pulp-test-threejs-resources pulp-test-threejs-bridge pulp-threejs-native-demo -j8
ctest --test-dir build -R "threejs|Three.js|pulp_bundle_threejs_for_jsc_smoke" --output-on-failure
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
Capture mode is also the CI/headless smoke path: `--capture` sets
`WindowOptions::initially_hidden=true` and exits before `run_event_loop()`.
Keep future capture changes on that hidden path; do not add `show()` or
focus-stealing activation before the screenshot is written.

**Engine/GPU identity:** `--print-engine-identity` brings up a real V8
ScriptEngine + offscreen Dawn surface and prints a parseable
`PULP_ENGINE_IDENTITY_BEGIN…END` block (engine_type, runtime_version,
provider_kind, provider_path, pulp_has_v8, gpu_available, gpu_native_bridge,
gpu_backend, gpu_software). Use it to prove *which* V8 is linked (e.g. the
sealed v8-builder seal vs Homebrew libnode — see the engine skill's
sealed-provider (`FindV8.cmake`) section) and that the GPU is real hardware
(`gpu_backend=Metal`, `gpu_software=0`). The strict
`v8_provider_identity_strict` CTest (gated on
`PULP_VALIDATE_V8_PROVIDER_STRICT`) parses that block with no skip-pass and
then requires a non-empty `--demo cube --capture` PNG.

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

## Benchmark Mode

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
  `bufferDataBase64` lane, not the vertex-buffer lane. Don't
  read a zero there as a bug.

## Zero-Copy Decision Status

The zero-copy JS↔GPU decision was evaluated twice:

1. The first pass measured `ui-preview`'s oscilloscope + spectrogram —
   wrong workload, C++-driven, never exercises the upload path.
2. The follow-up benchmark measured Three.js particles — honest
   workload, 0.036% → 0.26% of frame budget at 1K → 100K points.

Both landed NO-GO; the particle benchmark supersedes the earlier verdict. See
`planning/zero-copy-decision-1-re-evaluation-2026-04-20.md`. The
new `PerfCounters` fields (`base64_decode_total_us`,
`gpu_buffer_upload_count`, `gpu_buffer_bytes_resident_peak`) stay
merged for future workload-specific re-evaluations.

## JSC iOS lane

Pulp ships Three.js inside an AUv3 `.appex` on iOS via JSC (system framework, no V8 build). The full bring-up is in `planning/2026-05-29-ios-d3b-threejs-webgpu-program.md`.

**Path summary:**
- JSC runs `three.webgpu.js` as a Rollup-bundled IIFE (NOT ESM — JSC's ESM module-loader API is private on iOS, App Store rejection risk).
- The bundle script `tools/scripts/bundle_threejs_for_jsc.mjs` is a pure-Node ESM-to-IIFE transform; no Rollup runtime dep.
- `tools/cmake/PulpAuv3.cmake` runs the bundler at .appex build time (POST_BUILD step gated on `find_program(node)`).
- `core/view/src/threejs_resources_apple.mm` loads the embedded bundle from `Resources/threejs/three.iife.js` at runtime via `NSBundle`.
- WidgetBridge's `__gpu*Impl` family is engine-agnostic by construction — all 11 functions register through `engine_.register_function(...)` which works for V8 or JSC.

**Perf delta vs V8 macOS:**
- JSC interpreter measured at 230 FPS @ 2000 cubes on iPad Pro 11" 3rd-gen (scene-graph math only, no GPU). That's 80× the user's 30-FPS threshold with 14ms GPU budget remaining.
- JSC interpreter is 3-10× slower than JIT V8 on hot loops, but the iPad GPU runs Metal at full speed regardless of jitless JS.
- Three.js's tight `Object3D.updateMatrixWorld` traversal is actually MORE JIT-friendly than naive hand-rolled JS — Three.js r149 outperformed the earlier hand-rolled cube bench at every scene size.

**Don't reach for V8 on iOS unless a measurable feature gap surfaces** — the build is 3-5h, needs Chromium depot_tools + 50GB workspace, and JSC's jitless interpreter already clears the bar.

**The `presentable` flag** is the load-bearing signal that distinguishes a real swapchain-backed canvas from a silent offscreen texture. Both `__gpuCanvasConfigureImpl` and `__gpuCanvasDescribeCurrentTextureImpl` surface it. If a JSC-backed Three.js demo shows a black editor pane, grep the log for `presentable=false` before debugging anything else.

### OrbitControls on the iOS JSC lane (touch orbit/pinch)

The macOS native demo imports `OrbitControls` from `three/addons` (ESM). The iOS lane runs the IIFE bundle, which does NOT include addons by default. To ship OrbitControls on iOS:

- **Bundle it.** `tools/scripts/bundle_threejs_for_jsc.mjs` takes an optional `--orbit-controls <OrbitControls.js>`. It builds a synthetic esbuild entry (`export * from three.webgpu.js` + `export { OrbitControls }`) with an `alias: { three: <three.webgpu.js> }` so the addon's `import { Controls, MOUSE, TOUCH, ... } from 'three'` resolves to the SAME module instance (no duplicate three — verify the bundle grows only ~30KB, not ~2×). `three.webgpu.js` re-exports `Controls`/`MOUSE`/`TOUCH`/`Spherical`/etc. from `three.core.js`, which is exactly OrbitControls' import set. `tools/cmake/PulpAuv3.cmake` passes `--orbit-controls` for the demo. After it lands, `THREE.OrbitControls` is a global. **CMake gotcha:** editing `PulpAuv3.cmake` needs a reconfigure (`cmake -S . -B <dir> -G Xcode ...`) before the POST_BUILD bundler picks up the new flag; a bare incremental build reuses the baked command.
- **Pass OrbitControls the RAW canvas element, not the renderer's `PulpCanvas` wrapper.** OrbitControls calls `domElement.setPointerCapture`, `.getBoundingClientRect`, `.getRootNode`, and `.ownerDocument.addEventListener` — all provided by Pulp's native `HTMLCanvasElement` shim (web-compat-element.js) but NOT re-exposed by the minimal renderer wrapper. The renderer keeps the wrapper; OrbitControls gets `canvasEl`.
- **The load-bearing fix: pointer events must reach `document` listeners.** OrbitControls registers `pointerdown` on the canvas, then on first press MOVES its `pointermove`/`pointerup` listeners onto `domElement.ownerDocument` (= the `document` global). In Pulp, `document` owns a SEPARATE listener map and the element bubble walk (`_dispatchEvent`, `_parentElement` chain) never reaches it — so canvas moves fired only the canvas's own listeners and OrbitControls saw the press but no moves (camera frozen). `__dispatch__` now fans pointer events to `document.dispatchEvent` explicitly. Tell-tale to confirm: a JS `document.addEventListener('pointermove', …)` counter stays 0 while the canvas counter increments. Regression-guarded by `[view][bridge][pointer][issue-3217]` in `test_widget_bridge.cpp` (headless JSC lane; the iOS AUv3 GPU path has no CI).
- **Touch reaches JS only on the GPU editor path after the `PulpMetalPluginView` touch handlers** (see the `ios` skill). Wire `new THREE.OrbitControls(camera, canvasEl)` with `enableDamping`, `enablePan=false`, `enableZoom` (pinch), `enableRotate` (drag), and call `controls.update()` every frame.
