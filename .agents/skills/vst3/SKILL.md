---
name: vst3
description: VST3 format adapter for Pulp — SingleComponentEffect wiring, bus arrangement negotiation, parameter / MIDI event routing, state round-trip, and the pitfalls discovered while wiring the adapter against Steinberg's SDK.
---

# VST3 Skill

Use this skill when touching Pulp's VST3 adapter, when answering
questions about how a Pulp plugin behaves inside Cubase / Nuendo /
Studio One / Reaper (VST3 lane) / FL Studio / Ableton Live (VST3
lane), or when a `pluginval` run surfaces something odd. VST3 is one
of Pulp's three first-class plugin formats alongside CLAP and AU v3.

## When to use

- Editing `core/format/src/vst3_adapter.cpp` or its header.
- Changing the generator macro `PULP_VST3_PLUGIN(…)` in
  `core/format/include/pulp/format/vst3_entry.hpp`.
- Changing bus-arrangement negotiation, parameter registration, MIDI
  event routing, state save/restore, or editor lifecycle.
- A VST3 host reports a behaviour issue — sidechain not exposed,
  mono↔stereo swap crashes the track, automation not recorded,
  tempo-sync glitches, preset load misbehaves.
- A `pluginval --strictness-level 5` run regresses.
- Working on the ARA VST3 surface — but first read the `ara` skill.

## Files and entry points

| Role | Path |
|---|---|
| Core adapter (C++) | `core/format/src/vst3_adapter.cpp` |
| Adapter header / `PulpVst3Processor` | `core/format/include/pulp/format/vst3_adapter.hpp` |
| Entry-point generator macro | `core/format/include/pulp/format/vst3_entry.hpp` |
| Editor `IPlugView` implementation | `core/format/src/vst3_plug_view.cpp`, `core/format/include/pulp/format/vst3_plug_view.hpp` |
| Info.plist template (macOS bundle) | `tools/cmake/PulpInfoPlist.vst3.in` |
| VST3 SDK fetch | `external/vst3sdk` — `git clone --depth 1 --branch v3.7.12 https://github.com/steinbergmedia/vst3sdk.git` (MIT) |
| CLI validator invocation | `tools/cli/cmd_validate.cpp` (`pluginval --strictness-level 5 --timeout-ms 30000 --validate …`) |

There is no hand-written `PulpVst3.cmake` helper — the VST3 target is
wired directly in the top-level `CMakeLists.txt` / `PulpPlugin` CMake
surface.

The VST3 editor lives in a sibling file (`vst3_plug_view.cpp`) that is
**owned by the `view-bridge` skill** per `skill_path_map.json`. Edits
to the editor surface trigger the `view-bridge` skill rather than
this one.

## Core conventions

### SingleComponentEffect pattern

`PulpVst3Processor` extends
`Steinberg::Vst::SingleComponentEffect` — Steinberg's combined
processor-plus-controller class. That means **no separate
`IEditController`** instance: the same C++ object both processes audio
and advertises parameters. This is deliberate; it simplifies
bidirectional parameter sync and matches how CLAP/AU handle the two
roles in one object.

The `PULP_VST3_PLUGIN(uid, name, category, vendor, version, url,
factory_fn)` macro (in `vst3_entry.hpp`) expands to a single
`BEGIN_FACTORY_DEF / DEF_CLASS2 / END_FACTORY` block that registers
the class under `kVstAudioEffectClass`. One factory per TU — do not
macro-expand it twice in the same plugin.

### `initialize` — the setup path

```cpp
SingleComponentEffect::initialize(context)       // Steinberg base
processor_ = factory_()                          // Pulp Processor
processor_->set_state_store(&store_)
processor_->define_parameters(store_)
store_.set_gesture_callbacks(beginEdit, endEdit) // host-recorded gestures
addAudioInput / addAudioOutput                   // from desc.{input,output}_buses
addEventInput / addEventOutput                   // gated on desc.{accepts,produces}_midi
parameters.addParameter(…)                       // one per StateStore param
```

The `unitId` field on each VST3 `ParameterInfo` is populated from
Pulp's `ParamInfo::group_id` — that's how VST3 hosts render a
parameter tree / folder structure.

Context-aware behaviour: if `context` resolves to an
`IHostApplication`, the adapter logs `kVst3AraFactoryContextKey` for
ARA-aware VST3 hosts (Cubase, Studio One). Surfaces Pulp's ARA factory
through the companion-factory negotiation. See the `ara` skill for
the full story.

### Bus arrangement negotiation

`setBusArrangements(inputs, numIns, outputs, numOuts)` is **not** a
pass-through to the base class. The adapter delegates the accept/reject
decision to
`Processor::is_bus_layout_supported(BusesLayout)` — the cross-adapter
virtual hook that lets a plugin enforce tighter layout contracts
(linked sidechain, surround, instrument-only output, etc.) without
overriding `setBusArrangements` directly. The default policy still
matches the descriptor's per-side bus count and only accepts mono /
stereo per channel.

