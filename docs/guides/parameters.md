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

## Thread Model

| Thread | Can Read | Can Write | Mechanism |
|--------|----------|-----------|-----------|
| Audio thread | `get_value()`, `get_modulated()` | `set_value()` (for output params) | `std::atomic` relaxed |
| UI thread | `Binding::get()`, `poll()` | `Binding::set()` | `std::atomic` relaxed |
| Host thread | `serialize()` | `deserialize()`, `set_value()` | `std::atomic` relaxed, mutex for listeners |

The listener mutex in StateStore is only held when calling change listeners — not during atomic reads/writes. If the host calls `set_value()` from the audio thread, listeners may briefly block. For production use, consider draining listener notifications on the UI thread via a lock-free queue.

## Format Adapter Integration

Format adapters sync parameters bidirectionally:

- **Host → Plugin**: adapters read host parameter changes and call `store.set_value()` before `process()`
- **Plugin → Host**: adapters snapshot values before `process()`, then emit output events for any changes after
- **UI → Host**: `Binding::begin_gesture()` / `end_gesture()` forward to host undo system
- **CLAP modulation**: adapter handles `CLAP_EVENT_PARAM_MOD` and calls `set_mod_offset()` / `add_mod_offset()`
