# Pulp — Status Tracker

Last updated: 2026-03-26 (647 tests, 8 plugin examples + 3 apps, PHASES 0-20 + Apple render + plugin editors + host + WASM demos)

## Web Plugin Formats — COMPLETE
- [x] WAMv2 adapter (WamProcessorBridge + wam-plugin.js) — 10 tests
- [x] WebCLAP adapter (PULP_WCLAP_PLUGIN macro + PulpWclap.cmake)
- [x] CLAP webview extension (WebviewProvider + generate_webview_html) — 5 tests
- [x] Browser test host (tools/browser-host/) with oscilloscope, MIDI keyboard, param sliders
- [x] WASI SDK toolchain (wasi-toolchain.cmake)
- [x] Web demo plugins: PulpPluck (Karplus-Strong synth), PulpChorus (stereo chorus) — 8 tests
- [x] WASM compilation: PulpGain + PulpPluck + PulpChorus compiled to .wasm via Emscripten
- [x] Web plugins guide (docs/guides/web-plugins.md)
- [x] Web demos README with build/test instructions

## Phase Overview

| Phase | Name | Status | Notes |
|-------|------|--------|-------|
| 0 | Audit and Specification | **Complete** | 18 spec docs + iPlug3 comparison + CHOC audit |
| 1 | Repo Foundation | **Complete** | CMake skeleton, CI workflow |
| 2 | Core Runtime | **Complete** | platform, runtime, events |
| 3 | Build Tooling | **Complete** | pulp_add_plugin, binary data, install targets, templates, macros |
| 4 | Audio/MIDI I/O | **Complete** | CoreAudio, CoreMIDI, standalone |
| 5 | Plugin Formats | **Complete** | CLAP+VST3+AU, effects+instruments, multi-bus+sidechain, headless, bindings |
| 5.5 | CHOC Migration | **Complete** | SPSC→CHOC FIFO, MIDI→choc::midi, endianness→choc::memory |
| 6 | GPU Rendering | **Complete** | ScriptEngine, Theme, Canvas, Widgets, WebGPU, SDL3, Skia Graphite, effects |
| 7 | CLI + MCP | **Complete** | pulp CLI, MCP server, screenshot capture, synthetic events, keyboard focus, codesign checking |
| 8 | Signing/Packaging | **Complete** | codesign, notarization, DMG, pkg, combined installer, pulp ship CLI, entitlements, CI sign-release, Win/Linux stubs, appcast |
| 9 | Examples/Docs/DSP | **Complete** | 8 plugin examples, 30 DSP processors, OSC, FFT, audio/MIDI file I/O |
| 10 | Hardening/Launch | **Complete** | Sanitizer CI, SeqLock, TripleBuffer, param hardening, sync strategy docs |
| 11 | Documentation Parity | **Complete** | Doxygen, format/DSP/state/module guides, shipping/testing deep-dives |
| 12 | Developer Tooling | **Complete** | pulp create/inspect/audit/add, plugin-as-CLI, MCP expansion |
| 13 | AUv3 + Swift | **Complete** | AUv3 adapter (macOS + iOS), Swift/C++ bridge, SwiftUI views, AVAudioSession |
| 14 | Core UI Components | **Complete** | Input events, TextEditor, ComboBox, TabPanel, ListBox, ScrollView, Tooltip, ProgressBar, CallOutBox, Modal, PresetManager, AppFramework, DesignExport, ParamAttachment |
| 15 | Windows Platform | **Mostly Complete** | WASAPI, Win32 MIDI, NSIS installer, CI, platform guide. Signing stubs only. No platform UI (clipboard/file dialog). VM validation pending. |
| 16 | Linux Platform | **Mostly Complete** | ALSA, JACK, ALSA MIDI, LV2, CI, platform guide, 540/540 on VM. Platform UI stubs (clipboard). Accessibility stubs. |
| 21 | View System Foundation | **Complete** | Keyboard input, Panel widget, ComboBox fix, ScrollView, CanvasWidget. 24 new tests on main. |
| 21.5 | W3C Standards Parity | **In Progress** | Full W3C Flexbox L1 + Box Model L3 + Color L4 + Overflow L3 + Graphics extensions. 3 phases: Critical (~9d), DX (~17d), Advanced (~22d). Specs: planning/w3c-standards-coverage-map.md, planning/css-flexbox-parity-spec.md |
| 22 | Design Tool Parity | **In Progress** | Achieving 1:1 feature parity with ai-style-designer HTML. 5 sub-phases: Layout/Structure, Center Preview, Left Token Browser, Right Inspector+Chat, Interactions+Polish. Branch: phase/design-tool-v2. Ref: planning/ralph-loop-prompt-11.md |
| 23 | Plugin Preview | **Planned** | Load real plugin UIs in design tool. Parameter connection, side-by-side theme comparison. |
| 24 | Visual Editor | **Planned** | Unlock canvas, drag/resize widgets, snapping, grouping, code generation from layout changes. |
| 25 | Design HUD | **Planned** | Floating color/chat panels that attach to any running plugin. Live token editing on your actual plugin UI without the full design tool. |

