# Plugin Format Adapters

Pulp supports six native plugin formats (CLAP, VST3, AU v2, AU v3, LV2, and optional AAX)
plus standalone and headless hosts. You write one `Processor` subclass; format
adapters handle the rest.

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

**Host to plugin:** During `clap_process()`, the adapter iterates `in_events`,
looking for `CLAP_EVENT_PARAM_VALUE` events. Each one is pushed into the
per-block `ParameterEventQueue` and written with `StateStore::set_value_rt()`.
Gesture events (`CLAP_EVENT_PARAM_GESTURE_BEGIN` / `END`) are forwarded to
`StateStore::begin_gesture()` / `end_gesture()`.

The per-block queue is fixed-capacity and real-time safe. If a host sends more
than 1024 parameter value events in one block, the adapter keeps the first 1024
sample-accurate points, records the overflow/drop count on the queue, and still
writes every incoming value to `StateStore` so block-end reads observe the
latest host value.

The `params_flush()` extension callback handles the same events outside of `process()` (e.g., when the plugin is bypassed).

**Plugin to host:** Before calling `Processor::process()`, the adapter snapshots all parameter values. After processing, it compares each value against the snapshot. Any changed parameters are emitted as `CLAP_EVENT_PARAM_VALUE` events via `out_events->try_push()`. This allows hosts to record automation from plugin-side changes.

### CLAP Modulation

The adapter handles `CLAP_EVENT_PARAM_MOD` events. At the start of each process call, `store.reset_all_mod()` clears per-buffer modulation offsets. Incoming mod events are validated as global, control-rate `state::ModulationLane` routes before writing to `StateStore::set_mod_offset()`. Processors can call `store.get_modulated(id)` to read `base + mod_offset`.

### MIDI Routing

CLAP note and MIDI events are converted into Pulp's block event surfaces:

- `CLAP_EVENT_NOTE_ON` becomes `MidiEvent::note_on(channel, key, velocity * 127)`
- `CLAP_EVENT_NOTE_OFF` becomes `MidiEvent::note_off(channel, key, velocity * 127)`
- `CLAP_EVENT_NOTE_CHOKE` becomes a zero-velocity note-off
- `CLAP_EVENT_MIDI` carries raw MIDI 1.0 channel messages such as CC, pitch
  bend, channel pressure, poly pressure, and program change
- `CLAP_EVENT_MIDI_SYSEX` is copied into a preallocated inbound payload arena
  (128 events per block, 4096 bytes per event); overflow or larger payloads are
  dropped and counted. Outbound processor-owned SysEx emits as
  `CLAP_EVENT_MIDI_SYSEX`
- `CLAP_EVENT_MIDI2` is routed through `ump_input()` when the plugin opts into
  UMP
- `CLAP_EVENT_NOTE_EXPRESSION` feeds the MPE sidecar when the plugin opts into
  MPE
- `sample_offset` is set from `hdr->time` for sample-accurate timing

Note port declaration is driven by `descriptor().accepts_midi` and `descriptor().produces_midi`. Ports support both `CLAP_NOTE_DIALECT_CLAP` and `CLAP_NOTE_DIALECT_MIDI`, preferring CLAP dialect.

### State Save/Load

- **Save:** The adapter writes a host-facing blob containing `StateStore` plus
  any non-empty `Processor::serialize_plugin_state()` payload. When the
  processor-owned payload is empty, the adapter preserves the legacy raw
  `StateStore` blob for backward compatibility.
- **Load:** The adapter reads all bytes from `clap_istream_t` into a buffer
  (4 KB chunks), restores `StateStore`, then calls
  `deserialize_plugin_state()`. Older blobs without a processor-owned payload
  call `deserialize_plugin_state()` with empty bytes.

### Multi-Bus / Sidechain

Audio port count and info come from `descriptor().input_buses` and `descriptor().output_buses`. Each bus is reported with:

- Port ID: inputs start at 0, outputs at 100
- `CLAP_AUDIO_PORT_IS_MAIN` flag set on bus index 0
- Port type: `CLAP_PORT_MONO` for 1 channel, `CLAP_PORT_STEREO` otherwise
- `in_place_pair` set to `CLAP_INVALID_ID`

The process callback routes bus 0 as the main input/output and routes input bus
1 to `Processor::sidechain_input()` when present. Descriptor-declared secondary
output buses are routed through the richer `ProcessBuffers` surface for
processors that override it; processors that only implement the simple
`Processor::process()` signature write the main output and leave aux outputs
silent. Additional input buses beyond bus 1 are not exposed today.

`format::ProcessBuffers` and `format::ProcessBusBufferSet` are the additive
shared vocabulary for that richer surface. They are non-owning views over
host-owned bus buffers and let adapters validate active buses, declared channel
counts, and null channel pointers before projecting the current
main-in/main-out/sidechain view into `Processor::process()`.

Processors that override the richer surface can inspect no-input instrument
layouts, surround main outputs, and named auxiliary or stem outputs directly
through `ProcessBuffers::inputs` and `ProcessBuffers::outputs`. Use
`BusBufferSet::find()`, `find_by_index()`, or `find_by_name()` for secondary
buses; `main_input()`, `main_output()`, and `sidechain_input()` remain the
ergonomic compatibility helpers.

Inactive buses are treated as disconnected and should carry empty buffer views.
An active bus with any null channel pointer fails
`active_buses_have_storage()`, even when its declared channel count matches, so
adapters can fail closed or omit the bus instead of handing processors a
half-valid buffer.

### Latency and Tail

- **Latency:** `clap_plugin_latency_t::get` returns
  `processor->latency_samples()` after host-quirk clamping, so negative
  latency reports as 0 on the normal path.
- **Tail:** `clap_plugin_tail_t::get` returns `descriptor().tail_samples`. A
  value of `-1` (infinite tail) maps to `UINT32_MAX`.

### Known Limitations

- Bus 0 and one sidechain input are routed. Descriptor-declared secondary
  output buses are writable through `ProcessBuffers`; additional input buses
  beyond the sidechain are not exposed through the simple `Processor` process
  surface.
- Desktop CLAP plugin targets expose `CLAP_EXT_GUI` when built with
  `PULP_CLAP_GUI=1`; headless/CI/test environments and WCLAP builds do not
  expose a live desktop editor.
- Per-note modulation (`note_id`, `port_index`, `channel`, `key` fields in
  param mod events) is accepted but not per-note routed.

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

**Host to plugin:** During `process()`, the adapter reads
`data.inputParameterChanges`. Each point is denormalized, pushed into the
per-block `ParameterEventQueue`, and written with
`StateStore::set_normalized_rt()`. If a host sends more than 1024 points in one
block, the adapter keeps the first 1024 sample-accurate points, records the
overflow/drop count on the queue, and still writes every incoming point to
`StateStore` so block-end reads observe the latest host value.

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

MIDI controllers are host-mediated in VST3. For MIDI-accepting plug-ins, the
adapter implements `IMidiMapping` so hosts can map CC, pitch bend, and channel
aftertouch to hidden ParamIDs; `process()` decodes those parameter changes back
into sample-accurate MIDI messages. MPE-enabled plug-ins also expose
`INoteExpressionController`, routing VST3 tuning, volume, and brightness
note-expression events into Pulp's MPE sidecar.

### State Save/Load

- **Save (`getState`):** Writes a combined host-facing blob containing the
  `StateStore` payload plus any non-empty
  `Processor::serialize_plugin_state()` bytes.
- **Load (`setState`):** Reads the stream in 4 KB chunks, restores both layers,
  then syncs restored parameter values back to the VST3 parameter system via
  `setParamNormalized()`. Older blobs without a processor-owned payload still
  load and call `deserialize_plugin_state()` with empty bytes.

### Multi-Bus / Sidechain

Audio buses from `descriptor().input_buses` and `descriptor().output_buses` are added during `initialize()`:

- Bus type: `kMain` for required buses, `kAux` for optional (sidechain) buses
- Activity: `kDefaultActive` for required, 0 (inactive by default) for optional
- Speaker arrangement: `kMono` or `kStereo` based on `default_channels`
- Bus names are converted from `std::string` to VST3 `String128`

`setBusArrangements()` accepts any arrangement where at least one input and one output bus are present.

### Latency and Tail

- `getLatencySamples()` returns `processor->latency_samples()` after host-quirk
  clamping, so negative latency reports as 0 on the normal path.
- `getTailSamples()` returns `descriptor().tail_samples`, mapping `-1` to
  `kInfiniteTail`.

### Editor / GUI

`createView("editor")` returns a `PulpPlugView` when the VST3 target is built
with `PULP_VST3_GUI` and the processor reports an editor. Automation, headless,
CI, and test environments intentionally return `nullptr` so validators and
non-interactive runs do not launch editor windows.

### Known Limitations

- Bus 0, one sidechain input, and descriptor-declared secondary output buses
  are routed through `ProcessBuffers`. A multi-out processor that overrides
  `process(ProcessBuffers&)` writes each aux output bus; processors that only
  implement the simple callback leave aux buses silent.
- Dynamic bus arrangements are limited to descriptor-declared bus counts and mono/stereo layouts; unsupported layouts require host-quirk silence accommodation.

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

Use this macro for audio-only `aufx` effects. The factory function name must
match the `factoryFunction` in your AU's `Info.plist`.

### Macro (MIDI-receiving Effect)

```cpp
#include "my_processor.hpp"
#include <pulp/format/au_v2_entry.hpp>

PULP_AU_MIDI_PLUGIN(MyMidiEffectAU, my_namespace::create_my_processor)
```

`PULP_AU_MIDI_PLUGIN(ClassName, factory_fn)` generates the same effect adapter
class shape as `PULP_AU_PLUGIN`, but registers it through the AudioUnitSDK MIDI
effect factory so `MusicDeviceMIDIEvent` and SysEx selectors reach the adapter.
Pair it with an `aumf` component type: either let CMake emit `aumf` by setting
the processor descriptor's `accepts_midi = true` / `ACCEPTS_MIDI`, or keep a
custom `Info.plist.au` in sync manually.

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

**Host to plugin (effects and instruments):** `GetParameter()` and
`SetParameter()` are backed directly by the plugin `StateStore`, so AU host
automation playback, generic AU host UI edits, and preset recall all land in
the same store that `Processor::process()` reads. There is no per-buffer
`Globals()`-to-`StateStore` reconcile path. Initial AU parameter defaults are
seeded from the `StateStore` during construction so hosts can inspect them
before initialization.

**Editor/UI to host:** Editor/UI edits write the `StateStore` and the adapter's
inline store listener notifies the AU host with
`kAudioUnitEvent_ParameterValueChange`, guarded so host-originated
`SetParameter()` calls do not echo back into the host. This lets AU hosts
re-read via `GetParameter()` and record UI automation without calling
`AudioUnitSetParameter()` on the render path.

**Parameter event model:** AU v2 exposes current parameter values through the
AU parameter store rather than a render-event list. The effect adapter attaches
an empty `ParameterEventQueue` before `Processor::process()` so AU v2 effects
see the same non-null queue contract as other adapters. The instrument adapter
reads host parameters through the `StateStore` and does not expose a separate
AU v2 parameter-event sidecar. AU v2 parameter changes are block-rate
`StateStore` values today.

**Render-thread plugin output to host:** Parameter output changes made during
`Processor::process()` are not emitted back to the AU host. The render thread
neither pulls, pushes, nor notifies AU host parameters.

**Gesture callbacks:** The effect and instrument adapters wire `StateStore`
gesture callbacks to `AUEventListenerNotify()` with
`kAudioUnitEvent_BeginParameterChangeGesture` and
`kAudioUnitEvent_EndParameterChangeGesture` event types.

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

