# GAP Analysis: JUCE 8.0.12 vs Pulp Planning

Date: 2026-03-24
Source: Cross-reference of JUCE audit (`02-juce-audit.md`), capability matrix (`07-capability-matrix.md`), architecture spec (`08-architecture-spec.md`), phased roadmap (`14-phased-roadmap.md`), STATUS.md, and VISION.md.

---

## Methodology

Each JUCE capability was checked against:
1. Pulp's current implementation (STATUS.md)
2. Pulp's planning docs (roadmap, architecture spec, capability matrix)
3. Pulp's vision (VISION.md)

Gaps are categorized as:
- **TRUE GAP** — Useful capability not covered in any planning doc. Should be added.
- **PARTIAL GAP** — Mentioned but underspecified or missing from a specific phase. Needs clarification.
- **INTENTIONAL PASS** — JUCE has it but Pulp should skip it, with justification.

---

## TRUE GAPS — Add to Planning

### 1. Audio File Format I/O (pulp-audio or new pulp-media)

**JUCE:** Full audio file codec stack — WAV, AIFF, FLAC, Ogg Vorbis read/write; platform codecs (CoreAudio/Media Foundation) for AAC, ALAC, MP3; AudioFormatManager for format detection; BufferingAudioReader for streaming playback; AudioThumbnail for waveform cache.

**Pulp current:** Not implemented. STATUS.md lists "CHOC audio file I/O (WAV/FLAC/OGG/MP3) for PulpSampler" under Phase 9.

**Gap:** Audio file I/O is mentioned only as a Phase 9 item for PulpSampler, but it's a fundamental capability needed much earlier:
- Phase 4 standalone apps need audio file playback
- Phase 5 state serialization may embed audio
- Phase 6 waveform display needs audio file loading
- Golden-file tests need WAV read/write

**Recommendation:** Move audio file I/O to Phase 4 as a deliverable. Use CHOC's `choc::audio::AudioFileFormat` (WAV/FLAC/OGG/MP3). Add to `pulp-audio` or create `pulp-media`. Minimum: WAV read/write in Phase 4, full codec support in Phase 9.

**Where to add:** Phase 4 deliverables, `pulp-audio` subsystem.

---

### 2. OSC (Open Sound Control)

**JUCE:** `juce_osc` module — OSCSender/OSCReceiver over UDP, message/bundle types, address pattern matching.

**Pulp current:** Not mentioned in any planning document or STATUS.md.

**Gap:** OSC is standard for:
- Control surface integration (TouchOSC, Lemur)
- Inter-application communication (Max/MSP, SuperCollider, Processing)
- Live performance control
- Remote plugin control (complements MCP for non-AI use cases)

**Recommendation:** Add as Phase 9 or Phase 10 deliverable. Create `pulp-osc` module (small, ~2K lines). Implement from OSC 1.0 spec (public domain). Consider OSC over WebSocket for browser integration.

**Where to add:** Phase 9 deliverables, new `pulp-osc` subsystem.

---

### 3. Animation System

**JUCE:** `juce_animation` module — Animator for property animations, VBlank-synchronized animation targets, easing/interpolation.

**Pulp current:** Not explicitly mentioned. The capability matrix (07) lists "Animation system (property interpolation, easing)" as "No" for v1, "Yes" for long-term. The architecture spec mentions `View` properties can be bound to observables but doesn't describe animation.

**Gap:** Modern plugin UIs expect smooth transitions:
- Knob/fader value changes
- Theme switching
- Panel show/hide
- Meter ballistics (already covered separately)
- Widget hover/press states

**Recommendation:** Add animation primitives to Phase 6 `pulp-view`. Define an `Animator` or `Tween` system in the JS scripting layer (natural fit — JS animation APIs are well-understood). This is where Pulp can exceed JUCE: frame-rate-independent, GPU-accelerated animations defined in JS with easing functions.

**Where to add:** Phase 6 deliverables under `pulp-view`.

---

### 4. MIDI File I/O

**JUCE:** `MidiFile` class — standard MIDI file reader/writer, `MidiMessageSequence` with note-on/off pairing.

**Pulp current:** STATUS.md lists "CHOC MIDI file I/O for MIDI sequence import/export" under Phase 9.

**Gap:** MIDI file I/O is needed for:
- PulpDrums (Phase 6) — export generated patterns as MIDI files
- Preset systems that include MIDI sequences
- Test fixtures for MIDI processing

