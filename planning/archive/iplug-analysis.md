# iPlug2/iPlug3 Architectural Analysis for Pulp

## Executive Summary

iPlug exists in two forms: **iPlug2** (mature, shipping since 2018, 2,267 stars) and **iPlug3** (greenfield rewrite started January 1, 2026, not yet publicly released). Both are created by Oli Larkin. iPlug3 represents a radical rethinking of audio plugin frameworks for the "agentic era" — designed explicitly for AI-assisted development workflows. This analysis covers both, since iPlug3's design decisions are particularly relevant to Pulp's goals.

---

## 1. Repository Structure

### iPlug2 (`github.com/iPlug2/iPlug2`)

```
iPlug2/
├── IPlug/              # Core plugin abstraction
│   ├── AAX/            # AAX format wrapper
│   ├── APP/            # Standalone app wrapper
│   ├── AUv2/           # Audio Unit v2 wrapper
│   ├── AUv3/           # Audio Unit v3 wrapper
│   ├── CLAP/           # CLAP format wrapper
│   ├── VST2/           # VST2 wrapper (deprecated)
│   ├── VST3/           # VST3 wrapper
│   ├── WEB/            # Web Audio Module wrapper
│   ├── ReaperExt/      # REAPER extension wrapper
│   ├── Extras/         # DSP utilities (oscillators, envelopes, synth, WebView, OSC...)
│   ├── IPlugProcessor.h/cpp       # Audio processing base
│   ├── IPlugParameter.h/cpp       # Parameter system
│   ├── IPlugMidi.h                # MIDI handling
│   ├── IPlugEditorDelegate.h      # Editor/DSP bridge
│   └── ...
├── IGraphics/          # UI/Graphics framework
│   ├── Drawing/        # Rendering backends (NanoVG, Skia)
│   ├── Controls/       # Widget library (knobs, sliders, meters, keyboards...)
│   ├── Platforms/      # Platform windowing (Mac, Win, iOS, Linux, Web)
│   └── ...
├── WDL/                # Cockos WDL library (legacy dependency)
├── Dependencies/       # Third-party deps with download script
├── Examples/           # ~20 example projects
├── Scripts/            # Build/packaging scripts
├── Tests/              # Test projects
├── CMakeLists.txt      # CMake build system
├── iPlug2.cmake        # CMake configuration module
└── common-*.xcconfig/props/mk  # Platform-specific build configs
```

**Key insight**: Clean two-pillar architecture — IPlug (audio/plugin) and IGraphics (UI) are separable. Format wrappers live as subdirectories under IPlug. This is more modular than JUCE's approach.

### iPlug3 (`github.com/iPlug3`) — Organization

Currently only 3 public repos:
- `.github` — Organization profile with manifesto README
- `skia-builder` — Pre-built Skia binaries for multiple platforms (MIT licensed)
- `audio-plugin-dev-skills` — Claude Code skills for audio plugin development

The actual iPlug3 framework code is **not yet publicly released** (v0.0.1 ETA unknown). The architecture is documented in the manifesto.

### iPlug3 Planned Architecture

Two component frameworks:
- **MPLUG** ("Micro Plug-in Abstraction") — Headless plugin core, pure C++20/STL
- **MGFX** ("Massive Graphics Abstraction") — Standalone graphics/UI framework built on SDL3 + WebGPU (Dawn) + Skia Graphite

---

## 2. Licensing

### iPlug2
- **Custom zlib-like permissive license** — allows use in proprietary/commercial projects freely
- Only requirements: don't misrepresent origin, mark altered versions, preserve notice
- No copyleft, no revenue thresholds, no per-seat fees
- Dependencies carry their own permissive licenses: WDL (zlib), NanoVG/NanoSVG (zlib), Skia (BSD), RTAudio/RTMidi (MIT)

### iPlug3
- License **TBD** (not yet decided)

### Comparison to JUCE
- JUCE: Dual-licensed GPL3 / Commercial ($960/year personal, $2,400/year pro after $200K revenue)
- iPlug2: **Dramatically more permissive** — zero cost, zero restrictions on commercial use
- This is a major advantage for indie developers and small studios

---

## 3. Modularity

### iPlug2
- **IPlug and IGraphics are separable** — you can use IPlug without IGraphics and attach your own UI framework (SwiftUI, HTML/CSS, etc.)
- Format wrappers are independent modules (each in its own directory)
- Extras (DSP utilities) can be used independently
- However, **WDL dependency** permeates the codebase (custom string classes, containers)
- Not a package-manager-friendly modular design — it's a monorepo you clone

