# Pulp

## Five Principles

Pulp is built on five principles:

1. **Permissive licensing**
2. **Modular architecture**
3. **Native platform experiences**
4. **GPU-first rendering**
5. **Modern development workflows**

Everything else follows from these.

---

## What Is Pulp?

Pulp is a cross-platform framework for building audio plugins and applications. It targets VST3, Audio Unit, AUv3, CLAP, LV2, AAX, standalone, and the web — on macOS, Windows, Linux, iOS, and browsers.

It is MIT-licensed. No royalties. No revenue thresholds. No copyleft. Build what you want, ship it however you want.

It is designed for developers who build software the way software is built now — with AI tools, with fast iteration cycles, with design systems that export to code, with plugins that are scriptable and inspectable. Pulp doesn't bolt these capabilities on. They're in the architecture from the start.

---

## Why Pulp Exists

The dominant audio framework was designed in an era before AI-assisted coding, before GPU-accelerated UIs became practical, before Swift/C++ interop matured, before permissive licensing became table stakes for serious open-source infrastructure. Before WebGPU made it possible to run the same GPU code on desktop, mobile, and the web.

The tools haven't kept up with how we work now.

If you've ever tried to iterate on a plugin UI by recompiling C++ for every color change — you know the problem. If you've ever wanted an AI agent to test your plugin's parameters and compare A/B settings — you know the problem. If you've ever wanted to describe "warm analog synth" and see your entire plugin theme transform live — you know the problem.

Pulp is what we'd build if we started today, knowing what we know now.

---

## The Architecture

### Two Pillars: Processing and Presentation

The processing engine knows nothing about the UI. The UI knows nothing about the renderer. Your DSP code doesn't link against a windowing system. Your windowing system doesn't assume a specific graphics backend.

This isn't just clean architecture. It's what makes everything else possible: headless testing, GPU backend swapping, SwiftUI on Apple alongside JS-scripted UIs on Windows, web export via WASM, and AI agents that can reason about your audio code without loading your entire UI framework into context.

### GPU Rendering with a JavaScript Scripting Layer

This is a fundamental departure from how plugin UIs have been built.

Pulp's primary UI path uses native GPU rendering — WebGPU via Dawn, with Skia Graphite for 2D — driven by a JavaScript scripting layer. You write your plugin UI in JS or TypeScript. The renderer is native GPU at 60–120 FPS. There is no browser. No web view. No process explosion. No cache pollution. No CORS policies.

What you get:

- **Hot-reload** — change a color, a layout, a widget style, and see it instantly. No recompile.
- **Cross-platform from one codebase** — same JS UI runs on macOS (Metal), Windows (D3D12), Linux (Vulkan), iOS (Metal), and the web (WebGPU).
- **Platform-adaptive JS engine** — QuickJS, V8, or JavaScriptCore, selected per platform for the best balance of startup time and performance.
- **Themeable widgets designed for audio** — knobs, faders, meters, toggles, and controls that understand audio UX conventions.
- **Audio visualization built in** — lock-free queues from the audio thread, STFT abstractions, metering ballistics, waveform and spectral display primitives.
- **Post-processing effects** — bloom, blur, CRT simulation, and other GPU shader effects on the UI.
- **Native integration where it matters** — file dialogs, message boxes, and popup menus use the platform's native UI. Or render your own.
- **Multi-window support** — floating palettes, inspectors, custom popups, multi-monitor workflows.
- **Accessible by default** — cross-platform accessibility built into the widget system.
- **WebView support** — for embedding web content (Monaco editor, documentation panels) when you actually want a browser context.

This is the developer experience of the web — hot-reload, JS/TS, CSS-like theming — without the browser runtime baggage.

### The Design Workflow

Building a plugin UI should feel like modern frontend development, not like writing a graphics engine.

**AI Style Designer** — Describe a look in natural language ("80s Macintosh", "neon cyberpunk", "minimal Dieter Rams") and watch the entire component showcase transform: colors, widget shapes, shadows, typography, everything. This works with Claude Code, Codex, Stitch, or any AI tool that can read your design tokens and write back changes. The framework makes this trivial — your design system is structured data, not compiled C++.

