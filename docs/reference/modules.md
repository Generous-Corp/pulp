# Module Reference

Pulp is organized into independent subsystems under `core/`. Each is a separate CMake library (`pulp::runtime`, `pulp::audio`, etc.) that you link as needed.

---

## runtime

> Core utilities — the foundation everything else builds on.

| Feature | Header | What It Does |
|---------|--------|-------------|
| Analytics | `analytics.hpp` | Thread-safe event tracking with pluggable file/HTTP destinations |
| Base64 | `base64.hpp` | Encode/decode binary ↔ text |
| BigInteger | `big_integer.hpp` | Arbitrary-precision arithmetic via Mbed TLS MPI — for RSA |
| Child Process | `child_process.hpp` | Launch, capture stdout/stderr, wait for exit |
| Crypto | `crypto.hpp` | SHA-256, MD5, AES-256-CBC via Mbed TLS — hashing, encryption |
| Dynamic Library | `dynamic_library.hpp` | RAII dlopen / LoadLibrary — load plugins at runtime |
| Expression | `expression.hpp` | Math evaluator with variables, functions (sin/cos/sqrt/min/max), constants |
| HTTP | `http.hpp` | GET, POST, download via cpp-httplib — license checks, cloud presets |
| i18n | `i18n.hpp` | String translation — .strings, .po, .json file loaders, `tr()` helper |
| Identity | `identity.hpp` | UUIDv4 generation, typed IDs (SessionId, ObjectId, RunId) |
| IP Address | `ip_address.hpp` | IPv4 validation, local address queries, hostname |
| IPC Lock | `inter_process_lock.hpp` | Cross-process file lock (flock / CreateFile) |
| Licensing | `license.hpp` | RSA signature verification, key generation, online activation |
| Memory Map | `memory_mapped_file.hpp` | RAII mmap / MapViewOfFile for large file access |
| Named Pipes | `named_pipe.hpp` | Cross-platform IPC pipes (mkfifo / CreateNamedPipe) |
| Primes | `primes.hpp` | Miller-Rabin primality test, prime generation, Eratosthenes sieve |
| Range | `range.hpp` | Templated interval with intersection, union, constrain |
| Scope Guard | `scope_guard.hpp` | RAII cleanup — `PULP_ON_SCOPE_EXIT(...)` |
| SIMD | `simd.hpp` | Portable SSE/NEON/AVX via Google Highway — add, mul, fma, reduce, clamp |
| Sockets | `socket.hpp` | TCP/UDP client and server |
| System Info | `system.hpp` | CPU model, cores, RAM, OS version, architecture |
| Temp File | `temporary_file.hpp` | RAII temp file with auto-delete |
| Text Diff | `text_diff.hpp` | LCS-based line diff with formatted output |
| Timer | `high_resolution_timer.hpp` | Sub-millisecond periodic callback |
| XML | `xml.hpp` | Parse/generate XML via pugixml — XPath queries, file I/O |
| ZIP/GZIP | `zip.hpp` | Compress/decompress via miniz — archives, state blobs |

---

## events

> Event loop, timers, IPC, and process management.

| Feature | Header | What It Does |
|---------|--------|-------------|
| Action Broadcaster | `async_updater.hpp` | String-based action dispatch (menu commands) |
| Async Updater | `async_updater.hpp` | Coalesce rapid cross-thread updates into one callback |
| Child Process Pool | `child_process_manager.hpp` | Crash-isolated process management (plugin scanning) |
| Connected Process | `child_process_manager.hpp` | Launch child with bidirectional IPC channel |
| Event Loop | `event_loop.hpp` | Constructible (not singleton) message pump |
| IPC Connection | `interprocess_connection.hpp` | Length-prefixed messages over named pipes or TCP |
| IPC Server | `interprocess_connection.hpp` | Accept multiple client connections |
| Low Power Disable | `async_updater.hpp` | Prevent OS power throttling during audio |
| Multi Timer | `async_updater.hpp` | Multiple named timers from one object |
| Service Discovery | `volume_detector.hpp` | mDNS/Bonjour network service browsing |
| Timer | `timer.hpp` | Periodic and one-shot timers |
| Volume Detector | `volume_detector.hpp` | Monitor filesystem for volume mount/unmount |

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
| WAV | ✓ | ✓ | CHOC + StreamingWriter |
| FLAC | ✓ | ✓* | dr_flac / libflac (BSD-3, `pulp add libflac`) |
| MP3 | ✓ | ✓* | dr_mp3 / LAME (LGPL, `pulp add lame --accept-license LGPL-2.0`) |
| OGG Vorbis | ✓ | — | stb_vorbis |
| AIFF / AIFF-C | ✓ | ✓ | Native (8/16/24/32-bit) |
| AAC | ✓ | ✓* | ExtAudioFile (macOS) / FDK AAC (`pulp add fdk-aac --accept-license FDK-AAC`) |
| ALAC | ✓ | ✓* | ExtAudioFile (macOS) / Apple ALAC (Apache 2.0, `pulp add alac`) |
| CAF | ✓ | — | ExtAudioFile (macOS only) |

