# Module Reference

Pulp is organized into independent subsystems under `core/`. Each is a separate CMake library (`pulp::runtime`, `pulp::audio`, etc.) that you link as needed.

```cmake
# Link what you need
target_link_libraries(my_plugin PRIVATE pulp::format pulp::signal pulp::view)
```

---

## platform

Platform detection and operating-system services that higher-level subsystems
use without hard-coding macOS, Windows, Linux, iOS, or Web behavior.

**Link:** `pulp::platform` · **Include prefix:** `<pulp/platform/...>`

| Feature | Header | What It Does |
|---------|--------|--------------|
| Platform detection | `detect.hpp` | Compile-time and runtime platform checks |
| Native handles | `native_handle.hpp` | Typed OS window/device handles for adapters |
| Clipboard | `clipboard.hpp` | Cross-platform text clipboard access where implemented |
| File dialogs | `file_dialog.hpp` | Native open/save panels where implemented |
| Popup menus | `popup_menu.hpp` | Native menu affordances for UI surfaces |

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

### Streams — Unified byte I/O

One interface (`pulp::runtime::Stream`) for files, memory, pipes, TCP, and HTTP. See [docs/reference/streams.md](./streams.md) for the full API.

```cpp
#include <pulp/runtime/stream.hpp>
#include <pulp/runtime/async_stream.hpp>

// Synchronous file I/O via the common interface.
FileStream f("preset.bin", FileStream::Mode::Read);
std::uint8_t buf[512]{};
auto r = f.read(buf, sizeof(buf));   // StreamResult{bytes, error}

// Wrap any Stream in AsyncStream for backpressure + cancellation.
AsyncStream async(std::make_unique<FileStream>("large.wav"));
async.on_data([](auto* data, auto n) { /* on worker thread */ });
async.start();
```

### Budget Policy — Graceful degradation

`pulp::runtime::evaluate_runtime_budget()` gives background analysis, cache
refresh, validation helpers, and game-audio-style optional work a shared
run/defer/shed/bypass decision. Critical audio work always runs; interactive
work can defer to preserve reserve; background and opportunistic work can shed
or bypass when overload is active or budget is exhausted.

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

Current license keys are v2 AES-256-GCM payloads validated with a 32-byte
shared secret. Legacy v1 RSA-signed keys remain supported for migration and
compatibility. Online activation returns the license key string that your
plugin then validates locally.

