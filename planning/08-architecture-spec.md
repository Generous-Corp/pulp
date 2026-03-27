# Pulp Architecture Specification

Version: 0.2.0-draft
Date: 2026-03-24

## 1. Design Philosophy

Pulp is a cross-platform audio framework built on seven core principles:

1. **Clean separation of concerns (two-pillar architecture).** The processing pillar (audio, DSP, plugin formats) and the presentation pillar (rendering, views, theming) are fully independent. A plugin can ship headless with no UI dependency whatsoever.

2. **Minimal coupling between subsystems.** Each subsystem declares explicit dependencies. No subsystem reaches into another's internals. Public headers are the only contract surface.

3. **Modern language features.** C++20 (concepts, ranges, `std::span`, coroutines where beneficial) for the cross-platform core. Swift 6 with strict concurrency for the Apple-specific layer. No legacy C++11/14 patterns where modern alternatives exist.

4. **Platform-native where it matters, cross-platform where possible.** Audio backends, GPU rendering, and system integration use native APIs directly. Business logic, DSP, and state management are fully portable.

5. **Testable interfaces with clear contracts.** Every subsystem defines its thread-safety guarantees, ownership model, and error handling approach. All subsystems ship with tests.

6. **Designed for AI-assisted development.** A root `CLAUDE.md` provides framework context. Naming is explicit and greppable. Conventions are consistent so that AI agents can reason about the codebase without special-case knowledge. Design tokens are structured JSON -- any tool that can read/write JSON can update the design.

7. **Native GPU rendering with JS scripting, not web views.** UIs are rendered directly via WebGPU (Dawn) and Skia Graphite, with a lightweight JavaScript scripting layer for hot-reloadable UI code. This avoids the cost, complexity, and limitations of embedded web views while retaining a fast iteration loop.

---

## 2. Subsystem Overview

Pulp is composed of the following subsystems. Each subsystem is a distinct build target with its own public API, dependency list, and test suite.

### 2.1 pulp-platform (Platform Abstraction)

| Attribute | Detail |
|---|---|
| **Responsibilities** | Platform detection, native API bridging, compilation conditionals, native handle types |
| **Non-responsibilities** | No business logic. No utilities beyond platform identity. |
| **Public API** | `Platform` enum, `ObjCBridge` (Apple), `ComBridge` (Windows), native handle typedefs |
| **Dependencies** | None (leaf dependency) |
| **Thread safety** | All queries are stateless and thread-safe |
| **Ownership model** | Header-only where possible; no managed resources |
| **Error handling** | Compile-time assertions for unsupported platforms |

**Key design decisions:**
- Compile-time platform selection via CMake generator expressions and `#if` macros defined in a single `platform_detect.h` header.
- Platform-specific code lives in isolated directories (`platform/mac/`, `platform/win/`, etc.), never interleaved with cross-platform source files.
- Provides `PULP_PLATFORM_MAC`, `PULP_PLATFORM_WIN`, `PULP_PLATFORM_LINUX`, `PULP_PLATFORM_IOS`, `PULP_PLATFORM_ANDROID`, `PULP_PLATFORM_WASM` macros.

---

### 2.2 pulp-runtime (Core Runtime)

| Attribute | Detail |
|---|---|
| **Responsibilities** | Memory management utilities, lock-free containers, SIMD helpers, UTF-8 string helpers (thin wrappers around `std::string`), file I/O convenience, compression, logging, error handling |
| **Non-responsibilities** | NOT a parallel standard library. Does not reimplement containers, threads, mutexes, or filesystem that `std::` already covers. |
| **Public API** | `Log` (leveled logger), `LockFreeQueue<T>`, `SimdOps`, `Utf8` (string helpers), `BinaryReader`/`BinaryWriter`, `ZlibCodec`, `Result<T, E>` |
| **Dependencies** | pulp-platform, C++20 standard library, platform APIs |
| **Thread safety** | Lock-free structures are thread-safe by design. `Log` is thread-safe. Other utilities document per-function guarantees. |
| **Ownership model** | Value types where possible. RAII for resources. No raw `new`/`delete`. |
| **Error handling** | `Result<T, E>` for recoverable errors. Assertions for programmer errors. No exceptions in the audio path. |

**Key design decisions:**
- Use `std::string` everywhere. `Utf8` is a namespace of free functions (`Utf8::toUpper`, `Utf8::codePointCount`, etc.), not a string class.
- Use `std::filesystem` for path operations. Provide thin helpers only for platform-specific locations (app support directory, user documents, etc.).
- Use `std::thread`, `std::mutex`, `std::atomic`. Only provide lock-free structures (`LockFreeQueue`, `LockFreeStack`, `SpscRingBuffer`) that the standard library lacks.
- SIMD via `SimdOps` namespace with platform intrinsic backends (SSE/AVX on x86, NEON on ARM), auto-dispatched at compile time.

---

### 2.3 pulp-events (Event System)

