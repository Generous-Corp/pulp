# Pulp

Pulp is an MIT-licensed framework for building audio plugins and standalone applications. You can start from C++, a design tool like Figma, a conversation with an agent, or a JavaScript prototype, then iterate toward production.

It provides GPU-rendered UIs with hot reload, an extensible DSP layer supporting Faust, Cmajor, and JSFX, support for all major plugin formats, and CLI and MCP tooling aligned with modern development workflows.

---

## Five Principles

1. **Permissive licensing** — MIT. No royalties, no revenue thresholds, no copyleft.
2. **Modular architecture** — use what you need, ignore the rest.
3. **Native platform experiences** — Swift on Apple, C++ everywhere, GPU rendering on every platform.
4. **Flexible rendering** — GPU-native UI without a browser, but WebView is there if you want it.
5. **Modern workflows** — AI agents, design tools, hot-reload, and fast iteration from the start.

---

## Start How You Want

### Start from code

Write C++20. The core gives you a `Processor` base class, a thread-safe parameter system, lock-free runtime primitives, and a DSP library with oscillators, filters, dynamics, analysis, and effects. Build headless, test from the command line, and add a UI when you're ready.

If you prefer writing DSP in a dedicated language, Pulp integrates with Faust (offline codegen into native `Processor` instances), Cmajor (external toolchain with validation and generation), and JSFX (bounded subset parsing and validation).

### Start from design

Import from Figma, Pencil.dev, Google Stich and more. Define your visual identity as a design token system — colors, typography, widget geometry, knob styles, button shapes, shadows, gradients. Apply one design language across many plugins. Change a token, and changes appear live in the standalone host via hot-reload.

Describe a look in natural language — "80s Macintosh", "neon cyberpunk", "minimal Dieter Rams" — and watch the entire interface transform. The design system is structured data, not compiled C++, so any AI tool that can read JSON can modify your theme.

Tokens export to JSON, CSS variables, C++ headers, GPU shader uniforms, and OKLCH color systems.

### Start from conversation

The `pulp` CLI covers the full lifecycle: scaffold, build, test, validate, sign, ship. It's also an MCP server, so any AI agent — Claude Code, Codex, Gemini, and more — can drive it as a tool. CI pipelines can load, configure, process, and verify plugin output end-to-end without launching a DAW.

### Start from prototype

Write your plugin UI in JavaScript. The renderer is native GPU — WebGPU via Dawn, with Skia Graphite for 2D — running at 60–120 FPS with hot-reload in the standalone host. No browser, no IPC, no subprocess tree. The same JS UI renders through Metal on macOS/iOS, D3D12 on Windows, Vulkan on Linux, and WebGPU in browsers.

### Mix and match

These aren't exclusive paths. Write your DSP in Faust, your UI in JavaScript, and scaffold the project from an AI agent. Or write everything in C++. Or start with a Figma design and let an agent wire up the parameters.

---

## What You Get

### Plugin Formats

VST3, Audio Unit (v2 and v3), CLAP, LV2, AAX, and Standalone — from a single codebase. CLAP support is especially deep, with modulation, per-note control, presets, and the full extension surface.

### GPU-Rendered UI

Widgets include rotary knobs, faders, meters, toggles, XY pads, waveform and spectrum displays, text editors, combo boxes, list boxes, tabs, scroll views, tooltips, modals, tree views, and a preset browser — all styled by design tokens. Real-time visualization uses lock-free data flow from the audio thread with built-in metering ballistics and FFT helpers. Post-processing effects (blur, shadow, bloom, color grading) apply to any layer. Every widget exposes accessibility roles to platform screen readers.

**Inspector:** Click any widget in a running plugin to see its properties, bounds, theme tokens, and render performance — like browser DevTools for native GPU UI.

### DSP Library