### iPlug3
- **Explicit two-framework design**: MPLUG and MGFX are fully independent
- MGFX is described as usable for "games, interactive art, visualizers, and tools" independently
- MPLUG uses pure C++20/STL with no legacy dependencies
- Designed to be composable: "Like iPlug2, you can put whatever UI framework you like on top of MPLUG"

### Comparison to JUCE
- JUCE is more monolithic — pulling in juce_audio_processors brings significant baggage
- iPlug2 is more modular in practice (lighter weight, fewer coupling points)
- iPlug3 takes modularity further with the clean MPLUG/MGFX split

---

## 4. Graphics/UI Approach

### iPlug2
- **Two rendering backends**: NanoVG (lightweight, OpenGL-based vector graphics) and Skia (full-featured, GPU-accelerated)
- Platform windowing: native implementations for Mac (NSView), Windows (HWND), iOS (UIView), Linux (X11), Web (Canvas/Emscripten)
- **GPU acceleration**: Yes, via Skia's GPU backend and NanoVG's OpenGL backend
- Widget system: Purpose-built audio controls — knobs, sliders, meters, keyboards, spectrum analyzers, VU meters, bubble controls, LED controls, etc.
- Controls have "IV" prefix (vector-drawn) — themeable via style structs
- WebView support for embedding HTML/CSS UIs
- HiDPI/Retina support built in

### iPlug3 (MGFX)
- **Single high-quality backend**: WebGPU native (via Dawn) + Skia Graphite — always GPU-accelerated
- **SDL3** for cross-platform windowing/input
- JavaScript scripting via abstraction over QuickJS/V8/JavaScriptCore
- Canvas-like 2D API (same API in C++ and JavaScript)
- Hot-reloading for JavaScript UI code
- Post-processing effects (bloom, blur, CRT simulation)
- WebGPU compute shaders for GPU audio processing
- Offline video rendering mode
- Multi-window support
- SkSL runtime fragment shaders
- 120 FPS target
- Extensive themeable widget library for audio UIs
- Built-in accessibility support

### Comparison to JUCE
- JUCE uses its own software renderer by default, with an optional OpenGL attachment
- JUCE's GPU story is weaker — OpenGL only, no Metal/Vulkan/WebGPU
- iPlug2's Skia backend is already superior to JUCE's rendering
- iPlug3's Dawn/Skia Graphite stack is state-of-the-art, far beyond JUCE
- JUCE has a larger widget library (general-purpose UI), but iPlug's is more audio-focused

---

## 5. Plugin Format Support

### iPlug2
| Format | Status |
|--------|--------|
| CLAP | Supported |
| VST3 | Supported |
| VST2 | Supported (deprecated) |
| AUv2 | Supported |
| AUv3 | Supported |
| AAX | Supported (Native) |
| WAM v1 | Supported (Web Audio Module) |
| Standalone | Supported |
| REAPER Extension | Supported |

### iPlug3 (Planned)
All of iPlug2's formats plus:
- WebCLAP (portable WASM plugin format)
- WAM v2
- Python/Node.js module bindings
- MCP server integration (plugins as AI tool endpoints)

### Architecture Difference from JUCE
- Both use the "write once, wrap to formats" approach
- iPlug2/3: Format adapters are thin wrappers around a core plugin class
- JUCE: `AudioProcessor` base class with format-specific code generated by Projucer/CMake
- iPlug3's innovation: Uses **C++20 concepts** instead of inheritance for the plugin interface — compile-time validation without virtual function overhead
- iPlug3 is explicitly **format-agnostic** at the abstraction level, making it easier for AI agents to work with

---

## 6. Standalone Support

### iPlug2
- Yes, via the `IPlug/APP/` module
- Uses RTAudio/RTMidi for audio/MIDI I/O
- Produces native win32/macOS applications
- Minimal — not as full-featured as JUCE's standalone wrapper

### iPlug3
- Standalone apps on macOS, Windows, Linux, iOS, visionOS
- WASM/browser deployment
- Python/Node.js module deployment

---

## 7. Build System

