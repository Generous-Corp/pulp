# Clean-Room Capability Matrix

This matrix catalogs every capability observed in the audited framework, maps each to a generic requirement, and assigns it to a Pulp subsystem with an original design. The three-layer separation (observed capability, inferred requirement, proposed design) is maintained throughout.

**Column definitions**:
- **Domain**: Functional area
- **Observed Capability**: What the audited framework provides (Layer 1)
- **Why It Matters**: Business/technical justification
- **Required for Pulp v1**: Whether this must ship in Pulp's initial release
- **Required for Long-Term**: Whether this is needed eventually
- **Proposed Pulp Subsystem**: Original Pulp module name (Layer 3)
- **Clean-Room Risk**: HIGH / MEDIUM / LOW — likelihood of accidental contamination
- **Notes**: Design guidance, alternatives, or caveats

---

## Application Framework

| Domain | Observed Capability | Why It Matters | Required for Pulp v1 | Required for Long-Term | Proposed Pulp Subsystem | Clean-Room Risk | Notes |
|---|---|---|---|---|---|---|---|
| Application Framework | Event loop / message dispatch via a singleton manager | All GUI apps need a main-thread event loop for UI updates and cross-thread communication | Yes | Yes | **pulp-events** | HIGH | Pulp uses per-thread `EventLoop` instances wrapping native run loops; no global singleton |
| Application Framework | Application lifecycle management (launch, quit, focus, activation) | Apps must respond to OS lifecycle events cleanly | Yes | Yes | **pulp-events** | MEDIUM | Pulp defines an `AppDelegate` protocol; platform backends translate native events |
| Application Framework | Preferences / settings persistence | Users expect apps to remember settings across sessions | No | Yes | **pulp-runtime** | LOW | Use platform-native stores (NSUserDefaults, Registry, GSettings) or a JSON file |
| Application Framework | Document model (open, save, revert, recent files) | DAWs and editors need document-based workflows | No | Yes | **pulp-runtime** | MEDIUM | Pulp provides a `Document` protocol, not a base class hierarchy |
| Application Framework | Command / keyboard shortcut management | Pro audio apps rely heavily on keyboard shortcuts | No | Yes | **pulp-events** | MEDIUM | Pulp uses a `CommandRegistry` with declarative shortcut bindings |

## Windowing

| Domain | Observed Capability | Why It Matters | Required for Pulp v1 | Required for Long-Term | Proposed Pulp Subsystem | Clean-Room Risk | Notes |
|---|---|---|---|---|---|---|---|
| Windowing | Native window creation and management across platforms | Plugins and standalone apps need native OS windows | Yes | Yes | **pulp-view** | HIGH | Pulp uses `Surface` as the native window abstraction, managed by a `Compositor` |
| Windowing | Window types: normal, dialog, popup, tooltip | Different UI contexts require different window behaviors | Yes | Yes | **pulp-view** | MEDIUM | `Surface::Kind` enum with platform-appropriate native window styles |
| Windowing | Multi-monitor support and screen enumeration | Pro users often use multi-monitor setups | No | Yes | **pulp-platform** | LOW | Wrap platform APIs (NSScreen, EnumDisplayMonitors, Xrandr) |
| Windowing | Full-screen mode | Standalone apps may need full-screen presentation | No | Yes | **pulp-view** | LOW | Standard platform API usage |
| Windowing | DPI / Retina scaling with automatic high-DPI rendering | Modern displays require scale-aware rendering | Yes | Yes | **pulp-canvas** | MEDIUM | Pulp's `Canvas` operates in logical coordinates; `Surface` provides the scale factor |

## Graphics

| Domain | Observed Capability | Why It Matters | Required for Pulp v1 | Required for Long-Term | Proposed Pulp Subsystem | Clean-Room Risk | Notes |
|---|---|---|---|---|---|---|---|
| Graphics | 2D vector rendering: paths, shapes, affine transforms | All custom UIs need 2D drawing primitives | Yes | Yes | **pulp-canvas** | MEDIUM | `Canvas` API wraps CoreGraphics, Direct2D, Cairo/Skia per platform |
| Graphics | Image loading and rendering (PNG, JPEG, at minimum) | UIs need icons, backgrounds, and bitmap assets | Yes | Yes | **pulp-canvas** | LOW | Use stb_image or platform decoders; `Bitmap` type for pixel data |
| Graphics | Color management (color spaces, linear/sRGB) | Correct color reproduction across displays | No | Yes | **pulp-canvas** | LOW | Generic color science; no framework-specific patterns |
| Graphics | Font rendering and text layout (Unicode, BiDi, shaping) | Audio apps need readable, internationalized text | Yes | Yes | **pulp-canvas** | MEDIUM | Use HarfBuzz for shaping, FreeType/CoreText/DirectWrite for rasterization |
| Graphics | GPU-accelerated rendering (OpenGL, Metal, Direct2D, Vulkan) | Smooth UI performance, especially for meters and visualizations | No | Yes | **pulp-gpu** | MEDIUM | Optional GPU path via bgfx or native APIs; `pulp-canvas` can target CPU or GPU backends |

## UI / Widget System

