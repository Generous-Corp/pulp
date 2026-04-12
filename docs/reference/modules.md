# Module Reference

Pulp is organized into independent subsystems under `core/`. Each is a separate CMake library (`pulp::runtime`, `pulp::audio`, etc.) that you link as needed.

```cmake
# Link what you need
target_link_libraries(my_plugin PRIVATE pulp::format pulp::signal pulp::view)
```

---

## runtime

Core utilities — the foundation everything else builds on.

**Link:** `pulp::runtime` · **Include prefix:** `<pulp/runtime/...>`

### SIMD — Portable vectorized math

Hardware-accelerated math via Google Highway. Dispatches to the best instruction set at runtime (SSE2, AVX2, NEON). Use for inner-loop DSP where every cycle counts.

```cpp
#include <pulp/runtime/simd.hpp>
using namespace pulp::runtime;

float a[256], b[256], result[256];
simd_add(a, b, result, 256);        // result[i] = a[i] + b[i]
simd_scale(a, 0.5f, result, 256);   // result[i] = a[i] * 0.5
float peak = simd_reduce_max(a, 256);
```

### XML — Parse and generate XML

Wraps pugixml (MIT). Parse from string or file, query with XPath, generate documents.

```cpp
#include <pulp/runtime/xml.hpp>

XmlDocument doc;
doc.parse(R"(<plugin name="MyPlugin"><param>Gain</param></plugin>)");
auto name = doc.root_attribute("name");   // "MyPlugin"
auto params = doc.xpath_strings("//param"); // ["Gain"]
doc.save_file("settings.xml");
```

### HTTP — Network requests

GET, POST, and file download via cpp-httplib (MIT). Use for license checks, cloud presets, update notifications.

```cpp
#include <pulp/runtime/http.hpp>

auto response = http_get("https://api.example.com/presets");
if (response.ok())
    process_presets(response.body);

http_download("https://example.com/ir.wav", "/tmp/impulse.wav");
```

### Crypto — Hashing and encryption

SHA-256, MD5, AES-256-CBC via Mbed TLS (Apache 2.0). Use for license validation, integrity checks, secure preset storage.

```cpp
#include <pulp/runtime/crypto.hpp>

auto hash = sha256_hex("my data");              // 64-char hex string
auto id = machine_id();                          // Deterministic hardware fingerprint

auto encrypted = aes_encrypt(data, size, key32, iv16);
auto decrypted = aes_decrypt(encrypted->data(), encrypted->size(), key32, iv16);
```

### Licensing — Plugin copy protection

RSA-signed license keys with online activation. The public key lives in your plugin; the private key stays on your server.

```cpp
#include <pulp/runtime/license.hpp>

LicenseValidator validator;
validator.set_public_key(my_rsa_public_key_pem);

auto status = validator.validate(license_key_string);
if (status == LicenseStatus::Valid) { /* unlocked */ }
if (status == LicenseStatus::Expired) { /* show renewal dialog */ }

auto info = validator.validate_and_parse(key);
// info->product_id, info->user_email, info->edition
```

### i18n — String translation

Load translations from `.strings` (Apple), `.po` (gettext), or `.json` files. Positional argument substitution with `{0}`, `{1}`.

```cpp
#include <pulp/runtime/i18n.hpp>

LocalisedStrings::instance().load_json_file("lang/de.json");
auto text = tr("hello_user", {{"Max"}});  // "Hallo, Max!"
auto locale = LocalisedStrings::system_locale();  // "de"
```

### Expression — Math evaluator

Parse and evaluate math expressions at runtime. Supports variables, 15+ functions, constants (pi, e). Use for user-defined formulas, automation curves.

```cpp
#include <pulp/runtime/expression.hpp>

auto result = evaluate("sin(pi / 2)");           // 1.0
auto freq = evaluate("base * 2^(note/12)", {{"base", 440.0}, {"note", 7.0}});

ExpressionEvaluator eval;
eval.set("x", 0.5);
eval.evaluate("x * 100 + 10");  // 60.0
```

### Other runtime utilities

