# JUCE Framework Audit

**Subject:** JUCE 8.0.12 — Comprehensive framework audit for Pulp design reference
**Source:** `/Users/danielraffel/Code/JUCE`
**Date:** 2026-03-24

---

## Table of Contents

1. [Repository Organization](#repository-organization)
2. [Module Dependency Graph](#module-dependency-graph)
3. [Per-Module Analysis](#per-module-analysis)
4. [Pain Points and Legacy Concerns](#pain-points-and-legacy-concerns)
5. [Licensing](#licensing)
6. [Test Infrastructure](#test-infrastructure)
7. [Summary: Implications for Pulp](#summary-implications-for-pulp)

---

## Repository Organization

The audited framework ships as a monorepo with the following top-level structure:

```
JUCE/
├── modules/              # 22 framework modules (the core of the framework)
│   ├── CMakeLists.txt    # Module-level CMake aggregation
│   ├── juce_analytics/
│   ├── juce_animation/
│   ├── juce_audio_basics/
│   ├── juce_audio_devices/
│   ├── juce_audio_formats/
│   ├── juce_audio_plugin_client/
│   ├── juce_audio_processors/
│   ├── juce_audio_processors_headless/
│   ├── juce_audio_utils/
│   ├── juce_box2d/
│   ├── juce_core/
│   ├── juce_cryptography/
│   ├── juce_data_structures/
│   ├── juce_dsp/
│   ├── juce_events/
│   ├── juce_graphics/
│   ├── juce_gui_basics/
│   ├── juce_gui_extra/
│   ├── juce_javascript/
│   ├── juce_midi_ci/
│   ├── juce_opengl/
│   ├── juce_osc/
│   ├── juce_product_unlocking/
│   └── juce_video/
├── extras/               # Companion applications and tools
│   ├── Projucer/         # Legacy project generator (IDE-like GUI tool)
│   ├── AudioPluginHost/  # Plugin host for testing
│   ├── UnitTestRunner/   # Test harness application
│   ├── BinaryBuilder/    # Resource embedding utility
│   ├── AudioPerformanceTest/
│   ├── NetworkGraphicsDemo/
│   ├── WindowsDLL/       # Windows DLL wrapper example
│   └── Build/            # CMake machinery
│       └── CMake/
│           ├── JUCEUtils.cmake           # Core build utilities
│           ├── JUCEModuleSupport.cmake   # Module discovery and linking
│           ├── JUCEHelperTargets.cmake   # Helper target definitions
│           ├── JUCECheckAtomic.cmake     # Atomic library detection
│           └── (20+ additional CMake files, templates, storyboards)
├── examples/             # Demo applications and templates
│   ├── DemoRunner/       # All-in-one demo showcase
│   ├── CMake/            # CMake project templates
│   ├── Audio/            # Audio-focused examples
│   ├── DSP/              # DSP processing examples
│   ├── GUI/              # GUI component examples
│   ├── Plugins/          # Plugin format examples
│   ├── Utilities/        # Utility examples
│   └── Assets/           # Shared example resources
└── docs/                 # Documentation (sparse in-tree)
```

### Key Observations

- **22 modules** form the framework's functional surface area, ranging from 1,115 lines (analytics) to 135,345 lines (gui_basics)
- **extras/Build/** contains the CMake machinery that makes the framework usable as a CMake dependency, including the `juceaide` helper binary generation and plugin format packaging logic
- **Projucer** is still shipped but increasingly deprecated in favor of CMake
- **examples/** provides both standalone demos (DemoRunner) and CMake-based project templates (PIP system)
- Total module codebase: approximately 903,000 lines across 2,286 files (.h, .cpp, .mm)

---

## Module Dependency Graph

The 22 modules form a directed acyclic dependency graph. Each module declares its dependencies in its main header file via a `dependencies:` metadata field.

### Primary Chains

```
GUI Chain:
  juce_core
    └── juce_events
          └── juce_data_structures
          └── juce_graphics
                └── juce_gui_basics (depends on both juce_graphics AND juce_data_structures)
                      └── juce_gui_extra
                            └── juce_opengl
                            └── juce_video

Audio Chain:
  juce_core
    └── juce_audio_basics
          ├── juce_audio_devices (also depends on juce_events)
          ├── juce_audio_formats
          │     └── juce_dsp
          ├── juce_audio_processors_headless (also depends on juce_events)
          │     └── juce_audio_processors (also depends on juce_gui_extra)
          │           └── juce_audio_plugin_client
          │           └── juce_audio_utils (also depends on juce_audio_formats, juce_audio_devices)
          └── juce_midi_ci

Cryptography Chain:
  juce_core
    └── juce_cryptography
          └── juce_product_unlocking
```

### Standalone Leaf Modules

These modules have minimal dependencies and connect at specific points:

| Module | Depends On | Notes |
|--------|-----------|-------|
| juce_analytics | juce_gui_basics | GUI analytics reporting |
| juce_animation | juce_gui_basics | Component animation system |
| juce_box2d | juce_graphics | Physics library (unrelated to audio) |
| juce_javascript | juce_core | QuickJS/V8 JavaScript engine |
| juce_osc | juce_events | Open Sound Control protocol |

### Full Dependency Matrix

| Module | Direct Dependencies |
|--------|-------------------|
| juce_core | *(none — foundation)* |
| juce_events | juce_core |
| juce_data_structures | juce_events |
| juce_graphics | juce_events |
| juce_gui_basics | juce_graphics, juce_data_structures |
| juce_gui_extra | juce_gui_basics |
| juce_opengl | juce_gui_extra |
| juce_video | juce_gui_extra |
| juce_audio_basics | juce_core |
| juce_audio_devices | juce_audio_basics, juce_events |
| juce_audio_formats | juce_audio_basics |
| juce_dsp | juce_audio_formats |
| juce_audio_processors_headless | juce_audio_basics, juce_events |
| juce_audio_processors | juce_gui_extra, juce_audio_processors_headless |
| juce_audio_plugin_client | juce_audio_processors |
| juce_audio_utils | juce_audio_processors, juce_audio_formats, juce_audio_devices |
| juce_midi_ci | juce_audio_basics |
| juce_cryptography | juce_core |
| juce_product_unlocking | juce_cryptography |
| juce_analytics | juce_gui_basics |
| juce_animation | juce_gui_basics |
| juce_box2d | juce_graphics |
| juce_javascript | juce_core |
| juce_osc | juce_events |

### Critical Coupling Observation

The `juce_audio_processors` module depends on `juce_gui_extra`, which pulls in the entire GUI chain. This means building a plugin — even a headless one — required the full graphics stack until the introduction of `juce_audio_processors_headless` as a partial mitigation. This coupling is a significant architectural issue that Pulp must avoid.

---

## Per-Module Analysis

### 1. juce_core

| Attribute | Value |
|-----------|-------|
| **Size** | 103,712 lines / 282 files |
| **Complexity** | Very Large |
| **Dependencies** | None (foundation module) |
| **macOS Frameworks** | Cocoa, Foundation, IOKit, Security |

**Purpose:** Foundation module providing all base types, containers, utilities, threading primitives, file I/O, serialization, networking, and string handling.

**Key Capabilities:**
- Custom UTF-8 String class with reference-counted storage, CharPointer iterators, and extensive conversion methods
- Container classes: Array, OwnedArray, ReferenceCountedArray, SortedSet, HashMap, LinkedListPointer, AbstractFifo
- Threading: Thread, ThreadPool, CriticalSection, SpinLock, WaitableEvent, ReadWriteLock, atomic wrappers
- File I/O: File, FileInputStream/OutputStream, TemporaryFile, DirectoryIterator, FileSearchPath
- Serialization: XML parser/writer, JSON parser/writer, ValueTree (hybrid tree data structure with undo support)
- Networking: URL, Socket, StreamingSocket, WebInputStream
- Compression: bundled zlib, GZIPCompressor/Decompressor, ZipFile
- System: SystemStats, Process, PerformanceCounter, Time, RelativeTime, UUID, IPAddress
- Memory: MemoryBlock, MemoryOutputStream, HeapBlock, OptionalScopedPointer
- Var: variant value type supporting int, double, string, array, object, binary, method references

**Platform-Specific Implementations:**
- macOS/iOS: Foundation-based file I/O, Mach threading, CoreFoundation string bridge
- Windows: Win32 file I/O, Win32 threading, WideString conversion
- Linux: POSIX file I/O, pthread, inotify file watching
- Android: JNI interop layer

**Strengths:**
- Comprehensive and battle-tested across millions of deployed applications
- ValueTree is a powerful undo-capable hierarchical data structure
- Variant type (Var) is flexible for dynamic data

**Weaknesses:**
- Custom String class predates adequate `std::string` Unicode support but now creates constant conversion friction with standard library code
- Many container types duplicate `std::vector`, `std::map`, `std::unordered_map` functionality
- ValueTree has significant overhead for simple key-value storage (string-interned keys, dynamic variant values, listener lists)
- Networking is rudimentary compared to modern HTTP libraries
- No `std::filesystem` integration despite C++17 requirement

**Pulp Must-Have Parity:**
- File I/O abstraction (but use `std::filesystem`)
- Threading primitives (but use `std::thread`, `std::mutex`, `std::atomic`)
- JSON serialization
- Lock-free FIFO (AbstractFifo pattern)

**Modern Redesign Opportunities:**
- Replace custom String with `std::string` + ICU or `std::u8string`
- Replace custom containers with STL containers
- Replace custom threading with C++17/20 standard threading
- Replace networking with a modern HTTP library or platform APIs
- Replace XML with a lighter format (MessagePack, CBOR) for state serialization
- Replace ValueTree with a typed, schema-based state system

---

### 2. juce_events

| Attribute | Value |
|-----------|-------|
| **Size** | 9,252 lines / 55 files |
| **Complexity** | Medium |
| **Dependencies** | juce_core |
| **macOS Frameworks** | *(inherited from juce_core)* |

**Purpose:** Event dispatch system centered on the MessageManager singleton, which acts as the main-thread dispatcher for all UI and timer events.

**Key Capabilities:**
- MessageManager: Singleton that owns the message loop; all GUI operations must be dispatched through it
- Timer: Periodic callback on the message thread (not high-precision)
- AsyncUpdater: Coalesce multiple trigger calls into a single callback on the message thread
- ChangeBroadcaster/ChangeListener: Observer pattern for change notifications
- ActionBroadcaster/ActionListener: String-based message passing
- InterprocessConnection: Named pipe or socket-based IPC between processes
- CallbackMessage: Post arbitrary closures to the message thread
- MessageManagerLock: RAII lock for calling message-thread code from other threads

**Platform-Specific Implementations:**
- macOS: NSRunLoop integration, CFRunLoopTimer
- Windows: Win32 message pump, PostMessage/PeekMessage
- Linux: Custom X11 event loop with fd-based wakeup
- Android: Android Looper integration

**Strengths:**
- Reliable cross-platform event dispatch
- Timer and AsyncUpdater are well-designed for common audio plugin patterns
- InterprocessConnection provides useful IPC

**Weaknesses:**
- MessageManager singleton makes unit testing nearly impossible — you cannot construct components or test event-driven code without initializing the singleton, which conflicts with test isolation
- Tight coupling: all GUI code implicitly depends on this singleton existing
- No support for multiple event loops or custom dispatchers
- MessageManagerLock can deadlock if misused

**Pulp Must-Have Parity:**
- Main-thread dispatcher pattern (but not as a singleton)
- Timer callbacks
- Async coalescing (AsyncUpdater pattern)
- Lock-free message posting from audio thread

**Modern Redesign Opportunities:**
- Replace singleton with dependency-injected event loop
- Use `std::function` closures instead of inheritance-based listeners
- Support multiple dispatch contexts for testing
- Consider `std::jthread` and stop tokens for cancelable timers

---

### 3. juce_data_structures

| Attribute | Value |
|-----------|-------|
| **Size** | 5,571 lines / 21 files |
| **Complexity** | Small |
| **Dependencies** | juce_events |

**Purpose:** Higher-level data structures, primarily the UndoManager and ValueTree listener infrastructure.

**Key Capabilities:**
- UndoManager: Generic undo/redo stack with UndoableAction interface
- ValueTree: (Defined in juce_core but listener dispatch lives here due to events dependency)
- CachedValue: Typed wrapper around a ValueTree property with automatic synchronization
- Value: Observable wrapper around a Var that notifies listeners on change

**Strengths:**
- UndoManager is well-designed and widely used in plugin state management
- CachedValue provides convenient typed access to ValueTree properties

**Weaknesses:**
- Small module that exists primarily to break a circular dependency between juce_core and juce_events
- Value/CachedValue patterns encourage stringly-typed programming

**Pulp Must-Have Parity:**
- Undo/redo system
- Observable value wrapper

**Modern Redesign Opportunities:**
- Merge into core or events module
- Replace stringly-typed ValueTree properties with compile-time typed state

---

### 4. juce_graphics

| Attribute | Value |
|-----------|-------|
| **Size** | 78,545 lines / 220 files |
| **Complexity** | Large |
| **Dependencies** | juce_events |
| **macOS Frameworks** | Cocoa, QuartzCore |

**Purpose:** Full 2D rendering engine including geometry, color, images, fonts, and platform-specific rendering backends.

**Key Capabilities:**
- Geometry: Point, Line, Rectangle, Path, AffineTransform, PathFlatteningIterator, EdgeTable
- Color: Colour, ColourGradient, FillType, PixelARGB/RGB/Alpha
- Images: Image, ImageConvolutionKernel, ImageCache, ImageFileFormat (PNG, JPEG, GIF readers)
- Fonts: Font, Typeface, GlyphArrangement, AttributedString, TextLayout
- Text shaping: Bundled HarfBuzz library for complex text layout
- Bidirectional text: Bundled SheenBidi library for RTL/LTR text
- Rendering: Graphics context with software rasterizer, CoreGraphics (macOS/iOS), Direct2D (Windows)
- Drawables: SVG-like vector drawable hierarchy (DrawablePath, DrawableComposite, DrawableText, DrawableImage)

**Platform-Specific Implementations:**
- macOS/iOS: CoreGraphics (Quartz 2D) rendering backend, CoreText font rendering
- Windows: Direct2D rendering backend (added in JUCE 8), software fallback
- Linux: Software rendering (cairo-like EdgeTable rasterizer), FreeType font rendering
- All platforms: Software rasterizer as universal fallback

**Bundled Third-Party Libraries:**
- HarfBuzz (Old MIT license) — complex text shaping
- SheenBidi (Apache 2.0) — Unicode bidirectional algorithm
- jpeglib (IJG license) — JPEG decoding
- pnglib (zlib license) — PNG decoding

**Strengths:**
- Comprehensive 2D vector rendering API
- Good text shaping with HarfBuzz integration
- Direct2D support is a significant Windows improvement in JUCE 8

**Weaknesses:**
- Linux gets only software rendering — no GPU acceleration
- No Metal rendering path despite Metal being the standard on macOS since 2018 (Metal is listed as a weak framework but not used for 2D rendering)
- Renderer fragmentation: software + CoreGraphics + Direct2D + OpenGL all have different performance characteristics and visual output differences
- Large bundled dependency footprint for what could be delegated to platform APIs or a single cross-platform renderer

**Pulp Must-Have Parity:**
- 2D vector rendering (paths, gradients, images)
- Text rendering with proper Unicode support
- Cross-platform font handling

**Modern Redesign Opportunities:**
- Use Metal for macOS/iOS rendering
- Use a single cross-platform GPU renderer (Skia, or WebGPU/Dawn)
- Delegate to platform text rendering (CoreText, DirectWrite, Pango) rather than bundling HarfBuzz
- For plugin UIs: consider SwiftUI on Apple, or a retained-mode GPU-backed renderer
- Separate rendering backend from geometry/color primitives

---

### 5. juce_gui_basics

| Attribute | Value |
|-----------|-------|
| **Size** | 135,345 lines / 380 files |
| **Complexity** | Very Large (largest module) |
| **Dependencies** | juce_graphics, juce_data_structures |
| **macOS Frameworks** | Cocoa, QuartzCore |

**Purpose:** The complete GUI toolkit including the Component system, native windowing, widget library, layout, accessibility, theming, and drag-and-drop.

**Key Capabilities:**

*Component System:*
- Component: Base class for all visual elements. Hierarchical parent-child ownership, paint/resize callbacks, mouse/keyboard event dispatch
- ComponentPeer: Native window bridge abstraction (one per top-level window)
- Desktop: Singleton managing all screens, mouse sources, and global keyboard focus

*Widget Library:*
- Text input: TextEditor (single and multi-line), Label
- Buttons: TextButton, ToggleButton, DrawableButton, ImageButton, ShapeButton, HyperlinkButton, ArrowButton
- Sliders: Slider (linear, rotary, two-value, three-value variants)
- Selection: ComboBox, ListBox, TableListBox, TreeView
- Containers: TabbedComponent, Viewport, ScrollBar, ConcertinaPanel
- Dialogs: AlertWindow, DialogWindow, FileChooser (native and custom), ColourSelector
- Menus: PopupMenu, MenuBarComponent, BurgerMenuComponent
- Toolbars: Toolbar, ToolbarItemComponent
- Misc: ProgressBar, BubbleComponent, TooltipWindow, CaretComponent

*Layout:*
- FlexBox: CSS Flexbox-compatible layout
- Grid: CSS Grid-compatible layout
- ComponentBoundsConstrainer: Resize constraints
- GroupComponent, SplitComponent (manual layout helpers)

*Theming:*
- LookAndFeel: Virtual method-based theming system with V1, V2, V3, V4 versions
- Each widget defines a nested ColourId enum and delegates drawing to LookAndFeel methods

*Accessibility:*
- AccessibilityHandler: Abstract accessibility interface
- Platform implementations: NSAccessibility (macOS), UIAccessibility (iOS), UIA (Windows), TalkBack (Android)

*Drawables:*
- DrawablePath, DrawableRectangle, DrawableComposite, DrawableText, DrawableImage
- SVG parser for loading vector artwork

*Native Windows:*
- ComponentPeer implementations for Cocoa (NSView), Win32 (HWND), X11, Android

**Platform-Specific Implementations:**
- macOS: NSView-based ComponentPeer, NSMenu integration, NSAccessibility
- iOS: UIView-based ComponentPeer, UIAccessibility
- Windows: HWND-based ComponentPeer with Direct2D rendering, UIAutomation accessibility
- Linux: X11-based ComponentPeer, XDnd drag-and-drop
- Android: Android View-based ComponentPeer, TalkBack accessibility

**Strengths:**
- Comprehensive widget set covering most UI needs
- Good accessibility support across all platforms
- FlexBox/Grid layout is well-implemented
- Drawable system provides convenient vector graphics

**Weaknesses:**
- **Monolithic LookAndFeel**: A single class that inherits 20+ widget style interfaces. Third-party themes must override a massive virtual method table. Adding a new widget means modifying the LookAndFeel base class.
- **Manual Component ownership**: `addAndMakeVisible()` does NOT take ownership. Components are typically raw pointer members that must outlive their parent. Destructor order bugs are a common source of crashes.
- **Raw pointer child management**: No smart pointer integration, no ownership transfer semantics
- **Very large module**: 135K+ lines means long compile times and high coupling surface
- **Singleton Desktop**: Global state makes testing difficult

**Pulp Must-Have Parity:**
- For plugin UIs: parameter-aware controls (knobs, sliders, buttons, menus)
- Layout system
- Accessibility
- Native window embedding (for plugin editors inside DAW windows)

**Modern Redesign Opportunities:**
- Use SwiftUI on Apple platforms instead of custom widget toolkit
- Use protocol/trait-based theming instead of monolithic virtual class
- Use unique_ptr ownership for child components
- Split into smaller modules: windowing, widgets, layout, accessibility
- Consider retained-mode or declarative UI instead of imperative paint callbacks

---

### 6. juce_gui_extra

| Attribute | Value |
|-----------|-------|
| **Size** | 26,896 lines / 68 files |
| **Complexity** | Medium |
| **Dependencies** | juce_gui_basics |
| **macOS Frameworks** | WebKit |

**Purpose:** Additional GUI components that depend on platform-specific frameworks beyond the basics: web views, code editor, native view embedding, system tray.

**Key Capabilities:**
- WebBrowserComponent: Embedded web view (WKWebView on macOS/iOS, WebView2 on Windows, WebKitGTK on Linux)
- CodeEditorComponent: Syntax-highlighting code editor widget
- Native view embedding: NSViewComponent (macOS), UIViewComponent (iOS), HWNDComponent (Windows), XEmbedComponent (Linux), AndroidViewComponent
- SystemTrayIconComponent: System tray/menu bar integration
- LiveConstantEditor: Runtime value tweaking for development

**Platform-Specific Implementations:**
- macOS: WKWebView, NSView embedding, NSStatusItem system tray
- iOS: WKWebView, UIView embedding
- Windows: WebView2, HWND embedding, system tray via Shell_NotifyIcon
- Linux: WebKitGTK, XEmbed protocol

**Strengths:**
- WebBrowserComponent enables HTML/CSS/JS UIs within native apps
- Native view embedding is essential for hosting third-party views (Metal, OpenGL, etc.)
- Code editor is useful for DSP scripting interfaces

**Weaknesses:**
- WebView2 dependency on Windows requires a separate runtime
- WebKitGTK dependency on Linux adds significant build requirements
- Coupling: `juce_audio_processors` depends on this module, meaning headless audio processing pulls in web view support

**Pulp Must-Have Parity:**
- Native view embedding (essential for Metal/Vulkan/WebGPU views)
- Web view component (useful for documentation, settings UIs)

**Modern Redesign Opportunities:**
- Make web view an optional, separately-linked module
- Break native view embedding into the windowing module (it's a core need, not "extra")

---

### 7. juce_opengl

| Attribute | Value |
|-----------|-------|
| **Size** | 34,014 lines / 38 files |
| **Complexity** | Medium |
| **Dependencies** | juce_gui_extra |
| **macOS Frameworks** | OpenGL |

**Purpose:** OpenGL rendering integration for the Component system.

**Key Capabilities:**
- OpenGLContext: Attach an OpenGL context to any Component for GPU-accelerated rendering
- OpenGLRenderer: Callback interface for custom OpenGL drawing
- OpenGLShaderProgram, OpenGLTexture, OpenGLFrameBuffer: OpenGL resource wrappers
- Bundled GLEW (BSD+MIT) for extension loading on Windows/Linux

**Strengths:**
- Allows GPU acceleration of any component's rendering
- Useful for custom visualizations (spectrum analyzers, waveform displays)

**Weaknesses:**
- OpenGL is deprecated on macOS (since 2018) and iOS
- No Metal or Vulkan backend
- High line count relative to capability (34K lines, much of which is GLEW)
- Performance can be worse than software rendering for simple UIs due to context switching overhead

**Pulp Must-Have Parity:**
- GPU-accelerated rendering capability (but via Metal/WebGPU, not OpenGL)

**Modern Redesign Opportunities:**
- Replace entirely with Metal (Apple), Vulkan (Linux/Windows), or WebGPU (cross-platform)
- Consider Skia or similar as a unified GPU rendering backend

---

### 8. juce_audio_basics

| Attribute | Value |
|-----------|-------|
| **Size** | 44,518 lines / 109 files |
| **Complexity** | Large |
| **Dependencies** | juce_core |
| **macOS Frameworks** | Accelerate |

**Purpose:** Fundamental audio and MIDI types, buffer management, SIMD operations, and basic DSP building blocks.

**Key Capabilities:**

*Audio Buffers:*
- AudioBuffer (formerly AudioSampleBuffer): Multi-channel float/double audio buffer with SIMD-optimized operations
- AudioDataConverters: Format conversion between int8/16/24/32, float32, float64

*SIMD Operations:*
- FloatVectorOperations: SIMD-accelerated vector math (add, multiply, copy, find min/max, convert, RMS)
- Backends: vDSP (Apple Accelerate), SSE2/SSE4/AVX (x86), NEON (ARM), scalar fallback

*MIDI:*
- MidiMessage: Single MIDI event with timestamp
- MidiBuffer: Time-stamped collection of MIDI messages
- MidiMessageSequence: Ordered sequence with note-on/off pairing
- MidiFile: Standard MIDI file reader/writer
- MidiKeyboardState: Virtual keyboard state tracker
- MPESynthesiser, MPEValue, MPEZone: MIDI Polyphonic Expression support
- Universal MIDI Packet (UMP): MIDI 2.0 Universal MIDI Packet support

*DSP Primitives:*
- SmoothedValue: Parameter smoothing with linear or multiplicative ramp
- ADSR: Envelope generator
- IIRFilter: Basic biquad IIR filter
- Decibels: dB/gain conversion utilities
- AudioWorkgroup: macOS/iOS audio workgroup join/leave for real-time thread priority

*Audio Source Hierarchy:*
- AudioSource: Base interface for streaming audio
- AudioTransportSource, MixerAudioSource, ResamplingAudioSource, ChannelRemappingAudioSource
- AudioSourcePlayer: Bridges AudioSource to device callbacks

**Strengths:**
- AudioBuffer is well-designed and universally used
- FloatVectorOperations provides meaningful SIMD acceleration with clean API
- Comprehensive MIDI support including MIDI 2.0 (UMP) and MPE
- SmoothedValue is elegant and widely imitated
- AudioWorkgroup support for Apple's real-time priority system

**Weaknesses:**
- AudioBuffer uses heap allocation with no small-buffer optimization
- IIRFilter in this module is the legacy version (better version in juce_dsp)
- MPE implementation is tightly coupled to the synthesiser architecture

**Pulp Must-Have Parity:**
- Multi-channel audio buffer with SIMD operations
- MIDI 1.0 message handling and buffering
- MIDI 2.0 / UMP support
- Parameter smoothing
- ADSR envelope
- vDSP / NEON / SSE acceleration

**Modern Redesign Opportunities:**
- Use `std::span` for buffer views instead of raw pointer + size
- Template audio buffer on sample type (float/double) at compile time
- Consider stack-allocated small buffers for common channel counts
- Separate MIDI into its own module (it's large enough)

---

### 9. juce_audio_devices

| Attribute | Value |
|-----------|-------|
| **Size** | 53,701 lines / 201 files |
| **Complexity** | Very Large |
| **Dependencies** | juce_audio_basics, juce_events |
| **macOS Frameworks** | CoreAudio, CoreMIDI, AudioToolbox |

**Purpose:** Audio and MIDI hardware I/O abstraction across all platforms, including device enumeration, configuration, and real-time streaming.

**Key Capabilities:**

*Audio Device Abstraction:*
- AudioIODevice: Abstract base for a single audio device (open, close, start, stop, getAvailableSampleRates, getAvailableBufferSizes)
- AudioIODeviceType: Factory for enumerating and creating devices of a given type
- AudioDeviceManager: High-level manager that handles device selection, sample rate, buffer size, and provides the audio callback

*Audio Backends:*

| Backend | Platform | Notes |
|---------|----------|-------|
| CoreAudio | macOS/iOS | Primary Apple backend; low-latency, robust |
| WASAPI | Windows | Shared, exclusive, and low-latency modes |
| DirectSound | Windows | Legacy backend |
| ASIO | Windows | Professional audio; bundled ASIO SDK headers (proprietary) |
| ALSA | Linux | Direct hardware access |
| JACK | Linux | Professional audio server |
| Oboe | Android | Bundled Oboe library (Apache 2.0) for AAudio/OpenSL ES |

*MIDI I/O:*

| Backend | Platform | Notes |
|---------|----------|-------|
| CoreMIDI | macOS/iOS | Full MIDI I/O |
| Win32 MIDI | Windows | Legacy MME MIDI |
| WinRT MIDI | Windows | Modern MIDI API |
| Windows MIDI Services | Windows | MIDI 2.0 support |
| ALSA MIDI | Linux | ALSA sequencer |
| Android MIDI | Android | android.media.midi |

**Strengths:**
- Comprehensive backend coverage across all major platforms
- AudioDeviceManager handles the complexity of device selection and configuration
- WASAPI low-latency mode support
- Oboe integration provides modern Android audio

**Weaknesses:**
- ASIO SDK is proprietary (Steinberg license) — creates licensing complications
- Very large module (54K lines) with many platform-specific implementations that are hard to test
- Device enumeration and hot-plugging behavior varies significantly across platforms
- No Bluetooth MIDI handling beyond what platform APIs provide

**Pulp Must-Have Parity:**
- CoreAudio (macOS/iOS)
- WASAPI (Windows)
- ALSA + JACK (Linux)
- Oboe (Android)
- CoreMIDI, Windows MIDI Services, ALSA MIDI
- Device enumeration and selection

**Modern Redesign Opportunities:**
- Drop DirectSound (legacy, superseded by WASAPI)
- Drop ASIO due to licensing issues (WASAPI exclusive/low-latency is equivalent for most use cases; or use a permissively-licensed ASIO wrapper)
- Use platform MIDI 2.0 APIs (Windows MIDI Services, CoreMIDI with UMP) as primary instead of legacy MIDI
- Provide a callback-based API with `std::function` instead of inheritance
- Consider using an existing cross-platform audio I/O library (like miniaudio or RTAudio) for the non-Apple backends

---

### 10. juce_audio_formats

| Attribute | Value |
|-----------|-------|
| **Size** | 70,513 lines / 117 files |
| **Complexity** | Large |
| **Dependencies** | juce_audio_basics |
| **macOS Frameworks** | CoreAudio, CoreMIDI, QuartzCore, AudioToolbox |

**Purpose:** Audio file reading and writing for common audio formats.

**Key Capabilities:**

*Core Architecture:*
- AudioFormat: Abstract base for a file format (WAV, AIFF, etc.)
- AudioFormatReader / AudioFormatWriter: Stream-based reading and writing
- AudioFormatManager: Registry of available formats with automatic format detection
- AudioFormatReaderSource: Bridges file reading to the AudioSource interface
- BufferingAudioReader: Threaded background buffering for streaming playback

*Supported Formats:*

| Format | Read | Write | Implementation |
|--------|------|-------|---------------|
| WAV | Yes | Yes | Built-in |
| AIFF | Yes | Yes | Built-in |
| FLAC | Yes | Yes | Bundled libFLAC (BSD) |
| Ogg Vorbis | Yes | Yes | Bundled libvorbis (BSD) |
| CoreAudio codecs | Yes | Yes | Platform API (macOS/iOS) — AAC, ALAC, MP3, etc. |
| Windows Media | Yes | No | Platform API (Windows) — WMA, MP3, AAC |
| MP3 | Yes | No | Opt-in, patent concerns |
| LAME MP3 | No | Yes | Opt-in, links external LAME library |

*Additional Features:*
- AudioThumbnail: Waveform display cache with multi-resolution support
- LAMEEncoderAudioFormat: Optional MP3 encoding via external LAME library
- Audio CD reading (macOS via AudioToolbox, Windows via Windows API)

**Strengths:**
- WAV and AIFF implementations are robust and handle edge cases well
- Bundled FLAC and Vorbis encoders/decoders avoid external dependencies
- AudioFormatReaderSource + BufferingAudioReader provide good streaming playback patterns
- AudioThumbnail is a useful utility for waveform visualization

**Weaknesses:**
- Large module (70K lines), mostly due to bundled codec source code
- No Opus support (increasingly important for streaming and voice)
- MP3 handling has legacy patent sensitivity warnings despite patents having expired
- No modern container formats (M4A, WebM, MP4)

**Pulp Must-Have Parity:**
- WAV read/write
- FLAC read/write
- Platform codec access (CoreAudio, Media Foundation)
- Streaming audio reader

**Modern Redesign Opportunities:**
- Use system decoders where possible (CoreAudio, Media Foundation) instead of bundling codecs
- Add Opus support
- Separate codec implementations into individually linkable units
- Consider using a library like dr_libs (public domain WAV/FLAC/MP3) for lighter bundled implementations

---

### 11. juce_dsp

| Attribute | Value |
|-----------|-------|
| **Size** | 21,839 lines / 82 files |
| **Complexity** | Large |
| **Dependencies** | juce_audio_formats |
| **macOS Frameworks** | Accelerate |

**Purpose:** Digital signal processing library with SIMD abstractions, processor chain, FFT, convolution, filters, and effects.

**Key Capabilities:**

*SIMD:*
- SIMDRegister: Type-safe SIMD vector wrapper (SSE, AVX, NEON)
- AudioBlock: Non-owning view into audio buffer with SIMD-friendly operations

*Processor Architecture:*
- ProcessorChain: Variadic template chain of processors (compile-time composition)
- ProcessSpec: Sample rate, block size, channel count specification
- ProcessContextReplacing / ProcessContextNonReplacing

*FFT:*
- FFT: Fast Fourier Transform with multiple backends
  - Built-in (Cooley-Tukey)
  - Apple vDSP (via Accelerate)
  - FFTW3 (opt-in external)
  - Intel MKL (opt-in external)
- Convolution: Partitioned convolution engine (uniform and non-uniform)
- Windowing: Hann, Hamming, Blackman, Blackman-Harris, Kaiser, etc.

*Filters:*
- IIR::Filter / IIR::Coefficients: Improved biquad filter with coefficient objects
- FIR::Filter: Finite impulse response filter
- StateVariableTPTFilter: Topology-preserving transform SVF (highpass, lowpass, bandpass, notch, allpass)
- LinkwitzRileyFilter: Crossover filter
- LadderFilter: Transistor ladder filter model (LP12, LP24, HP12, HP24, BP12, BP24)
- BallisticsFilter: Attack/release envelope follower
- FirstOrderTPTFilter: First-order TPT filter

*Effects:*
- Reverb: Freeverb-based reverb
- Chorus, Phaser: Modulation effects
- Compressor, Limiter, NoiseGate: Dynamics processing
- DelayLine: Fractional delay with multiple interpolation methods (none, linear, Lagrange, Thiran)
- Oversampling: 2x/4x/8x/16x oversampling with configurable filter order

*Utilities:*
- Oscillator: Wavetable oscillator with lookup table
- WaveShaper: Waveshaping distortion
- Gain, DryWetMixer, Panner
- LookupTable, LookupTableTransform

**Strengths:**
- ProcessorChain is an elegant compile-time composition pattern
- FFT with multiple backends provides good platform optimization
- Convolution engine supports both uniform and non-uniform partitioning
- Filter implementations are high quality (especially TPT variants)
- Oversampling is well-implemented with configurable stages

**Weaknesses:**
- Dependencies on juce_audio_formats (heavy) when most DSP doesn't need file I/O
- Some effects (Reverb) are simplistic compared to production needs
- No SIMD-optimized filter implementations (filters process sample-by-sample)
- Compressor/Limiter are basic and lack common features (sidechain filter, look-ahead)

**Pulp Must-Have Parity:**
- FFT (with platform-optimized backends)
- Convolution engine
- Filter library (IIR, FIR, SVF/TPT)
- Oversampling
- DelayLine
- SIMD register abstraction

**Modern Redesign Opportunities:**
- Remove dependency on juce_audio_formats (DSP should only depend on audio_basics)
- Add GPU compute for heavy DSP (FFT, convolution) via Metal/WebGPU
- SIMD-vectorize filter implementations
- Use C++20 concepts for the Processor interface instead of duck-typing
- Add more production-quality dynamics (multiband, look-ahead limiter)

---

### 12. juce_audio_processors_headless

| Attribute | Value |
|-----------|-------|
| **Size** | 97,273 lines / 272 files |
| **Complexity** | Very Large |
| **Dependencies** | juce_audio_basics, juce_events |
| **macOS Frameworks** | CoreAudio, CoreMIDI, AudioToolbox |

**Purpose:** The headless (no GUI) plugin abstraction layer, including the plugin base class, parameter system, processor graph, and host-side plugin loading for all formats.

**Key Capabilities:**

*Plugin Base Class:*
- AudioProcessor: The central abstraction that plugins implement. Defines the interface for processing audio, managing parameters, saving/loading state, describing bus layouts, and reporting latency/tail.

*Parameter System:*
- AudioProcessorParameter: Base class for all parameters
- AudioProcessorParameterWithID: Adds string ID for host identification
- RangedAudioParameter: Adds normalized range mapping
- AudioParameterFloat: Float parameter with NormalisableRange
- AudioParameterInt: Integer parameter
- AudioParameterBool: Boolean parameter
- AudioParameterChoice: Enumerated choice parameter
- AudioProcessorParameterGroup: Hierarchical parameter grouping

*Processor Graph:*
- AudioProcessorGraph: DAG of AudioProcessor nodes with audio/MIDI routing
- AudioProcessorGraph::Node: Wrapper for a processor in the graph
- AudioProcessorGraph::AudioGraphIOProcessor: Input/output nodes

*Host-Side Plugin Loading:*
- AudioPluginFormat: Abstract base for loading external plugins
- AudioPluginFormatManager: Registry of available plugin formats
- VST3, AU, LV2 format implementations for scanning and loading plugins
- KnownPluginList, PluginDescription: Plugin database and metadata

*State Management:*
- MemoryBlock-based binary state (getStateInformation/setStateInformation)
- XML-based state alternative

*Bundled SDKs:*
- VST3 SDK (MIT license) — Steinberg's VST3 plugin interface
- LV2 SDK (ISC license) — LV2 plugin interface
- AudioUnitSDK (Apache 2.0) — Apple's Audio Unit SDK

**Strengths:**
- AudioProcessor is a proven, battle-tested abstraction used by thousands of plugins
- Parameter system is comprehensive with good host automation support
- AudioProcessorGraph is powerful for building modular audio applications
- Support for all major plugin formats from a single base class

**Weaknesses:**
- Very large module (97K lines) — second largest after gui_basics
- AudioProcessorGraph is only safe to modify from the message thread when audio is stopped
- Host-side plugin loading bundles significant third-party SDK code
- State serialization is unstructured (raw binary blob or XML)
- Fixed bus layout after initialization (cannot dynamically change)

**Pulp Must-Have Parity:**
- Plugin base class with audio/MIDI processing callbacks
- Parameter system with automation support
- State save/load
- Bus layout configuration
- Host-side plugin scanning and loading (for DAW-like applications)

**Modern Redesign Opportunities:**
- Separate "plugin authoring" from "host-side loading" into different modules
- Use concepts/protocols instead of virtual inheritance for the plugin interface
- Typed, schema-based state serialization instead of raw binary/XML
- Thread-safe audio graph modification
- Normalize parameter representation at the format adapter boundary, not in the core

---

### 13. juce_audio_processors

| Attribute | Value |
|-----------|-------|
| **Size** | 11,427 lines / 35 files |
| **Complexity** | Medium |
| **Dependencies** | juce_gui_extra, juce_audio_processors_headless |

**Purpose:** GUI-dependent plugin infrastructure: editor, parameter-state-GUI binding, and plugin scanning UI.

**Key Capabilities:**
- AudioProcessorEditor: Base class for plugin GUI editors (owned by the processor)
- GenericAudioProcessorEditor: Auto-generated UI for any plugin's parameters
- AudioProcessorValueTreeState (APVTS): The primary mechanism for binding parameters to ValueTree state and GUI components
  - ParameterLayout: Declarative parameter definition
  - SliderAttachment, ButtonAttachment, ComboBoxAttachment: Two-way bindings between parameters and GUI widgets
- KnownPluginList: (Scanning UI aspects)
- PluginDirectoryScanner: Threaded plugin directory scanning

**Strengths:**
- APVTS is the most-used pattern in the framework — it elegantly solves parameter/state/GUI binding
- ParameterAttachments provide automatic two-way sync between parameters and widgets
- GenericAudioProcessorEditor is useful for rapid prototyping

**Weaknesses:**
- APVTS is built on ValueTree, inheriting its overhead (string-interned keys, variant values)
- Depends on juce_gui_extra, which is why all plugin code pulls in the GUI chain
- ParameterAttachments use raw pointer observation (no weak reference safety)

**Pulp Must-Have Parity:**
- Parameter-to-GUI binding system
- Auto-generated parameter UI for prototyping

**Modern Redesign Opportunities:**
- Make this module optional — headless plugins should not link any GUI code
- Use reactive/declarative binding instead of imperative attachment objects
- Replace ValueTree-based state with typed state containers

---

### 14. juce_audio_plugin_client

| Attribute | Value |
|-----------|-------|
| **Size** | 66,331 lines / 244 files |
| **Complexity** | Very Large |
| **Dependencies** | juce_audio_processors |

**Purpose:** Format-specific export wrappers that compile an AudioProcessor into each target plugin format binary.

**Key Capabilities (per-format wrappers):**

| Format | Target | Bundled SDK | SDK License |
|--------|--------|-------------|-------------|
| VST3 | Windows, macOS, Linux | VST3 SDK | MIT |
| AU (v2) | macOS | AudioUnitSDK | Apache 2.0 |
| AUv3 | macOS, iOS | AudioUnitSDK | Apache 2.0 |
| AAX | macOS, Windows | AAX SDK | Proprietary / GPLv3 |
| LV2 | Linux, macOS, Windows | LV2 SDK | ISC |
| VST2 | Windows, macOS | *(deprecated, requires external SDK)* | Proprietary |
| Unity | Windows, macOS | Unity plugin interface | — |
| Standalone | All platforms | StandaloneFilterWindow | — |
| ARA | macOS, Windows | ARA SDK | — |

*Standalone Wrapper:*
- StandaloneFilterWindow: Full windowed application hosting the plugin
- StandalonePluginHolder: Manages audio device, MIDI, and the processor instance

**Strengths:**
- Single codebase compiles to all major plugin formats — the core value proposition
- Mature, well-tested format adapters used by thousands of shipping plugins
- VST3 SDK is MIT-licensed (permissive)
- AudioUnitSDK is Apache 2.0 (permissive)
- LV2 SDK is ISC (permissive)

**Weaknesses:**
- AAX SDK is proprietary (Avid license) — cannot redistribute in open-source projects under permissive terms; only available under GPLv3
- Very large module (66K lines) with per-format implementations that are difficult to maintain
- VST2 is deprecated but still present
- Unity plugin format is niche
- Standalone wrapper is basic compared to dedicated audio applications

**Pulp Must-Have Parity:**
- VST3 wrapper
- AU / AUv3 wrappers
- CLAP wrapper (notably absent from the audited framework — added by community patches)
- Standalone wrapper
- LV2 wrapper

**Modern Redesign Opportunities:**
- Add first-class CLAP support (modern, permissive alternative to VST3)
- Drop AAX due to licensing (or provide as a separate proprietary-compatible module)
- Drop VST2 entirely
- Drop Unity plugin format
- Each format wrapper should be an independently compilable module
- Use C++20 concepts for the plugin-to-format adapter interface

---

### 15. juce_audio_utils

| Attribute | Value |
|-----------|-------|
| **Size** | 11,384 lines / 38 files |
| **Complexity** | Medium |
| **Dependencies** | juce_audio_processors, juce_audio_formats, juce_audio_devices |

**Purpose:** High-level audio utilities that combine the processor, format, and device modules — primarily GUI components for audio applications.

**Key Capabilities:**
- AudioDeviceSelectorComponent: GUI for audio device selection and configuration
- MidiKeyboardComponent: Virtual MIDI keyboard widget
- AudioProcessorPlayer: Connects an AudioProcessor to an AudioDeviceManager
- CDReaderComponent: Audio CD ripping UI
- AudioAppComponent: Base class for simple audio applications (combines device manager + processor)

**Strengths:**
- AudioDeviceSelectorComponent provides a complete device settings UI
- MidiKeyboardComponent is useful for testing and standalone synth plugins
- AudioProcessorPlayer bridges processors to devices cleanly

**Weaknesses:**
- Heavy dependency chain (pulls in all audio and GUI modules)
- CDReaderComponent is obsolete
- Small utility module that could be merged elsewhere

**Pulp Must-Have Parity:**
- Audio device selection UI
- MIDI keyboard widget
- Processor-to-device bridge

**Modern Redesign Opportunities:**
- Split into individual utilities rather than a monolithic module
- Remove obsolete CD reading functionality

---

### 16. juce_midi_ci

| Attribute | Value |
|-----------|-------|
| **Size** | 12,325 lines / 45 files |
| **Complexity** | Medium |
| **Dependencies** | juce_audio_basics |

**Purpose:** Full implementation of MIDI Capability Inquiry (MIDI-CI), the MIDI 2.0 protocol for discovering device capabilities and negotiating protocol versions.

**Key Capabilities:**
- MIDI-CI Device: Acts as initiator or responder in CI conversations
- Profile Inquiry: Discover and enable/disable MIDI profiles on connected devices
- Property Exchange: Get/set device properties via JSON-like messages
- Protocol Negotiation: Negotiate MIDI 1.0 vs MIDI 2.0 protocol
- Process Inquiry: System-exclusive process management

**Strengths:**
- Comprehensive MIDI 2.0 CI implementation
- Forward-looking — MIDI 2.0 adoption is increasing

**Weaknesses:**
- Complex specification that few devices currently implement
- Requires juce_audio_basics but not juce_audio_devices (cannot actually send MIDI without additional code)

**Pulp Must-Have Parity:**
- MIDI-CI support (eventually, not at launch)

**Modern Redesign Opportunities:**
- Combine with a MIDI 2.0 transport layer for end-to-end MIDI 2.0 support

---

### 17. juce_cryptography

| Attribute | Value |
|-----------|-------|
| **Size** | 3,049 lines / 15 files |
| **Complexity** | Small |
| **Dependencies** | juce_core |

**Purpose:** Cryptographic primitives for hashing, encryption, and key generation.

**Key Capabilities:**
- MD5, SHA256: Hash functions
- RSAKey: RSA public/private key generation and encryption
- BlowfishEncryption: Blowfish symmetric encryption
- Primes: Prime number generation for RSA

**Strengths:**
- Self-contained, no external crypto dependencies

**Weaknesses:**
- MD5 and Blowfish are cryptographically outdated
- No modern algorithms (AES, ChaCha20, Ed25519, Argon2)
- Custom implementation rather than using platform crypto (CommonCrypto, BCrypt, OpenSSL)

**Pulp Must-Have Parity:**
- SHA256 hashing (for state checksums)
- Ed25519 (for update signing) — use platform or standard library

**Modern Redesign Opportunities:**
- Use platform cryptography APIs instead of custom implementations
- Add Ed25519 for auto-update signing
- Remove outdated algorithms

---

### 18. juce_product_unlocking

| Attribute | Value |
|-----------|-------|
| **Size** | 4,152 lines / 14 files |
| **Complexity** | Small |
| **Dependencies** | juce_cryptography |

**Purpose:** Software licensing and product registration system.

**Key Capabilities:**
- OnlineUnlockStatus: Base class for managing license state
- OnlineUnlockForm: GUI for entering registration details
- KeyGeneration: RSA-based license key generation
- MachineIDUtilities: Hardware fingerprinting for machine-locked licenses

**Strengths:**
- Provides a complete licensing framework out of the box

**Weaknesses:**
- Tightly coupled to RSA-based licensing (no modern alternatives)
- Hardware fingerprinting is fragile and privacy-concerning
- Encourages a specific licensing model that may not suit all products

**Pulp Must-Have Parity:**
- None (licensing is a business concern, not a framework concern)

**Modern Redesign Opportunities:**
- Remove entirely — licensing should be a separate, optional library
- If included, support subscription validation, hardware-agnostic approaches

---

### 19. juce_javascript

| Attribute | Value |
|-----------|-------|
| **Size** | 72,977 lines / 21 files |
| **Complexity** | Large (mostly bundled engine) |
| **Dependencies** | juce_core |

**Purpose:** Embedded JavaScript engine for scripting.

**Key Capabilities:**
- JavascriptEngine: Evaluate JavaScript code with custom native bindings
- Bundled QuickJS engine (MIT licensed, via CHOC library)
- Can expose native C++ functions to JavaScript
- JSON interop through the Var type

**Strengths:**
- Enables scripting and dynamic behavior without recompilation
- QuickJS is small and fast for an embedded engine

**Weaknesses:**
- 73K lines, most of which is the bundled QuickJS engine
- Limited use cases in typical audio plugins
- No debugging or profiling tools

**Pulp Must-Have Parity:**
- None (scripting is optional/specialized)

**Modern Redesign Opportunities:**
- Offer as a completely separate, optional package
- Consider offering both QuickJS (small, embedded) and V8/JSC (full-featured) options

---

### 20. juce_osc

| Attribute | Value |
|-----------|-------|
| **Size** | 5,268 lines / 18 files |
| **Complexity** | Small |
| **Dependencies** | juce_events |

**Purpose:** Open Sound Control (OSC) protocol implementation for network-based music control.

**Key Capabilities:**
- OSCSender: Send OSC messages and bundles over UDP
- OSCReceiver: Receive and parse OSC messages
- OSCMessage, OSCBundle: Message types with typed arguments
- OSCAddress, OSCAddressPattern: OSC address matching

**Strengths:**
- Clean implementation of the OSC 1.0 specification
- Useful for controller integration and inter-application communication

**Weaknesses:**
- UDP only (no TCP, no OSC over WebSocket)
- No OSC query protocol support

**Pulp Must-Have Parity:**
- OSC support (eventually, not at launch)

**Modern Redesign Opportunities:**
- Add OSC over WebSocket for browser integration
- Add OSC query protocol

---

### 21. juce_analytics

| Attribute | Value |
|-----------|-------|
| **Size** | 1,115 lines / 9 files |
| **Complexity** | Small |
| **Dependencies** | juce_gui_basics |

**Purpose:** Usage analytics framework for tracking user interactions.

**Key Capabilities:**
- Analytics: Singleton for logging analytics events
- AnalyticsDestination: Abstract base for sending events to a backend
- ThreadedAnalyticsDestination: Batched, background sending

**Strengths:**
- Clean abstraction for analytics backends

**Weaknesses:**
- Depends on juce_gui_basics unnecessarily (analytics events don't require GUI)
- Minimal functionality — most products need a real analytics SDK

**Pulp Must-Have Parity:**
- None

**Modern Redesign Opportunities:**
- Remove or make standalone (no GUI dependency)

---

### 22. juce_animation

| Attribute | Value |
|-----------|-------|
| **Size** | 2,472 lines / 16 files |
| **Complexity** | Small |
| **Dependencies** | juce_gui_basics |

**Purpose:** Animation system for Component properties.

**Key Capabilities:**
- Animator: Manages property animations on components
- VBlankAnimationTarget: Synchronize animations to display refresh
- AnimatedPosition: Spring-physics-based position animation

**Strengths:**
- VBlank synchronization is the correct approach for smooth animation
- Spring physics provides natural-feeling motion

**Weaknesses:**
- Limited scope — only property animation, no keyframe or complex timeline support
- New module (added in JUCE 7/8), less battle-tested

**Pulp Must-Have Parity:**
- Animation system (integrated into whichever UI approach is chosen)

**Modern Redesign Opportunities:**
- If using SwiftUI, leverage its built-in animation system instead
- If custom UI, build a more complete animation system (keyframes, easing curves, stagger)

---

### 23. juce_box2d

| Attribute | Value |
|-----------|-------|
| **Size** | 20,797 lines / 95 files |
| **Complexity** | Medium |
| **Dependencies** | juce_graphics |

**Purpose:** Bundled Box2D 2D physics engine.

**Key Capabilities:**
- Full Box2D physics simulation (rigid bodies, joints, collision detection)
- Integration with the framework's graphics coordinate system

**Strengths:**
- Complete physics engine if needed

**Weaknesses:**
- **Zero audio relevance** — a physics library has no business in an audio framework
- 21K lines of code that adds to build times
- Depends on juce_graphics unnecessarily

**Pulp Must-Have Parity:**
- None. Do not include.

**Modern Redesign Opportunities:**
- Remove entirely. If users need physics, they can add Box2D themselves.

---

### 24. juce_video

| Attribute | Value |
|-----------|-------|
| **Size** | 10,622 lines / 14 files |
| **Complexity** | Medium |
| **Dependencies** | juce_gui_extra |
| **macOS Frameworks** | AVKit, AVFoundation, CoreMedia |

**Purpose:** Video playback component.

**Key Capabilities:**
- VideoComponent: Embedded video player using platform APIs
- macOS/iOS: AVPlayer-based
- Windows: DirectShow or Media Foundation
- Camera capture (CameraDevice)

**Strengths:**
- Uses platform-native video playback

**Weaknesses:**
- Marginal relevance to audio plugin development
- Camera support is minimal

**Pulp Must-Have Parity:**
- None at launch

**Modern Redesign Opportunities:**
- Remove or make a fully separate optional package

---

## Pain Points and Legacy Concerns

The following issues represent significant architectural problems that Pulp should explicitly avoid or redesign.

### 1. Monolithic LookAndFeel

The theming system uses a single class that inherits 20+ widget style interfaces through a deep inheritance chain (V1 -> V2 -> V3 -> V4). Every widget delegates its drawing to virtual methods on this one class.

**Problems:**
- Third-party themes must subclass V4 and override potentially hundreds of methods
- Adding a new widget type requires modifying the LookAndFeel base class
- No composition — you cannot mix-and-match themes for different widget types
- Brittle: changes to the base class break all custom themes
- The V1/V2/V3/V4 version chain is pure legacy cruft

**Pulp Design Direction:** Use protocol/trait-based theming where each widget type defines its own style protocol. Themes implement only the protocols they want to customize.

---

### 2. Manual Component Ownership

The Component class uses raw pointer parent-child relationships. `addAndMakeVisible(child)` does NOT transfer ownership. The caller must ensure children outlive their parent.

**Problems:**
- Destructor order bugs are a top source of crashes in framework-based applications
- No smart pointer integration — `std::unique_ptr` members must be manually managed
- Removing a child does not delete it
- OwnedArray-based child management is possible but not enforced

**Pulp Design Direction:** Use `std::unique_ptr` ownership for child components, or (on Apple) leverage SwiftUI's value-type view hierarchy.

---

### 3. MessageManager Singleton Coupling

All GUI operations must go through a single global MessageManager instance. It must be created before any Component and destroyed after all Components.

**Problems:**
- Unit testing GUI code requires initializing the MessageManager singleton
- Cannot run parallel GUI tests
- Cannot mock the event dispatch system
- Static initialization order problems
- Deadlock risks with MessageManagerLock

**Pulp Design Direction:** Dependency-inject the event loop. Allow test code to provide a mock or synchronous event dispatcher.

---

### 4. Custom String Class

The framework's String class is a custom UTF-8 string implementation with reference-counted storage, predating good standard library Unicode support.

**Problems:**
- Constant conversion friction when interacting with `std::string`, `const char*`, platform strings
- Different semantics from `std::string` (e.g., comparison behavior)
- AI coding assistants frequently make mistakes with the custom API vs standard string API
- Performance characteristics differ from `std::string` (reference counting overhead)

**Pulp Design Direction:** Use `std::string` with explicit encoding utilities. Use `std::string_view` for non-owning references. Use platform string types (NSString, BSTR) at the platform boundary only.

---

### 5. AudioProcessorGraph Thread Safety

The audio processor graph can only be safely modified from the message thread when audio processing is stopped.

**Problems:**
- Cannot dynamically add/remove nodes during playback without stopping audio
- No lock-free graph modification mechanism
- Limits use in live performance and modular synthesis contexts

**Pulp Design Direction:** Implement a lock-free audio graph with double-buffered or RCU-based topology updates.

---

### 6. Unity Build Pattern

Each module has a single `.cpp` file that `#include`s all implementation files. This is essentially a unity build.

**Problems:**
- Poor incremental compilation — changing one file recompiles the entire module
- Long initial compilation times
- Internal linkage conflicts between implementation files
- Makes it difficult to compile individual translation units

**Pulp Design Direction:** Standard per-file compilation units. Use CMake for proper dependency tracking.

---

### 7. ValueTree Overhead

ValueTree is an XML-inspired tree data structure with string-interned keys and dynamic Var values. It's used extensively for plugin state via APVTS.

**Problems:**
- String key lookups for property access (not compile-time typed)
- Dynamic Var values have boxing/unboxing overhead
- XML serialization for state is verbose
- Listener callback overhead on every property change
- No schema validation

**Pulp Design Direction:** Use compile-time typed state containers. Consider a code-generated state system or Swift Codable-style serialization.

---

### 8. Dual Projucer/CMake Build Paths

The framework historically used a proprietary project generator (Projucer). CMake was added later and is now recommended, but Projucer still ships.

**Problems:**
- Documentation confusion — some tutorials use Projucer, some use CMake
- Two separate configuration systems
- Projucer-generated projects and CMake projects have different behaviors
- New users don't know which to choose

**Pulp Design Direction:** CMake only. No proprietary project generator. SPM for Apple-specific Swift code.

---

### 9. AAX SDK Licensing

The AAX SDK (Avid's plugin format) is bundled with the framework under a proprietary license that restricts redistribution. It's also available under GPLv3.

**Problems:**
- Cannot ship AAX support in a permissively-licensed open-source project
- Must accept Avid's terms to develop AAX plugins
- Creates a licensing incompatibility for Pulp's permissive license goals

**Pulp Design Direction:** Do not bundle AAX SDK. If AAX support is needed, provide it as a separately-licensed module or let developers provide their own SDK.

---

### 10. Box2D Module

A 2D physics engine is bundled as a framework module.

**Problems:**
- Zero relevance to audio development
- Adds 21K lines of code and build time
- Depends on the graphics module unnecessarily

**Pulp Design Direction:** Do not include non-audio libraries. If physics is needed, users add it themselves.

---

### 11. Graphics Renderer Fragmentation

The framework provides four different rendering backends: software rasterizer, CoreGraphics (macOS), Direct2D (Windows), and OpenGL.

**Problems:**
- Each renderer has different performance characteristics
- Visual output can differ between renderers
- OpenGL is deprecated on Apple platforms
- Linux only has the software renderer (no GPU acceleration)
- No Metal rendering despite Apple deprecating OpenGL years ago

**Pulp Design Direction:** Use a single cross-platform GPU renderer (Metal on Apple, Vulkan/D3D12 on others, or WebGPU everywhere). Or use platform-native rendering (SwiftUI on Apple, etc.).

---

### 12. No Metal Rendering

Despite Metal being Apple's primary graphics API since 2014 (and OpenGL being deprecated since 2018), the framework does not use Metal for its 2D rendering.

**Problems:**
- Missing modern GPU features on Apple platforms
- OpenGL compatibility layer adds overhead on macOS
- Cannot take advantage of Metal-specific optimizations (tile-based rendering, etc.)

**Pulp Design Direction:** Use Metal on Apple platforms. Consider MetalKit for custom rendering and SwiftUI (which uses Metal internally) for widget-based UIs.

---

## Licensing

### Framework License

The audited framework uses **dual licensing**:

1. **AGPLv3** — Free for open-source projects that comply with AGPL terms
2. **Commercial License** — Paid subscription for closed-source commercial products
   - Personal: applicable below revenue thresholds
   - Professional: required above revenue thresholds
   - Enterprise: custom terms for large organizations

The commercial license is required for any closed-source product that uses the framework, regardless of whether it's sold or distributed freely.

### Bundled Dependency Licenses

| Dependency | License | Module | Notes |
|------------|---------|--------|-------|
| AudioUnitSDK | Apache 2.0 | audio_plugin_client | Apple's AU SDK |
| Oboe | Apache 2.0 | audio_devices | Google's Android audio library |
| SheenBidi | Apache 2.0 | graphics | Unicode bidirectional algorithm |
| FLAC | BSD 3-Clause | audio_formats | Lossless audio codec |
| Ogg Vorbis | BSD 3-Clause | audio_formats | Lossy audio codec |
| GLEW | BSD + MIT | opengl | OpenGL extension loader |
| jpeglib | IJG License | graphics | JPEG image codec |
| pnglib | zlib License | graphics | PNG image codec |
| zlib | zlib License | core | Compression library |
| Box2D | zlib License | box2d | Physics engine |
| HarfBuzz | Old MIT (permissive) | graphics | Text shaping engine |
| CHOC / QuickJS | ISC / MIT | javascript | JavaScript engine and utilities |
| LV2 SDK | ISC License | audio_processors_headless | LV2 plugin interface |
| VST3 SDK | MIT License | audio_processors_headless | VST3 plugin interface |
| AAX SDK | **Proprietary / GPLv3** | audio_plugin_client | **Restrictive** — Avid proprietary terms |
| ASIO SDK | **Proprietary / GPLv3** | audio_devices | **Restrictive** — Steinberg proprietary terms |

### Licensing Implications for Pulp

- All bundled dependencies with permissive licenses (Apache, BSD, MIT, ISC, zlib) are compatible with Pulp's permissive licensing goals
- **AAX SDK** and **ASIO SDK** are incompatible with permissive open-source licensing and must be handled separately (not bundled, or provided as optional proprietary-licensed modules)
- The framework's AGPL/Commercial dual license means Pulp **cannot use any of its code** — only study its architecture

---

## Test Infrastructure

### Test Framework

The audited framework includes a custom unit test framework rather than using an established testing library (Google Test, Catch2, doctest).

**Key Characteristics:**
- Custom `UnitTest` base class with `expect()`, `expectEquals()`, `expectWithinAbsoluteError()`, etc.
- `UnitTestRunner` application in extras/ runs all registered tests
- Tests require the `JUCE_UNIT_TESTS=1` compile-time flag to be included
- Tests are embedded within module source files (not in a separate test directory)
- Approximately 32 test files with ~108 test classes

### Coverage and Gaps

| Area | Test Coverage |
|------|--------------|
| Core types (String, Array, etc.) | Good |
| Audio buffers and MIDI | Good |
| DSP (FFT, filters) | Moderate |
| ValueTree and serialization | Good |
| GUI components | **None** |
| Plugin format wrappers | **None** |
| Visual regression | **None** |
| Integration tests | **None** |
| Platform-specific code | **Minimal** |

### CI/CD

- No CI configuration in the repository tree
- No automated test runs
- Tests must be manually built and run via the UnitTestRunner application
- No visual regression testing
- No fuzzing or property-based testing

### Implications for Pulp

- Pulp should use an established test framework (Catch2 or Google Test)
- Tests should be in a separate `tests/` directory, not embedded in source
- Tests should run in CI (GitHub Actions) on all target platforms
- GUI testing strategy needed: screenshot comparison, accessibility tree validation
- Plugin format validation via external tools (pluginval, auval) should be automated
- Consider property-based testing for DSP code

---

## Summary: Implications for Pulp

### What to Carry Forward

1. **AudioBuffer pattern**: Multi-channel audio buffer with SIMD operations is well-designed
2. **SmoothedValue pattern**: Elegant parameter smoothing
3. **ProcessorChain pattern**: Compile-time DSP processor composition
4. **AbstractFifo pattern**: Lock-free FIFO for audio thread communication
5. **Parameter system concept**: Float/Int/Bool/Choice parameter types with normalized range mapping
6. **Format adapter architecture**: Write-once plugin, compile to multiple formats
7. **FFT with multiple backends**: Platform-optimized FFT (vDSP, etc.)
8. **Accessibility support**: Cross-platform accessibility is essential and well-implemented

### What to Reject

1. Custom String class — use `std::string`
2. Custom containers — use STL
3. Singleton event dispatch — use dependency injection
4. Monolithic LookAndFeel — use protocol-based theming
5. Manual Component ownership — use smart pointers or SwiftUI
6. GUI-dependent audio processors — keep audio and GUI fully separate
7. Unity build pattern — use standard compilation units
8. Box2D inclusion — no non-audio libraries
9. Proprietary SDK bundling — permissive licenses only
10. Custom test framework — use Catch2 or Google Test
11. Projucer — CMake only (plus SPM for Apple Swift)

### What to Redesign

1. **State management**: Replace ValueTree/APVTS with typed, schema-based state
2. **Rendering**: Replace multi-backend fragmentation with unified GPU rendering (Metal/WebGPU/Vulkan)
3. **Plugin/GUI coupling**: Headless plugin core with optional, separately-linked GUI module
4. **Thread-safe graph**: Lock-free audio graph modification
5. **Module structure**: Finer-grained modules with minimal transitive dependencies
6. **Build system**: CMake + SPM, no proprietary tools
7. **Testing**: CI-integrated, multi-platform, with visual regression and format validation
