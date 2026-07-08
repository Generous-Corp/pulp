---
name: clap
description: CLAP format adapter for Pulp — how Processor bridges to clap_plugin_t, how parameters / modulation / sidechain / MPE / UMP / sysex flow, and the pitfalls discovered while wiring the adapter.
---

# CLAP Skill

Use this skill when touching Pulp's CLAP adapter, when answering
questions about how a Pulp plugin appears to a CLAP host, or when a
CLAP validator run surfaces something odd. CLAP is Pulp's first-class,
MIT-safe plugin format — every plugin built with Pulp ships a CLAP
binary and CLAP is the fastest iteration lane because the
`clap-validator` runs without a DAW.

## When to use

- Editing `core/format/src/clap_adapter.cpp` or the generated entry header
  `core/format/include/pulp/format/clap_entry.hpp` (the boilerplate-
  generator macro `PULP_CLAP_PLUGIN(…)`).
- Adding or changing a CLAP extension (`audio-ports`, `note-ports`,
  `params`, `state`, `gui`, `preset-load`, ARA companion factory, …).
- A CLAP host reports a behaviour issue — sidechain missing, MIDI
  events dropped, presets not loading, GUI refusing to attach.
- A `clap-validator` pass regresses.
- Working on the MPE or UMP sidecar as it flows through the CLAP path
  — the CLAP adapter is currently the canonical consumer of
  `set_mpe_input` / `set_ump_input`.
- Cross-referencing CLAP behaviour against VST3 / AU when debugging
  host-specific regressions — use the three adapters as each other's
  "oracle" during parity fixes.

## Files and entry points

| Role | Path |
|---|---|
| Core adapter (C++) | `core/format/src/clap_adapter.cpp` |
| Adapter header / `PulpClapPlugin` | `core/format/include/pulp/format/clap_adapter.hpp` |
| Entry-point generator macro | `core/format/include/pulp/format/clap_entry.hpp` |
| CLAP module (FetchContent) | declared in `CMakeLists.txt`; CLAP headers are MIT and fetched at configure time — there is no hand-written `PulpClap.cmake` |
| WebAssembly compact variant (wclap) | `tools/cmake/PulpWclap.cmake`, `core/format/src/wasm/` |
| Remote-control page builder | `core/format/src/clap_remote_controls.cpp` |
| CLAP+ARA surface | `core/format/src/ara*`, see the `ara` skill |
| Tests | `test/test_clap_entry.cpp` (dlopen + descriptor), `test/test_clap_ara_extension.cpp` (ARA companion factory), `test/test_clap_webview.cpp` (WebView bridge) |
| CLI validator invocation | `tools/cli/cmd_validate.cpp` (`clap-validator validate …` with dlopen-only fallback) |

The `PULP_CLAP_PLUGIN(factory_fn)` macro (bottom of `clap_entry.hpp`)
is the sole developer-facing surface. It expands to the static
`g_factory` initialisation, calls `register_plugin(factory_fn)`, fills
in `g_clap_desc` from the `PluginDescriptor`, and defines the
`clap_entry` exported symbol. There is no separate "factory" TU — the
macro is the factory.

## Core conventions

### The shape of a CLAP instance

`PulpClapPlugin` (in `clap_adapter.hpp`) is the shared per-instance
struct. It owns:

- `std::unique_ptr<Processor> processor` — the user's DSP.
- `state::StateStore store` — parameter state, wired to the processor
  via `set_state_store(&store)` during `clap_init`.
- Pre-allocated `input_ptrs` / `output_ptrs` / `sidechain_ptrs` arrays
  sized to `kMaxChannels = 8`. **Process must not allocate** — these
  pointer fan-outs are static across calls.
- `param_snapshot` for detecting plugin-side parameter edits during
  `process()`. After `processor->process()`, the adapter compares each
  param to its snapshot and emits `CLAP_EVENT_PARAM_VALUE` out-events
  so the host can record automation.
  - **RT-safety invariant:** every scratch vector `process()` sizes to
    `all_params().size()` (via `resize`/`assign`) runs on the audio thread,
    so each MUST be `reserve()`d to that size in `clap_activate()` — otherwise
    the first block grows-and-allocates. `param_snapshot` **and**
    `output_param_has_event` (the sample-accurate-output skip-set) are both
    reserved there. Adding a new per-param scratch means adding its reserve;
    the `ClapSlot::process is allocation-free` rt-safety test catches a miss
    (it measures the first block, where the missing reserve shows up).
- `param_events` (`state::ParameterEventQueue`) for the current block's
  inbound `CLAP_EVENT_PARAM_VALUE` events. It preserves every host
  automation point with `header.time` before the StateStore dual-write
  lands the last value for ordinary parameter reads.