```cpp
#include <pulp/runtime/license.hpp>

LicenseValidator validator;
validator.set_shared_secret(shared_secret_32, 32);

auto status = validator.validate(license_key_string);
if (status == LicenseStatus::Valid) { /* unlocked */ }
if (status == LicenseStatus::Expired) { /* show renewal dialog */ }

auto info = validator.validate_and_parse(key);
// info->product_id, info->user_email, info->edition

// Legacy v1 keys can still be validated with the RSA public key.
validator.set_public_key(my_rsa_public_key_pem);
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
| Analytics | `analytics.hpp` | Thread-safe `Analytics::instance().log_event("preset_load", {{"name", "Init"}})` |
| Base64 | `base64.hpp` | `base64_encode(data)` / `base64_decode(text)` |
| BigInteger | `big_integer.hpp` | Arbitrary-precision math for RSA — `a.mod_pow(exp, modulus)` |
| Child Process | `child_process.hpp` | `run_process("/usr/bin/auval", {"-a"})` with stdout capture |
| Dynamic Library | `dynamic_library.hpp` | `lib.open("plugin.dylib"); lib.find_symbol("entry")` |
| Identity | `identity.hpp` | `Uuid::generate()`, typed SessionId/ObjectId/RunId |
| IP Address | `ip_address.hpp` | `local_ipv4_address()`, `hostname()`, `is_valid_ipv4(addr)` |
| IPC Lock | `inter_process_lock.hpp` | Cross-process mutex via file locks |
| Memory Map | `memory_mapped_file.hpp` | Zero-copy large file access via mmap |
| Named Pipes | `named_pipe.hpp` | Cross-platform IPC (directional paired POSIX FIFOs / CreateNamedPipe) |
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
| Action Broadcaster | `coalesced_updater.hpp` | `broadcaster.send_action("file_open")` to all listeners |
| Async Updater | `coalesced_updater.hpp` | Coalesce rapid cross-thread triggers into one callback |
| Event Loop | `event_loop.hpp` | `EventLoop loop; loop.dispatch([]{...}); loop.dispatch_after(100ms, []{...})` |
| Service Discovery | `service_discovery.hpp` | `ServicePublisher` / `ServiceBrowser` use a platform default backend when available; use `NetworkServiceDiscovery::install_backend()` for custom or test backends |
| Timer | `timer.hpp` | `Timer timer(loop, 100ms, []{...}); timer.start();` — periodic or one-shot |
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

### Sampler, looper, and analysis primitives

Reusable low-level pieces for building samplers, generated-audio freeze/loop workflows, waveform displays, and offline/background sample analysis. These are primitives, not a full sampler UI. Callback-safe operations are documented in `rt_safety_contract.hpp`; import/export, analysis, waveform thumbnail build, publication writes, and materialization stay off the audio callback.

| Feature | Headers | Description |
|---------|---------|-------------|
| Stream handoff and rolling capture | `audio_stream_handoff.hpp`, `planar_audio_ring_buffer.hpp`, `rolling_audio_capture_buffer.hpp`, `realtime_sample_recorder.hpp` | Bridge generated/live/model audio into host-paced processing, keep bounded rolling history, freeze stable windows, and materialize captures off the audio thread |
| Sample publication and storage | `published_sample_store.hpp`, `sample_slot_bank.hpp`, `sample_slot_materializer.hpp`, `sample_pool.hpp`, `sample_stream_window.hpp` | Publish captured/imported/rendered samples through generation-safe views, fixed slot banks, stable sample IDs, and resident streaming pages |
| Looping and playback | `loop_types.hpp`, `loop_reader.hpp`, `loop_renderer.hpp`, `loop_point_analyzer.hpp`, `sample_voice_renderer.hpp`, `voice_sum_mixer.hpp` | Render one-shots, forward/reverse loops, fades/crossfades, loop-point assistance, scalar sample voices, and summed voice scratch buffers |
| Mapping and instrument policy | `sample_zone_map.hpp`, `sample_key_map.hpp`, `instrument_runtime.hpp`, `instrument_voice_allocator.hpp`, `instrument_envelope.hpp`, `voice_modulation_buffer.hpp` | Represent key/velocity zones, chromatic/fixed-pitch/slice mappings, pool-backed trigger resolution, voice allocation, AHDSR envelopes, and per-voice modulation lanes |
| Editing, import/export, and bounce metadata | `sample_edit_document.hpp`, `sample_asset_io.hpp`, `wav_metadata.hpp` | Track non-destructive edit intent, import/export policy, drop classification, and WAV metadata/interchange outside realtime paths |
| Onset, slice, key/tempo, and transient analysis | `onset_detector.hpp`, `slice_point_analyzer.hpp`, `slice_map.hpp`, `analyzer_provider.hpp`, `built_in_key_tempo_analyzer.hpp`, `built_in_transient_classifier.hpp` | Provide package-free fallback analysis plus neutral provider/provenance metadata for future package-backed MIR adapters |
| Time/pitch extension point | `analyzer_provider.hpp`, `signalsmith_time_pitch_processor.hpp` | Optional package-backed time-stretch/pitch-shift processor contract; availability and licensing stay explicit |
| Waveform summaries and render backends | `waveform_overview.hpp`, `waveform_gpu_primitives.hpp`, `waveform_gpu_render_controller.hpp`, `waveform_headless_render_backend.hpp` | Build/cache serialized CPU waveform summaries, plan generation-keyed static layer uploads, exercise backend resource lifecycle in CPU/headless paths, and keep future GPU-assisted analysis/rendering off live audio-thread waits |
| Realtime contract labels | `rt_safety_contract.hpp` | Machine-checkable sampler/looper RT-safety labels for representative hot paths and off-thread helpers |

### Other audio features

| Feature | Header | Description |
|---------|--------|-------------|
| Buffering Reader | `buffering_reader.hpp` | Ring buffer with background read thread for streaming |
| Channel Sets | `channel_set.hpp` | `ChannelSet::surround_5_1()`, mono through 7.1.4 Atmos |
| Load Measurer | `load_measurer.hpp` | Track CPU usage of your audio callback; `evaluate_audio_runtime_overload()` classifies process-load/xrun telemetry into nominal, watch, overloaded, or critical validation states with explicit shed/bypass guidance |
| Memory-Mapped Reader | `mmap_reader.hpp` | Zero-copy access for large sample libraries |
| Offline Processor | `offline_processor.hpp` | `offline_process(input, callback, 512)` for simple batch render; `offline_render(input, callback, options)` for deterministic block schedules, absolute sample positions, transport timeline, state generation, render-speed hints, render seeds, and explicit tail policy; `offline_render_stems()` extracts named channel groups; `compare_offline_render_audio()` reports golden/null residuals; `create_offline_render_manifest()` records artifact hashes, render-plan hashes, chunk boundaries, staged resource hashes, and cache-reuse metadata for reproducible offline/distributed renders; `evaluate_offline_render_compute_policy()` keeps GPU-assisted analysis out of live audio-thread scopes and makes CPU fallback explicit |
| Subsection Reader | `subsection_reader.hpp` | Read frame range without copying — `reader.sample(ch, frame)` |
| System Volume | `system_volume.hpp` | `get_system_volume()` / `set_system_volume(0.8f)` |

Offline render manifests intentionally separate artifact identity from render
plan identity. Equivalent renders with different chunk schedules can have the
same `audio_sha256` and a zero residual while still carrying different
`render_plan_sha256` values and chunk metadata for distributed reproduction.

---

## midi

MIDI I/O, file handling, MIDI 2.0 support, and provider-neutral note tuning.

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
| Device I/O | platform/ | CoreMIDI (macOS), WinMIDI (Windows), ALSA (Linux); Web MIDI scaffold is not wired into the shipped WASM build |
| Files | `midi_file.hpp` | Read/write Standard MIDI Files |
| Messages | via CHOC | `ShortMessage::noteOn(0, 60, 100)` |
| Tuning | `tuning.hpp`, `mts_esp_tuning.hpp`, `scala_tuning.hpp` | Provider-neutral note-to-frequency API with 12-TET default, optional MTS-ESP session/SysEx provider, and optional Scala SCL/KBM local-file provider |
| UMP | `ump.hpp` | MIDI 2.0 Universal MIDI Packets, MPE zones |
| MPE | `mpe_voice_tracker.hpp`, `mpe_buffer.hpp`, `mpe_synth_voice.hpp` | Per-note pitch bend / pressure / timbre tracking, opt-in sidecar buffer, and voice/allocator helpers. See [docs/guides/mpe.md](../guides/mpe.md) |

---

## signal

30+ real-time-safe DSP processors. Process methods operate on single samples or buffers and are safe for the audio thread after the helper's documented construction/configuration/`prepare()` step. Setup methods that allocate storage must run off the audio thread.

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

`PartitionedConvolver` is partitioned for one fixed block size, and `process()`
must be handed exactly that many samples. `load_ir()` rounds its `block_size`
argument **up to the next power of two** (its FFT is radix-2), so the size you
must feed is `conv.block_size()` — not necessarily the value you passed in.

```cpp
#include <pulp/signal/convolver.hpp>

