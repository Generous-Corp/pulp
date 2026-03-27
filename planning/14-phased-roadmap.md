# Phased Development Roadmap

## Overview

This document defines the phased development plan for Pulp, from initial audit through public launch. Each phase has clear goals, deliverables, prerequisites, risks, clean-room considerations, acceptance criteria, and a demonstration target.

The roadmap is designed for a small team (2-4 engineers) with an estimated total duration of 12-18 months.

---

## Phase 0: Audit and Contamination Controls (Complete)

**Status:** Complete

**Goals:** Understand the audio plugin development landscape, establish clean-room boundaries, and produce specification documents that guide all subsequent development without contaminating the implementation.

**Deliverables:**
- Capability audit of the audited framework (what it does, not how it does it)
- Architecture specification for Pulp (independent design)
- Module decomposition and dependency graph
- Plugin format strategy (VST3, AU, AUv3, CLAP, LV2, AAX)
- Build system strategy (CMake + SPM)
- CI/CD plan (this document set)
- GPU integration strategy (Dawn/Skia Graphite; Visage evaluated as legacy alternative)
- Licensing strategy
- This phased roadmap
- Validation and testing plan

**Acceptance:** All audit and specification documents reviewed and approved by the project lead.

**Clean-Room Notes:** This phase involves studying the audited framework's public API surface and documentation to understand capabilities. No source code is examined. All observations are documented as behavioral capabilities, not implementation details.

**Duration:** 2-4 weeks

---

## Phase 1: Architecture and Repository Foundation

**Goals:** Establish the repository, build system, CI pipeline, and basic infrastructure needed for all subsequent phases.

**Deliverables:**

- **Monorepo structure**
  ```
  pulp/
    CMakeLists.txt              # Root build file
    Package.swift               # SPM manifest for Apple targets
    LICENSE.md                  # MIT License
    NOTICE                      # Third-party attributions
    CONTRIBUTING.md             # Contribution guidelines
    CLAUDE.md                   # AI-assisted development context
    .github/
      workflows/
        build.yml
        validate.yml
        sign-and-release.yml
    modules/
      pulp-platform/
      pulp-runtime/
      pulp-events/
      pulp-audio/
      pulp-midi/
      pulp-state/
      pulp-format/
      pulp-canvas/
      pulp-view/
      pulp-gpu/
      pulp-view-swift/
      pulp-build/
      pulp-signal/
      pulp-host/
    external/                   # Third-party dependencies
    templates/                  # Project scaffolding templates
    examples/                   # Example projects
    tests/                      # Integration tests
    docs/                       # Documentation
  ```

- **CMake build system skeleton**
  - Root `CMakeLists.txt` with project definition, C++20 standard, platform detection
  - Per-module `CMakeLists.txt` files (initially empty, defining targets)
  - `pulp_add_plugin()` function stub
  - `pulp_add_app()` function stub
  - Cross-platform toolchain configuration

- **SPM manifest** for Apple targets (Package.swift)

- **CI pipeline** (`build.yml`) that builds the empty framework skeleton on all platforms

- **CLAUDE.md** for AI-assisted development context and project conventions

- **MIT LICENSE.md** and initial NOTICE file

- **Contributing guidelines** (CONTRIBUTING.md) with DCO requirement

**Prerequisites:** Phase 0 complete.

**Risks:**
- Build system complexity: CMake + SPM + multiple platforms is inherently complex
- Directory structure decisions are hard to change later
- Mitigation: Keep initial structure minimal; expand as needed

**Clean-Room:** No risk. This phase involves only build system and project infrastructure, with no audio framework concepts.

**Acceptance:** Empty framework skeleton builds successfully on macOS (ARM64), Windows (x64), and Linux (x64). CI pipeline passes on all three platforms.

**Demo:** `cmake -B build && cmake --build build` succeeds on all platforms. GitHub Actions shows green builds.

**Duration:** 2-3 weeks

---

## Phase 2: Core Runtime and Platform Abstraction

**Goals:** Build the foundational layers that all other modules depend on: platform detection, runtime utilities, and the event system.

**Deliverables:**

- **pulp-platform**
  - Platform detection macros and constexpr queries (`pulp::platform::isMacOS()`, etc.)
  - Native handle types (`NativeWindowHandle`, `NativeViewHandle`)
  - Compiler and architecture detection
  - Endianness utilities