**Recommendation:** Move to Phase 6 alongside PulpDrums. Use CHOC's `choc::midi::File` (already a dependency). Minimal effort since CHOC provides it.

**Where to add:** Phase 6 deliverables (or Phase 4 with MIDI subsystem).

---

### 5. Plugin Hosting (pulp-host)

**JUCE:** `AudioProcessorGraph`, `AudioPluginFormat`, `AudioPluginFormatManager`, `KnownPluginList`, `PluginDescription`, `PluginDirectoryScanner` — complete infrastructure for scanning, loading, instantiating, and routing plugins.

**Pulp current:** Listed in capability matrix as "No" for v1, "Yes" for long-term. Listed in architecture spec subsystem list. NOT in the phased roadmap as a deliverable in any phase.

**Gap:** Plugin hosting enables:
- Building DAW-like applications
- Plugin chaining tools
- Test harnesses that load and validate other plugins
- Modular routing applications

**Recommendation:** Add as a Phase 10 or post-launch deliverable. This is a major feature that differentiates Pulp as a full application framework, not just a plugin SDK. Specify in roadmap even if deferred. Create `pulp-host` module with `Scanner`, `PluginSlot`, `SignalGraph` abstractions.

**Where to add:** Phase 10 deliverables or explicitly marked as post-launch.

---

### 6. Clipboard and Drag-and-Drop

**JUCE:** Platform clipboard (copy/paste text, images, custom data), OS-level drag-and-drop (files, custom types), internal drag-and-drop within component trees.

**Pulp current:** VISION.md mentions "file dialogs, message boxes, and popup menus use the platform's native UI." Capability matrix lists drag-and-drop as "No" for v1, "Yes" for long-term. Not in roadmap deliverables.

**Gap:** Critical for:
- Preset drag-and-drop
- File loading (drag audio file onto plugin)
- PulpSampler (Phase 9) — drag-and-drop audio loading
- Copy/paste of parameter values, presets, MIDI data
- Modular routing UIs (drag wires between nodes)

**Recommendation:** Add clipboard to Phase 6 (`pulp-view`). Add drag-and-drop to Phase 6 or Phase 9. Platform clipboard is small (~200 lines per platform). Drag-and-drop is larger but essential for PulpSampler.

**Where to add:** Phase 6 deliverables under `pulp-view`.

---

### 7. Native File/Color Dialogs

**JUCE:** Native file chooser (open/save), color picker, alert dialogs — all using platform-native widgets.

**Pulp current:** VISION.md says "file dialogs, message boxes, and popup menus use the platform's native UI." Architecture spec mentions `WindowManager` for popups. Not explicitly in roadmap deliverables.

**Gap:** Needed for:
- File open/save (presets, audio files, exports)
- Standalone app workflows
- PulpSampler file loading

**Recommendation:** Add to Phase 6 under `pulp-view` platform integration. CHOC may provide some of this (`choc::ui::DesktopWindow` has dialog helpers). Small effort, high impact.

**Where to add:** Phase 6 deliverables under `pulp-view`.

---

### 8. IPC (Inter-Process Communication)

**JUCE:** `InterprocessConnection` — named pipe or socket-based IPC between processes, `InterprocessConnectionServer`.

**Pulp current:** MCP server provides AI-tool IPC. No generic IPC abstraction.

**Gap:** Needed for:
- Multi-process plugin architectures (sandbox isolation)
- Communication between standalone app and plugin instances
- External controller apps
- Diagnostic tools connecting to running plugins

**Recommendation:** The MCP server covers the AI use case. For generic IPC, add as Phase 9 or Phase 10. CHOC's `choc::network` may cover this. Consider whether MCP's JSON-RPC is sufficient for most IPC needs (it likely is — this may be an intentional pass).

**Where to add:** Phase 9 or 10, or document as covered by MCP.

---

### 9. Generic Auto-Generated Parameter UI

**JUCE:** `GenericAudioProcessorEditor` — automatically generates a basic UI for any plugin's parameters (sliders for floats, toggles for bools, combo boxes for choices).

**Pulp current:** Not mentioned explicitly, though the JS widget bridge could generate UIs from parameter definitions.

**Gap:** Essential for:
- Rapid prototyping (see parameters before building a real UI)
- Headless-to-visual debugging
- Plugin validation and testing

**Recommendation:** Add to Phase 6 or Phase 7. The JS scripting layer makes this trivial: a generic JS widget file that reads parameter definitions and generates controls. Exceeds JUCE by being hot-reloadable and themeable.