PartitionedConvolver conv;
conv.load_ir(impulse_response.data(), ir_length, block_size);  // may round up

// In your process callback — num_samples MUST equal conv.block_size():
conv.process(input, output, conv.block_size());
```

If your host delivers blocks of any other size (variable blocks, or a size that
is not a power of two), re-block the audio into `conv.block_size()` chunks
yourself and report the added delay from `Processor::latency_samples()`.

A loaded convolver handed the wrong block size **fails closed**: it emits
silence and increments `conv.block_size_violations()`. It deliberately does not
pass the input through, because a pass-through is audibly indistinguishable from
a working convolution and would hide the bug. Assert
`conv.block_size_violations() == 0` in your tests.

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
| Oversampling | `oversampling.hpp` | 2x/4x/8x/16x realtime up/downsampling; minimum-phase IIR and 96/140 dB-prototype linear-phase FIR tiers with exact latency reporting |
| Phaser | `phaser.hpp` | All-pass filter chain with LFO modulation for sweeping comb effects |
| Reverb | `reverb.hpp` | Algorithmic stereo reverb with room size, damping, and width controls |
| Waveshaper | `waveshaper.hpp` | Static nonlinear distortion via transfer function (tanh, soft clip, custom) |

#### Dynamics

| Processor | Header | Description |
|-----------|--------|-------------|
| Ballistics Filter | `ballistics_filter.hpp` | Envelope follower with configurable attack/release for meter and dynamics |
| Compressor | `compressor.hpp` | Soft-knee downward compressor with threshold, ratio, attack, release |
| DryWetMixer | `dry_wet_mixer.hpp` | Parallel mix with latency compensation — equal-power or linear crossfade |
| Gain | `gain.hpp` | Scalar gain stage; pair with `smoothed_value.hpp`, `log_ramped_value.hpp`, or audio `apply_gain_ramp()` when transitions need de-clicking |
| Noise Gate | `noise_gate.hpp` | Silence signals below threshold with hysteresis to avoid chatter |

#### Generators and analysis

| Processor | Header | Description |
|-----------|--------|-------------|
| ADSR | `adsr.hpp` | Attack-decay-sustain-release envelope generator for amplitude or filter modulation |
| FFT | `fft.hpp` | Fast Fourier Transform — uses vDSP on Apple, fallback on other platforms |
| Multi-Channel Meter | `multi_channel_meter.hpp` | Peak and RMS level measurement across multiple channels |
| Oscillator | `oscillator.hpp` | Wavetable oscillator with sine, saw, square, triangle waveforms |
| Spectrogram | `spectrogram.hpp` | Rolling time-frequency analysis for visual display of spectral content |
| STFT | `stft.hpp` | Short-time Fourier Transform for visualization (analysis-only; for processing use `spectral_frame_engine.hpp`) |

#### Spectral processing

| Processor | Header | Description |
|-----------|--------|-------------|
| Spectral Frame Engine | `spectral_frame_engine.hpp` | Streaming STFT analysis + overlap-add synthesis with coherent multichannel frame groups and variable synthesis hop |
| Realtime Pitch/Time | `realtime_pitch_time_processor.hpp` | Phase-vocoder pitch shifting (fixed duration, exact reported latency) and independent time stretching, with transient preservation, formant follow/preserve, and freeze |
| Phase Coordinator | `multichannel_phase_coordinator.hpp` | Laroche-Dolson phase propagation with identity peak locking, applied as one rotation per bin across a channel group — preserves inter-channel phase exactly |
| Envelope Shifter | `spectral_envelope_shifter.hpp` | Cepstral spectral-envelope estimation (true-envelope refinement) and formant warping with exact unity bypass |
| Transient Policy | `transient_phase_policy.hpp` | Spectral-flux transient detection (median + energy-relative gates) driving phase reset at onsets |
| Freeze Hold | `freeze_hold.hpp` | Spectral freeze / infinite hold with de-looped phase evolution, click-free engage/release, and a no-mute latch policy |
| Pitched Feedback Delay | `pitched_feedback_delay.hpp` | Delay with a latency-bearing processor inside the feedback loop, tempo sync, freeze-aware feedback gating, and a computed minimum delay |
| Control Smoother | `latency_aware_control_smoother.hpp` | Closed-form one-pole smoothing with attack/release asymmetry, semitone/ratio domains, block-size-independent trajectories |
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
| Smoothed Value | `smoothed_value.hpp` | Linear parameter ramps for zipper-noise reduction; use `log_ramped_value.hpp` for multiplicative/log smoothing |
| Special Functions | `special_functions.hpp` | sinc, Bessel, dB↔linear, MIDI note↔frequency conversions |

---

## dsl

External DSP language lanes that generate or adapt source into ordinary Pulp
processors while keeping the third-party toolchain developer-supplied.

**Link:** `pulp::dsl` · **Include prefix:** `<pulp/dsl/...>`

| Lane | What It Does | Guide |
|------|--------------|-------|
| FAUST | Offline code generation into checked-in C++ headers | [FAUST guide](../guides/faust.md) |
| Cmajor | External Cmajor toolchain validation and generation | [Cmajor guide](../guides/cmajor.md) |
| JSFX | Bounded source-only JSFX subset parsing and validation | [JSFX guide](../guides/jsfx.md) |

All DSL lanes use the same contract: Pulp owns the processor wrapper and build
integration; the external compiler/runtime remains opt-in and outside the
public repository unless its license allows redistribution.

---

## state

Parameters, state trees, presets, and settings.

**Link:** `pulp::state` · **Include prefix:** `<pulp/state/...>`

### Plugin parameters — the core state system

```cpp
#include <pulp/state/store.hpp>