- **pulp-runtime**
  - Logging system (leveled, with platform-appropriate backends: os_log, OutputDebugString, stderr)
  - Lock-free utilities: SPSC queue, atomic value wrapper, lock-free FIFO
  - SIMD helpers: only where standard library is insufficient (e.g., aligned allocation helpers, SIMD dispatch)
  - Assertion and debug utilities
  - Scope guard and RAII utilities
  - NOT a standard library replacement: use `std::` for strings, containers, algorithms, smart pointers

- **pulp-events**
  - `EventLoop`: Non-singleton event loop (can create multiple instances; one per context)
  - `Timer`: One-shot and repeating timers, integrated with EventLoop
  - `AsyncDispatcher`: Dispatch work from any thread to the EventLoop's thread
  - Platform backends: Grand Central Dispatch (macOS), Windows message loop, epoll (Linux)
  - All event primitives are non-singleton -- the EventLoop instance is passed explicitly or via dependency injection, never accessed through a global

- **Unit tests** for all of the above using Catch2

**Prerequisites:** Phase 1

**Risks:**
- Over-engineering core utilities: resist the urge to build a full standard library replacement
- Event loop design is foundational; getting the API wrong here ripples through everything
- Mitigation: Start with the simplest API that supports the known use cases; iterate

**Clean-Room:** MEDIUM risk. The audited framework uses a singleton MessageManager pattern for its event loop. Pulp's `EventLoop` MUST be non-singleton and instance-based. This is an intentional architectural divergence, not just a naming difference.

**Acceptance:** EventLoop, Timer, and AsyncDispatcher work correctly on all three platforms. All unit tests pass. No singletons in the event system.

**Demo:** Console application that creates an EventLoop, schedules timers, dispatches async work across threads, and logs output.

**Duration:** 4-6 weeks

---

## Phase 3: Build and Project Scaffolding

**Goals:** Implement the `pulp_add_plugin()` CMake function, project templates, and the initial Claude Code plugin commands for project creation and building.

**Deliverables:**

- **pulp-build** (CMake module)
  - `pulp_add_plugin(target_name ...)` -- Creates a plugin target with format-specific build rules
  - `pulp_add_app(target_name ...)` -- Creates a standalone application target
  - `pulp_add_binary_data(target_name ...)` -- Embeds binary resources (images, presets, etc.) as C++ data
  - Format selection: `FORMATS VST3 AU CLAP Standalone` (developer chooses which formats to build)
  - Plugin metadata: `PLUGIN_NAME`, `PLUGIN_CODE`, `MANUFACTURER_CODE`, `MANUFACTURER_NAME`, `VERSION`, etc.
  - Install targets: platform-appropriate install locations for each format

- **Project template** (plugin starter)
  - Minimal plugin project with audio processing, parameter, and UI stubs
  - Ready to build on all platforms immediately after scaffolding
  - Includes `.github/workflows/` for CI

- **Basic /pulp:create command**
  - Interactive project scaffolding
  - Prompts for: project name, plugin type (effect/synth/midi), formats, manufacturer info
  - Generates complete buildable project from template

- **Basic /pulp:build command**
  - Wraps CMake configure + build
  - Detects when CMake regeneration is needed (CMakeLists.txt changed)
  - Reports build results clearly

**Prerequisites:** Phase 2

**Risks:**
- CMake function API design: must be clean and intuitive, not just a copy of existing patterns
- Template maintenance: templates must be kept in sync with framework changes
- Mitigation: Keep templates minimal; generate most boilerplate programmatically

**Clean-Room:** HIGH risk. The audited framework provides a CMake function (`juce_add_plugin`) with specific parameter names and conventions. Pulp's `pulp_add_plugin()` must use independently designed parameter names and a different organizational pattern. Review all parameter names against the audited framework before finalizing.

**Acceptance:** `/pulp:create` scaffolds a complete project that builds on all three platforms without manual editing. `/pulp:build` compiles the project successfully.

**Demo:** Create a minimal plugin project via Claude Code and build it, all within 5 minutes.

**Duration:** 3-4 weeks

---

## Phase 4: Audio/MIDI and Application Foundation

**Goals:** Implement audio device I/O, MIDI I/O, and standalone application support. This phase runs in parallel with Phase 3.

**Deliverables:**