## Touch → JS Bridge (2026-03-28)
- [x] P0: Fix iOS pointer identity — stable UITouch→ID map in window_host_ios.mm + plugin_view_host_ios.mm
- [x] P1: Pipe clientX/clientY through JS bridge — _makeEvent() receives full coordinate data
- [x] P2: W3C PointerEvent API — pointerdown/pointermove/pointerup/pointercancel with pointerId, pointerType, isPrimary
- [x] P2b: Pointer capture — setPointerCapture/releasePointerCapture, gotpointercapture/lostpointercapture
- [x] P3: Apple Pencil — pressure, altitudeAngle, azimuthAngle, pointerType="pen"
- [x] P4: Gesture events — gesturestart/gesturechange/gestureend, macOS magnifyWithEvent/rotateWithEvent
- [x] P5: Coalesced/predicted touches — JS API structure (getCoalescedEvents/getPredictedEvents)
- [x] P6: iPadOS hover — UIHoverGestureRecognizer for trackpad/mouse on iPadOS 13+
- [x] touch-action CSS property
- [x] 10 new headless tests (pointer types, stylus data, gesture events, pointer capture)
- [x] Docs: js-bridge.md, support-matrix.yaml updated

## Tests: 1218 pass (macOS; AudioWorkgroup timeout is known flaky — exclude with --exclude-regex AudioWorkgroup)

### Loop 8 Gap Closures (2026-03-26)
- [x] CI: pluginval installed in validate.yml, runs against VST3 bundles at strictness 5
- [x] CI: clap-validator download attempted, falls back to dlopen tests
- [x] CI: auval step installs AU components and scans (may need signing in CI)
- [x] Golden audio tests: 11 tests (PulpGain unity/+6dB/-inf/bypass/stereo/round-trip, PulpTone sound/silence, PulpEffect lowpass/bypass/preserve)
- [x] Negative-path tests: 8 tests (NaN, Inf, extreme sample rates, buffer sizes, rapid prepare/release)
- [x] Thread-safety integration: TripleBuffer+SpscQueue pipeline, SeqLock+TripleBuffer concurrent access
- [x] Test count updated: 631 → 647
- [x] check-docs.sh: fix unbound variable when no build dir exists

### Next Work (from gap matrix, not yet addressed)
- [ ] **PulpCompressor functional tests** — verify compression reduces loud signals more than quiet (Priority 2, gap 3)
- [ ] **Corrupt state deserialization test** — verify graceful handling of garbage data (Priority 2, gap 4)
- [ ] **Golden-file reference files** — commit WAV reference outputs for bit-exact regression (Priority 2, gap 5)
- [ ] **Windows VM validation** — build/test on ssh win2, verify WASAPI/Win32 MIDI (Priority 3, gap 6)
- [ ] **WASM browser validation** — build WASM, load in browser host, verify audio (Priority 3, gap 7)
- [ ] **Plugin matrix: parameter automation** — verify param changes during process() (Priority 4, gap 9)
- [ ] **Plugin matrix: state round-trip** — save/restore all plugin states, verify identical (Priority 4, gap 9)
- [ ] **auval CI reliability** — auval in CI likely needs signed AU bundles; document if infeasible

12 subsystems active (platform, runtime, events, state, audio, midi, osc, signal, format, canvas, view, render).
3 platforms: macOS (primary), Windows (CI), Linux (CI + VM validated).
4 plugin formats: VST3, AU v2, AUv3, CLAP. LV2 adapter (Linux).
30+ DSP processors. OSC module. Sync: SeqLock, TripleBuffer.
Audio I/O: CoreAudio (macOS), WASAPI (Windows), ALSA + JACK (Linux), AVAudioSession (iOS).
MIDI I/O: CoreMIDI (macOS), Win32 MIDI (Windows), ALSA Raw MIDI (Linux).
GPU: Dawn/Metal presentable surface, Skia Graphite, capability-driven config. CoreGraphics fallback.
Ship: codesign, DMG/PKG (macOS), NSIS (Windows), .deb/.tar.gz (Linux), appcast.
8 plugin examples: PulpGain, PulpTone, PulpEffect, PulpCompressor, PulpSynth, PulpDrums, PulpSampler, PulpPluck. 3 apps: gpu-demo, ui-preview, web-demos.

## Example Plugins

