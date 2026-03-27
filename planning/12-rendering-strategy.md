# GPU Rendering and Scripting Strategy

## 1. Overview

Pulp's UI rendering uses native GPU rendering driven by a JavaScript scripting layer. No browser. No web view. The JS engine (QuickJS/V8/JavaScriptCore) runs in-process alongside the GPU renderer.

The rendering stack is built on Dawn (Google's WebGPU implementation), Skia Graphite (next-generation GPU-accelerated 2D rendering), and QuickJS (lightweight embeddable JS engine). This combination delivers the developer experience of web development -- JavaScript, hot-reload, CSS-like theming -- without the browser runtime baggage.

---

## 2. Technology Stack

- **Dawn** (Google's WebGPU implementation, BSD-3-Clause) -- cross-platform GPU abstraction targeting Metal, D3D12, Vulkan, and WebGPU
- **Skia Graphite** (BSD-3-Clause) -- Skia's next-generation GPU-accelerated 2D rendering backend, built on top of Dawn/WebGPU
- **QuickJS** (MIT) -- lightweight embeddable JS engine for scripting. V8 or JavaScriptCore as alternatives for platforms where startup time matters less than peak performance.

---

## 3. Why This Stack

- **Single GPU API across all targets.** WebGPU provides one API that targets Metal, D3D12, Vulkan, AND WebGPU in browsers. No per-platform rendering code.
- **Future-proof 2D rendering.** Skia Graphite is the future of Skia's GPU rendering, used by Chrome, Android, and Flutter. It is actively maintained by Google and designed for modern GPU architectures.
- **Tiny scripting engine.** QuickJS is ~210KB, has fast startup, is MIT licensed, and is perfect for plugin UIs where binary size and instantiation speed matter.
- **Permissive licensing.** All components are BSD-3-Clause or MIT. No GPL, no LGPL, no copyleft concerns for commercial plugin distribution.
- **True cross-platform.** The same rendering code works on desktop, mobile, AND web via WASM + WebGPU. One codebase, every target.
- **Independent validation.** This is the same stack iPlug3's MGFX chose independently, confirming the approach is sound for audio plugin UIs.

---

## 4. Architecture

```
Plugin Binary
+-- C++ Core (DSP, state, format adapters)
|     +-- CLI interface
|     +-- MCP server interface
|
+-- Rendering Engine (pulp-render)
|     +-- Dawn (WebGPU API)
|     |     +-- Metal / D3D12 / Vulkan / WebGPU backend
|     +-- Skia Graphite (2D rendering on Dawn)
|     +-- JS Engine (QuickJS / V8 / JSC)
|           +-- UI code (.js/.ts files)
|                 +-- Widget definitions
|                 +-- Layout
|                 +-- Theme/design tokens
|                 +-- Animations
|
+-- Platform Integration
      +-- Native window (NSWindow/HWND/X11/UIView)
      +-- GPU surface (MTKView/SwapChain/VkSurface)
      +-- Native dialogs, menus, clipboard
```

The C++ core handles DSP, plugin format adaptation (VST3/AU/CLAP/AAX), and state management. The rendering engine owns the GPU pipeline and the JS runtime. Platform integration provides the native window and OS services. These three layers communicate through well-defined C++ interfaces -- the JS layer never touches DSP code directly, and the DSP layer never touches rendering.

---

## 5. JS Scripting Layer

UI is defined in JavaScript/TypeScript files loaded at runtime. These files are NOT compiled into the plugin binary -- they live on disk and are hot-reloadable.

The JS API provides:

- **Canvas drawing** -- direct access to Skia drawing primitives (paths, text, images, gradients, effects)
- **Widget creation** -- instantiate and compose UI widgets (knobs, sliders, buttons, meters, graphs)
- **Layout** -- flex-like layout system for positioning and sizing widgets
- **Theming** -- read and apply design tokens, switch themes at runtime
- **Event handling** -- mouse, keyboard, touch, drag, scroll, parameter changes
- **Parameter binding** -- two-way binding between UI controls and plugin parameters
- **Animation** -- frame-rate-independent animations with easing functions

### Engine Selection

The JS engine is platform-adaptive:

| Engine | Use Case |
|--------|----------|
| QuickJS | Default. Fast startup (~1ms), tiny footprint (~210KB), MIT licensed. Best for typical plugin UIs. |
| V8 | When peak JS execution performance matters more than binary size or startup time. BSD-3-Clause. |
| JavaScriptCore (JSC) | On Apple platforms, for native integration and JIT performance. Already present on macOS/iOS. |

The JS code does NOT run in a browser. It runs in an embedded engine alongside the GPU renderer, with direct access to native APIs exposed through C++ bindings.

---

## 6. Hot-Reload Workflow

1. Developer edits a `.js` or `.ts` UI file
2. File watcher detects the change
3. JS engine reloads the UI module
4. GPU re-renders with new UI code
5. Audio keeps playing, parameters stay in place
6. Typical reload time: < 100ms

This means UI iteration happens at the speed of saving a file, not at the speed of recompiling C++. The audio engine is completely unaffected by UI reloads -- the DSP thread never pauses, parameters retain their values, and automation continues uninterrupted.

For TypeScript workflows, a background transpiler watches `.ts` files and produces `.js` output. The file watcher monitors the `.js` output, so the hot-reload chain is: save `.ts` -> transpile -> reload `.js` -> re-render.

---

## 7. Design Token System

All visual properties are stored as structured JSON:

- **Color tokens** (36+) -- background, surface, primary, secondary, accent, error, warning, success, and their variants, all defined in OKLCH for perceptual uniformity
- **Typography** -- font families, sizes, weights, line heights, letter spacing
- **Spacing** -- padding, margins, gaps at multiple scales
- **Widget geometry** -- knob diameters, slider track widths, button heights, border radii
- **Effects** -- shadows, glows, blur radii, opacity levels

### Widget Style Modes

Widgets support multiple visual modes, all driven by tokens:

| Widget | Modes |
|--------|-------|
| Knob | arc, filled, notched, glossy |
| Button | flat, raised, outlined, beveled, glossy |
| Toggle | pill, checkbox, rocker |
| Slider | track, filled, bipolar |
| Meter | bar, segmented, analog |

### AI-Tool-Friendly

Any tool that reads and writes JSON can update the design. There is no proprietary format, no binary encoding, no special tooling required. An LLM, a design tool, or a simple script can modify tokens and see the result immediately via hot-reload.

### Token Export Targets

Tokens export to:

- **JSON** -- canonical format, consumed by the JS UI layer
- **CSS variables** -- for web/WASM target and documentation
- **C++ headers** -- for compile-time access in performance-critical paths
- **GPU shader uniforms** -- for post-processing effects
- **OKLCH** -- perceptually uniform color space for all color operations

One design language applies across multiple plugins. Change the token file, and every plugin using that token set updates its appearance.

---

## 8. Audio Visualization

- **Lock-free ring buffers** for audio thread to UI data transfer. The audio thread writes, the UI thread reads. No mutexes, no priority inversion, no glitches.
- **STFT processing abstractions** for spectral analysis. Configurable window size, overlap, and windowing function.
- **Metering ballistics** -- peak, RMS, VU, K-system (K-12, K-14, K-20). Configurable attack/release times and integration windows.
- **Waveform display primitives** -- scrolling waveform, static waveform, triggered oscilloscope view.
- **Spectral/frequency display primitives** -- spectrum analyzer, spectrogram, waterfall.
- All visualizations run at GPU frame rate. Data arrives from the audio thread without blocking. The UI thread consumes whatever data is available at each frame -- if a frame is missed, the visualization smoothly catches up rather than stuttering.

---

## 9. Post-Processing Effects

Full-screen GPU shader effects applied to the rendered UI:

- **Bloom** -- glow around bright UI elements (meters, active indicators)
- **Blur** -- gaussian blur for depth-of-field or frosted-glass effects
- **CRT simulation** -- scanlines, curvature, phosphor glow for vintage aesthetics
- **Vignette** -- darkened edges for visual focus
- **Chromatic aberration** -- color fringing for stylistic effect

These effects are applied via offscreen render targets. The UI is first rendered to a texture, then the post-processing shader reads that texture and writes to the screen. Effects are configurable per-widget or per-window, and can be animated.

---

## 10. Component Inspector (pulp-inspect)

A built-in development tool, analogous to browser DevTools but for native GPU-rendered plugin UIs.

**Inspection:**
- Click any widget in a running plugin
- See: bounds, theme tokens, render time, accessibility state, parameter bindings
- Accessibility tree viewer showing the full a11y hierarchy

**Live editing:**
- Change a token value and see it applied immediately
- Adjust layout properties in real time
- Toggle widget style modes

**Render stats overlay:**
- FPS counter
- Draw call count
- GPU memory usage
- Frame time histogram

The inspector connects to a running plugin instance. It can run as an overlay within the plugin window or as a separate companion window.

---

## 11. WebGPU Compute for Audio

Dawn's WebGPU API provides access to GPU compute shaders, enabling GPU-accelerated audio processing:

- **Additive synthesis** -- thousands of partials computed in parallel on the GPU
- **FFT** -- large FFT sizes (4096+) computed faster on GPU than CPU
- **Physical modeling** -- waveguide meshes and finite element methods
- **ML inference** -- neural network inference for amp modeling, style transfer, source separation

This is cross-platform: the same compute shader code runs on Vulkan, Metal, D3D12, and WebGPU. No platform-specific compute code.

This is exploration territory -- architecturally enabled from day one but not required for basic plugin development. The GPU compute pipeline is opt-in: plugins that do not use it pay no cost.

---

## 12. Web/WASM Target

The same plugin codebase compiles to a web application:

- **Emscripten** compiles the C++ core to WASM
- **JS UI code** runs natively in the browser (no embedded engine needed -- the browser IS the JS engine)
- **WebGPU canvas** for rendering (same Dawn/Skia Graphite rendering code, targeting the browser's WebGPU API)
- **Web Audio API** for audio I/O
- **Web MIDI API** for MIDI input/output

### Use Cases

- Interactive demos on a plugin vendor's website
- Browser-based audio tools and instruments
- Embeddable previews in documentation or marketing pages
- Rapid prototyping without native compilation
- Educational tools and tutorials

The web target is not a replacement for native plugins -- it is a bonus output from the same codebase. DAW integration (VST3/AU/CLAP/AAX) requires native builds. The web target serves distribution, demonstration, and prototyping scenarios.

---

## 13. Why Not Web Views

Using WKWebView (macOS/iOS), WebView2 (Windows), or WebKitGTK (Linux) as the plugin UI runtime introduces a long list of problems:

- **Cache pollution.** Each plugin instance creates per-plugin caches in system locations (`~/Library/Caches`, `%LOCALAPPDATA%`, etc.). Users end up with gigabytes of web cache from plugin UIs they never asked for.
- **Process explosion.** On Windows, WebView2 spawns multiple subprocesses per web view instance. A session with 10 plugins open can mean 30+ browser subprocesses.
- **Painful bridging.** The plugin's C++ state and the web view's JavaScript state are two separate contexts connected by message passing. Every parameter change, every meter update, every automation value must be serialized across this bridge.
- **Visualization bottlenecks.** On WebKit (macOS/iOS), there is no shared memory between the audio thread and the web view's rendering context. Audio visualization data must be copied through message-passing APIs, adding latency and CPU overhead.
- **First-load jank.** Web views have an unavoidable flicker on first render while the web content loads. Users see a blank or white rectangle before the UI appears.
- **Memory overhead.** A web view idles at 50-100MB+ of memory. Multiply by the number of open plugin instances.
- **Security and sandboxing restrictions.** CORS policies, Content Security Policy, and browser sandboxing interfere with legitimate plugin operations like file access and local resource loading.
- **Pointer locking workarounds.** Browser-based pointer lock (needed for knob dragging) behaves differently across platforms and requires workarounds that break in some DAW hosts.
- **File dialog API mismatch.** The browser's file picker API does not match native file dialogs, leading to inconsistent UX or complex native bridging.

Pulp provides the developer experience of web development (JavaScript, hot-reload, CSS-like theming) without the browser runtime baggage. The JS engine is embedded directly in the plugin process, rendering goes through the GPU, and there is no web view in the stack.

---

## 14. Why Not Visage/bgfx

Pulp's rendering strategy originally considered Visage (a GPU-accelerated UI framework built on bgfx). The shift to Dawn/Skia Graphite/QuickJS was driven by several factors:

- **No web target.** Visage uses bgfx, which lacks WebGPU support. There is no path to a WASM/WebGPU browser target with bgfx.
- **No scripting layer.** Visage has no JS scripting layer. UI changes require C++ recompilation -- no hot-reload, no rapid iteration.
- **Slow iteration.** Every UI tweak in Visage means recompiling C++. With the JS scripting approach, UI changes take < 100ms to appear.
- **Aging abstraction.** bgfx is an older GPU abstraction layer. Dawn/WebGPU is the modern standard, backed by Google, Mozilla, and Apple, with active development and a clear specification process.
- **Dependency trajectory.** Dawn and Skia Graphite are core components of Chrome and Android. They will be maintained and improved for the foreseeable future. bgfx's maintenance trajectory is less certain.

Visage remains a viable rendering backend for projects that do not need a web target or JS scripting. It is not the recommended path for new Pulp projects.

---

## 15. Platform Rendering Matrix

| Platform | GPU Backend | JS Engine | Window Type |
|----------|-------------|-----------|-------------|
| macOS | Metal (via Dawn) | QuickJS / JSC | NSView |
| Windows | D3D12 (via Dawn) | QuickJS / V8 | HWND |
| Linux | Vulkan (via Dawn) | QuickJS / V8 | X11 / Wayland |
| iOS | Metal (via Dawn) | QuickJS / JSC | UIView |
| Web | WebGPU (native) | Browser JS | Canvas |

On every platform, the rendering code is identical. Dawn handles the translation from WebGPU API calls to the platform's native GPU API. The JS engine choice is a deployment decision, not a code change -- the same JS UI files run on any engine.

---

## 16. Third-Party Dependencies

| Component | License | Purpose |
|-----------|---------|---------|
| Dawn | BSD-3-Clause | WebGPU implementation (GPU abstraction) |
| Skia Graphite | BSD-3-Clause | 2D GPU rendering (text, shapes, images, effects) |
| QuickJS | MIT | JS engine (default, lightweight) |
| V8 | BSD-3-Clause | JS engine (alternative, high performance) |
| JavaScriptCore | LGPL-2.0 (system) | JS engine (alternative, Apple platforms, already present on system) |
| Emscripten | MIT / LLVM | WASM compilation for web target |

All runtime dependencies are permissively licensed (BSD or MIT). JavaScriptCore is used only as a system framework on Apple platforms, not bundled.

---

## 17. Risks and Mitigations

| Risk | Impact | Mitigation |
|------|--------|------------|
| Dawn maturity for non-Chrome use | Dawn's standalone embedding API is less battle-tested than its use inside Chrome | Track Dawn's embedding API closely. Contribute fixes upstream. Dawn is already used standalone by iPlug3/MGFX, providing a second data point. |
| Skia Graphite readiness | Graphite is newer than Skia's Ganesh backend and may have gaps | Fall back to Skia Ganesh (the current production GPU backend) if Graphite has issues. Ganesh uses the same Skia drawing API, so the switch is a backend configuration change, not a rewrite. |
| QuickJS performance for complex UIs | QuickJS is an interpreter with no JIT. Very complex UIs with heavy JS computation may be slow. | Use V8 or JSC when peak JS performance is needed. Profile and move hot paths to C++ if necessary. Most plugin UIs are not JS-computation-heavy. |
| WebGPU browser support | WebGPU is not yet available in all browsers | Target evergreen browsers only (Chrome, Edge, Firefox, Safari). WebGPU is shipping in Chrome and Edge, and is in development in Firefox and Safari. For the web target, WebGPU availability is the gating factor. |
| JS engine memory in plugin context | Multiple plugin instances in a DAW each running a JS engine could add up | QuickJS uses very little memory (~1-2MB per instance). For V8, share a single engine (isolate) across plugin instances in the same host process. |
| Build complexity | Dawn and Skia are large codebases with complex build systems | Provide pre-built binaries for all target platforms. The build system downloads pre-built Dawn/Skia rather than compiling from source. Source builds are supported but not required. |
