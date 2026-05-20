# State and Parameter System

The state subsystem provides thread-safe, automatable parameters for audio plugins. It handles the full lifecycle: registration, real-time access, host automation, UI binding, modulation, and state serialization.

## Core Types

### ParamID

Every parameter has a unique `uint32_t` identifier that must be stable across plugin versions. Use a hash or manual assignment.

```cpp
constexpr pulp::state::ParamID kGain = 1;
constexpr pulp::state::ParamID kPan  = 2;
constexpr pulp::state::ParamID kMix  = 3;
```

### ParamRange

Defines the numeric range and normalization of a parameter. Hosts work in normalized [0, 1] space; Pulp maps between real and normalized values automatically.

```cpp
ParamRange{.min = -60.0f, .max = 12.0f, .default_value = 0.0f}  // dB
ParamRange{.min = 20.0f, .max = 20000.0f, .default_value = 1000.0f}  // Hz
ParamRange{.min = 0.0f, .max = 1.0f, .default_value = 0.5f, .step = 0.01f}  // quantized
```

If `step > 0`, values snap to the nearest step when denormalized.

### ParamInfo

Immutable metadata registered once at initialization.

```cpp
store.add_parameter({
    .id = kGain,
    .name = "Gain",
    .unit = "dB",
    .range = {-60.0f, 12.0f, 0.0f},
    .group_id = 0,
    .to_string = [](float v) { return std::to_string(v) + " dB"; },
    .from_string = [](const std::string& s) { return std::stof(s); },
});
```

Fields:
- `id` — unique stable identifier
- `name` — display name for host and UI
- `unit` — display unit string ("dB", "Hz", "%", "ms")
- `range` — min/max/default/step
- `group_id` — for hierarchical organization (0 = ungrouped)
- `to_string` — optional display formatter
- `from_string` — optional string parser
- `rate` — `ControlRate` by default; `AudioRate` opts into future audio-rate graph edges
- `smoothing_ramp_seconds` — optional control-rate smoothing time; `0` means off

### ParamValue

Thread-safe atomic value for lock-free audio/UI communication. Uses `std::atomic<float>` with relaxed ordering.

```cpp
ParamValue gain(0.0f);

// Audio thread reads:
float g = gain.get();
float g_mod = gain.get_modulated();  // base + modulation offset

// UI thread writes:
gain.set(-6.0f);
```

Why relaxed ordering is safe: each parameter is independent. There is no dependency between reading param A and param B. For coherent multi-field reads (e.g., transport state), use `SeqLock<T>` instead.

## StateStore

Centralized parameter storage. Owns all `ParamValue` instances and provides the thread-safe access API.

### Registration

Called once in `Processor::define_parameters()`:

```cpp
void define_parameters(state::StateStore& store) override {
    store.add_parameter({.id = kGain, .name = "Gain", .unit = "dB",
                         .range = {-60.0f, 12.0f, 0.0f}});
    store.add_parameter({.id = kPan, .name = "Pan", .unit = "",
                         .range = {-1.0f, 1.0f, 0.0f}});

    // Optional: organize into groups
    store.add_group({.id = 1, .name = "Dynamics"});
    store.add_parameter({.id = kThreshold, .name = "Threshold", .unit = "dB",
                         .range = {-60.0f, 0.0f, -20.0f}, .group_id = 1});
}
```

### Reading Values (Audio Thread)

```cpp
void process(...) override {
    float gain_db = state().get_value(kGain);        // raw value
    float gain_mod = state().get_modulated(kGain);   // base + CLAP modulation
    float gain_norm = state().get_normalized(kGain);  // [0, 1]
}
```

All reads are lock-free (atomic loads, relaxed ordering). Safe for the audio thread.

### Writing Values

```cpp
state().set_value(kGain, -6.0f);           // raw value
state().set_normalized(kGain, 0.5f);       // from normalized
state().reset_to_default(kGain);           // back to default
state().reset_all_to_defaults();           // reset everything
```

### Modulation (CLAP)

CLAP hosts can modulate parameters per-voice. The modulation offset is separate from the base value:

```cpp
state().set_mod_offset(kCutoff, 0.2f);     // absolute offset
state().add_mod_offset(kCutoff, 0.05f);    // stack on top
state().reset_all_mod();                    // clear all offsets

// In process(), read the modulated value:
float cutoff = state().get_modulated(kCutoff);  // base + offset
```

The CLAP adapter handles `CLAP_EVENT_PARAM_MOD` events and calls `set_mod_offset()` / `add_mod_offset()` before each process block.

## Sample-Accurate Automation

Format adapters preserve host automation points in a per-block
`ParameterEventQueue` and expose it during `Processor::process()`:

```cpp
void process(audio::BufferView<float>& output,
             const audio::BufferView<const float>& input,
             midi::MidiBuffer&,
             midi::MidiBuffer&,
             const format::ProcessContext&) override {
    if (const auto* events = param_events()) {
        for (const auto& event : *events) {
            // event.param_id, event.sample_offset, event.value
        }
    }
}
```

Adapters still dual-write `StateStore` before `process()` so old processors
continue to see the block-end value from `state().get_value(id)`. Processors
that split a block should seed a `ParamCursor` with their own pre-automation
snapshot:

```cpp
std::array<format::ParamSnapshotEntry, 1> initial{{
    {kGain, previous_gain_},
}};

format::for_each_subblock(output, input, state(), param_events(), initial,
    [&](auto& out, const auto& in, const format::ParamCursor& params) {
        const float gain = params.value(kGain);
        // Process this sub-block with a coherent gain value.
    });

previous_gain_ = state().get_value(kGain);
```

`for_each_subblock` never mutates `StateStore`; it advances a block-local
cursor as event offsets are crossed. `ParamInfo::smoothing_ramp_seconds` and
`format::ControlRateParamSmoother` provide an opt-in ramp for processors that
want click-free block-rate changes without splitting into sub-blocks.

## Binding (UI Integration)

`Binding` wraps a parameter with reactive change notification and gesture tracking. Use it in UI widgets.

### Creating Bindings

```cpp
Binding gain_binding(store, kGain);

// Or create bindings for all parameters at once:
auto all_bindings = create_bindings(store);
```

### Reading and Writing

```cpp
float value = gain_binding.get();
float norm = gain_binding.get_normalized();
gain_binding.set(-6.0f);
gain_binding.set_normalized(0.5f);
gain_binding.reset();  // back to default
```

### Gesture Tracking

Wrap user interactions (mouse drag, scroll) in begin/end gesture calls. This tells the host to group the changes into one undo step:

```cpp
// On mouse down:
gain_binding.begin_gesture();

// On mouse drag:
gain_binding.set_normalized(knob_position);

// On mouse up:
gain_binding.end_gesture();
```

### Change Notification

Register callbacks that fire when the value changes (from user interaction or host automation):

```cpp
gain_binding.on_change([&](float new_value) {
    knob.repaint();
});
```

### Polling for External Changes

Host automation changes parameters without going through the Binding. Call `poll()` periodically from the UI thread to detect these:

```cpp
// In a timer callback (e.g., 30 Hz):
if (gain_binding.poll()) {
    // Value changed externally — UI will be updated via on_change callback
}
```

## State Serialization

StateStore serializes all parameter values to a binary format for DAW project save/load:

```cpp
// Save
std::vector<uint8_t> data = store.serialize();

// Load
bool ok = store.deserialize(data);
```

The format includes a version number and CRC32 checksum:

```
[4 bytes: version] [4 bytes: param count] [per param: 4 bytes id + 4 bytes value] [4 bytes: CRC32]
```

Use `set_state_version()` for forward compatibility when adding parameters in new plugin versions.

## Processor-Owned Plugin State

`StateStore` is intentionally limited to flat, automatable parameters. Anything
that should survive host/session recall but should not appear in the host's
automation lane belongs in `Processor`'s plugin-owned state hooks instead:

```cpp
std::vector<uint8_t> serialize_plugin_state() const override;
bool deserialize_plugin_state(std::span<const uint8_t> data) override;
```

Typical examples:

- snapshot banks or scene slots
- variable layouts that change what the parameters mean
- editor/model state stored as `StateTree` JSON or a custom binary payload

Format adapters, `HeadlessHost`, and `ValidationHarness` save an outer
host-facing blob. The inner `StateStore` payload remains the same parameter-only
binary format shown above. If `serialize_plugin_state()` returns an empty blob,
Pulp preserves the legacy raw `StateStore` format for backward compatibility.

`deserialize_plugin_state()` receives an empty span when loading an older blob
that contains only `StateStore` data. Override implementations should treat
empty input as "reset persisted plugin-owned state to defaults".

## Thread Model

| Thread | Can Read | Can Write | Mechanism |
|--------|----------|-----------|-----------|
| Audio thread | `get_value()`, `get_modulated()` | `set_value_rt()` for host-driven writes | `std::atomic` relaxed + SPSC listener queue |
| UI thread | `Binding::get()`, `poll()` | `Binding::set()` | `std::atomic` relaxed |
| Host thread | `serialize()` | `deserialize()`, `set_value()` | `std::atomic` relaxed, mutex for listeners |

The generic listener path can allocate when main-thread listeners are
marshalled through an event loop. Format adapters use `set_value_rt()` on the
audio thread, which writes the atomic value and defers main-thread listener
callbacks through a bounded SPSC queue drained by the UI tick.

## Format Adapter Integration

Format adapters sync parameters bidirectionally:

- **Host → Plugin**: adapters read host parameter changes, preserve sample offsets in `ParameterEventQueue`, attach the queue via `Processor::param_events()`, and call `store.set_value_rt()` before `process()` for legacy block-end reads
- **Plugin → Host**: adapters snapshot values before `process()`, then emit output events for any changes after
- **UI → Host**: `Binding::begin_gesture()` / `end_gesture()` forward to host undo system
- **CLAP modulation**: adapter handles `CLAP_EVENT_PARAM_MOD` and calls `set_mod_offset()` / `add_mod_offset()`

Host/project save-load uses both layers:

- **Automatable state**: `StateStore::serialize()` / `deserialize()`
- **Opaque plugin-owned state**: `Processor::serialize_plugin_state()` / `deserialize_plugin_state()`