- **pulp-audio**
  - Audio device enumeration (list available input/output devices)
  - Audio device selection and configuration (sample rate, buffer size, channel count)
  - Audio callback interface: developer provides a callback function that receives input buffers and fills output buffers
  - `AudioBuffer<T>`: Multi-channel audio buffer with zero-copy channel access, template on sample type (float, double)
  - Platform backends:
    - macOS: CoreAudio (AudioToolbox/AudioUnit framework)
    - Windows: WASAPI (low-latency shared and exclusive modes)
    - Linux: ALSA (with JACK as optional alternative)
  - Sample rate conversion utilities
  - Audio file reading/writing (WAV, AIFF at minimum; optionally FLAC, OGG)

- **pulp-midi**
  - MIDI device enumeration
  - MIDI input and output
  - `MidiEvent`: Represents a single MIDI message with timestamp
  - `MidiBuffer`: Collection of timestamped MIDI events within a buffer period
  - Platform backends:
    - macOS: CoreMIDI
    - Windows: Win32 MIDI (midiIn/midiOut) and UWP MIDI
    - Linux: ALSA sequencer

- **Standalone application wrapper**
  - Creates a native window with audio/MIDI device selection
  - Routes audio from device to plugin's processing callback and back
  - Routes MIDI from selected MIDI input to plugin
  - Basic settings persistence (last used device, sample rate, buffer size)

- **Audio test harness**
  - Generate test signals (sine, impulse, noise, sweep) programmatically
  - Capture output for analysis
  - Measure latency, verify signal integrity
  - Headless operation for CI

**Prerequisites:** Phase 2 (platform abstraction and event loop)

**Risks:**
- Platform-specific audio debugging: WASAPI exclusive mode has many edge cases; ALSA configuration varies wildly across Linux distributions
- Latency measurement: accurately measuring round-trip latency requires careful timer management
- Mitigation: Start with the simplest working implementation on each platform; optimize later

**Clean-Room:** LOW risk. Audio callback patterns (provide a function pointer that processes buffers of samples) are universal across all audio frameworks and are dictated by the underlying platform APIs (CoreAudio, WASAPI, ALSA). There is nothing proprietary about this pattern.

**Acceptance:** Standalone application plays audio (generated sine wave) on all three desktop platforms. MIDI input is received and triggers note events. Audio test harness runs in CI.

**Demo:** Sine wave generator standalone application with MIDI note control: pressing a MIDI key changes the pitch of the sine wave.

**Duration:** 6-8 weeks

---

## Phase 5: Plugin Format Support and Parameter/State Model

**Goals:** Implement plugin format adapters (VST3, AU, CLAP) and the parameter/state system. This is the most technically challenging phase.

**Deliverables:**

- **pulp-state**
  - `Parameter<T>`: Strongly-typed parameter with range, default, name, and string conversion
  - `ParameterGroup`: Hierarchical grouping of parameters (for organized DAW display)
  - `StateStore`: Centralized parameter storage with thread-safe access
  - `Binding<T>`: Reactive binding that notifies listeners when a parameter changes (used by UI)
  - State serialization: versioned binary format with forward compatibility
  - Host automation gesture support: begin/end change gestures for undo grouping

- **pulp-format**
  - **VST3 adapter:** Implements Steinberg's VST3 interfaces (`IComponent`, `IAudioProcessor`, `IEditController`, etc.) by wrapping Pulp's plugin interface. Built directly from the VST3 SDK documentation and headers.
  - **AU adapter:** Implements Apple's Audio Unit interfaces (`AUAudioUnit` subclass for AUv3, or `AudioComponentFactoryFunction` for AU v2) by wrapping Pulp's plugin interface. Built from Apple's Audio Unit documentation.
  - **CLAP adapter:** Implements the CLAP plugin entry point and extensions by wrapping Pulp's plugin interface. Built from CLAP specification and headers.
  - **Standalone adapter:** Wraps the plugin interface for standalone application use (from Phase 4).
  - **Headless adapter:** Wraps the plugin interface for headless processing (no UI, no audio device -- for batch processing and testing).

- **Headless processing mode**
  - Load plugin without UI or audio device
  - Feed audio buffers programmatically
  - Read output buffers programmatically
  - Used by test harness, CI validation, and batch processing tools

- **State serialization**
  - Binary format with version header
  - Forward compatibility: old versions can load new state (unknown parameters ignored)
  - Backward compatibility: new versions can load old state (missing parameters use defaults)
  - CRC or hash integrity check

- **Host automation gestures**
  - `beginParameterChange(id)` / `endParameterChange(id)` bracketing
  - DAWs use these to group parameter changes for undo/redo
  - Must be called on the correct thread per format specification

**Prerequisites:** Phase 4 (audio/MIDI infrastructure)