| Feature | Header | Description |
|---------|--------|-------------|
| Analytics | `analytics.hpp` | Thread-safe `Analytics::instance().log("preset_load", {{"name", "Init"}})` |
| Base64 | `base64.hpp` | `base64_encode(data)` / `base64_decode(text)` |
| BigInteger | `big_integer.hpp` | Arbitrary-precision math for RSA — `a.mod_pow(exp, modulus)` |
| Child Process | `child_process.hpp` | `run_process("/usr/bin/auval", {"-a"})` with stdout capture |
| Dynamic Library | `dynamic_library.hpp` | `lib.open("plugin.dylib"); lib.find_symbol("entry")` |
| Identity | `identity.hpp` | `Uuid::generate()`, typed SessionId/ObjectId/RunId |
| IP Address | `ip_address.hpp` | `local_ipv4_address()`, `hostname()`, `is_valid_ipv4(addr)` |
| IPC Lock | `inter_process_lock.hpp` | Cross-process mutex via file locks |
| Memory Map | `memory_mapped_file.hpp` | Zero-copy large file access via mmap |
| Named Pipes | `named_pipe.hpp` | Cross-platform IPC (mkfifo / CreateNamedPipe) |
| Primes | `primes.hpp` | `is_prime(97)`, `generate_prime(32)`, `sieve_primes(1000)` |
| Range | `range.hpp` | `Range<float>(0, 1).contains(0.5)`, intersection, union |
| Scope Guard | `scope_guard.hpp` | `PULP_ON_SCOPE_EXIT(file.close())` |
| Sockets | `socket.hpp` | TCP/UDP client and server for networked audio |
| System Info | `system.hpp` | CPU model, core count, RAM, OS, SIMD features (runtime detected) |
| Temp File | `temporary_file.hpp` | Auto-deleting temp file — `TemporaryFile tmp(".wav")` |
| Text Diff | `text_diff.hpp` | Line-by-line diff with formatted +/- output |
| Timer | `high_resolution_timer.hpp` | Sub-millisecond periodic callback on a dedicated thread |
| ZIP/GZIP | `zip.hpp` | Compress/decompress data and archives (miniz) |

---

## events

Event loop, timers, IPC, and process management.

**Link:** `pulp::events` · **Include prefix:** `<pulp/events/...>`

### IPC — Inter-process communication

Length-prefixed messages over named pipes or TCP sockets. Use for crash-isolated plugin scanning, multi-process architectures, standalone↔plugin communication.

```cpp
#include <pulp/events/interprocess_connection.hpp>

// Server side
InterprocessConnection server;
server.on_text_message = [](std::string_view msg) { handle(msg); };
server.create_server("my_pipe", IpcTransport::NamedPipe);

// Client side
InterprocessConnection client;
client.connect("my_pipe", IpcTransport::NamedPipe);
client.send_message("scan_plugin:/path/to/plugin.vst3");
```

### Child Process Pool — Crash-isolated workers

Launch child processes with IPC channels. If a child crashes (e.g., while loading a broken plugin), the host survives.

```cpp
#include <pulp/events/child_process_manager.hpp>

ChildProcessManager pool;
auto* worker = pool.launch("/usr/bin/my-scanner", {"--plugin", path});
worker->send_message("scan");
worker->on_message = [](std::string_view result) { update_list(result); };
```

### Other event utilities

| Feature | Header | Description |
|---------|--------|-------------|
| Action Broadcaster | `async_updater.hpp` | `broadcaster.send_action("file_open")` to all listeners |
| Async Updater | `async_updater.hpp` | Coalesce rapid cross-thread triggers into one callback |
| Event Loop | `event_loop.hpp` | `EventLoop loop; loop.dispatch([]{...}); loop.dispatch_after(100ms, []{...})` |
| Service Discovery | `volume_detector.hpp` | mDNS/Bonjour browsing for networked audio devices |
| Timer | `timer.hpp` | `Timer t; t.start(100ms, []{...})` — periodic or one-shot |
| Volume Detector | `volume_detector.hpp` | Poll for USB drive mount/unmount events |

---

## audio

Device I/O, file formats, channel layouts, and offline processing.

**Link:** `pulp::audio` · **Include prefix:** `<pulp/audio/...>`

### Reading and writing audio files

The `FormatRegistry` handles codec dispatch. Pass a file path — it picks the right decoder from the extension.

```cpp
#include <pulp/audio/format_registry.hpp>

// Read any supported format
auto data = FormatRegistry::instance().read("drums.flac");
// data->channels[0] = left channel floats, data->sample_rate = 44100

// Write to WAV
FormatRegistry::instance().write("output.wav", *data);

// Read just the metadata (no audio decode)
auto info = FormatRegistry::instance().read_info("song.mp3");
// info->duration_seconds, info->num_channels, info->sample_rate
```

### Streaming write — large files without loading into memory