| Domain | Observed Capability | Why It Matters | Required for Pulp v1 | Required for Long-Term | Proposed Pulp Subsystem | Clean-Room Risk | Notes |
|---|---|---|---|---|---|---|---|
| UI / Widget System | Hierarchical UI element tree with parent-child relationships | Fundamental to any retained-mode UI framework | Yes | Yes | **pulp-view** | HIGH | Pulp uses `Widget` as base element; tree managed by ownership, not a custom ref-counting component model |
| UI / Widget System | Event propagation: mouse, keyboard, focus traversal | Interactive UIs need input event routing | Yes | Yes | **pulp-view** | HIGH | Pulp uses a `Dispatcher` that routes `InputEvent` variants through the widget tree; hit-testing is method-based |
| UI / Widget System | Standard widgets: sliders, buttons, toggles, text fields, combo boxes | Audio plugins need common UI controls | Yes | Yes | **pulp-view** | HIGH | All widget names and APIs are original: `Knob`, `Fader`, `Toggle`, `TextInput`, `Picker`, etc. |
| UI / Widget System | Layout system (absolute positioning, flex-like, grid-like) | Complex UIs need flexible layout | Yes | Yes | **pulp-view** | MEDIUM | Pulp provides `LayoutEngine` with `Flex`, `Grid`, and `Fixed` strategies; inspired by CSS Flexbox/Grid specs, not the audited framework |
| UI / Widget System | Styling / theming system | Users and developers want customizable appearance | Yes | Yes | **pulp-view** | HIGH | Token-based `ThemeContext` with cascading properties; no monolithic style class |
| UI / Widget System | Accessibility: screen readers, keyboard navigation | Legal requirement in many jurisdictions; ethical imperative | No | Yes | **pulp-view** | MEDIUM | Platform accessibility APIs (NSAccessibility, UIA, ATK); Pulp's `AccessibleNode` protocol |
| UI / Widget System | Drag and drop (internal and OS-level) | Useful for modular routing, preset management | No | Yes | **pulp-view** | LOW | Wrap platform drag-and-drop APIs |
| UI / Widget System | Menus: popup context menus, application menu bar | Standard UI pattern for all desktop apps | Yes | Yes | **pulp-view** | MEDIUM | `Menu` and `MenuEntry` types; platform-native menu bar on macOS |
| UI / Widget System | Animation system (property interpolation, easing) | Modern UIs expect smooth transitions | No | Yes | **pulp-view** | MEDIUM | `Animator` with `Tween` objects; not tied to the audited framework's repaint-based approach |

## Audio I/O

| Domain | Observed Capability | Why It Matters | Required for Pulp v1 | Required for Long-Term | Proposed Pulp Subsystem | Clean-Room Risk | Notes |
|---|---|---|---|---|---|---|---|
| Audio I/O | Hardware audio device enumeration | Apps must discover available audio hardware | Yes | Yes | **pulp-audio** | MEDIUM | `DeviceRegistry` enumerates via platform APIs; original naming |
| Audio I/O | Device open/configure (sample rate, buffer size, channels) | Must configure hardware for desired audio quality | Yes | Yes | **pulp-audio** | MEDIUM | `StreamConfig` struct passed to `DeviceRegistry::openStream()` |
| Audio I/O | Real-time audio callback (process block of samples) | Core of any audio application | Yes | Yes | **pulp-audio** | MEDIUM | `StreamCallback` protocol with `process(input: SampleBlock, output: &mut SampleBlock, time: TimeStamp)` |
| Audio I/O | Multi-channel support (surround, Atmos, Ambisonics) | Pro audio often exceeds stereo | No | Yes | **pulp-audio** | LOW | Channel layouts defined as `ChannelArrangement` enum; standard industry formats |
| Audio I/O | Device hot-plug notification | Users connect/disconnect interfaces during sessions | No | Yes | **pulp-audio** | LOW | Platform callbacks (CoreAudio property listeners, Windows device notifications) |
| Audio I/O | Platform backends: CoreAudio, WASAPI, ALSA, JACK, Oboe | Cross-platform audio requires multiple backends | Yes | Yes | **pulp-platform** | LOW | Each backend implements the `AudioBackend` protocol; Oboe (Apache 2.0) for Android |
| Audio I/O | ASIO support for Windows pro audio | Many pro audio interfaces on Windows require ASIO | No | Yes | **pulp-audio** | LOW | Requires separate ASIO SDK license from Steinberg, or use PortAudio as intermediary |

## MIDI I/O

| Domain | Observed Capability | Why It Matters | Required for Pulp v1 | Required for Long-Term | Proposed Pulp Subsystem | Clean-Room Risk | Notes |
|---|---|---|---|---|---|---|---|
| MIDI I/O | MIDI device enumeration and I/O | Plugins and apps must communicate with MIDI hardware | Yes | Yes | **pulp-midi** | MEDIUM | `MidiPort` abstraction over CoreMIDI, WinMM/WinRT MIDI, ALSA |
| MIDI I/O | MIDI 1.0 message parsing and generation | Standard protocol for musical control | Yes | Yes | **pulp-midi** | MEDIUM | `MidiPacket` value type designed from MIDI 1.0 spec; original accessors |
| MIDI I/O | MIDI 2.0 UMP (Universal MIDI Packet) support | Emerging standard with higher resolution and richer semantics | No | Yes | **pulp-midi** | LOW | Implement from MIDI 2.0 specification directly |
| MIDI I/O | MIDI-CI protocol negotiation | Required for full MIDI 2.0 compliance | No | Yes | **pulp-midi** | LOW | Implement from MIDI-CI specification |
| MIDI I/O | MPE (MIDI Polyphonic Expression) | Growing adoption for expressive controllers (Sensel, Roli, Linnstrument) | No | Yes | **pulp-midi** | LOW | MPE is a specification-defined extension of MIDI 1.0 |
| MIDI I/O | Bluetooth MIDI | Wireless MIDI controllers are increasingly common | No | Yes | **pulp-midi** | LOW | Platform BLE APIs; standard MIDI-over-BLE profile |