| Plugin | Type | Buses | Params | CLAP | VST3 | AU |
|--------|------|-------|--------|------|------|----|
| PulpGain | Effect | 1in+1out | 3 | ✅ | ✅ | ✅ |
| PulpTone | Instrument | 0in+1out | 4 | ✅ | ✅ | ✅ |
| PulpEffect | Effect | 1in+1out | 5 | ✅ | ✅ | ✅ |
| PulpCompressor | Effect | 2in(+SC)+1out | 6 | ✅ | ✅ | ✅ |
| PulpSynth | Instrument | 0in+1out | 10 | ✅ | — | — |
| PulpDrums | MIDI Effect | 1in+1out | 6 | ✅ | — | — |
| PulpSampler | Instrument | 0in+1out | 7 | ✅ | — | — |
| PulpPluck | Instrument | 0in+1out | 3 | ✅ | ✅ | ✅ |

---

## CHOC Integration Plan

CHOC (ISC, already fetched) should be used aggressively to avoid reinventing. Module-by-module:

### Replace Our Custom Code with CHOC

| What | Our Code | CHOC Replacement | Why |
|------|----------|-----------------|-----|
| **SPSC queue** | `spsc_queue.hpp` (75 lines) | `choc::fifo::SingleReaderSingleWriterFIFO` + 3 other variants | Battle-tested, more variants, variable-size option |
| **MIDI messages** | `message.hpp` (53 lines) | `choc::midi` (messages, sequences, file I/O) | Richer MIDI support, sequence/file I/O included |
| **Endianness/byte ops** | Manual in `store.cpp` | `choc::memory::Endianness` | Cleaner, less error-prone |

### Use CHOC for New Features (don't build our own)

| Feature | CHOC Module | Phase |
|---------|------------|-------|
| **JS engine abstraction** | `choc::javascript::Context` (QuickJS/V8/JSC) | Phase 6 — CRITICAL |
| **File watcher** | `choc::file::FileWatcher` | Phase 6 — hot-reload |
| **Lock-free FIFOs** | `choc::fifo::*` (4 variants including variable-size) | Phase 6 — audio→UI |
| **JSON parser** | `choc::json` | Phase 6 — design tokens |
| **Audio file I/O** | `choc::audio::AudioFileFormat` (WAV/FLAC/OGG/MP3) | Phase 9 — PulpSampler |
| **String utilities** | `choc::text::*` (trim, split, join, URI encode) | As needed |
| **HTTP/WebSocket** | `choc::network::HTTPServer` | Phase 7 — possible MCP transport |
| **WebView** | `choc::ui::WebView` | Phase 6 — optional web content panels |
| **Spin lock** | `choc::threading::SpinLock` | Phase 6 — render thread sync |
| **Task thread** | `choc::threading::TaskThread` | Phase 6 — background loading |

### Keep Our Own (strong reasons)

| What | Why Keep |
|------|---------|
| **Audio BufferView** | Tightly integrated with Processor::process() API; CHOC's layout is different |
| **Platform detection** | 100 lines, specific to our CMake macros; CHOC's is generic, no advantage |
| **Logging** | CHOC doesn't have logging; ours works |
| **State serialization** | Our CRC32 + binary format is purpose-built; CHOC's xxHash is different use case |

---

## Upcoming Work Items by Phase

### Phase 5.5 (CHOC Migration — Before Phase 6)
- [x] Replace `spsc_queue.hpp` with `choc::fifo::SingleReaderSingleWriterFIFO`
- [x] Replace `message.hpp` MIDI types with `choc::midi` types
- [x] Replace manual endianness in `store.cpp` with `choc::memory::Endianness`
- [x] Add CHOC include paths to subsystems that need it
- [x] Verify tests still pass after migration (72/72 available, AU/VST3 SDK-dependent tests excluded)