```cpp
#include <pulp/audio/streaming_writer.hpp>

StreamingWriter writer;
writer.open("recording.wav", 48000, 2, 24);  // 48kHz stereo 24-bit

while (recording) {
    writer.write_frames(buffer, num_frames);  // Write in chunks
}
writer.close();  // Finalizes WAV header
```

### Audio device I/O

```cpp
#include <pulp/audio/device.hpp>

auto system = create_audio_system();  // Platform-appropriate backend
auto devices = system->enumerate_devices();

DeviceConfig config;
config.sample_rate = 48000;
config.buffer_size = 256;

auto device = system->create_device(devices[0].id);
device->open(config);
device->start([](const auto& input, auto& output, const auto& ctx) {
    // Real-time audio callback — no allocation, no locks
    process(input, output, ctx.buffer_size);
});
```

**Device backends:** CoreAudio (macOS), WASAPI (Windows), ALSA + JACK (Linux), Web Audio (browser)

### Audio file format support

| Format | Read | Write | Backend |
|--------|:----:|:-----:|---------|
| AAC | ✓ | ✓* | ExtAudioFile (macOS) / FDK AAC (`pulp add fdk-aac --accept-license FDK-AAC`) |
| AIFF / AIFF-C | ✓ | ✓ | Native (8/16/24/32-bit big-endian) |
| ALAC | ✓ | ✓* | ExtAudioFile (macOS) / Apple ALAC (`pulp add alac`) |
| FLAC | ✓ | ✓* | dr_flac / libflac (`pulp add libflac`) |
| MP3 | ✓ | ✓* | dr_mp3 / LAME (`pulp add lame --accept-license LGPL-2.0`) |
| OGG Vorbis | ✓ | — | stb_vorbis |
| WAV | ✓ | ✓ | CHOC + StreamingWriter |

*\*Write via optional `pulp add` packages. Permissive (libflac, ALAC) install freely. Copyleft (LAME, fdk-aac) require `--accept-license`.*

### Other audio features

| Feature | Header | Description |
|---------|--------|-------------|
| Buffering Reader | `buffering_reader.hpp` | Ring buffer with background read thread for streaming |
| Channel Sets | `channel_set.hpp` | `ChannelSet::surround_5_1()`, mono through 7.1.4 Atmos |
| Load Measurer | `load_measurer.hpp` | Track CPU usage of your audio callback |
| Memory-Mapped Reader | `mmap_reader.hpp` | Zero-copy access for large sample libraries |
| Offline Processor | `offline_processor.hpp` | `offline_process(input, callback, 512)` — batch render |
| Subsection Reader | `subsection_reader.hpp` | Read frame range without copying — `reader.sample(ch, frame)` |
| System Volume | `system_volume.hpp` | `get_system_volume()` / `set_system_volume(0.8f)` |

---

## midi

MIDI I/O, file handling, and MIDI 2.0 support.

**Link:** `pulp::midi` · **Include prefix:** `<pulp/midi/...>`

### MIDI message sequence — editing and offline processing

```cpp
#include <pulp/midi/midi_message_sequence.hpp>

MidiMessageSequence seq;
seq.add_note_on(0.0, 0, 60, 100);   // C4 at time 0
seq.add_note_off(0.5, 0, 60);       // Release after 0.5s
seq.add_cc(0.25, 0, 1, 64);         // Modulation at 0.25s

auto events = seq.events_in_range(0.0, 1.0);  // All events in first second
auto off = seq.find_note_off(0);               // Find matching note-off
```

### MIDI CI — Device discovery and profiles

```cpp
#include <pulp/midi/midi_ci.hpp>

CiDiscovery ci;
ci.on_device_discovered = [](const CiDeviceInfo& device) {
    log("Found: MUID=" + std::to_string(device.muid.value));
};

auto inquiry = ci.create_discovery_inquiry();
send_sysex(inquiry);  // Send over MIDI port
```

### Other MIDI features

| Feature | Header | Description |
|---------|--------|-------------|
| Buffer | `midi_buffer.hpp` | Timestamped event buffer for `process()` callbacks |
| Device I/O | platform/ | CoreMIDI (macOS), WinMIDI (Windows), ALSA (Linux), Web MIDI |
| Files | `midi_file.hpp` | Read/write Standard MIDI Files |
| Messages | via CHOC | `ShortMessage::noteOn(0, 60, 100)` |
| UMP | `ump.hpp` | MIDI 2.0 Universal MIDI Packets, MPE zones |

