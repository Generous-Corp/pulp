# Module Reference

Pulp is organized into independent subsystems under `core/`. Each is a separate CMake library (`pulp::runtime`, `pulp::audio`, etc.) that you link as needed.

---

## runtime

> Core utilities — the foundation everything else builds on.

| Feature | Header | What It Does |
|---------|--------|-------------|
| SIMD | `simd.hpp` | Portable SSE/NEON/AVX via Google Highway — add, mul, fma, reduce, clamp |
| XML | `xml.hpp` | Parse/generate XML via pugixml — XPath queries, file I/O |
| ZIP/GZIP | `zip.hpp` | Compress/decompress via miniz — archives, state blobs |
| HTTP | `http.hpp` | GET, POST, download via cpp-httplib — license checks, cloud presets |
| Sockets | `socket.hpp` | TCP/UDP client and server |
| Named Pipes | `named_pipe.hpp` | Cross-platform IPC pipes (mkfifo / CreateNamedPipe) |
| IP Address | `ip_address.hpp` | IPv4 validation, local address queries, hostname |
| Crypto | `crypto.hpp` | SHA-256, MD5, AES-256-CBC via Mbed TLS — hashing, encryption |
| Licensing | `license.hpp` | RSA signature verification, key generation, online activation |
| BigInteger | `big_integer.hpp` | Arbitrary-precision arithmetic via Mbed TLS MPI — for RSA |
| i18n | `i18n.hpp` | String translation — .strings, .po, .json file loaders, `tr()` helper |
| Analytics | `analytics.hpp` | Thread-safe event tracking with pluggable file/HTTP destinations |
| Base64 | `base64.hpp` | Encode/decode binary ↔ text |
| Memory Map | `memory_mapped_file.hpp` | RAII mmap / MapViewOfFile for large file access |
| Child Process | `child_process.hpp` | Launch, capture stdout/stderr, wait for exit |
| Dynamic Library | `dynamic_library.hpp` | RAII dlopen / LoadLibrary — load plugins at runtime |
| IPC Lock | `inter_process_lock.hpp` | Cross-process file lock (flock / CreateFile) |
| Temp File | `temporary_file.hpp` | RAII temp file with auto-delete |
| Timer | `high_resolution_timer.hpp` | Sub-millisecond periodic callback |
| System Info | `system.hpp` | CPU model, cores, RAM, OS version, architecture |
| Range | `range.hpp` | Templated interval with intersection, union, constrain |
| Text Diff | `text_diff.hpp` | LCS-based line diff with formatted output |
| Primes | `primes.hpp` | Miller-Rabin primality test, prime generation, Eratosthenes sieve |
| Expression | `expression.hpp` | Math evaluator with variables, functions (sin/cos/sqrt/min/max), constants |
| Scope Guard | `scope_guard.hpp` | RAII cleanup — `PULP_ON_SCOPE_EXIT(...)` |
| Identity | `identity.hpp` | UUIDv4 generation, typed IDs (SessionId, ObjectId, RunId) |

---

## events

> Event loop, timers, IPC, and process management.

| Feature | Header | What It Does |
|---------|--------|-------------|
| Event Loop | `event_loop.hpp` | Constructible (not singleton) message pump |
| Timer | `timer.hpp` | Periodic and one-shot timers |
| Async Updater | `async_updater.hpp` | Coalesce rapid cross-thread updates into one callback |
| Multi Timer | `async_updater.hpp` | Multiple named timers from one object |
| Action Broadcaster | `async_updater.hpp` | String-based action dispatch (menu commands) |
| IPC Connection | `interprocess_connection.hpp` | Length-prefixed messages over named pipes or TCP |
| IPC Server | `interprocess_connection.hpp` | Accept multiple client connections |
| Child Process Pool | `child_process_manager.hpp` | Crash-isolated process management (plugin scanning) |
| Connected Process | `child_process_manager.hpp` | Launch child with bidirectional IPC channel |
| Volume Detector | `volume_detector.hpp` | Monitor filesystem for volume mount/unmount |
| Service Discovery | `volume_detector.hpp` | mDNS/Bonjour network service browsing |
| Low Power Disable | `async_updater.hpp` | Prevent OS power throttling during audio |

---

## audio

> Device I/O, file formats, channel layouts, and offline processing.

**Device backends:**