*\* Write support requires installing the optional package via `pulp add`. Permissive packages (libflac, ALAC) install freely. Copyleft packages (LAME, fdk-aac) require `--accept-license`.*

**Other features:**

| Feature | Header | What It Does |
|---------|--------|-------------|
| Buffering Reader | `buffering_reader.hpp` | Background-thread ring buffer for gapless streaming |
| Channel Sets | `channel_set.hpp` | Named layouts: mono, stereo, 5.1, 7.1, 7.1.4 (Atmos) |
| Format Registry | `format_registry.hpp` | Extensible codec registry — register custom readers/writers |
| Load Measurer | `load_measurer.hpp` | Real-time CPU usage tracking for audio callbacks |
| Memory-Mapped Reader | `mmap_reader.hpp` | Zero-copy audio file access via mmap + FormatRegistry |
| Offline Processor | `offline_processor.hpp` | Batch-process files through a callback — bouncing, golden files |
| Streaming Writer | `streaming_writer.hpp` | Chunked WAV write — open, write_frames N times, close (16/24/32-bit) |
| Subsection Reader | `subsection_reader.hpp` | Read a frame range from audio data without copying |
| System Volume | `system_volume.hpp` | Get/set system output volume and mute (all platforms) |

---

## midi

> MIDI I/O, file handling, and MIDI 2.0 support.

| Feature | Header | What It Does |
|---------|--------|-------------|
| ALSA MIDI | platform/linux | Linux MIDI device I/O |
| CoreMIDI | platform/mac | macOS MIDI device I/O |
| MIDI Buffer | `midi_buffer.hpp` | Timestamped event buffer for process callbacks |
| MIDI CI | `midi_ci.hpp` | Device discovery, profile management, property exchange |
| MIDI Files | `midi_file.hpp` | Read/write Standard MIDI Files |
| MIDI Messages | via CHOC | `ShortMessage`, note/CC helpers, channel voice |
| MIDI Sequence | `midi_message_sequence.hpp` | Ordered timestamped events with note pairing, range queries |
| UMP | `ump.hpp` | Universal MIDI Packets (MIDI 2.0), MPE zones |
| Web MIDI | src/web_midi.cpp | Browser MIDI access |
| WinMIDI | platform/win | Windows MIDI device I/O |

---

## signal

> 30+ real-time-safe DSP processors.

**Filters:**

| Processor | Header | Description |
|-----------|--------|-------------|
| Biquad (IIR) | `biquad.hpp` | Low/high/band pass, notch, allpass, shelf |
| Filter Design | `filter_design.hpp` | Butterworth, Chebyshev coefficient calculation |
| FIR Filter | `fir_filter.hpp` | Arbitrary-length finite impulse response |
| Ladder Filter | `ladder_filter.hpp` | 4-pole resonant (Moog-style) |
| Linkwitz-Riley | `linkwitz_riley.hpp` | Crossover-grade linear-phase |
| State Variable (TPT) | `tpt_filter.hpp` | Zero-delay feedback topology |

**Effects:**