**Risks:**
- This is the hardest engineering phase. Plugin format specifications are complex and often under-documented.
- VST3 parameter ID stability: VST3 requires stable parameter IDs across plugin versions. The parameter system must enforce this.
- AU validation: Apple's `auval` is strict and tests many edge cases. Passing all tests requires careful implementation.
- Thread safety: Different formats have different threading models (VST3 separates processor and controller; AU may call from any thread).
- Mitigation: Start with one format (VST3), get it fully working and validated, then implement others.

**Clean-Room:** HIGH risk. Format adapter code is the area most likely to accidentally replicate patterns from existing frameworks. All adapter code must be implemented from the SDK documentation and headers directly. Do NOT study existing adapters from any framework. Each adapter should be reviewed for independent design before merging.

**Acceptance:**
- Plugin loads in at least three DAWs (Ableton Live, Logic Pro, Reaper) as VST3 and AU
- Parameters automate correctly (move a DAW knob, see the parameter change in the plugin and vice versa)
- State saves and restores correctly (save project, reload, all parameters are identical)
- Plugin passes `auval` (AU) and `pluginval` (VST3) validation
- CLAP plugin loads in Bitwig

**Demo:** Simple gain plugin with input gain, output gain, and bypass parameters. Passes auval, passes pluginval, loads and operates correctly in Ableton, Logic, and Reaper.

**Duration:** 8-12 weeks

---

## Phase 6: GPU Rendering Engine, JS Scripting, Design System, Inspector

**Goals:** Build the cross-platform UI system with Dawn/Skia Graphite GPU rendering, QuickJS scripting for widget logic, a design token system, component inspector, and SwiftUI alternative for Apple platforms. This is the largest subsystem by code volume.

**Deliverables:**

- **pulp-canvas** (2D rendering abstraction)
  - Platform backends:
    - All platforms: Skia Graphite (GPU-accelerated via Dawn/WebGPU)
    - macOS fallback: CoreGraphics (Quartz 2D)
    - Windows fallback: Direct2D
    - Linux fallback: Skia (software)
    - Web: WebGPU via Emscripten/WASM (see Phase 9)
  - Drawing API: paths, shapes, gradients, images, text
  - Font rendering (platform-native where possible, FreeType on Linux)
  - HiDPI/Retina support (backing scale factor handling)

- **Dawn/Skia Graphite integration**
  - Dawn as the WebGPU implementation (Metal, D3D12, Vulkan backends)
  - Skia Graphite for high-performance 2D rendering on top of Dawn
  - Unified GPU rendering path across all desktop platforms
  - Graceful fallback to CPU rendering if GPU is unavailable

- **QuickJS embedding**
  - QuickJS engine for JS-scripted widget definitions
  - JS widget API: define widget behavior, layout, and rendering in JavaScript
  - Hot-reload workflow: edit JS widget files, see changes instantly without recompilation
  - C++/JS bridge for parameter binding and event handling

- **pulp-view** (view hierarchy and widgets)
  - View base class with lifecycle (attach, detach, resize, paint, event handling)
  - Layout engine: flexbox-inspired layout (or CSS Grid-inspired) -- NOT a custom tree-walking algorithm
  - Core widgets:
    - `Knob` (rotary control with configurable arc, sensitivity, and value display)
    - `Fader` / `Slider` (linear control, horizontal and vertical)
    - `Toggle` / `Switch` (boolean control)
    - `TextInput` (single-line and multi-line)
    - `Label` (static and dynamic text)
    - `Button` (momentary and toggle)
    - `ComboBox` / `Dropdown` (selection from list)
    - `WaveformView` (audio waveform display, GPU-accelerated)
    - `SpectrumView` (frequency spectrum display, GPU-accelerated)
    - `XYPad` (2D parameter control)
    - `Meter` (level meter with peak hold, GPU-accelerated)
    - `Group` / `Panel` (container with optional border/title)
  - Audio visualization primitives (waveform, spectrum, meter) with GPU-accelerated rendering
  - Composable theme system:
    - Themes are data (colors, fonts, spacing, corner radii, etc.)
    - Themes can be composed and overridden per-view
    - Built-in themes: Light, Dark, and a "pro audio" dark theme
    - Custom themes via `/pulp:theme` command
  - Accessibility:
    - Platform accessibility APIs (NSAccessibility, UI Automation, AT-SPI)
    - All widgets expose accessible names, roles, and values
    - Keyboard navigation support