- `mpe_tracker` + `mpe_buffer` + `mpe_enabled` — MPE sidecar populated
  only if `PluginDescriptor::effective_capabilities().supports_mpe`
  is true. The effective value ORs the legacy descriptor flag with the
  node ABI capability field. `clap_activate()` reserves and capacity-limits
  the sidecar too; one MIDI event can fan out to many MPE callbacks.
- `ump_buffer` + `ump_enabled` — UMP sidecar. Cleared at the top of
  every block, then filled from BOTH sources every block: native
  `CLAP_EVENT_MIDI2` packets append directly during the event loop,
  and after decode `midi1_to_ump(midi_in, ump_buffer)` always runs
  (synthesises UMP from the MIDI 1.0 stream). Both paths run
  unconditionally because real hosts mix transports — notes via
  `CLAP_EVENT_NOTE_*` and CCs via `CLAP_EVENT_MIDI2` is common, and
  skipping the synthesis when MIDI2 is present silently drops the
  note half from the UMP buffer. See Gotchas. `clap_activate()` reserves
  and capacity-limits this sidecar.
- `native_f64_enabled` plus the `data64` scratch/pointer arrays — CLAP hosts
  may provide double-precision audio buffers. If
  `PluginDescriptor::effective_capabilities().supports_f64_audio` is false,
  the adapter converts host `data64` to the normal f32 `process(...)` path and
  converts f32 outputs back to `data64`. If the plugin opts in, every active
  routed bus for the block must be f64 before the adapter calls
  `Processor::process_f64(ProcessBuffers64&, ...)`; mixed f32/f64 blocks stay
  on the compatibility path.
- `ara_controller` — lazily created on the first host query for the
  ARA companion-factory extension.
- `bridge` + `editor_host` + `editor_visible` — gated on
  `PULP_CLAP_GUI`. Editor lifecycle flows through `ViewBridge`; see the
  `view-bridge` skill for the open/attach/close protocol.

`PulpClapPlugin` is consumed from two translation units: the per-plugin
`clap_entry.cpp` and the shared `clap_adapter.cpp` in `pulp-format`.
They must see the same GUI define. Desktop plugin builds compile both
with `PULP_CLAP_GUI=1`; WCLAP/non-GUI builds compile both with
`PULP_CLAP_GUI=0`. Do not add GUI-only fields under a preprocessor
condition unless the shared adapter target receives the same condition.
A layout mismatch presents as random CLAP lifecycle corruption, often a
REAPER crash in `gui_create()` or `clap_activate()` touching a bogus
`bridge`/`editor_host` pointer.

### Parameters

Parameters are defined by the Processor during `define_parameters(store)`
and enumerated to the host by the `params` extension in
`clap_entry.hpp`:

- `params_count` → `store.param_count()`.
- `params_get_info` → builds a `clap_param_info_t` from the stored
  `ParamInfo`. `CLAP_PARAM_IS_AUTOMATABLE` is always set.
  `CLAP_PARAM_IS_STEPPED` is set when `range.step >= 1` and the range
  is narrow (`< 10`).
- `params_get_value` returns the current **base** value (without
  modulation).
- `params_value_to_text` uses `ParamInfo::to_string` when provided,
  otherwise falls back to `"%.2f %s"` with the unit.

During `clap_process`, the adapter routes host events into the store:

```
CLAP_EVENT_PARAM_VALUE   → store.set_value_rt(id, value)   ← RT-safe
CLAP_EVENT_PARAM_MOD     → store.set_mod_offset(id, amount)
CLAP_EVENT_PARAM_GESTURE_BEGIN / _END → store.begin_gesture / end_gesture
```

**Use `set_value_rt`, not `set_value`, on the audio thread.** The generic
`set_value()` path dispatches `ListenerThread::Main` listeners through
the installed `EventLoop`, and that dispatch lambda allocates on the
firing thread — fatal for the audio thread. `set_value_rt()` writes the
atomic + pushes an event on a non-allocating SPSC queue; the editor's
UI tick drains via `store.pump_listeners()`. Audio listeners still fire
inline (caller asserts RT-safety), so audio-thread listeners must be
trivial, non-allocating, and bounded.

Do not collapse inbound CLAP parameter automation to a single last point.
`clap_process` appends every `CLAP_EVENT_PARAM_VALUE` to
`PulpClapPlugin::param_events`, sorts by sample offset, and still calls
`store.set_value_rt(...)` for the same events so legacy block-level reads
observe the final value.
Before calling `Processor::process()`, the adapter attaches that queue via
`processor->set_param_events(&param_events)`, so sample-accurate processors
read the same sorted events through `Processor::param_events()`.