## DSP Utilities

| Domain | Observed Capability | Why It Matters | Required for Pulp v1 | Required for Long-Term | Proposed Pulp Subsystem | Clean-Room Risk | Notes |
|---|---|---|---|---|---|---|---|
| DSP Utilities | FFT: forward/inverse, multiple size options | Spectral analysis, convolution, visualization | Yes | Yes | **pulp-signal** | LOW | Wrap vDSP (macOS/iOS), PFFFT, or KissFFT; mathematical operation |
| DSP Utilities | Convolution: partitioned, real-time capable | Reverb impulse responses, cabinet simulation | No | Yes | **pulp-signal** | MEDIUM | Implement from published algorithms (Gardner 1995, partitioned overlap-save) |
| DSP Utilities | Filters: IIR biquad, SVF, TPT, Linkwitz-Riley, ladder | Essential building blocks for EQ, crossovers, synthesis | Yes | Yes | **pulp-signal** | MEDIUM | Implement from Audio EQ Cookbook (Bristow-Johnson) and published papers; `Filter` protocol with `BiquadFilter`, `StateVariableFilter`, etc. |
| DSP Utilities | Oversampling (2x/4x/8x with anti-aliasing) | Reduces aliasing in nonlinear processing | No | Yes | **pulp-signal** | MEDIUM | `Oversampler` class using standard polyphase FIR design |
| DSP Utilities | SIMD abstraction (SSE/AVX/NEON) | Performance-critical DSP benefits from vectorization | No | Yes | **pulp-signal** | MEDIUM | Pulp's `SimdVec` type; design from intrinsics documentation, not the audited framework's wrapper |
| DSP Utilities | Delay lines with interpolation | Chorus, flanger, comb filters, echo | No | Yes | **pulp-signal** | LOW | Standard circular buffer with linear/cubic interpolation |
| DSP Utilities | Oscillators (sine, saw, square, wavetable) | Synthesis building blocks | No | Yes | **pulp-signal** | LOW | Standard DSP; polyBLEP, wavetable, etc. from published literature |
| DSP Utilities | Waveshapers and saturation | Distortion, tape emulation, soft clipping | No | Yes | **pulp-signal** | LOW | Mathematical functions; no framework-specific design |
| DSP Utilities | Audio block processing model (process context with I/O buffers) | Structured approach to per-block DSP | Yes | Yes | **pulp-signal** | MEDIUM | `Processor` protocol with `process(context: &mut ProcessContext)`; original API shape |
| DSP Utilities | Smooth parameter ramping (value smoothing over time) | Avoids zipper noise when parameters change | Yes | Yes | **pulp-signal** | MEDIUM | `Ramp<T>` utility; exponential or linear smoothing. Generic concept but audited framework's specific API should be avoided |
| DSP Utilities | Compressor, limiter, gate | Common dynamics processing | No | Yes | **pulp-signal** | LOW | Implement from Giannoulis et al. (2012) and standard literature |
| DSP Utilities | Reverb algorithms | Spatial effects are fundamental | No | Yes | **pulp-signal** | LOW | FDN, Schroeder, Dattorro algorithms from published papers |

## Plugin Format Wrappers

| Domain | Observed Capability | Why It Matters | Required for Pulp v1 | Required for Long-Term | Proposed Pulp Subsystem | Clean-Room Risk | Notes |
|---|---|---|---|---|---|---|---|
| Plugin Format Wrappers | VST3 plugin export | Industry-standard plugin format | Yes | Yes | **pulp-format** | HIGH | Implement from VST3 SDK (MIT) documentation only; no reference to audited framework's adapter |
| Plugin Format Wrappers | AU / AUv3 plugin export | Required for macOS/iOS distribution | Yes | Yes | **pulp-format** | HIGH | Implement from Apple AU SDK (Apache 2.0) and Apple documentation only |
| Plugin Format Wrappers | AAX plugin export | Required for Pro Tools compatibility | No | Yes | **pulp-format** | HIGH | Implement from AAX SDK (proprietary, obtained from Avid) only |
| Plugin Format Wrappers | LV2 plugin export | Required for Linux DAW compatibility | No | Yes | **pulp-format** | MEDIUM | Implement from LV2 specification (ISC license) |
| Plugin Format Wrappers | CLAP plugin export | Modern, permissively-licensed format gaining traction | Yes | Yes | **pulp-format** | LOW | Implement from CLAP headers (MIT); clean API, no legacy baggage |
| Plugin Format Wrappers | Standalone app wrapper | Every plugin should also run standalone for testing | Yes | Yes | **pulp-format** | MEDIUM | `StandaloneHost` wraps a `Patch` with audio I/O from `pulp-audio` |
| Plugin Format Wrappers | VST2 plugin export (legacy) | Some hosts still only support VST2 | No | No | **pulp-format** | MEDIUM | VST2 SDK is no longer distributed by Steinberg; only possible if developer has legacy license |
| Plugin Format Wrappers | Unity native audio plugin | Game audio is a growing market | No | Yes | **pulp-format** | LOW | Unity's native audio plugin SDK is freely available |
| Plugin Format Wrappers | ARA (Audio Random Access) support | Enables deep DAW integration (Melodyne-style) | No | Yes | **pulp-format** | MEDIUM | Implement from Celemony's ARA SDK |

