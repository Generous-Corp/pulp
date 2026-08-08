# Architecture

## Subsystem Dependency Direction

Pulp's subsystems form a directed acyclic graph. Lower-level subsystems do not depend on higher-level ones.

```
platform              (leaf — no dependencies)
    ^
 runtime              (depends on: platform)
    ^
    +------+------+------+------+------+
    |      |      |      |      |      |
 events  state  audio  midi  canvas   osc
           ^      ^     ^      ^
           |      |     |      |
           +--+---+-----+      +--- render  (depends on: runtime, canvas)
              |
           format                            (depends on: state, audio, midi)
              |
           view                              (depends on: canvas, events, state)

 signal                                      (header-only; music/runtime/timebase)
```

Key rules:

- `platform` has no internal dependencies (OS detection, native types)
- `runtime` depends on `platform` only
- `state` depends on `runtime` only
- `format` depends on `state`, `audio`, `midi` -- not on `view` or `render`
- `view` and `render` are optional -- plugins can be built and tested headless
- `signal` is header-only and depends on the low-level `music`, `runtime`, and
  `timebase` value vocabularies; it does not depend on host, UI, or format layers
- `osc` depends on `runtime` for logging and assertions

## Public vs Internal Surfaces

Each subsystem exposes public headers under `core/<subsystem>/include/pulp/<subsystem>/`. These are the consumer API.

Internal implementation lives in `core/<subsystem>/src/`. Nothing outside the subsystem should include from `src/`.

The convention:

```
core/format/
  include/pulp/format/     <-- public headers (consumers include from here)
    processor.hpp
    clap_entry.hpp
    vst3_entry.hpp
    au_v2_entry.hpp
    headless.hpp
  src/                     <-- internal implementation
    clap_adapter.cpp
    vst3_adapter.cpp
    au_v2_adapter.cpp
    standalone.cpp
```

## The Processor Interface

The central abstraction is `pulp::format::Processor`. Plugin developers subclass it and implement:

- `descriptor()` -- returns plugin metadata (name, manufacturer, category, bus layout)
- `define_parameters()` -- registers parameters with the state store
- `prepare()` -- called once before processing begins
- `process()` -- called on the real-time audio thread every buffer cycle

Format adapters (VST3, AU, CLAP) wrap a `Processor` instance and translate between the format's API and Pulp's interface. The developer writes one processor; the build system creates multiple format targets.

## Thread Model

Pulp uses three thread contexts:

| Thread | What Runs | Rules |
|--------|-----------|-------|
| Audio thread | `Processor::process()`, parameter reads | No allocation, no locks, no exceptions, no I/O |
| UI thread | Widget updates, event loop, change listeners | May allocate, may lock (briefly) |
| Host thread | `prepare()`, `release()`, state serialization | Full access |

Communication between threads uses lock-free primitives from `runtime/`:

| Primitive | Purpose |
|-----------|---------|
| `std::atomic<T>` | Single values, latest-wins (parameter values) |
| `SeqLock<T>` | Multi-field coherent reads (transport state) |
| `TripleBuffer<T>` | Large data swaps (wavetables, IR buffers, meter data) |
| `SPSCQueue<T>` | Ordered event streams (MIDI events, UI commands) |

## Build System

CMake is the build system. The key function is `pulp_add_plugin()`, which creates:

- `${target}_Core` -- shared processor code (object or interface library)
- `${target}_VST3` -- VST3 bundle (`.vst3`)
- `${target}_AU` -- AU v2 component (`.component`, macOS only)
- `${target}_CLAP` -- CLAP bundle (`.clap`)
- `${target}_Standalone` -- standalone app target (`.app` bundle on macOS; executable elsewhere)

Each format target links against the core library and its format-specific adapter. The developer provides small entry-point files (`vst3_entry.cpp`, `clap_entry.cpp`, `au_v2_entry.cpp`, `main.cpp`) that use one-line macros.

## GPU Rendering Stack

The rendering stack (experimental) layers:

1. **Dawn** -- WebGPU implementation providing GPU access via Metal (macOS), D3D12, Vulkan
2. **Skia Graphite** -- 2D rendering on top of Dawn's GPU context
3. **Canvas** -- abstraction layer over Skia and CoreGraphics, with SVG and post-processing
4. **View** -- widgets, layout, themes, JS scripting engine, hot-reload

This stack is optional. Plugins can be built, tested, and shipped without any UI code in the dependency tree.

## JavaScript Engine Abstraction

The view system uses a `JsEngine` abstraction that supports multiple JavaScript backends:

| Engine | Use Case | Features |
|--------|----------|----------|
| **QuickJS** | Default for plugins | Fast cold-start, low memory, portable |
| **V8** | Three.js, heavy workloads | JIT compilation, typed arrays, promises, full ES module support |
| **JavaScriptCore** | Apple platforms | Native Apple integration, available on macOS and iOS |

The engine is selected at build time or runtime via `JsEngineType`. Your UI code doesn't change — the same JS runs on any backend. QuickJS is always available. V8 requires an external library (Node.js or standalone V8 monolith). JSC is available on Apple platforms.

Key APIs: `ScriptEngine` (high-level wrapper), `JsEngine` (backend interface), `WidgetBridge` (JS-to-native widget binding).

The Three.js native demo uses V8 for full WebGPU compatibility — Three.js needs typed arrays, promises, and ES module `import` support that QuickJS doesn't provide.

## Platform Isolation

Platform-specific code lives in `core/platform/` and behind `#ifdef` guards or platform subdirectories. The rest of the codebase uses platform-agnostic APIs.

On Apple, additional Swift code lives in `apple/`. C++/Swift interop uses direct interop (Swift 5.9+), not Objective-C++ bridges.
