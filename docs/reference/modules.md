# Module Reference

Each Pulp subsystem is a separate CMake library under `core/`. This page lists every module, its purpose, maturity, and key public headers.

## runtime

**Status**: stable
**Dependencies**: platform
**Headers**: `pulp/runtime/runtime.hpp`, `pulp/runtime/spsc_queue.hpp`, `pulp/runtime/seqlock.hpp`, `pulp/runtime/triple_buffer.hpp`, `pulp/runtime/log.hpp`, `pulp/runtime/assert.hpp`, `pulp/runtime/scope_guard.hpp`

Lock-free synchronization primitives, logging, and assertions. Provides `SeqLock<T>`, `TripleBuffer<T>`, `SPSCQueue<T>`, `ScopeGuard`. Depends on `platform` for OS detection and types.

## events

**Status**: usable
**Dependencies**: runtime
**Headers**: `pulp/events/events.hpp`, `pulp/events/event_loop.hpp`, `pulp/events/timer.hpp`

Event loop (constructible, not singleton), timers, and async dispatch. Used by UI and host integration layers.

## audio

**Status**: usable
**Dependencies**: runtime
**Headers**: `pulp/audio/audio.hpp`, `pulp/audio/buffer.hpp`, `pulp/audio/device.hpp`, `pulp/audio/audio_file.hpp`, `pulp/audio/load_measurer.hpp`, `pulp/audio/buffering_reader.hpp`, `pulp/audio/workgroup.hpp`

Audio buffer types, device I/O (CoreAudio on macOS), and audio file reading/writing. `BufferView<T>` is the primary buffer abstraction used throughout the framework. `AudioProcessLoadMeasurer` tracks real-time CPU usage. `BufferingReader` provides background-thread ring buffer for gapless streaming. `AudioWorkgroup` manages real-time thread priority (macOS os_workgroup API).

## midi

**Status**: usable
**Dependencies**: runtime
**Headers**: `pulp/midi/midi.hpp`, `pulp/midi/buffer.hpp`, `pulp/midi/message.hpp`, `pulp/midi/device.hpp`, `pulp/midi/midi_file.hpp`, `pulp/midi/rpn_parser.hpp`, `pulp/midi/keyboard_state.hpp`

MIDI message types, buffer management, device I/O (CoreMIDI on macOS), and MIDI file reading/writing. `MidiBuffer` is the primary MIDI container. `RpnParser` extracts 14-bit RPN/NRPN parameter messages from CC sequences. `MidiKeyboardState` tracks polyphonic key state across 16 channels with velocity and listener callbacks.

## signal

**Status**: usable
**Dependencies**: none (header-only INTERFACE library)
**Headers**: `pulp/signal/signal.hpp`, and individual algorithm headers

DSP algorithm library. Includes:

- Oscillators (sine, saw, square, triangle, noise)
- Filters: biquad, SVF, ladder, Linkwitz-Riley, FIR (windowed-sinc), TPT first-order
- Effects: delay line, chorus, phaser, reverb, waveshaper, compressor, noise gate
- Utilities: gain, panner, ADSR, smoothed value, log-ramped value, oversampling, FFT, windowing, ballistics filter, processor chain, lookup table, interpolators (linear, Hermite, Lagrange, windowed-sinc), filter design (Audio EQ Cookbook + Butterworth cascades)

Each algorithm is a standalone header-only class with no framework coupling.

## state

**Status**: stable
**Dependencies**: runtime
**Headers**: `pulp/state/parameter.hpp`, `pulp/state/store.hpp`, `pulp/state/preset_manager.hpp`, `pulp/state/undo_manager.hpp`

Thread-safe parameter system. `ParamValue` uses relaxed atomics for lock-free audio-thread reads. `StateStore` manages parameter registration, value access, gesture tracking, change notification, and binary state serialization. Supports modulation offsets (for CLAP per-voice modulation). `PresetManager` provides built-in preset save/load/delete/rename/import with factory vs user separation, JSON file format, and platform-standard storage locations. `UndoManager` provides generic undo/redo with named actions, transaction grouping, and configurable history depth.

## format