| Platform | Backend | Header |
|----------|---------|--------|
| macOS | CoreAudio | `platform/mac/coreaudio_device.hpp` |
| Windows | WASAPI | `platform/win/wasapi_device.hpp` |
| Linux | ALSA | `platform/linux/alsa_device.hpp` |
| Linux | JACK | `platform/linux/jack_device.hpp` |
| Web | Web Audio | `src/web_audio.cpp` |

**Audio file formats** (via `format_registry.hpp`):

| Format | Read | Write | Backend |
|--------|:----:|:-----:|---------|
| WAV | ✓ | ✓ | CHOC |
| FLAC | ✓ | — | dr_flac |
| MP3 | ✓ | — | dr_mp3 |
| OGG Vorbis | ✓ | — | stb_vorbis |
| AIFF / AIFF-C | ✓ | ✓ | Native (8/16/24/32-bit) |
| AAC / ALAC / CAF | ✓ | — | ExtAudioFile (macOS only) |

**Other features:**

| Feature | Header | What It Does |
|---------|--------|-------------|
| Channel Sets | `channel_set.hpp` | Named layouts: mono, stereo, 5.1, 7.1, 7.1.4 (Atmos) |
| Offline Processor | `offline_processor.hpp` | Batch-process files through a callback — bouncing, golden files |
| Buffering Reader | `buffering_reader.hpp` | Background-thread ring buffer for gapless streaming |
| System Volume | `system_volume.hpp` | Get/set system output volume and mute (all platforms) |
| Streaming Writer | `streaming_writer.hpp` | Chunked WAV write — open, write_frames N times, close (16/24/32-bit) |
| Memory-Mapped Reader | `mmap_reader.hpp` | Zero-copy audio file access via mmap + FormatRegistry |
| Subsection Reader | `subsection_reader.hpp` | Read a frame range from audio data without copying |
| Format Registry | `format_registry.hpp` | Extensible codec registry — register custom readers/writers |
| Load Measurer | `load_measurer.hpp` | Real-time CPU usage tracking for audio callbacks |

---

## midi

> MIDI I/O, file handling, and MIDI 2.0 support.

| Feature | Header | What It Does |
|---------|--------|-------------|
| MIDI Messages | via CHOC | `ShortMessage`, note/CC helpers, channel voice |
| MIDI Buffer | `midi_buffer.hpp` | Timestamped event buffer for process callbacks |
| MIDI Files | `midi_file.hpp` | Read/write Standard MIDI Files |
| UMP | `ump.hpp` | Universal MIDI Packets (MIDI 2.0), MPE zones |
| MIDI CI | `midi_ci.hpp` | Device discovery, profile management, property exchange |
| MIDI Sequence | `midi_message_sequence.hpp` | Ordered timestamped events with note pairing, range queries |
| CoreMIDI | platform/mac | macOS MIDI device I/O |
| WinMIDI | platform/win | Windows MIDI device I/O |
| ALSA MIDI | platform/linux | Linux MIDI device I/O |
| Web MIDI | src/web_midi.cpp | Browser MIDI access |

---

## signal

> 30+ real-time-safe DSP processors.

**Filters:**

| Processor | Header | Description |
|-----------|--------|-------------|
| Biquad (IIR) | `biquad.hpp` | Low/high/band pass, notch, allpass, shelf |
| FIR Filter | `fir_filter.hpp` | Arbitrary-length finite impulse response |
| State Variable (TPT) | `tpt_filter.hpp` | Zero-delay feedback topology |
| Ladder Filter | `ladder_filter.hpp` | 4-pole resonant (Moog-style) |
| Linkwitz-Riley | `linkwitz_riley.hpp` | Crossover-grade linear-phase |
| Filter Design | `filter_design.hpp` | Butterworth, Chebyshev coefficient calculation |

**Effects:**

| Processor | Header | Description |
|-----------|--------|-------------|
| Convolution | `convolver.hpp` | Partitioned convolution for impulse responses |
| Reverb | `reverb.hpp` | Algorithmic reverb |
| Chorus | `chorus.hpp` | Modulated delay chorus |
| Phaser | `phaser.hpp` | All-pass phaser |
| Delay Line | `delay_line.hpp` | Interpolated delay (linear, cubic, sinc) |
| Waveshaper | `waveshaper.hpp` | Nonlinear distortion |
| Oversampling | `oversampling.hpp` | 2x/4x/8x with anti-aliasing |

**Dynamics:**