| Processor | Header | Description |
|-----------|--------|-------------|
| Chorus | `chorus.hpp` | Modulated delay chorus |
| Convolution | `convolver.hpp` | Partitioned convolution for impulse responses |
| Delay Line | `delay_line.hpp` | Interpolated delay (linear, cubic, sinc) |
| Oversampling | `oversampling.hpp` | 2x/4x/8x with anti-aliasing |
| Phaser | `phaser.hpp` | All-pass phaser |
| Reverb | `reverb.hpp` | Algorithmic reverb |
| Waveshaper | `waveshaper.hpp` | Nonlinear distortion |

**Dynamics:**

| Processor | Header | Description |
|-----------|--------|-------------|
| Bias | `bias.hpp` | Add constant DC offset to signal |
| Compressor | `compressor.hpp` | Feed-forward with soft knee |
| DryWetMixer | `dry_wet_mixer.hpp` | Latency-compensated blend (linear or equal-power) |
| Limiter | `compressor.hpp` | Brickwall with instant attack |
| Noise Gate | `noise_gate.hpp` | Threshold-based gate |

**Generators & Math:**

| Processor | Header | Description |
|-----------|--------|-------------|
| ADSR | `adsr.hpp` | Envelope generator |
| Fast Math | `fast_math.hpp` | Approximations for sin, cos, tanh, exp |
| FFT | `fft.hpp` | Radix-2, vDSP on Apple |
| Lookup Table | `lookup_table.hpp` | Pre-computed function table |
| Matrix | `matrix.hpp` | 2×2, 3×3, 4×4 matrix ops + transforms |
| Oscillator | `oscillator.hpp` | Wavetable with anti-aliased waveforms |
| SIMD Buffer | `simd_buffer.hpp` | 64-byte aligned buffer for vectorized access |
| Smoothed Value | `smoothed_value.hpp` | Parameter smoothing (linear, log-ramped) |
| Special Functions | `special_functions.hpp` | sinc, bessel, lanczos, dB↔linear, MIDI↔freq |
| STFT | `stft.hpp` | Short-time Fourier transform |
| Windowing | `windowing.hpp` | Hann, Hamming, Blackman, Kaiser |

---

## state

> Parameters, state trees, presets, and settings.

| Feature | Header | What It Does |
|---------|--------|-------------|
| App Properties | `properties_file.hpp` | Platform-standard user/common settings paths |
| Binding | `binding.hpp` | UI ↔ parameter connection with gesture undo grouping |
| Cached Property | `cached_property.hpp` | Listener-backed StateTree accessor with local cache |
| Observable Value | `state_tree.hpp` | Generic observable with change listeners |
| ParamInfo | `param_info.hpp` | Parameter metadata (range, name, string mapping) |
| Preset Manager | `preset_manager.hpp` | Factory/user presets, navigation, import/export |
| PropertiesFile | `properties_file.hpp` | JSON-backed persistent settings (via CHOC) |
| Serialization | `store.hpp` | Versioned binary format with CRC |
| StateStore | `store.hpp` | Atomic parameter values with format adapter sync |
| StateTree | `state_tree.hpp` | Reactive hierarchical key-value store with JSON serialization |
| StateTree Sync | `state_tree_sync.hpp` | Delta-based binary sync over IPC |
| Undo Manager | `undo_manager.hpp` | Undo/redo with action grouping |

---

## format

> Plugin format adapters — write once, deploy to 9 formats.