---

## signal

30+ real-time-safe DSP processors. All operate on single samples or buffers. All are safe for the audio thread.

**Link:** `pulp::signal` · **Include prefix:** `<pulp/signal/...>`

### Using a processor

Every processor follows the same pattern: configure, set sample rate, process.

```cpp
#include <pulp/signal/compressor.hpp>

Compressor comp;
comp.set_params({.threshold_db = -20, .ratio = 4, .attack_ms = 5, .release_ms = 100});
comp.set_sample_rate(48000);

for (int i = 0; i < num_samples; ++i)
    buffer[i] = comp.process(buffer[i]);
```

### Applying a mono processor to stereo

```cpp
#include <pulp/signal/processor_duplicator.hpp>

ProcessorDuplicator<Compressor> stereo_comp;
stereo_comp.prepare(2, 48000);
stereo_comp.for_each([](Compressor& c) { c.set_params({...}); });
stereo_comp.process(channels, 2, num_samples);
```

### Dry/wet mixing with latency compensation

```cpp
#include <pulp/signal/dry_wet_mixer.hpp>

DryWetMixer mixer;
mixer.set_mix(0.7f);                    // 70% wet
mixer.set_curve(MixCurve::EqualPower);  // Constant-power crossfade
mixer.set_wet_latency(512);             // Compensate 512 samples of plugin latency
mixer.prepare(2, 1024);

mixer.push_dry(input_channels, 2, num_frames);
// ... run your effect on the wet path ...
mixer.mix_wet(output_channels, 2, num_frames);
```

### Convolution — load an impulse response

```cpp
#include <pulp/signal/convolver.hpp>

PartitionedConvolver conv;
conv.init(impulse_response.data(), ir_length, block_size);

// In your process callback:
conv.process(input, output, block_size);
```

### Available processors

#### Filters

| Processor | Header | Description |
|-----------|--------|-------------|
| Biquad | `biquad.hpp` | Second-order IIR filter — low/high/band-pass, notch, shelf, peaking EQ |
| Filter Design | `filter_design.hpp` | Generate Butterworth and Chebyshev coefficient sets for arbitrary order |
| FIR | `fir_filter.hpp` | Finite impulse response filter with arbitrary tap count for linear-phase EQ |
| Ladder | `ladder_filter.hpp` | 4-pole Moog-style resonant filter with saturation — classic analog synth sound |
| Linkwitz-Riley | `linkwitz_riley.hpp` | Phase-aligned crossover filter for splitting audio into frequency bands |
| State Variable (TPT) | `svf.hpp` / `tpt_filter.hpp` | Topology-preserving transform filter — simultaneous LP/HP/BP/notch outputs |

#### Effects

| Processor | Header | Description |
|-----------|--------|-------------|
| Chorus | `chorus.hpp` | Modulated delay for stereo widening and detuning effects |
| Convolver | `convolver.hpp` | Partitioned frequency-domain convolution for reverb impulse responses |
| Delay Line | `delay_line.hpp` | Sample-accurate delay with linear, cubic, or sinc interpolation |
| Oversampling | `oversampling.hpp` | 2x/4x/8x up/downsampling with anti-alias filtering for nonlinear processing |
| Phaser | `phaser.hpp` | All-pass filter chain with LFO modulation for sweeping comb effects |
| Reverb | `reverb.hpp` | Algorithmic stereo reverb with room size, damping, and width controls |
| Waveshaper | `waveshaper.hpp` | Static nonlinear distortion via transfer function (tanh, soft clip, custom) |

#### Dynamics

| Processor | Header | Description |
|-----------|--------|-------------|
| Ballistics Filter | `ballistics_filter.hpp` | Envelope follower with configurable attack/release for meter and dynamics |
| Compressor | `compressor.hpp` | Soft-knee downward compressor with threshold, ratio, attack, release |
| DryWetMixer | `dry_wet_mixer.hpp` | Parallel mix with latency compensation — equal-power or linear crossfade |
| Gain | `gain.hpp` | Smoothed gain stage that ramps between values to avoid clicks |
| Noise Gate | `noise_gate.hpp` | Silence signals below threshold with hysteresis to avoid chatter |

#### Generators and analysis