StateStore store;
constexpr ParamID kGainId = 1;
constexpr ParamID kMixId = 2;

store.add_parameter({.id = kGainId, .name = "Gain", .unit = "dB",
                     .range = {-60.0f, 12.0f, 0.0f}});
store.add_parameter({.id = kMixId, .name = "Mix",
                     .range = {0.0f, 1.0f, 1.0f}});

// Audio thread reads atomically (no locks)
float gain = store.get_value(kGainId);

// UI thread writes with gesture grouping for undo
store.begin_gesture(kGainId);
store.set_value(kGainId, -6.0f);
store.end_gesture(kGainId);
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

`PropertyValue` supports scalar values (`std::monostate`, `bool`, `int64_t`,
`double`, `std::string`) plus provider-neutral structured leaves:

```cpp
root->set("macroState", make_property_object({
    {"name", std::string("Brightness")},
    {"points", make_property_array({0.0, 0.5, 1.0})},
    {"metadata", make_property_object({{"enabled", true}, {"revision", int64_t(2)}})},
}));
```

Use arrays/objects for structured leaf data that belongs to a property
(JSON-like arrays, dictionaries, custom state records, imported `var`-style
values). Use child `StateTree` nodes for owned tree structure: a ValueTree-like
record with child records should become a `StateTree` parent with child nodes,
while each node's non-structural payload can use scalar/array/object
properties. Pulp intentionally does **not** store `StateTree` nodes inside
`PropertyValue`: node ownership lives in the parent/child graph, which keeps
`deep_copy()`, `clone_synced()`, and `StateTreeSynchroniser` free from hidden
aliasing and cycles.

Existing scalar typed getters and `std::get_if` checks keep working; exhaustive
`std::visit` handlers over `PropertyValue` should add Array/Object cases.

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

## timebase

Musical and media time primitives for scheduling without accumulated floating-point
drift. `CompiledTempoMap` maps strong integer tick and sample positions across
constant tempos and BPM-linear-in-tick ramps. Tempo-point boundaries use exact
integer sample anchors; `samples_to_ticks()` returns a canonical tick so
sample-to-tick-to-sample conversion is exact while the tick grid is at least as
dense as the sample grid. Arbitrary ticks map monotonically to the integer sample
grid and may canonicalize to a neighboring tick. On a sparser grid,
`resolve_sample()` returns the nearest representable tick plus its exact sample
error; for example, at 48 kHz and 1 BPM, ticks 0 and 1 map to samples 0 and 4, so
sample 2 cannot have an exact integer-tick inverse.

The module has no Pulp module dependencies. Tick and sample positions use their
full signed 64-bit ranges; tick-position, duration, and `MonotonicBeat`
arithmetic saturates at the nearest endpoint rather than overflowing.
`ticks_to_samples()` likewise saturates when its mathematical result exceeds the
sample domain. When a requested sample lies outside the image of the tick
domain, `resolve_sample()` reports `exact == false`, a nearest canonical edge
representation, and the actual sample error.

**Link:** `pulp::timebase` · **Include prefix:** `<pulp/timebase/...>`

```cpp
#include <pulp/timebase/compiled_tempo_map.hpp>

using namespace pulp::timebase;
const TempoPoint points[] = {
    {{0}, 120.0, TempoCurve::LinearInTicks},
    {{8 * kTicksPerQuarter}, 160.0, TempoCurve::Constant},
};
const CompiledTempoMap tempo(points, {48'000, 1});
const auto sample = tempo.ticks_to_samples({4 * kTicksPerQuarter});
```

`MonotonicBeat` is the strong type for the transport's non-looping musical
clock; the transport owns advancement while timeline positions may seek or
wrap. `MeterMap` is intentionally deferred to the timeline engine.

## timeline

Immutable document-model foundations and a bounded typed editing core for musical timelines. `Project`,
`Sequence`, `Track`, and `Clip` are cheap copyable snapshots whose construction
factories validate identities, ranges, references, and non-overlapping sparse
arrangement lanes. Tracks retain persistent AVL indexes for both
`(anchor, start, ItemId)` timeline order and `ItemId` lookup. `replace_clip()`
path-copies only affected search paths while older snapshots share untouched
subtrees.

`InsertClip`, `RemoveClip`, `MoveClip`, and `SetNoteVelocity` apply only through
atomic transactions. `DocumentSession` serializes multiple control-thread
writers, publishes pinned immutable snapshots, rejects stale revisions and
typed precondition failures without partial application, and records a precise
dirty set. Its bounded journal rejects when full rather than losing replay
history; inverse-command undo/redo append ordinary new transactions.