**Audio-only effects (`aufx`):** AU hosts do not route MIDI to plain effect
components. Use `PULP_AU_PLUGIN` only when the processor does not accept MIDI.

**MIDI-receiving effects (`aumf`):** Use `PULP_AU_MIDI_PLUGIN` and package the
component as `aumf`. Host MIDI and SysEx arrive via `HandleMIDIEvent()` /
`HandleSysEx()`, are queued on bounded lock-free queues, and are drained into
`midi_in` at the start of each `ProcessBufferLists()` call. Sample offsets from
`inStartFrame` are preserved for short MIDI messages.

**Instruments (`aumu`):** MIDI notes arrive via `HandleNoteOn()` and
`HandleNoteOff()` callbacks. These are buffered in `pending_midi_` (protected by
a mutex) and drained into `midi_in` at the start of each `Render()` call. The
sample offset (`inStartFrame`) is preserved.

### State Save/Load

The AU adapter stores Pulp state alongside the standard AU state dictionary:

- **Save:** Calls `AUEffectBase::SaveState()` (or `MusicDeviceBase::SaveState()`),
  then appends a `CFData` blob under the key `"pulp-state"`. That blob contains
  the parameter-only `StateStore` payload plus any non-empty
  `Processor::serialize_plugin_state()` bytes.
- **Load:** Calls the base `RestoreState()`, then looks for the `"pulp-state"`
  key. If found, restores both layers and syncs all parameter values back to
  the AU parameter system via `Globals()->SetParameter()`. Older blobs with only
  raw `StateStore` data still load and call `deserialize_plugin_state()` with
  empty bytes.

### Tail and Latency

**Effects:** `GetTailTime()` converts `descriptor().tail_samples` to seconds by dividing by sample rate. `GetLatency()` does the same for `latency_samples()`. A tail of `-1` maps to `infinity`.

**Instruments:** `GetTailTime()` and `GetLatency()` report the same processor
runtime contract as effects, using the output stream sample rate. A tail of `-1`
maps to `infinity`.

### auval Validation

Run `auval -a` to list registered Audio Units, then validate:

```bash
auval -v aufx MyPl Plup   # Effect: type/subtype/manufacturer
auval -v aumu MySy Plup   # Instrument
```

The type codes (`aufx`, `aumu`) and four-character codes are set in your AU's `Info.plist`, not in the Pulp code.

### Known Limitations

- Effects do not emit parameter output changes back to the host.
- AU v2 effects can receive MIDI as `aumf`, but outgoing MIDI from `midi_out` is not emitted back to the host yet.
- AU v2 effects use `ProcessBufferLists` which receives interleaved audio. The adapter de-interleaves per buffer.
- Instruments use a `std::mutex` to buffer MIDI between the host's note callbacks and the render call. This is safe because Apple guarantees these calls occur on the same thread or with proper synchronization, but it adds a small overhead.

---

## AU v3 (AUAudioUnit — app extension)

**Entry point:** `pulp_add_plugin(... FORMATS AUv3 ...)` generates a
`.appex` bundle via the CMake helper `_pulp_add_auv3()` in
`tools/cmake/PulpUtils.cmake`. On iOS, use the wrapper
`pulp_add_ios_auv3()` — see [`ios-auv3-guidance.md`](ios-auv3-guidance.md)
for the iOS-specific entitlement, bundle-id, and signing story.

Unlike AU v2, AU v3 plugins are **always app extensions** and always run
sandboxed. This is Apple's deployment model for AUv3; there is no non-
sandboxed AUv3 form. In a compatible host (Logic Pro, GarageBand, Cubase,
AUM, AUAudioUnit-aware DAWs), the host loads the `.appex` as an out-of-
process extension with its own audio-unit view controller.

Status: `experimental` on macOS and iOS per
[`docs/status/support-matrix.yaml`](../status/support-matrix.yaml). AUv3 ships
today but hasn't yet passed the host-compatibility bar (auval v2 validation,
Logic Pro + GarageBand + AUM cross-check) that would justify promotion to
`usable`; see the status manifest for the current promotion criteria.