- **Design token system**
  - Centralized design token definitions (colors, spacing, typography, radii, shadows)
  - Export to JSON, CSS custom properties, C++ constants, OKLCH color definitions
  - Token pipeline: define once, consume across all rendering paths
  - AI-driven design session via `/pulp:design` command
  - Live token editing via `/pulp:inspect` component inspector

- **Component inspector**
  - `/pulp:inspect` command to attach to running plugins
  - Click-to-inspect: widget properties, bounding boxes, applied tokens, render stats
  - Live design token editing with immediate visual feedback
  - Render stats overlay (draw calls, GPU time per widget)

- **pulp-view-swift** (SwiftUI plugin UI -- Apple only)
  - SwiftUI view that hosts plugin parameters via Binding<T>
  - C++/Swift interop layer
  - `/pulp:setup-swift` command to enable SwiftUI UI
  - Works for both macOS and iOS (AUv3)

- **`/pulp:theme` command**
  - Generate custom themes from color palettes
  - Preview themes in a standalone test window
  - Export themes as code or data files

**Prerequisites:** Phase 5 (plugin format support, so UI can be embedded in DAW host windows)

**Risks:**
- UI framework is the largest subsystem -- scope management is critical
- Dawn integration complexity: building Dawn from source is non-trivial; binary distribution and platform-specific shader compilation add build system burden
- Skia Graphite is relatively new; API surface may evolve
- QuickJS embedding requires careful sandboxing and memory management at the C++/JS boundary
- SwiftUI/C++ interop: bridging Swift value types and C++ objects requires careful memory management
- Mitigation: Build core widgets first (Knob, Fader, Toggle); add advanced widgets incrementally. Pin Dawn and Skia to stable releases.

**Clean-Room:** HIGH risk. The audited framework's UI system (Component, ComponentPeer, LookAndFeel, Graphics) has specific patterns. Pulp must use independently designed class names, hierarchy structure, and rendering architecture. Specifically:
- No `Component` base class name (use `View`)
- No `LookAndFeel` pattern (use composable themes)
- No `Graphics` context passed to paint (use `Canvas` or similar)
- No `ComponentPeer` (use platform-specific `ViewHost` or `WindowBackend`)

**Acceptance:**
- Plugin with custom GUI loads in DAW and displays correctly
- All core widgets work (parameter-bound knob, fader, toggle)
- Dawn/Skia GPU rendering works on macOS, Windows, and Linux
- QuickJS hot-reload: modify a JS widget file, see the change in under 1 second
- Design tokens can be exported and re-imported in all supported formats
- Component inspector attaches to a running plugin and displays widget properties
- SwiftUI alternative works on macOS
- Theme system produces visually distinct themes
- Basic accessibility: widgets are announced by screen readers

**Demo:** Synthesizer plugin with knobs (filter cutoff, resonance), faders (ADSR envelope), GPU-accelerated waveform display and spectrum view. Hot-reload a JS widget change. Inspect a widget and edit a design token live. Available in both standard (CPU) and GPU-rendered variants.

**Duration:** 8-12 weeks

---

## Phase 7: Claude-Native Pulp Plugin

**Goals:** Build the full-featured Claude Code plugin that serves as the primary developer interface for Pulp.

**Deliverables:**

- **All /pulp: commands:**

  | Command | Description |
  |---------|-------------|
  | `/pulp:create` | Multi-stage interactive project scaffolding (name, type, formats, features) |
  | `/pulp:build` | Build project with CMake (with skip logic for unchanged config) |
  | `/pulp:test` | Run test suite |
  | `/pulp:ci` | CI pipeline interaction (build, validate, publish, secrets) |
  | `/pulp:port` | Guided migration from other frameworks |
  | `/pulp:ship` | Full release pipeline (tag, build, sign, publish) |
  | `/pulp:status` | Project status dashboard (build state, format support, dependencies) |
  | `/pulp:setup-gpu` | Enable Dawn/Skia GPU rendering |
  | `/pulp:setup-ios` | Enable iOS/AUv3 target |
  | `/pulp:setup-updates` | Configure auto-update system (Sparkle/WinSparkle) |
  | `/pulp:setup-swift` | Enable SwiftUI UI layer |
  | `/pulp:vm` | Cross-platform build via virtual machines |
  | `/pulp:website` | Generate product website (gh-pages) |
  | `/pulp:theme` | Theme creation and preview |