**Where to add:** Phase 6 deliverables under `pulp-view` or Phase 7 as a `/pulp:inspect` feature.

---

### 10. SVG/Vector Asset Loading

**JUCE:** Drawable system with SVG parser — load SVG files as vector graphics for icons, decorations, scalable artwork.

**Pulp current:** Not mentioned. Skia Graphite can render paths/shapes but no SVG loader is specified.

**Gap:** Plugin UIs commonly use SVG for:
- Scalable icons
- Custom knob/slider artwork
- Background artwork
- Logo/branding assets

**Recommendation:** Add SVG loading to Phase 6 under `pulp-canvas`. Skia has built-in SVG support. Alternatively, use a lightweight SVG parser (nanosvg, MIT) to convert SVG to Canvas paths at load time.

**Where to add:** Phase 6 deliverables under `pulp-canvas`.

---

### 11. Audio Data Converters (Sample Format Conversion)

**JUCE:** `AudioDataConverters` — convert between int8/16/24/32, float32, float64 sample formats.

**Pulp current:** Not explicitly mentioned. The audio subsystem uses float buffers.

**Gap:** Needed for:
- Audio file I/O (reading int16/24 WAV files)
- ASIO/WASAPI exclusive mode (may deliver int32 buffers)
- Interop with platform audio APIs

**Recommendation:** Add to Phase 4 under `pulp-audio`. Small utility (~200 lines). CHOC may provide some of this.

**Where to add:** Phase 4 deliverables under `pulp-audio`.

---

### 12. Popup Menus and Context Menus

**JUCE:** `PopupMenu` — native-looking popup menus with submenus, checkmarks, icons, separators, keyboard shortcuts.

**Pulp current:** VISION.md mentions popup menus. Capability matrix lists menus as "Yes" for v1. Not explicitly in Phase 6 deliverables.

**Gap:** Every plugin needs right-click context menus for:
- Preset selection
- Parameter reset to default
- Copy/paste parameter values
- Format-specific options

**Recommendation:** Ensure popup menus are explicitly listed in Phase 6 widget deliverables. Use platform-native menus (NSMenu, Win32 TrackPopupMenu) for correct DAW behavior.

**Where to add:** Phase 6 deliverables under `pulp-view`.

---

## PARTIAL GAPS — Needs Clarification in Planning

### 13. DSP Library Breadth (pulp-signal)

**JUCE juce_dsp:** ProcessorChain, FFT (multi-backend), Convolution (uniform + non-uniform partitioned), IIR/FIR/SVF/TPT/LadderFilter, LinkwitzRileyFilter, Oversampling (2x-16x), DelayLine (4 interpolation modes), Compressor/Limiter/NoiseGate, Reverb, Chorus/Phaser, Oscillator (wavetable), WaveShaper, Gain/DryWetMixer/Panner, LookupTable, BallisticsFilter, WindowingFunction.

**Pulp current:** `pulp-signal` listed in Phase 9 as "DSP library (filters, FFT, oversampling, oscillators)". STATUS.md confirms "not started."

**Gap:** The scope of pulp-signal is underspecified. Phase 9 just says "DSP library" without listing which processors are included.

**Recommendation:** Enumerate the target DSP processors in Phase 9:
- **Core (must-have):** FFT, Convolver, Biquad/IIR, SVF/TPT, Oversampling, DelayLine, SmoothedValue/Ramp, ADSR, Oscillator (polyBLEP + wavetable), Gain, DryWetMixer
- **Effects (should-have):** Compressor, Limiter, NoiseGate, Chorus, Phaser, Reverb (FDN), Panner
- **Advanced (nice-to-have):** LadderFilter, LinkwitzRiley, WaveShaper, WindowingFunction, ConvolutionReverb

**Where to add:** Phase 9 deliverables with explicit list.

---

### 14. Accessibility Depth

**JUCE:** Full platform accessibility on macOS (NSAccessibility), iOS (UIAccessibility), Windows (UI Automation), Android (TalkBack). Every standard widget exposes accessible name, role, value, and actions.

**Pulp current:** STATUS.md shows "Accessibility foundation — AccessRole/label/value on View, default roles for all widgets (2 tests)" as done in Phase 6. "Full accessibility: NSAccessibility protocol in PulpPluginView" is listed as TODO.

**Gap:** The foundation is laid but full platform integration is only partially specified:
- macOS NSAccessibility: listed as TODO
- Windows UI Automation: not mentioned
- Linux AT-SPI: not mentioned
- Keyboard navigation: not explicitly specified

