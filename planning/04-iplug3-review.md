# iPlug3 Inspiration Review

**Subject:** Architectural analysis of iPlug2 and iPlug3 for Pulp design inspiration
**Sources:** iPlug2 GitHub repository, iPlug3 organization manifesto, Patreon manifesto, community research
**Date:** 2026-03-24

---

## Table of Contents

1. [Background](#background)
2. [Two-Pillar Architecture](#two-pillar-architecture)
3. [Graphics Stack](#graphics-stack)
4. [C++20 Concepts for Plugin Interfaces](#c20-concepts-for-plugin-interfaces)
5. [Agentic-First Philosophy](#agentic-first-philosophy)
6. [Plugin Format Support](#plugin-format-support)
7. [Parameter System](#parameter-system)
8. [Build and Dependency Strategy](#build-and-dependency-strategy)
9. [Platform Support](#platform-support)
10. [Ideas Worth Studying](#ideas-worth-studying)
11. [Ideas Worth Avoiding](#ideas-worth-avoiding)
12. [Possible Patterns to Adapt for Pulp](#possible-patterns-to-adapt-for-pulp)
13. [Differences in Philosophy vs the Audited Framework](#differences-in-philosophy-vs-the-audited-framework)
14. [Alignment with Pulp Goals](#alignment-with-pulp-goals)
15. [Key Takeaways](#key-takeaways)

---

## Background

### iPlug2: Mature and Shipping

iPlug2 is a mature, actively maintained audio plugin framework created by Oli Larkin (ex-Ableton, 5 years at the company). It has been shipping since 2018 and powers numerous commercial plugins.

| Metric | Value |
|--------|-------|
| GitHub Stars | 2,267 |
| Forks | 338 |
| License | Custom zlib-like (highly permissive) |
| Primary Language | C++ |
| C++ Standard | C++17 |
| Last Activity | Active (commits as of March 2026) |
| Lead Developer | Oli Larkin |
| Key Contributors | Alex Harker |

**Notable Users:**
- Full Bucket Music (30+ plugins)
- Surreal Machines
- Neural Amp Modeler
- Forever 89

**Community:**
- Discord server (with #iplug3 channel)
- Discourse forum
- Patreon-supported development
- Open Collective donations

### iPlug3: Greenfield Rewrite

iPlug3 is a complete rewrite started **January 1, 2026**. It is not a refactor of iPlug2 but a clean-sheet design informed by 7+ years of iPlug2 experience. As of March 2026, the actual framework code is **not publicly available**.

What exists publicly:
- **Organization profile** at `github.com/iPlug3` with architectural manifesto
- **skia-builder** repository — pre-built Skia binaries for multiple platforms (MIT licensed)
- **audio-plugin-dev-skills** — Claude Code skills for audio plugin development (47 stars)
- **Patreon manifesto** — detailed architectural vision and design rationale

The framework is being developed privately with an expected v0.0.1 release at an unspecified date.

**Critical Assessment:** iPlug3's ideas are compelling but unproven. No shipping code exists. Every claim about performance, developer experience, and capability is aspirational until public release. Pulp should study the architectural ideas but cannot evaluate the execution.

---

## Two-Pillar Architecture

The most significant architectural idea from the iPlug lineage is the **clean separation of audio plugin logic from graphics/UI**.

### iPlug2 Implementation

iPlug2 has two distinct subsystems:

| Pillar | Name | Purpose |
|--------|------|---------|
| Plugin Core | IPlug | Audio processing, parameter management, MIDI, state, format adapters |
| Graphics Engine | IGraphics | Rendering, widgets, windowing, input handling |

These are **separable** — you can use IPlug without IGraphics and attach any UI framework (SwiftUI, HTML/CSS, Qt, etc.). Format adapters live as subdirectories under IPlug (VST3/, AU/, CLAP/, etc.). An editor delegate interface bridges the two pillars.

### iPlug3 Formalization

iPlug3 formalizes this split into two independent frameworks:

| Framework | Name | Description |
|-----------|------|-------------|
| Plugin Core | MPLUG ("Micro Plug-in Abstraction") | Headless plugin core, pure C++20/STL, no platform dependencies |
| Graphics Engine | MGFX ("Massive Graphics Abstraction") | Standalone graphics/UI framework built on SDL3 + WebGPU (Dawn) + Skia Graphite |

MGFX is explicitly designed to be usable independently — for games, interactive art, visualizers, and tools — not just audio plugin UIs. MPLUG uses pure C++20/STL with zero legacy dependencies.

### Why This Matters for Pulp

The two-pillar architecture is the **proven correct pattern** for audio plugin frameworks. It solves several critical problems:

1. **Headless testing:** Plugin logic can be tested without any graphics infrastructure
2. **UI flexibility:** Developers can choose their own UI framework (SwiftUI, WebView, custom Metal, etc.)
3. **Format purity:** Plugin format adapters don't need to know about rendering
4. **Build isolation:** Graphics-heavy builds don't affect DSP compilation
5. **Cross-platform strategy:** The DSP core can be identical everywhere while the UI adapts per platform

**Contrast with the audited framework:** The audited framework couples `AudioProcessor` to `AudioProcessorEditor` through `juce_audio_processors` depending on `juce_gui_extra`, which pulls in the entire GUI chain. The `_headless` module split was a late mitigation, not a clean separation.

**Pulp should adopt this pattern from day one:** A pure C++ plugin/DSP core with zero UI dependencies, and a separate (optional) UI layer that can be SwiftUI on Apple, a custom renderer on other platforms, or nothing at all.

---

## Graphics Stack

### iPlug2 Graphics

iPlug2 offers two rendering backends:

| Backend | Technology | Characteristics |
|---------|-----------|----------------|
| NanoVG | OpenGL-based vector graphics | Lightweight, simple, fast for basic UIs |
| Skia | Google's 2D rendering engine | Full-featured, GPU-accelerated, production-quality |

Platform windowing uses native implementations:
- macOS: NSView
- Windows: HWND
- iOS: UIView
- Linux: X11
- Web: Canvas/Emscripten

The widget library provides purpose-built audio controls (knobs, sliders, meters, keyboards, spectrum analyzers, VU meters, LED controls) with "IV" prefix for vector-drawn controls. Controls are themeable via style structs.

### iPlug3 Graphics (MGFX)

MGFX represents a significant leap forward:

| Component | Technology | Purpose |
|-----------|-----------|---------|
| GPU API | WebGPU (via Dawn) | Cross-platform GPU abstraction |
| 2D Renderer | Skia Graphite | Hardware-accelerated 2D rendering on WebGPU |
| Windowing | SDL3 | Cross-platform window management and input |
| Scripting | QuickJS / V8 / JSC | JavaScript UI scripting with Canvas-like API |

**Key Capabilities:**
- 120 FPS target rendering
- Immediate-mode + retained drawing model with real-time animation and scrolling
- Post-processing effects (bloom, blur, CRT simulation) via GPU
- SkSL runtime fragment shaders for custom visual effects
- WebGPU compute shaders for GPU-accelerated audio processing (additive synthesis, FFT, physical modeling, ML inference)
- Hot-reloading for JavaScript UI code
- Multi-window support
- Offline video rendering mode (frame-by-frame AV content creation)
- Built-in accessibility support
- Extensive themeable widget library for audio UIs

**Comparison to the Audited Framework:**

| Aspect | Audited Framework | iPlug3 MGFX |
|--------|------------------|-------------|
| Primary renderer | Software / CoreGraphics / Direct2D | WebGPU + Skia Graphite |
| GPU acceleration | OpenGL (deprecated on Apple) | WebGPU (modern, cross-platform) |
| Target framerate | VBlank (typically 60 FPS) | 120 FPS |
| Shader support | None built-in | SkSL + WebGPU compute |
| Post-processing | None | Bloom, blur, CRT, custom |
| Hot-reload | None | JavaScript UI hot-reload |
| Video rendering | None | Offline frame-by-frame |

MGFX is state-of-the-art for cross-platform GPU-accelerated rendering in an audio context. It is far ahead of the audited framework's OpenGL approach.

---

## C++20 Concepts for Plugin Interfaces

One of iPlug3's most architecturally significant decisions is using **C++20 concepts** instead of virtual inheritance for the plugin interface.

### The Problem with Virtual Inheritance

Traditional audio plugin frameworks (including the audited one) define plugin interfaces via abstract base classes:

```
// Traditional approach (pseudocode)
class PluginBase {
    virtual void prepare(double sampleRate, int blockSize) = 0;
    virtual void process(AudioBuffer& buffer) = 0;
    virtual void getState(MemoryBlock& data) = 0;
    virtual void setState(const void* data, int size) = 0;
    // ... 30+ virtual methods
};
```

**Problems:**
- Virtual function call overhead on every audio callback (v-table lookup, branch misprediction)
- Cannot be optimized by the compiler (no inlining through virtual calls)
- Inheritance couples plugins to the framework's class hierarchy
- Adding methods to the base class is a breaking change

### The Concepts Approach

iPlug3 uses C++20 concepts to define what a plugin must provide:

```
// Concepts approach (pseudocode, illustrative)
template <typename T>
concept Plugin = requires(T p, double sr, int bs) {
    { p.prepare(sr, bs) } -> std::same_as<void>;
    { p.process(/* buffer */) } -> std::same_as<void>;
    // ... requirements
};
```

**Advantages:**
- **Zero overhead:** No virtual dispatch, no v-table lookup on the audio thread
- **Compile-time validation:** Errors at compile time if a plugin doesn't meet the interface
- **No inheritance coupling:** Plugins are plain structs/classes that happen to satisfy the concept
- **Inlining opportunity:** The compiler can see through the interface and optimize
- **Composability:** Concepts can be combined with `&&` to require multiple interfaces

### Relevance to Pulp

Pulp has an additional option not available to iPlug3: **Swift protocols** on Apple platforms. Swift protocols provide similar benefits to C++20 concepts:

| Feature | C++20 Concepts | Swift Protocols |
|---------|---------------|----------------|
| Compile-time interface checking | Yes | Yes |
| Zero-cost when statically dispatched | Yes | Yes (with generics) |
| Dynamic dispatch available | Via std::any/type erasure | Via existentials (`any Protocol`) |
| Composability | `Concept1 && Concept2` | `Protocol1 & Protocol2` |
| Default implementations | Via CRTP or templates | Via protocol extensions |

Pulp should use C++20 concepts for the cross-platform C++ core and Swift protocols for the Apple-specific Swift layer.

---

## Agentic-First Philosophy

iPlug3's manifesto introduces a novel design philosophy: the framework should be designed **for AI-assisted development** from the ground up.

### Design Principles

| Principle | How It Manifests |
|-----------|-----------------|
| **Fast builds** | Minimal dependencies, pre-built binaries, small compilation units. AI agents iterate faster with quick feedback loops. |
| **Screenshot-based testing** | Render plugin UI offscreen, capture screenshot. Multimodal LLMs can visually validate UI without running a DAW. |
| **MCP server integration** | Plugins can expose themselves as MCP servers, allowing AI agents to programmatically control plugin parameters, inspect state, and validate behavior. |
| **CLAUDE.md context files** | Repository includes structured context files that AI coding agents read to understand the project. |
| **Parameterize everything** | Configuration via data (not code) so agents can modify behavior without deep code understanding. |
| **Claude Code skills** | The `audio-plugin-dev-skills` repository provides Claude Code skills specifically for audio plugin development with iPlug3. |

### Assessment

This is genuinely **novel and forward-thinking**. No other audio plugin framework has designed for AI-assisted development as a primary concern. The juce-dev plugin (audited separately) achieves some of this through Markdown instructions, but iPlug3 embeds agentic-first thinking into the framework architecture itself.

However, some caution is warranted:

| Claim | Assessment |
|-------|-----------|
| "CMake is the kind of task you hand to an agent" | True, but agents still need well-structured CMake to modify |
| "Screenshot-based testing" | Promising but unproven at scale — pixel-perfect comparison is fragile |
| "MCP server integration" | Novel, but adds complexity; unclear adoption |
| "CLAUDE.md context files" | Simple and effective — Pulp should adopt this immediately |

### Relevance to Pulp

Pulp should adopt the agentic-first philosophy:
1. **CLAUDE.md** in the repository for agent context
2. **Fast builds** as a design constraint
3. **Screenshot capture** for visual validation in CI
4. **Structured configuration** (`.env`, JSON, YAML) that agents can parse and modify
5. **MCP server** for framework tooling (project creation, build, test)

The juce-dev plugin already demonstrates the Markdown-instruction approach to agentic-first development. Pulp should build on this foundation while also embedding agentic-first thinking into the framework's own architecture (fast compilation, testability, inspectability).

---

## Plugin Format Support

### iPlug2 Format Matrix

| Format | Status | Notes |
|--------|--------|-------|
| CLAP | Supported | Modern, permissive |
| VST3 | Supported | Industry standard |
| VST2 | Supported (deprecated) | Legacy, requires Steinberg SDK |
| AU v2 | Supported | macOS standard |
| AUv3 | Supported | macOS/iOS standard |
| AAX | Supported (native) | Pro Tools, requires Avid SDK |
| WAM v1 | Supported | Web Audio Module |
| Standalone | Supported | Via RTAudio/RTMidi |
| REAPER Extension | Supported | REAPER-specific API |

### iPlug3 Planned Additions

| Format | Status | Notes |
|--------|--------|-------|
| All iPlug2 formats | Carried forward | — |
| WebCLAP | New | Portable WASM plugin format |
| WAM v2 | Upgraded | Updated Web Audio Module spec |
| Python module | New | Plugin as importable Python module |
| Node.js module | New | Plugin as importable Node.js module |
| MCP server | New | Plugin as AI tool endpoint |

### Notable Observations

- **CLAP as a first-class format:** Unlike the audited framework (which requires community patches for CLAP), both iPlug2 and iPlug3 treat CLAP as a primary format
- **Web as a deployment target:** WAM and WebCLAP support means plugins can run in browsers — something the audited framework does not support at all
- **Format adapters as thin wrappers:** Each format adapter translates between the format's C API and the core plugin abstraction, keeping the adapter code minimal
- **REAPER Extension format:** Unique to iPlug, allows building REAPER extensions (not just plugins) from the same codebase

### Relevance to Pulp

Pulp should support these formats at a minimum:
- **VST3** — industry standard (MIT-licensed SDK)
- **AU / AUv3** — Apple platform standard (Apache 2.0 SDK)
- **CLAP** — modern, permissive, growing adoption
- **Standalone** — essential for development and distribution
- **LV2** — Linux standard (ISC-licensed SDK)

Later additions to consider:
- WebCLAP/WAM — web deployment
- AAX — Pro Tools (separate module due to licensing)

---

## Parameter System

### iPlug2 Approach

iPlug2's parameter system differs significantly from the audited framework:

| Aspect | iPlug2 | Audited Framework |
|--------|--------|------------------|
| Value storage | **Non-normalized** (actual values, e.g., Hz, dB) | Normalized 0-1 range |
| Parameter count | Fixed at compile time | Dynamic (can add at runtime) |
| Indexing | Enum-based integer indices | String ID + integer index |
| Type | `IParam` with double precision | Typed subclasses (Float, Int, Bool, Choice) |
| Shapes | Custom shapes for non-linear mapping | `NormalisableRange` with skew |
| Creation | Single line of code | Multi-line verbose setup |
| Control linking | Single line | Attachment objects |

### Non-Normalized Values

The most significant design difference is that iPlug2 stores **actual parameter values** internally (e.g., frequency = 440.0 Hz) rather than normalized 0-1 values. Normalization happens at the **format adapter boundary** when communicating with the host.

**Advantages:**
- More intuitive for developers (think in Hz, dB, ms — not 0.0-1.0)
- Less error-prone (no manual denormalization)
- Parameter value directly usable in DSP code
- Display formatting is straightforward

**Disadvantages:**
- Must convert to/from normalized at the host boundary
- Some hosts expect normalized values in certain callbacks

### Relevance to Pulp

Pulp should consider the non-normalized approach:
- Store actual values in the parameter system
- Perform normalization only at the format adapter boundary
- Define parameter ranges and shapes with actual value units
- One-line parameter creation for common types

---

## Build and Dependency Strategy

### iPlug2 Build System

| Aspect | Details |
|--------|---------|
| Primary | CMake |
| Legacy | .xcconfig (macOS), .props (Windows), .mk (WASM) |
| C++ Standard | C++17 |
| CI | Azure Pipelines |
| Dependencies | Download script (`download-prebuilt-libs.sh`) |
| Presets | CMakePresets.json |

### iPlug3 Build System

| Aspect | Details |
|--------|---------|
| Primary | CMake only (no legacy configs) |
| C++ Standard | C++20 |
| CI | Not documented yet |
| Dependencies | CMake FetchContent + pre-built binaries via `skia-builder` |
| Philosophy | "CMake is the kind of task you hand to an agent" |

### Pre-Built Dependency Distribution

The `skia-builder` repository demonstrates a pragmatic approach to large dependencies:

1. GitHub Actions builds Skia for all target platforms (macOS arm64/x86_64, Windows x64, Linux x64)
2. Pre-built binaries are published as GitHub releases
3. CMake FetchContent downloads the appropriate pre-built binary at configure time
4. Developers never need to build Skia themselves

This pattern is valuable for any framework that depends on large C++ libraries (Skia, Dawn, ICU, etc.).

### Relevance to Pulp

- **CMake only** for cross-platform C++ code (no Projucer, no legacy build configs)
- **SPM** for Apple-specific Swift code
- **Pre-built binaries** for large dependencies (if Pulp uses Skia, Dawn, etc.)
- **No legacy build config files** — one build system, one way
- **C++20** as the minimum standard (concepts, ranges, modules eventually)

---

## Platform Support

### iPlug2 Platform Matrix

| Platform | Audio | Graphics | Plugin Formats |
|----------|-------|----------|---------------|
| macOS 10.13+ | CoreAudio | NanoVG/Skia (NSView) | AU, AUv3, VST3, AAX, CLAP, Standalone |
| Windows 8+ | WASAPI/ASIO | NanoVG/Skia (HWND) | VST3, AAX, CLAP, Standalone |
| iOS 15+ | CoreAudio | NanoVG/Skia (UIView) | AUv3, Standalone |
| visionOS 26.0+ | CoreAudio | Skia | AUv3 |
| Linux | JACK/ALSA | NanoVG/Skia (X11) | VST3, CLAP, LV2, Standalone |
| Web/WASM | Web Audio API | Canvas (Emscripten) | WAM |
| Windows ARM64EC | WASAPI | Skia | VST3, CLAP |

### iPlug3 Platform Matrix (Planned)

All iPlug2 platforms plus:
- Linux as a **first-class target** (not an afterthought)
- Android: **TBC** (to be confirmed — not committed)
- Web/WASM as a **first-class citizen** (not a secondary target)

### Platform Abstraction Strategy

| Layer | iPlug2 | iPlug3 |
|-------|--------|--------|
| Windowing | Per-platform native code | SDL3 (cross-platform) |
| GPU | OpenGL (NanoVG) or Skia GPU | WebGPU via Dawn (cross-platform) |
| Audio I/O | RTAudio/RTMidi | Not documented (likely platform APIs) |

### Relevance to Pulp

Pulp should target:
- **macOS** (first-class, Swift + C++)
- **iOS/iPadOS** (first-class, Swift + C++)
- **Windows** (first-class, C++)
- **Linux** (first-class, C++)
- **Android** (from day one — where iPlug3 is uncertain)
- **Web/WASM** (future consideration)

---

## Ideas Worth Studying

These ideas from iPlug2/3 deserve careful study and potential adoption by Pulp:

### 1. Two-Pillar Architecture
The clean separation of headless plugin core from graphics engine is the single most important architectural pattern. Pulp should formalize this as two independent libraries/packages from the start.

### 2. WebGPU as Cross-Platform GPU API
Dawn (Google's WebGPU implementation) provides a modern, cross-platform GPU abstraction that maps to Metal (Apple), D3D12 (Windows), and Vulkan (Linux). It is the most forward-looking choice for cross-platform GPU rendering.

### 3. C++20 Concepts for Interfaces
Using concepts instead of virtual inheritance for the plugin interface provides zero-overhead abstraction with compile-time validation. Pulp can pair this with Swift protocols on Apple platforms.

### 4. Agentic-First Design
Designing the framework for AI-assisted development (fast builds, screenshot testing, MCP integration, structured context files) is genuinely novel and aligns perfectly with Pulp's goals.

### 5. Non-Normalized Parameter Values
Storing actual values (Hz, dB, ms) instead of normalized 0-1 values is more intuitive and less error-prone. Normalization at the format adapter boundary is the correct architecture.

### 6. Pre-Built Dependency Distribution
Using GitHub Actions to build large dependencies and distribute pre-built binaries via FetchContent eliminates a major friction point for developers.

### 7. IPC-Based Graphics
Running the graphics engine in a separate process (or thread with IPC) from the audio plugin allows:
- Independent crash isolation (UI crash doesn't kill audio)
- Independent update of the UI without reloading the plugin
- Testing the UI without loading it in a DAW

### 8. Permissive Licensing
iPlug2's zlib-like license is a model for Pulp. Zero cost, zero restrictions on commercial use, no revenue thresholds. This is a major competitive advantage over the audited framework's AGPL/Commercial model.

### 9. Web as a Deployment Target
WASM compilation for Web Audio Modules and WebCLAP opens entirely new distribution channels (browser-based DAWs, collaborative tools, educational platforms).

### 10. Hot-Reloading for UI Development
JavaScript UI hot-reload (iPlug3) and DSL hot-reload (FAUST/Cmajor) enable rapid iteration cycles that traditional C++ recompilation cannot match.

### 11. Screenshot-Based Visual Testing
Rendering the plugin UI offscreen and capturing screenshots enables:
- CI-based visual regression testing
- Multimodal LLM visual validation
- Automated UI testing without a DAW

### 12. Tiny Binary Footprint Philosophy
iPlug2 produces plugin binaries in the hundreds of KB range, compared to the audited framework's 5MB+. Designing for small output size is a worthwhile constraint.

---

## Ideas Worth Avoiding

### 1. Being Vaporware
iPlug3 has been in development since January 2026 with no public release. All claims are unverified. Pulp should ship working code before announcing grand visions.

### 2. Heavyweight Mandatory Graphics Stack
iPlug3 explicitly chose Dawn + Skia Graphite as the only graphics option: "I want to focus on the best — not worry about making it lightweight." This means every plugin must pull in these large dependencies even for a simple gain knob. Pulp should offer **tiered graphics options**:
- No UI (headless)
- Platform-native UI (SwiftUI on Apple, etc.)
- Custom GPU-rendered UI (Metal/WebGPU)

### 3. Single Primary Maintainer
iPlug2/3 development is bottlenecked by one person (Oli Larkin, with Alex Harker contributing). Pulp should be designed for community contribution from day one: clear module boundaries, documented architecture, contribution guidelines.

### 4. Legacy WDL Dependency
iPlug2's reliance on Cockos WDL (custom string classes, containers) is a long-term tax. AI agents struggle with WDL's non-standard types. Pulp should use standard library types exclusively.

### 5. Undecided Licensing
iPlug3's license is "TBD" — creating uncertainty for potential adopters. Pulp should commit to a clear, permissive license from day one (MIT, Apache 2.0, or zlib).

### 6. Poor Documentation Culture
iPlug2's "learn from examples" approach doesn't scale. The documentation is sparse, and deep features are poorly explained. Pulp needs documentation-first culture: every public API documented, every pattern explained, every module has a guide.

### 7. In-Source Build Default
iPlug2's `duplicate.py` approach created projects inside the framework source tree. Always default to out-of-source builds.

### 8. Multiple Build Config Formats
iPlug2 ships `.xcconfig`, `.props`, `.mk`, and CMake files. This creates confusion about what's canonical. Pulp should have exactly one build system: CMake for C++, SPM for Swift.

---

## Possible Patterns to Adapt for Pulp

### 1. Headless Core / Pluggable Renderer Split

```
pulp-core (C++20, cross-platform):
  ├── DSP Engine
  ├── Parameter System
  ├── State Serialization
  ├── MIDI Processing
  ├── Format Adapters (VST3, AU, CLAP, LV2)
  └── Lock-free Utilities

pulp-ui (optional, per-platform):
  ├── Apple: SwiftUI + Metal
  ├── Windows: Custom renderer (D3D12 or WebGPU)
  ├── Linux: Custom renderer (Vulkan or WebGPU)
  └── Cross-platform: WebGPU + Skia (optional heavyweight path)
```

### 2. Concept-Based Plugin Interfaces

For the C++ core, define plugin requirements as concepts:
- `AudioPlugin` concept: prepare, process, getState, setState
- `MidiPlugin` concept: extends AudioPlugin with MIDI processing
- `ParameterizedPlugin` concept: parameter enumeration and access
- `EditablePlugin` concept: creates an editor (optional, only if UI is linked)

For the Swift layer, mirror these as protocols:
- `AudioPlugin` protocol
- `ParameterizedPlugin` protocol
- `EditablePlugin` protocol

### 3. Screenshot-Based Visual Testing

Integrate into the CI pipeline:
1. Build plugin in headless mode
2. Create editor window offscreen
3. Set known parameter values
4. Capture screenshot
5. Compare against golden reference image
6. Fail CI if pixel difference exceeds threshold

Optionally: send screenshot to a multimodal LLM for qualitative assessment ("Does this look like a synthesizer plugin?").

### 4. MCP Server for Framework Tooling

Expose Pulp development commands as an MCP server:
- `pulp.create` — create new project
- `pulp.build` — build project
- `pulp.test` — run tests
- `pulp.validate` — validate plugin format compliance
- `pulp.screenshot` — capture UI screenshot
- `pulp.parameters` — list/get/set parameters

This enables any MCP-compatible AI agent (not just Claude) to develop Pulp plugins.

### 5. Lock-Free DSP-to-UI Communication

Adapt iPlug2's `ISender` pattern:
- Lock-free SPSC (single-producer, single-consumer) queue for audio-thread-to-UI data
- Type-safe message types for common patterns (meter levels, waveform data, spectrum data)
- Configurable update rate to avoid UI thread saturation

### 6. Pre-Built Dependency Pipeline

If Pulp uses large C++ dependencies (Skia, Dawn, ICU):
1. Create a `pulp-deps` repository
2. GitHub Actions builds dependencies for all target platforms
3. CMake FetchContent downloads pre-built binaries at configure time
4. Developers never build Skia/Dawn/etc. themselves

### 7. Non-Normalized Parameter System

```
Parameter system stores actual values:
  Frequency: 20.0 Hz — 20000.0 Hz (stored as 440.0 Hz)
  Gain: -60.0 dB — +12.0 dB (stored as -3.0 dB)
  Attack: 0.1 ms — 5000.0 ms (stored as 10.0 ms)

Normalization to 0-1 happens only at format adapter boundary:
  VST3 adapter: normalize(440.0, 20.0, 20000.0, log_shape) → 0.435...
  AU adapter: normalize(440.0, 20.0, 20000.0, log_shape) → 0.435...
```

---

## Differences in Philosophy vs the Audited Framework

| Dimension | Audited Framework | iPlug2/3 |
|-----------|------------------|----------|
| **Scope** | Full application framework (networking, XML, JSON, crypto, OpenGL, unit testing, physics) | Focused plugin framework + optional graphics engine |
| **Business model** | Dual AGPL/Commercial, revenue from subscriptions | Free permissive license, community-funded (Patreon/Open Collective) |
| **UI approach** | Own software renderer, component hierarchy, monolithic LookAndFeel | Pluggable graphics backends, simpler control model, style structs |
| **GPU support** | OpenGL (deprecated on Apple), no Metal, no Vulkan | WebGPU/Skia Graphite (state-of-the-art) |
| **Binary size** | Large (5MB+ for simple plugin) | Small (hundreds of KB for iPlug2) |
| **Web target** | Not supported | First-class WASM/Web Audio Module support |
| **Build system** | CMake (previously Projucer) | CMake + legacy configs (iPlug2), CMake-only (iPlug3) |
| **Code verbosity** | Verbose — many lines for basic setup | Terse — single-line parameter/control creation |
| **Modularity** | Monolithic modules with deep coupling | Separable core + graphics |
| **Documentation** | Extensive tutorials, API docs, large community | Sparse — examples-driven, AI tutor |
| **AI integration** | None built-in | MCP servers, screenshot validation, CLAUDE.md, agentic-first |
| **Target audience** | Professional studios + hobbyists, broad market | Creative developers, indie, "agentic era" developers |
| **DSL support** | None built-in | FAUST, Cmajor, JSFX integration with hot-reload |
| **Parameter model** | Normalized 0-1 with denormalization | Non-normalized real values with normalization at boundary |
| **Dependency philosophy** | Self-contained (ships everything, including Box2D and a JavaScript engine) | External deps (WDL, Skia, SDL3, Dawn) |
| **C++ standard** | C++17 | C++17 (iPlug2), C++20 (iPlug3) |
| **Plugin interface** | Virtual inheritance from AudioProcessor | C++20 concepts (iPlug3) |
| **State management** | ValueTree (dynamic, stringly-typed) | Binary chunks (simple, efficient) |
| **Licensing** | AGPL / Commercial (restrictive for commercial use) | zlib-like (maximally permissive) |
| **Community size** | Large (corporate backing, extensive ecosystem) | Small but dedicated (single maintainer, niche) |

---

## Alignment with Pulp Goals

### Strong Alignment

| Pulp Goal | iPlug Alignment | Details |
|-----------|----------------|---------|
| Permissive licensing | iPlug2: zlib-like license | Model for Pulp's licensing. No cost, no restrictions. |
| GPU support | iPlug3: WebGPU + Skia Graphite | State-of-the-art. Pulp should study this stack. |
| Modularity | Two-pillar architecture | Proven pattern. Pulp should adopt from day one. |
| Agentic-first design | iPlug3: MCP, screenshots, CLAUDE.md | Novel philosophy. Pulp should adopt and extend. |
| Modern C++ | iPlug3: C++20 concepts | Correct direction. Pulp should use C++20. |
| Cross-platform | iPlug2: macOS, Windows, Linux, iOS, Web | Good coverage. Pulp adds Android. |
| CLAP support | iPlug2: first-class CLAP | Pulp should also treat CLAP as first-class. |
| Small binaries | iPlug2: hundreds of KB | Worthwhile design constraint. |

### Gaps and Differences

| Pulp Goal | iPlug Gap | Pulp Differentiation |
|-----------|----------|---------------------|
| Swift on Apple platforms | No Swift support in either iPlug | Pulp's unique advantage — Swift/C++ interop for AUv3 + SwiftUI |
| Android support | iPlug3: "TBC" | Pulp should commit to Android from day one |
| Documentation-first | iPlug: sparse documentation | Pulp should have comprehensive docs |
| Public code | iPlug3: no public code | Pulp should develop in the open |
| License clarity | iPlug3: license TBD | Pulp should commit to a license before any code |
| Community design | Single-maintainer bottleneck | Pulp should be designed for community contribution |
| Tiered UI options | iPlug3: heavyweight-only (Dawn + Skia) | Pulp should offer headless, native, and custom GPU tiers |

---

## Key Takeaways

### What Pulp Should Learn from iPlug

1. **The two-pillar architecture is proven and correct.** Separate headless plugin/audio core from graphics/UI engine. Make them independently usable. This is the single most important architectural decision.

2. **Permissive licensing is a competitive advantage.** iPlug2's zlib-like license has attracted developers who cannot or will not use the audited framework's AGPL/Commercial model. Commit to permissive licensing from day one.

3. **Agentic-first design is the future.** iPlug3's philosophy of designing for AI-assisted development is novel and aligned with where software development is heading. Pulp should adopt this philosophy deeply.

4. **Non-normalized parameter values are more intuitive.** Developers think in Hz, dB, and ms — not 0.0-1.0. Normalize at the format boundary.

5. **Web/WASM deployment is forward-thinking.** Browser-based DAWs and collaborative tools are growing. Supporting WASM compilation opens new distribution channels.

### Where Pulp Can Differentiate from iPlug

1. **Swift-native Apple platform support** — neither iPlug2 nor iPlug3 has Swift at the core. Pulp can offer Swift AUv3 subclasses, SwiftUI plugin UIs, and Swift/C++ interop for the best Apple platform experience.

2. **Android from day one** — iPlug3 lists Android as "TBC". Pulp should commit to Android support, leveraging Oboe for audio and Jetpack Compose or custom rendering for UI.

3. **Documentation-first culture** — iPlug's "learn from examples" approach doesn't scale. Every Pulp API should be documented, every pattern should have a guide.

4. **Tiered graphics options** — while iPlug3 requires Dawn + Skia for all UIs, Pulp should offer multiple tiers: headless (no UI), platform-native (SwiftUI, etc.), and custom GPU (Metal/WebGPU).

5. **Available now** — iPlug3 has no public code. Pulp can ship first and iterate.

6. **Community-oriented design** — clear module boundaries, contribution guidelines, and governance designed for multiple contributors rather than a single maintainer.