**Link:** `pulp::timeline` · **Include prefix:** `<pulp/timeline/...>`

```cpp
#include <pulp/timeline/model.hpp>

using namespace pulp::timeline;
auto empty = Clip::create({3}, {0}, {705'600}, EmptyContent{});
if (!empty)
    return;
auto track = Track::create({2}, "Notes", {std::move(empty).value()});
```

Every owned object uses a nonzero monotonic `ItemId`; a `Project` stores the
next never-used value; `UINT64_MAX` is the explicit exhausted allocator state
and is valid project state after ownership reaches `UINT64_MAX - 1`.
`ClipTimeAnchor` distinguishes tempo-following musical tick ranges from fixed
absolute ranges expressed as `SamplePosition`, integer sample count, and a
normalized `RationalRate`. Phase 1 rejects mixed-anchor clips within one Track
until a context-owned projection can compare those domains; a Sequence can
still contain separate musical and absolute Tracks and bound both domains.
`remap_ids()` performs two passes: it allocates every destination identity
before rebuilding the immutable hierarchy. Clip, Track, and Sequence subtree
overloads distinguish owned IDs from external media-asset references and accept
an atomic `ExternalIdFixup`; closure-wide duplicate owned IDs are rejected
before allocator state changes. `NoteContent` is a flat POD array sorted by
`(start, ItemId)`. Fallible construction uses
`pulp::runtime::Result` and reports `ModelError` without exceptions.

`assets.hpp` separates durable SHA-256 content identity from optional resolution
hints and alternate representations. `schema_registry.hpp` provides an explicit
immutable registry with typed extension codecs and bounded per-version
migrations. `serialize.hpp` reads and writes deterministic JSON snapshots:
64-bit values are canonical decimal strings, malformed or oversized input is
rejected under `DecodeLimits`, and unknown extension envelopes retain their
exact validated bytes for lossless re-save. `SerializedSnapshot` flags those
opaque objects so callers can surface compatibility risk. This is snapshot JSON
only; it does not read or write ZIP/package containers.

This initial surface intentionally excludes commands, undo, journals, package
I/O, publication, playback, automation, launch slots, takes, nesting, devices,
routing, and UI.

## playback

The master timeline transport publishes integer-authoritative block snapshots.
Normal blocks contain one `TransportRange`; a block crossing a loop boundary
contains exactly two, with the second range marked as a discontinuity. Timeline
ticks wrap while `MonotonicBeat` remains continuous, so launch and scheduling
intent can use a clock that does not repeat. Forward, backward, and stopped
seeks reanchor only the timeline; they never jump the monotonic clock.

Control-thread changes are published as one coherent `SeqLock` state. The audio
thread consumes that state through allocation-free `begin_block()` calls. A
stopped block still covers the callback's frames while holding both musical
clocks. Loops shorter than the configured maximum block are rejected, which
guarantees one block can cross at most one loop boundary.

Snapshots carry first-block-safe transport and meter change flags, while each
range carries its own tempo and `tempo_changed` flag. This keeps a loop split
across tempo regions faithful when projected into per-range `ProcessContext`s.

**Link:** `pulp::playback` · **Include prefix:** `<pulp/playback/...>`

```cpp
#include <pulp/playback/transport.hpp>

pulp::playback::MasterTransport transport;
transport.prepare(tempo, {
    .max_buffer_size = 1024,
    .initially_playing = true,
});

pulp::playback::TransportSnapshot block;
transport.begin_block(512, block);
for (std::uint8_t i = 0; i < block.range_count; ++i) {
    schedule(block.ranges[i]);
}
```

Plugin/host adapters include
`<pulp/format/playback_context_projection.hpp>` to project a range into the
public `ProcessContext`. That header is the only lossy tick-to-double boundary;
`core/playback` does not depend on format, host, or view code.

The module also compiles immutable `PlaybackProgram` snapshots off the audio
thread. A request carries one immutable Project snapshot, its external document
revision, a precompiled tempo map, and an explicit dirty-track set. Clean
`TrackProgram` objects retain shared-pointer identity while dirty tracks receive
a newer generation; revisions skipped by request coalescing are valid.
Sparse per-track policy deltas select an available provider and whether a stable
shell carries state by ItemId or resets it on stateless adoption. Omitted tracks
retain their published policy, and coalescing merges deltas with latest-track
wins before publication.
Phase 1 compiles arrangement payloads only: launcher/external-input selections
and availability bits are rejected until those provider programs exist.
`DeferredCompileExecutor` advances bounded slices from an idle/UI pump and
`WorkerCompileExecutor` supplies the native background lane, with an explicit
unsupported stub in threadless builds.

At an audio callback boundary, one `PlaybackProgramBlockLatch` pins the whole
program for the block. Every `StableRendererShell` consults that same pin,
adopts only a newer generation for the same ItemId, and carries its small cursor
snapshot through a `SeqLock`. This layer contains structural clip identities
only: note/audio rendering, graph binding, commands, undo, and persistence
schemas remain outside this slice.

## format

Plugin format adapters — write your plugin once, deploy to 9 formats.

**Link:** `pulp::format` · **Include prefix:** `<pulp/format/...>`

### Writing a plugin