- **All skills (contextual activations):**

  | Skill | Trigger | Description |
  |-------|---------|-------------|
  | `pulp-starter` | New project detected | Guides initial project setup |
  | `pulp-render` | GPU-related discussion | Assists with Dawn/Skia/QuickJS integration |
  | `pulp-inspect` | Inspector/design discussion | Assists with component inspector and AI design workflow |
  | `pulp-swift` | SwiftUI-related discussion | Assists with SwiftUI integration |
  | `pulp-theme` | Theme-related discussion | Assists with theme creation |

- **New commands:**

  | Command | Description |
  |---------|-------------|
  | `/pulp:inspect` | Component inspector with live design token editing |
  | `/pulp:design` | AI-driven design session with natural language prompts |

- **CLI + MCP server dual mode**
  - All plugin commands operate as both standard CLI tools and MCP servers
  - MCP server mode enables any AI tool (Claude Code, Codex, Stitch, or others) to invoke Pulp workflows programmatically
  - Structured input/output for tool interoperability

- **Project scaffolding** with all feature combinations (effect, synth, MIDI effect, standalone app, with/without GPU, with/without Swift, with/without iOS)

- **Interactive configuration flows** with validation and error recovery

**Prerequisites:** Phases 5 and 6 (framework features must exist for the plugin to wrap them)

**Risks:**
- Command complexity: `/pulp:create` alone has 5 interactive stages
- Maintaining parity with the framework's capabilities (commands must work with all feature combinations)
- Mitigation: Build commands incrementally; start with `/pulp:create` and `/pulp:build`, add others as framework features are completed

**Clean-Room:** LOW risk. The Claude Code plugin is entirely new code with no analog in any existing audio framework. All command names, interaction patterns, and implementation are original.

**Acceptance:** Full project lifecycle manageable via Claude Code: create, build, test, sign, publish, auto-update. All commands handle error cases gracefully.

**Demo:** Complete workflow demonstration: create a project, add GPU UI, add iOS support, build, test, package, and release -- all via Claude Code commands.

**Duration:** 4-6 weeks

---

## Phase 8: CI/CD, Packaging, Release, Signing/Notarization

**Goals:** Production-ready build and distribution pipeline.

**Deliverables:**

- **GitHub Actions workflow templates**
  - `build.yml`: Multi-platform build and test
  - `validate.yml`: Plugin format validation
  - `sign-and-release.yml`: Signing, notarization, and release

- **macOS packaging and signing**
  - `.pkg` installer creation via `pkgbuild` + `productbuild`
  - Code signing with Developer ID certificates
  - Notarization via `notarytool`
  - Stapling notarization tickets

- **Windows packaging and signing**
  - Installer creation (NSIS or WiX)
  - Authenticode signing or Azure Trusted Signing
  - RFC 3161 timestamping

- **Linux packaging**
  - `.tar.gz` archive with install script
  - Optional `.deb` package generation

- **EdDSA signing for auto-updates**
  - Ed25519 keypair generation
  - Artifact signing
  - Signature verification in update client

- **Appcast generation**
  - Sparkle-compatible appcast XML (macOS)
  - WinSparkle-compatible appcast XML (Windows)
  - Appcast includes version, download URL, EdDSA signature, minimum OS version, release notes

- **`/pulp:ship` command** -- orchestrates the full release pipeline

- **`/pulp:ci secrets` sync** -- synchronizes local credentials to GitHub Secrets

- **Download page generation** -- static site on gh-pages with platform-detected download buttons

**Prerequisites:** Phase 7

**Risks:**
- Signing credential management: secrets must never be exposed in logs or process lists
- Notarization timing: Apple's notarization service can take 2-30 minutes, varying by load
- Cross-platform installer testing: must verify installers work on clean systems
- Mitigation: Extensive testing on fresh VMs; automated verification of signing and notarization

**Clean-Room:** LOW risk. All packaging, signing, and distribution uses standard platform tools (codesign, notarytool, signtool, pkgbuild, NSIS). Nothing proprietary or framework-specific.

**Acceptance:**
- Signed, notarized macOS `.pkg` installs cleanly on a fresh macOS system
- Signed Windows installer installs cleanly on a fresh Windows system
- Linux `.tar.gz` extracts and installs correctly
- Auto-update cycle completes (detect update, download, verify, install)
- All CI workflows pass on all platforms

**Demo:** Full publish cycle: tag a release, CI builds and signs on all platforms, artifacts appear in a GitHub Release, auto-update detects the new version and installs it.

