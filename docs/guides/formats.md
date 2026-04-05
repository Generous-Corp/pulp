# Plugin Format Adapters

Pulp supports three plugin formats (CLAP, VST3, AU v2) plus standalone and headless hosts. You write one `Processor` subclass; format adapters handle the rest.

Each format is activated by including a single entry-point header and calling a macro in one `.cpp` file per target. The macro generates all boilerplate: factory functions, extension dispatch, parameter registration, and lifecycle management.

---

## CLAP

**Entry point header:** `<pulp/format/clap_entry.hpp>`

### Macro

```cpp
#include "my_processor.hpp"
#include <pulp/format/clap_entry.hpp>

PULP_CLAP_PLUGIN(my_namespace::create_my_processor)
```

`PULP_CLAP_PLUGIN(factory_fn)` generates:

- A `clap_plugin_entry_t` exported as `clap_entry` (the CLAP shared library entry point)
- A `clap_plugin_factory_t` that creates one plugin
- Extension dispatch for audio-ports, note-ports, params, state, latency, and tail
- Static-init registration via `pulp::format::register_plugin()`

The plugin descriptor (id, name, vendor, version, features) is derived automatically from `Processor::descriptor()`. Category mapping:

| `PluginCategory` | CLAP feature |
|---|---|
| `Effect` | `CLAP_PLUGIN_FEATURE_AUDIO_EFFECT` |
| `Instrument` | `CLAP_PLUGIN_FEATURE_INSTRUMENT` |
| `MidiEffect` | `CLAP_PLUGIN_FEATURE_NOTE_EFFECT` |

### Parameter Sync

**Host to plugin:** During `clap_process()`, the adapter iterates `in_events`, looking for `CLAP_EVENT_PARAM_VALUE` events. Each one writes to `StateStore::set_value()`. Gesture events (`CLAP_EVENT_PARAM_GESTURE_BEGIN` / `END`) are forwarded to `StateStore::begin_gesture()` / `end_gesture()`.

The `params_flush()` extension callback handles the same events outside of `process()` (e.g., when the plugin is bypassed).

**Plugin to host:** Before calling `Processor::process()`, the adapter snapshots all parameter values. After processing, it compares each value against the snapshot. Any changed parameters are emitted as `CLAP_EVENT_PARAM_VALUE` events via `out_events->try_push()`. This allows hosts to record automation from plugin-side changes.

### CLAP Modulation

The adapter handles `CLAP_EVENT_PARAM_MOD` events. At the start of each process call, `store.reset_all_mod()` clears per-buffer modulation offsets. Incoming mod events write to `StateStore::set_mod_offset()`. Processors can call `store.get_modulated(id)` to read `base + mod_offset`.

### MIDI Routing

Note events are converted between CLAP's `clap_event_note_t` format and Pulp's `MidiEvent`:

- `CLAP_EVENT_NOTE_ON` becomes `MidiEvent::note_on(channel, key, velocity * 127)`
- `CLAP_EVENT_NOTE_OFF` becomes `MidiEvent::note_off(channel, key, velocity * 127)`
- `sample_offset` is set from `hdr->time` for sample-accurate timing

Note port declaration is driven by `descriptor().accepts_midi` and `descriptor().produces_midi`. Ports support both `CLAP_NOTE_DIALECT_CLAP` and `CLAP_NOTE_DIALECT_MIDI`, preferring CLAP dialect.

### State Save/Load

- **Save:** `store.serialize()` returns a `std::vector<uint8_t>`. The adapter writes it to the CLAP `clap_ostream_t`.
- **Load:** The adapter reads all bytes from `clap_istream_t` into a buffer (4 KB chunks), then calls `store.deserialize()`.

### Multi-Bus / Sidechain

Audio port count and info come from `descriptor().input_buses` and `descriptor().output_buses`. Each bus is reported with:

- Port ID: inputs start at 0, outputs at 100
- `CLAP_AUDIO_PORT_IS_MAIN` flag set on bus index 0
- Port type: `CLAP_PORT_MONO` for 1 channel, `CLAP_PORT_STEREO` otherwise
- `in_place_pair` set to `CLAP_INVALID_ID`

The process callback currently routes the first input and first output bus. Additional buses are declared but not yet routed to `Processor::process()`.

### Latency and Tail

- **Latency:** `clap_plugin_latency_t::get` returns `processor->latency_samples()`.
- **Tail:** `clap_plugin_tail_t::get` returns `descriptor().tail_samples`. A value of `-1` (infinite tail) maps to `UINT32_MAX`.

### Known Limitations