Order matters: **call the hook before mutating `audioInputs` /
`audioOutputs`**. A rejected proposal returns `kResultFalse` and the
host falls back to the default arrangement, matching Steinberg's
spec — if you mutate first, the rejected reply leaves the bus state
diverged from `descriptor()`. The AU v3 / AU v2 / CLAP adapters carry
the same hook for the day they grow dynamic layout negotiation.

Why: hosts swap project channel layouts (load a stereo session over a
mono plugin slot) and expect the plugin's `descriptor()` view to
follow. Without the in-place update, the Pulp Processor's channel
counts diverge from the VST3 bus state. The dynamic-bus-arrangement tests
cover this contract.

### `setupProcessing` / `setActive` sequence

The Steinberg lifecycle is:

```
initialize → setBusArrangements → setupProcessing → setActive(true)
    → process loop → setActive(false) → terminate
```

`setupProcessing` calls `processor_->prepare(ctx)` with the host's
sample rate, max buffer size, and the descriptor's default channel
counts. `setActive(false)` calls `processor_->release()` so the
Processor can free prepare-time resources. Never move `prepare()` out
of `setupProcessing` — Steinberg guarantees `process()` is only called
after a successful `setupProcessing` + `setActive(true)` sequence.

### Parameters

Parameters flow both ways:

- **Host → plugin**: every block, `process()` walks
  `data.inputParameterChanges`, preserves **every** point from each
  `IParamValueQueue` in `param_events_`, and calls
  `store_.set_normalized_rt(id, value)` for each point. VST3 values are
  always normalised 0..1 — the event queue stores plain-domain values
  after denormalising through `ParamInfo::range`, while `set_normalized_rt`
  denormalises through the ParamInfo range, writes the atomic, and
  pushes an SPSC event for `ListenerThread::Main` listeners. The editor
  drains via `store.pump_listeners()` on its UI tick. The generic
  `set_normalized()` path would dispatch a heap-allocated lambda through
  the EventLoop — fatal on the audio thread. Before
  `Processor::process()`, `param_events_` is attached through
  `processor_->set_param_events(&param_events_)` so processors that opt
  into `Processor::param_events()` see the same sorted event stream.
- **Plugin → host**: `param_snapshot_` is taken before `process()`;
  after, any changed param emits a point via
  `data.outputParameterChanges->addParameterData(id).addPoint(0, norm)`
  **and** `setParamNormalized(id, norm)` keeps the SDK-side parameter
  cache in sync. Without both, automation-recording hosts miss the
  edit.
- **Gesture grouping**: `store_.set_gesture_callbacks(beginEdit,
  endEdit)` forwards Pulp gesture begin/end to Steinberg's undo-group
  primitives. UI code that edits params via `Binding` automatically
  gets gestures.

`kIsBypass` is auto-set on any parameter named `"Bypass"` whose range
is `[0,1]` with `step >= 1` — Steinberg requires exactly one bypass
parameter per plugin for the host bypass control to work.

### Bypass routing — cached ParamID + render short-circuit

`initialize()` caches the `ParamID` of the kIsBypass-tagged parameter
in `bypass_param_id_`; the value is exposed via
`bypass_parameter_id()` for tests and diagnostics. Inside `process()`,
when the cached parameter's current value is `>= 0.5`, the adapter
short-circuits to processBlockBypassed-style pass-through — copies the
main input to the main output (or zero-fills for instrument
descriptors), drops the sidechain, and returns **without invoking
`Processor::process`**.

Why cache the ID: hosts hit `process()` thousands of times per second
and walking the parameter table to find the bypass slot on every
block is a non-trivial cost. The cache is set once at `initialize()`
and never mutated. When a plugin has no Bypass parameter the adapter
falls back to "always render" — no synthetic atomic.

### Latency / tail change notifications

A Processor can flag a mid-render latency or tail change from the
audio thread via `flag_latency_changed()` / `flag_tail_changed()`
(RT-safe atomic store-release). Never call host APIs from `process()`
directly — the format adapter owns the host-thread publish path.

VST3 wiring (post-process): the adapter checks
`consume_latency_changed_flag()` / `consume_tail_changed_flag()` and
calls `componentHandler->restartComponent(kLatencyChanged |
kReloadComponent)`. Steinberg documents `restartComponent` as safe
from the audio callback (the handler queues main-thread delivery), so
no extra dispatch is needed on the VST3 side. Tests in
`pulp-test-processor-layout-latency` pin the round-trip and the
two-thread hammer for data-race freedom.

### MIDI events

VST3 delivers note-on / note-off through `IEventList`:

```
Event::kNoteOnEvent  → MidiEvent::note_on
Event::kNoteOffEvent → MidiEvent::note_off
Event::kDataEvent (type=kMidiSysEx) → midi_in_.add_sysex_copy(bytes, size, sampleOffset, 0.0)
```

Non-note short MIDI (CC, mod wheel, sustain, pitch bend, channel
aftertouch) is **not** delivered by Steinberg's event list — VST3 routes
controllers through `IMidiMapping` instead. The adapter implements it so
these reach MIDI-accepting plug-ins on the same `midi_in_` buffer as notes:

```
IMidiMapping::getMidiControllerAssignment(bus, channel, cc) → reserved hidden ParamID
host then sends that controller as a normal parameter change
process(): a param change whose ID is a REGISTERED controller
  → decode (channel, controller) → CC / pitch-bend / channel-pressure MidiEvent → midi_in_
```

See `core/format/include/pulp/format/detail/vst3_midi_mapping.hpp` for the
ParamID scheme (base `0xC0000000`, 16 channels × 130 controllers; controller
0..127 = CCs, 128 = aftertouch, 129 = pitch bend) and the decode helpers.
Load-bearing constraints:
- **The reserved ParamIDs MUST be registered parameters** (flagged
  `kIsHidden`, NOT `kCanAutomate`) — VST3 rejects a mapping to an
  unregistered ID, and the SDK's MIDI-mapping validation suite asserts every
  returned tag is in the parameter set. That is why `initialize()` registers
  2080 hidden controller params (only when `desc.accepts_midi`).
- **ONE predicate for register / map / divert — `is_registered_controller()`,
  NOT a bare range test.** `initialize()` builds a `std::vector<bool>` bitmap
  (`registered_controller_ids_`, indexed by `id - base`) recording exactly the
  controller IDs it registered. A reserved ID that collides with a real
  plug-in parameter is **skipped** at registration AND its bitmap bit stays
  clear, so `getMidiControllerAssignment()` declines that controller and
  `process()` does NOT divert that ID to MIDI — the host's param-change for it
  still reaches `store_`. Using a bare `is_vst3_midi_cc_param()` range test in
  `process()` would silently hijack a colliding real param into MIDI (state
  corruption). The bitmap lookup is O(1) and allocation-free on the audio
  thread (built once at init). Regression: `[vst3][midimapping][collision]`.
- **Gate on `desc.accepts_midi`.** An effect that ignores MIDI registers
  none of these, so its host-visible parameter count is exactly what it
  declared — existing param-count / state-format contracts are unaffected.
  (Controllers never enter `store_`, so saved state never contains a
  controller ID — verified by `[vst3][midimapping][state]`.)
- **The event input bus must declare 16 channels** (`addEventInput(name, 16)`),
  not 1 — the host queries `getMidiControllerAssignment` per channel up to the
  bus's `channelCount`, so a 1-channel bus only ever maps channel 0.
- Controllers are decoded in the parameter-change loop (before the note/SysEx
  loop), so `midi_in_` is cleared at the **top** of `process()`, not just
  before the event loop, and `midi_in_.sort()` runs after both sources append
  so controllers and notes interleave in sample order. Real plug-in param
  changes still flow to `store_` / `param_events_` unchanged.
- **Decode is defensively hardened:** value `std::clamp`ed to 0..1 before
  encoding, CC/AT clamped to 0..127, bend to 0..16383, and a param-change
  `sampleOffset` outside `[0, numSamples)` is dropped. The controller `add()`
  is the same capacity-limited, drop-on-overflow, alloc-free path as notes.
- **`MidiBuffer::sort()` is insertion-stable** (index sort over a pre-reserved
  scratch keyed by `(sample_offset, original_index)`, NOT `std::stable_sort` —
  which can allocate, and NOT a byte tie-break — which would silently reorder
  same-offset events by status byte and change musical semantics). A controller
  add()'ed before a note-on at the same offset stays before it, deterministic
  run-to-run. The scratch is reserved by `reserve()`/`reserve_events()`, so the
  sort stays allocation-free on the audio thread.

MIDI output mirrors the inverse: note_on / note_off in
`midi_out_` are written back into `data.outputEvents`.

**Real-time-safe MIDI buffers (no per-block allocation).** `midi_in_` /
`midi_out_` are reused `MidiBuffer` *members*, not block-local: `setupProcessing()`
calls `reserve(events, sysex, sysexPayloadBytes)` + `set_realtime_capacity_limit(true)`
so `add()` / `add_sysex_copy()` reuse reserved capacity and *drop* past the
worst-case instead of growing on the audio thread. Two footguns:
- **Reset BOTH stores every block.** `MidiBuffer::clear()` empties only the
  short-event store; the SysEx sidecar needs `clear_sysex()` as well. Calling
  only `clear()` leaks a block's SysEx payload into later blocks. `process()`
  calls both at the top of the block.
