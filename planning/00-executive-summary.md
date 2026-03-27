# Pulp Framework — Executive Summary

## What Is Pulp?

Pulp is a proposed clean-room, cross-platform audio application and plugin framework designed to replace reliance on proprietary/restrictively-licensed frameworks (specifically JUCE) with a modern, permissive, modular alternative. It includes both the framework itself and a native Claude Code plugin for AI-assisted audio development.

## Why Pulp?

1. **Licensing freedom**: MIT-licensed framework vs AGPL/commercial dual-licensing. No royalties, no revenue thresholds, no restrictive terms.
2. **Modern architecture**: Two-pillar design (headless processing + independent UI), C++20 concepts, Swift-first on Apple platforms, reactive state management.
3. **AI-native development**: Built from the ground up for AI-assisted development via Claude Code plugin, CLAUDE.md context files, and agentic-first design principles.
4. **GPU-accelerated UI**: Dawn (WebGPU) + Skia Graphite for 60-120 FPS rendering via Metal/D3D12/Vulkan — far beyond traditional framework OpenGL support. JS-scripted GPU widgets via QuickJS with hot-reload.
5. **Swift on Apple**: AUv3 in Swift, SwiftUI for plugin UIs, direct C++/Swift interop with zero overhead. Native Apple experience, not a C++ approximation.
6. **Modular by design**: 16+ independent subsystems with clear boundaries, minimal coupling, and the ability to use pieces independently.

## Key Design Decisions

- **Language strategy**: C++ cross-platform core (DSP, format adapters, platform abstraction) + Swift Apple layer (AUv3, SwiftUI, parameter binding). C++ for what must be portable, Swift for what should be native.
- **Plugin format support**: VST3, AU, AUv3, CLAP, LV2, AAX (via developer-obtained Avid SDK), Standalone. All from official format SDKs.
- **Build system**: CMake for cross-platform C++ core, Swift Package Manager for Apple Swift layer.
- **UI architecture**: Composable themes (not monolithic), RAII view ownership, reactive data binding, flex/grid layout. GPU path via Dawn/Skia Graphite with QuickJS-scripted widgets and hot-reload. Optional SwiftUI path on Apple.
- **Design token system**: Centralized design tokens (colors, spacing, typography) exportable to JSON, CSS, C++, OKLCH. AI-driven design sessions via natural language.
- **Component inspector**: Live inspection of widget properties, bounds, tokens, and render stats. Real-time design token editing.
- **Plugins as CLIs and MCP servers**: All plugin commands operate as both CLI tools and MCP servers for AI tool interoperability (Claude Code, Codex, Stitch, etc.).
- **Web/WASM target**: Dawn/Skia stack enables compilation to WebAssembly for browser-based plugin demos and web audio applications.
- **Testing**: Catch2/doctest for unit tests, audio golden-file comparison, plugin format validators (auval, pluginval), visual regression via screenshots.

## What Was Audited

1. **JUCE 8.0.12** — 22 modules, comprehensive capability audit, pain points identified, licensing analyzed
2. **juce-dev Claude Code plugin** — 11 commands, 3 skills, complete workflow inventory, JUCE coupling points mapped
3. **iPlug3** — Architecture inspiration (two-pillar design, WebGPU rendering, C++20 concepts, agentic-first philosophy)
4. **Swift/C++ interop** — Production-ready, zero overhead, recommended architecture defined

## Scope

Pulp aims to eventually support:

- Cross-platform desktop (macOS, Windows, Linux)
- Mobile (iOS/iPadOS via AUv3, Android long-term)
- All major plugin formats (VST3, AU, AUv3, CLAP, AAX, LV2)
- Standalone applications
- Full CI/CD, packaging, signing, distribution pipeline
- GPU-accelerated UI
- Deep Claude Code integration

## Phased Approach (10 phases, ~12-18 months)

- **Phase 0**: Audit (complete)
- **Phase 1-2**: Foundation (repo, runtime, events, platform abstraction)
- **Phase 3-4**: Build tooling + Audio/MIDI I/O
- **Phase 5**: Plugin formats + parameter system (hardest phase)
- **Phase 6**: GPU rendering engine (Dawn/Skia), JS scripting (QuickJS), design system, inspector
- **Phase 7-8**: Claude plugin + CI/CD + packaging
- **Phase 9-10**: Examples, docs, validation, launch

## Is This Realistic?

Yes, with caveats:

- The hardest part is plugin format support (Phase 5) — format-specific quirks require extensive DAW testing
- UI framework (Phase 6) is the largest subsystem by code volume
- A small team (2-4 engineers) could reach a useful v1 in 12-18 months
- Early phases produce useful tools (build system, scaffolding, Claude plugin) before the full framework is complete
- The modular architecture means subsystems can be used independently as they're completed

## Highest-Risk Areas

1. Plugin format compatibility across DAWs (edge cases are legion)
2. UI framework scope (tendency to over-build)
3. Clean-room contamination (especially in format wrappers and parameter system)
4. GPU UI integration complexity (Dawn build system, Skia Graphite API evolution, QuickJS sandboxing)
5. Swift/C++ interop evolution (Swift version changes may require adaptation)

## What Would Make Pulp Better Than the Existing Stack

- Permissive licensing eliminates business risk
- Swift-native Apple experience instead of C++ everywhere
- GPU UI as a first-class option with Dawn/Skia Graphite
- JS-scripted widgets with hot-reload for rapid UI iteration
- AI-driven design workflow: describe a look in natural language, preview live, export design tokens
- Design token system with multi-format export (JSON/CSS/C++/OKLCH)
- Component inspector for live widget debugging and token editing
- Plugins as both CLIs and MCP servers, enabling any AI tool to drive the workflow
- Web/WASM target for browser-based plugin demos and previews
- Reactive state management instead of timer-based polling
- Composable theming instead of monolithic LookAndFeel
- RAII ownership instead of raw pointer management
- AI-native development workflow via Claude Code
- Modular architecture allows incremental adoption