- Only the first input and output bus are passed to `process()`. Sidechain buses are declared to the host but not yet routed.
- No GUI extension is wired.
- Per-note modulation (`note_id`, `port_index`, `channel`, `key` fields in param mod events) is accepted but not per-note routed.

---

## VST3

**Entry point header:** `<pulp/format/vst3_entry.hpp>`

### Macro

```cpp
#include "my_processor.hpp"
#include <pulp/format/vst3_entry.hpp>

static const Steinberg::FUID kMyPluginUID(0x12345678, 0x9ABCDEF0, 0x00000001, 0x00000001);

PULP_VST3_PLUGIN(kMyPluginUID, "My Plugin", "Fx", "My Company",
                  "1.0.0", "https://example.com",
                  my_namespace::create_my_processor)
```

`PULP_VST3_PLUGIN(uid, name, category, vendor, version, url, factory_fn)` generates:

- A `GetPluginFactory()` export using the VST3 SDK's `BEGIN_FACTORY_DEF` / `END_FACTORY` macros
- A single class registration via `DEF_CLASS2`
- The factory function that creates a `PulpVst3Processor` (which subclasses `SingleComponentEffect`)

The FUID must be generated once and never changed across versions. It is the stable identity of your plugin in every VST3 host.

### Parameter Sync

**Host to plugin:** During `process()`, the adapter reads `data.inputParameterChanges`. For each changed parameter, it reads the last point value (normalized 0-1) and writes it to `StateStore::set_normalized()`.

**Plugin to host:** The adapter snapshots all parameter values before calling `Processor::process()`. After processing, any value that changed is:
1. Written to `data.outputParameterChanges` as a normalized value (so the host can record automation)
2. Synced to the VST3 parameter system via `setParamNormalized()`

**Gesture callbacks:** During `initialize()`, the adapter wires `StateStore` gesture callbacks to `beginEdit()` / `endEdit()`. This supports undo grouping in DAWs.

### VST3 Parameter Groups

Parameters are assigned to VST3 units via `ParamInfo::group_id`. When you set `group_id` on a parameter in `define_parameters()`, the adapter maps it to `ParameterInfo::unitId`. This enables parameter grouping in DAW interfaces.

Boolean parameters (step >= 1, range 0-1) get `stepCount = 1`. Parameters named "Bypass" additionally get the `kIsBypass` flag.

### MIDI Routing

The adapter adds event buses based on `descriptor()`:

- `accepts_midi = true` adds an event input bus ("MIDI In")
- `produces_midi = true` adds an event output bus ("MIDI Out")

VST3 note events (`Event::kNoteOnEvent`, `Event::kNoteOffEvent`) are converted to/from `MidiEvent`. Velocity is scaled between float 0-1 (VST3) and integer 0-127 (MIDI). Sample offsets are preserved.

### State Save/Load

- **Save (`getState`):** `store_.serialize()` writes the binary blob to `IBStream`.
- **Load (`setState`):** Reads the stream in 4 KB chunks, calls `store_.deserialize()`, then syncs all restored values back to the VST3 parameter system via `setParamNormalized()`.

### Multi-Bus / Sidechain

Audio buses from `descriptor().input_buses` and `descriptor().output_buses` are added during `initialize()`:

- Bus type: `kMain` for required buses, `kAux` for optional (sidechain) buses
- Activity: `kDefaultActive` for required, 0 (inactive by default) for optional
- Speaker arrangement: `kMono` or `kStereo` based on `default_channels`
- Bus names are converted from `std::string` to VST3 `String128`

`setBusArrangements()` accepts any arrangement where at least one input and one output bus are present.

### Latency and Tail

- `getLatencySamples()` returns `processor->latency_samples()`
- `getTailSamples()` returns `descriptor().tail_samples`, mapping `-1` to `kInfiniteTail`

### Known Limitations

- Only the first input/output bus channels are passed to `process()`. Sidechain bus routing is declared but not yet connected.
- Channel count in `setupProcessing()` defaults to 2; dynamic bus arrangement queries are not yet implemented.
- No VST3 GUI (`IPlugView`) is wired.

---

## AU v2

**Entry point headers:**
- Effects: `<pulp/format/au_v2_entry.hpp>`
- Instruments: `<pulp/format/au_v2_instrument_entry.hpp>`

### Macro (Effect)

```cpp
#include "my_processor.hpp"
#include <pulp/format/au_v2_entry.hpp>

PULP_AU_PLUGIN(MyPluginAU, my_namespace::create_my_processor)
```