- **SysEx: use `add_sysex_copy(ptr, size, …)`, not `add_sysex(std::vector(…))`** —
  the latter heap-allocates a fresh payload per event; the former copies into the
  buffer's reserved payload pool (alloc-free in realtime mode).
Prove no-alloc with the `RtAllocationProbe` harness (see
`test_vst3_plugin_state.cpp` `[vst3][realtime][perf]`). Note: a pooled-SysEx
residual allocation inside `MidiBuffer` itself is a known `core/midi` follow-up.

### Per-note expression (MPE) — `INoteExpressionController`

VST3 carries per-note pitch / pressure / timbre as
`Event::kNoteExpressionValueEvent` in `data.inputEvents`, **not** as channel
MIDI. The host first queries `INoteExpressionController` to learn which
expression types the plug-in accepts, then sends value events keyed by the
originating note-on's `noteId`. The adapter implements this so an MPE/expressive
synth that works in CLAP isn't flat in Cubase/VST3. It reuses the **same**
`MpeVoiceTracker` + `MpeBuffer` + `Processor::set_mpe_input()` sidecar contract
the CLAP adapter uses (`core/midi/.../mpe_buffer.hpp`, `mpe_voice_tracker.hpp`).

```
INoteExpressionController::getNoteExpressionCount / getNoteExpressionInfo
  → declare Tuning / Volume / Brightness / Pan  (ONLY when desc.supports_mpe)
process(): Event::kNoteExpressionValueEvent (noteId, typeId, value)
  → look up noteId → (channel, note)
  → synthesize the channel-wide MidiEvent the MpeVoiceTracker narrows to that voice:
       kTuningTypeID     → per-note pitch bend  (VST3 plain = 240*(norm-0.5) st)
       kVolumeTypeID     → per-note pressure    (status 0xD0 channel aftertouch)
       kBrightnessTypeID → per-note timbre      (CC74)
       kPanTypeID        → declared for host completeness, NOT routed (no MPE axis)
  → run midi_in_ through mpe_tracker_ → mpe_buffer_ → processor_->set_mpe_input(&mpe_buffer_)
```

Load-bearing constraints:
- **Expose the interface in `queryInterface`.** `PulpVst3Processor` adds
  `INoteExpressionController` alongside `IMidiMapping`; the override does
  `QUERY_INTERFACE(iid, obj, INoteExpressionController::iid, …)` then delegates to
  `SingleComponentEffect`. Forgetting the iid means the host never asks for the
  types and per-note expression is silently dead. Regression:
  `[vst3][noteexpression][mpe]` (`resolves via queryInterface`).
- **Gate everything on `desc.effective_capabilities().supports_mpe`.** A non-MPE
  plug-in returns `getNoteExpressionCount == 0`, declines `getNoteExpressionInfo`,
  and does **zero** per-note work in `process()` (`set_mpe_input(nullptr)`) — no
  overhead, no host-visible note-expression lanes.
- **noteId → (channel, note) linkage is mandatory.** VST3 note expressions
  reference the note-on's `noteId`, not (channel, note). The adapter keeps a
  fixed-capacity (`kMaxLiveNoteIds = 128`) `note_id_map_` populated on note-on
  (when `noteId >= 0`), cleared on note-off, looked up on each value event.
  Bounded + allocation-free; a full table drops the mapping (the expression just
  won't route) rather than allocating. An expression for an unknown/released
  noteId is ignored. Regressions: `[vst3][noteexpression][mpe][process]`
  (unknown-noteId ignored, note-off releases the mapping).
- **MPE lower zone: channel 0 is the manager, channels 1-15 are members.** A
  note-on must land on a **member** channel for the tracker to create a voice the
  expression then narrows to — a note-on on channel 0 is a manager message and
  creates no per-note record. Tests use channels 1-4.
- **Synthesized expression messages go into `midi_in_`** (same buffer as notes),
  so `midi_in_.sort()` keeps the note-on (offset 0) at/before its expression, and
  the existing per-block `mpe_tracker_.process(ev)` loop turns them into per-note
  `MpeExpressionEvent`s. The sidecar is reserved + `set_realtime_capacity_limit`
  in `setupProcessing()` and cleared per block — alloc-free
  (`[vst3][noteexpression][mpe][realtime][perf]`).
- **Reset on re-activation.** `setupProcessing()` and `setActive(false)` reset
  `mpe_tracker_` and clear `note_id_map_` so a stale noteId never routes to a
  voice that no longer exists.

The VST3 type → MPE axis mapping is a **clean-room** choice derived from the SDK
note-expression value ranges (`ivstnoteexpression.h`) and the MPE spec's three
axes — not transcribed from any reference adapter.

**Scoping (by design, matches CLAP):** VST3 note expression is noteId-targeted,
but Pulp's per-note model IS MPE (one note per member channel) and the Processor
exposes no noteId-targeted expression API. The adapter therefore bridges to the
MPE model via a channel-wide MIDI message — **identical to the CLAP path**
(`clap_adapter.cpp` does the same: note-expression → channel-wide ShortMessage
onto the MPE sidecar). Per-note targeting is **exact when each note is on its own
MPE member channel** (the expressive-controller case MPE is designed for) and
**collapses to channel-wide when multiple notes share a channel** — same trade-off
as CLAP, not a VST3-specific defect. Do not build a separate noteId-targeted path:
there is nowhere on the Processor for it to land.

**Drop observability:** `note_expression_drop_count()` is a saturating atomic
(reset at session boundaries) bumped when the bounded noteId map (128 slots) is
full on a note-on insert, or a value event references an unmapped/released noteId.
It mirrors `MpeBuffer::dropped_event_count()` — a host-pollable signal that a
session exceeded the adapter's fixed per-note capacity. RT-safe (no audio-thread
logging). Regression: `[vst3][noteexpression][mpe][realtime]` (overflow).