The **modulation offset is per-buffer**: `store.reset_all_mod()` runs
at the top of every `process()` before applying new `PARAM_MOD` events.
DSP reads modulated values via `store.get_modulated(id)` = base +
current mod offset. Plugins that only read `store.get_value(id)` do
**not** see host modulation.

### Audio buses (incl. sidechain)

`audio_ports` enumeration in `clap_entry.hpp` is descriptor-driven:
`desc.input_buses` / `desc.output_buses`. **Bus 0 is always the main
bus** (flag `CLAP_AUDIO_PORT_IS_MAIN`); **bus 1 (when present) is the
sidechain** and is routed via `Processor::set_sidechain(&view)` before
`process()`. Additional input buses beyond index 1 are ignored — the
Processor API exposes a single sidechain slot.

**Secondary (aux) output buses ARE routed** to the richer
`Processor::process(ProcessBuffers&)` surface (role `Aux`, index ≥1) for
multi-out instruments (drum machines, multitimbral, stem renderers).
`clap_process` builds one `ProcessBusBufferView<float>` per host output
bus from pre-allocated `aux_output_ptrs[kMaxOutputBuses-1][kMaxChannels]`
storage (row `b-1` backs host bus `b`; the main bus uses `output_ptrs`),
so the routing path never allocates on the audio thread. Each aux bus is
**pre-zeroed before `process()`**, so a single-output processor (whose
default `process(ProcessBuffers&)` writes only the main bus) leaves aux
buses silent — no uninitialised memory, no behavioural change. A
multi-out processor overrides `process(ProcessBuffers&)` and writes each
aux bus. The aux view's `declared_channels` is the descriptor's declared
count (cached in `clap_activate` — never call `descriptor()` on the audio
thread; it allocates), while `buffer.num_channels()` carries the actual
routed count, so `matches_declared_layout()` can detect a host-vs-declared
mismatch. Host output buses beyond `kMaxOutputBuses` are zero-filled but
not routed.

### MIDI: short messages, sysex, note-expression, UMP

Inbound event decode in `clap_process()`:

```
CLAP_EVENT_NOTE_ON / _NOTE_OFF → MidiEvent::note_on / note_off
CLAP_EVENT_MIDI                → MidiEvent::from_bytes(data[0..2])
                                 — CC, pitch bend, channel AT,
                                   poly AT, program change
CLAP_EVENT_MIDI_SYSEX          → midi_in.add_sysex_copy(bytes, time, 0.0)
                                 backed by a preallocated payload pool
CLAP_EVENT_NOTE_EXPRESSION     → synthesised MIDI 1.0 (see table)
CLAP_EVENT_NOTE_CHOKE          → note_off(channel, key, velocity=0)
CLAP_EVENT_MIDI2               → self->ump_buffer.add(packet)
                                 (guarded by CLAP_VERSION_GE(1,1,0) —
                                  the event is an enumerator, NOT a
                                  preprocessor macro; see Gotchas)
```

**Note-expression → MIDI 1.0 mapping.** `MpeVoiceTracker` only ingests
MIDI 1.0, so per-note expressions are synthesised to channel-wide
equivalents and narrowed back per-voice by the tracker:

| CLAP expression id | Synthesised MIDI 1.0 |
|---|---|
| `PRESSURE` | channel aftertouch `0xDn` |
| `TUNING` | 14-bit pitch bend (normalised to ±48st member range) |
| `BRIGHTNESS` | CC 74 |
| `VOLUME` | CC 7 (0..4 → 0..127 log-domain scale) |
| `PAN` | CC 10 |
| `VIBRATO`, `EXPRESSION` | dropped — no unambiguous MIDI 1.0 equivalent; UMP-aware plug-ins should consume via the `CLAP_EVENT_MIDI2` path |

Non-MPE descriptors drop note-expression events with a one-time
debug log. See the `mpe` skill for tracker details.

**Outbound MIDI**: the processor's `midi_out` emits short messages as
`CLAP_EVENT_MIDI` and sysex entries as
`CLAP_EVENT_MIDI_SYSEX`, both via `out_events->try_push`.
`sample_offset` carries through to `header.time`. The sysex
`clap_event_midi_sysex_t.buffer` field is non-owning — the backing
vector is alive for the duration of `clap_process()`, which is all
CLAP's push contract requires (the host copies before returning).

`midi_in`, `midi_out`, `mpe_buffer`, and `ump_buffer` are per-instance
buffers on `PulpClapPlugin`, not fresh locals inside `clap_process()`.
`clap_activate()` reserves their storage and enables realtime capacity
limits, then `clap_process()` clears and reuses them every block. Past
the reserved capacity, appends must drop and increment the relevant drop
counters; they must not grow vectors under the process no-allocation
guard.