| Processor | Header | Description |
|-----------|--------|-------------|
| Compressor | `compressor.hpp` | Feed-forward with soft knee |
| Limiter | `compressor.hpp` | Brickwall with instant attack |
| Noise Gate | `noise_gate.hpp` | Threshold-based gate |
| DryWetMixer | `dry_wet_mixer.hpp` | Latency-compensated blend (linear or equal-power) |
| Bias | `bias.hpp` | Add constant DC offset to signal |

**Generators & Math:**

| Processor | Header | Description |
|-----------|--------|-------------|
| Oscillator | `oscillator.hpp` | Wavetable with anti-aliased waveforms |
| ADSR | `adsr.hpp` | Envelope generator |
| FFT | `fft.hpp` | Radix-2, vDSP on Apple |
| STFT | `stft.hpp` | Short-time Fourier transform |
| Windowing | `windowing.hpp` | Hann, Hamming, Blackman, Kaiser |
| Smoothed Value | `smoothed_value.hpp` | Parameter smoothing (linear, log-ramped) |
| Lookup Table | `lookup_table.hpp` | Pre-computed function table |
| Fast Math | `fast_math.hpp` | Approximations for sin, cos, tanh, exp |
| Special Functions | `special_functions.hpp` | sinc, bessel, lanczos, dB↔linear, MIDI↔freq |
| Matrix | `matrix.hpp` | 2×2, 3×3, 4×4 matrix ops + transforms |
| SIMD Buffer | `simd_buffer.hpp` | 64-byte aligned buffer for vectorized access |

---

## state

> Parameters, state trees, presets, and settings.

| Feature | Header | What It Does |
|---------|--------|-------------|
| StateStore | `store.hpp` | Atomic parameter values with format adapter sync |
| ParamInfo | `param_info.hpp` | Parameter metadata (range, name, string mapping) |
| Binding | `binding.hpp` | UI ↔ parameter connection with gesture undo grouping |
| StateTree | `state_tree.hpp` | Reactive hierarchical key-value store with JSON serialization |
| StateTree Sync | `state_tree_sync.hpp` | Delta-based binary sync over IPC |
| Observable Value | `state_tree.hpp` | Generic observable with change listeners |
| Cached Property | `cached_property.hpp` | Listener-backed StateTree accessor with local cache |
| PropertiesFile | `properties_file.hpp` | JSON-backed persistent settings (via CHOC) |
| App Properties | `properties_file.hpp` | Platform-standard user/common settings paths |
| Preset Manager | `preset_manager.hpp` | Factory/user presets, navigation, import/export |
| Undo Manager | `undo_manager.hpp` | Undo/redo with action grouping |
| Serialization | `store.hpp` | Versioned binary format with CRC |

---

## format

> Plugin format adapters — write once, deploy to 9 formats.

| Format | Status | Notes |
|--------|--------|-------|
| VST3 | ✓ Stable | Full parameter sync, state, editor resize |
| AU v2 | ✓ Stable | macOS only, via AudioUnitSDK |
| AU v3 | ✓ Stable | macOS + iOS |
| CLAP | ✓ Stable | First-class with modulation, WebView |
| LV2 | ✓ Usable | Linux plugin format |
| AAX | ✓ Usable | Requires developer-supplied SDK |
| Standalone | ✓ Stable | Desktop app with device selector |
| WAM | ✓ Experimental | Web Audio Module for browsers |
| WCLAP | ✓ Experimental | Web CLAP for browsers |

**Other format features:**

| Feature | Header | What It Does |
|---------|--------|-------------|
| Host Detection | `host_type.hpp` | Detect DAW from process name (Logic, Reaper, Ableton, etc.) |
| ARA | `ara.hpp` | Audio Random Access document controller stub |

---

## canvas

> 2D drawing with GPU acceleration and smart text layout.

| Feature | Header | What It Does |
|---------|--------|-------------|
| Canvas API | `canvas.hpp` | 25+ draw commands — rect, rounded rect, path, arc, text, image |
| Skia Backend | `src/skia_canvas.cpp` | GPU-accelerated via Skia Graphite (Metal/Vulkan/D3D12) |
| CoreGraphics | `platform/mac/cg_canvas.mm` | Native macOS/iOS rendering |
| TextShaper | `text_shaper.hpp` | **Measure once, reflow forever** — PreText-style layout engine |
| Attributed String | `attributed_string.hpp` | Rich text spans with font, color, weight, decoration |
| Text Layout | `text_layout.hpp` | Multi-line layout with word wrapping |
| Rectangle List | `rectangle_list.hpp` | Clip regions with add/subtract/intersect |
| Image Convolution | `image_convolution.hpp` | Blur, sharpen, edge detect, emboss kernels |
| SVG | `svg.hpp` | SVG loading and rendering via nanosvg |
| Effects | `effects.hpp` | Drop shadow, bloom, blur, color adjust |
| Recording Canvas | `recording_canvas.hpp` | Record draw calls for replay/serialization |