**Design Language System** — Define your visual identity as a token system: 36+ color tokens, widget geometry, knob rendering styles (arc, filled, notched, glossy), button depths (flat, raised, outlined, beveled), toggle shapes (pill, checkbox, rocker), shadows, gradients, typography. Apply one design language across many plugins. Change a token, everything updates.

**Component Inspector** — Click any widget in a running plugin to see its properties, bounds, theme tokens, render performance, accessibility state. Like browser DevTools but for your native GPU-rendered plugin UI. Debug visually, iterate fast.

**Export Everywhere** — Design tokens export to JSON, CSS variables, C++ headers, GPU shader uniforms, and OKLCH color systems. One source of truth, many output formats.

### Swift-Native on Apple, C++ at the Core

The cross-platform core — DSP engine, plugin format adapters, parameter system, state management, MIDI, audio I/O — is C++20. Same code on every platform.

On Apple platforms, Pulp adds a Swift layer:

- **AUv3 in Swift** — the way Apple intended, not a C++ approximation
- **SwiftUI as an alternative UI path** — declarative, native-feeling, for when you want Apple-native rather than cross-platform
- **Direct C++/Swift interop** — zero function-call overhead since Swift 5.9, no Objective-C++ bridge
- **Accelerate/vDSP** — platform-optimized DSP where it matters

The boundary is clean: C++ owns the real-time audio thread (no ARC, no allocation, no exceptions). Swift owns the host interface and UI. The bridge is narrow and well-defined.

### Plugins as CLIs and MCP Servers

Every Pulp plugin compiles to a command-line tool and exposes an MCP (Model Context Protocol) server. This means any AI agent — Claude Code, Codex, a custom script — can:

- **Query a plugin's parameters and current state**
- **Adjust parameters programmatically and hear the results**
- **Process audio files through the plugin**
- **Send MIDI notes and control changes**
- **Capture screenshots of the UI at any state**
- **Validate expected behavior automatically**
- **Compare A/B settings**

The CLI is useful on its own — batch processing, headless rendering, integration testing, CI validation. The MCP server extends it for AI agent workflows. This transforms plugin development from a manual, GUI-driven workflow into something that can be scripted, automated, and iterated upon by AI.

The plugin, the CLI, and the MCP server are the same binary. No separate tooling to maintain.

### Web and WASM

The same plugin that runs as a VST3 in your DAW can run in a browser.

WebGPU (via Dawn) means the GPU rendering layer targets Vulkan, Metal, D3D12, and WebGPU backends without code changes. Emscripten compiles the C++ core to WASM. The JS UI layer runs natively in the browser.

Use cases: interactive web demos, browser-based audio tools, embeddable plugin previews, educational tools, prototyping without installing anything.

### WebGPU Compute for Audio

WebGPU isn't just for graphics. Compute shaders provide direct access to GPU parallel processing:

- **Additive synthesis** — summing thousands of partials in parallel
- **FFT-based processing** — spectral analysis and resynthesis on the GPU
- **Physical modeling** — large-scale waveguide networks and modal synthesis
- **ML inference** — running trained models for amp simulation, source separation, generative audio

This is exploration territory — pushing boundaries. But the architecture enables it from day one. The cross-platform nature of WebGPU means these compute workloads run on Vulkan, Metal, and D3D12 backends without code changes.

### Modular, Not Monolithic

Pulp is organized as independent subsystems:

| Subsystem | Responsibility |
|-----------|---------------|
| **pulp-runtime** | Lock-free structures, SIMD helpers, logging. Uses `std::` where it can. |
| **pulp-events** | Event loop (constructible, not singleton), timers, async dispatch |
| **pulp-audio** | Audio device I/O — CoreAudio, WASAPI, ALSA, Oboe |
| **pulp-midi** | MIDI I/O, MIDI 2.0 UMP, MPE, MIDI-CI |
| **pulp-signal** | DSP — FFT, convolution, filters, oversampling, SIMD, GPU compute |
| **pulp-state** | Typed parameters, reactive bindings, versioned state, undo/redo |
| **pulp-format** | Format adapters — VST3, AU, AUv3, CLAP, LV2, AAX, Standalone, CLI, MCP |
| **pulp-render** | GPU rendering engine — WebGPU/Dawn, Skia Graphite, JS scripting layer |
| **pulp-view** | Widgets, layout, themes, design tokens, accessibility, audio visualization |
| **pulp-inspect** | Component inspector, render stats, live design editing |
| **pulp-platform** | Platform detection, native API bridging, Swift interop |
| **pulp-build** | CMake tooling — `pulp_add_plugin()`, `pulp_add_app()`, SPM, Emscripten |
| **pulp-test** | Testing, plugin validation, audio golden-files, visual regression |
| **pulp-ship** | Packaging, signing, notarization, auto-updates |
| **pulp-ci** | GitHub Actions, secrets management, release automation |
| **pulp-claude** | Claude Code plugin — scaffold, build, test, sign, ship from conversation |