Inbound CLAP SysEx is the exception to the move-based adapter pattern:
the host gives the adapter a non-owning payload pointer, so accepting it
requires copying bytes into owned storage. `clap_activate()` reserves a
bounded payload pool on `midi_in`; `MidiBuffer::add_sysex_copy()` copies
into that pool on the realtime-limited process path and drops only when
the sidecar count or per-payload capacity is exceeded. Keep the happy
path and overflow case covered in `test_clap_midi_events.cpp`.

If you add a new inbound/outbound MIDI path, cover the overflow case in
`test_clap_midi_events.cpp`. If you add a test processor that
captures/forwards sysex while behind the CLAP no-alloc guard, preallocate
destination SysEx payload storage in `prepare()` and call
`MidiBuffer::add_sysex_copy()` for explicit captures. Whole-event
forwarding with `midi_out.add_sysex(std::move(sx))` is supported because
pool-backed input events copy into the destination's prepared payload
pool; moving only `sx.data` out of `midi_in` is intentionally not
supported. Copying a vector payload inside `process()` will trip the RT
allocation trap under ASan/TSan/debug test builds.

### State save / restore

Serialisation goes through the single `StateStore::serialize()` /
`deserialize(bytes)` path (in `clap_entry.hpp` `state_ext`). Format is
the Pulp binary blob — identical bytes across CLAP / VST3 / AU, so
round-trip parity is trivial to test.

### Transport context

`clap_process()` maps `process->transport` into `ProcessContext` when
the host supplies it:

- `is_playing` / `is_recording` from the CLAP transport flags.
- `tempo_bpm` only when `CLAP_TRANSPORT_HAS_TEMPO` is set.
- `position_beats` from `song_pos_beats / CLAP_BEATTIME_FACTOR` when
  `CLAP_TRANSPORT_HAS_BEATS_TIMELINE` is set.
- `position_samples` from `song_pos_seconds / CLAP_SECTIME_FACTOR *
  sample_rate` when `CLAP_TRANSPORT_HAS_SECONDS_TIMELINE` is set.
  Leave it at 0 when the host omits the seconds timeline; deriving an
  absolute sample position from beats + current tempo is not authoritative
  in tempo-mapped projects and can create false transport jumps.
- time signature, loop range, and bar from the matching CLAP fields.

Keep `position_samples` non-zero when the host provides a seconds timeline;
native-core processors forward it as `playhead_frames`, so leaving it at the
default 0 makes CLAP-only playhead-sensitive processors think every block
starts at the song origin.

### Editor

Gated on `PULP_CLAP_GUI`; for desktop CLAP, both the shared
`pulp-format` adapter TU and the per-plugin entry TU must compile with
the same value. Lifecycle flows through
`pulp::format::ViewBridge`: `gui_create` → `bridge->open()`, the host
then calls `gui_set_parent(window)` → `editor_host->attach_to_parent` +
`bridge->notify_attached()`, `gui_destroy` → `bridge->close()`. See the
`view-bridge` skill for the full contract — the CLAP adapter is the
reference implementation for the "open, then notify_attached after
host has attached" protocol.

Window API negotiation is compile-time platform-switched to Cocoa /
Win32 / X11.

### Proportional resize with aspect lock

`gui_can_resize` returns `true`. `gui_get_resize_hints` advertises
`preserve_aspect_ratio=true` with `aspect_ratio_{width,height}` set to
the editor's preferred design size, so DAWs (Bitwig, Reaper, Live, …)
lock the corner-drag to the design aspect. `gui_adjust_size` snaps the
requested rectangle to the design aspect (largest box at the design
aspect that fits within the request), then clamps to plugin min/max
constraints.

`gui_create` calls `host->set_design_viewport(design_w, design_h)` so
the host scales content to fit the resized window via a paint-time
canvas transform — the JS/Yoga tree still thinks it's at design size,
and the existing `gui_set_size` → `host->set_size(...)` path resizes
the surfaces without re-laying out. This is the proportional+locked
behavior the standalone host already had (same design-viewport contract as WindowHost); AU v2
cannot offer it because the DAW resizes the returned NSView directly
with no host-side `gui_can_resize` analogue. Cross-format design lives
in the `view-bridge` skill.

`gui_create` calls
`pulp::format::decide_gpu_host(*bridge)` so a Skia/Dawn/scripted editor
auto-selects the GPU `PluginViewHost`, wires the per-vsync scripted idle pump
(`make_scripted_idle_pump`), and screams via `warn_if_unexpected_cpu_fallback`
on a silent CPU fallback. CLAP's `gui_set_size` already resizes the bridge +
host, so no extra resize seam is needed (unlike AU v2). Full contract: the
`view-bridge` skill's "GPU view host auto-selection" section.

### ARA companion factory

