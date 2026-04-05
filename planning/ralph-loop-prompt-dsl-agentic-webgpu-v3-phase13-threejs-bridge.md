"Implement Phase 13: Three.js / WebGPU JavaScript bridge for the v3 track.

References:
- planning/dsl-agentic-validation-webgpu-audit-v3.md
- planning/ralph-loop-prompt-dsl-agentic-webgpu-v3-phase10-jsengine.md
- planning/ralph-loop-prompt-dsl-agentic-webgpu-v3-phase11-webgpu.md
- planning/v3-phase13-readiness.md
- core/view/include/pulp/view/script_engine.hpp
- core/view/src/script_engine.cpp
- core/render/src/gpu_surface_dawn.cpp
- core/view/js/web-compat.js
- https://www.w3.org/TR/webgpu/ (WebGPU spec — defines the JS API surface)
- https://github.com/nicbarker/mystral-native (reference: QuickJS + Dawn + Skia integration)

PROVEN REFERENCE IMPLEMENTATIONS:
- react-native-webgpu (~/Code/react-native-webgpu):
  JSI HostObjects wrapping WebGPU C++ objects, PlatformContext abstraction over Metal/Vulkan,
  thread-safe surface registry, manual frame scheduling via context.present(),
  worklet serialization for cross-context object passing.
  This is the closest existing implementation to what Pulp Phase 13 builds.
- react-native-skia (~/Code/react-native-skia):
  Skia + native UI compositing via dedicated SkiaView, GPU context sharing via
  GrDirectContext/MTLDevice, JSI HostObject binding pattern for C++ → JS exposure,
  frame sync with native refresh rate, platform backends (Metal on iOS, OpenGL/Vulkan on Android).
  Key pattern: Skia and WebGPU can share a GPU device/context.
- Three.js WebGPU renderer (~/Code/three.js):
  2600-line WebGPUBackend.js, node-based material system (TSL) that generates WGSL,
  extensive resource caching (bind groups, samplers, pipelines), async pipeline creation,
  device loss recovery. Uses ~20 core WebGPU interfaces for basic rendering.

GOAL:
Build a native WebGPU JavaScript API bridge so that Three.js (and other WebGPU-based JS libraries)
can run natively inside Pulp's JS engine against Dawn — no browser, no WebView. JavaScript 3D scene
code renders at full GPU speed, composited into the same surface as the 2D Skia UI.