**`associatedParameterId` = `kNoParamId`** (the SDK sentinel `0xFFFFFFFF`), not a
literal 0 — the types associate with no global parameter, and a literal 0 would
point a host at real plug-in parameter id 0. The `kAssociatedParameterIDValid`
flag stays clear, so the field is advisory.

**String conversions are surface-validated:** `getNoteExpressionStringByValue` /
`getNoteExpressionValueByString` decline (`kResultFalse`) unless MPE is on, the
bus is 0, the channel is in `[0, 16)`, AND the type id is one of the declared
`kNoteExprTypes` — they don't blindly convert an arbitrary type id.

### Audio buses (incl. sidechain)

Same "bus 0 = main, bus 1 = sidechain" rule as CLAP/AU. The adapter
defensively guards against inactive sidechain buses:

```cpp
if (data.numInputs > 1 &&
    data.inputs[1].numChannels > 0 &&
    data.inputs[1].channelBuffers32 &&
    data.inputs[1].channelBuffers32[0]) {
    // publish sidechain
}
```

Secondary **output** buses **are routed** to the richer
`Processor::process(ProcessBuffers&)` surface (role `Aux`, index ≥1) for
multi-out instruments — identical model to CLAP. Each aux bus is
pre-zeroed before `process()`, so a single-output processor leaves aux
buses silent; a multi-out processor that overrides
`process(ProcessBuffers&)` writes each declared aux bus.

**Size aux storage from the ACCEPTED arrangement, not the descriptor
default.** `setBusArrangements` can accept a mono→stereo shift on an aux
bus (when `is_bus_layout_supported` permits it). `setupProcessing` sizes
each `aux_output_ptrs_[i]` sub-vector from the live `AudioBus`
arrangement (`bus->getArrangement()` → `SpeakerArr::getChannelCount`),
not `desc.output_buses[b].default_channels` — otherwise the routing path
clamps to the descriptor default and silently drops the channel the host
negotiated. The aux view's `declared_channels` reports the descriptor
count (cached in `declared_aux_channels_`) while `buffer.num_channels()`
carries the routed count, so `matches_declared_layout()` detects a
mismatch. Sizing is `max(declared, accepted)`. Storage is bounded by
`BusBufferSet::kMaxBuses`; wider host layouts are zero-filled, not routed.

The process callback builds a stack-owned `ProcessBuffers` block for
the active main input, optional sidechain input, main output, **and each
routed aux output**, then dispatches through
`Processor::process(ProcessBuffers&, ...)`.
Processors that only override the legacy main-in/main-out callback
still run through the base projection; processors that override the
richer surface can inspect the VST3 bus set directly.

### Transport context

`ProcessContext` is populated from `data.processContext`:

- `is_playing` from `state & kPlaying`.
- `tempo_bpm` always read.
- `position_samples` always read.
- `time_sig_numerator/denominator` only when
  `state & kTimeSigValid`.

No `processContextRequirements` flag is currently requested — if a
host needs opt-in declaration of which fields Pulp reads, we will add
`IProcessContextRequirements`. Today every supported host delivers all
required fields by default.

### State save / restore

`getState(stream)` serialises `store_.serialize()` bytes directly.
`setState(stream)` chunks up the stream via a 4 KiB buffer, feeds
`store_.deserialize`, and then `setParamNormalized`s every restored
param back through the Steinberg parameter cache so the host UI
re-reads the correct values. Format is identical to CLAP/AU — test
with a round-trip across all three adapters for parity regressions.

### Editor

`createView("editor")` returns a `PulpPlugView` (in
`vst3_plug_view.cpp`) when the build defines `PULP_VST3_GUI` and the
Processor `has_editor()`. The editor flows through
`pulp::format::ViewBridge` — see the `view-bridge` skill for the
lifecycle protocol. Editing `vst3_plug_view.cpp` triggers `view-bridge`,
not this skill.