`clap_get_extension(kClapAraFactoryExtension)` lazily creates the
plugin's `AraDocumentController` on first query, then returns the
companion factory pointer. Only instantiates when the Processor
overrode `create_ara_document_controller()` — plugins that don't
participate in ARA return `nullptr` naturally. See the `ara` skill.

### Bypass routing — auto-detected

CLAP doesn't model "bypass" as a first-class extension the way VST3
(`kIsBypass`) or AU v3 (`AUAudioUnitBypass`) do — hosts treat a
plugin-declared `"Bypass"` parameter as the on/off lane. The adapter
auto-detects that parameter at `clap_init` and short-circuits
`clap_process` to pass-through (in→out for effects, zero-fill for
instruments) when the cached parameter's current value is `>= 0.5`,
without invoking `Processor::process`. MIDI output stays empty so
bypassed MIDI FX don't leak notes — same contract the VST3 and AU v3
adapters honour.

**Param designation (declared bypass) + trigger params.** The bypass
parameter is found through the shared `pulp::state::is_bypass_param`
contract, not a re-implemented name/range check. A Processor author can
declare `ParamInfo::designation = ParamDesignation::Bypass` to mark a
param as the bypass control *independently of its name* — the legacy
boolean-`"Bypass"` name/range heuristic remains the fallback when no
designation is declared, so existing plugins are unchanged. The adapter
also calls `StateStore::reset_triggers_rt()` to auto-reset trigger /
momentary params (`is_trigger`, or a `ParamDesignation::Reset`
"reset/panic" control) back to their default. The call sits AFTER the
bypass if/else (not inside the non-bypass branch), so it is a
**single-exit invariant**: a trigger raised while bypassed still settles
this block instead of firing late. It runs before the `out_events` scan,
so the host records the settle. Input param events are applied to the
store before the bypass check, so a host-raised trigger is observed-then-
settled within the same block whether or not the plugin is bypassed.

### Latency / tail change notifications

A Processor flags a mid-render latency or tail change via
`flag_latency_changed()` / `flag_tail_changed()` (RT-safe atomic
store-release). Don't call `clap_host_latency->changed()` from
`process()` directly — the spec requires that on the main thread.

CLAP wiring (the most involved of the four adapters):

1. `create_plugin()` captures the `clap_host_t*` pointer for later
   `request_callback()` use.
2. `process()` peeks via `latency_change_pending()` /
   `tail_change_pending()` (non-mutating — does NOT drain the edge)
   and, if either is set, calls `host->request_callback()` to ask
   the host for a main-thread callback.
3. `clap_on_main_thread()` then drains via
   `consume_latency_changed_flag()` / `consume_tail_changed_flag()`
   and calls `clap_host_latency->changed()` /
   `clap_host_tail->changed()`.

The peek-vs-consume split exists specifically for CLAP — VST3 / AU
v3 / AU v2 drain in-line because their host APIs are safe from the
audio callback path. Don't collapse the two helpers into one if you
add another adapter that needs the same edge.

### Preset loading

`clap_plugin_preset_load` is exposed only when the Processor builds a
`PresetManager` during `clap_init` (driven by
`desc.manufacturer`/`desc.name`). Today only
`CLAP_PRESET_DISCOVERY_LOCATION_FILE` is honoured; bundle- and plugin-
internal preset sources are ignored and return false.

### Remote controls and dynamic extension routing

`clap_entry.hpp::get_extension(plugin, id)` handles static extensions first,
then delegates to `clap_adapter::clap_get_extension(plugin, id)` for
instance-owned extensions such as `preset-load`, ARA, and
`clap.remote-controls/2`. Any new adapter-owned extension must be reachable
through the real host callback path, not only through direct unit-test calls
to the adapter helper.

Remote-control page generation lives in `clap_remote_controls.cpp`. It is a
main-thread, metadata-only extension: build pages from `StateStore` parameter
and group metadata, do not touch DSP state, and do not add audio-thread work
to `clap_process()`. The current pages expose ungrouped params as `Main`,
then grouped params in registered group order, eight params per page, with
unused slots set to `CLAP_INVALID_ID`. Keep the stable
`CLAP_EXT_REMOTE_CONTROLS` id and the compatibility id routed together unless
the CLAP headers remove the older spelling.

## Gotchas

### Sidechain `data32` can be null — guard before routing

A host may report `audio_inputs_count > 1` but hand the adapter a null
`data32` pointer (bus deactivated). A loose translation of "bus exists
→ publish sidechain" hands the Processor a `BufferView` over garbage.
The guard in `clap_process` demotes the whole sidechain bus to "not
supplied" if any per-channel pointer is null — do not remove it.