READINESS SNAPSHOT (2026-04-02):
- The Phase 10 engine abstraction is merged on `main`.
- `quickjs` and `jsc` are configure/build/test-proven on current `main`.
- `v8` is now proven on a real machine path when explicit external inputs are supplied (`V8_INCLUDE_DIR`, `V8_LIB_DIR`, and `V8_LIBRARY_PATH` for non-`v8_monolith` providers such as Homebrew Node's `libnode`).
- The current `JsEngine` capability floor is only partially raised:
  - typed arrays are now a real tested capability on `jsc`
  - a first native-backed host-object descriptor seam is now tested across the proven backends
  - a first native promise-returning seam is now tested across the proven backends
  - a first browser-style bridge smoke slice is now tested across the proven backends through `ScriptEngine`
  - `v8` now also has a real configure/build/test proof path with typed-array coverage when built through that explicit provider contract
  - deferred native async settlement is still not implemented as a held native resolver
  - full opaque wrapper / proxy-style HostObjects are still not implemented or proven
- Read `planning/v3-phase13-readiness.md` before treating this phase as ready to implement.

CURRENT IMPLEMENTATION TRUTH (current local slices):
- The current bridge slices stay narrow and truthful:
  - `HTMLCanvasElement.getContext('2d')` is backed by the existing `CanvasWidget` bridge
  - `HTMLCanvasElement.getContext('webgpu')` now returns a mock `GPUCanvasContext`
  - `window.pulp.gpu.getInfo()` exposes native GPU availability/backend truth
  - `navigator.gpu` is now present as a browser-shaped entry object
  - `navigator.gpu.getPreferredCanvasFormat()` returns `"bgra8unorm"`
  - `navigator.gpu.requestAdapter()` returns a real JS `Promise` that resolves to a mock adapter object
  - a first mock object graph now exists behind that entry surface: `GPUAdapter`, `GPUDevice`, `GPUQueue`, `GPUBuffer`, `GPUTexture`, `GPUCommandEncoder`, and `GPUCanvasContext`
  - core WebGPU globals are now present for bitmask-driven browser code: `GPUBufferUsage`, `GPUTextureUsage`, `GPUMapMode`, `GPUShaderStage`, and `GPUColorWrite`
  - browser helper shims from the older monolith are now present in the split prelude too: `performance.now`, storage, `Image`, `atob`/`btoa`, `crypto.getRandomValues`, `TextEncoder`, `TextDecoder`, and `structuredClone`
  - asset-backed browser helpers now exist for bundled/local resources: `window.pulp.fetch`, `Response`, `Blob`, and `URL.createObjectURL`, backed by `AssetManager`
- These slices are useful because they prove the browser-style surface can attach to native rendering and GPU facts, expose the first synchronous global constants and early object graph that real WebGPU libraries expect, and now cover common browser helper APIs that loader code tends to assume, without claiming a real Dawn-backed `GPUDevice`, WGSL execution, or Three.js compatibility yet.
- UTF-8 JSON payloads now have explicit focused proof across the native asset bridge, including byte-length preservation and canonical `application/json;charset=utf-8` data-URI handling.
- DOM `id` and native widget `_id` are not yet the same contract; do not overclaim DOM/native parity until that seam is intentionally raised.
- Promise-returning seams are currently proven only at the "real JS Promise object exists" level. The stock backend wrappers do not yet provide a portable cross-backend guarantee that settled `.then(...)` callbacks are observable from synchronous evaluation loops, so do not overclaim resolved adapter/device semantics yet.

ACTIVE EXECUTION ORDER (2026-04-02):
1. Keep Phase 13 scoped to truthful browser-shaped entry slices until the first real WebGPU object graph is ready.
2. Prefer narrow bridge increments that can be tested locally in `pulp-test-web-compat-prelude` / `pulp-test-widget-bridge`.
3. Keep the draft PR [#103](https://github.com/danielraffel/pulp/pull/103) and `planning/v3-phase-status.md` aligned with the real branch state after each landed slice.
4. Do not reopen Phase 4 or Phase 5 here; their parked licensing disposition stays tracked in [#89](https://github.com/danielraffel/pulp/issues/89).
5. Phase 14 must explicitly account for both:
   - parked Phases 4/5 via [#89](https://github.com/danielraffel/pulp/issues/89)
   - deferred Windows/Linux native WebView parity via [#92](https://github.com/danielraffel/pulp/issues/92)

CONCEPT:
Three.js's WebGPU renderer calls the standard WebGPU JavaScript API. This phase implements those
JS API interfaces as native C++ HostObjects that call through to Dawn's WebGPU API. Three.js thinks
it's running in a browser with WebGPU support, but it's hitting the GPU directly via Dawn.

This follows the same architectural pattern proven by react-native-webgpu (JSI HostObjects → Dawn)
and aligns with Pulp's philosophy: web-like developer experience without the browser runtime.

DEPENDENCIES:
- Requires Phase 10 (JS engine abstraction) — V8 strongly preferred for Three.js perf and now the explicit recommended implementation path for this phase.
  Phase 10 MUST expose a fast native-object binding API (like JSI HostObjects) that Phase 13
  uses to wrap Dawn objects. QuickJS's C API can do this but V8's JIT makes it viable for
  real Three.js scenes.
- Requires Phase 11 (WebGPU exploration) — must understand Dawn device lifecycle, compute
  capabilities, and surface management. Phase 11's Dawn device MUST be shareable with
  Skia Graphite (same wgpu::Device used for both 2D and 3D rendering).
- Benefits from Phase 6 (multi-window) — 3D views may be in their own windows.

CAN RUN IN PARALLEL:
- no; depends on Phases 10 and 11

BINDING ARCHITECTURE (modeled on react-native-webgpu):

```
Three.js (JavaScript) — runs in Pulp's JS engine (V8 preferred)
  │
  ├── new THREE.WebGPURenderer({ canvas: pulpCanvas })
  │     calls standard WebGPU JS API
  │
  ▼
HostObject Binding Layer (C++)
  │
  │  Each WebGPU JS interface = one C++ HostObject class:
  │  ├── GPUHostObject<wgpu::Device>     → wraps Dawn device
  │  ├── GPUHostObject<wgpu::Buffer>     → wraps Dawn buffer
  │  ├── GPUHostObject<wgpu::Texture>    → wraps Dawn texture
  │  ├── GPUHostObject<wgpu::RenderPipeline> → wraps Dawn pipeline
  │  └── ... (~20 core interfaces)
  │
  │  Pattern: get/set property → Dawn accessor
  │           method call → Dawn method with TypedArray/ArrayBuffer marshaling
  │           destructor → release Dawn object ref
  │
  ▼
PlatformContext abstraction (from react-native-webgpu pattern)
  │
  │  Abstracts surface creation per platform:
  │  ├── macOS: CAMetalLayer surface via Dawn
  │  ├── Windows: HWND surface via Dawn
  │  ├── Linux: Vulkan surface via Dawn
  │  └── Surface registry: thread-safe map of active surfaces
  │
  ▼
Dawn (native WebGPU C++ implementation)
  │
  │  SHARED wgpu::Device with Skia Graphite
  │  (same GPU device renders both 2D widgets and 3D content)
  │
  ▼
Metal / Vulkan / D3D12
```

NON-NEGOTIABLES:
- No WebView, no browser engine, no Chromium. This is native JS → native GPU.
- The bindings must follow the W3C WebGPU spec closely enough that Three.js's WebGPU renderer
  works unmodified (or with minimal patching).
- 3D rendering must composite into the same GPU surface as 2D Skia UI.
- V8 is the recommended engine. QuickJS will work for simple scenes but will struggle with
  complex Three.js workloads (Three.js library is ~1MB of JS to parse and JIT).
  This is a target state, not current stock-checkout proof.
- The bridge is optional: build flag PULP_BUILD_WEBGPU_JS_BRIDGE.
- Do not fork or modify Three.js. The goal is API compatibility.
- Dawn device MUST be shared between Skia Graphite and the WebGPU JS bridge.
  Do not create a second GPU device.
- Treat the currently landed host-object descriptor seam as the first floor, not the final API.
  The real bridge still needs opaque wrapper objects, and may still need held native async promise plumbing if the first Dawn-backed bridge slices prove the current microtask-wrapped promise seam is insufficient.

DELIVERABLES:

### 1. WebGPU JS API Bindings — HostObject Layer

Implement as C++ HostObject classes (following react-native-webgpu's proven pattern).
Each class wraps a Dawn C++ object and exposes the W3C WebGPU JS API.

Priority tiers based on Three.js WebGPUBackend.js analysis:

TIER 1 — Required for any Three.js rendering:
- navigator.gpu / GPU — entry point, requestAdapter()
- GPUAdapter — requestDevice(), features, limits
- GPUDevice — all createX() methods, queue property, features, limits, lost promise, destroy()
- GPUQueue — submit(), writeBuffer(), writeTexture(), copyExternalImageToTexture(), onSubmittedWorkDone()
- GPUBuffer — mapAsync(), getMappedRange(), unmap(), destroy(), size, usage, mapState
- GPUTexture — createView(), destroy(), width, height, format, usage, dimension, mipLevelCount, sampleCount
- GPUTextureView — (read-only properties: format, dimension, aspect, etc.)
- GPUSampler — (immutable after creation)
- GPUShaderModule — getCompilationInfo() for error reporting
- GPUBindGroupLayout / GPUBindGroup — (immutable after creation)
- GPUPipelineLayout — (immutable after creation)
- GPURenderPipeline — getBindGroupLayout()
- GPUCommandEncoder — beginRenderPass(), beginComputePass(), copyBufferToBuffer(),
    copyTextureToBuffer(), copyBufferToTexture(), finish()
- GPURenderPassEncoder — setPipeline, setBindGroup, setVertexBuffer, setIndexBuffer,
    setViewport, setScissorRect, setStencilReference, draw, drawIndexed, end
- GPUCommandBuffer — (immutable after creation)
- GPUCanvasContext — configure(), getCurrentTexture(), present() (the present() call
    follows react-native-webgpu's pattern of explicit frame submission)

TIER 2 — Required for real 3D scenes (depth, shadows, MSAA, compute):
- GPUComputePipeline — getBindGroupLayout()
- GPUComputePassEncoder — setPipeline, setBindGroup, dispatchWorkgroups,
    dispatchWorkgroupsIndirect, end
- GPUQuerySet — for occlusion queries (Three.js uses these for visibility)
- GPURenderBundle / GPURenderBundleEncoder — for render bundle optimization
- drawIndirect / drawIndexedIndirect on GPURenderPassEncoder

TIER 3 — Optimization and advanced features:
- createRenderPipelineAsync / createComputePipelineAsync — async pipeline creation
- Device feature detection — device.features.has() for optional capabilities
- Device limits queries — maxUniformBufferBindingSize, maxStorageBufferBindingSize, etc.
- uncapturederror event on device — async GPU error monitoring

### 2. DOM Shims — Minimal Browser API Surface

Based on empirical analysis of Three.js WebGPUBackend.js, WebGPUTextureUtils.js,
and the node-based material system (WGSLNodeBuilder.js), Three.js requires:

REQUIRED:
- HTMLCanvasElement shim:
  - width, height properties (track Pulp view dimensions)
  - getContext('webgpu') → returns GPUCanvasContext HostObject
  - addEventListener('resize', ...) for resize handling
  - toBlob() / toDataURL() for screenshot capture (bridge to Pulp's capture_png)
- requestAnimationFrame / cancelAnimationFrame:
  - bridge to Pulp's frame loop (same loop that drives Skia rendering)
  - frame callback receives high-resolution timestamp
  - manual scheduling pattern from react-native-webgpu: context.present() signals frame end
- Image / ImageBitmap:
  - Image() constructor with src, onload, onerror
  - createImageBitmap() for async image decoding
  - bridge to Pulp's AssetManager (Phase 9) for actual image loading
  - support data: URIs for inline texture data
- Performance.now() — high-resolution timer (use std::chrono)
- TextEncoder / TextDecoder — for WGSL string handling (ArrayBuffer ↔ string)

NEEDED FOR LOADERS:
- fetch() — minimal implementation for loading local bundled assets:
  - support file:// and pulp:// URLs (bridge to asset system)
  - return Response with arrayBuffer(), text(), json(), blob() methods
  - do NOT implement full HTTP — this is for bundled resources only
- URL / URL.createObjectURL / URL.revokeObjectURL — for blob URL handling
- Blob — for binary data handling in loaders

NOT NEEDED (Three.js WebGPU renderer does not use these):
- DOM manipulation (createElement, appendChild, etc.) — not used by renderer
- CSS / style / layout — not used by renderer
- WebSocket / WebRTC — not used by renderer
- localStorage / sessionStorage — not used by renderer
- Worker / SharedWorker — not used (Three.js workers are optional)

### 3. Rendering Integration — 2D/3D Compositing

Based on react-native-skia's compositing patterns:

APPROACH: Dedicated render region (not shared surface)
- Pulp layout system (Yoga) allocates a rectangular region for the 3D view
- That region gets its own Dawn render target (texture)
- Three.js renders into that render target via the GPUCanvasContext bridge
- Skia composites the 3D render target into the final 2D scene as an image
- This avoids ordering/blending complexity of interleaving 2D and 3D draw calls

Implementation:
- ThreeJSView widget (subclass of View):
  - owns a Dawn texture as the render target
  - GPUCanvasContext.getCurrentTexture() returns this texture
  - GPUCanvasContext.present() signals frame complete → Skia composites it
  - resize events update the texture dimensions
- Shared Dawn device:
  - Skia Graphite's wgpu::Device is passed to the WebGPU JS bridge
  - both renderers submit to the same wgpu::Queue
  - frame ordering: Skia renders 2D → Three.js renders 3D → Skia composites 3D texture → present
- Depth buffer:
  - Three.js manages its own depth texture within its render target
  - no depth interaction between 2D Skia content and 3D content

### 4. Audio Plugin Examples Using Three.js

These examples validate the bridge works end-to-end for real audio plugin use cases:

EXAMPLE 1: 3D Spectrum Analyzer
- Audio input → FFT (via AudioBridge) → frequency bins published to UI via TripleBuffer
- Three.js scene: array of 3D bars (BoxGeometry) with heights driven by FFT magnitudes
- Camera orbits the spectrum
- Native Pulp controls: gain knob, FFT size selector, color scheme picker
- Validates: AudioBridge → JS data flow, real-time geometry updates, hybrid 2D+3D layout

EXAMPLE 2: Reactive Particle Visualizer
- Audio input → beat detection + RMS level → published to UI
- Three.js scene: particle system (PointsMaterial or compute-based) that reacts to audio
  - particles expand on beats, color shifts with RMS, rotation follows tempo
- Native Pulp controls: particle count, sensitivity, color palette
- Validates: compute shaders through the bridge, high particle counts, performance under load

EXAMPLE 3: Room Reverb Visualizer
- Reverb plugin with IR (impulse response) visualization
- Three.js scene: 3D room mesh with ray-traced reflection paths visualized as lines
  - early reflections as bright lines, late reverb as diffuse cloud
- Native Pulp controls: room dimensions, absorption coefficients, source/listener position (XYPad)
- Validates: dynamic mesh generation, line rendering, parameter-driven 3D scene updates

EXAMPLE 4: 3D Waveform Ribbon
- Audio input → waveform samples published to UI
- Three.js scene: scrolling 3D ribbon/surface where Z-axis is time, Y-axis is amplitude,
  color encodes frequency content
- Native Pulp controls: scroll speed, ribbon width, color mapping
- Validates: streaming geometry updates, large vertex buffers, texture-based color lookup

### 5. Performance Benchmarks

Test matrix:
| Benchmark | V8 | QuickJS | Chrome WebGPU | Notes |
|-----------|-----|---------|---------------|-------|
| Three.js init (parse + create renderer) | ms | ms | ms | Library parse time is key for QuickJS |
| Spinning cube (60fps target) | fps/ms | fps/ms | fps/ms | Basic rendering overhead |
| 1000 instanced cubes | fps/ms | fps/ms | fps/ms | Draw call batching |
| 10K particles (compute) | fps/ms | fps/ms | fps/ms | Compute pipeline through bridge |
| Texture upload (2048x2048) | ms | ms | ms | writeTexture / copyExternalImageToTexture |
| Buffer readback (mapAsync) | ms | ms | ms | Async readback overhead |
| Memory: scene create/destroy cycle | MB | MB | MB | Leak detection |
| Startup: cold start to first frame | ms | ms | ms | End-to-end init time |

Expected outcome: V8 should be within 1.5x of Chrome for most benchmarks.
QuickJS will likely be 5-20x slower for JS-heavy workloads (Three.js scene graph
traversal, material compilation) but similar for GPU-bound workloads.

### 6. Compatibility Testing

Three.js feature matrix (test with Three.js r170+ WebGPU renderer):
| Feature | Status | Notes |
|---------|--------|-------|
| WebGPURenderer init | | Adapter → device → context.configure |
| Basic geometry (Box, Sphere, Plane) | | BufferGeometry → vertex/index buffers |
| MeshBasicMaterial | | Simplest material, no lighting |
| MeshStandardMaterial (PBR) | | Node-based, generates WGSL via TSL |
| MeshPhysicalMaterial | | Advanced PBR (clearcoat, iridescence) |
| DirectionalLight / PointLight | | Uniform buffer updates per frame |
| Shadow mapping | | Depth texture, comparison sampler |
| Environment maps | | Cube texture sampling |
| Texture loading (PNG/JPG) | | Image shim → createTexture → writeTexture |
| MSAA (4x) | | Multi-sample render target + resolve |
| Post-processing (EffectComposer) | | Render-to-texture → fullscreen quad |
| Compute shaders (GPUComputationRenderer) | | Compute pipeline dispatch |
| Instanced rendering | | Instance attributes in vertex buffer |
| Skinned mesh (skeletal animation) | | Storage buffer for bone matrices |
| OrbitControls | | Pointer event shims |
| GLTF loader | | fetch shim for binary loading |
| Render bundles | | Pre-recorded command sequences |

Fill in during implementation. Document workarounds needed.

### 7. Tests

Unit tests (per-interface):
- Each HostObject: create, inspect properties, call methods, destroy
- Buffer: create → writeBuffer → mapAsync → getMappedRange → verify data → unmap
- Texture: create → writeTexture → createView → verify format/dimensions
- Shader: createShaderModule with valid WGSL → getCompilationInfo success
- Shader: createShaderModule with invalid WGSL → getCompilationInfo errors
- Pipeline: create render pipeline → getBindGroupLayout → create bind group
- Command encoding: full draw call sequence → submit → no GPU errors
- Canvas context: configure → getCurrentTexture → present → frame visible

Integration tests:
- Three.js smoke: import Three.js, create scene/camera/renderer, render one frame, screenshot verify
- Spinning cube: 60 frames of animation, verify no crashes or GPU errors
- Hybrid UI: Pulp 2D widgets + Three.js 3D in same window, screenshot verify both render
- Audio-reactive: AudioBridge data flows to Three.js scene, geometry updates correctly
- Resource cleanup: create and destroy 100 scenes, verify memory stable

Performance regression tests:
- Spinning cube must hold 60fps on reference hardware
- Scene create/destroy cycle must not leak >1MB over 100 iterations
- Buffer upload latency must be <2ms for 1MB buffer

### 8. Threading Model

Following react-native-webgpu's proven pattern:
- JS execution: on the JS engine thread (same thread as Pulp's UI scripting)
- Dawn device operations: on the JS thread (Dawn is thread-safe for single-device use)
- GPU command submission: device.queue.submit() is non-blocking, returns immediately
- Async readback: buffer.mapAsync() returns a Promise, resolved when GPU work completes
- Frame loop: requestAnimationFrame callback fires on the JS thread, aligned with Pulp's
  render loop (typically vsync-driven 60fps or display refresh rate)
- Skia rendering: on the same thread, before or after Three.js rendering per frame
- No multi-threaded GPU submission in this phase. Keep it simple.

TOP RISKS:
- WebGPU JS API surface is large. Prioritize Tier 1 interfaces first, add Tier 2/3 incrementally
  based on which Three.js features are needed by the examples.
- Three.js library parse time in QuickJS may be unacceptable (>1s). V8 mitigates this.
  Fallback: pre-parse/snapshot Three.js if QuickJS must be supported.
- Compositing 2D Skia + 3D render target has GPU synchronization requirements — must ensure
  Three.js render completes before Skia composites the texture. Use Dawn's onSubmittedWorkDone()
  or careful command buffer ordering.
- Dawn device sharing between Skia Graphite and the WebGPU JS bridge may surface resource
  contention. Test with simultaneous 2D animation + 3D rendering.
- Three.js node material system (TSL) generates WGSL at runtime — shader compilation latency
  may cause frame hitches. Use async pipeline creation (createRenderPipelineAsync) where possible.
- GPU memory management: Three.js and Skia both allocate from the same Dawn device.
  Monitor total GPU memory and document limits.

ACCEPTANCE:
- Three.js WebGPU renderer runs natively in Pulp without a browser or WebView
- All 4 audio plugin examples render correctly (screenshot-verified)
- The 3D spectrum analyzer example works end-to-end: audio in → FFT → Three.js → screen
- Hybrid 2D+3D layout works: native Pulp widgets and Three.js content in the same window
- Performance is documented and V8 is within 2x of Chrome for GPU-bound workloads
- The bridge is optional (build flag) and does not affect non-3D Pulp users
- V8 is recommended; QuickJS works for simple scenes with documented limitations
- All Tier 1 + Tier 2 WebGPU interfaces pass unit tests
- No GPU memory leaks over 100 create/destroy cycles
- docs describe this as native Three.js support, not WebView

Use repo prompt when searching the codebase

ONLY WHEN ALL CONDITIONS ARE MET:
Output exactly: <promise>DONE</promise>

IF STUCK:
- After 20 iterations, document in LEARNINGS.md:
- What is blocked
- Why
- What was attempted
- What assumption may be wrong" --completion-promise "DONE" --max-iterations 120