### iPlug2
- **CMake** (primary, added later in project life)
- Legacy platform-specific configs still present: `.xcconfig` (macOS/iOS), `.props` (Windows), `.mk` (WASM)
- `CMakePresets.json` for build presets
- `iPlug2.cmake` module sets up: C++17, universal binaries, deployment targets, MSVC static runtime
- Azure Pipelines for CI (`azure-pipelines.yml`)
- `download-prebuilt-libs.sh` for fetching pre-built dependencies

### iPlug3
- **CMake only** — no legacy build configs
- CMake handles resource bundling, code signing, platform packaging
- Explicit philosophy: "CMake is notoriously painful to write—but that's exactly the kind of task you hand to an agent"
- Pre-built Skia binaries downloaded automatically by CMake via `skia-builder`

---

## 8. Project Generation/Scaffolding

### iPlug2
- `Examples/duplicate.py` — Python script that clones a template project, performing multi-file find-and-replace
- ~20 example projects serve as templates (IPlugEffect, IPlugInstrument, IPlugMidiEffect, IPlugSideChain, etc.)
- `iPlug2OOS` (separate repo) for recommended out-of-source project setup
- No GUI-based project generator (unlike JUCE's Projucer)

### iPlug3
- Not yet documented, but the agentic-first philosophy suggests project creation would be AI-assisted rather than tool-driven

---

## 9. Platform Support

### iPlug2
| Platform | Status |
|----------|--------|
| macOS 10.13+ | Full support |
| Windows 8+ | Full support |
| iOS 15+ | Full support |
| visionOS 26.0+ | Full support |
| Linux | IGraphics platform support exists |
| Web/WASM | Via Emscripten |
| Windows ARM64EC | Supported |

### iPlug3
All of iPlug2's platforms plus:
- Linux as a first-class target
- Android TBC
- Web/WASM as a "first-class citizen"

### Platform Abstraction
- iPlug2: Platform-specific implementations in `IGraphics/Platforms/` (per-platform .cpp/.mm files)
- iPlug3: SDL3 handles cross-platform windowing/input, Dawn handles cross-platform GPU

---

## 10. Audio/MIDI Architecture

### iPlug2
- `IPlugProcessor` — base class for audio processing with `ProcessBlock()` method
- Realtime-safe constraints enforced (no allocations, no locks, no I/O in process callback)
- Multi-bus architecture (main, aux, sidechain)
- MIDI: Custom `IMidiMsg` struct, MIDI queue system
- `ISender` — lock-free data transfer from DSP to UI thread
- DSP extras: oscillators, ADSR envelopes, LFO, oversampler, convolution engine, SVF, DC blocker, noise gate, resampler, synth voice system

### iPlug3
- Same realtime-safe philosophy
- MIDI2 support via libremidi
- MPE support with built-in per-note expression
- Multi-bus architecture
- Leans on CHOC (Tracktion's ISC-licensed audio utility library)
- WebGPU compute shaders for GPU-accelerated audio processing (additive synthesis, FFT, physical modeling, ML inference)

---

## 11. Parameter System

### iPlug2
- Fixed parameter count determined at compile time
- Parameters indexed through enumerations
- **Non-normalized values** stored internally (unlike JUCE which normalizes to 0-1)
- `IParam` class with double-precision values
- Parameter types: bool, int, double, enum
- Shapes for non-linear mapping
- State serialization: `SerializeParams()` writes binary chunk of all parameter values
- `SerializeState()`/`UnserializeState()` can be overridden for custom chunk data
- Parameter-control linking is straightforward (single line of code)
- Automation support across all formats

### Comparison to JUCE
- JUCE uses `AudioProcessorParameter` hierarchy with normalized 0-1 range
- iPlug2's non-normalized approach is more intuitive for developers
- iPlug2's parameter creation is terser — one line vs JUCE's more verbose setup
- JUCE has more parameter types (e.g., `AudioParameterChoice`, `AudioParameterFloat`)

---

## 12. Developer Ergonomics

### iPlug2
- **Getting started**: Clone repo, run duplicate.py on a template, open in IDE
- **Learning curve**: Moderate — simpler than JUCE for basic plugins, less documentation for advanced features
- **Code conciseness**: Creating a parameter or UI control is typically a single line of C++
- **Binary size**: Very small — hundreds of KB vs JUCE's 5MB+ for a simple gain plugin
- **Documentation**: Limited — examples are the primary learning resource; iPlug2GPT exists as an AI tutor
- **Community support**: Discord server, Discourse forum, KVR threads
- **Pain points**: Historically complex build setup, WDL dependency confusion, some DAW compatibility issues

### iPlug3
- Designed for AI-agent-assisted development
- Fast builds for rapid agent iteration loops
- Screenshot capture for visual validation by multimodal LLMs
- MCP server integration for programmatic plugin control
- Hot-reloading for UI code
- CLAUDE.md file in repo for AI agent context

---

## 13. Community and Ecosystem

- **iPlug2**: 2,267 GitHub stars, 338 forks, actively maintained (commits as recent as March 2026)
- Lead developer: Oli Larkin (worked at Ableton for 5 years)
- Notable users: Full Bucket Music (30+ plugins), Surreal Machines, Neural Amp Modeler, Forever 89
- Community channels: Discord (#iplug3 channel), Discourse forum, Patreon
- **Smaller community than JUCE** — JUCE has corporate backing (PACE/now Focusrite group), larger userbase, more tutorials
- iPlug2 accepts donations via Open Collective
- iPlug3 org has early community interest (47 stars on the skills repo)

---

## 14. Unique Design Choices (What iPlug Does That JUCE Doesn't)

1. **WASM/Web as first-class target** — Compile plugins to run in the browser via Web Audio Modules
2. **C++20 concepts instead of inheritance** (iPlug3) — compile-time plugin interface validation
3. **UI-agnostic plugin core** — explicitly designed to work with any UI framework
4. **WebGPU native graphics** (iPlug3) — Dawn/Skia Graphite for state-of-the-art GPU rendering
5. **MCP server integration** (iPlug3) — plugins as AI tool endpoints
6. **JavaScript scripting for UI** (iPlug3) — Canvas-like API with hot-reload
7. **Agentic-first design** (iPlug3) — framework designed for AI-assisted development
8. **DSL integration** — FAUST, Cmajor, JSFX with hot-reloading
9. **Tiny binary footprint** — hundreds of KB vs JUCE's megabytes
10. **WebCLAP support** (iPlug3) — portable WASM plugin format
11. **GPU compute for audio** (iPlug3) — WebGPU compute shaders for DSP
12. **Offline video rendering mode** (iPlug3) — frame-by-frame AV content creation

---

## 15. Known Limitations

1. **Smaller community** — Far fewer developers, tutorials, and third-party resources than JUCE
2. **Documentation gaps** — Examples are the primary learning resource; deep features poorly documented
3. **DAW compatibility** — Historical issues with certain hosts (Studio One VST3 noted)
4. **Fewer UI components** — Audio-focused widget set, not a general-purpose UI toolkit
5. **Single primary maintainer** — Bus factor of 1 (Oli Larkin), though Alex Harker contributes
6. **iPlug3 not yet released** — All the exciting new features are vaporware until public release
7. **Large dependencies** (iPlug3) — Dawn and Skia are notoriously large and complex to build
8. **No Android support yet** — Listed as TBC for iPlug3
9. **Legacy WDL dependency** (iPlug2) — Custom containers/strings that confuse AI agents and newcomers
10. **Less corporate adoption** — No major DAW company officially backs it (though Oli works at Ableton)

---

## Four Lists

### Ideas Worth Studying

1. **Two-pillar architecture** (IPlug/IGraphics, MPLUG/MGFX) — clean separation of audio plugin core from UI/graphics. This is the single most important architectural idea. Pulp should have an equally clean split.

2. **C++20 concepts for plugin interface** — compile-time validation without virtual function overhead. Pulp could explore similar patterns in C++ or use Swift protocols for the same effect.

3. **Format-agnostic core abstraction** — The plugin core knows nothing about VST3, AU, CLAP specifics. Format adapters are thin wrappers. This enables better testing and AI-assisted workflows.

4. **Non-normalized parameter values** — Storing actual values (e.g., frequency in Hz) rather than 0-1 normalized values makes the API more intuitive and less error-prone.

5. **WebGPU/Dawn + Skia Graphite as graphics stack** — State-of-the-art cross-platform GPU rendering. This is exactly the kind of modern GPU support Pulp should target.

6. **SDL3 for windowing/input** — Battle-tested cross-platform abstraction instead of rolling your own platform layer.

7. **WASM as first-class target** — Treating the web as a primary deployment platform, not an afterthought.

8. **MCP server integration** — Making plugins programmable by AI agents is genuinely novel and forward-thinking.

9. **Hot-reloading workflow** — JavaScript UI hot-reload + DSL hot-reload (FAUST/Cmajor) for rapid iteration.

10. **Pre-built dependency distribution** — The `skia-builder` approach of building large dependencies in CI and distributing pre-built binaries via CMake is pragmatic.

11. **Agentic-first design principles** — Fast builds, screenshot capture for visual validation, parameterize everything, CLAUDE.md context files.

12. **Tiny binary footprint philosophy** — Designing for small output binaries rather than accepting bloat.

### Ideas Worth Avoiding

1. **Single-maintainer dependency** — iPlug's development pace is bottlenecked by one person. Pulp should be designed for community contribution from the start.

2. **WDL legacy dependency** — iPlug2's reliance on Cockos WDL (custom string classes, containers that AI doesn't understand well) was a long-term tax. Use standard library types.

3. **"License TBD"** — iPlug3's undecided license creates uncertainty. Pulp should commit to a clear, permissive license from day one.

4. **Dawn/Skia as mandatory dependencies** — iPlug3 explicitly chose to not offer lightweight alternatives ("I want to focus on the best—not worry about making it lightweight"). This means large build times and binary sizes for the graphics layer. Pulp should consider offering tiered graphics options.

5. **Historically poor documentation** — iPlug2's "learn from examples" approach doesn't scale. Pulp needs documentation-first culture.

6. **In-source build default** — iPlug2's original duplicate.py approach created in-source builds. Always default to out-of-source.

7. **Platform-specific build configs alongside CMake** — iPlug2 carried `.xcconfig`, `.props`, `.mk` files alongside CMake, creating confusion about what's canonical. Pick one build system.

8. **"Anti-slop" gatekeeping philosophy** — iPlug3's manifesto positions itself against certain use cases. Pulp should be welcoming to all developers regardless of their project ambitions.

### Possible Architectural Patterns to Adapt

1. **Plugin Core / Graphics Engine split as independent libraries**
   - MPLUG-like headless plugin core: pure C++/Swift, no UI, no platform dependencies
   - MGFX-like graphics engine: can be used standalone for non-plugin applications
   - Clear interface between them (editor delegate pattern)

2. **Format adapter pattern**
   - Thin wrapper classes per format (CLAP, VST3, AU, AUv3, AAX)
   - Each adapter translates between the format's API and the core abstraction
   - Core plugin class uses concepts/protocols, not inheritance from format base classes

3. **Lock-free DSP-to-UI communication**
   - iPlug2's `ISender` pattern: lock-free queues for transferring data from audio thread to UI
   - Essential for meters, visualizers, waveform displays

4. **Parameter system with non-normalized storage**
   - Parameters store real values with associated ranges/shapes
   - Normalization happens at the format adapter boundary
   - One-line parameter creation API

5. **Pre-built dependency pipeline**
   - GitHub Actions builds large dependencies (Skia, Dawn) for all platforms
   - CMake FetchContent or similar downloads pre-built libs automatically
   - Developers never need to build Skia/Dawn themselves

6. **Screenshot-based visual testing**
   - Render plugin UI offscreen, capture screenshot
   - Use in CI for regression testing
   - Use with multimodal LLMs for AI-assisted visual validation

7. **Embedded JavaScript engine for UI prototyping**
   - Canvas-like API accessible from both C++/Swift and JavaScript
   - Hot-reload JavaScript for rapid UI iteration
   - Compile to native for production

### Differences in Philosophy vs JUCE

| Dimension | JUCE | iPlug2/3 |
|-----------|------|----------|
| **Scope** | Full application framework (networking, XML, JSON, cryptography, OpenGL, unit testing, etc.) | Focused plugin framework + graphics engine |
| **Business model** | Dual GPL/Commercial license, revenue from subscriptions | Free permissive license, community-funded |
| **UI approach** | Own software renderer, component hierarchy, look-and-feel system | Pluggable graphics backends, simpler control model |
| **GPU support** | Basic OpenGL attachment | WebGPU/Skia Graphite (state-of-the-art) |
| **Binary size** | Large (5MB+ for simple plugin) | Small (hundreds of KB for iPlug2) |
| **Web target** | Not supported | First-class WASM/Web Audio Module support |
| **Build system** | CMake (previously Projucer) | CMake + legacy configs (iPlug2), CMake-only (iPlug3) |
| **Code verbosity** | Verbose — many lines for basic setup | Terse — single-line parameter/control creation |
| **Modularity** | Monolithic modules with internal dependencies | Separable core + graphics |
| **Documentation** | Extensive tutorials, API docs, JUCE community | Sparse — examples-driven, AI tutor |
| **AI integration** | None built-in | MCP servers, screenshot validation, CLAUDE.md, agentic-first |
| **Target audience** | Professional studios + hobbyists | Creative developers, indie, "anti-slop" |
| **DSL support** | None built-in | FAUST, Cmajor, JSFX integration |
| **Parameter model** | Normalized 0-1 with denormalization | Non-normalized real values |
| **Dependency philosophy** | Self-contained (ships everything) | External deps (WDL, Skia, SDL3, Dawn) |

---

## Alignment with Pulp's Goals

### More permissive licensing
**Strong alignment with iPlug2.** Its zlib-like license is a model for what Pulp should do. iPlug3's license is TBD — an opportunity for Pulp to differentiate by committing to permissive licensing from day one.

### Better GPU support
**Strong alignment with iPlug3.** The WebGPU (Dawn) + Skia Graphite stack is the most modern GPU graphics approach in any audio plugin framework. This is exactly the direction Pulp should study. The SDL3 windowing layer is also a solid choice.

### Cross-platform including mobile
**Good alignment.** iPlug2 supports iOS and visionOS. iPlug3 targets macOS, Windows, Linux, iOS, visionOS, and Web/WASM (Android TBC). Pulp should aim for similar breadth, with Android from the start (where iPlug3 is still uncertain).

### Modern build systems
**Good alignment with iPlug3.** CMake-only, pre-built dependency distribution, CMake handling code signing and packaging. iPlug2's mixed build system (CMake + xcconfig + props + mk) is a cautionary tale.

### Good CI/CD integration
**Moderate alignment.** iPlug2 uses Azure Pipelines. The `skia-builder` repo demonstrates GitHub Actions for building large dependencies cross-platform. Screenshot-based testing is a novel CI idea. But there's no documented CI/CD best-practice guide.

### Swift compilation for Apple platforms
**Weak alignment.** iPlug2 has a SwiftUI example (`IPlugSwiftUI`) showing SwiftUI can be used as the UI layer, but the core framework is C++. iPlug3 is also C++20. Neither has Swift at the core. This is an area where Pulp can differentiate significantly — a Swift-native plugin core for Apple platforms with C++ interop for cross-platform DSP.

---

## Key Takeaways for Pulp

1. **The two-pillar architecture is proven and correct.** Separate the headless plugin/audio core from the graphics/UI engine. Make them independently usable.

2. **iPlug3's graphics stack (Dawn + Skia Graphite + SDL3) is the current state of the art** for cross-platform GPU-accelerated rendering. Study it carefully, but be aware of the build complexity and binary size trade-offs.

3. **The permissive license is a competitive advantage** against JUCE. Commit early.

4. **WASM/Web as a target is forward-thinking** and increasingly relevant for web-based DAWs and collaborative music tools.

5. **The "agentic-first" philosophy is novel** — designing a framework to be AI-friendly (fast builds, screenshot validation, MCP integration, CLAUDE.md) is a genuine innovation that Pulp should adopt.

6. **Where Pulp can differentiate from iPlug**: Swift-native Apple platform support, Android from day one, stronger documentation culture, more welcoming community philosophy, tiered graphics options (lightweight to heavyweight), and being available now (iPlug3 is not yet released).

Sources:
- [iPlug2 GitHub Repository](https://github.com/iPlug2/iPlug2)
- [iPlug3 GitHub Organization](https://github.com/iPlug3)
- [iPlug3 Manifesto](https://github.com/iPlug3/.github)
- [iPlug2 Official Website](https://iplug2.github.io/)
- [iPlug3 Patreon Manifesto](https://www.patreon.com/posts/iplug3-manifesto-149394263)
- [iPlug2 vs JUCE - KVR Forum](https://www.kvraudio.com/forum/viewtopic.php?t=565161)
- [iPlug2 Forum](https://iplug2.discourse.group/)
- [iPlug2 Wiki - Out of Source Builds](https://github.com/iPlug2/iPlug2/wiki/Out-of-source-builds)
- [iPlug2 Distributed Plugins Wiki](https://github.com/iPlug2/iPlug2/wiki/Distributed-Plugins)
- [Skia Builder for iPlug3](https://github.com/iPlug3/skia-builder)