| Processor | Header | Description |
|-----------|--------|-------------|
| ADSR | `adsr.hpp` | Attack-decay-sustain-release envelope generator for amplitude or filter modulation |
| FFT | `fft.hpp` | Fast Fourier Transform — uses vDSP on Apple, fallback on other platforms |
| Multi-Channel Meter | `multi_channel_meter.hpp` | Peak and RMS level measurement across multiple channels |
| Oscillator | `oscillator.hpp` | Wavetable oscillator with sine, saw, square, triangle waveforms |
| Spectrogram | `spectrogram.hpp` | Rolling time-frequency analysis for visual display of spectral content |
| STFT | `stft.hpp` | Short-time Fourier Transform for overlap-add spectral processing |
| Windowing | `windowing.hpp` | Hann, Hamming, Blackman, Kaiser window functions for FFT analysis |

#### Math and utilities

| Processor | Header | Description |
|-----------|--------|-------------|
| Bias | `bias.hpp` | Shift a signal's DC offset — useful for asymmetric waveshaping |
| Fast Math | `fast_math.hpp` | Approximations of sin, cos, tanh, exp for inner loops where precision is traded for speed |
| Interpolator | `interpolator.hpp` | Lagrange and Hermite interpolation for fractional-sample delay and resampling |
| Log Ramped Value | `log_ramped_value.hpp` | Logarithmic smoothing for perceptually linear parameter transitions |
| Lookup Table | `lookup_table.hpp` | Pre-computed function table for fast repeated evaluation of expensive functions |
| Matrix | `matrix.hpp` | 2×2 through 4×4 matrix math for mid/side encoding, rotation, spatial processing |
| Panner | `panner.hpp` | Stereo and surround panning with equal-power or linear law |
| Polynomial Math | `poly_math.hpp` | Polynomial evaluation and Horner's method for waveshaper transfer functions |
| Processor Chain | `processor_chain.hpp` | Connect multiple processors in series — automatic prepare/process forwarding |
| SIMD Buffer | `simd_buffer.hpp` | Aligned memory buffer for SIMD-safe block processing |
| Smoothed Value | `smoothed_value.hpp` | Linear or exponential parameter smoothing to prevent zipper noise |
| Special Functions | `special_functions.hpp` | sinc, Bessel, dB↔linear, MIDI note↔frequency conversions |

---

## state

Parameters, state trees, presets, and settings.

**Link:** `pulp::state` · **Include prefix:** `<pulp/state/...>`

### Plugin parameters — the core state system

```cpp
#include <pulp/state/store.hpp>

StateStore store;
auto gain_id = store.add_param({.name = "Gain", .min = -60, .max = 12, .default_val = 0});
auto mix_id = store.add_param({.name = "Mix", .min = 0, .max = 1, .default_val = 1});

// Audio thread reads atomically (no locks)
float gain = store.get(gain_id);

// UI thread writes with gesture grouping for undo
store.begin_gesture(gain_id);
store.set(gain_id, -6.0f);
store.end_gesture(gain_id);
```

### StateTree — reactive hierarchical state

Like a JSON document that notifies you when anything changes. Use for complex plugin state beyond flat parameters.

```cpp
#include <pulp/state/state_tree.hpp>

auto root = StateTree::create("synth");
root->set("name", std::string("My Patch"));
root->set("polyphony", int64_t(8));

auto osc = StateTree::create("oscillator");
osc->set("waveform", std::string("saw"));
osc->set("detune", 7.0);
root->add_child(osc);

root->add_listener([](StateTree& node, std::string_view prop, auto&, auto& new_val) {
    log(std::string(prop) + " changed");
});

std::string json = root->to_json();           // Serialize
auto restored = StateTree::from_json(json);   // Deserialize
```

### Persistent settings

```cpp
#include <pulp/state/properties_file.hpp>

PropertiesFile settings;
settings.load("~/.config/MyPlugin/settings.json");
settings.set_string("theme", "dark");
settings.set_int("buffer_size", 512);
settings.save();

// Or use platform-standard paths automatically:
ApplicationProperties app("MyPlugin");
app.load();
app.user_settings().set_bool("first_run", false);
app.save();
```

### Other state features

| Feature | Header | Description |
|---------|--------|-------------|
| Binding | `binding.hpp` | Connect UI widget ↔ parameter with undo gesture grouping |
| Cached Property | `cached_property.hpp` | `CachedProperty<double> freq(tree, "freq", 440.0)` — auto-updates |
| Preset Manager | `preset_manager.hpp` | Factory/user presets, next/prev navigation, import/export |
| StateTree Sync | `state_tree_sync.hpp` | Binary delta sync over IPC for multi-process state |
| Undo Manager | `undo_manager.hpp` | `undo_mgr.perform(action)` / `undo_mgr.undo()` |