**Recommendation:** Ensure Phase 6 explicitly includes:
- macOS: Complete NSAccessibility protocol on PulpPluginView (already listed as TODO). This is high priority because it also enables XCUITest and XcodeBuildMCP UI automation — giving Pulp accessibility-based UI testing for free on macOS. Every widget's AccessRole/label/value (already implemented) becomes discoverable and interactable by external test tools.
- Phase 6: Keyboard focus traversal (Tab/Shift-Tab through widgets)
- Phase 9/10: Windows UIA, Linux AT-SPI

**Where to add:** Phase 6 and Phase 10 deliverables.

---

## INTENTIONAL PASSES — Skip with Justification

### P1. Product Unlocking / Licensing (`juce_product_unlocking`)

**JUCE:** RSA-based license key generation, online/offline activation, machine fingerprinting, trial management.

**Why pass:** Licensing is a business concern, not a framework concern. The JUCE audit itself says "None" for Pulp parity. Modern licensing is better handled by dedicated services (Keygen.sh, Gumroad, Paddle) or custom per-vendor implementations. Including a licensing system in the framework creates vendor lock-in and encourages a specific business model. Pulp's MCP/CLI architecture actually makes it easier for third-party licensing solutions to integrate.

---

### P2. Analytics (`juce_analytics`)

**JUCE:** Singleton analytics event logging with pluggable backend destinations.

**Why pass:** Tiny module (1,115 lines) with minimal functionality. Real analytics requires a proper SDK (Sentry, Amplitude, PostHog). The JUCE module is a thin abstraction that most serious products outgrow immediately. Pulp's diagnostic reporter component (VISION.md) is more useful — collecting system info, crash logs, and DAW context for support tickets, rather than tracking user behavior.

---

### P3. Video/Camera (`juce_video`)

**JUCE:** Basic camera capture and video playback.

**Why pass:** Audio plugins don't need video. The few audio apps that need video (DAWs with video sync) use platform-specific video frameworks directly (AVFoundation, Media Foundation) for performance reasons. JUCE's video module is thin and rarely used. Not aligned with Pulp's focus.

---

### P4. Audio CD Reading

**JUCE:** `CDReaderComponent` — audio CD ripping UI (macOS/Windows).

**Why pass:** Optical drives are effectively obsolete. The JUCE audit itself calls this "obsolete." No modern audio workflow depends on CD reading. Not worth the code.

---

### P5. OpenGL Module

**JUCE:** OpenGL context attachment, shader/texture wrappers, GLEW bundling.

**Why pass:** OpenGL is deprecated on macOS (since 2018) and iOS. Pulp explicitly chose WebGPU/Dawn + Skia Graphite as the GPU path — this is a fundamental architectural decision, not a gap. Dawn targets Metal, D3D12, Vulkan, and WebGPU, covering all platforms with modern APIs. This is a clear strategic improvement over JUCE.

---

### P6. Box2D Physics (`juce_box2d`)

**JUCE:** Bundled Box2D physics engine with JUCE graphics integration.

**Why pass:** Physics in a UI framework is a novelty feature. If someone needs physics for a creative UI, Box2D is a standalone MIT-licensed library they can integrate themselves. Not a framework concern.

---

### P7. Cryptography Module (`juce_cryptography`)

**JUCE:** Custom MD5, SHA256, RSA, Blowfish implementations.

**Why pass:** Platform cryptography APIs (CommonCrypto, BCrypt, OpenSSL) are better maintained and more secure than custom implementations. Pulp should use platform crypto where needed (EdDSA for update signing, SHA256 for state checksums) rather than bundling its own crypto. CHOC or standard libraries cover hash needs.

---

### P8. Code Editor Widget (`CodeEditorComponent`)

**JUCE:** Syntax-highlighting code editor widget.

**Why pass:** A code editor is a niche widget. If needed (e.g., for a scripting interface in a plugin), Monaco via WebView or a lightweight editor library is superior. Pulp's WebView support (via CHOC) covers this use case better than a custom widget. The JS scripting layer could also expose a Monaco-based editor component.

---

### P9. System Tray (`SystemTrayIconComponent`)

**JUCE:** System tray/menu bar status item.

**Why pass:** Plugins don't use system trays. Standalone apps rarely need them. If needed, platform APIs are trivial to wrap directly. Not worth framework-level support for the audio plugin use case.

---