### State Save/Load

AU v3 mirrors the AU v2 state contract through `AUAudioUnit.fullState`:

- **Save:** `fullState` writes a `NSData` payload under the key `"pulpState"`.
  That payload contains the parameter-only `StateStore` bytes plus any
  non-empty `Processor::serialize_plugin_state()` blob.
- **Load:** `setFullState:` reads `"pulpState"` and restores both layers.
  Older blobs that contain only raw `StateStore` data still load and call
  `deserialize_plugin_state()` with empty bytes.

### Parameter Events

AU v3 receives sample-accurate `AURenderEventParameter` and
`AURenderEventParameterRamp` events in the render event list. The adapter writes
each event into the realtime `ParameterEventQueue` and also updates
`StateStore` with the latest value. The queue is fixed-capacity; events beyond
capacity are dropped from the sparse queue and counted as overflow, while the
latest value still reaches `StateStore` for block-rate reads.

### Multi-Bus / Sidechain

AU v3 exposes descriptor input bus 0 as the main input and descriptor input bus
1 as an optional sidechain `AUAudioUnitBus` when it has a positive channel
count. The render block pulls input bus 1 separately, publishes it through
`Processor::set_sidechain()`, and includes it as the sidechain bus in
`ProcessBuffers`.

Additional input buses and secondary output buses are not exposed through the
AUv3 adapter surface yet; the adapter creates one main output bus.

---

## LV2

**Entry point header:** `<pulp/format/lv2_entry.hpp>`

### Macro

```cpp
#include "my_processor.hpp"
#include <pulp/format/lv2_entry.hpp>

PULP_LV2_PLUGIN(my_namespace::create_my_processor,
                "http://example.com/plugins/my-plugin")
```

`PULP_LV2_PLUGIN(factory_fn, plugin_uri)` generates:

- An exported `lv2_descriptor(uint32_t index)` entry point
- Static-init registration that attaches the plugin URI to the descriptor
- `instantiate`, `connect_port`, `activate`, `run`, `deactivate`, and `cleanup` callbacks

The plugin URI must be stable across versions. It is the plugin's identity in every LV2 host and is the key used by the bundle's TTL files.

### Status

`experimental` on Linux per
[`docs/status/support-matrix.yaml`](../status/support-matrix.yaml). The
adapter loads in real LV2 hosts, routes audio + control ports, and handles
atom MIDI input and output for 1–3-byte short messages. Sysex through atom
sequences and broader control/atom coverage are still in progress.

### Parameter and Audio Ports

`connect_port` maps indices to roles in this order:

1. Audio input ports (count from `descriptor().input_buses[...].default_channels`)
2. Audio output ports
3. Control input ports (one per parameter)
4. Atom input port (present when `descriptor().accepts_midi == true`)
5. Atom output port (present when `descriptor().produces_midi == true`)

`run()` reads control-input port values into `StateStore` at the top of each buffer.
LV2 control ports are block-rate current values, not scheduled sparse
parameter events. The adapter still attaches an empty
`ParameterEventQueue` before `Processor::process()` so
`Processor::param_events()` is non-null, but control-port ingress does not
consume sparse queue capacity; large host-block capacity for LV2 is ordinary
control-port count/state update behavior.

### MIDI Routing

During `instantiate()` the adapter resolves the host-supplied
`LV2_URID__map` feature and caches URIDs for `atom:Sequence`, `atom:Chunk`,
and `midi:MidiEvent`. Hosts that do not provide `LV2_URID__map` are
refused loudly (`instantiate()` returns `nullptr`).

`run()` walks the connected `LV2_Atom_Sequence` input and promotes each
short (1–3 byte) MIDI event into a `pulp::midi::MidiEvent` with
`sample_offset = ev->time.frames`. Outgoing MIDI written into the
`Processor::process()` `midi_out` buffer is serialized back into the atom
output port via `lv2_atom_sequence_append_event()`; events that don't fit
are dropped rather than truncated.