## Gotchas

### Missing `vst3_entry.cpp` → no `GetPluginFactory` symbol → silent host reject

`PulpPluginFormats.cmake`'s `_pulp_add_vst3()` only links a user-side
factory file if `${CMAKE_CURRENT_SOURCE_DIR}/vst3_entry.cpp` exists.
The macro `PULP_VST3_PLUGIN(...)` (from `vst3_entry.hpp`) is what
expands to Steinberg's `BEGIN_FACTORY_DEF / END_FACTORY` block — and
that block is where `extern "C" GetPluginFactory()` is **defined**.
Without `vst3_entry.cpp`, the linked `.vst3` has `bundleEntry` /
`bundleExit` from `macmain.cpp` but **no `GetPluginFactory`** at all.

`pulp_add_plugin(... FORMATS VST3)` will still build the bundle
cleanly. CLAP / AU / AUv3 entry files have separate registration
paths (`PULP_AUV3_PLUGIN`, etc.) — adding only those does **not**
cover VST3. Hosts call `dlsym(bundle, "GetPluginFactory")`, get NULL,
and silently drop the plugin during scan. In Reaper that shows up as
a hash-only `MyPlugin.vst3=<hash>` line in
`reaper-vstplugins_arm64.ini` (no comma-separated UID/name after the
hash) — exactly the same surface symptom as the UID-collision case
below, so check both.

This bit us when porting ChainerSynth to VST3: AU/AUv3/CLAP all
loaded; VST3 disappeared from Reaper after rescan with no log.

**Diagnostic — verify the symbol exists before debugging anything
else:**

```bash
# All factory/bundle symbols (C-linkage, leading underscore on macOS)
nm -gU MyPlugin.vst3/Contents/MacOS/MyPlugin | grep -v __Z | \
    grep -iE 'factory|bundle'
# Expect:  _GetPluginFactory  _bundleEntry  _bundleExit
# If _GetPluginFactory is missing, you're hitting this gotcha.
```

A 30-line `dlopen` + `dlsym` probe is the fastest way to confirm a
silently-broken VST3; reuse the pattern in
`tools/scripts/probe_vst3_factory.c` (if absent, write a one-off — it
beats round-tripping through a DAW for every build).

**Fix:** add a `vst3_entry.cpp` next to the plugin sources:

```cpp
#include "my_plugin.hpp"
#include <pulp/format/vst3_entry.hpp>

static const Steinberg::FUID MyPluginUID(0x50554C50, 0x...);

PULP_VST3_PLUGIN(MyPluginUID, "MyPlugin",
                  Steinberg::Vst::PlugType::kInstrumentSynth,
                  "Vendor", "1.0.0", "https://example.com",
                  pulp::examples::create_my_plugin)
```

Then **reconfigure** (`cmake -S . -B build`) — CMake's
`if(EXISTS ...)` for `vst3_entry.cpp` is evaluated at configure time,
so a plain `cmake --build` will not pick the new file up.

**Long-term:** `pulp_add_plugin(... FORMATS VST3)` should either
fail-fast with a clear error at configure time when `vst3_entry.cpp`
is missing, or auto-synthesize a default factory from the existing
`PLUGIN_CODE`/`MANUFACTURER_CODE`/`CATEGORY` arguments. Either is
better than the current silent-drop default.

### Reaper de-dupes VST3s by VST3 UID (and silently rejects collisions)

Reaper's macOS VST3 scanner (`reaper-vstplugins_arm64.ini`) keys plugins
by VST3 UID. If you install a new `.vst3` whose UID matches an entry
already in the scan database — even from a `.vst3` no longer on disk —
Reaper marks the new bundle as **"Plug-ins that failed to scan"** with
no console output and no crash log. The default Reaper preference
"Allow multiple plug-ins with the same VST3 UID" is OFF, so two builds
of the same plugin under different paths cannot coexist.

A side-by-side build under a second bundle path can hit this even when the
older bundle has been deleted: Reaper's scan DB may still hold an orphaned
entry from the previous path, and the new VST3's UID collision triggers the
silent reject.

**Diagnostics (no log, no crash):**

1. `Reaper → Preferences → Plug-ins → VST → Re-scan… → "Plug-ins that
   failed to scan"` shows the rejected path.
2. `grep -i <plugin-name> ~/Library/Application\ Support/REAPER/reaper-vstplugins_arm64.ini`
   reveals both the orphaned entry (path that no longer exists) and the
   under-scored failing-scan entry (no plugin metadata after `=`).

**Fix:** delete stale entries from Reaper's scan DB AND install the
plugin under exactly one path:

```bash
# Quit Reaper first
sed -i.bak '/^MyPlugin.*\.vst3=/d' \
    ~/Library/Application\ Support/REAPER/reaper-vstplugins_arm64.ini
# Install under one canonical name
rm -rf ~/Library/Audio/Plug-Ins/VST3/MyPlugin*.vst3
cp -R build/VST3/MyPlugin.vst3 ~/Library/Audio/Plug-Ins/VST3/MyPlugin.vst3
# Relaunch Reaper — it will re-scan fresh
```

Alternatively the user can flip "Allow multiple plug-ins with the same
VST3 UID" in Reaper Preferences, but the default-off behavior is the
one most VST3 hosts enforce, so the workflow above is more durable.

**Don't** ship two builds of the same plugin with the same VST3 UID —
if you need a side-by-side comparison build, bump the VST3 UID's
SubCategory bytes (last 4 bytes of the 16-byte UID, by convention) so
the two builds register as separate plugins.

### `DEVELOPMENT` / `RELEASE` macro must precede the SDK include

VST3 SDK fails to compile unless **exactly one** of `DEVELOPMENT` or
`RELEASE` is defined. `vst3_adapter.hpp` defines them from `NDEBUG` at
the top of the file. If you rearrange includes and pull an SDK header
in before that block, you get a confusing sea of SDK complaints. Keep
the define block first.

### `#include <public.sdk/source/vst/vstsinglecomponenteffect.h>` first

The VST3 SDK demands this header be the first SDK include — ordering
requirements bleed through its internal `#pragma` state. Honour that
even when IDE auto-formatting wants to reorder.

### Host reports `numChannels > 0` but buffer is null

A VST3 bus can be active per its channel count but have
`channelBuffers32 == nullptr` or `channelBuffers32[0] == nullptr` when
the host hasn't actually activated the bus. The main-input branch
doesn't guard against this today — only the sidechain branch does. If you
ever hit a null-deref on bus 0, the same guard needs to apply there. See
CLAP's sidechain guard for the parallel fix.

### `setupProcessing` reuses the same `ProcessSetup` across re-activation

Steinberg's SDK does not guarantee a fresh `ProcessSetup` on each
`setActive(true)` — hosts commonly reuse the same setup. Don't rely on
`setupProcessing` being called again just because the host toggled
active — if you need to recompute anything per-activation, hook it
into `setActive` instead.

### `param_snapshot_` is **post-input-events, pre-process**

Same contract as CLAP — host events are applied first, then snapshot,
then process. If the adapter needs additional logic (e.g.
gesture-coalesced output events), insert it after `process()` and
before the snapshot diff, not before `set_normalized`.

### `getTailSamples` returns `kInfiniteTail` for `tail_samples < 0`

Pulp uses `tail_samples = -1` to mean "infinite" (reverb, delay with
feedback). The adapter converts that to Steinberg's
`Steinberg::Vst::kInfiniteTail` constant. Hosts interpret the literal
`0xFFFFFFFF` value — do not clamp the tail to any other uint32.

### `addParameter` must match the later `setParamNormalized` ID

Parameters are indexed by the `ParamID` cast from Pulp's
`ParamInfo::id`. If a plugin re-orders its parameter registration
between versions, stored automation data breaks. Do not reorder —
append only, and never reuse a retired ParamID. This is a VST3-wide
backward-compat requirement, not a Pulp quirk.

### Only mono + stereo are negotiable today

`setBusArrangements` rejects anything other than
`SpeakerArr::kMono` or `kStereo`. Surround / immersive layouts
require expanding the `supported` lambda — do not add surround without
verifying the descriptor, DSP, and `kSpeakerArr` constants align.

### VST3 SDK is MIT, fetched via `git clone` in setup.sh

`external/vst3sdk` is not checked in. `setup.sh` clones v3.7.12 by
default. If you bump the SDK version, also update the note in
`docs/guides/formats.md` and verify `public.sdk/source/...` ABI didn't
shift.

## Validation recipes

Build and validate a VST3 bundle:

```bash
./build/pulp build
./build/pulp validate         # runs pluginval --strictness-level 5
```

Direct `pluginval` invocation (matches what `cmd_validate.cpp` uses):

```bash
pluginval --strictness-level 5 --timeout-ms 30000 \
  --validate "$(pwd)/build/path/to/MyPlugin.vst3"
```

`pluginval` install paths:

```bash
brew install pluginval               # macOS (Homebrew tap)
# Linux / Windows: download a release binary from
# https://github.com/Tracktion/pluginval/releases
```

`pluginval` returns a non-zero exit code on any strictness-5 failure.
Treat it as gating — VST3 bundles failing strict pluginval must not
ship. `pulp build --install` refuses to copy a failing VST3 into
`~/Library/Audio/Plug-Ins/VST3/`.

### Validator runs must disable editor creation