You can use subsystems independently. `pulp-signal` doesn't require `pulp-view`. `pulp-audio` doesn't require `pulp-format`. The dependency graph is a tree, not a tangle.

### Reusable Components

Pulp ships with composed features that go beyond individual widgets — things you'd otherwise rebuild for every project:

| Component | What It Is |
|-----------|-----------|
| **Musical Typing** | Computer keyboard as MIDI input + on-screen piano keyboard. Standard for standalone instrument apps. |
| **Waveform Editor** | Audio waveform display with zoom, selection, and scrubbing. For samplers, editors, recorders. |
| **Diagnostic Reporter** | One-click diagnostic collection (system info, plugin status, crash logs, DAW logs) with privacy-first anonymization and GitHub issue upload. For end-user support. |
| **Audio Device Selector** | Audio/MIDI device picker with sample rate and buffer size configuration. Standard for standalone apps. |
| **Preset Browser** | Load, save, organize, and share presets. Categories, search, favorites. |

Add them to a project:

```bash
pulp add component musical-typing
pulp add component waveform-editor
pulp add component diagnostics
```

Components are styled by your design tokens — they inherit your design language automatically. They're also fully customizable: fork one into your project if you need to modify behavior beyond what configuration allows.

As you build features that are worth reusing, you can extract them as components and share them across your projects or with the community.

---

## The Tooling

### The `pulp` CLI

The developer CLI is the foundation of all Pulp tooling. It works standalone — no AI agent required.

```bash
pulp create "My Synth" --formats vst3,au,clap
pulp build --target vst3
pulp test
pulp ship --sign --notarize
pulp inspect                  # launch component inspector
pulp design                   # launch design tool
pulp add component musical-typing
pulp skill create "midi-routing"  # extract a reusable skill from recent work
pulp audit ~/Code/some-library    # check license, clean-room risk, arch fit
pulp status
```

The CLI is also an MCP server (`pulp mcp serve`), so any AI agent — Claude Code, Codex, Gemini — can use it as a tool. The Claude Code plugin wraps the CLI with conversational workflows, but the CLI is the source of truth.

### Claude Code Plugin

Pulp ships with a Claude Code plugin that covers the full development lifecycle:

| Command | What It Does |
|---------|-------------|
| `/pulp:create` | Scaffold a new plugin or app project with interactive configuration |
| `/pulp:build` | Build with intelligent CMake regeneration detection |
| `/pulp:test` | Unit tests, format validation, golden-file comparison, visual regression |
| `/pulp:ci` | GitHub Actions — trigger builds, check status, sync secrets |
| `/pulp:ship` | Package, sign, notarize, create GitHub Release, update download page |
| `/pulp:port` | Audit and port between macOS, Windows, Linux, and web |
| `/pulp:setup-gpu` | Add GPU-accelerated rendering to an existing project |
| `/pulp:setup-swift` | Add the Swift UI layer for Apple platforms |
| `/pulp:setup-ios` | Add an iOS/iPadOS target |
| `/pulp:setup-updates` | Add auto-update support with EdDSA signing |
| `/pulp:inspect` | Launch the component inspector on a running plugin |
| `/pulp:design` | AI-driven design session — describe a look, export tokens |
| `/pulp:status` | Project dashboard — config, build state, features, VMs |
| `/pulp:website` | Generate a GitHub Pages download page |

| `/pulp:skill` | Extract a reusable skill from something you built, or create one from scratch |
| `/pulp:audit` | Audit a library, project, or code pattern for license compatibility, clean-room risk, and architecture fit |

This isn't a bolt-on. The framework, the build system, the project templates, and the Claude plugin are designed together. The plugin knows the framework's structure. The framework is structured so the plugin can reason about it.

### CI/CD

GitHub Actions workflows ship with every Pulp project:

- Multi-platform build matrix (macOS ARM64, Windows x64, Linux x64)
- Automated plugin format validation (auval, pluginval, clap-validator)
- macOS code signing and notarization via `notarytool`
- Windows Authenticode and Azure Trusted Signing
- EdDSA-signed artifacts for Sparkle/WinSparkle auto-updates
- Draft GitHub Releases with per-platform installers
- Automatic download page updates on `gh-pages`
- WASM build and deploy for web demos

### Build System

CMake 3.24+ for the cross-platform core. Swift Package Manager for Apple Swift targets. Emscripten for WASM/web builds. All work together.

```cmake
pulp_add_plugin(MySynth
    FORMATS VST3 AU AUv3 CLAP Standalone Web
    PLUGIN_NAME "My Synth"
    BUNDLE_ID "com.mycompany.mysynth"
    VERSION "1.0.0"
)
```

---

## The Design Decisions

### Why MIT?

The audio development ecosystem needs permissive infrastructure. AGPL forces source disclosure or commercial licensing. Dual-license models create business risk and adoption friction. MIT means: use it, ship it, don't worry about it.

### Why C++20?

Concepts for plugin interfaces (compile-time validation without virtual function overhead). `std::span`, `std::filesystem`, `std::jthread` where they fit. No parallel standard library — use what the language provides, add only what's missing.

### Why GPU Rendering with JS, Not Web Views?

Web views (WKWebView, WebView2) have real advantages: hot-reload, familiar tooling. But the runtime implications are hard to ignore:

- **Cache pollution** — web views dump cache files per-plugin into system locations
- **Process explosion** — open a web view plugin on Windows, watch your DAW spawn a constellation of subprocesses
- **Bridging pain** — communicating between the web view and your C++ DSP is confusing and error-prone
- **Visualization bottlenecks** — getting real-time audio data to a browser frontend efficiently is hard
- **First-load jank** — unavoidable flicker when the web view initializes
- **Memory overhead** — 50–100MB+ idle, even for simple UIs
- **Security sandboxing** — CORS, CSP, and other web policies that make no sense in a plugin

Pulp sidesteps all of this with native GPU rendering driven by a JS scripting layer — the developer experience of the web without the browser runtime baggage.

### Why Plugins as CLIs and MCP Servers?

Because audio plugin development should work like modern software development. Your plugin should be testable from a script. Your CI should be able to load it, set parameters, process audio, and verify output — without launching a DAW. An AI agent should be able to interact with your plugin the same way it interacts with any other tool.

The CLI gives you batch processing, headless testing, and automation. The MCP server gives AI agents native access. Same binary, multiple interfaces.

### Why Composable Themes and Design Tokens?

The dominant framework's styling system is a single class that inherits from every widget's style interface. Add a widget, modify the base class. Upgrade the framework, fix your theme.

Pulp uses a design token system. Colors, typography, spacing, widget geometry, and rendering styles are structured data — JSON that any tool can read and write. AI tools can update your design by writing tokens. Your design language applies across plugins. Changes propagate instantly via hot-reload. Themes are data, not inheritance hierarchies.

### Why Reactive Bindings?

Plugin parameters should update the UI when they change — not 30 times per second regardless. Pulp's parameter bindings are reactive: UI views subscribe to parameter changes and update only when values actually change. No timer ticks. No wasted repaints.

### Why Headless-First?

`pulp-format` links against `pulp-audio`, `pulp-midi`, and `pulp-state` — not against `pulp-view` or `pulp-render`. You can build, test, and validate a plugin's audio processing without any UI code in the dependency tree. This makes CI faster, tests cleaner, CLIs lighter, and AI agents more effective.

---

## Platform Support

| Platform | Audio | MIDI | Plugin Formats | UI | GPU | Swift |
|----------|-------|------|----------------|-----|-----|-------|
| macOS (ARM64) | CoreAudio | CoreMIDI | VST3, AU, AUv3, CLAP, AAX, Standalone | JS + GPU | Metal | Yes |
| Windows (x64) | WASAPI | Win32 MIDI | VST3, CLAP, AAX, Standalone | JS + GPU | D3D12 | — |
| Linux (x64) | ALSA, JACK | ALSA MIDI | VST3, CLAP, LV2, Standalone | JS + GPU | Vulkan | — |
| iOS (ARM64) | AVAudioSession | CoreMIDI | AUv3, Standalone | JS + GPU | Metal | Yes |
| Web (WASM) | Web Audio | Web MIDI | — | JS + WebGPU | WebGPU | — |