```cpp
if (sc_bus.data32) {
    sc_channels = std::min(static_cast<int>(sc_bus.channel_count), kMaxChannels);
    for (int ch = 0; ch < sc_channels; ++ch) {
        self->sidechain_ptrs[ch] = sc_bus.data32[ch];
        if (!self->sidechain_ptrs[ch]) { sc_channels = 0; break; }
    }
}
```

The VST3 adapter carries the same guard for null bus channel pointers.
Mirror both whenever reshaping the sidechain path.

### Aux output bus `data32` can be null too — guard the pre-zero loop

A deactivated **secondary output** bus can report `channel_count > 0`
while `data32 == nullptr`, exactly like the sidechain case. The aux
pre-zero loop in `clap_process` runs *before* the routing loop's own
`data32` guard, so it must `if (!bus.data32) continue;` before indexing
`bus.data32[ch]` — otherwise a host that presents an inactive aux output
null-derefs on the audio thread. Both the pre-zero loop and the routing
loop guard the bus pointer; keep both.

### Reset modulation offsets **every** buffer

`store.reset_all_mod()` is the first line of `clap_process()`. If you
refactor the process prologue, keep it first — otherwise stale
`PARAM_MOD` offsets from a previous block leak into the next one and
the plugin's DSP drifts away from the host's expected modulated value.
Found during CLAP modulation bring-up.

### `param_snapshot` is **per-buffer**, not cached

The snapshot is taken after host events are applied but before
`processor->process()`. The diff compared against current values at
the end is what the adapter emits as `PARAM_VALUE` out-events. If you
optimise this into a persisted snapshot you will drop plugin-side
param edits that happen at block boundaries.

### Secondary output buses must be zero-filled

Multi-out instruments that don't route to bus ≥ 1 leave those output
buffers whatever the host's last tenant wrote. The adapter zeroes
every secondary output channel every block — do not skip this even for
"only bus 0 used" plugins; some hosts reuse memory across plugin
slots.

### ARA companion factory is returned **only after Processor exists**

`clap_get_extension` may be called before `clap_init` populates
`self->processor`. The current impl returns the static companion
factory pointer early; it only lazily instantiates the
`AraDocumentController` once `self->processor != nullptr`. If you
refactor this path, preserve that ordering — eagerly constructing the
controller at extension-query time triggers the
`create_ara_document_controller()` virtual before the Processor is
alive.

### UMP sidecar: native + synthesised, both always run

The adapter handles every host shape: pure MIDI 1.0 (`CLAP_EVENT_NOTE_*`
+ `CLAP_EVENT_MIDI`), pure MIDI 2.0 (`CLAP_EVENT_MIDI2`), and mixed
(notes via NOTE_*, CCs via MIDI2 — common in real DAWs).

1. At the top of every `clap_process()` block, `ump_buffer.clear()`
   runs when `ump_enabled`. This is load-bearing — keep the clear
   up-front so the buffer reflects only the current block.
2. During event decode, `CLAP_EVENT_MIDI2` packets are appended
   directly to `self->ump_buffer`. `host_delivered_ump` is retained as
   observability only; it must not gate MIDI 1.0 synthesis.
3. After the decode loop, `midi1_to_ump(midi_in, self->ump_buffer)`
   ALWAYS runs when `ump_enabled`. Skipping synthesis when the host
   delivered any MIDI2 silently drops the note half of mixed streams from
   the UMP buffer. CLAP guarantees a spec-conformant host won't redundantly
   encode the same logical event in two transports, so unconditional
   synthesis doesn't double-deliver.

The UMP buffer shape lives in `core/midi/include/pulp/midi/ump_buffer.hpp`
and the CLAP adapter's `ump_buffer` sidecar.

### CLAP event types are enumerators, not preprocessor macros

When gating on a new CLAP event type, **do not** write
`#ifdef CLAP_EVENT_MIDI2` — `CLAP_EVENT_MIDI2` is a C enumerator value,
and `#ifdef` on an enum always evaluates false. Use
`#if defined(CLAP_VERSION_GE) && CLAP_VERSION_GE(1, 1, 0)` (or the
release that introduced the event) instead. Same trap applies to any
future `CLAP_EVENT_*` additions — the CLAP header does not define
them as macros. Use the guard shape in `core/format/src/clap_adapter.cpp`.

### Param text parsing must be locale-independent — but not via `std::from_chars<float>`