| Attribute | Detail |
|---|---|
| **Responsibilities** | Event loop abstraction, async dispatch, timer management, change notification (observer pattern) |
| **Non-responsibilities** | NOT a singleton. NOT a UI framework. Does not own the main thread. |
| **Public API** | `EventLoop` (constructible object), `Timer`, `AsyncDispatcher`, `Signal<Args...>`, `Connection` (RAII subscription handle) |
| **Dependencies** | pulp-runtime, platform event loops (CFRunLoop, Win32 message pump, epoll/kqueue) |
| **Thread safety** | `EventLoop` methods are callable from any thread (dispatch posts to loop's thread). `Signal` connections are thread-safe to add/remove. Emission happens on the emitting thread. |
| **Ownership model** | `EventLoop` is owned by the creator. `Connection` handles are RAII: destruction disconnects. |
| **Error handling** | Invalid timer intervals assert in debug, clamp in release. |

**Key design decisions:**
- `EventLoop` is **not** a singleton. You construct one, run it, and destroy it. A plugin host may provide its own event loop; Pulp adapts to it.
- Main-thread dispatch is explicit: `eventLoop.dispatchAsync([]{...})`. There is no global `dispatchToMainThread()` function.
- `Signal<Args...>` is a type-safe observer pattern. `Connection` objects manage lifetime. No raw function pointer registration.
- Supports structured concurrency: `EventLoop::post()` returns a future-like handle for cancellation.

---

### 2.4 pulp-render (GPU Rendering Engine)

| Attribute | Detail |
|---|---|
| **Responsibilities** | GPU-accelerated rendering via WebGPU (Dawn) with Skia Graphite for 2D, JavaScript scripting layer for UI code, hot-reload of JS/TS UI files, post-processing effects, WebGPU compute shader access for audio DSP |
| **Non-responsibilities** | NOT widgets or layout (that is pulp-view). NOT audio processing. |
| **Public API** | `RenderSurface`, `RenderContext`, `ShaderEffect`, `PostProcessPipeline`, `ScriptEngine`, `HotReloader`, `ComputeDispatch` |
| **Dependencies** | pulp-runtime, pulp-events, Dawn (WebGPU), Skia Graphite, QuickJS (default) / V8 / JSC (platform-adaptive) |
| **Thread safety** | GPU rendering on the render thread. Script execution on the event loop thread. Compute shaders dispatched from any thread. |
| **Ownership model** | `RenderSurface` owns GPU resources and the Skia Graphite context. Destroyed with the view hierarchy. `ScriptEngine` owns the JS runtime. |
| **Error handling** | Surface creation returns `Result<RenderSurface, RenderError>`. Shader compilation errors reported via callback. Script errors logged and surfaced to pulp-inspect. |

**Rendering backends:**

| Platform | Backend | GPU API |
|---|---|---|
| macOS / iOS | Dawn | Metal |
| Windows | Dawn | D3D12 |
| Linux | Dawn | Vulkan |
| Web / WASM | Dawn | WebGPU (native browser API) |

**JavaScript scripting layer:**

| Platform | Default Engine | Notes |
|---|---|---|
| All (embedded) | QuickJS | Lightweight, zero dependencies, ~200 KB |
| macOS / iOS | JavaScriptCore (JSC) | Platform-native, optional for performance |
| Windows / Linux | V8 | Optional, for full ES2024+ support |
| Web / WASM | N/A | JS runs natively in the browser |

**Key design decisions:**
- **pulp-render is the primary rendering engine**, not an optional add-on. All Pulp UIs render through this subsystem by default. There is no fallback to platform-specific 2D APIs (CoreGraphics, Direct2D) -- Dawn/Skia Graphite provides a single unified GPU rendering path across all platforms.
- **Skia Graphite for 2D.** All vector drawing, text rendering, image compositing, and anti-aliasing go through Skia Graphite, which targets Dawn's WebGPU backend. This replaces the per-platform backend approach.
- **Hot-reload of UI code.** JS/TS files defining widget behavior and layout are loaded at runtime, not compiled into the binary. Changing a `.js` file triggers an immediate re-render without restarting the plugin. The hot-reload cycle targets sub-second latency.
- **Post-processing effects.** `PostProcessPipeline` chains GPU shader effects: bloom, blur, CRT scanline, custom WGSL shaders. Effects are defined declaratively and applied per-surface or per-widget.
- **WebGPU compute shaders for audio DSP.** `ComputeDispatch` provides access to WebGPU compute shaders for parallelizable audio tasks: additive synthesis, FFT, ML inference. Data transfer between CPU audio buffers and GPU compute is managed via staging buffers with explicit synchronization.
- **QuickJS as the default JS engine.** QuickJS is embedded by default for its small footprint and zero external dependencies. On platforms where a higher-performance engine is available (JSC on Apple, V8 on desktop), the engine can be swapped at build time via a CMake option.
- **Script-to-native bridge.** The JS scripting layer exposes pulp-view widgets, pulp-state parameters, and pulp-render drawing primitives as JS objects. Scripts can create widgets, bind to parameters, and define custom draw routines.

---

### 2.5 pulp-canvas (2D Drawing API)

| Attribute | Detail |
|---|---|
| **Responsibilities** | Abstract 2D drawing API surface: vector paths, image decoding, font loading, text layout, color management |
| **Non-responsibilities** | NOT a rendering backend. NOT widgets. NOT layout. NOT event handling. |
| **Public API** | `Canvas` (abstract drawing context), `Path`, `Transform`, `Brush` (solid, gradient, pattern), `Font`, `TextLayout`, `Image`, `Color` |
| **Dependencies** | pulp-runtime, pulp-render (Skia Graphite provides the implementation) |
| **Thread safety** | `Canvas` is not thread-safe (one canvas per thread/context). `Image` and `Font` are immutable after creation and thread-safe to read. |
| **Ownership model** | `Canvas` is created by pulp-render's `RenderSurface`. `Path`, `Brush`, `Image` are value types (COW or shared internally). |
| **Error handling** | Factory methods return `Result<Canvas, Error>`. Drawing operations silently clip to bounds. |

**Key design decisions:**
- **pulp-canvas is now the 2D API surface provided by Skia Graphite.** It is no longer an abstract interface with multiple per-platform backends. Instead, it is a single API backed by Skia Graphite running on pulp-render's Dawn/WebGPU context.
- The `Canvas` interface remains the public drawing contract. Code draws against `Canvas`; the implementation is Skia Graphite on all platforms.
- Text rendering via `TextLayout`: create a layout, measure it, then draw it to a `Canvas`. Supports attributed text (mixed fonts, colors).
- Color management: `Color` stores linear RGBA internally. sRGB conversion at the display boundary.
- No per-platform backends (CoreGraphics, Direct2D) -- Skia Graphite provides consistent cross-platform rendering with sub-pixel accuracy.

---

### 2.6 pulp-view (UI Framework)

| Attribute | Detail |
|---|---|
| **Responsibilities** | View hierarchy, input event routing (mouse, keyboard, touch, pen), built-in widgets, layout engine, theming, accessibility, audio visualization primitives, design token system, multi-window support |
| **Non-responsibilities** | NOT audio processing. NOT GPU rendering (delegates to pulp-render via pulp-canvas). |
| **Public API** | `View` (base class), `Knob`, `Fader`, `Toggle`, `Selector`, `TextInput`, `ListView`, `TreeView`, `LayoutEngine` (flex/grid), `Theme`, `AccessibilityProvider`, `DesignTokens`, `WindowManager`, `Waveform`, `Spectrum`, `Meter`, `SpectralDisplay` |
| **Dependencies** | pulp-canvas, pulp-events, pulp-render (for JS widget definitions and hot-reload) |
| **Thread safety** | View hierarchy must be accessed from the event loop thread only. Property bindings may be updated from any thread (queued dispatch). Audio visualization primitives read from lock-free queues (no mutex contention with the audio thread). |
| **Ownership model** | Views own their children (unique_ptr). RAII destruction. No manual `addChild`/`removeChild` with raw pointers. |
| **Error handling** | Layout constraint violations logged as warnings. Invalid property bindings assert in debug. |

**Key design decisions:**
- **Widgets are defined via JS/TS, rendered by pulp-render.** Widget behavior, layout, and custom draw routines are authored in JavaScript or TypeScript. The JS files are loaded at runtime by pulp-render's `ScriptEngine` and hot-reloaded on change. C++ built-in widgets serve as the base primitives that JS code composes.
- **View ownership is RAII.** A parent `View` holds `std::unique_ptr<View>` to children. Destruction cascades. No orphaned views.
- **Theme is composable, not monolithic.** Each widget type has a corresponding style struct (`KnobStyle`, `FaderStyle`, `ToggleStyle`). A `Theme` is a collection of style factories. You can override individual widget styles without touching others.
- **Reactive data binding.** View properties can be bound to `Observable<T>` values. Changes propagate automatically without timer-based polling.
- **Layout via declarative descriptions.** `LayoutEngine` supports flexbox and grid models, specified as data (not imperative code). Layout is computed in a single pass where possible.
- **Accessibility.** `AccessibilityProvider` is attached to any `View` to expose it to platform accessibility APIs (VoiceOver, Narrator, AT-SPI).
- **On Apple platforms, `pulp-view-swift` provides a SwiftUI alternative.** Both paths coexist; the developer chooses one.

**Audio visualization primitives:**

| Primitive | Purpose | Key Properties |
|---|---|---|
| `Waveform` | Real-time waveform display | bufferSize, zoom, color, lock-free audio queue |
| `Spectrum` | FFT-based frequency spectrum | fftSize, windowFunction, dBRange, peakHold |
| `Meter` | Level metering with ballistics | peakHoldTime, decayRate, rmsWindow, clip indicator |
| `SpectralDisplay` | Spectrogram / waterfall view | timeRange, frequencyRange, colorMap |

Audio visualization is built on lock-free queues (`SpscRingBuffer` from pulp-runtime) fed by the audio thread. The visualization widgets perform STFT, metering ballistics, and waveform rendering on the render thread -- never blocking the audio callback.

**Design token system:**

Design tokens define the visual language of a Pulp UI: colors, typography, spacing, border radii, shadows, and widget geometry. Tokens are structured JSON data:

```json
{
  "colors": {
    "background": "#1a1a2e",
    "surface": "#16213e",
    "primary": "#0f3460",
    "accent": "#e94560",
    "text": "#eaeaea"
  },
  "typography": {
    "body": { "family": "Inter", "size": 13, "weight": 400 },
    "label": { "family": "Inter", "size": 11, "weight": 600 }
  },
  "spacing": { "xs": 4, "sm": 8, "md": 16, "lg": 24 },
  "widgets": {
    "knob": { "diameter": 48, "trackWidth": 3, "thumbRadius": 6 }
  }
}
```

Because tokens are plain JSON, **any tool that can read and write JSON can update the design** -- AI agents, design tools, CLI scripts, or a live editor. The `DesignTokens` class loads token files, watches for changes, and triggers re-theming on update.

**Multi-window support:**

`WindowManager` supports multiple OS-level windows: floating palettes, inspectors, popups, and detachable panels. Each window gets its own `RenderSurface` from pulp-render. Windows can be positioned relative to the main plugin window or independently.

**Widget inventory:**

| Widget | Purpose | Key Properties |
|---|---|---|
| `Knob` | Rotary parameter control | value, range, sensitivity, bipolar |
| `Fader` | Linear slider | value, range, orientation |
| `Toggle` | Binary switch | state, label |
| `Selector` | Dropdown / segmented | items, selectedIndex |
| `TextInput` | Editable text field | text, placeholder, validation |
| `ListView` | Scrollable item list | dataSource, selection |
| `TreeView` | Hierarchical item view | dataSource, expandedNodes |
| `Label` | Static text display | text, font, alignment |
| `Panel` | Container with optional border | backgroundColor, border |
| `ImageView` | Image display | image, scaleMode |
| `Waveform` | Real-time waveform | see audio visualization above |
| `Spectrum` | Frequency spectrum | see audio visualization above |
| `Meter` | Level meter | see audio visualization above |
| `SpectralDisplay` | Spectrogram | see audio visualization above |

---

### 2.7 pulp-view-swift (Swift UI Layer -- Apple Only)

| Attribute | Detail |
|---|---|
| **Responsibilities** | SwiftUI-based plugin UIs for macOS and iOS |
| **Public API** | `ParameterBinding` (Swift property wrapper), `PulpAudioUnitView` (SwiftUI hosting), Metal custom rendering helpers, `PulpViewRepresentable` (NSViewRepresentable / UIViewRepresentable bridge) |
| **Dependencies** | pulp-state (via C++ interop), SwiftUI, Metal |
| **Thread safety** | All UI access on MainActor. Parameter reads are atomic (bridged from C++). |
| **Ownership model** | SwiftUI value types. C++ state accessed via non-owning references. |
| **Error handling** | Swift error types bridged from C++ `Result`. |

**Key design decisions:**
- Uses Swift/C++ interop (zero overhead since Swift 5.9) to access `pulp-state` parameters directly. No Objective-C bridging layer.
- `@ParameterBinding` property wrapper provides two-way binding between SwiftUI views and C++ `Parameter<T>` objects.
- AUv3 hosts get a native SwiftUI view controller via `AUAudioUnit.requestViewController`.
- Metal custom rendering available for complex visualizations (spectrum analyzers, waveform displays) embedded in SwiftUI.

---

### 2.8 pulp-audio (Audio Engine)

| Attribute | Detail |
|---|---|
| **Responsibilities** | Audio device enumeration, configuration, real-time callback delivery, multi-channel routing |
| **Non-responsibilities** | NOT DSP processing. NOT plugin hosting. NOT MIDI. |
| **Public API** | `AudioSystem` (device enumeration), `AudioDevice` (open/configure/start/stop), `AudioCallback` (function signature), `AudioBuffer` (non-owning multi-channel view) |
| **Dependencies** | pulp-runtime |
| **Thread safety** | `AudioSystem` enumeration on any thread. `AudioCallback` invoked on real-time audio thread. `AudioDevice` configuration from main thread only. |
| **Ownership model** | `AudioSystem` creates `AudioDevice` (unique ownership). `AudioBuffer` is non-owning (like `std::span`). |
| **Error handling** | Device operations return `Result<void, AudioError>`. Callback must never fail (real-time). |

**Backends:**

| Platform | Backend |
|---|---|
| macOS | CoreAudio (AudioUnit HAL) |
| iOS | AVAudioSession + CoreAudio |
| Windows | WASAPI (exclusive and shared mode) |
| Linux | ALSA (direct), PipeWire (preferred) |
| Android | Oboe (wraps AAudio / OpenSL ES) |

**Key design decisions:**
- `AudioCallback` is a function signature (`void(AudioBuffer input, AudioBuffer output, uint32_t frameCount)`), not a class to inherit from. Register it on an `AudioDevice`.
- `AudioBuffer` is a non-owning multi-channel buffer view. It holds a pointer to channel pointers and a frame count. Similar to `std::span` but for interleaved or non-interleaved multi-channel audio. Zero-copy.
- Device management is completely separate from callback delivery. You enumerate devices, open one, configure it, register a callback, and start it. Each step is explicit.
- Hot-plugging support: `AudioSystem` emits device-change signals.

```cpp
// Example usage
using AudioCallback = void(*)(AudioBuffer input, AudioBuffer output, uint32_t frameCount);

struct AudioBuffer {
    float* const* channels;   // Non-owning pointer to channel data
    uint32_t numChannels;
    uint32_t numFrames;

    float* operator[](uint32_t ch) const { return channels[ch]; }
};
```

---

### 2.9 pulp-midi (MIDI Engine)

| Attribute | Detail |
|---|---|
| **Responsibilities** | MIDI device I/O, message types, MIDI 2.0 UMP, MPE state tracking, MIDI-CI |
| **Non-responsibilities** | NOT audio. NOT synthesis. |
| **Public API** | `MidiSystem` (device enumeration), `MidiPort` (I/O), `MidiEvent` (lightweight message), `MidiBuffer` (timestamped event collection), `UmpMessage`, `MpeState` |
| **Dependencies** | pulp-runtime |
| **Thread safety** | `MidiSystem` enumeration on any thread. `MidiPort` callback on real-time thread. `MidiBuffer` iteration is thread-safe if single-writer. |
| **Ownership model** | `MidiEvent` is a lightweight value type (16 bytes max). `MidiBuffer` owns its storage. |
| **Error handling** | Port operations return `Result`. Malformed MIDI data is logged and skipped. |

**Key design decisions:**
- `MidiEvent` is lightweight: status byte + data bytes + timestamp, packed into 16 bytes or less. No heap allocation per event.
- `MidiBuffer` stores a sorted sequence of timestamped events. Zero-copy iteration via iterator pair.
- MIDI 2.0 UMP (`UmpMessage`) is a first-class type, not an afterthought. Conversion between MIDI 1.0 and UMP is provided.
- `MpeState` tracks per-note expression state for MPE-aware processing.
- Hot-plugging support: `MidiSystem` emits device-change signals.

---

### 2.10 pulp-signal (DSP Library)

| Attribute | Detail |
|---|---|
| **Responsibilities** | Audio signal processing building blocks: filters, FFT, convolution, dynamics, oscillators, oversampling |
| **Non-responsibilities** | NOT plugin format handling. NOT device I/O. |
| **Public API** | `SignalBlock`, `Processor` concept, `Fft`, `Convolver`, `Biquad`, `StateVariableFilter`, `CrossoverFilter`, `Oversampler`, `DelayBuffer`, `EnvelopeFollower`, `Compressor`, `Limiter`, `Oscillator` |
| **Dependencies** | pulp-runtime |
| **Thread safety** | Processor instances are single-threaded (one per audio stream). Factory functions are thread-safe. |
| **Ownership model** | Processors own their internal state. `SignalBlock` is non-owning. |
| **Error handling** | `prepare()` returns `Result` if configuration is invalid. `process()` never fails. |

**Key design decisions:**
- **C++20 concepts for the Processor interface, not virtual inheritance.** Any type satisfying the `Processor` concept can be used in the signal chain:

```cpp
template <typename T>
concept Processor = requires(T p, SignalBlock block, double sampleRate, uint32_t maxFrames) {
    { p.prepare(sampleRate, maxFrames) } -> std::same_as<Result<void, ProcessorError>>;
    { p.process(block) } -> std::same_as<void>;
    { p.reset() } -> std::same_as<void>;
};
```

- `SignalBlock` is the DSP equivalent of `AudioBuffer`: a non-owning multi-channel buffer view designed for in-place processing.
- FFT via platform-optimized backends (vDSP on Apple, FFTW or pffft on others), hidden behind the `Fft` interface.
- SIMD via `pulp-runtime`'s `SimdOps`, used internally by filters and oscillators.
- All DSP types are real-time safe: no allocation, no locking, no system calls in `process()`.

---

### 2.11 pulp-state (Parameter & State System)

| Attribute | Detail |
|---|---|
| **Responsibilities** | Plugin parameters, state serialization/deserialization, GUI binding, undo/redo, host automation interface |
| **Non-responsibilities** | NOT audio processing. NOT UI rendering. |
| **Public API** | `Parameter<T>` (typed parameter), `ParameterGroup` (tree structure), `StateStore` (serialize/deserialize/undo/redo), `Binding<T>` (reactive link), `AutomationGesture` |
| **Dependencies** | pulp-runtime |
| **Thread safety** | `Parameter<T>` has atomic read/write for audio-thread access. `StateStore` serialization is main-thread only. `Binding<T>` dispatches changes to the target thread. |
| **Ownership model** | `StateStore` owns all `Parameter` and `ParameterGroup` instances. `Binding<T>` is a non-owning observer. |
| **Error handling** | Deserialization returns `Result` with version mismatch info. |

**Key design decisions:**
- **Parameters are typed structs, not a class hierarchy.**

```cpp
Parameter<float> gain { "gain", "Gain", 0.0f, 1.0f, 0.75f };
Parameter<int>   mode { "mode", "Mode", 0, 3, 0, {"Clean", "Warm", "Hot", "Burn"} };
Parameter<bool>  bypass { "bypass", "Bypass", false };
```

- **State serialization uses a versioned binary format by default**, not XML. Compact, fast, forward-compatible with version tags. JSON export available for debugging.
- **Atomic parameter access.** The audio thread reads parameter values via lock-free atomic loads. The UI thread writes via atomic stores. No mutex contention on the audio thread.
- **Reactive bindings.** `Binding<T>` connects a `Parameter<T>` to a UI property. Changes in either direction propagate automatically. No timer-based polling.
- **Undo/redo built into `StateStore`.** Each parameter change is recorded. Undo groups parameter changes into logical transactions.
- **`AutomationGesture`** marks begin/end of user interaction for host automation recording.

---

### 2.12 pulp-format (Plugin Format Adapters)

| Attribute | Detail |
|---|---|
| **Responsibilities** | Wrapping a Pulp processor as VST3, AU, AUv3, AAX, CLAP, LV2, standalone application, CLI tool, MCP server, or Web/WASM target |
| **Non-responsibilities** | NOT the processor itself. NOT the UI. |
| **Public API** | `PluginDescriptor` (metadata struct), `FormatExporter` (compile-time format selection macros/templates) |
| **Dependencies** | pulp-audio, pulp-midi, pulp-state, format SDKs |
| **Thread safety** | Each format adapter follows its host's threading model. |
| **Ownership model** | Format adapters own the processor instance they wrap. |
| **Error handling** | Format-specific error codes mapped to Pulp's `Result` type where applicable. |

**Supported formats:**

| Format | SDK License | Notes |
|---|---|---|
| VST3 | Dual (GPLv3 / proprietary) | Via Steinberg VST3 SDK |
| Audio Unit (v2) | Apple | macOS only |
| AUv3 | Apple | macOS / iOS, Swift-native option |
| AAX | Proprietary (Avid) | Requires Avid agreement |
| CLAP | MIT | Modern, open format |
| LV2 | ISC | Linux-primary, cross-platform |
| Standalone | N/A | Native app with device selection |
| CLI | N/A | Command-line tool (see below) |
| MCP Server | N/A | Model Context Protocol server (see below) |
| Web / WASM | N/A | Emscripten + WebGPU (see below) |

**Key design decisions:**
- The processor is defined once using the `Processor` concept. Format adapters are compile-time selected, not runtime polymorphism. You never write format-specific code.
- Each format adapter is a separate compilation unit. Unused formats add zero code to the binary.
- `PluginDescriptor` is a plain data struct:

```cpp
constexpr PluginDescriptor desc {
    .name = "My Synth",
    .vendor = "My Company",
    .version = "1.0.0",
    .uid = "com.mycompany.mysynth",
    .category = PluginCategory::Synthesizer,
    .inputChannels = 0,
    .outputChannels = 2,
};
```

- **Headless mode.** A processor can be instantiated and run without any UI dependency. This enables server-side rendering, automated testing, and command-line batch processing.
- **AUv3 via Swift.** On Apple platforms, the AUv3 adapter can be implemented in Swift via `pulp-format-swift`, providing a native `AUAudioUnit` subclass.

**CLI mode:**

Every Pulp plugin compiles as a command-line tool. The CLI adapter wraps the processor with stdin/stdout audio streaming, file-based I/O (WAV, AIFF, FLAC), parameter control via command-line flags, and preset loading. This enables batch processing pipelines, automated testing, and integration with shell scripts.

```bash
# Example: process a file with a Pulp plugin
my-plugin --input song.wav --output processed.wav --gain 0.8 --mode 2
```

**MCP server mode:**

Every Pulp plugin exposes a Model Context Protocol (MCP) server. This allows AI agents to discover, configure, and invoke the plugin programmatically. The MCP adapter exposes:
- Parameter listing and modification
- Preset enumeration and loading
- Audio processing (file-based or streaming)
- State inspection and serialization

This makes every Pulp plugin an AI-tool-friendly endpoint with zero additional code from the developer.

**Web / WASM target:**

Plugins can be compiled to WebAssembly via Emscripten. The WASM build targets the browser's native WebGPU API for rendering (via Dawn's Emscripten backend) and Web Audio API for audio I/O. The same JS/TS UI code used in native builds runs directly in the browser. This enables:
- Browser-based plugin demos
- Web DAW integration
- Online preset editors with live preview