ASIO on Windows supported when developers provide the Steinberg SDK independently. AAX requires the Avid SDK from Avid.

---

## What Pulp Is Not

Pulp is not a fork of any existing framework. This is a clean-room design informed by experience, implemented independently from specification.

Pulp is not a wrapper. It implements audio I/O, MIDI, plugin formats, rendering, and UI from the ground up — using permissively-licensed format SDKs (VST3 under MIT, AudioUnit under Apache 2.0, CLAP under MIT).

Pulp is not finished. This is the beginning of a serious, phased engineering effort. The architecture is specified. The roadmap is defined. The work is starting.

---

## The Road Ahead

Every phase ships a validation example that proves the framework works. These examples stay in the repo and grow across phases.

| Phase | What Ships | Validation Example |
|-------|-----------|-------------------|
| 1 | Repository, build system, CI pipeline | — |
| 2–3 | Core runtime, event system, CMake tooling, scaffolding | **PulpGain** — trivial gain plugin (standalone) |
| 4 | Audio I/O, MIDI I/O, standalone apps | **PulpTone** — oscillator synth with MIDI and musical typing keyboard |
| 5 | VST3, AU, CLAP adapters, parameter system, state | **PulpEffect** — filter plugin with automation and state save/load |
| 6 | GPU rendering, JS scripting, design tokens, inspector | **PulpDrums** — generative drum sequencer with GPU UI and audio visualization |
| 7 | CLI + MCP server, full Claude Code plugin | **PulpSynth** — macro oscillator synth with full GPU UI, presets, and AI-addressable MCP interface |
| 8 | Signing, notarization, packaging, auto-updates | All examples signed and distributed |
| 9 | Web/WASM target, documentation, DSP library | **PulpSampler** — YouTube audio sampler with waveform editor and musical typing (native only; web version TBD) |
| 10 | Hardening, DAW validation, public release | All examples validated across DAWs |

### The Five Examples

**PulpGain** — The simplest possible plugin. A gain knob. Validates that the build system works, a plugin can be scaffolded, and audio passes through. Every framework needs a "hello world."

**PulpTone** — A simple oscillator synth (sine, saw, square) with MIDI input. The standalone app includes a musical typing keyboard — use your computer keyboard as a piano (like GarageBand/Logic's Musical Typing) with an on-screen piano keyboard for mouse/touch input. This becomes a standard feature available to all Pulp standalone instrument apps. Validates audio I/O, MIDI I/O, and the standalone app wrapper.

**PulpEffect** — A filter effect (lowpass/highpass/bandpass) with cutoff, resonance, and mix parameters. Loads in a DAW as VST3, AU, and CLAP. Parameters automate. State saves and restores. Validates the format adapters and parameter system end-to-end.

**PulpDrums** — A generative drum sequencer MIDI effect inspired by topographic rhythm maps. Three voices (kick, snare, hi-hat), density and chaos controls, pattern morphing. Outputs MIDI — drop it before a drum instrument in your DAW. The first example with a full GPU-rendered UI: knobs, a pattern grid, real-time metering, design tokens. Validates the rendering engine, JS scripting, hot-reload, MIDI output, audio visualization, and the component inspector.

**PulpSynth** — A macro oscillator synthesizer with multiple oscillator models, modulation, and a full preset system. GPU UI with the design token system applied. Exposes CLI and MCP server interfaces so AI agents can adjust parameters, compare presets, and process audio programmatically. Validates the complete plugin-as-tool workflow.

**PulpSampler** — A sample player and YouTube audio sampler with drag-and-drop loading, a waveform editor, and the musical typing keyboard. The native version downloads audio from YouTube via bundled tools (yt-dlp, ffmpeg) — drop a URL, get a playable sample. The most complex example — validates file I/O, external tool integration, complex UI interactions, and the full development workflow. A simplified web version (file loading only, no YouTube downloading) may follow later.

---

## Status

Phase 0 is complete. The specification is written — 18 documents, 9,400+ lines covering architecture, capability matrix, contamination analysis, phased roadmap, validation plan, and more.

Phase 1 is underway. Follow the repo for updates.