`params_text_to_value` and `params_value_to_text` must be immune to a
comma-decimal global host locale (a DAW that called `setlocale`): a typed-in
`"0.5"` must never parse as `0.0`, and a formatted value must never emit
`"0,5"`. Use `std::to_chars` for formatting (always C-locale). For *parsing*,
the obvious choice — `std::from_chars` — is a **trap for floats**: libc++
leaves the floating-point `from_chars` overloads `=delete`d on some toolchains
(notably the github-hosted `macos-15` sanitizer image), so
`std::from_chars(first, last, a_double)` hard-fails the **Sanitizer Tests**
build with "call to deleted function 'from_chars'" while the Mac Studio
`macos` gate (which *has* the overload) stays green — the break hides on the
advisory lane. Integer `from_chars` is fine everywhere. For the float value,
parse through `pulp::format::detail::parse_double_c_locale`
(`core/format/include/pulp/format/detail/locale_independent_float.hpp`), a
C-locale `strtod` wrapper shared with the `.pulpset` parser.

### GUI layout must match across CLAP TUs

Do not use `#ifdef PULP_CLAP_GUI` for CLAP GUI fields or extension
dispatch; use `#if defined(PULP_CLAP_GUI) && PULP_CLAP_GUI`. The WCLAP
path may define `PULP_CLAP_GUI=0`, and `#ifdef` treats that as enabled.
The desktop shared adapter and the per-plugin entry must agree on
whether GUI fields exist in `PulpClapPlugin`, or later lifecycle fields
shift and hosts crash when opening or activating the editor.

### ARA CLAP lives outside `CLAP_EXT_*`

The ARA companion factory is keyed on
`kClapAraFactoryExtension` (Pulp-private identifier), not one of CLAP's
reserved `CLAP_EXT_*` strings. Don't rename it; other Pulp + ARA hosts
already search for that exact key. Defined in `pulp/format/ara.hpp`.

### `clap-validator` is optional — fallback is dlopen

`pulp validate` (`tools/cli/cmd_validate.cpp`) runs
`clap-validator validate …` when installed, otherwise falls back to a
plain `dlopen` check. CI lanes without `clap-validator` still exercise
the "plugin loads" path; full spec conformance requires the validator
binary.

### AAX-parity sweep

AAX and CLAP share the same sysex-sidecar pattern. When you change the
CLAP sysex accumulator, the AAX adapter
(`core/format/src/aax_runtime.cpp`) and the VST3 / AU halves need to
stay in sync — see the memory note on AAX-parity.

### Filter in-events by `space_id` in every dispatch loop

Every `clap_input_events` dispatch loop in the adapter MUST check
`hdr->space_id == CLAP_CORE_EVENT_SPACE_ID` at the top and `continue`
on mismatch. Non-zero namespaces belong to third-party extensions
Pulp doesn't implement, and their type IDs may alias core type IDs
(e.g. a fictional extension's event type `5` could be mistaken for
`CLAP_EVENT_PARAM_VALUE` and mutate the param store). clap-validator
`param-set-wrong-namespace` exercises this with `space_id = 0xb33f`.

Covered sites today:

- `clap_adapter.cpp` process() param/gesture loop
- `clap_adapter.cpp` process() note/MIDI loop
- `clap_entry.hpp` `params_flush()` path