```cpp
#include <pulp/format/processor.hpp>

class MyPlugin : public Processor {
public:
    PluginDescriptor descriptor() const override {
        return {
            .name = "MyGain",
            .manufacturer = "MyCompany",
            .bundle_id = "com.myco.gain",
            .version = "1.0.0",
            .category = PluginCategory::Effect,
        };
    }
    
    void define_parameters(state::StateStore& store) override {
        store.add_parameter({.id = gain_id_, .name = "Gain", .unit = "dB",
                             .range = {-60.0f, 12.0f, 0.0f}});
    }
    
    void prepare(const PrepareContext& context) override {}
    
    void process(audio::BufferView<float>& audio_output,
                 const audio::BufferView<const float>& audio_input,
                 midi::MidiBuffer& midi_in, midi::MidiBuffer& midi_out,
                 const ProcessContext& ctx) override {
        float gain = db_to_linear(state().get_value(gain_id_));
        for (std::size_t ch = 0;
             ch < audio_output.num_channels() && ch < audio_input.num_channels();
             ++ch) {
            auto in = audio_input.channel(ch);
            auto out = audio_output.channel(ch);
            for (std::size_t i = 0; i < out.size(); ++i) {
                out[i] = in[i] * gain;
            }
        }
    }

private:
    state::ParamID gain_id_ = 1;
};
```

This single class automatically works as VST3, AU, CLAP, LV2, AAX, Standalone, WAM, and WCLAP.

### Supported formats

| Format | Status | Notes |
|--------|--------|-------|
| AAX | ✓ Usable | Requires developer-supplied Avid SDK |
| AU v2 | ✓ Usable | macOS only, via AudioUnitSDK |
| AU v3 | ✓ Usable | macOS + iOS (always runs sandboxed as `.appex` extension) |
| CLAP | ✓ Stable | First-class — modulation, WebView, note expressions |
| LV2 | ✓ Usable | Linux plugin format |
| Standalone | ✓ Stable | Desktop app with audio settings, test signal |
| VST3 | ✓ Stable | Full parameter sync, state, editor resize |
| WAM | ✓ Experimental | Web Audio Module (browser) |
| WCLAP | ✓ Experimental | Web CLAP (browser) |

### Other format features

| Feature | Header | Description |
|---------|--------|-------------|
| ARA | `ara.hpp` | Audio Random Access document-controller scaffold and SDK-gated companion-factory hooks |
| Host Detection | `host_type.hpp` | `detect_host_type()` → Logic, Reaper, Ableton, etc. |
| Settings Panel | `settings_panel.hpp` | Audio/MIDI device selector with test signal and meters |
| ViewBridge | `view_bridge.hpp` | Editor-view lifecycle: `create_view()`, `on_view_{opened,closed,resized}`, multi-view attach (editor + inspector + remote). See `docs/guides/view-bridge.md`. |

The SDK-facing `Processor` and host-side `PluginSlot` surfaces share the
[node ABI policy](./node-abi.md): virtual methods are append-only within a node
ABI generation, and new optional behavior should prefer additive descriptor
capabilities over new virtual methods.

---

## host

Plugin *hosting* — the mirror of `format`. Load VST3 / AU / CLAP / LV2
plug-ins, wire them into a DAG, and process audio through the chain.