### State Save/Load

The current adapter does not implement the LV2 `state:interface` extension.
Presets authored via the host work, but Pulp-defined `StateStore`
serialization is not yet exposed over the LV2 state path.

### Bundle Layout

LV2 plug-ins ship as a `.lv2/` directory containing the compiled shared
object plus one or more `.ttl` files that describe ports and metadata.
`pulp_add_plugin(... FORMATS LV2 ...)` writes the shared object into
`build/LV2/<name>.lv2/`; the matching TTL is generated by
`generate_plugin_ttl()` so port indices always agree with the adapter.

### Known Limitations

- Atom sysex events are ignored — only 1–3-byte short MIDI messages are
  routed through the LV2 atom input sequence.
- No `state:interface` implementation yet; host-side preset save/restore
  works, but Pulp's own `StateStore` binary state is not exposed through
  the LV2 state extension.
- Worker extension, UI extension (`ui:UI` / `ui:Qt5UI` / `ui:X11UI`), and
  `lv2:CVPort` support are not wired.
- Bundle TTL discovery in `core/host/src/plugin_slot_lv2.cpp` uses a tiny
  regex scanner intentionally; complex multi-plugin bundles that rely on
  blank-node syntax beyond the common form may not be discovered correctly.

---

## AAX (optional)

**Entry point header:** `<pulp/format/aax_entry.hpp>`

### Macro

```cpp
#include "my_processor.hpp"
#include <pulp/format/aax_entry.hpp>

PULP_AAX_PLUGIN(my_namespace::create_my_processor)
```

`PULP_AAX_PLUGIN(factory_fn)` generates:

- A `GetEffectDescriptions()` export for the AAX host
- metadata generation from `Processor::descriptor()`
- Parameter, state, latency, transport, and MIDI registration through the AAX runtime

### Status

`experimental` on macOS and Windows, `unsupported` on Linux per
[`docs/status/support-matrix.yaml`](../status/support-matrix.yaml). AAX is
intentionally opt-in: the adapter compiles and loads in Pro Tools, but
custom editor surface, AudioSuite role exercise, and public-CI coverage
remain incomplete. The SDK is developer-supplied and never bundled or
shipped by Pulp.

### Build Requirements

AAX is intentionally opt-in:

- Supported only on macOS and Windows
- Requires `PULP_ENABLE_AAX=ON`
- Requires `PULP_AAX_SDK_DIR` to point to a developer-supplied out-of-tree AAX SDK
- Requires `aax_entry.cpp` in the plugin source directory

Typical CMake usage:

```cmake
pulp_add_plugin(MyPlugin
    FORMATS VST3 AU CLAP AAX Standalone
    PLUGIN_NAME "MyPlugin"
    BUNDLE_ID "com.example.myplugin"
    MANUFACTURER "Example Audio"
    MANUFACTURER_CODE "Exmp"
    AAX_PRODUCT_CODE "ExPl"
    AAX_NATIVE_CODE "ExPn"
)
```

Linux and Ubuntu do not support AAX. If `FORMATS AAX` is requested there,
configuration fails with an explicit error.

### Format Model

The current AAX adapter supports:

- Effect, instrument, and MIDI-effect categories
- One main output bus
- One main input bus plus one optional mono sidechain
- State save/load
- Host automation and latency reporting
- Transport access
- Local MIDI input/output nodes when declared by the processor

### Validation

When DigiShell + AAX Validator are installed locally:

- `pulp validate` runs a fast describe-validation probe for each `.aaxplugin`
- `pulp validate --all` runs the fuller AAX validator suite

If the validator is missing, the CLI reports a guided skip and points to the
Avid download page instead of guessing.

### Known Limitations

- Native AAX only. DSP and AudioSuite are out of scope.
- No AAX GUI layer yet. Current validator output may warn that the effect does not contain `EffectGUI`.
- Public CI does not build or validate AAX because the SDK and validator are not bundled by Pulp.
- Component layouts are intentionally constrained to keep the surface small.

