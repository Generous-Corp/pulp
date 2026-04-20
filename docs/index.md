# Pulp Documentation

Pulp is a cross-platform framework for building audio plugins and applications.
MIT-licensed. No royalties. No copyleft.

## What is supported today

- **Formats**: VST3, AU v2, AUv3, CLAP, LV2, WAMv2, WebCLAP, standalone, headless
- **Platforms**: macOS, Windows (WASAPI, NSIS), Linux (ALSA, JACK, LV2), Browser (WASM)
- **Audio**: CoreAudio, WASAPI, ALSA, JACK, Web Audio API — device I/O, buffer processing, file read/write
- **MIDI**: CoreMIDI, Win32 MIDI, ALSA MIDI, Web MIDI, MIDI 2.0 UMP, MPE
- **DSP**: 30+ signal processors (oscillator, biquad, SVF, ladder, FIR, TPT, compressor, reverb, delay, chorus, phaser, FFT, convolver, and more)
- **Parameters**: thread-safe, automatable, serializable, with CLAP modulation, presets, undo/redo
- **GPU rendering**: Dawn (Metal/D3D12/Vulkan) + Skia Graphite on all platforms
- **View system**: TextEditor, ComboBox, TabPanel, ListBox, TreeView, and more — flex layout, JS scripting, hot-reload
- **Plugin hosting**: PluginScanner, PluginSlot, SignalGraph for DAW-like apps
- **Testing**: 1622+ automated tests across 13 subsystems
- **Shipping**: codesign, notarization, DMG/PKG (macOS), NSIS (Windows), .deb (Linux), appcast

## Where to start

- [Getting Started](guides/getting-started.md) — build your first plugin step by step
- [Examples](examples/index.md) — browse example projects by category
- [Capabilities](reference/capabilities.md) — full capability matrix with status
- [Overview](concepts/overview.md) — what Pulp is and how it is organized

## Reference

- [Modules](reference/modules.md) — 13 subsystems with status, dependencies, and key headers
- [API Reference](api/index.html) — Doxygen-generated class and function documentation
- [CLI Reference](reference/cli.md) — `pulp` command reference
- [CMake Reference](reference/cmake.md) — `pulp_add_plugin()` and build system functions
- [Licensing & Acknowledgements](reference/licensing.md) — dependencies, standards, attribution
- [Architecture](concepts/architecture.md) — subsystem dependencies, thread model, GPU stack

## Guides

- [Building](guides/build.md) — requirements, options, platform notes
- [Testing](guides/testing.md) — running tests, validation, writing tests
- [Web Plugins](guides/web-plugins.md) — WAMv2, WebCLAP, browser demos
- [Docs Maintenance](guides/docs-maintenance.md) — how docs stay consistent with code

## Policies

- [Code Style](policies/code-style.md) — coding standards and architectural rules
- [Agent Rules](policies/agent-contribution-rules.md) — contribution rules for AI agents