**Text shaping option:** `PULP_TEXT_SHAPING` (CMake)
- Default: **ON** when GPU enabled, **OFF** without
- ON: Uses SkFont/HarfBuzz (bundled in Skia) for real font metrics
- OFF: Falls back to character-width estimation
- Same API either way — only measurement accuracy differs

---

## view

> Full widget toolkit with CSS-inspired layout and JS scripting.

**Widgets** (30+):

| Widget | What It Does |
|--------|-------------|
| Knob | Rotary control with arc, SkSL shaders, Lottie animation |
| Fader | Linear slider (vertical/horizontal) |
| Toggle / Checkbox | Boolean on/off controls |
| TextButton | Push button with text label |
| ComboBox | Dropdown selector |
| TextEditor | Full-featured with IME, selection, undo |
| Label | Static/dynamic text with styling |
| ListBox | Virtualized scrollable list |
| TreeView | Hierarchical tree with expand/collapse |
| TableListBox | Sortable columns with header click-to-sort |
| Toolbar | Horizontal/vertical with buttons, toggles, separators |
| TabPanel | Tabbed container |
| SplitView | Resizable split panes |
| ScrollView | Scrollable viewport |
| ConcertinaPanel | Accordion-style collapsible sections |
| Panel | Styled container with background/border tokens |
| Meter | Audio level meter with peak hold |
| MultiMeter | Multi-channel meter (any channel count) |
| CorrelationMeter | Stereo phase display |
| XYPad | 2D parameter surface |
| WaveformView | Audio waveform display |
| SpectrumView | Frequency spectrum (bars, line, filled) |
| SpectrogramView | Scrolling time-frequency heatmap |
| EqCurveView | Parametric EQ frequency response |
| MidiKeyboard | Piano keyboard with note display |
| PresetBrowser | Factory/user presets with search |
| ColorPicker | HSV/RGB color selection |
| ImageView | Image display from file |
| CanvasWidget | Custom drawing via Canvas API |
| CodeEditor | Code editor with line numbers and syntax highlighting |
| LassoComponent | Rubber-band marquee selection |
| FileBrowser | Directory navigation with filters |
| FileTree | Tree-structured filesystem view |
| SplashScreen | Borderless window with fade animation |
| LiveConstantEditor | Debug overlay for tweaking numeric constants |

**Layout:** Yoga flexbox + CSS Grid, absolute/fixed positioning, intrinsic sizing.

**Theming:** Token-based design system (`"bg.surface"`, `"control.border"`), contrast-aware, theme presets.

**JS Scripting:** QuickJS (default), JavaScriptCore (Apple), V8 (optional). Hot-reload. Full `WidgetBridge` + `AudioBridge` for parameter access from JS.

**Accessibility:** AccessRole, value/text/table/cell interfaces, VoiceOver (macOS), UIA (Windows), AT-SPI (Linux).

---

## osc

> Open Sound Control messaging.

| Feature | Header | What It Does |
|---------|--------|-------------|
| OSC Sender | `osc.hpp` | Send messages over UDP |
| OSC Receiver | `osc.hpp` | Receive messages over UDP |
| OSC Bundle | `bundle.hpp` | Timetag + nested messages per OSC 1.0 |
| Address Matching | `bundle.hpp` | Wildcard patterns: `*`, `?`, `[...]`, `{...}` |
| Argument Types | `osc.hpp` | int, float, string, blob |

---

## render

> GPU surface management.

| Feature | What It Does |
|---------|-------------|
| Dawn/WebGPU | Cross-platform GPU abstraction (Metal, Vulkan, D3D12, OpenGL) |
| Skia Graphite | 2D rendering engine on top of Dawn |
| GPU Compute | Experimental batch audio processing (>64K elements) |

---

## ship

> Packaging and distribution.

| Feature | What It Does |
|---------|-------------|
| Code Signing | macOS (`codesign`) and Windows (`signtool`) |
| Notarization | macOS notarization workflow |
| DMG / PKG | macOS installer creation |
| Windows Installer | NSIS-based installer |
| Linux Packaging | .deb and .tar.gz |
| Appcast | Sparkle/WinSparkle update feed generation |