---

### 2.13 pulp-format-swift (Swift Format Layer -- Apple Only)

| Attribute | Detail |
|---|---|
| **Responsibilities** | AUv3 Audio Unit implementation in Swift |
| **Public API** | `PulpAudioUnit` (Swift `AUAudioUnit` subclass), parameter tree construction helpers, Swift/C++ bridge for the render block |
| **Dependencies** | pulp-state, AudioToolbox, AVFoundation |
| **Thread safety** | Render block executes on the real-time thread. Parameter tree access follows AUv3 threading rules. |
| **Ownership model** | `PulpAudioUnit` owns the C++ processor via an opaque pointer. |
| **Error handling** | Swift error types for configuration failures. |

**Key design decisions:**
- Direct Swift/C++ interop for the render block. The C++ `Processor::process()` is called directly from Swift with zero bridging overhead.
- Parameter tree is constructed in Swift from `pulp-state`'s `ParameterGroup` hierarchy.
- View controller factory returns a SwiftUI hosting controller (from `pulp-view-swift`).

---

### 2.14 pulp-inspect (Component Inspector)

| Attribute | Detail |
|---|---|
| **Responsibilities** | Runtime inspection of running plugin UIs: component tree, property values, layout bounds, design tokens, render statistics |
| **Non-responsibilities** | NOT a debugger. NOT a profiler (though it surfaces render stats). Does not modify plugin audio behavior. |
| **Public API** | `Inspector`, `InspectorOverlay`, `InspectorPanel`, `RenderStats`, `TokenInspector` |
| **Dependencies** | pulp-view, pulp-render, pulp-events |
| **Thread safety** | Inspector reads view hierarchy on the event loop thread. Render stats collected atomically from the render thread. |
| **Ownership model** | `Inspector` is owned by the host application or debug harness. Non-owning references to the inspected view hierarchy. |
| **Error handling** | Inspection of destroyed views returns empty results. No crashes on stale references. |