## Plugin Host Support

| Domain | Observed Capability | Why It Matters | Required for Pulp v1 | Required for Long-Term | Proposed Pulp Subsystem | Clean-Room Risk | Notes |
|---|---|---|---|---|---|---|---|
| Plugin Host Support | Plugin format scanning and loading (VST3, AU, etc.) | DAWs and hosts must discover installed plugins | No | Yes | **pulp-host** | HIGH | `Scanner` discovers plugins via platform-standard search paths; format-specific `Loader` implementations |
| Plugin Host Support | Plugin instance management (create, configure, destroy) | Hosts must manage plugin lifecycles | No | Yes | **pulp-host** | HIGH | `PluginSlot` manages a single loaded plugin instance |
| Plugin Host Support | Audio/MIDI routing graph | Hosts need flexible signal routing | No | Yes | **pulp-host** | MEDIUM | `SignalGraph` with `Node` and `Wire` abstractions; directed acyclic graph processing |
| Plugin Host Support | Plugin UI hosting (embedding plugin editor windows) | Hosts must display plugin GUIs | No | Yes | **pulp-host** | MEDIUM | `EditorHost` embeds a plugin's native view into a Pulp `Surface` |

## Standalone App Support

| Domain | Observed Capability | Why It Matters | Required for Pulp v1 | Required for Long-Term | Proposed Pulp Subsystem | Clean-Room Risk | Notes |
|---|---|---|---|---|---|---|---|
| Standalone App Support | Standalone audio app with device selection UI | Testing plugins without a DAW; shipping standalone products | Yes | Yes | **pulp-format** | MEDIUM | `StandaloneHost` provides device picker and wraps `pulp-audio` |
| Standalone App Support | Wrapping a plugin processor as a standalone app | Single codebase for plugin and standalone | Yes | Yes | **pulp-format** | MEDIUM | `StandaloneHost` instantiates user's `Patch` and bridges audio/MIDI |
| Standalone App Support | Native windowing integration for standalone mode | Standalone apps need proper OS window management | Yes | Yes | **pulp-view** | MEDIUM | Uses `pulp-view` `Surface` directly; no plugin-host window embedding needed |

## File System

| Domain | Observed Capability | Why It Matters | Required for Pulp v1 | Required for Long-Term | Proposed Pulp Subsystem | Clean-Room Risk | Notes |
|---|---|---|---|---|---|---|---|
| File System | Cross-platform file and path abstraction | Portable code must handle paths correctly | Yes | Yes | **pulp-runtime** | MEDIUM | Use `std::filesystem` (C++17); Pulp adds convenience wrappers like `AppPaths` for standard locations |
| File System | File streams: buffered read/write | Basic I/O for presets, audio files, configs | Yes | Yes | **pulp-runtime** | LOW | Standard C++ streams with RAII wrappers |
| File System | Memory-mapped files | Efficient access to large sample libraries | No | Yes | **pulp-runtime** | LOW | Thin wrapper over mmap/MapViewOfFile |
| File System | Temporary files and directories | Build processes, caches, intermediate data | Yes | Yes | **pulp-runtime** | LOW | `std::filesystem::temp_directory_path()` with Pulp helpers |
| File System | Directory iteration and file watching | Preset browsers, auto-reload on change | No | Yes | **pulp-runtime** | LOW | Standard directory iterators; platform file-watching APIs |

## Networking

| Domain | Observed Capability | Why It Matters | Required for Pulp v1 | Required for Long-Term | Proposed Pulp Subsystem | Clean-Room Risk | Notes |
|---|---|---|---|---|---|---|---|
| Networking | HTTP/HTTPS client | License validation, update checks, cloud presets | No | Yes | **pulp-runtime** | LOW | Use libcurl or platform-native (NSURLSession, WinHTTP) |
| Networking | TCP/UDP sockets | OSC communication, custom protocols | No | Yes | **pulp-runtime** | LOW | Thin RAII wrappers over BSD sockets / Winsock |
| Networking | Named pipes / IPC | Inter-process communication for multi-process plugins | No | Yes | **pulp-runtime** | LOW | Platform-native IPC mechanisms |
| Networking | Network service discovery (mDNS/Bonjour) | Discovering devices on local network (controllers, remote apps) | No | Yes | **pulp-runtime** | LOW | Wrap dns-sd / Avahi APIs |

## Threading / Concurrency