`PULP_AU_PLUGIN(ClassName, factory_fn)` generates:

- A class `ClassName` subclassing `PulpAUEffect` (which subclasses `AUEffectBase`)
- A factory function `ClassNameFactory` for the `Info.plist` `factoryFunction` entry
- Plugin registration via `PULP_REGISTER_PLUGIN`

The factory function name must match the `factoryFunction` in your AU's `Info.plist`.

### Macro (Instrument)

```cpp
#include "my_synth.hpp"
#include <pulp/format/au_v2_instrument_entry.hpp>

PULP_AU_INSTRUMENT(MySynthAU, my_namespace::create_my_synth)
```

`PULP_AU_INSTRUMENT(ClassName, factory_fn)` generates:

- A class `ClassName` subclassing `PulpAUInstrument` (which subclasses `MusicDeviceBase`)
- A factory function via `AUSDK_COMPONENT_ENTRY(ausdk::AUMusicDeviceFactory, ClassName)`

Instruments have zero audio inputs and one audio output. MIDI is received via `HandleNoteOn()` / `HandleNoteOff()`.

### Parameter Sync

**Host to plugin (effects):** Each buffer, `ProcessBufferLists()` reads every parameter from the AU system via `GetParameter()` and writes to `StateStore::set_value()`. This brute-force sync is acceptable for typical parameter counts (< 50).

**Host to plugin (instruments):** `Render()` reads parameters via `Globals()->GetParameterRT()`.

**Plugin to host:** Parameter output changes are not yet emitted back to the AU host. Initial defaults are set via `Globals()->SetParameter()` during `Initialize()`.

**Gesture callbacks (effects):** The adapter wires `StateStore` gesture callbacks to `AUEventListenerNotify()` with `kAudioUnitEvent_BeginParameterChangeGesture` and `kAudioUnitEvent_EndParameterChangeGesture` event types.

### AU Parameter Units

The adapter maps Pulp unit strings to AU parameter units:

| `ParamInfo::unit` | AU unit |
|---|---|
| `"dB"` | `kAudioUnitParameterUnit_Decibels` |
| `"Hz"` | `kAudioUnitParameterUnit_Hertz` |
| `"%"` | `kAudioUnitParameterUnit_Percent` |
| Boolean (step >= 1, range 0-1) | `kAudioUnitParameterUnit_Boolean` |
| Everything else | `kAudioUnitParameterUnit_Generic` |

Stepped parameters with a `to_string` function get value string arrays via `GetParameterValueStrings()`.

### MIDI Routing

**Effects:** No MIDI. `ProcessBufferLists()` passes empty `MidiBuffer` objects to `Processor::process()`.

**Instruments:** MIDI notes arrive via `HandleNoteOn()` and `HandleNoteOff()` callbacks. These are buffered in `pending_midi_` (protected by a mutex) and drained into `midi_in` at the start of each `Render()` call. The sample offset (`inStartFrame`) is preserved.

### State Save/Load

The AU adapter stores Pulp state alongside the standard AU state dictionary:

- **Save:** Calls `AUEffectBase::SaveState()` (or `MusicDeviceBase::SaveState()`), then appends a `CFData` blob under the key `"pulp-state"` containing `store_.serialize()`.
- **Load:** Calls the base `RestoreState()`, then looks for the `"pulp-state"` key. If found, calls `store_.deserialize()` and syncs all values back to the AU parameter system via `Globals()->SetParameter()`.

### Tail and Latency

**Effects:** `GetTailTime()` converts `descriptor().tail_samples` to seconds by dividing by sample rate. `GetLatency()` does the same for `latency_samples()`. A tail of `-1` maps to `infinity`.

**Instruments:** Tail and latency return 0 (override in your processor if needed).

### auval Validation

Run `auval -a` to list registered Audio Units, then validate:

```bash
auval -v aufx MyPl Plup   # Effect: type/subtype/manufacturer
auval -v aumu MySy Plup   # Instrument
```

The type codes (`aufx`, `aumu`) and four-character codes are set in your AU's `Info.plist`, not in the Pulp code.

### Known Limitations

- Effects do not emit parameter output changes back to the host.
- AU v2 effects use `ProcessBufferLists` which receives interleaved audio. The adapter de-interleaves per buffer.
- No AU v3 adapter exists yet.
- Instruments use a `std::mutex` to buffer MIDI between the host's note callbacks and the render call. This is safe because Apple guarantees these calls occur on the same thread or with proper synchronization, but it adds a small overhead.

---

## Standalone Host

**Header:** `core/format/src/standalone.hpp`

