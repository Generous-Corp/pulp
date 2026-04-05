# Overview

## What Is Pulp

Pulp is a new MIT-licensed, cross-platform framework for building audio plugins and apps that is meant to feel modern from the ground up: modular, GPU-first, AI-friendly, and native on each platform rather than stuck in older framework assumptions.

The big idea is C++ at the core for audio and plugin formats, a native GPU UI layer with JS/TS for fast hot-reloadable interfaces, and a Swift-native path on Apple where that makes sense. It is designed so the same project can target major plugin formats and platforms, while also working well with CI, CLIs, and AI agents.

In plain terms: it is an attempt to build the audio framework you would make today if you wanted JUCE-like reach, much more permissive licensing, better architecture, faster UI iteration, and tooling that fits how people actually build software now.

See the [Vision](vision.html) page for the full architecture, design decisions, and roadmap.

## What It Does Today

On the current branch, Pulp provides:

- **Six default native plugin formats** — VST3, AU v2, AUv3, CLAP, LV2, and standalone — with format-specific validation in CI (pluginval for VST3, clap-validator for CLAP, auval for AU with signing caveats)
- **Optional AAX support** — clean-room AAX Native targets on macOS and Windows when developers provide their own AAX SDK and validator locally
- **Two web plugin formats** — WAMv2 (Web Audio Modules) and WebCLAP (CLAP-to-WebAssembly) for browser-based DAWs
- **Effects, instruments, and MIDI effects** — all plugin types supported across formats
- **Three desktop platforms** — macOS (primary), Windows (WASAPI, Win32 MIDI, NSIS installer), Linux (ALSA, JACK, LV2, .deb packaging) — plus experimental iOS (AVAudioSession, AUv3, Metal) and Web/WASM support
- **A thread-safe parameter system** — atomic values, normalization, modulation offsets, state serialization, undo/redo
- **Audio I/O** — CoreAudio (macOS), WASAPI (Windows), ALSA + JACK (Linux), Web Audio API (browser)
- **MIDI I/O** — CoreMIDI (macOS), Win32 MIDI (Windows), ALSA Raw MIDI (Linux), Web MIDI API (browser)
- **MIDI 2.0** — Universal MIDI Packet (UMP) support with per-note control, MPE zone configuration
- **30+ DSP processors** — oscillators, filters (biquad, SVF, ladder, Linkwitz-Riley, FIR, TPT), delay lines, compressor, limiter, reverb, chorus, phaser, waveshaper, FFT, convolver, oversampling, ADSR, panning, noise gate, windowing, smoothed values, interpolators, lookup tables, fast math approximations
- **GPU rendering** — Dawn (WebGPU) and Skia Graphite integration for 2D GPU-accelerated UIs on all platforms
- **A view/widget system** — TextEditor, ComboBox, TabPanel, ListBox, ScrollView, Tooltip, ProgressBar, TreeView, modal overlays, keyboard shortcuts, drag-and-drop, animation, inspector
- **Canvas abstraction** — CoreGraphics, Skia, and recording backends, SVG support, post-processing effects
- **Preset management** — save/load/browse presets with factory/user separation, DAW integration
- **Plugin hosting** — PluginScanner, PluginSlot, and SignalGraph for building DAW-like applications
- **OSC support** — Open Sound Control 1.0 sender/receiver over UDP
- **Identity system** — UUIDv4 with typed wrappers (SessionId, RunId, ObjectId, CorrelationId) for AI workflows
- **WebView embedding** (optional, off by default) — CHOC-based native WebView panel with async JS bridge, structured native/web messages, and bundled-resource serving for Monaco editors, docs panels, and custom web UIs
- **Headless processing** — test and batch-process plugins without a DAW
- **A CLI tool** — `pulp build`, `pulp test`, `pulp validate`, `pulp create`, `pulp inspect`, `pulp audit`, `pulp ship`, `pulp status`, `pulp clean`, `pulp docs`
- **Code signing and packaging** — codesign, notarization, DMG/PKG (macOS), NSIS (Windows), .deb/.tar.gz (Linux), appcast
- **Python and Node.js bindings** — HeadlessHost via pybind11 and Node-API
- **MCP server** — AI agent parameter control, screenshot capture, view introspection
- **Accessibility** — NSAccessibility (macOS), UI Automation stubs (Windows), AT-SPI stubs (Linux)
- **A browser test host** — HTML/JS app for loading and interacting with WASM-compiled plugins
- **Eight plugin examples** — PulpGain, PulpTone, PulpEffect, PulpCompressor, PulpSynth, PulpDrums, PulpSampler, PulpPluck — plus GPU Demo, UI Preview, Design Tool, Showcase, and Web Demos
- **Automated tests** — unit tests, golden-file audio tests, format validation, UI component tests, DSP tests

## How It Is Organized

Pulp's code is organized into independent subsystems under `core/`:

| Subsystem | Purpose |
|-----------|---------|
| `runtime` | Lock-free primitives, logging, assertions, UUID/identity system |
| `events`  | Event loop, timers, async dispatch |
| `audio`   | Audio device I/O (CoreAudio, WASAPI, ALSA, JACK, Web Audio), buffers, file I/O |
| `midi`    | MIDI device I/O (CoreMIDI, Win32, ALSA, Web MIDI), MIDI 2.0 UMP, MPE, RPN/NRPN |
| `signal`  | 30+ DSP processors — filters, oscillators, effects, FFT, interpolation |
| `state`   | Parameters, presets, state serialization, undo/redo, change notification |
| `format`  | Plugin format adapters (VST3, AU, AUv3, CLAP, optional AAX, LV2, WAMv2, WebCLAP, standalone, headless) |
| `platform`| Platform detection, native dialogs, clipboard |
| `canvas`  | 2D drawing abstraction (CoreGraphics, Skia), SVG, effects |
| `render`  | GPU surface management (Dawn/Skia Graphite), render loop, SDL3 surface extraction |
| `view`    | Widgets, layout, themes, JS scripting, hot-reload, inspector, WebView, accessibility |
| `osc`     | Open Sound Control messaging |
| `host`    | Plugin hosting — scanner, plugin slot, signal graph for DAW-like apps |

Supporting directories:

| Directory   | Purpose |
|-------------|---------|
| `tools/`    | CLI, CMake modules, build utilities, browser host |
| `test/`     | Test framework and harnesses |
| `ship/`     | Code signing, notarization, packaging, appcast |
| `examples/` | Fourteen example plugins and demos |
| `external/` | Third-party dependencies |
| `apple/`    | Swift subsystems (Apple only) |
| `docs/`     | Documentation and YAML manifests |
| `ci/`       | CI helper scripts (main workflows in `.github/workflows/`) |

## Five Principles

Pulp is built on five principles:

1. **Permissive licensing** — MIT, no strings attached
2. **Modular architecture** — use only what you need
3. **Native platform experiences** — Swift on Apple, C++ core everywhere
4. **GPU-first rendering** — WebGPU via Dawn, Skia Graphite for 2D
5. **Modern development workflows** — AI-friendly, hot-reload, inspectable, browser-deployable