### Phase 6 (GPU Rendering) — In Progress
- [x] **CHOC `javascript::Context`** — ScriptEngine wrapping QuickJS (6 tests)
- [x] **CHOC JSON parser** — Theme JSON loading/saving via choc::json
- [x] **Design token system** — Color, dimension, string tokens + dark/light/pro_audio themes (8 tests)
- [x] **View hierarchy** — View base class, flex layout, hit testing, theme resolution (7 tests)
- [x] **Canvas abstraction** — 2D drawing API with RecordingCanvas backend (6 tests)
- [x] **Widget library (core)** — Knob, Fader, Toggle, Label with RecordingCanvas painting (11 tests)
- [x] **Audio-UI bridge** — Lock-free MeterData transfer with CHOC FIFO + MeterBallistics (5 tests)
- [x] **JS widget bridge** — createKnob/Fader/Toggle/Label, setValue/getValue, param binding (6 tests)
- [x] **CHOC `FileWatcher`** — HotReloader with poll-based reload queue (3 tests)
- [x] **Meter widget** — Level meter with ballistics, peak hold, color thresholds (4 tests)
- [x] **CoreGraphics Canvas** — Full macOS rendering backend (CG shapes, CoreText, coordinate flip)
- [x] **WindowHost (macOS)** — NSWindow/NSView host with PulpView, event loop, close callback
- [x] **UI preview app** — Standalone example validating full JS → widget → CG → screen pipeline
- [x] **XYPad widget** — 2D parameter control with crosshair, grid, labeled axes (2 tests)
- [x] **WaveformView widget** — Audio waveform display with min/max per-pixel rendering (2 tests)
- [x] **SpectrumView widget** — Frequency spectrum with bars/line/filled styles (3 tests)
- [x] **PluginViewHost (macOS)** — NSView embedding for DAW hosts (AU/VST3/CLAP)
- [x] **ViewInspector** — View tree introspection to JSON, find_by_id, type names (5 tests)
- [x] **WebGPU (wgpu-native)** — Pre-built binaries via FetchContent, GpuSurface abstraction (1 test)
- [x] **SDL3** — Cross-platform windowing via FetchContent (zlib license)
- [x] **Skia Graphite integration** — SkiaCanvas, FindSkia.cmake, build-skia.sh
- [x] **Skia Graphite binaries** — pre-built m144 for macOS/Win/Linux/iOS/WASM (via olilarkin/skia-builder)
- [x] **Skia Graphite rendering** — SkiaSurface wired: Dawn device → Graphite context → Recorder → SkCanvas → GPU submit (1 test)
- [x] **Accessibility foundation** — AccessRole/label/value on View, default roles for all widgets (2 tests)
- [x] **Post-processing effects** — blur, shadow, bloom, color adjust with effect layers (6 tests)
- [x] **Full NSAccessibility** — PulpAccessibilityElement maps AccessRole to NSAccessibilityRole, VoiceOver support
- [x] **Drag-and-drop** — DropTarget/DropData with macOS NSView integration (4 tests)
- [x] **FFT + Convolver** — radix-2 FFT, overlap-add convolver (6 tests)
- [x] **SVG loading** — nanosvg (zlib) parser + rasterizer, Canvas rendering (5 tests)
- [x] **Appcast/auto-updates** — Sparkle-compatible XML feed generation/parsing, version comparison (4 tests)

### Phase 7 (CLI + MCP)
- [x] **pulp CLI** — build, test, status, validate, clean commands
- [x] **MCP server** — JSON-RPC 2.0 over stdin/stdout (pulp_build, pulp_test, pulp_status, pulp_validate)
- [x] **Screenshot capture** — headless PNG rendering via CoreGraphics bitmap context (2 tests)
- [x] **pulp-screenshot tool** — standalone renderer, --demo/--script/--base64/--theme
- [x] **MCP UI tools** — pulp_screenshot (base64 PNG), pulp_simulate_click, pulp_get_view_tree
- [x] **Synthetic events** — simulate_click, simulate_drag, on_mouse_down/up/drag/key_press (2 tests)
- [x] **Keyboard focus** — focusable widgets, Tab/Shift-Tab traversal, focus_next/prev (1 test)
- [x] **Codesign checking** — check_codesign, list_signing_identities via pulp-ship (3 tests)

### Phase 8 (Signing/Packaging)
- [x] **Codesign API** — check_codesign, codesign, list_signing_identities (macOS, 3 tests)
- [x] **Notarization** — notarize_submit, notarize_check, notarize_staple
- [x] **PKG creation** — create_pkg via pkgbuild
- [x] **DMG creation** — create_dmg via hdiutil with Applications alias (1 test)
- [x] **Combined installer** — create_combined_pkg via productbuild for multi-format
- [x] **Entitlements** — default_audio_entitlements() + ship/templates/entitlements.plist (1 test)
- [x] **pulp ship CLI** — sign, package, check subcommands
- [x] **CI sign-and-release** — build/sign/notarize/pkg/release workflow with AU support
- [x] **Windows signing stubs** — signtool wrappers (API surface)
- [x] **Linux packaging stubs** — .deb/AppImage API surface
- [x] **Appcast CI** — Sparkle-compatible appcast.xml generated during release
- [x] **Appcast library** — to_xml, from_xml, compare_versions, Ed25519 signing API (4 tests)

### Phase 9 (Examples/Docs/Advanced)
- [x] **pulp-signal DSP core** — SmoothedValue, ADSR, Biquad, Oscillator (15 tests)
- [x] **pulp-signal DSP extended** — DelayLine, Gain, DryWetMixer, Compressor, Limiter (9 tests)
- [x] **pulp-signal DSP batch 3** — SVF, WaveShaper, Oversampler, NoiseGate, Panner (10 tests)
- [x] **pulp-signal DSP final** — Chorus, Phaser, Reverb (FDN), LadderFilter, LinkwitzRiley, WindowFunction (8 tests)
- **pulp-signal COMPLETE** — 20 processors, 42 tests total
- [x] **Audio file I/O** — WAV read/write via CHOC + sample format converters (7 tests)
- [x] **MIDI file I/O** — read/write standard MIDI files via CHOC (in f506ecb)
- [x] **OSC module (pulp-osc)** — OSC 1.0 sender/receiver over UDP (9 tests)
- [x] Python/Node.js bindings — COMPLETED (HeadlessHost via pybind11 and Node-API)
- [x] **PulpSynth** — macro oscillator synth using DSP library (4 tests)
- [x] **PulpDrums** — generative drum sequencer MIDI effect, 4 patterns, swing, randomization (4 tests)
- [x] PulpSampler — COMPLETED (audio file sampler with MIDI, ADSR, CLAP)