`pluginval` may ask the VST3 adapter for its editor during validation.
Build/test automation must run it with
`PULP_DISABLE_PLUGIN_EDITOR=1 PULP_HEADLESS=1 PULP_TEST_MODE=1`; the
adapter returns `nullptr` from `createView(kEditor)` under those guards.
Do not remove this environment just because `pluginval` also has
`--skip-gui-tests` -- hosts can still probe editor availability.

## Cross-references

- `.agents/skills/view-bridge/SKILL.md` — the editor contract;
  `vst3_plug_view.cpp` edits route through that skill.
- `.agents/skills/ara/SKILL.md` — IHostApplication-based factory
  negotiation (`kVst3AraFactoryContextKey`).
- `.agents/skills/mpe/SKILL.md` — MPE sidecar. The adapter now wires
  per-note expression directly: it implements `INoteExpressionController`
  and routes `Event::kNoteExpressionValueEvent` through the shared
  `MpeVoiceTracker` / `MpeBuffer` to `Processor::set_mpe_input()` (gated on
  `desc.supports_mpe`). See "Per-note expression (MPE)" above. Channel
  per-note short MIDI (member-channel pitch bend / pressure / CC74) also
  still reaches the tracker via the normal `IMidiMapping` → `midi_in_` path.
- `.agents/skills/clap/SKILL.md` and `.agents/skills/auv3/SKILL.md` —
  cross-format parity for host-specific regressions.
- `docs/guides/formats.md` — user-facing format overview.
- `docs/guides/host-matrix.md` — per-host VST3 + ARA compatibility.
- Memory note: "Tests ship with fixes" — every VST3 process-path behavior
  change needs a Catch2 fixture.

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

## silence_unsupported_bus_arrangements

`setBusArrangements` gates its rejection of unsupported layouts on the
`silence_unsupported_bus_arrangements` quirk (cached in `quirks_` at init):

- Bus-COUNT mismatch still hard-rejects (structural, not an arrangement issue).
- An arrangement the processor can't natively support (non-mono/stereo, or
  `is_bus_layout_supported()` says no): with the quirk enforced it is
  **accepted** (`setArrangement` to the host request, `silence_unsupported_active_=true`);
  with `PULP_HOST_QUIRKS=off` the original `kResultFalse` reject is preserved.

**Key invariant:** `setupProcessing` always `prepare()`s the processor with
*descriptor-default* channel counts (cached as `native_in_`/`native_out_`),
NOT the negotiated arrangement. So when `silence_unsupported_active_`,
`process()` hands the processor **clamped** views (`min(host, native)`) and
**zero-fills all of the host's main-bus output channels first** — the
processor never reads/writes past what `prepare()` allocated, and the host's
extra channels emit silence instead of uninitialised memory. Empirical proof:
`pulp-test-vst3-plugin-state` `[bus-arrangement]` drives a 5.1 output through
a stereo processor and asserts channels 2–5 are silent + the processor saw 2.

## synthesize_bypass_parameter

When the plugin declares no Bypass parameter and the quirk is enforced,
the adapter calls `pulp::format::maybe_synthesize_bypass(store, quirks)`
(in `quirk_apply.hpp`) right after `define_parameters` — injecting an
automatable boolean `"Bypass"` param with the reserved ID
`kSynthesizedBypassParamId` (0x70427970). The adapter's EXISTING bypass
detection (name == "Bypass", boolean range) then adopts it, so the
pass-through short-circuit honors it with no further wiring.
`PULP_HOST_QUIRKS=off` synthesizes nothing. Existing "no-bypass" tests
must set `kQuirkFilterOff` to keep that premise. (CLAP + AU v2 are NOT
wired — they have no bypass process path; injecting a param there would
appear-but-do-nothing, so they're a separate follow-up.)

## Bypass pass-through null-guard

The `processBlockBypassed` short-circuit in `process()` MUST null-check
`output_ptrs_[ch]` before the memcpy/memset — a VST3 bus can report
`numChannels > 0` while an individual `channelBuffers32[ch]` is null.
This is reachable for plugins that never declared a Bypass because the
synthesized-bypass quirk can still enter the short-circuit. Regression:
`pulp-test-vst3-plugin-state`
`[vst3][bypass][regression]` runs the bypass path with a null channel-1
output pointer and asserts no crash + the live channel still passes through.

## silence_unsupported_bus_arrangements — honor processor vetoes

The silence accommodation applies ONLY to non-mono/stereo (exotic, e.g. 5.1)
arrangements — there it accepts + silences the extra channels. A mono/stereo
layout the processor vetoes via `is_bus_layout_supported()` is a real
contract (linked main/sidechain counts, stereo-only) with no extra channels
to silence, so `setBusArrangements` HONORS the veto (returns kResultFalse)
even with the quirk on — matching the baseline behavior. Regression:
`pulp-test-vst3-plugin-state` `veto_bus_layout` config + the
"honors a processor mono/stereo bus-layout veto" case.
