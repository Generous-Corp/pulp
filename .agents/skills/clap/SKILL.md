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
- `param_events` (`state::ParameterEventQueue`) for the current block's
  inbound `CLAP_EVENT_PARAM_VALUE` events. It preserves every host
  automation point with `header.time` before the StateStore dual-write
  lands the last value for ordinary parameter reads.
- `mpe_tracker` + `mpe_buffer` + `mpe_enabled` — MPE sidecar populated
  only if `PluginDescriptor::effective_capabilities().supports_mpe`
  is true. The effective value ORs the legacy descriptor flag with the
  node ABI capability field.
- `ump_buffer` + `ump_enabled` — UMP sidecar. Cleared at the top of
  every block, then filled from BOTH sources every block: native
  `CLAP_EVENT_MIDI2` packets append directly during the event loop,
  and after decode `midi1_to_ump(midi_in, ump_buffer)` always runs
  (synthesises UMP from the MIDI 1.0 stream). Both paths run
  unconditionally because real hosts mix transports — notes via
  `CLAP_EVENT_NOTE_*` and CCs via `CLAP_EVENT_MIDI2` is common, and
  skipping the synthesis when MIDI2 is present silently drops the
  note half from the UMP buffer (Codex P1 review on PR #627). See
  Gotchas.
- `ara_controller` — lazily created on the first host query for the
  ARA companion-factory extension.
- `bridge` + `editor_host` + `editor_visible` — gated on
  `PULP_CLAP_GUI`. Editor lifecycle flows through `ViewBridge`; see the
  `view-bridge` skill for the open/attach/close protocol.

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
inline (caller asserts RT-safety). See Slice 2 in
`planning/2026-05-18-rt-safety-and-debug-dx.md`.

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
Processor API exposes a single sidechain slot. Secondary **output**
buses are zero-filled so multi-out instruments don't surface
uninitialised memory to hosts.

### MIDI: short messages, sysex, note-expression, UMP

Inbound event decode in `clap_process()` (as of PR #627):

```
CLAP_EVENT_NOTE_ON / _NOTE_OFF → MidiEvent::note_on / note_off
CLAP_EVENT_MIDI                → MidiEvent::from_bytes(data[0..2])
                                 — CC, pitch bend, channel AT,
                                   poly AT, program change
CLAP_EVENT_MIDI_SYSEX          → midi_in.add_sysex(bytes, time, 0.0)
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

**Outbound MIDI** (the processor's `midi_out` — previously dropped):
short messages emit as `CLAP_EVENT_MIDI`, sysex entries as
`CLAP_EVENT_MIDI_SYSEX`, both via `out_events->try_push`.
`sample_offset` carries through to `header.time`. The sysex
`clap_event_midi_sysex_t.buffer` field is non-owning — the backing
vector is alive for the duration of `clap_process()`, which is all
CLAP's push contract requires (the host copies before returning).

### State save / restore

Serialisation goes through the single `StateStore::serialize()` /
`deserialize(bytes)` path (in `clap_entry.hpp` `state_ext`). Format is
the Pulp binary blob — identical bytes across CLAP / VST3 / AU, so
round-trip parity is trivial to test.

### Editor

Gated on `PULP_CLAP_GUI` (set for plugin targets, off for the shared
format lib to keep the core thin). Lifecycle flows through
`pulp::format::ViewBridge`: `gui_create` → `bridge->open()`, the host
then calls `gui_set_parent(window)` → `editor_host->attach_to_parent` +
`bridge->notify_attached()`, `gui_destroy` → `bridge->close()`. See the
`view-bridge` skill for the full contract — the CLAP adapter is the
reference implementation for the "open, then notify_attached after
host has attached" protocol.

Window API negotiation is compile-time platform-switched to Cocoa /
Win32 / X11.

### Proportional resize with aspect lock (2026-05)

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

`gui_create` no longer hardcodes `Options::use_gpu`; it calls
`pulp::format::decide_gpu_host(*bridge)` so a Skia/Dawn/scripted editor
auto-selects the GPU `PluginViewHost`, wires the per-vsync scripted idle pump
(`make_scripted_idle_pump`), and screams via `warn_if_unexpected_cpu_fallback`
on a silent CPU fallback. CLAP's `gui_set_size` already resizes the bridge +
host, so no extra resize seam is needed (unlike AU v2). Full contract: the
`view-bridge` skill's "GPU view host auto-selection" section.
(GPU-plugin-view-host work, 2026-05.)

### ARA companion factory

`clap_get_extension(kClapAraFactoryExtension)` lazily creates the
plugin's `AraDocumentController` on first query, then returns the
companion factory pointer. Only instantiates when the Processor
overrode `create_ara_document_controller()` — plugins that don't
participate in ARA return `nullptr` naturally. See the `ara` skill.

### Bypass routing — auto-detected (PR #2937)

CLAP doesn't model "bypass" as a first-class extension the way VST3
(`kIsBypass`) or AU v3 (`AUAudioUnitBypass`) do — hosts treat a
plugin-declared `"Bypass"` parameter as the on/off lane. The adapter
auto-detects that parameter at `clap_init` and short-circuits
`clap_process` to pass-through (in→out for effects, zero-fill for
instruments) when the cached parameter's current value is `>= 0.5`,
without invoking `Processor::process`. MIDI output stays empty so
bypassed MIDI FX don't leak notes — same contract the VST3 and AU v3
adapters honour.

### Latency / tail change notifications (PR #2934, item 3.11)

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

## Gotchas

### Sidechain `data32` can be null — guard before routing (#277)

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

The VST3 adapter carries the same guard (`#178` review). Mirror both
whenever reshaping the sidechain path.

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
buffers whatever the host's last tenant wrote. The adapter now zeroes
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
   directly to `self->ump_buffer` (sets `host_delivered_ump = true`
   as a hint, no longer used for gating).
3. After the decode loop, `midi1_to_ump(midi_in, self->ump_buffer)`
   ALWAYS runs when `ump_enabled`. The earlier "skip when host
   delivered any MIDI2" branch (PR #627 v1) silently dropped the
   note half of mixed streams from the UMP buffer — Codex P1 review
   on PR #627 caught this. CLAP guarantees a spec-conformant host
   won't redundantly encode the same logical event in two
   transports, so unconditional synthesis doesn't double-deliver.

See `#141` / `#139` for the UMP buffer shape.

### CLAP event types are enumerators, not preprocessor macros

When gating on a new CLAP event type, **do not** write
`#ifdef CLAP_EVENT_MIDI2` — `CLAP_EVENT_MIDI2` is a C enumerator value,
and `#ifdef` on an enum always evaluates false. Use
`#if defined(CLAP_VERSION_GE) && CLAP_VERSION_GE(1, 1, 0)` (or the
release that introduced the event) instead. Same trap applies to any
future `CLAP_EVENT_*` additions — the CLAP header does not define
them as macros. See PR #627's `clap_adapter.cpp` for the canonical
guard shape.

### GUI is gated on `PULP_CLAP_GUI`

The shared `pulp_format` library is built without `PULP_CLAP_GUI` so
the adapter stays thin. Only the per-plugin CLAP target turns it on.
If you add a new GUI-dependent member to `PulpClapPlugin`, wrap it in
`#ifdef PULP_CLAP_GUI` or the non-GUI builds break.

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

AAX and CLAP share CLAP's sysex-sidecar pattern (#239). When you
change the CLAP sysex accumulator, the AAX adapter
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
the core namespace [issue-743]".

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
`test/test_clap_host_validation.cpp` (item 3.4 of the macOS plugin
authoring plan). It pins the four contracts hosts have historically
broken on:

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
  close protocol; CLAP was the reference wiring in PR #140.
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