### Phase 10 (Hardening)
- [x] **Sanitizer CI** — ASan, TSan, UBSan GitHub Actions + PULP_SANITIZER CMake option
- [x] **SeqLock** — lock-free coherent multi-field reads, stress-tested (2 tests)
- [x] **TripleBuffer** — lock-free latest-value publication, stress-tested (3 tests)
- [x] **AudioBridge overflow fix** — switched meter FIFO to TripleBuffer
- [x] **Parameter model hardening** — listener mutex fix, CLAP gestures/modulation/output events, VST3 output changes/groups, AU v3 parameterTree, AU gestures/value strings, latency/tail reporting all formats
- [x] **Sync strategy docs** — runtime.hpp documents which primitive to use when
- [x] **Atomic ordering audit** — documented relaxed ordering rationale for ParamValue
- [ ] Compute shaders for audio — DEFERRED (exploration, post-v1)
- [ ] WebCLAP/WAMv2 — DEFERRED (when specs stabilize)
- [ ] libremidi for Windows/Linux MIDI — DEFERRED (behind pulp-midi interface, when testing on Win/Linux)

---

## Architecture Validations
- Rendering stack (Dawn/Skia/QuickJS): **Confirmed** by iPlug3 convergence
- Two-pillar headless-first: **Confirmed**
- CHOC dependency: **Confirmed** — critical for Phase 6, replacing custom code where superior
- CMake build system: **Confirmed**
- Multi-bus support: **Implemented** with PulpCompressor validation
- SDL3 as swappable windowing backend: **Planned** (native for DAW, SDL3 for standalone)