**Key design decisions:**
- **Like browser DevTools for native GPU-rendered UIs.** pulp-inspect provides the same inspect-and-tweak workflow that web developers expect, but for Pulp's native rendering stack.
- **Click-to-inspect.** With the inspector active, clicking any widget highlights it and displays its properties, bounds, applied design tokens, and render statistics in the inspector panel.
- **Live property inspection.** Property values update in real time as the plugin runs. Bound parameters show their current value, binding source, and update frequency.
- **Layout bounds overlay.** Toggle an overlay that draws bounding boxes, padding, and margin for every widget in the hierarchy. Color-coded by layout type (flex, grid, absolute).
- **Design token inspection.** For any widget, see which design tokens are applied and their resolved values. Edit tokens live and see changes reflected immediately.
- **Render statistics.** Per-frame GPU timing, draw call count, texture memory, shader compilation status, and JS script execution time.
- **Inspector runs in a separate window** (via pulp-view's multi-window support). It can also be embedded as a collapsible panel within the plugin window for constrained hosts.
- **Debug builds only by default.** The inspector is compiled out of release builds unless explicitly opted in via a CMake flag (`PULP_ENABLE_INSPECTOR=ON`).

---

### 2.15 pulp-build (Build Tooling)

| Attribute | Detail |
|---|---|
| **Responsibilities** | CMake functions for project creation, format target generation, binary asset embedding |
| **Public API** | `pulp_add_plugin()`, `pulp_add_app()`, `pulp_add_binary_data()`, `pulp_set_format_sdk_path()` |
| **Dependencies** | CMake 3.24+ |

**Key design decisions:**
- `pulp_add_plugin()` creates all format targets in a single call:

```cmake
pulp_add_plugin(MyPlugin
    FORMATS VST3 AU AUv3 CLAP Standalone CLI MCP WASM
    DESCRIPTOR_FILE "${CMAKE_CURRENT_SOURCE_DIR}/descriptor.h"
    SOURCES src/Processor.cpp src/Editor.cpp
    JS_UI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/ui/"
)
```

- `pulp_add_binary_data()` embeds binary assets (images, presets, wavetables) as C++ data arrays, accessible at compile time.
- `JS_UI_DIR` specifies the directory containing JS/TS UI files. These are bundled with the plugin binary and watched for hot-reload in debug builds.
- SPM integration for Apple Swift targets: the CMake build can invoke `swift build` for `pulp-view-swift` and `pulp-format-swift`.
- Supports both standalone builds and consumption as a dependency (via CMake `FetchContent`).
- WASM target uses Emscripten toolchain file, automatically configured when `WASM` is in the `FORMATS` list.

---

### 2.16 pulp-test (Testing Framework)

| Attribute | Detail |
|---|---|
| **Responsibilities** | Audio-specific test harnesses, plugin format validation, visual regression testing, hot-reload testing |
| **Public API** | `AudioTestHarness` (offline render + compare), `VisualTestHarness` (screenshot comparison), `HotReloadTestHarness` (JS reload cycle testing), plugin validators |
| **Dependencies** | pulp-runtime, Catch2 or doctest |

**Key design decisions:**
- Does not reinvent a test framework. Uses Catch2 (or doctest) for the test runner. Pulp provides audio-specific harnesses on top.
- `AudioTestHarness`: instantiates a processor offline, feeds it known input, captures output, compares against golden reference files with configurable tolerance (RMS, peak, spectral).
- `VisualTestHarness`: renders a view hierarchy to an offscreen `RenderSurface`, captures a screenshot, compares against a reference image with perceptual diff.
- `HotReloadTestHarness`: modifies a JS UI file, verifies the hot-reload triggers within a timeout, captures a post-reload screenshot, and compares against reference.
- Plugin format validators: wraps `auval` (AU), pluginval-equivalent checks (VST3/CLAP), and format-specific compliance tests.

---

### 2.17 pulp-ship (Packaging & Distribution)

| Attribute | Detail |
|---|---|
| **Responsibilities** | Installer creation, code signing, notarization, auto-update infrastructure |
| **Public API** | CLI scripts and CMake targets for packaging per platform |
| **Dependencies** | Platform signing tools, Sparkle (macOS), WinSparkle (Windows) |

**Packaging per platform:**

| Platform | Package Format | Signing |
|---|---|---|
| macOS | .pkg installer | Developer ID + notarization |
| Windows | NSIS or WiX installer | Authenticode or Azure Trusted Signing |
| Linux | .tar.gz, .deb | GPG signature |
| Web | Static site (HTML + WASM + JS) | N/A |

---

### 2.18 pulp-ci (CI/CD & GitHub Integration)

| Attribute | Detail |
|---|---|
| **Responsibilities** | GitHub Actions workflow templates, secrets management helpers, release automation |
| **Public API** | Workflow YAML templates, CLI helper scripts |

---

### 2.19 pulp-claude (Claude Code Plugin)

The Claude Code plugin for Pulp development workflows. See [09-plugin-spec.md](09-plugin-spec.md) for the complete specification.

---

## 3. Subsystem Dependency Graph

```
pulp-platform (leaf -- no dependencies)
  +-- pulp-runtime
        |
        +-- pulp-events
        |     +-- pulp-render -------> Dawn (WebGPU), Skia Graphite, QuickJS/V8/JSC
        |     |     +-- pulp-canvas (2D API surface, backed by Skia Graphite)
        |     |
        |     +-- pulp-view ---------> pulp-canvas, pulp-render
        |     |     +-- pulp-inspect
        |     |
        |     (pulp-view-swift -- Apple only, depends on pulp-state)
        |
        +-- pulp-audio
        |
        +-- pulp-midi
        |
        +-- pulp-signal
        |
        +-- pulp-state
              +-- pulp-format -----> format SDKs (VST3, AU, CLAP, LV2, AAX)
                    +-- pulp-format-swift (Apple only)

Apple Swift layer (depends on pulp-state via C++ interop):
  pulp-view-swift
  pulp-format-swift

Build and tooling (standalone, not linked into products):
  pulp-build    (CMake modules)
  pulp-test     (test harnesses)
  pulp-ship     (packaging scripts)
  pulp-ci       (CI/CD templates)
  pulp-claude   (Claude Code plugin)
```

### Dependency rules

1. Dependencies flow downward only. No circular dependencies.
2. `pulp-platform` depends on nothing. Everything else may depend on it (transitively through `pulp-runtime`).
3. The processing pillar (`pulp-audio`, `pulp-midi`, `pulp-signal`, `pulp-state`, `pulp-format`) has **zero dependency** on the presentation pillar (`pulp-render`, `pulp-canvas`, `pulp-view`, `pulp-inspect`).
4. The presentation pillar depends on `pulp-events` for event dispatch, `pulp-render` for GPU rendering, and `pulp-canvas` for the 2D drawing API. It does **not** depend on audio or DSP subsystems.
5. `pulp-state` bridges the two pillars: processing reads parameters, presentation binds to them. But `pulp-state` itself depends on neither pillar.
6. `pulp-render` is the foundation of the presentation pillar. `pulp-canvas` is its 2D API surface. `pulp-view` builds widgets on top of both.
7. `pulp-inspect` depends on `pulp-view` and `pulp-render` -- it is a presentation-pillar-only tool.

---

## 4. Design Decisions

### 4.1 Why Not Web Views

Embedding web views (WKWebView on macOS/iOS, WebView2 on Windows) for plugin UIs is a common approach. Pulp explicitly rejects this in favor of native GPU rendering with a JS scripting layer. The reasons:

**Cache pollution and shared state.** WKWebView shares cookie storage, local storage, and HTTP cache across all WKWebView instances on the system unless elaborate process-pool isolation is configured. Multiple plugin instances in a DAW session can corrupt each other's cached state. WebView2 has similar profile-sharing issues.

**Process explosion.** Each WKWebView spawns multiple child processes (WebContent, Networking, GPU). A DAW session with 20 plugin instances can spawn 60+ child processes, consuming hundreds of megabytes of memory before any UI content loads. WebView2 mitigates this somewhat with shared renderer processes, but the overhead is still substantial.

**Bridging pain.** Communication between native C++ plugin code and JavaScript inside a web view requires serialization across a process boundary. Parameter updates, audio visualization data, and user input all pay this serialization tax. The round-trip latency makes tight audio-visual synchronization difficult.

**Visualization bottlenecks.** Real-time audio visualization (waveforms, spectra, meters) requires pushing data from the audio thread to the GPU at 60 fps. With a web view, this data must cross: audio thread -> main thread -> IPC to WebContent process -> JavaScript -> DOM/Canvas -> GPU. With native GPU rendering, the path is: audio thread -> lock-free queue -> GPU. The difference is measurable in both latency and CPU overhead.

**First-load jank.** Web views have cold-start overhead: process launch, HTML parsing, CSS layout, JavaScript compilation. The first time a plugin UI opens, there is a visible delay (200-800ms) before content appears. Native GPU rendering has near-instant first frame.

**Memory overhead.** A minimal WKWebView instance consumes 30-50 MB of memory. A native GPU-rendered UI with equivalent functionality uses 5-10 MB. In a DAW with many plugins open, this difference is significant.

**Security sandboxing.** WKWebView inherits the web security model (CORS, CSP, sandboxed filesystem). This is useful for browsers but counterproductive for plugin UIs that need direct access to local files, plugin state, and audio data. Workarounds (custom URL schemes, message handlers) add complexity and fragility.

**Pulp's alternative: native GPU + JS scripting.** By using Dawn/WebGPU for GPU access and Skia Graphite for 2D rendering, Pulp gets a single unified rendering path across all platforms with no process boundaries. The QuickJS scripting layer provides the rapid iteration and hot-reload benefits of web development without the overhead of a full browser engine. UI code is JS; rendering is native GPU. The result is web-like developer experience with native performance.

---

## 5. Interface Contracts

### 5.1 Contract Template

Every subsystem's public API adheres to the following contract structure:

| Contract Element | Description |
|---|---|
| **Provides** | What the subsystem's public headers expose |
| **Requires** | What dependencies must be linked and initialized |
| **Thread safety** | Which methods are safe from which threads |
| **Ownership** | Who owns allocated resources; RAII guarantees |
| **Error model** | How errors are reported (Result, assertion, exception-free) |
| **Real-time safety** | Which functions are safe to call from the audio thread |

### 5.2 Real-Time Safety Contract

Functions callable from the audio/real-time thread MUST:
- Never allocate or free heap memory
- Never acquire a mutex or other blocking lock
- Never perform system calls (I/O, logging, etc.)
- Never throw exceptions
- Complete in bounded time

Functions meeting these criteria are annotated with `[[pulp::realtime]]` (a custom attribute macro that enables static analysis).

### 5.3 Cross-Pillar Communication

The processing pillar and presentation pillar communicate exclusively through `pulp-state`:

```
[Audio Thread]                   [UI Thread]
     |                                |
     |  Processor reads               |  View reads
     |  Parameter<T> atomically       |  Parameter<T> via Binding<T>
     |         \                     /|
     |          +-- pulp-state -----+ |
     |         /                     \|
     |  Processor writes              |  View writes
     |  output meters atomically      |  parameter changes
     |                                |
```

No direct function calls between pillars. No shared mutable state beyond `pulp-state`'s atomic parameters.

Audio visualization data (waveforms, spectra) flows via lock-free `SpscRingBuffer` from the audio thread to pulp-view's visualization widgets. This is a one-way data flow that does not go through `pulp-state` -- it is raw sample data, not parameter state.

---

## 6. Testing Strategy Per Subsystem

| Subsystem | Test Type | Approach |
|---|---|---|
| pulp-platform | Unit | Compile-time assertions, platform macro verification |
| pulp-runtime | Unit | Comprehensive unit tests for all utilities; stress tests for lock-free structures |
| pulp-events | Unit + integration | EventLoop dispatch ordering, timer accuracy, signal connection lifecycle |
| pulp-render | Visual + integration | Screenshot-based visual regression (render known scenes, compare against reference images with perceptual diff); hot-reload cycle testing (modify JS, verify re-render within timeout); cross-platform rendering comparison (same scene rendered on Metal/D3D12/Vulkan/WebGPU, compared for consistency) |
| pulp-canvas | Unit + visual | Render known primitives via Skia Graphite, compare output against reference images |
| pulp-view | Unit + visual | Layout computation tests; visual regression via screenshot comparison; design token application tests |
| pulp-view-swift | UI tests | Xcode UI tests for SwiftUI views |
| pulp-inspect | Integration | Inspector overlay accuracy (bounds match actual layout); property value correctness; render stats plausibility |
| pulp-audio | Integration | Loopback device tests (record output, verify against input); device hot-plug simulation |
| pulp-midi | Unit + integration | Message parsing round-trips; UMP conversion; device I/O with virtual ports |
| pulp-signal | Unit + golden-file | DSP output comparison against golden reference files (RMS tolerance < -120 dB) |
| pulp-state | Unit + fuzz | Serialization round-trips; version migration; fuzz testing for deserializer robustness |
| pulp-format | Integration | Per-format validation: `auval` for AU, compliance scanners for VST3/CLAP; CLI mode I/O tests; MCP server request/response tests |
| pulp-format-swift | Integration | AUv3 hosting in test harness; parameter tree validation |
| pulp-build | Integration | CMake configure + build for example projects on all platforms; WASM build verification |

### pulp-render Testing Detail

**Screenshot-based visual regression:**
- Each test case renders a known scene (widget tree with fixed parameters and design tokens) to an offscreen `RenderSurface`.
- The resulting framebuffer is captured as a PNG and compared against a golden reference image.
- Comparison uses perceptual diff (SSIM or equivalent) with a configurable threshold. Sub-pixel differences from different GPU drivers are tolerated.
- Reference images are stored in the repository per platform (Metal, D3D12, Vulkan) since minor rendering differences are expected.

**Hot-reload cycle testing:**
- `HotReloadTestHarness` starts a plugin UI, modifies a JS file on disk, and measures the time until the UI reflects the change.
- Asserts that the reload completes within a configurable timeout (default: 500ms).
- Captures pre- and post-reload screenshots and verifies the expected visual change occurred.

**Cross-platform rendering comparison:**
- The same test scene is rendered on all supported backends (Metal, D3D12, Vulkan, WebGPU).
- Screenshots from each backend are compared pairwise.
- Differences beyond a platform-specific tolerance threshold are flagged. This catches backend-specific rendering bugs while allowing for driver-level variation.

### Continuous Integration Matrix

| Platform | Compiler | Architectures |
|---|---|---|
| macOS 14+ | Apple Clang 15+ | arm64, x86_64 |
| Windows 11 | MSVC 17.8+ | x64 |
| Ubuntu 22.04+ | GCC 13+ / Clang 17+ | x86_64 |
| iOS 17+ | Apple Clang 15+ | arm64 |
| Web (WASM) | Emscripten 3.1+ | wasm32 |

---

## 7. Glossary

| Term | Definition |
|---|---|
| **Two-pillar architecture** | Separation of processing (audio/DSP/state) from presentation (rendering/UI). Either pillar works independently. |
| **Processor concept** | A C++20 concept requiring `prepare()`, `process()`, and `reset()` methods. The fundamental unit of audio processing. |
| **SignalBlock** | Non-owning multi-channel audio buffer view for DSP processing. |
| **AudioBuffer** | Non-owning multi-channel audio buffer view for device I/O. |
| **StateStore** | Central parameter and state management, bridging processing and presentation pillars. |
| **Binding** | Reactive connection between a parameter and a UI property. |
| **FormatExporter** | Compile-time mechanism to wrap a Processor as a specific plugin format. |
| **Canvas** | Abstract 2D drawing context backed by Skia Graphite. |
| **View** | Base class for UI elements, with RAII child ownership and declarative layout. |
| **Theme** | Composable collection of per-widget style objects. |
| **Design tokens** | Structured JSON data defining colors, typography, spacing, and widget geometry. AI-tool-friendly. |
| **RenderSurface** | GPU rendering context backed by Dawn/WebGPU, hosting Skia Graphite for 2D drawing. |
| **Dawn** | Google's open-source WebGPU implementation, providing Metal/D3D12/Vulkan/WebGPU backends. |
| **Skia Graphite** | Skia's next-generation GPU rendering backend, targeting WebGPU/Dawn. |
| **QuickJS** | Lightweight embeddable JavaScript engine (~200 KB), used as the default scripting runtime. |
| **Hot-reload** | Runtime reloading of JS/TS UI files without restarting the plugin. |
| **MCP** | Model Context Protocol. Enables AI agents to discover and interact with Pulp plugins programmatically. |
| **Inspector** | pulp-inspect's runtime UI inspection tool, analogous to browser DevTools for native GPU-rendered UIs. |