| Domain | Observed Capability | Why It Matters | Required for Pulp v1 | Required for Long-Term | Proposed Pulp Subsystem | Clean-Room Risk | Notes |
|---|---|---|---|---|---|---|---|
| Threading / Concurrency | Thread creation and management | Background work, parallel processing | Yes | Yes | **pulp-runtime** | MEDIUM | Use `std::thread` and `std::jthread`; Pulp adds `WorkerThread` with name and priority |
| Threading / Concurrency | Thread pools for parallel work | Efficient CPU utilization for batch operations | No | Yes | **pulp-runtime** | LOW | `ThreadPool` using standard primitives; consider `std::execution` when available |
| Threading / Concurrency | Mutexes, locks, condition variables | Thread synchronization | Yes | Yes | **pulp-runtime** | LOW | Use C++ standard library primitives directly |
| Threading / Concurrency | Lock-free data structures: FIFO, ring buffer | Real-time audio threads cannot use locks | Yes | Yes | **pulp-runtime** | MEDIUM | `LockFreeQueue<T>` and `RingBuffer<T>`; standard SPSC designs from literature |
| Threading / Concurrency | Async dispatch to main thread | Audio thread must communicate with UI thread without blocking | Yes | Yes | **pulp-events** | HIGH | `EventLoop::dispatch(fn)` posts to the platform run loop; no singleton message manager |
| Threading / Concurrency | Timer callbacks (periodic and one-shot) | UI refresh, polling, scheduled tasks | Yes | Yes | **pulp-events** | MEDIUM | `Timer` objects scheduled on an `EventLoop` instance |
| Threading / Concurrency | Audio workgroup support (Apple) | macOS/iOS real-time thread priority management | No | Yes | **pulp-platform** | LOW | Direct use of Apple's `os_workgroup` API |

## Timing

| Domain | Observed Capability | Why It Matters | Required for Pulp v1 | Required for Long-Term | Proposed Pulp Subsystem | Clean-Room Risk | Notes |
|---|---|---|---|---|---|---|---|
| Timing | High-resolution timer / clock | Performance measurement, profiling | Yes | Yes | **pulp-runtime** | LOW | `std::chrono::steady_clock` and `std::chrono::high_resolution_clock` |
| Timing | Performance measurement utilities | Profiling DSP, UI rendering | No | Yes | **pulp-runtime** | LOW | `ScopedTimer` RAII utility; generic pattern |
| Timing | VBlank synchronization for UI rendering | Smooth animations without tearing | No | Yes | **pulp-view** | LOW | Platform-specific CVDisplayLink / DWM / VSync APIs |

## Serialization

| Domain | Observed Capability | Why It Matters | Required for Pulp v1 | Required for Long-Term | Proposed Pulp Subsystem | Clean-Room Risk | Notes |
|---|---|---|---|---|---|---|---|
| Serialization | XML parsing and generation | Plugin state, presets, configuration files | Yes | Yes | **pulp-runtime** | LOW | Use pugixml (MIT) or platform XML APIs |
| Serialization | JSON parsing and generation | Modern config files, web API communication, presets | Yes | Yes | **pulp-runtime** | LOW | Use nlohmann/json (MIT) or simdjson |
| Serialization | Binary serialization (compact state snapshots) | Efficient plugin state save/restore | Yes | Yes | **pulp-state** | MEDIUM | Pulp's `BinaryArchive` with versioned format; original design |
| Serialization | Structured tree-based data model for state | Hierarchical state representation with change notification | No | Yes | **pulp-state** | HIGH | `StateTree` with typed nodes, string keys, and subscription-based observation; deliberately different from the audited framework's reactive tree |

## State Management

| Domain | Observed Capability | Why It Matters | Required for Pulp v1 | Required for Long-Term | Proposed Pulp Subsystem | Clean-Room Risk | Notes |
|---|---|---|---|---|---|---|---|
| State Management | Plugin parameter model: float, int, bool, choice types | All plugin formats require typed, automatable parameters | Yes | Yes | **pulp-state** | HIGH | `ParamSpec` descriptors: `FloatParam`, `IntParam`, `BoolParam`, `ChoiceParam`; registered via `ParamGroup` |
| State Management | Parameter grouping and hierarchy | Organizes parameters for host display | Yes | Yes | **pulp-state** | MEDIUM | `ParamGroup` contains named sub-groups and parameters; flat iteration available |
| State Management | Host automation gestures (begin/end edit) | DAWs need to know when user is actively editing a parameter | Yes | Yes | **pulp-state** | MEDIUM | `HostLink` protocol with `beginGesture()` / `endGesture()` |
| State Management | State save/restore (binary and text formats) | Presets, session recall, undo state | Yes | Yes | **pulp-state** | MEDIUM | `Snapshot` type with `encode()` / `decode()` using `BinaryArchive` or JSON |
| State Management | GUI-parameter binding (two-way sync) | UI controls must reflect and update parameter values | Yes | Yes | **pulp-state** | HIGH | `Binding<T>` connects a `Widget` property to a parameter; observer pattern, not a monolithic tree-state object |
| State Management | Undo/redo system | DAWs provide undo for parameter changes | No | Yes | **pulp-state** | MEDIUM | `UndoStack` with `Action` protocol; generic command pattern |

## Host Integration