Header-only C++ across six categories: oscillators, filters (biquad, SVF, ladder, Linkwitz-Riley, TPT, FIR, ballistics), dynamics (compressor, noise gate, gain, panner), modulation (ADSR, smoothed/log-ramped values), effects (delay, chorus, phaser, reverb, waveshaper), and analysis (FFT, windowing, oversampling, interpolation, filter design, processor chain). All processors work standalone or chained, independent of the UI or format subsystems.

### DSP Authoring Languages

**Faust** — Write DSP in Faust, generate C++ offline, and wrap it as a native `Processor` with automatic parameter reflection. Faust processors integrate with the full format adapter stack.

**Cmajor** — External toolchain integration with validation and code generation for C++ and CLAP targets.

**JSFX** — Bounded subset support for REAPER-style effect scripts with parameter extraction.

### Three JavaScript Engines

**QuickJS** — fast cold-start, low memory, the default. **V8** — JIT compilation, host objects, typed arrays, promises, full Three.js compatibility. **JavaScriptCore** — Apple-native, available on macOS and iOS. The engine factory selects at build time or runtime; your UI code doesn't change.

### Three.js on Native GPU

Three.js doesn't need a browser — it needs a JS engine, a graphics API, and a host environment. Pulp provides a native WebGPU bridge against Dawn, DOM and Canvas compatibility shims, a buffered GPU draw path, and lock-free integration with the audio thread for feeding visualization data into 3D scenes.

The result is Three.js rendering inside a plugin window, composited by your DAW, with no browser process or WebView lifecycle issues. The gains over WebViews aren't raw GPU throughput — they're lower memory overhead, better startup behavior, lower latency between plugin state and UI, better multi-instance scaling, and no browser process model or CORS/CSP friction. Web-style authoring on a native plugin architecture.

### Swift on Apple

C++20 at the core, Swift where Apple intends it: AUv3 in Swift, SwiftUI as an alternative UI path, direct C++/Swift interop (zero overhead since Swift 5.9), and Accelerate/vDSP for platform-optimized DSP. C++ owns the real-time audio thread; Swift owns the host interface.

### Application Framework

Beyond plugins: standalone apps with MenuBar, Toolbar, keyboard shortcuts, settings persistence, preset management, undo/redo, multi-window support (floating palettes, inspectors), audio device I/O across platforms, MIDI 2.0 UMP/MPE, and OSC 1.0 over UDP.

### CLI, Build, and Ship

```bash
pulp create "My Synth" --formats vst3,au,clap
pulp build
pulp test
pulp ship --sign --notarize
pulp inspect
pulp design
```

Three-platform CI (macOS ARM64, Windows x64, Linux x64). Code signing and notarization on macOS, Authenticode on Windows. DMG/PKG, NSIS, and .deb/.tar.gz packaging. EdDSA-signed auto-updates. WASM builds for web deployment. The CLI is also an MCP server for AI agent integration.

### Web and WASM

The same plugin that runs as a VST3 in your DAW can run in a browser via Emscripten and WebGPU. WebView embedding is also available when you genuinely want browser content — a code editor, documentation panel, or HTML preview.

---

## The Architecture

### Processing and Presentation Are Separate

Your DSP code doesn't link against a windowing system. This enables headless testing, GPU backend swapping, SwiftUI alongside JS-scripted UIs, web export, and AI agents that reason about audio code without loading the UI framework.

### Thread Model

- **Audio thread**: atomics for params, lock-free triple buffers for meter data
- **UI thread**: polls param changes, updates widgets
- **Main thread**: init, state serialization, file I/O

Sync primitives: `std::atomic`, `SeqLock`, `TripleBuffer`, `SpscQueue`.

### Modular, Not Monolithic