---

## format

Plugin format adapters — write your plugin once, deploy to 9 formats.

**Link:** `pulp::format` · **Include prefix:** `<pulp/format/...>`

### Writing a plugin

```cpp
#include <pulp/format/processor.hpp>

class MyPlugin : public Processor {
public:
    PluginDescriptor descriptor() const override {
        return {.name = "MyGain", .vendor = "MyCompany", .uid = "com.myco.gain"};
    }
    
    void define_parameters(state::StateStore& store) override {
        gain_id_ = store.add_param({.name = "Gain", .min = -60, .max = 12});
    }
    
    void prepare(const PrepareContext& context) override {}
    
    void process(audio::BufferView<float>& audio_output,
                 const audio::BufferView<const float>& audio_input,
                 midi::MidiBuffer& midi_in, midi::MidiBuffer& midi_out,
                 const ProcessContext& ctx) override {
        float gain = db_to_linear(store().get(gain_id_));
        for (int ch = 0; ch < audio_output.num_channels(); ++ch)
            for (int i = 0; i < audio_output.num_frames(); ++i)
                audio_output[ch][i] = audio_input[ch][i] * gain;
    }
};
```

This single class automatically works as VST3, AU, CLAP, LV2, AAX, Standalone, WAM, and WCLAP.

### Supported formats

| Format | Status | Notes |
|--------|--------|-------|
| AAX | ✓ Usable | Requires developer-supplied Avid SDK |
| AU v2 | ✓ Stable | macOS only, via AudioUnitSDK |
| AU v3 | ✓ Stable | macOS + iOS |
| CLAP | ✓ Stable | First-class — modulation, WebView, note expressions |
| LV2 | ✓ Usable | Linux plugin format |
| Standalone | ✓ Stable | Desktop app with audio settings, test signal |
| VST3 | ✓ Stable | Full parameter sync, state, editor resize |
| WAM | ✓ Experimental | Web Audio Module (browser) |
| WCLAP | ✓ Experimental | Web CLAP (browser) |

### Other format features

| Feature | Header | Description |
|---------|--------|-------------|
| ARA | `ara.hpp` | Audio Random Access document controller (stub) |
| Host Detection | `host_type.hpp` | `detect_host_type()` → Logic, Reaper, Ableton, etc. |
| Settings Panel | `settings_panel.hpp` | Audio/MIDI device selector with test signal and meters |

---

## canvas

2D drawing with GPU acceleration and smart text layout.

**Link:** `pulp::canvas` · **Include prefix:** `<pulp/canvas/...>`

### Drawing

```cpp
void MyWidget::paint(Canvas& canvas) {
    // Background
    canvas.set_fill_color(Color::rgba(30, 30, 40));
    canvas.fill_rounded_rect(0, 0, width, height, 8);
    
    // Gradient fill
    canvas.set_fill_gradient(LinearGradient{0, 0, 0, height, 
        Color::rgba(80, 120, 255), Color::rgba(40, 60, 180)});
    canvas.fill_rect(10, 10, width - 20, 4);
    
    // Text
    canvas.set_fill_color(Color::rgba(220, 220, 230));
    canvas.set_font("Inter", 14);
    canvas.fill_text("Hello Pulp", 10, 30);
    
    // Image
    canvas.draw_image_from_file("icon.png", x, y, 32, 32);
}
```

**Backends:** Skia Graphite (GPU — Metal/Vulkan/D3D12) or CoreGraphics (macOS/iOS native).

### TextShaper — measure once, reflow forever