`StandaloneApp` runs a Processor as a native desktop application with real audio I/O:

```cpp
pulp::format::StandaloneApp app(my_namespace::create_my_processor);

pulp::format::StandaloneConfig config;
config.sample_rate = 48000.0;
config.buffer_size = 256;
config.output_channels = 2;
config.input_channels = 2;  // 0 for instrument mode
app.set_config(config);

app.start();  // Blocks until stop() is called
```

Configuration options:

| Field | Default | Description |
|---|---|---|
| `audio_device_id` | `""` (system default) | Audio device identifier |
| `midi_input_id` | `""` (first available) | MIDI input device identifier |
| `sample_rate` | 48000.0 | Sample rate in Hz |
| `buffer_size` | 256 | Audio buffer size in samples |
| `output_channels` | 2 | Number of output channels |
| `input_channels` | 0 | Number of input channels (0 = no input) |

The standalone host manages `AudioSystem`, `AudioDevice`, `MidiSystem`, and `MidiInput` instances. MIDI is buffered with a mutex between the MIDI callback and the audio callback.

---

## HeadlessHost

**Header:** `<pulp/format/headless.hpp>`

`HeadlessHost` drives a Processor programmatically with no audio device, no UI, and no DAW. Use cases: CI tests, batch rendering, golden-file comparisons, benchmarks.

```cpp
pulp::format::HeadlessHost host(MyPlugin::create);
host.prepare(48000, 512);
host.state().set_value(kGainID, -6.0f);

pulp::audio::Buffer<float> in(2, 512), out(2, 512);
auto in_view = in.view();
auto out_view = out.view();
host.process(out_view, in_view);
```

Key methods:

| Method | Description |
|---|---|
| `prepare(sample_rate, max_buffer_size, in_ch, out_ch)` | Initialize the processor |
| `process(output, input)` | Process audio (no MIDI) |
| `process(output, input, midi_in, midi_out)` | Process audio with MIDI |
| `release()` | Release processing resources |
| `state()` | Access the `StateStore` for parameter reads/writes |
| `save_state()` | Serialize current parameter state to bytes |
| `load_state(data)` | Restore parameter state from bytes |
| `descriptor()` | Read the plugin's `PluginDescriptor` |

Input and output views may alias for in-place processing.

---

## Choosing Which Formats to Build

Each format is a separate CMake target. A typical `CMakeLists.txt` creates one target per format, all sharing the same processor source:

```cmake
# CLAP target
add_library(MyPlugin_CLAP MODULE
    src/my_processor.cpp
    src/clap_entry.cpp       # Contains PULP_CLAP_PLUGIN(...)
)
target_link_libraries(MyPlugin_CLAP PRIVATE pulp::format clap)

# VST3 target
add_library(MyPlugin_VST3 MODULE
    src/my_processor.cpp
    src/vst3_entry.cpp       # Contains PULP_VST3_PLUGIN(...)
)
target_link_libraries(MyPlugin_VST3 PRIVATE pulp::format vst3sdk)

# AU v2 target (macOS only)
if(APPLE)
    add_library(MyPlugin_AU MODULE
        src/my_processor.cpp
        src/au_entry.cpp     # Contains PULP_AU_PLUGIN(...)
    )
    target_link_libraries(MyPlugin_AU PRIVATE pulp::format AudioUnitSDK)
endif()
```

Each entry-point `.cpp` file includes the processor header and calls the format-specific macro. The processor code is identical across all targets.

---

## Comparison Table

| Feature | CLAP | VST3 | AU v2 |
|---|---|---|---|
| Entry macro | `PULP_CLAP_PLUGIN` | `PULP_VST3_PLUGIN` | `PULP_AU_PLUGIN` / `PULP_AU_INSTRUMENT` |
| Param values | Raw float | Normalized 0-1 | Raw float |
| Param modulation | Yes (`PARAM_MOD` events) | No | No |
| Param gestures | Yes (event-based) | Yes (`beginEdit`/`endEdit`) | Yes (`AUEventListenerNotify`) |
| MIDI in events | Yes (note events) | Yes (VST3 events) | Effects: no, Instruments: yes |
| State format | Binary via stream | Binary via `IBStream` | Binary in `CFDictionary` |
| Multi-bus declared | Yes | Yes | No |
| Plugin-side param output | Yes | Yes | Not yet |
| Latency reporting | Yes | Yes | Yes (seconds) |
| Tail reporting | Yes | Yes | Yes (seconds) |
| Stable ID | `bundle_id` string | `FUID` (128-bit) | Four-char codes in Info.plist |