| Format | Status | Notes |
|--------|--------|-------|
| AAX | ✓ Usable | Requires developer-supplied SDK |
| AU v2 | ✓ Stable | macOS only, via AudioUnitSDK |
| AU v3 | ✓ Stable | macOS + iOS |
| CLAP | ✓ Stable | First-class with modulation, WebView |
| LV2 | ✓ Usable | Linux plugin format |
| Standalone | ✓ Stable | Desktop app with device selector |
| VST3 | ✓ Stable | Full parameter sync, state, editor resize |
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
| Attributed String | `attributed_string.hpp` | Rich text spans with font, color, weight, decoration |
| Canvas API | `canvas.hpp` | 25+ draw commands — rect, rounded rect, path, arc, text, image |
| CoreGraphics | `platform/mac/cg_canvas.mm` | Native macOS/iOS rendering |
| Effects | `effects.hpp` | Drop shadow, bloom, blur, color adjust |
| Image Convolution | `image_convolution.hpp` | Blur, sharpen, edge detect, emboss kernels |
| Recording Canvas | `recording_canvas.hpp` | Record draw calls for replay/serialization |
| Rectangle List | `rectangle_list.hpp` | Clip regions with add/subtract/intersect |
| Skia Backend | `src/skia_canvas.cpp` | GPU-accelerated via Skia Graphite (Metal/Vulkan/D3D12) |
| SVG | `svg.hpp` | SVG loading and rendering via nanosvg |
| Text Layout | `text_layout.hpp` | Multi-line layout with word wrapping |
| TextShaper | `text_shaper.hpp` | **Measure once, reflow forever** — PreText-style layout engine |

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
| CanvasWidget | Custom drawing via Canvas API |
| CodeEditor | Code editor with line numbers and syntax highlighting |
| ColorPicker | HSV/RGB color selection |
| ComboBox | Dropdown selector |
| ConcertinaPanel | Accordion-style collapsible sections |
| CorrelationMeter | Stereo phase display |
| EqCurveView | Parametric EQ frequency response |
| Fader | Linear slider (vertical/horizontal) |
| FileBrowser | Directory navigation with filters |
| FileTree | Tree-structured filesystem view |
| ImageView | Image display from file |
| Knob | Rotary control with arc, SkSL shaders, Lottie animation |
| Label | Static/dynamic text with styling |
| LassoComponent | Rubber-band marquee selection |
| ListBox | Virtualized scrollable list |
| LiveConstantEditor | Debug overlay for tweaking numeric constants |
| Meter | Audio level meter with peak hold |
| MidiKeyboard | Piano keyboard with note display |
| MultiMeter | Multi-channel meter (any channel count) |
| Panel | Styled container with background/border tokens |
| PresetBrowser | Factory/user presets with search |
| ScrollView | Scrollable viewport |
| SpectrogramView | Scrolling time-frequency heatmap |
| SpectrumView | Frequency spectrum (bars, line, filled) |
| SplashScreen | Borderless window with fade animation |
| SplitView | Resizable split panes |
| TabPanel | Tabbed container |
| TableListBox | Sortable columns with header click-to-sort |
| TextButton | Push button with text label |
| TextEditor | Full-featured with IME, selection, undo |
| Toggle / Checkbox | Boolean on/off controls |
| Toolbar | Horizontal/vertical with buttons, toggles, separators |
| TreeView | Hierarchical tree with expand/collapse |
| WaveformView | Audio waveform display |
| XYPad | 2D parameter surface |

**Layout:** Yoga flexbox + CSS Grid, absolute/fixed positioning, intrinsic sizing.

**Theming:** Token-based design system (`"bg.surface"`, `"control.border"`), contrast-aware, theme presets.

**JS Scripting:** QuickJS (default), JavaScriptCore (Apple), V8 (optional). Hot-reload. Full `WidgetBridge` + `AudioBridge` for parameter access from JS.

**Accessibility:** AccessRole, value/text/table/cell interfaces, VoiceOver (macOS), UIA (Windows), AT-SPI (Linux).

---

## osc

> Open Sound Control messaging.

| Feature | Header | What It Does |
|---------|--------|-------------|
| Address Matching | `bundle.hpp` | Wildcard patterns: `*`, `?`, `[...]`, `{...}` |
| Argument Types | `osc.hpp` | int, float, string, blob |
| OSC Bundle | `bundle.hpp` | Timetag + nested messages per OSC 1.0 |
| OSC Receiver | `osc.hpp` | Receive messages over UDP |
| OSC Sender | `osc.hpp` | Send messages over UDP |

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
| Appcast | Sparkle/WinSparkle update feed generation |
| Code Signing | macOS (`codesign`) and Windows (`signtool`) |
| DMG / PKG | macOS installer creation |
| Linux Packaging | .deb and .tar.gz |
| Notarization | macOS notarization workflow |
| Windows Installer | NSIS-based installer |