Inspired by [Cheng Lou's PreText](https://github.com/chenglou/pretext). The expensive text measurement runs once; resizing uses just arithmetic.

```cpp
#include <pulp/canvas/text_shaper.hpp>

TextShaper shaper;
auto prepared = shaper.prepare("Long text that wraps...", "Inter", 14);

// On every resize — pure arithmetic, no font calls:
auto layout = shaper.layout(prepared, container_width);
// layout.line_count, layout.total_height, layout.lines[i].text

float height = shaper.measure_height(prepared, 300.0f);  // Fast
```

**CMake option:** `PULP_TEXT_SHAPING` — ON with GPU (real HarfBuzz metrics via Skia), OFF without (character-width estimation). Same API either way.

### Other canvas features

| Feature | Header | Description |
|---------|--------|-------------|
| Attributed String | `attributed_string.hpp` | Rich text spans — mixed font, color, weight per range |
| Effects | `effects.hpp` | Drop shadow, bloom, blur, color adjustment |
| Image Convolution | `image_convolution.hpp` | `ImageConvolutionKernel::gaussian_blur_5().apply(pixels, w, h)` |
| Rectangle List | `rectangle_list.hpp` | Clip regions with add/subtract/intersect for dirty tracking |
| SVG | `svg.hpp` | Load and render SVG vector graphics via nanosvg |
| SDF text | `sdf_atlas.hpp` | Single-channel signed distance field glyph atlas for resolution-independent GPU text. See [docs/reference/sdf-text.md](./sdf-text.md). |
| MSDF text | `msdf_atlas.hpp` | Multi-channel SDF atlas with `median(r,g,b)` sampler for sharp corners at extreme zoom (in-house median-of-three generator). |
| PSDF text | `psdf_atlas.hpp` | Pseudo-SDF variant with vector-fallback helper for extreme zoom. |
| SDF effects | `sdf_effects.hpp` | Design-token presets for outline / shadow / glow / bevel over any SDF atlas (SkSL effect module). |
| SDF atlas cache | `sdf_atlas_cache.hpp` | Frame-based LRU glyph sharing across `fill_text_sdf` call-sites with dirty-rect upload hints. |
| Path → SDF | `path_to_sdf.hpp` | Runtime EDT of a binary mask to produce an SDF for procedural shapes (Phase 5). |

---

## view

Full widget toolkit with CSS-inspired layout and JS scripting.

**Link:** `pulp::view` · **Include prefix:** `<pulp/view/...>`

### Creating a UI

```cpp
#include <pulp/view/widgets.hpp>

auto root = std::make_unique<Panel>();
root->set_background_token("bg.surface");

auto knob = std::make_unique<Knob>();
knob->set_label("Gain");
knob->set_value(0.5f);
knob->on_change = [&](float v) { store.set(gain_id, v); };
root->add_child(std::move(knob));

auto meter = std::make_unique<Meter>();
meter->set_orientation(Meter::Orientation::vertical);
root->add_child(std::move(meter));
```

### Available widgets (30+)

#### Controls

| Widget | Description |
|--------|-------------|
| Checkbox | Boolean on/off control rendered as a checkmark box |
| ComboBox | Drop-down menu for selecting one option from a list |
| Fader | Vertical or horizontal slider for continuous parameter control |
| Knob | Rotary control for parameters like gain, frequency, resonance |
| TextButton | Clickable button with a text label — supports toggle mode |
| TextEditor | Single or multi-line text input with selection, copy/paste, undo |
| Toggle | Two-state switch control for enabling/disabling features |

#### Containers

| Widget | Description |
|--------|-------------|
| ConcertinaPanel | Accordion-style stacked panels — expand one section, collapse others |
| Panel | Basic container with optional background, border, and layout |
| ScrollView | Scrollable viewport for content larger than the visible area |
| SplitView | Resizable split between two child views with a draggable divider |
| TabPanel | Tabbed container — switch between child views via tab bar |
| Toolbar | Horizontal or vertical bar of buttons, toggles, separators, and custom views |

#### Data display

| Widget | Description |
|--------|-------------|
| Breadcrumb | Navigation trail showing the current location in a hierarchy |
| FileBrowser | File system browser with navigation, filtering, and selection |
| FileTree | Hierarchical file/folder tree with expand/collapse |
| Label | Static text display with font, color, and alignment options |
| ListBox | Scrollable list of selectable items with virtual rendering for large datasets |
| PropertyList | Two-column key/value editor for inspector-style property panels |
| TableListBox | Sortable, scrollable table with column headers and row selection |
| TreeView | Hierarchical data display with expand/collapse and lazy loading |

#### Audio visualization

| Widget | Description |
|--------|-------------|
| CorrelationMeter | Displays stereo phase correlation from -1 (out of phase) to +1 (mono) |
| EqCurveView | Interactive frequency response curve for parametric EQ — drag handles to edit bands |
| Meter | Peak/RMS level meter with configurable ballistics and clip indicators |
| MultiMeter | Multiple level meters side-by-side for multi-channel monitoring |
| SpectrogramView | Rolling time-frequency heatmap showing spectral content over time |
| SpectrumView | Real-time frequency spectrum analyzer with configurable FFT size |
| WaveformView | Audio waveform display with zoom, selection, and playhead |
| XYPad | Two-dimensional control surface for parameters like pan/width or filter freq/res |

#### Specialized

| Widget | Description |
|--------|-------------|
| CanvasWidget | Custom-drawn view — override `paint()` to draw anything with the Canvas API |
| CodeEditor | Syntax-highlighted text editor with line numbers, designed for script editing |
| ColorPicker | HSV color selector with hue ring, saturation/value square, and hex input |
| FileDropZone | Drop target that accepts files dragged from the OS file manager |
| ImageView | Display raster images (PNG, JPEG) with optional scaling and aspect ratio |
| LassoComponent | Rubber-band marquee selection tool for selecting multiple items by dragging |
| LiveConstantEditor | In-app slider overlay for tweaking magic numbers during development |
| MidiKeyboard | Interactive piano keyboard — click to play notes, highlight active voices |
| PresetBrowser | Factory/user preset list with next/prev navigation and search |
| SplashScreen | Timed overlay window for branding or loading screens on app startup |
| SpriteStrip | Filmstrip-based control rendered from a sprite sheet image |
| WaveformEditor | Editable waveform display for drawing custom oscillator shapes |

### Layout — Yoga flexbox + CSS Grid

```cpp
// Flexbox
root->style().set_flex_direction(FlexDirection::Row);
root->style().set_gap(8);

knob->style().set_flex_grow(1);
meter->style().set_width(40);
meter->style().set_height_percent(100);
```

### Theming — design tokens

```cpp
Theme theme;
theme.set("bg.surface", Color::rgba(25, 25, 35));
theme.set("control.accent", Color::rgba(80, 130, 255));
theme.set("text.primary", Color::rgba(220, 220, 230));

// Widgets resolve tokens automatically:
canvas.set_fill_color(resolve_color("bg.surface"));
```

### JS-scripted UI

Write your entire plugin UI in JavaScript with hot-reload during development:

```javascript
// plugin-ui.js
const knob = new Knob({ label: "Gain", param: "gain" });
const meter = new Meter({ orientation: "vertical" });
document.body.append(knob, meter);
```

**Engines:** QuickJS (default, lightweight), JavaScriptCore (Apple, fast JIT), V8 (full ES2024)

### Accessibility

VoiceOver (macOS + iOS), UIA (Windows), AT-SPI (Linux). Widgets declare their role and Pulp maps to platform accessibility APIs.

```cpp
knob->set_access_role(AccessRole::slider);
knob->set_access_label("Gain");
knob->set_access_value("-6 dB");
```

---

## osc

Open Sound Control messaging for networked audio control.

**Link:** `pulp::osc` · **Include prefix:** `<pulp/osc/...>`

```cpp
#include <pulp/osc/osc.hpp>

// Send
Sender sender;
sender.connect("192.168.1.100", 9000);
sender.send("/synth/freq", 440.0f);
sender.send("/synth/gate", 1);

// Receive
Receiver receiver;
receiver.bind(9000);
receiver.on_message = [](const Message& msg) {
    if (msg.address == "/synth/freq")
        set_frequency(msg.args[0].as_float());
};
```

Supports bundles with timetags and address pattern matching (`*`, `?`, `[...]`, `{a,b}`).

---

## render

GPU surface management — you rarely use this directly. Canvas and View handle it.

| Feature | What It Does |
|---------|-------------|
| Dawn/WebGPU | Cross-platform GPU abstraction (Metal, Vulkan, D3D12, OpenGL) |
| GPU Compute | Experimental batch processing for >64K element workloads |
| Skia Graphite | 2D rendering on top of Dawn — what Canvas uses internally |

---

## ship

Packaging and distribution — from code signing to installer to update feed.

**Link:** `pulp::ship` · **Include prefix:** `<pulp/ship/...>`

```bash
# Sign all plugin bundles
pulp ship sign --identity "Developer ID Application: My Company"

# Package — creates .pkg (macOS) or NSIS installer (Windows)
pulp ship package --version 1.2.0

# Check signing status
pulp ship check
```

| Feature | What It Does |
|---------|-------------|
| Code Signing | macOS `codesign` + Windows `signtool` |
| DMG / PKG | macOS installer creation |
| Linux Packaging | `.deb` and `.tar.gz` |
| Notarization | macOS notarization with `notarytool` |
| Signing Check | Verify signing status of all built plugin bundles |
| Windows Installer | NSIS-based with optional Authenticode |