| Feature | Header | Description |
|---------|--------|-------------|
| Scanner | `pulp/host/scanner.hpp` | Walk system plug-in paths; return `PluginInfo` |
| PluginSlot | `pulp/host/plugin_slot.hpp` | Uniform load/prepare/process interface over every format |
| SignalGraph | `pulp/host/signal_graph.hpp` | DAG topology + topological sort |
| Bake | `pulp/host/baked_graph_processor.hpp` | Freeze a lowerable `SignalGraph` into an optimized `BakedGraphProcessor` (bit-identical to the live graph). `bake()` is the in-process (trusted) path |
| Baked codec | `pulp/host/baked_codec.hpp` | Signed on-disk `.pulpbake` artifact: `write_baked_signed` + verify-before-parse `load_baked` (Ed25519 trust-set, bounded parse). See [signal-graph](signal-graph.md#baking-a-graph-to-a-shippable-artifact) |

All four format loaders (CLAP, VST3, AU, LV2) are implemented in
`core/host/src/plugin_slot_*`: each opens the bundle, resolves the
format's factory/descriptor, and wires the host-side PluginSlot onto
the format's real processing entry point. Feature coverage varies per
format (parameter automation, MIDI routing, editor views, and state
serialization are each at different stages); see the per-format
source files for the current scope. If an SDK is not compiled in at
configure time (for example, AU on Linux), the matching case in
`PluginSlot::load()` logs a warning and returns `nullptr`. See
[hosting guide](../guides/hosting.md) and
[signal-graph reference](./signal-graph.md).

---

## canvas

2D drawing with GPU acceleration and smart text layout.

**Link:** `pulp::canvas` · **Include prefix:** `<pulp/canvas/...>`

### Drawing

```cpp
void MyWidget::paint(Canvas& canvas) {
    // Background
    canvas.set_fill_color(Color::rgba8(30, 30, 40));
    canvas.fill_rounded_rect(0, 0, width, height, 8);
    
    // Gradient fill
    const Color colors[] = {Color::rgba8(80, 120, 255), Color::rgba8(40, 60, 180)};
    const float positions[] = {0.0f, 1.0f};
    canvas.set_fill_gradient_linear(0, 0, 0, height, colors, positions, 2);
    canvas.fill_rect(10, 10, width - 20, 4);
    canvas.clear_fill_gradient();
    
    // Text
    canvas.set_fill_color(Color::rgba8(220, 220, 230));
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
| MSDF text | `msdf_atlas.hpp` | Multi-channel SDF atlas scaffold with `median(r,g,b)` sampler plumbing; current generator emits equal RGB placeholder distances until `msdfgen` is wired. |
| PSDF text | `psdf_atlas.hpp` | Pseudo-SDF variant with vector-fallback helper for extreme zoom. |
| SDF effects | `sdf_effects.hpp` | Host-side design-token presets for outline / shadow / glow / bevel; visible rendering waits for the SkSL-backed SDF text draw path. |
| SDF atlas cache | `sdf_atlas_cache.hpp` | Frame-based LRU glyph sharing across `fill_text_sdf` call-sites with dirty-rect upload hints. |
| Path → SDF | `path_to_sdf.hpp` | Runtime EDT of a binary mask to produce an SDF for procedural shapes. |

---

## view

Full widget toolkit with CSS-inspired layout and JS scripting.

**Link:** `pulp::view` · **Include prefix:** `<pulp/view/...>`

`pulp::view` is the full compatibility target and links both native widgets and
the JS runtime bridge. Baked/native UI code that constructs `View` trees
directly and does not evaluate JS can link `pulp::view-core`; code that uses
`ScriptEngine`, `WidgetBridge`, scripted UIs, or runtime import should link
`pulp::view-script` or the full `pulp::view` target.

### Creating a UI

```cpp
#include <pulp/view/widgets.hpp>

auto root = std::make_unique<Panel>();
root->set_background_token("bg.surface");

auto knob = std::make_unique<Knob>();
knob->set_label("Gain");
knob->set_value(0.5f);
knob->on_change = [&](float v) { store.set_value(gain_id, v); };
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
| TextEditor | Single or multi-line text input with native keyboard movement, selection, copy/paste, undo, IME, and grapheme-safe UTF-8 editing |
| Toggle | Two-state switch control for enabling/disabling features |

##### TextEditor Behavior

`TextEditor` is the SDK-level text-entry control used by native views, imported
HTML `<input>`, and imported `<textarea>` controls. It implements platform-style
caret movement and selection by default: character, word, line, document, page,
and Shift-selection variants; word/line delete shortcuts; double-click word
selection with word-granular drag extension; triple-click line selection in
multi-line mode; standard Cut/Copy/Paste/Select All context menus; and
mouse/trackpad scrolling for multi-line fields.

Text positions are stored as UTF-8 byte offsets for host/IME compatibility, but
editing commands snap those offsets to grapheme-cluster boundaries. This keeps
emoji, combining marks, flags, and ZWJ sequences from being split by arrow keys,
Backspace/Delete, hit testing, or selection expansion.

Applications can tune text-field policy without forking key handling:
`read_only` allows focus, navigation, selection, and copy while blocking
mutation; `View::set_enabled(false)` disables interaction entirely;
`tab_behavior` chooses focus traversal, literal tab insertion, commit callback,
or consume/ignore behavior;
`multi_line_return_behavior` chooses Return/Shift-Return behavior; `max_length`
counts grapheme clusters; `paste_sanitizer` handles paste-only cleanup;
`input_filter` sanitizes typed and pasted insertion text; and `validator`
accepts or rejects a whole-buffer candidate before an edit lands.
`line_ending_policy` normalizes, strips, or preserves inserted line endings
where the control shape allows it. `clipboard_policy` can disable clipboard
traffic entirely or explicitly allow password contents to leave the field.
Password fields mask display text and disable selected-text export, copy, and
cut by default unless `allow_password_clipboard` or
`ClipboardPolicy::allow_password_contents` is enabled.

Programmatic `set_text()` is a host/state-sync operation, so it clears the
editor undo stack instead of recording a user-edit undo entry. Use
`set_caret_pos()`, `set_selection()`, `selection_anchor()`,
`selection_active()`, and `selection_range()` when a host, importer, IME, or
test needs explicit caret/selection control; all public offsets are clamped to
grapheme boundaries.

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
theme.colors["bg.surface"] = Color::rgba8(25, 25, 35);
theme.colors["control.accent"] = Color::rgba8(80, 130, 255);
theme.colors["text.primary"] = Color::rgba8(220, 220, 230);

// Widgets resolve tokens automatically:
canvas.set_fill_color(resolve_color("bg.surface"));
```

### Design import

Per-source parsers under `core/view/src/design_import_*.cpp` translate
external designs into Pulp's import IR for `pulp import-design`.
`design_import_designmd.cpp` parses Google's DESIGN.md format
(YAML-frontmatter + Markdown body) into a DTCG `tokens.json`;
yaml-cpp (MIT, vendored via CMake FetchContent) provides the
frontmatter parse. See [`reference/imports/designmd.md`](imports/designmd.md).

### JS-scripted UI

Write your plugin UI in JavaScript. Live hot reload is validated in the macOS
standalone development host; plugin targets can load the same `UI_SCRIPT`, but
live reload is not yet guaranteed across hosts:

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
#include <pulp/osc/bundle.hpp>

// Send
pulp::osc::Sender sender;
sender.connect("192.168.1.100", 9000);
sender.send(pulp::osc::Message("/synth/freq").add(440.0f));
sender.send(pulp::osc::Message("/synth/gate").add(1));

// Receive
pulp::osc::Receiver receiver;
receiver.listen(9000, [](const pulp::osc::Message& msg) {
    if (msg.address == "/synth/freq")
        set_frequency(msg.get_float(0, 440.0f));
});

// Bundle
pulp::osc::Bundle bundle;
bundle.add(pulp::osc::Message("/synth/freq").add(880.0f));
sender.send(bundle);
```

Supports bundles with timetags, the optional OSC RGBA colour tag (`r`), raw datagram sends, and address pattern matching (`*`, `?`, `[...]`, `{a,b}`) through receiver routes.

---

## native-components

The language-neutral C ABI for opt-in native-language audio components (Rust
first, also C / Zig / generated DSP). A C++ `Processor` adapter owns a
source-built native DSP core through this private, C-shaped FFI.

**Link:** `pulp::native-components` · **Include prefix:** `<pulp/native_components/...>`

```cpp
#include <pulp/native_components/native_core.h>   // the canonical C contract
#include <pulp/native_components/native_core.hpp>  // optional C++ sugar (hash, asserts)
```

The header carries POD structs with leading `size`/`abi_version`, opaque
instance handles, status codes, host-owned borrowed buffers, a sorted
parameter-event view, an opaque versioned state span, and explicit
suspend/resume/reset lifecycle — embodying the twelve forward-compatibility
decisions in [native-components reference](native-components.md). It has **no
Rust dependency**: the module builds on every platform, and the opt-in Rust
staticlib lane lives behind the `PULP_BUILD_NATIVE_COMPONENT_RUST_TESTS` CMake
option (OFF by default).

This is the *Processor-level* FFI, deliberately independent of `SignalGraph`.
The module also ships the public `pulp_node_v1` node ABI (`pulp_node_v1.h`) for
custom graph nodes; its `NativeCoreProcessor` adapter lives in `pulp::format`, and
signed dynamic node-pack loading lives in `pulp::host` (`node_pack.hpp`). See
[node-abi](node-abi.md) for the `pulp_node_v1` contract and node packs.

---

## render

GPU surface management — you rarely use this directly. Canvas and View handle it.

| Feature | What It Does |
|---------|-------------|
| Dawn/WebGPU | Cross-platform GPU abstraction (Metal, Vulkan, D3D12, OpenGL) |
| GPU Compute | Experimental batch processing for >64K element workloads |
| Skia Graphite | 2D rendering on top of Dawn — what Canvas uses internally |

---

## gpu_audio

**Experimental.** A fixed-latency bridge that lets a real-time `process()` block
offload heavy DSP to the GPU without blocking the audio thread. `GpuAudioTransport`
owns lock-free input/output rings and a polling worker; a `GpuAudioNode` submits
whole blocks to `render::GpuCompute` and reads results back one block later, so the
audio thread never waits on the device. When the GPU can't keep up (an offline
bounce running faster than real time, or a cold pipeline), a configurable **miss
policy** falls back to CPU or silence rather than glitching.