| Domain | Observed Capability | Why It Matters | Required for Pulp v1 | Required for Long-Term | Proposed Pulp Subsystem | Clean-Room Risk | Notes |
|---|---|---|---|---|---|---|---|
| Host Integration | Host identification (name, version) | Plugins may need host-specific workarounds | Yes | Yes | **pulp-format** | LOW | `HostInfo` struct populated by format wrapper |
| Host Integration | Transport state (play, stop, record, loop, position) | Time-synced effects, sequenced plugins | Yes | Yes | **pulp-format** | LOW | `TransportState` struct; fields defined by plugin format specs |
| Host Integration | Tempo and time signature from host | Tempo-synced delays, LFOs, arpeggios | Yes | Yes | **pulp-format** | LOW | Part of `TransportState`; standard across all plugin formats |
| Host Integration | Plugin latency reporting | Hosts need latency for delay compensation | Yes | Yes | **pulp-format** | LOW | `Patch` reports latency in samples; format wrappers relay to host |

## Platform Abstraction

| Domain | Observed Capability | Why It Matters | Required for Pulp v1 | Required for Long-Term | Proposed Pulp Subsystem | Clean-Room Risk | Notes |
|---|---|---|---|---|---|---|---|
| Platform Abstraction | macOS, Windows, Linux desktop support | Primary target platforms for audio software | Yes | Yes | **pulp-platform** | LOW | Platform-specific code isolated behind protocols/interfaces |
| Platform Abstraction | iOS and Android support | Mobile music apps are a growing market | No | Yes | **pulp-platform** | MEDIUM | Mobile support adds touch input, different lifecycle, background audio |
| Platform Abstraction | Native API bridging (Objective-C, Win32, JNI) | Platform features require native API calls | Yes | Yes | **pulp-platform** | LOW | Standard interop techniques; C++/ObjC++ on Apple, C++/COM on Windows, JNI on Android |
| Platform Abstraction | Platform-specific code isolation via compile-time selection | Clean separation of platform code from shared logic | Yes | Yes | **pulp-platform** | LOW | CMake-based source selection; `#ifdef`-free shared code where possible |

## Testing

| Domain | Observed Capability | Why It Matters | Required for Pulp v1 | Required for Long-Term | Proposed Pulp Subsystem | Clean-Room Risk | Notes |
|---|---|---|---|---|---|---|---|
| Testing | Unit test framework with assertions and test runners | Code quality requires automated testing | Yes | Yes | **pulp-test** | MEDIUM | Use Catch2 or GoogleTest; Pulp adds audio-specific matchers (`expectSamplesNear`, `expectSilence`) |
| Testing | Plugin validation (format compliance checks) | Plugins must pass format validators (AU Validation, VST3 validator) | Yes | Yes | **pulp-test** | LOW | Run external validators (pluginval, auval, VST3 validator) via `pulp-build` |
| Testing | Audio processing test utilities (compare buffers, measure SNR) | DSP code needs numerical verification | No | Yes | **pulp-test** | LOW | Utility functions for buffer comparison, FFT analysis, SNR measurement |
| Testing | GUI testing (automated UI interaction) | Catch visual regressions and interaction bugs | No | Yes | **pulp-test** | MEDIUM | `TestDriver` that simulates input events on a `Widget` tree |

## Packaging

| Domain | Observed Capability | Why It Matters | Required for Pulp v1 | Required for Long-Term | Proposed Pulp Subsystem | Clean-Room Risk | Notes |
|---|---|---|---|---|---|---|---|
| Packaging | macOS installer (.pkg) creation | Professional distribution on macOS | Yes | Yes | **pulp-ship** | LOW | Shell scripts / CMake driving `pkgbuild` and `productbuild` |
| Packaging | Windows installer (.exe / .msi) creation | Professional distribution on Windows | Yes | Yes | **pulp-ship** | LOW | Inno Setup, WiX, or NSIS; CMake integration |
| Packaging | Linux packaging (tar.gz, .deb) | Distribution on Linux | No | Yes | **pulp-ship** | LOW | Standard packaging tools; CPack integration |
| Packaging | Code signing (macOS and Windows) | Required for distribution without security warnings | Yes | Yes | **pulp-ship** | LOW | `codesign` on macOS, `signtool` on Windows |
| Packaging | macOS notarization | Required by Apple for distribution outside App Store | Yes | Yes | **pulp-ship** | LOW | `notarytool` / `xcrun` automation |
| Packaging | EdDSA signing for auto-update verification | Secure update distribution | No | Yes | **pulp-ship** | LOW | Standard cryptographic signing; Sparkle/WinSparkle compatible |

## Examples / Templates

| Domain | Observed Capability | Why It Matters | Required for Pulp v1 | Required for Long-Term | Proposed Pulp Subsystem | Clean-Room Risk | Notes |
|---|---|---|---|---|---|---|---|
| Examples / Templates | Plugin project template (synth, effect) | Developers need a quick-start path | Yes | Yes | **pulp-build** | MEDIUM | Pulp's own template structure; must not mirror audited framework's project layout |
| Examples / Templates | Standalone app template | For non-plugin audio applications | No | Yes | **pulp-build** | LOW | Minimal app with audio I/O and a window |
| Examples / Templates | Console app template | CLI audio tools, batch processing | No | Yes | **pulp-build** | LOW | Minimal CMake + main.cpp |
| Examples / Templates | Example demos (synthesizer, effects, visualizer) | Demonstrate framework capabilities | No | Yes | **pulp-build** | MEDIUM | Original demo designs; must not replicate audited framework's demo structure |

## Code Generation / Scaffolding