| Subsystem | Purpose |
|-----------|---------|
| `pulp-runtime` | Lock-free structures, logging, assertions |
| `pulp-events` | Event loop, timers, async dispatch |
| `pulp-audio` | Audio device I/O — CoreAudio, WASAPI, ALSA, Oboe |
| `pulp-midi` | MIDI I/O, MIDI 2.0 UMP, MPE, MIDI-CI |
| `pulp-signal` | DSP processors, FFT, filters, effects, math |
| `pulp-state` | Parameters, bindings, serialization, undo/redo, presets |
| `pulp-format` | Plugin format adapters, standalone, headless, CLI |
| `pulp-dsl` | Faust/Cmajor/JSFX integration |
| `pulp-render` | GPU rendering — Dawn, Skia Graphite, JS scripting |
| `pulp-view` | Widgets, layout, themes, design tokens, accessibility |
| `pulp-platform` | Platform detection, native API bridging, Swift interop |
| `pulp-ship` | Packaging, signing, notarization, auto-updates |

---

## The Decisions

### Why MIT?

MIT means no royalties, no adoption friction, no business risk. Use it, ship it, don't worry about it.

### Why GPU + JS Instead of WebViews?

Every DAW developer who's opened Activity Monitor after loading WebView plugins knows the cost — per-instance renderer processes, caches, memory overhead, serialization across process boundaries. Pulp runs a native GPU renderer with an in-process JS engine. Same hot-reload and rapid iteration, but native GPU performance and lock-free audio visualization.

### Why Design Tokens?

Structured data — JSON that any tool can read and write — instead of inheritance hierarchies. AI tools update your design by writing tokens. Changes propagate instantly in the standalone host via hot-reload.

### Why Headless-First?

`pulp-format` links against audio, MIDI, and state — not view or render. Build, test, and validate without any UI code. CI is faster, CLIs are lighter, and AI agents are more effective.

---

## Standing on Shoulders

**Oli Larkin's** [iPlug2](https://github.com/iPlug2/iPlug2) and [iPlug3](https://github.com/iPlug3) arrived at many of the same architectural conclusions — Dawn, Skia Graphite, QuickJS. **[Visage](https://github.com/VitalAudio/visage)** by Matt Tytel informed our GPU UI thinking. **[CLAP](https://cleveraudio.org)**, **[CHOC](https://github.com/Tracktion/choc)** by Jules Storer, **[Dawn](https://dawn.googlesource.com/dawn)**, and **[Skia](https://skia.org)** are foundational.

Pulp started primarily for one end customer — the original author — with the hope that it's useful to others who have ideas for how audio app development could work better.

---

## Platform Support

| Platform | Audio | MIDI | Plugin Formats | UI | GPU | Swift |
|----------|-------|------|----------------|-----|-----|-------|
| iOS (ARM64) | AVAudioSession | CoreMIDI | AUv3, Standalone | JS + GPU | Metal | Yes |
| Linux (x64, ARM64) | ALSA, JACK | ALSA MIDI | VST3, CLAP, LV2, Standalone | JS + GPU | Vulkan | — |
| macOS (ARM64) | CoreAudio | CoreMIDI | VST3, AU, AUv3, CLAP, AAX, LV2, Standalone | JS + GPU | Metal | Yes |
| Web (WASM) | Web Audio | Web MIDI | — | JS + WebGPU | WebGPU | — |
| Windows (x64, ARM64) | WASAPI | Win32 MIDI | VST3, CLAP, AAX, Standalone | JS + GPU | D3D12 | — |

ASIO on Windows supported when developers provide the Steinberg SDK independently. AAX requires the Avid SDK from Avid.

---

## Current State

Pulp is not a fork or wrapper. It implements audio I/O, MIDI, plugin formats, rendering, and UI from the ground up using permissively-licensed format SDKs.

The core framework, format adapters, DSP library, GPU rendering, JS scripting with hot-reload, multi-window support, and CLI toolchain are working. GPU rendering is runtime-validated on macOS (Metal); Windows (D3D12) and Linux (Vulkan) surfaces are implemented and awaiting hardware validation. Web/WASM builds compile and deploy but lack automated browser end-to-end tests.

Pulp ships with example projects — from a simple gain plugin to a Three.js demo with live audio visualization — that validate the framework and serve as starting points. The set grows as the framework evolves.