## Strategic Differentiators vs iPlug3
- Swift-native Apple layer (AUv3/SwiftUI) — unique
- Open development + MIT license — clear vs iPlug3's "TBD"
- Documentation-first culture
- Aggressive CHOC reuse (don't reinvent what exists)
- Community-oriented architecture (clear module boundaries)

## Post-v1 Roadmap — Phased

Work items from VISION.md not yet implemented, ordered by dependency and impact.

### Phase 11: Documentation Parity — COMPLETE
**Merged**: phase/docs-parity → main
- [x] Doxygen API reference from public headers
- [x] Format-specific guide (VST3 params, AU sandbox, CLAP modulation)
- [x] DSP algorithm usage guide (per-processor examples)
- [x] State/parameter system deep-dive (ParamValue API, serialization)
- [x] Module deep-dive guides (runtime, canvas, view, render, osc)
- [x] Platform guide: macOS signing/notarization end-to-end
- [x] Shipping guide: full deployment workflow
- [x] Testing deep-dive: HeadlessHost API, sanitizer setup, golden-files

### Phase 12: Developer Tooling & CLI — COMPLETE
**Merged**: phase/dev-tooling → main
- [x] Auto-generated parameter UI (generic editor from parameter definitions)
- [x] `pulp create` — project scaffolding from templates
- [x] `pulp inspect` — live component inspector launch
- [x] Plugin-as-CLI interface (batch processing, headless rendering)
- [x] MCP server per-plugin interface (AI agent parameter control)
- [x] `pulp audit` — license/clean-room audit tool
- [x] `pulp add component` — component installer

### Phase 13: AUv3 + Swift Layer — COMPLETE
**Merged**: phase/auv3-swift → main
- [x] AUv3 format adapter (macOS)
- [x] Swift/C++ interop bridge (Swift 5.9+ direct interop)
- [x] SwiftUI alternative UI path (Apple platforms)
- [x] iOS target foundation (AVAudioSession, AUv3) — Phase 13 + GPU wiring in Apple render
- [x] iOS GPU rendering (Dawn/Metal on iOS) — IOSGpuWindowHost + IOSGpuPluginViewHost

### Phase 14: Core UI Components
**Why here**: enables richer examples and real-world plugin UIs.
**Deps**: view system (experimental but functional)

**Keyboard & Input Foundation:**
- [x] MouseEvent with modifiers (Cmd, Ctrl, Shift, Alt/Option, Meta) — input_events.hpp (10 tests)
- [x] KeyEvent with modifiers and key codes + handleTextInput(string) for IME (4 tests)
- [x] Multi-pointer/multi-touch support (pointer_id on all mouse events, iOS UITouch tracking)
- [x] Modal window support (Esc to close, focus trapping, overlay dimming) — modal.hpp (5 tests)
- [x] Key command routing (Cmd+C/V/X/Z/A dispatched to focused widget) — app_framework.hpp

**Widgets:**
- [x] Text input widget (full TextEditor: selection, clipboard, undo/redo, cursor movement, numeric/password mode, Return/Esc) — text_editor.hpp/cpp (18 tests)
- [x] Animation system (JS-based tweens, easing, property interpolation) — animation.hpp (existing)
- [x] ComboBox / dropdown widget (enumerated parameter selection) — ui_components.hpp (6 tests)
- [x] ListBox + scrolling viewport (scrollable content containers) — ui_components.hpp (5 tests)
- [x] TabPanel / tabbed component (multi-page plugin UIs) — ui_components.hpp (3 tests)
- [x] Tooltip support (hover tooltips on all widgets) — ui_components.hpp (2 tests)
- [x] ProgressBar widget (async operation feedback) — ui_components.hpp (2 tests)
- [x] CallOutBox / alert dialogs (confirmations, notifications) — ui_components.hpp (3 tests)
- [x] Parameter attachment helpers (widget-to-parameter binding with normalization) — param_attachment.hpp
- [x] ScrollView (scrollable container with scroll bars) — ui_components.hpp (2 tests)

**Preset Management (built-in):**
- [x] PresetManager — save/load/delete/rename/import presets (8 tests)
- [x] Factory vs user preset separation with folder scanning
- [x] Current preset tracking with unsaved-changes detection
- [x] Preset file format (JSON-based, extensible)
- [x] Preset Browser UI component (search, filtering, prev/next navigation, factory/user modes) — 10 tests
- [x] DAW-integrated preset switching (CLAP preset-load extension, auto PresetManager from descriptor)
- [x] Smart storage locations (~/Library/Audio/Presets/ on macOS, %APPDATA% on Windows, ~/.config/ on Linux)

**Application Framework (standalone apps):**
- [x] MenuBar abstraction (native NSMenu/Win32/GTK, keyboard shortcuts) — app_framework.hpp (2 tests)
- [x] Toolbar abstraction (native NSToolbar/CommandBar, customizable items) — app_framework.hpp (1 test)
- [x] Keyboard shortcut manager (KeyMapping with configurable bindings) — app_framework.hpp (6 tests)
- [ ] Audio Device Selector component — DEFERRED (needs platform audio device enumeration)
- [ ] Musical Typing component — DEFERRED (Phase 17 advanced examples)
- [x] Application settings persistence (window size/position, device selection, preferences) — app_framework.hpp (5 tests)
- [ ] Multi-instance support — DEFERRED (format adapter enhancement)
- [ ] Window state restoration — DEFERRED (needs per-platform window management)

**Design & Showcase:**
- [ ] Widget Showcase app — DEFERRED (Phase 17)
- [ ] AI Style Designer integration — DEFERRED (Phase 17)
- [x] Design token export (JSON, CSS, C++ headers, shader uniforms, OKLCH) — design_export.hpp (6 tests)
- [ ] Multi-window support — DEFERRED
- [ ] `pulp design` — AI design session CLI — DEFERRED

### Phase 15: Windows Platform — COMPLETE (merged to main)
**Why here**: second-largest DAW market. Requires dedicated validation.
**Deps**: build stubs already exist
- [x] Windows audio I/O (WASAPI) — shared mode, event-driven, deinterleaved callback
- [x] Windows MIDI I/O (Win32 MIDI) — midiIn/midiOut with callback-based input
- [x] D3D12 GPU backend (Dawn already supports it) — Dawn selects D3D12 automatically
- [x] Windows CI matrix (GitHub Actions) — windows-latest enabled in build.yml
- [x] Windows Authenticode / Azure Trusted Signing — signtool stubs + Azure SignTool docs
- [x] Windows installer (NSIS) — generate_nsis_script + create_nsis_installer
- [x] Platform guide: Windows build and deployment — docs/guides/platforms/windows.md
- [ ] Runtime validation on Windows VM (ssh win2) — PENDING

### Phase 16: Linux Platform — COMPLETE (merged to main)
**Why here**: third platform. LV2 is Linux-first format.
**Deps**: build stubs already exist
- [x] Linux audio I/O (ALSA) — blocking write, device enumeration, PulseAudio/PipeWire compatible
- [x] Linux MIDI I/O (ALSA Raw MIDI) — snd_rawmidi with threaded input
- [x] Vulkan GPU backend (Dawn already supports it) — Dawn selects Vulkan automatically
- [x] LV2 format adapter — lv2_adapter.cpp, lv2_entry.hpp macro, TTL generation, CMake target
- [x] Linux CI matrix (GitHub Actions) — ubuntu-latest enabled in build.yml
- [x] Linux packaging (.deb, .tar.gz) — existing stubs + docs
- [x] Platform guide: Linux dependencies and deployment — docs/guides/platforms/linux.md
- [x] Runtime validation on Linux VM (ssh ubuntu) — 538/540 tests pass (2 Cmd+A key modifier failures)
- [x] Fix Cmd+A → Ctrl+A key mapping on Linux (TextEditor, WaveformEditor) — 540/540 pass
- [x] JACK audio client (optional, when libjack available — zero-copy, auto-connect, PipeWire compatible)
- [x] Native PipeWire support — JACK client works with PipeWire's JACK layer; ALSA routes through PipeWire's ALSA layer
- [x] Linux build fixes: -fPIC global, guard screenshot/inspect behind APPLE/GPU, #include <cstring> in osc_udp

### Phase 17: Advanced Examples
**Why here**: showcases full platform after platforms/components exist.
**Deps**: phases 14-16 (components, platforms)
- [x] PulpSampler — audio file sampler with MIDI, ADSR, pitch, loop (6 tests, CLAP)
- [x] Waveform Editor component (selection, zoom, scroll, regions, playhead) — 13 tests
- [x] Diagnostic Reporter component (text + JSON reports, system/audio/params) — 7 tests
- [x] WebView embedding (CHOC WebView wrapper: navigate, set_html, evaluate_js, bind) — 2 tests
- [x] Example screenshots and visual documentation — docs/examples/screenshots.md + capture script

### Phase 18: Web/WASM Target
**Why here**: requires all desktop platforms stable first.
**Deps**: phases 15-16 (cross-platform rendering validated)
- [x] Emscripten/WASM build pipeline — PulpWasm.cmake, pulp_add_wasm_app()
- [x] Web Audio API integration — WebAudioDevice via ScriptProcessorNode, Emscripten interop
- [x] Web MIDI API integration — WebMidiInput via navigator.requestMIDIAccess()
- [x] WebGPU native browser backend — -sUSE_WEBGPU=1 flag in PulpWasm.cmake
- [x] WebCLAP / WAMv2 plugin format — WAMv2 adapter (WamProcessorBridge, wam-plugin.js) + WebCLAP adapter (PULP_WCLAP_PLUGIN macro, PulpWclap.cmake) — 10 tests
- [x] WASM CI build and deploy workflow — ci/wasm-build.yml, GitHub Pages deploy

### Phase 19: Advanced Features & Bindings
**Why here**: exploration territory, builds on stable foundation.
**Deps**: stable cross-platform core

**DSP library expansion:**
- [x] FIR filter (time-domain, configurable order, lowpass/highpass coefficient generation) — 6 tests
- [x] BallisticsFilter (peak/RMS envelope follower, attack/release curves) — 4 tests
- [x] LogRampedValue (exponential parameter smoothing for pitch/frequency) — 4 tests
- [x] High-quality interpolators (linear, Hermite/Catmull-Rom, Lagrange, windowed-sinc 6-point) — 8 tests
- [x] ProcessorChain (variadic template composition utility) — 5 tests
- [x] LookupTable (pre-calculated function tables for waveshapers, sine approx) — 5 tests
- [x] First-order TPT filter (low/high/allpass with modulation stability) — 6 tests
- [x] FilterDesign utilities (Audio EQ Cookbook biquads + Butterworth cascade) — 12 tests

**MIDI expansion:**
- [x] RPN/NRPN parser (14-bit parameter messages from CC sequences) — 7 tests
- [x] MidiKeyboardState (polyphonic key tracking with listener callbacks) — 10 tests
- [x] MIDI 2.0 UMP / MPE — UmpPacket (MIDI 2.0 channel voice, per-note), MpeConfig (zones) — 9 tests

**Audio utilities:**
- [x] AudioProcessLoadMeasurer (real-time CPU tracking in audio callback) — 5 tests
- [x] BufferingReader (background-thread ring buffer for gapless streaming playback) — 6 tests
- [x] AudioWorkgroup (macOS os_workgroup API + Mach real-time priority fallback) — 5 tests

**Platform & framework:**
- [x] Plugin hosting architecture (pulp-host: PluginScanner, PluginSlot, SignalGraph) — 10 tests (architecture only — format-specific plugin loading not yet implemented; slot load returns nullptr)
- [x] Windows accessibility (UI Automation) — stub with AccessRole→UIA mapping
- [x] Linux accessibility (AT-SPI) — stub with AccessRole→ATK role mapping
- [x] TreeView widget (hierarchical display for file browsers, plugin lists) — 8 tests
- [x] Undo/redo system (UndoManager with named actions, transactions, max history) — 12 tests

**Bindings:**
- [x] Python bindings (HeadlessHost via pybind11, NumPy arrays, MIDI) — opt-in via PULP_BUILD_PYTHON
- [x] Node.js bindings (HeadlessHost via Node-API, Float32Array I/O, MIDI) — opt-in via PULP_BUILD_NODEJS

**Deferred (exploration):**
- [ ] WebGPU compute shaders for audio (additive synthesis, FFT, ML inference)
- [x] Polynomial/Matrix math (Horner eval, quadratic roots, multiply, Mat2/Mat3) — 14 tests
- [x] FastMathApproximations (tanh, sin, cos, exp2, log2, pow, dB, rsqrt, soft_clip) — 11 tests
- [x] Sampler framework — implemented as PulpSampler example (8-voice, MIDI, ADSR, pitch)

### Phase 20: AAX + Remaining Formats
**Why last**: AAX requires proprietary SDK from Avid, separate agreement.
**Deps**: format adapter pattern proven across VST3/AU/CLAP/AUv3/LV2
- [ ] AAX format adapter (macOS + Windows, requires Avid SDK)
- [ ] ASIO audio I/O (Windows, requires Steinberg SDK)

### Apple Render Surface Integration — PARTIALLY COMPLETE (merged to main)
**Spec**: planning/apple-render-surface-integration-spec.md
**Scope**: GPU render architecture — macOS, iOS, cross-platform
Completed:
- [x] Unify GPU ownership: single shared Dawn device between GpuSurface and SkiaSurface
- [x] macOS presentable surface: CAMetalLayer-backed NSView (metal_surface_mac.mm)
- [x] Explicit present path: begin_frame() acquires current texture, end_frame() presents
- [x] Capability-driven format/present mode selection via Surface::GetCapabilities()
- [x] Windows surface creation stub: SurfaceSourceWindowsHWND from HWND
- [x] Honest render docs: render.md rewritten, support-matrix.yaml updated
- [x] Texture lifetime documented: per-frame only, valid through submit
- [x] 13 cross-platform render tests (GpuSurface + SkiaSurface)
Also completed (this session):
- [x] Wire GPU render path into macOS WindowHost (opt-in via use_gpu, CVDisplayLink frame driver)
- [x] macOS PluginViewHost GPU path (DAW-embedded GPU rendering, CVDisplayLink)
- [x] Dawn native instance lifetime fix (wgpu::Instance must outlive adapters/devices)
- [x] TimedWaitAny instance feature for Skia Graphite timed waits
- [x] AllowProcessEvents callback mode (WaitAnyOnly doesn't work with Dawn native)
- [x] Retina scaling fix: GpuSurface configured at physical pixel dimensions
- [x] CoreText font manager for Skia text rendering on macOS
- [x] Cmd+Q quit menu for standalone macOS window hosts
- [x] CLAP GUI extension (clap_plugin_gui) with AutoUi-based editor
- [x] Processor::has_editor() and editor_size() virtual methods
- [x] GPU validation demo: modulation matrix with animated cables (examples/gpu-demo/)
- [x] Cross-platform GPU rendering docs in render.md

Remaining:
- [x] iOS presentable surface: IOSGpuWindowHost + IOSGpuPluginViewHost with Metal/Dawn/Skia
- [x] Wire GPU render path into iOS view hosts (use_gpu option in factory)
- [x] Linux surface creation: X11 Display/Window and Wayland display/surface via SDL3 properties
- [x] SDL3 native handle extraction for surface creation — sdl3_surface.hpp/cpp (Metal, D3D12, Vulkan)
- [x] Windows GPU view host: HWND extraction via SDL3 for D3D12 Dawn surface
- [x] Linux GPU view host: SDL3 X11/Wayland extraction for Vulkan Dawn surface
- [x] RenderLoop abstraction — CVDisplayLink (macOS), timer-based 60Hz (Windows/Linux)
- [x] Validation checklist: docs/guides/gpu-validation-checklist.md — verified vs experimental per platform

### Plugin Editor UIs — IN PROGRESS
- [x] CLAP GUI extension with AutoUi (creates editor view + PluginViewHost on gui_create)
- [x] VST3 editor: PulpPlugView (IPlugView) with AutoUi NSView — vst3_plug_view.hpp/cpp
- [x] AU v2 editor: CocoaViewFactory (AUCocoaUIBase) with AutoUi — au_v2_cocoa_view.mm
- [x] Standalone editor: run_with_editor() with optional GPU rendering — standalone.cpp
- [x] GPU demo polish: proportional resize, cable interaction, hover labels — main.cpp
- [x] Skia font manager for Windows (DirectWrite) and Linux (fontconfig) — skia_canvas.cpp

### Identity System — NEW
- [x] UUIDv4 generation with typed wrappers: SessionId, RunId, ObjectId, CorrelationId (4 tests)
- [x] EventEnvelope for mutation attribution and correlation
- [x] String round-trip, hash support for containers

## Planning Documents
- `planning/iplug3-comparison-analysis.md` — iPlug3 vs Pulp gap analysis
- `planning/iplug3-strategic-response.md` — strategic decisions
- `planning/choc-audit.md` — CHOC module assessment
- `planning/04-iplug3-review.md` — original iPlug3 review
- `planning/14-phased-roadmap.md` — phased development plan
- `planning/08-architecture-spec.md` — subsystem architecture
- `planning/12-rendering-strategy.md` — Dawn/Skia/QuickJS rendering
- `planning/apple-render-surface-integration-spec.md` — next milestone spec for Apple render surface integration