For setup, download, and rules, see [AAX Setup](aax.md).

---

## Standalone Host

**Header:** `<pulp/format/standalone.hpp>`

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
| `try_prepare(sample_rate, max_buffer_size, in_ch, out_ch, limits)` | Initialize only if the processor's prepare-resource estimate fits the supplied non-zero limits |
| `process(output, input)` | Process audio (no MIDI) |
| `process(output, input, midi_in, midi_out)` | Process audio with MIDI |
| `render_offline(input, options)` | Render effect-shaped `AudioFileData` through deterministic offline blocks |
| `release()` | Release processing resources |
| `state()` | Access the `StateStore` for parameter reads/writes |
| `save_state()` | Serialize current plugin state to bytes |
| `load_state(data)` | Restore parameter and plugin-owned state from bytes |
| `descriptor()` | Read the plugin's `PluginDescriptor` |

Use `try_prepare()` when a test, batch render, or benchmark needs to prove a
plugin fails closed before allocating oversized prepare-time resources:

```cpp
pulp::format::PrepareResourceLimits limits;
limits.max_total_bytes = 8 * 1024 * 1024;
limits.max_voices = 64;

if (!host.try_prepare(48000, 512, 2, 2, limits)) {
    auto reason = host.last_prepare_limit_failure();
    // Report or assert the first exceeded budget.
}
```

When `try_prepare()` fails a non-zero limit, it returns before
`Processor::prepare()` and leaves the previous successful prepared render
context intact. Use `last_prepare_limit_failure()` for diagnostics, then either
continue rendering with the prior prepare or retry with adjusted limits.
If the host also reports memory pressure, a processor may shed rebuildable
owner-thread caches before the retry, but it must keep prepared core state and
fixed per-block scratch accounting valid.

Input and output views may alias for in-place processing.

Use `render_offline()` when a batch/golden path already has an
`AudioFileData` artifact and needs `OfflineRenderOptions` metadata forwarded to
`ProcessContext`: scheduled block size, sample position, tempo, beat position,
and render-speed hint. It is effect-shaped today, so the rendered artifact has
the same channel count as the input. Parity fixtures should compare both the
rendered samples and the per-block `ProcessContext` metadata against direct
stepped processing for the same schedule.

Format adapter runtime-mode tests should assert the adapter-owned source of
truth rather than inferring mode from transport. VST3 maps
`ProcessSetup::processMode == kOffline` to `ProcessMode::Offline` with a
faster-than-realtime render hint. CLAP has no equivalent process-mode field, so
its adapter reports realtime mode and realtime render speed unless a future CLAP
extension exposes stronger host intent. AU v2 effect and instrument render
callbacks also report realtime mode and realtime render speed because the v2 SDK
does not surface offline-bounce intent to `ProcessBufferLists()` / `Render()`;
AU v3 mirrors that explicit realtime render-path contract. Bypass, tail-drain,
reset, and transport-jump flags stay explicit `ProcessContext` metadata and
should be covered where the host API can actually deliver them. VST3 process
context sample-position discontinuities, AU v2 host-callback sample-position
discontinuities, AU v3 transport-state sample-position discontinuities, and
CLAP beat-timeline discontinuities are diffed against the previous block and set
`transport_jump`, so processors that use `should_reset_dsp_state()` can reset
delay, lookahead, or oscillator state on host seeks.

---

## Audio precision (f32 / f64)

Pulp processes audio in 32-bit float by default, and supports 64-bit float
end-to-end as an opt-in. The contract:

- `Processor::process(...)` is the f32 surface every plugin implements.
- `Processor::process_f64(...)` (both the simple `BufferView<double>` form and
  the richer `ProcessBuffers64` form) is additive. The default implementation
  is an allocation-free fallback that converts f64→f32 at the boundary, runs
  your f32 `process()`, and converts back — so every Pulp plugin works in a
  64-bit host with no extra code.