**Duration:** 4-6 weeks

---

## Phase 9: Examples, Templates, Documentation, and Advanced Features

**Goals:** Build a developer-ready ecosystem with examples, documentation, and advanced features that extend Pulp's capabilities.

**Deliverables:**

- **Example projects:**
  - Simple effect (gain/EQ) -- minimal, well-commented
  - Simple synthesizer (subtractive synth with filter) -- demonstrates MIDI, parameters, UI
  - Standalone audio application -- demonstrates standalone mode without plugin formats
  - GPU UI demo -- demonstrates Dawn/Skia Graphite integration with animated UI and hot-reload
  - Swift UI demo -- demonstrates SwiftUI plugin UI on macOS

- **Project templates:**
  - Plugin (minimal) -- bare minimum buildable plugin
  - Plugin (full) -- plugin with UI, presets, auto-update
  - Application (standalone) -- standalone audio app
  - Console (headless) -- batch processing tool

- **API reference documentation**
  - Generated from source doc comments (Doxygen or similar)
  - Hosted on gh-pages or dedicated docs site
  - Searchable, cross-referenced

- **Developer guides:**
  - Getting Started Guide -- create your first plugin in 30 minutes
  - Architecture Guide -- how Pulp is structured and why
  - Migration Guide -- for developers coming from other frameworks (describes Pulp concepts, not how other frameworks work)
  - Theme Customization Guide
  - GPU UI Guide
  - iOS/AUv3 Guide
  - CI/CD and Distribution Guide

- **Web/WASM target**
  - Compile Pulp plugins and UIs to WebAssembly via Emscripten
  - Dawn/Skia Graphite renders via WebGPU in the browser
  - QuickJS widget logic runs natively in WASM
  - Enables web-based plugin demos, previews, and lightweight web audio applications

- **Advanced features:**
  - MIDI 2.0 / UMP (Universal MIDI Packet) support
  - MIDI-CI (MIDI Capability Inquiry)
  - MPE (MIDI Polyphonic Expression)
  - AUv3 for iOS (App Extension)
  - LV2 format adapter
  - AAX format adapter (requires developer-supplied SDK)
  - `pulp-signal` DSP library:
    - FFT (using platform-accelerated backends: vDSP, IPP, or FFTW)
    - Convolution (partitioned, zero-latency)
    - Filters (biquad, state-variable, ladder, comb, allpass)
    - Oversampling (2x, 4x, 8x with configurable anti-aliasing filters)
    - Envelope followers
    - Waveshaping
    - Delay lines (fractional delay with interpolation)
  - Plugin host support (`pulp-host`): load and process third-party plugins
  - Oboe/Android audio support (long-term)

**Prerequisites:** Phase 8

**Risks:**
- Documentation scope: comprehensive documentation is a massive effort
- Android platform support adds significant complexity
- AAX SDK licensing restricts what can be included in the repository
- DSP library quality: audio DSP must be numerically correct and efficient
- Mitigation: Prioritize documentation that unblocks developers (Getting Started, Architecture); add advanced features incrementally

**Clean-Room:** MEDIUM risk for the DSP library. Filter designs are mathematical (bilinear transform, topology, etc.) and not proprietary, but the API shape (class names, method signatures) must be original. Review DSP API surface against existing frameworks before finalizing.

**Acceptance:** A new developer can create, build, and distribute a plugin following only Pulp documentation, without needing external references.

**Demo:** Complete example project (synthesizer with UI, presets, auto-update) built and distributed, with accompanying documentation.

**Duration:** 8-12 weeks

---

## Phase 10: Hardening, Parity Validation, and Public Launch

**Goals:** Achieve production quality, validate against the capability matrix, and execute a public launch.

**Deliverables:**

- **Comprehensive test suite**
  - Unit tests for all modules
  - Integration tests (plugin loads in DAW, processes audio, saves/restores state)
  - Format validation (auval, pluginval, clap-validator, lv2lint)
  - Visual regression tests (screenshot comparison for UI)
  - Fuzz tests (corrupted state data, malformed MIDI, extreme parameter values)

- **Performance benchmarks**
  - Audio callback jitter: < 1ms variance at 256-sample buffer size
  - UI render time: < 16ms per frame (60 FPS) for CPU, < 8ms (120 FPS) for GPU
  - Build time (incremental): < 10 seconds
  - Build time (clean): < 120 seconds
  - Plugin scan time: < 100ms per plugin
  - State save/load: < 10ms for typical plugin state