**Link:** `pulp::gpu-audio` · **Include prefix:** `<pulp/gpu_audio/...>` ·
**Depends on:** `audio`, `runtime`, `signal` (always) · `render` (GPU builds only)

| Node / primitive | What It Does |
|------------------|-------------|
| `GpuAudioTransport` | Fixed-latency RT↔non-RT bridge: lock-free rings + polling worker over `GpuCompute` |
| `GpuConvolver` | GPU-resident fused / batched partitioned convolution (long IRs, many instances) |
| `GpuMultiConvolver` | Batched multi-IR / multi-room convolution — one GPU submit per block across N IRs |
| `GpuStft` | GPU STFT / ISTFT primitive — the spectral toolkit's analysis/synthesis stage |
| `GpuSpectralFreeze` / `GpuSpectralMorph` / `GpuSpectralStack` | Capture-and-render spectral engines (single freeze, A/B morph, N-layer stack/cloud) |

The node boundary is **not** real-time-safe at the device level by design — the
GPU round-trip is amortized across a block of fixed latency, not paid per sample.
Only the GPU node *implementations* are gated on `pulp::render`; the
`GpuAudioTransport` bridge and the public node classes still compile and link in
a build without the GPU stack, report `gpu_available() == false`, and route the
`signal::*` CPU fallback.

**Example plugins built on it** (in-tree, `examples/`):

| Example | Uses | Docs |
|---------|------|------|
| [SuperConvolver](../examples/super-convolver.md) | `GpuConvolver` (single IR) / `GpuMultiConvolver` (many rooms) — convolution reverb with live IR swap; GPU carries very long IRs / many rooms | [super-convolver](../examples/super-convolver.md) |
| [Spectral Lab](../examples/spectral-lab.md) | GPU spectral stack — N-layer freeze / morph "cloud" | [spectral-lab](../examples/spectral-lab.md) |
| [GPU NAM](../examples/gpu-nam.md) | `render::GpuCompute::wavenet_forward` neural-inference primitive (own repo) | [gpu-nam](../examples/gpu-nam.md) |

**SDK guide:** [GPU Audio SDK](../guides/gpu-audio-sdk.md) — building nodes,
the transport contract, and the validation checklist.

---

## ship

Packaging and distribution — from code signing to installer to update feed.

**Link:** `pulp::ship` · **Include prefix:** `<pulp/ship/...>`

```bash
# Sign all plugin bundles
pulp ship sign --identity "Developer ID Application: My Company"

# Package — creates .pkg/.dmg (macOS), NSIS (Windows), or .deb/.tar.gz (Linux)
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