If you add a third in-events dispatch (e.g. a transport-event loop,
or a new extension's callback), add the same guard. Test pattern:
`test_clap_entry.cpp` → "CLAP params_flush ignores events outside
the core namespace [issue-743]" (the bracketed token is the Catch2 test tag).

### `clap_ostream::write` may short-write — loop state_save

`state_save` (in `clap_entry.hpp`) MUST loop on `stream->write()`
until the full payload is delivered. Per CLAP spec, a single `write`
call may return fewer bytes than requested even on success; only
negative or zero returns are errors. clap-validator's
`state-reproducibility-flush` exercises this by capping every write
at 23 bytes.

Symmetric note: `state_load`'s `stream->read` loop was already
correct; the bug was only on the write side.

## Validation recipes

Build and smoke a CLAP bundle with the Pulp CLI:

```bash
# Build everything, then validate
./build/pulp build
./build/pulp validate          # runs clap-validator if installed
```

Direct `clap-validator` usage (matches what `cmd_validate.cpp` invokes):

```bash
# macOS / Linux
clap-validator validate "$(pwd)/build/path/to/MyPlugin.clap"

# Install if missing
cargo install clap-validator
```

CI's fallback when `clap-validator` is not on the path is a dlopen
check — load the bundle's entry symbol (`clap_entry`) and verify the
factory hands back a valid descriptor. See
`test/test_clap_entry.cpp` for the in-repo equivalent.

`pulp build --test` runs validation before allowing
`pulp build --install` to write into
`~/Library/Audio/Plug-Ins/CLAP/`. Do not `--skip-validation` a CLAP
build before a DAW scan — a crashing entry point takes the DAW down
with it.

### Validator runs must disable editor creation

`clap-validator` can query `CLAP_EXT_GUI` and call GUI callbacks even
when the test's intent is non-visual validation. Run validator automation
with `PULP_DISABLE_PLUGIN_EDITOR=1 PULP_HEADLESS=1 PULP_TEST_MODE=1`.
Under those guards, Pulp hides `CLAP_EXT_GUI` from `get_extension()` and
the GUI callbacks fail closed if a host cached the extension pointer.

## Host-API contract pinned by `test/test_clap_host_validation.cpp`

Real-DAW validation (Bitwig, Reaper, FL Studio, Studio One) requires
a license + manual install, so the CI proxy is
`test/test_clap_host_validation.cpp`. It pins the four contracts hosts
have historically broken on:

1. Plugin id + parameter id + range stability across instances.
2. `CLAP_EVENT_PARAM_MOD` does NOT bleed across blocks — the adapter
   calls `store.reset_all_mod()` at the top of every `process()`.
3. Non-core event spaces (`hdr->space_id != CLAP_CORE_EVENT_SPACE_ID`)
   are ignored, matching `clap-validator`'s
   `param-set-wrong-namespace` expectation.
4. `state.save` → `state.load` → `state.save` produces byte-equivalent
   output (Studio One project-recall determinism).

When changing the adapter's event dispatch or param surface, run
`pulp-test-clap-host-validation` first — it will catch the regression
before a host scan does.

## Cross-references

- `.agents/skills/view-bridge/SKILL.md` — editor open / attach /
  close protocol; CLAP is the reference wiring for this adapter family.
- `.agents/skills/mpe/SKILL.md` — MPE sidecar contract. CLAP is the
  canonical consumer.
- `.agents/skills/ara/SKILL.md` — ARA SDK setup and companion-factory
  lifecycle.
- `.agents/skills/vst3/SKILL.md` and `.agents/skills/auv3/SKILL.md` —
  cross-format parity table when triaging host-specific bugs.
- `docs/guides/formats.md` — user-facing format overview.
- `docs/guides/host-matrix.md` — per-host ARA / CLAP compatibility
  notes.
- Memory note: CHOC-first policy — prefer `choc::midi` helpers over
  hand-rolled MIDI decode when touching the adapter.

### CLAP editor hands GpuSurface to ScriptedUiSession

`clap_entry.hpp::gui_create` calls
`p->bridge->scripted_ui()->attach_gpu_surface(p->editor_host->gpu_surface())`
right after `PluginViewHost::create()` succeeds. Without this, a
CLAP plugin whose UI uses Three.js or raw WebGPU JS renders black —
the JS shim silently falls back to mocks. See the `view-bridge` skill's
"GpuSurface plumbing into WidgetBridge" section.

## Host-quirks consumption

This adapter consumes the host-quirks ledger at init: it caches
`resolved_quirks(detect_host_info().type, version)` once (the runtime
policy — `PULP_HOST_QUIRKS` env / `set_host_quirk_policy()` API / compile
default — applies via `resolved_quirks()`), then gates DAW accommodations
on those flags instead of hardcoding them.

First wired flag: `clamp_latency_to_nonneg`. Latency reporting routes
through the pure helper `pulp::format::reported_latency_samples(raw, quirks)`
(in `host_quirks.hpp`): a negative `latency_samples()` clamps to 0 when the
quirk is enforced, and passes through raw (wrapping the unsigned host field)
when `PULP_HOST_QUIRKS=off`. See `docs/reference/host-quirks-policy.md`.

## synthesize_bypass_parameter pass-through

At init (clap_init / PulpAUEffect ctor), the adapter calls
`pulp::format::maybe_synthesize_bypass(store, host_quirks)` then detects the
"Bypass" param (shared boolean-range heuristic: name=="Bypass", step>=1,
0..1) into a cached `bypass_param_id`. In the audio callback (clap_process /
ProcessBufferLists) it short-circuits to a **null-guarded pass-through**
(copy main input → output, zero any output channel without a matching input)
and skips the Processor when the param value is >= 0.5 — mirroring the VST3
processBlockBypassed path. `PULP_HOST_QUIRKS=off` synthesizes nothing
(bypass_param_id stays 0). The pass-through MUST null-check each destination
channel pointer because a bus can report channels with null buffers.

## Sample-accurate parameter output

CLAP's `out_events` requires **globally ascending time across ALL event types**.
The param-output drain (`clap_adapter.cpp`) therefore does a **three-cursor
merge** of output param events + MIDI shorts + sysex by ascending sample offset,
not "params then MIDI". Snapshot-diff fallback params (all at offset 0) emit
first; a per-param skip-set stops them double-reporting a param that pushed
explicit `push_output_param_event()` events. CLAP param values are **plain
domain** (`double`), unlike VST3 which normalizes. `test_clap_midi_events.cpp`
asserts the `param@0, midi@8, param@16` interleave stays non-decreasing in time.