| Domain | Observed Capability | Why It Matters | Required for Pulp v1 | Required for Long-Term | Proposed Pulp Subsystem | Clean-Room Risk | Notes |
|---|---|---|---|---|---|---|---|
| Code Generation | Project creation from template with placeholder replacement | Fast project setup | Yes | Yes | **pulp-build** | LOW | `pulp new` CLI command; Pulp's own template format |
| Code Generation | Feature flag configuration (enable/disable formats, subsystems) | Not all projects need all features | Yes | Yes | **pulp-build** | LOW | CMake options: `PULP_ENABLE_VST3`, `PULP_ENABLE_AU`, etc. |
| Code Generation | Dependency management for third-party libraries | Reproducible builds | No | Yes | **pulp-build** | LOW | CMake FetchContent or vcpkg/Conan integration |

## Documentation

| Domain | Observed Capability | Why It Matters | Required for Pulp v1 | Required for Long-Term | Proposed Pulp Subsystem | Clean-Room Risk | Notes |
|---|---|---|---|---|---|---|---|
| Documentation | API reference generation | Developers need searchable API docs | No | Yes | **pulp-build** | LOW | Doxygen or standardese for C++; DocC for Swift |
| Documentation | In-tree documentation and tutorials | Lowers barrier to adoption | No | Yes | — | LOW | Markdown docs in repository |
| Documentation | Getting-started guides and tutorials | Critical for developer onboarding | Yes | Yes | — | LOW | Original content; must not paraphrase audited framework's tutorials |

## CI/CD

| Domain | Observed Capability | Why It Matters | Required for Pulp v1 | Required for Long-Term | Proposed Pulp Subsystem | Clean-Room Risk | Notes |
|---|---|---|---|---|---|---|---|
| CI/CD | GitHub Actions workflow generation | Automated builds and tests | Yes | Yes | **pulp-ci** | LOW | YAML workflow templates for macOS, Windows, Linux matrix |
| CI/CD | Multi-platform build matrix | Must verify builds on all targets | Yes | Yes | **pulp-ci** | LOW | Standard GitHub Actions matrix strategy |
| CI/CD | Automated testing in CI | Catch regressions before merge | Yes | Yes | **pulp-ci** | LOW | Run `pulp-test` suite in CI |
| CI/CD | Release automation (build, sign, package, upload) | Streamlined release process | No | Yes | **pulp-ci** | LOW | GitHub Actions workflow for tagged releases |

## Installer / Release

| Domain | Observed Capability | Why It Matters | Required for Pulp v1 | Required for Long-Term | Proposed Pulp Subsystem | Clean-Room Risk | Notes |
|---|---|---|---|---|---|---|---|
| Installer / Release | Auto-update mechanism (check, download, apply) | Users expect seamless updates | No | Yes | **pulp-ship** | LOW | Sparkle (macOS, MIT), WinSparkle (Windows, MIT) integration |
| Installer / Release | Appcast / update feed generation | Server-side component for auto-update | No | Yes | **pulp-ship** | LOW | Generate RSS/Atom feed with EdDSA signatures |
| Installer / Release | Download page generation | Marketing/distribution website | No | Yes | **pulp-ship** | LOW | Static site generator or template |

## Developer Tooling

| Domain | Observed Capability | Why It Matters | Required for Pulp v1 | Required for Long-Term | Proposed Pulp Subsystem | Clean-Room Risk | Notes |
|---|---|---|---|---|---|---|---|
| Developer Tooling | Project status dashboard | Quick overview of build state, test results | No | Yes | **pulp-build** | LOW | CLI command: `pulp status` |
| Developer Tooling | Build command wrapper | Simplified build invocation | Yes | Yes | **pulp-build** | LOW | `pulp build`, `pulp run`, `pulp test` CLI commands |
| Developer Tooling | Environment validation | Check that SDK, compilers, signing certs are present | Yes | Yes | **pulp-build** | LOW | `pulp doctor` command |
| Developer Tooling | Cross-platform porting assistance | Help developers fix platform-specific issues | No | Yes | **pulp-build** | LOW | Diagnostic suggestions in build output |

## GitHub Integration

| Domain | Observed Capability | Why It Matters | Required for Pulp v1 | Required for Long-Term | Proposed Pulp Subsystem | Clean-Room Risk | Notes |
|---|---|---|---|---|---|---|---|
| GitHub Integration | Repository creation from template | Quick project bootstrap with remote | No | Yes | **pulp-ci** | LOW | `pulp init --github` wrapping `gh repo create` |
| GitHub Integration | PR and release management | Structured development workflow | No | Yes | **pulp-ci** | LOW | Integration with `gh` CLI |
| GitHub Integration | GitHub Pages deployment | Hosting docs and download pages | No | Yes | **pulp-ci** | LOW | Workflow template for Pages deployment |
| GitHub Integration | Secrets synchronization for CI | Signing certificates and keys in CI | No | Yes | **pulp-ci** | LOW | `pulp ci secrets` wrapping `gh secret set` |

## Claude Code Integration