- To process natively in double precision, set
  `caps.supports_f64_audio = true` in the descriptor's capabilities and
  override `process_f64`. The `core/signal` DSP classes are sample-type
  templated (`Gain64`, `Biquad64`, …) and the SIMD kernels have f64 variants,
  so a genuinely double-precision chain is available throughout.
  `examples/pulp-gain` is the reference native-f64 plugin.

Which surfaces can actually carry 64-bit audio is fixed by each host API:

| Surface | Host can deliver f64? | Behavior |
|---|---|---|
| VST3 | Yes (`kSample64`) | Accepted unconditionally (`canProcessSampleSize`). Native dispatch when `supports_f64_audio`, otherwise boundary conversion. |
| CLAP / WebCLAP | Yes (`data64`) | All ports advertise `CLAP_AUDIO_PORT_SUPPORTS_64BITS`; native-f64 plugins also advertise `PREFERS_64BITS`. Native dispatch requires every active routed bus to be 64-bit; mixed 32/64 blocks demote to the f32 boundary path. |
| AU v2 / AU v3 | No — the AU render ABI is `Float32` | f32 only, by format. |
| AAX | No — AAX audio buffers are 32-bit float | f32 only, by format. |
| LV2 | No — `lv2:AudioPort` is `float` | f32 only, by format. |
| Standalone | No — the device layer runs 32-bit float streams | f32 only. |
| WAM (web) | No — Web Audio is f32 | f32 only. |
| HeadlessHost / ValidationHarness / Python / Node bindings | Yes (API) | `process_f64` passes straight through; non-native plugins hit the single boundary conversion in the core fallback. |

Additional precision notes:

- **Parameters** are 32-bit floats (normalized at the store); host automation
  values arriving as doubles narrow at the store boundary. Transport and
  timing fields on `ProcessContext` are doubles.
- **GPU audio, SignalGraph, PluginSlot, and the experimental live-kernel are
  f32 surfaces by policy.** A native-f64 processor that routes through them is
  narrowed to f32 at that node boundary.
- **Latency-compensated bypass** runs its dry-delay line in f32 even under a
  64-bit host, so bypassed passthrough with nonzero reported latency carries an
  f32-precision floor; non-delayed bypass is a bit-exact copy.
- Denormal flushing (`ScopedFlushDenormals`) wraps the audio callback on both
  the f32 and f64 paths — FTZ/DAZ on x86 and FPCR.FZ on arm64 apply to both
  sample widths.

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

| Feature | CLAP | VST3 | AU v2 | AU v3 |
|---|---|---|---|---|
| Entry macro | `PULP_CLAP_PLUGIN` | `PULP_VST3_PLUGIN` | `PULP_AU_PLUGIN` / `PULP_AU_INSTRUMENT` | `pulp_add_plugin(... FORMATS AUv3 ...)` |
| Param values | Raw float | Normalized 0-1 | Raw float | Raw float |
| Param modulation | Yes (`PARAM_MOD` events) | No | No | No |
| Param gestures | Yes (event-based) | Yes (`beginEdit`/`endEdit`) | Yes (`AUEventListenerNotify`) | Yes (`AUParameterTree`) |
| MIDI in events | Yes (note events) | Yes (VST3 events) | Effects: `aumf` yes / `aufx` no, Instruments: yes | Yes (raw bytes) |
| State format | Binary via stream | Binary via `IBStream` | Binary in `CFDictionary` | Binary in `fullState` |
| Multi-bus declared | Yes | Yes | No | Main input + sidechain input |
| Editor/UI param write-back | Yes | Yes | Yes (`AUEventListenerNotify`) | Yes (`AUParameterTree`) |
| Render-thread param output | Yes | Yes | Not yet | Yes |
| Latency reporting | Yes | Yes | Yes (seconds) | Yes (seconds) |
| Tail reporting | Yes | Yes | Yes (seconds) | Yes (seconds) |
| Stable ID | `bundle_id` string | `FUID` (128-bit) | Four-char codes in Info.plist | Bundle identifier |

Optional AAX uses its own runtime and follows the constraints listed
in the AAX section above.