### P10. VST2, Unity Plugin, ARA Formats

**JUCE:** VST2 (deprecated SDK), Unity native audio plugin, ARA (Audio Random Access).

**Why pass:**
- **VST2:** SDK no longer distributed by Steinberg. Dead format for new development.
- **Unity:** Niche. Can be added as a community module if demand exists.
- **ARA:** Complex Celemony SDK integration. Extremely specialized (pitch editing, time-stretching inside DAW). Worth considering post-launch but not parity-critical.

---

### P11. WebView/Browser Embedding

**JUCE:** `WebBrowserComponent` — WKWebView (macOS/iOS), WebView2 (Windows), WebKitGTK (Linux).

**Why pass:** Pulp's entire vision is built around *not* using web views for plugin UIs. VISION.md has a dedicated section ("Why GPU Rendering with JS, Not Web Views?") listing the problems: cache pollution, process explosion, bridging pain, first-load jank, 50-100MB+ idle memory, CORS/CSP nonsense. Pulp's JS scripting + GPU rendering gives the web DX without the browser runtime baggage. If someone truly needs a WebView for a niche case (embedding a Monaco editor, showing HTML documentation), CHOC's `choc::ui::WebView` is available as a dependency — but Pulp should not build framework infrastructure around it or promote it as a UI path. For license text display, use a styled `Label` or `TextView` widget rendered natively through the canvas.

---

## TRUE GAP — Added Post-Initial Audit

### 16. Programmatic UI Testing via MCP (ViewInspector + Synthetic Events)

**JUCE:** No built-in UI testing infrastructure. Developers use external tools (Appium, XCUITest) or manual testing.

**Pulp current:** ViewInspector exists (find_by_id, view tree → JSON, 5 tests). RecordingCanvas enables headless rendering. Hit-testing infrastructure is built. MCP server planned for Phase 7.

**Gap:** No way to programmatically click/drag/interact with widgets in a running or headless plugin.

**Recommendation:** Add synthetic event simulation to Phase 7 as part of the MCP server. This is a **parity-exceeding differentiator** — no other audio framework has native AI-drivable UI testing.

Implementation (two layers):

**Layer 1 — Headless event simulation (Phase 6, CI-friendly):**
- Add `View::simulateMouseDown(x, y)`, `simulateMouseUp`, `simulateMouseDrag`, `simulateKeyPress` that feed synthetic `InputEvent` objects through the existing event dispatch
- Works with RecordingCanvas — no window, no GPU, pure C++ tests
- Use in Catch2 tests: instantiate View tree → simulate click on knob → assert parameter changed → assert canvas drew new state
- Enables UI interaction testing in CI with zero platform dependencies

**Layer 2 — MCP server endpoints (Phase 7, AI-drivable):**
- Expose via MCP: `simulate_click`, `simulate_drag`, `get_view_value`, `screenshot`
- Reuses Layer 1's synthetic event infrastructure, adds JSON-RPC transport
- AI agents can drive the full UI through the same MCP interface used for parameter control
- Works both headless and with a live window
- On macOS, full NSAccessibility on PulpPluginView also enables XCUITest/XcodeBuildMCP as a complementary path

**Where to add:** Layer 1 in Phase 6 (view/widget deliverables), Layer 2 in Phase 7 (MCP server).

---

## Summary: What to Add to the Prompt/Roadmap

### High Priority (add to active phases)
1. **Audio file I/O** → Phase 4 (via CHOC)
2. **Animation system** → Phase 6 (JS-based tweens)
3. **MIDI file I/O** → Phase 6 (via CHOC)
4. **Clipboard** → Phase 6
5. **Native file/color dialogs** → Phase 6
6. **Popup/context menus** → Phase 6
7. **SVG loading** → Phase 6 (via Skia)
8. **Auto-generated parameter UI** → Phase 6/7
9. **Audio data converters** → Phase 4
10. **DSP library scope** → Phase 9 (enumerate processors)

### High Priority (cont.)
11. **Programmatic UI testing via MCP** → Phase 7 (synthetic events + ViewInspector + screenshot)
12. **Headless event simulation for CI** → Phase 6/7 (synthetic InputEvent dispatch into View tree + RecordingCanvas, no window needed)

### Medium Priority (add to later phases)
13. **OSC** → Phase 9
14. **Plugin hosting** → Phase 10 or post-launch
15. **IPC** → Phase 9 (or covered by MCP)
16. **Full accessibility (Windows/Linux)** → Phase 10
