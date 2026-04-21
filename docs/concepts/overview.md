# Overview

Pulp is an MIT-licensed, cross-platform framework for building audio plugins and applications. C++20 at the core, GPU-rendered UIs via Dawn and Skia, JavaScript scripting with hot-reload, and tooling built for modern development workflows.

See [the Vision doc](vision.md) for the full architecture and design decisions. See [Capabilities Reference](../reference/capabilities.md) for detailed per-feature status.

## What It Does

- **Plugin formats** — VST3, AU v2, AUv3, CLAP, LV2, AAX, Standalone, WAM/WebCLAP
- **Platforms** — macOS (primary), Windows, Linux, iOS, Web/WASM (experimental)
- **DSP** — 30+ processors, thread-safe parameters, headless processing, Faust/Cmajor/JSFX integration
- **UI** — GPU-rendered widgets, CSS Flexbox/Grid layout, design tokens, hot-reload, Three.js on native GPU
- **Design** — import from Figma, Pencil, Stitch; AI-driven design tool; design token export
- **Tooling** — `pulp` CLI for build/test/validate/ship, MCP server, local + cloud CI, Claude Code plugin
- **Shipping** — code signing, notarization, DMG/PKG packaging, Sparkle auto-updates

## How It's Organized

Pulp is modular — each subsystem is an independent library under `core/`:

| Subsystem | Purpose |
|-----------|---------|
| `runtime` | Lock-free primitives, logging, assertions |
| `audio` | Audio device I/O, buffers, file I/O |
| `midi` | MIDI I/O, MIDI 2.0 UMP, MPE |
| `signal` | DSP processors, FFT, filters, effects |
| `state` | Parameters, presets, serialization, undo/redo |
| `format` | Plugin format adapters, standalone, headless |
| `canvas` | 2D drawing (CoreGraphics, Skia), SVG |
| `render` | GPU surfaces (Dawn/Skia Graphite) |
| `view` | Widgets, layout, themes, JS scripting, hot-reload |
| `dsl` | Faust/Cmajor/JSFX integration |
| `ship` | Packaging, signing, auto-updates |

## Getting Started

See the [Getting Started guide](../guides/getting-started.md).
