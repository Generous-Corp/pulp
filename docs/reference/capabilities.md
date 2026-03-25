# Capabilities Reference

A categorized inventory of what Pulp can do today. Each capability lists its current status, the module that provides it, and pointers to relevant documentation and examples.

**Status vocabulary**: `stable` | `usable` | `experimental` | `partial` | `planned` | `unsupported`

---

## Plugin Formats

Pulp wraps a single `Processor` subclass and exposes it through multiple plugin format adapters.

| Capability | Status | Module | Docs | Examples |
|---|---|---|---|---|
| VST3 effect | usable | [format](modules.md#format) | [getting-started](../guides/getting-started.md) | pulp-gain, pulp-effect, pulp-compressor |
| VST3 instrument | usable | [format](modules.md#format) | | PulpSynth, PulpDrums |
| Audio Unit v2 effect | usable | [format](modules.md#format) | [getting-started](../guides/getting-started.md) | pulp-gain, pulp-effect |
| Audio Unit v2 instrument | usable | [format](modules.md#format) | | PulpSynth |
| CLAP effect | usable | [format](modules.md#format) | | pulp-gain, pulp-effect |
| CLAP instrument | usable | [format](modules.md#format) | | PulpSynth |
| Standalone app | usable | [format](modules.md#format) | | pulp-gain |
| Headless host | usable | [format](modules.md#format) | [testing](../guides/testing.md) | |
| AU v3 | planned | | | |
| LV2 | planned | | | |
| AAX | planned | | | |

Key headers: `pulp/format/processor.hpp`, `pulp/format/vst3_adapter.hpp`, `pulp/format/clap_adapter.hpp`, `pulp/format/headless.hpp`

---

## Platforms

| Capability | Status | Module | Notes |
|---|---|---|---|
| macOS (ARM64 + x86_64) | usable | [platform](modules.md#platform) | Primary development platform |
| Windows | partial | [platform](modules.md#platform) | Build stubs exist; not validated end-to-end |
| Linux | partial | [platform](modules.md#platform) | Build stubs exist; not validated end-to-end |
| iOS | planned | | |
| Web / WASM | planned | | |

Key headers: `pulp/platform/detect.hpp`, `pulp/platform/native_handle.hpp`

---

## Audio I/O

| Capability | Status | Module | Docs |
|---|---|---|---|
| BufferView (non-owning channel pointer wrapper) | usable | [audio](modules.md#audio) | [modules](modules.md#audio) |
| Audio device enumeration and streaming (CoreAudio) | usable | [audio](modules.md#audio) | |
| Audio file read/write | usable | [audio](modules.md#audio) | |
| WASAPI device I/O | planned | | |
| ALSA device I/O | planned | | |

Key headers: `pulp/audio/buffer.hpp`, `pulp/audio/device.hpp`, `pulp/audio/audio_file.hpp`

---

## MIDI I/O

| Capability | Status | Module | Docs |
|---|---|---|---|
| MidiEvent / MidiBuffer | usable | [midi](modules.md#midi) | [modules](modules.md#midi) |
| MIDI device I/O (CoreMIDI) | usable | [midi](modules.md#midi) | |
| MIDI file read/write | usable | [midi](modules.md#midi) | |
| Win32 MIDI | planned | | |
| ALSA MIDI | planned | | |

Key headers: `pulp/midi/message.hpp`, `pulp/midi/buffer.hpp`, `pulp/midi/device.hpp`, `pulp/midi/midi_file.hpp`

---

## DSP / Signal Processing

All signal processors live in the `signal` module. Each is a standalone, stateless-friendly C++ class.

| Capability | Status | Module | Examples |
|---|---|---|---|
| Gain | usable | [signal](modules.md#signal) | pulp-gain |
| ADSR envelope | usable | [signal](modules.md#signal) | PulpSynth |
| Biquad filter | usable | [signal](modules.md#signal) | pulp-effect |
| State-variable filter (SVF) | usable | [signal](modules.md#signal) | |
| Ladder filter | usable | [signal](modules.md#signal) | PulpSynth |
| Linkwitz-Riley crossover | usable | [signal](modules.md#signal) | |
| Oscillator | usable | [signal](modules.md#signal) | pulp-tone, PulpSynth |
| Delay line | usable | [signal](modules.md#signal) | pulp-effect |
| Chorus | usable | [signal](modules.md#signal) | |
| Phaser | usable | [signal](modules.md#signal) | |
| Compressor | usable | [signal](modules.md#signal) | pulp-compressor |
| Noise gate | usable | [signal](modules.md#signal) | |
| Reverb | usable | [signal](modules.md#signal) | pulp-effect |
| Waveshaper | usable | [signal](modules.md#signal) | |
| Panner | usable | [signal](modules.md#signal) | |
| Oversampling | usable | [signal](modules.md#signal) | |
| FFT | usable | [signal](modules.md#signal) | |
| Windowing functions | usable | [signal](modules.md#signal) | |
| SmoothedValue | usable | [signal](modules.md#signal) | |

Key headers: all under `pulp/signal/` -- e.g., `pulp/signal/compressor.hpp`, `pulp/signal/oscillator.hpp`

---

## State and Automation

| Capability | Status | Module | Docs | Examples |
|---|---|---|---|---|
| ParamValue (lock-free atomic float) | stable | [state](modules.md#state) | [modules](modules.md#state) | all |
| ParamInfo (metadata, range, units) | stable | [state](modules.md#state) | | all |
| ParamRange (normalize / denormalize) | stable | [state](modules.md#state) | | |
| StateStore (centralized parameter registry) | stable | [state](modules.md#state) | | all |
| Parameter groups | stable | [state](modules.md#state) | | |
| Binding (reactive UI-parameter link) | stable | [state](modules.md#state) | | |
| Gesture begin/end (host undo grouping) | stable | [state](modules.md#state) | | |
| State serialization / deserialization | stable | [state](modules.md#state) | | |
| CLAP modulation offset | stable | [state](modules.md#state) | | |
| Change listeners | stable | [state](modules.md#state) | | |

Key headers: `pulp/state/parameter.hpp`, `pulp/state/store.hpp`, `pulp/state/binding.hpp`

---

## View / UI

| Capability | Status | Module | Docs | Examples |
|---|---|---|---|---|
| View hierarchy (tree, bounds, hit-testing) | experimental | [view](modules.md#view) | [modules](modules.md#view) | ui-preview |
| Flex layout | experimental | [view](modules.md#view) | | ui-preview |
| Theme system (color resolution up the tree) | experimental | [view](modules.md#view) | | ui-preview |
| Knob widget | experimental | [view](modules.md#view) | | ui-preview |
| Fader widget | experimental | [view](modules.md#view) | | ui-preview |
| Toggle widget | experimental | [view](modules.md#view) | | ui-preview |
| Label widget | experimental | [view](modules.md#view) | | ui-preview |
| Meter widget (RMS + peak hold) | experimental | [view](modules.md#view) | | ui-preview |
| XYPad widget | experimental | [view](modules.md#view) | | ui-preview |
| WaveformView | experimental | [view](modules.md#view) | | |
| SpectrumView | experimental | [view](modules.md#view) | | |
| AudioBridge (param + meter sync) | experimental | [view](modules.md#view) | | |
| Auto-UI generation | experimental | [view](modules.md#view) | | |
| JS scripting (QuickJS bridge) | experimental | [view](modules.md#view) | | |
| Hot reload | experimental | [view](modules.md#view) | | |
| Component inspector | experimental | [view](modules.md#view) | | |
| Drag and drop | experimental | [view](modules.md#view) | | |
| Screenshot capture | experimental | [view](modules.md#view) | | |
| Plugin view hosting | experimental | [view](modules.md#view) | | |
| SDL window host | experimental | [view](modules.md#view) | | |
| Animation | experimental | [view](modules.md#view) | | |
| Accessibility roles | experimental | [view](modules.md#view) | | |

Key headers: `pulp/view/view.hpp`, `pulp/view/widgets.hpp`, `pulp/view/theme.hpp`, `pulp/view/script_engine.hpp`, `pulp/view/hot_reload.hpp`

---

## Rendering / GPU

| Capability | Status | Module | Docs |
|---|---|---|---|
| Dawn/Metal GPU surface | experimental | [render](modules.md#render) | [modules](modules.md#render) |
| Skia Graphite rendering | experimental | [render](modules.md#render) | |
| Dawn/D3D12 surface | planned | | |
| Dawn/Vulkan surface | planned | | |

Key headers: `pulp/render/gpu_surface.hpp`, `pulp/render/skia_surface.hpp`

---

## Canvas / 2D Drawing

| Capability | Status | Module | Docs |
|---|---|---|---|
| Canvas abstraction (paths, fills, strokes, text) | experimental | [canvas](modules.md#canvas) | [modules](modules.md#canvas) |
| CoreGraphics backend | experimental | [canvas](modules.md#canvas) | |
| Skia backend | experimental | [canvas](modules.md#canvas) | |
| SVG rendering | experimental | [canvas](modules.md#canvas) | |
| Effects (shadow, blur) | experimental | [canvas](modules.md#canvas) | |

Key headers: `pulp/canvas/canvas.hpp`, `pulp/canvas/cg_canvas.hpp`, `pulp/canvas/skia_canvas.hpp`, `pulp/canvas/svg.hpp`, `pulp/canvas/effects.hpp`

---

## Runtime Primitives

| Capability | Status | Module | Docs |
|---|---|---|---|
| SeqLock (coherent multi-field reads) | stable | [runtime](modules.md#runtime) | [architecture](../concepts/architecture.md) |
| TripleBuffer (latest-value publication) | stable | [runtime](modules.md#runtime) | [architecture](../concepts/architecture.md) |
| SPSCQueue (single-producer single-consumer FIFO) | stable | [runtime](modules.md#runtime) | |
| ScopeGuard | stable | [runtime](modules.md#runtime) | |
| Logging | stable | [runtime](modules.md#runtime) | |
| Assertions | stable | [runtime](modules.md#runtime) | |

Key headers: `pulp/runtime/seqlock.hpp`, `pulp/runtime/triple_buffer.hpp`, `pulp/runtime/spsc_queue.hpp`, `pulp/runtime/log.hpp`

---

## Events

| Capability | Status | Module | Docs |
|---|---|---|---|
| Event loop | usable | [events](modules.md#events) | [modules](modules.md#events) |
| Timers | usable | [events](modules.md#events) | |

Key headers: `pulp/events/event_loop.hpp`, `pulp/events/timer.hpp`

---

## Platform Services

| Capability | Status | Module |
|---|---|---|
| OS detection | usable | [platform](modules.md#platform) |
| Clipboard access | usable | [platform](modules.md#platform) |
| Native file dialogs | usable | [platform](modules.md#platform) |
| Popup menus | usable | [platform](modules.md#platform) |
| Native window handle | usable | [platform](modules.md#platform) |

Key headers: `pulp/platform/detect.hpp`, `pulp/platform/clipboard.hpp`, `pulp/platform/file_dialog.hpp`, `pulp/platform/popup_menu.hpp`

---

## OSC (Open Sound Control)

| Capability | Status | Module | Docs |
|---|---|---|---|
| OSC 1.0 message encode/decode | experimental | [osc](modules.md#osc) | [modules](modules.md#osc) |
| UDP sender | experimental | [osc](modules.md#osc) | |
| UDP receiver | experimental | [osc](modules.md#osc) | |

Key header: `pulp/osc/osc.hpp`

---

## Tooling / CLI

The `pulp` CLI wraps common development workflows.

| Capability | Status | Docs |
|---|---|---|
| `pulp build` (configure + build) | usable | [cli](cli.md) |
| `pulp test` (run test suite) | usable | [cli](cli.md) |
| `pulp validate` (auval, clap-validator) | usable | [cli](cli.md) |
| `pulp status` (show project info, test list) | usable | [cli](cli.md) |
| `pulp clean` (remove build directory) | usable | [cli](cli.md) |
| `pulp ship sign` | usable | [cli](cli.md) |
| `pulp ship package` | usable | [cli](cli.md) |
| `pulp ship check` | usable | [cli](cli.md) |
| `pulp docs` (local docs lookup) | usable | [cli](cli.md) |

---

## Shipping / Release

| Capability | Status | Module | Docs |
|---|---|---|---|
| Code signing (macOS) | usable | ship | [cli](cli.md) |
| Notarization submit/check/staple | usable | ship | |
| DMG creation | usable | ship | |
| PKG installer creation | usable | ship | |
| Combined multi-format PKG | usable | ship | |
| Entitlements generation | usable | ship | |
| Signing identity listing | usable | ship | |
| Appcast feed generation (Sparkle-compatible) | usable | ship | |
| Appcast XML parsing | usable | ship | |
| Ed25519 update signing | usable | ship | |
| Semantic version comparison | usable | ship | |
| Windows code signing | partial | ship | Stub exists |
| Linux packaging | partial | ship | Stub exists |

Key headers: `pulp/ship/codesign.hpp`, `pulp/ship/appcast.hpp`

---

## Processor Interface

The `Processor` base class defines what plugin developers implement.

| Capability | Status | Module | Docs | Examples |
|---|---|---|---|---|
| Plugin descriptor (name, category, buses, MIDI flags) | usable | [format](modules.md#format) | [getting-started](../guides/getting-started.md) | all |
| Multi-bus I/O (sidechain, aux) | usable | [format](modules.md#format) | | pulp-compressor |
| Effect / Instrument / MidiEffect categories | usable | [format](modules.md#format) | | |
| Transport context (tempo, time sig, position) | usable | [format](modules.md#format) | | |
| Latency reporting | usable | [format](modules.md#format) | | |
| Tail time | usable | [format](modules.md#format) | | |
| Plugin registry (multi-plugin bundles) | usable | [format](modules.md#format) | | |