- **Parity validation**
  - Verify every v1-required capability from the capability matrix is implemented and tested
  - Black-box behavioral testing only (test what it does, not how it does it)
  - Document any intentional deviations from parity (with rationale)

- **Security audit**
  - Signing credential handling reviewed
  - No secrets in logs, process lists, or artifacts
  - EdDSA verification logic reviewed for correctness
  - State deserialization reviewed for buffer overflows and memory safety

- **Beta testing**
  - 3 or more real plugin projects built with Pulp
  - Beta testers from the audio development community
  - Bug reports triaged and fixed
  - Performance validated in real-world DAW sessions

- **Public launch**
  - GitHub repository made public
  - Documentation site live
  - Claude Code plugin published to marketplace
  - Launch announcement (blog post, social media, audio developer forums)
  - Example plugins available for download

**Prerequisites:** Phase 9

**Risks:**
- Edge cases in DAW compatibility (each DAW has its own quirks)
- Performance regressions discovered during beta testing
- Community reception and feedback volume
- Mitigation: Extended beta period; prioritize DAW-specific fixes; establish clear issue triage process

**Clean-Room:** Final contamination review before public release. All source files reviewed for any references to or patterns from the audited framework. API surface reviewed for independent design.

**Acceptance:**
- 3 or more real plugins built and distributed using Pulp
- All plugin format validators pass
- All performance benchmarks met
- Documentation complete and verified by new developers
- No known critical bugs

**Demo:** Public launch with working plugins available for download, documentation site live, and Claude Code plugin available in the marketplace.

**Duration:** 4-8 weeks

---

## Timeline Summary

| Phase | Description | Duration | Cumulative |
|-------|-------------|----------|------------|
| 0 | Audit and Contamination Controls | 2-4 weeks | 2-4 weeks |
| 1 | Architecture and Repo Foundation | 2-3 weeks | 4-7 weeks |
| 2 | Core Runtime and Platform Abstraction | 4-6 weeks | 8-13 weeks |
| 3 | Build and Project Scaffolding | 3-4 weeks | 11-17 weeks* |
| 4 | Audio/MIDI and App Foundation | 6-8 weeks | 14-21 weeks* |
| 5 | Plugin Format Support and Parameters | 8-12 weeks | 22-33 weeks |
| 6 | GPU Rendering Engine, JS Scripting, Design System, Inspector | 8-12 weeks | 30-45 weeks |
| 7 | Claude-Native Pulp Plugin | 4-6 weeks | 34-51 weeks |
| 8 | CI/CD, Packaging, Release | 4-6 weeks | 38-57 weeks |
| 9 | Examples, Docs, Advanced Features | 8-12 weeks | 46-69 weeks |
| 10 | Hardening and Public Launch | 4-8 weeks | 50-77 weeks |

*Phases 3 and 4 run in parallel after Phase 2, so cumulative time reflects the longer of the two.

**Total estimated duration: 12-18 months** for a team of 2-4 engineers.

---

## Dependency Graph

```
Phase 0 (Audit)
  |
  v
Phase 1 (Repo Foundation)
  |
  v
Phase 2 (Core Runtime)
  |
  +----------+----------+
  |                      |
  v                      v
Phase 3 (Build/Scaffold)  Phase 4 (Audio/MIDI)
  |                      |
  +----------+----------+
             |
             v
       Phase 5 (Plugin Formats + Parameters)
             |
             v
       Phase 6 (GPU Rendering + JS + Design + Inspector)
             |
             v
       Phase 7 (Claude Code Plugin)
             |
             v
       Phase 8 (CI/CD + Packaging)
             |
             v
       Phase 9 (Examples + Docs + Advanced)
             |
             v
       Phase 10 (Hardening + Launch)
```

### Parallelism Opportunities

- **Phases 3 and 4** can run fully in parallel (no dependencies between them)
- **Phase 6 UI work** can begin partially during Phase 5 (canvas and view hierarchy do not depend on plugin format adapters; only embedding in DAW host windows requires Phase 5)
- **Documentation** (Phase 9) can begin incrementally during earlier phases
- **CI/CD** (Phase 8) groundwork can be laid during Phase 1 and refined incrementally

### Critical Path

The critical path runs through: Phase 0 -> 1 -> 2 -> 4 -> 5 -> 6 -> 7 -> 8 -> 9 -> 10

Phase 5 (plugin formats) is the highest-risk item on the critical path and receives the longest time allocation.