| Domain | Observed Capability | Why It Matters | Required for Pulp v1 | Required for Long-Term | Proposed Pulp Subsystem | Clean-Room Risk | Notes |
|---|---|---|---|---|---|---|---|
| Claude Code Integration | Plugin/skill architecture for Claude Code | AI-assisted audio development workflow | No | Yes | **pulp-claude** | LOW | Slash commands and context providers for Claude Code |
| Claude Code Integration | Command-based workflow automation | Streamline repetitive tasks via conversation | No | Yes | **pulp-claude** | LOW | `/pulp:build`, `/pulp:test`, `/pulp:ship` commands |
| Claude Code Integration | Contextual knowledge bases | Provide Pulp API knowledge to Claude | No | Yes | **pulp-claude** | LOW | CLAUDE.md and knowledge files in repository |
| Claude Code Integration | Interactive project configuration | Conversational project setup | No | Yes | **pulp-claude** | LOW | Guided `pulp new` via Claude Code interaction |

## Optional GPU-Accelerated UI (Visage Path)

| Domain | Observed Capability | Why It Matters | Required for Pulp v1 | Required for Long-Term | Proposed Pulp Subsystem | Clean-Room Risk | Notes |
|---|---|---|---|---|---|---|---|
| GPU UI | GPU rendering backend (Metal, D3D11, Vulkan via bgfx) | High-performance UI for complex visualizations | No | Yes | **pulp-gpu** | LOW | bgfx (BSD 2-Clause) as abstraction layer; alternative to CPU-rendered `pulp-canvas` |
| GPU UI | Integration bridge with framework windowing | GPU renderer must work within Pulp's window system | No | Yes | **pulp-gpu** | MEDIUM | `GpuSurface` implements `Canvas` protocol backed by GPU |
| GPU UI | GPU-aware theme system | Themes must work with both CPU and GPU renderers | No | Yes | **pulp-gpu** | LOW | Same `ThemeContext` tokens; `GpuSurface` interprets them for shaders |
| GPU UI | Custom widget rendering at 60-120 FPS | Smooth meters, oscilloscopes, spectrograms | No | Yes | **pulp-gpu** | LOW | `GpuWidget` subclass with `render(gpu: &GpuContext)` method |

---

## Summary Statistics

| Category | Total Capabilities | Required for v1 | Required Long-Term | High Risk | Medium Risk | Low Risk |
|---|---|---|---|---|---|---|
| Application Framework | 5 | 2 | 5 | 1 | 3 | 1 |
| Windowing | 5 | 2 | 5 | 1 | 2 | 2 |
| Graphics | 5 | 3 | 5 | 0 | 3 | 2 |
| UI / Widget System | 9 | 5 | 9 | 3 | 4 | 2 |
| Audio I/O | 7 | 3 | 7 | 0 | 3 | 4 |
| MIDI I/O | 6 | 2 | 6 | 0 | 2 | 4 |
| DSP Utilities | 12 | 4 | 12 | 0 | 6 | 6 |
| Plugin Format Wrappers | 9 | 4 | 8 | 3 | 3 | 3 |
| Plugin Host Support | 4 | 0 | 4 | 2 | 2 | 0 |
| Standalone App Support | 3 | 3 | 3 | 0 | 3 | 0 |
| File System | 5 | 3 | 5 | 0 | 1 | 4 |
| Networking | 4 | 0 | 4 | 0 | 0 | 4 |
| Threading / Concurrency | 7 | 5 | 7 | 1 | 3 | 3 |
| Timing | 3 | 1 | 3 | 0 | 0 | 3 |
| Serialization | 4 | 3 | 4 | 1 | 1 | 2 |
| State Management | 6 | 5 | 6 | 2 | 3 | 1 |
| Host Integration | 4 | 4 | 4 | 0 | 0 | 4 |
| Platform Abstraction | 4 | 2 | 4 | 0 | 1 | 3 |
| Testing | 4 | 2 | 4 | 0 | 2 | 2 |
| Packaging | 6 | 4 | 6 | 0 | 0 | 6 |
| Examples / Templates | 4 | 1 | 4 | 0 | 2 | 2 |
| Code Generation | 3 | 2 | 3 | 0 | 0 | 3 |
| Documentation | 3 | 1 | 3 | 0 | 0 | 3 |
| CI/CD | 4 | 3 | 4 | 0 | 0 | 4 |
| Installer / Release | 3 | 0 | 3 | 0 | 0 | 3 |
| Developer Tooling | 4 | 2 | 4 | 0 | 0 | 4 |
| GitHub Integration | 4 | 0 | 4 | 0 | 0 | 4 |
| Claude Code Integration | 4 | 0 | 4 | 0 | 0 | 4 |
| GPU UI | 4 | 0 | 4 | 0 | 1 | 3 |
| **TOTALS** | **148** | **66** | **147** | **14** | **45** | **89** |

---

## Key Takeaways

1. **148 capabilities** identified across 29 domains.
2. **66 capabilities** are required for Pulp v1 — focused on core audio, plugin formats, UI, and state management.
3. **14 high-risk items** require the most careful clean-room discipline — concentrated in plugin format wrappers, UI system, state management, and event dispatch.
4. **45 medium-risk items** need attention but have more generic solutions available.
5. **89 low-risk items** involve standard patterns, specifications, or third-party libraries with permissive licenses.
6. The highest contamination risk is in **pulp-format** (plugin wrappers), **pulp-view** (UI system), **pulp-state** (parameter/state management), and **pulp-events** (event dispatch) — these subsystems must be implemented with the strictest clean-room discipline.