**Status**: usable
**Dependencies**: state, audio, midi
**Headers**: `pulp/format/processor.hpp`, `pulp/format/headless.hpp`, `pulp/format/clap_entry.hpp`, `pulp/format/vst3_entry.hpp`, `pulp/format/au_v2_entry.hpp`

Plugin format adapters. `Processor` is the central interface that plugin developers implement. Format adapters wrap it for VST3, AU v2, CLAP, and standalone. `HeadlessHost` enables processing without a DAW for testing and batch work.

Supports effects, instruments, and MIDI effects. Multi-bus (sidechain) support is available. Transport context (tempo, time signature, position) is passed via `ProcessContext`.

## platform

**Status**: usable
**Dependencies**: none
**Headers**: `pulp/platform/platform.hpp`, `pulp/platform/detect.hpp`, `pulp/platform/file_dialog.hpp`, `pulp/platform/popup_menu.hpp`, `pulp/platform/clipboard.hpp`, `pulp/platform/native_handle.hpp`

Platform detection, native file dialogs, popup menus, clipboard access, and native window handle management. macOS implementation is primary; other platforms have stubs.

## canvas

**Status**: experimental
**Dependencies**: runtime
**Headers**: `pulp/canvas/canvas.hpp`, `pulp/canvas/cg_canvas.hpp`, `pulp/canvas/skia_canvas.hpp`, `pulp/canvas/svg.hpp`, `pulp/canvas/effects.hpp`

2D drawing abstraction with CoreGraphics and Skia backends. Provides SVG rendering support and post-processing effects (blur, shadow, bloom, color adjustment).

## render

**Status**: experimental
**Dependencies**: runtime, canvas
**Headers**: `pulp/render/gpu_surface.hpp`, `pulp/render/skia_surface.hpp`

GPU surface management. Creates Dawn (WebGPU) GPU contexts and Skia Graphite rendering surfaces. Metal backend on macOS.

## view

**Status**: usable
**Dependencies**: canvas, events, state
**Headers**: `pulp/view/view.hpp`, `pulp/view/widgets.hpp`, `pulp/view/theme.hpp`, `pulp/view/script_engine.hpp`, `pulp/view/hot_reload.hpp`, `pulp/view/inspector.hpp`, `pulp/view/animation.hpp`, `pulp/view/frame_clock.hpp`, `pulp/view/audio_bridge.hpp`, `pulp/view/auto_ui.hpp`, `pulp/view/drag_drop.hpp`, `pulp/view/screenshot.hpp`, `pulp/view/window_host.hpp`, `pulp/view/input_events.hpp`, `pulp/view/text_editor.hpp`, `pulp/view/ui_components.hpp`, `pulp/view/modal.hpp`, `pulp/view/param_attachment.hpp`, `pulp/view/app_framework.hpp`, `pulp/view/design_export.hpp`

Widget system with JS scripting (QuickJS), theme/design tokens, hot-reload, drag-and-drop, animation, screenshot capture, audio visualization bridge, automatic UI generation, and a component inspector. SDL window host available.

Phase 14 additions: rich input events (MouseEvent/KeyEvent with modifiers and pointer IDs), full TextEditor widget (selection, clipboard, undo/redo, numeric/password modes), UI components (ComboBox, TabPanel, ListBox, ScrollView, Tooltip, ProgressBar, CallOutBox), modal overlay, parameter attachment helpers, application framework (KeyMapping, MenuBar, Toolbar, AppSettings), and design token export (JSON, CSS, C++, OKLCH, WGSL). Phase 19: TreeView widget for hierarchical displays (file browsers, preset trees). Animation phase: `FrameClock` shared time source with automatic invalidation, `ValueAnimation` widget-local animator, motion tokens (`motion.duration.*`, `motion.easing.*`) in the theme system, hover events on View, and JS bridge animation/visibility APIs (`animate()`, `setMotionToken()`, `setVisible()`, `removeWidget()`).

## osc

**Status**: experimental
**Dependencies**: runtime
**Headers**: `pulp/osc/osc.hpp`

Open Sound Control messaging. Depends on `runtime` for logging and assertions.
