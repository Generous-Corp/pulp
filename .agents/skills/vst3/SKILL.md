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
| VST3 SDK fetch | `external/vst3sdk` — `git clone --depth 1 --branch v3.8.0_build_66 https://github.com/steinbergmedia/vst3sdk.git` (MIT) |
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

### Multi-plugin bundle — one VST3 binary, many plugins

VST3's factory natively lists multiple classes (multiple `DEF_CLASS2` between one
`BEGIN_FACTORY_DEF`/`END_FACTORY`), so a bundle is structured exactly like
Steinberg's own multi-class factory. The bundle macros in `vst3_entry.hpp`:

```cpp
PULP_VST3_BUNDLE_PLUGIN(Foo, create_foo, {.id="com.x.foo"})   // file scope: create fn + keyed reg
PULP_VST3_BUNDLE_PLUGIN(Bar, create_bar, {.id="com.x.bar"})
PULP_VST3_FACTORY_BEGIN("My Co", "https://x.com", "mailto:info@x.com")
    PULP_VST3_BUNDLE_CLASS(Foo, kFooUID, "Foo", Steinberg::Vst::PlugType::kFx, "1.0.0")
    PULP_VST3_BUNDLE_CLASS(Bar, kBarUID, "Bar", Steinberg::Vst::PlugType::kFx, "1.0.0")
PULP_VST3_FACTORY_END
```

Each `_PLUGIN` generates a file-scope `_pulp_vst3_create_<Ident>` bound to its own
`factory_fn` (`PulpVst3Processor` already takes the factory — no global read), and
registers a keyed `PluginRegistration` (see the `auv2` skill + `registry.hpp`) for
enumeration / per-plugin editor assets. Load-bearing rules:

- **`Ident` links `_PLUGIN` to `_CLASS`** by naming the same generated create-fn
  symbol — a mismatch fails to COMPILE (undefined symbol), so the pair can't
  silently desync. **Every class needs a distinct `FUID`** (Reaper de-dupes by
  UID — see that gotcha below).
- **`factory_fn` is the single source of truth**, force-assigned onto the keyed
  entry's `.factory`; do NOT set `.factory` in the braced-init (ignored). Same
  rule as the AU bundle macros.
- **A test/executable that expands the factory macros must compile the SDK glue**
  the plugin CMake target does — `public.sdk/source/main/pluginfactory.cpp`
  (`CPluginFactory`/`gPluginFactory`), `moduleinit.cpp`, and the platform main
  (`macmain.cpp` defines `moduleHandle`, referenced by `moduleinit.cpp`). Omitting
  the platform main gives an undefined `_moduleHandle`. See
  `test/test_vst3_bundle_entry.cpp` + its target in
  `test/cmake/core_audio_platform_format_tests.cmake`.

Single-plugin `PULP_VST3_PLUGIN` is unchanged; a bundle is opt-in.

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
counts, then prepares the default f64 fallback scratch. `setActive(false)` calls
`processor_->release()` so the
Processor can free prepare-time resources. Never move `prepare()` out
of `setupProcessing` — Steinberg guarantees `process()` is only called
after a successful `setupProcessing` + `setActive(true)` sequence.

VST3 can ask for `kSample64`. If
`PluginDescriptor::effective_capabilities().supports_f64_audio` is false, keep
the adapter-boundary conversion path: copy host double buffers to f32 scratch,
run the existing f32 Processor callback, and copy active outputs back to double.
Only call `Processor::process_f64(ProcessBuffers64&, ...)` for plugins that
explicitly opt in; native f64 processors are responsible for doing real double
DSP, not relying on the compatibility conversion. On the boundary path the
output scratch is pre-zeroed for only `num_samples` frames (the writeback
copies exactly that many), and the aux f32→f64 writeback is bounded by the
routed view's channel count — not the host bus channel count — so a bus
demoted by a null mid-bus channel pointer keeps its block-start pre-zero
instead of reading back stale scratch.

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

`kIsBypass` is set on whichever parameter the shared
`pulp::state::is_bypass_param` contract identifies: a Processor that
declares `ParamInfo::designation = ParamDesignation::Bypass` marks its
bypass control *by intent, independent of name*; otherwise the legacy
heuristic (a parameter named `"Bypass"` with range `[0,1]`, `step >= 1`)
applies as the fallback so existing plugins are unchanged. Steinberg
requires exactly one bypass parameter per plugin for the host bypass
control to work, so declare at most one. **A declared `Bypass` param is
FORCED to a two-state toggle** (`stepCount = 1`) regardless of its
`ParamRange`: the designation is interpreted as off<0.5 / on>=0.5, and
`process()` short-circuits on that same threshold, so a continuous range
must not leak to the host as a knob. (The legacy name/range heuristic
only matches a boolean range, so it is a no-op for it.)

**Trigger / momentary params.** The adapter calls
`StateStore::reset_triggers_rt()` as a **single-exit invariant of the
audio callback** to auto-reset trigger params (`ParamInfo::is_trigger`,
or a `ParamDesignation::Reset` "reset/panic" control) back to their
default. It runs on BOTH paths: in the normal path before the
output-parameter-change scan (so the host records the auto-reset as
automation), AND right before the bypass short-circuit's
`return kResultOk` — otherwise a panic/reset raised while bypassed would
fire late on the next non-bypassed block. Incoming host param changes are
applied at the top of `process()`, before either exit, so a trigger
raised this block is always observed-then-settled within it.

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

VST3 wiring: `process()` consumes
`consume_latency_changed_flag()` / `consume_tail_changed_flag()` and
OR-accumulates the resulting restart flags into `restart_publisher_`
(`detail::Vst3RestartPublisher`) — a lock-free, allocation-free atomic.
It does **not** call `componentHandler->restartComponent()` from
`process()`. `restartComponent` is a HOST callback that may take locks,
allocate, and synchronously re-enter the plug-in (some hosts deactivate
and reactivate the component inside it), so calling it on the real-time
audio thread is an RT-safety violation even though latency changes are
infrequent. The matching `restartComponent(flags)` fires on the main
thread via `drain_pending_restart()` / `deliver_pending_restart()`.

Delivery is driven primarily by a **paced self-rescheduling poll on the
host main thread** (`MainThreadDispatcher::call_async_after`, ~33ms,
started at `setActive(true)` and stopped at `setActive(false)` /
`terminate()`). VST3 — unlike CLAP, which has
`clap_host->request_callback` — exposes no RT-safe host wakeup an
audio-thread block could trigger, and the audio thread must not post
(`call_async` allocates/locks). The poll closes that gap: a mid-stream
latency/tail change is delivered within one tick even if the host never
issues another query. The poll tick is cheap (an idle tick is one
acquire load + early return). As belt-and-suspenders the drain also runs
from the main-thread host entrypoints the adapter already receives
(`getLatencySamples` / `getTailSamples` / `setActive` / `getState`).
When a backend reports the call is running off the main thread, the
delivery is marshaled to the main thread via a one-shot `call_async`.
The publisher coalesces a burst of flagged blocks into a single host
notification.

**Lifetime safety.** Every main-thread lambda the adapter posts (the
paced tick and the off-main one-shot) captures a shared `alive` flag and
only touches `this` while it is true. `terminate()` clears that flag
before the component is destroyed, so a tick still queued on the host
run loop becomes a safe no-op rather than a use-after-free. `terminate()`
and the queued lambda both run on the host main thread, so they cannot
race; the flag check is authoritative. The `poll_active_` flag is
cleared on deactivate/terminate so the chain stops re-posting.

Tests: `pulp-test-processor-layout-latency` pins the processor round-trip
and the two-thread hammer; the `[vst3][rt-safety]` cases in
`pulp-test-vst3-plugin-state` assert `restartComponent` never fires
synchronously on the audio thread, fires once with the coalesced flags
on the main thread, that the paced poll delivers a change with **no**
host query, that flagging a restart adds no allocation to `process()`,
and that a tick queued past `terminate()` is a no-op (no UAF). The
publisher `static_assert`s its atomics are always lock-free.

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
- **The event input/output buses must declare 16 channels**
  (`addEventInput(name, 16)` / `addEventOutput(name, 16)`), not 1. Hosts query
  `getMidiControllerAssignment` per input channel up to the input bus's
  `channelCount`, and MIDI-output-aware hosts use the output bus count for
  multi-channel routing. A 1-channel event bus silently collapses routing to
  channel 0.
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
`midi_out_` are written back into `data.outputEvents`, with each event's
`Event.sampleOffset` set from the shared cross-format helper
`detail::vst3_output_offset(me.sample_offset)` (identity for VST3 — the host
clamps the signed offset). That helper lives in
`core/format/include/pulp/format/detail/midi_out_offset.hpp` and is the single
source of truth for the "offset N in → offset N out" contract shared with AU v2
and CLAP; do NOT re-open-code the offset mapping here. Parity is pinned by
`test/test_midi_out_offset_parity.cpp`.

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

### `process()` phase order is the contract

`process()` reads as a sequence of named phases — `build_process_context`,
`write_midi_output`, and the inline blocks between them. Named phases look like
independent steps that would be safe to reorder or reuse. They are not: the
order carries contracts nothing checks for you.

- **`midi_in_.sort()` must precede the MPE tracker loop.** `MpeVoiceTracker` is
  a state machine over a sample-ordered stream — controllers and note events are
  appended from two independent sources, so an unsorted stream attributes
  expression to the wrong notes.
- **`build_process_context()` is stateful, not a pure decode.** It advances
  `playhead_prev_` in place, so it must run exactly once per block and on every
  block. Its name and signature suggest otherwise: call it twice, or skip it on
  an early-return path, and the *next* block's `transport_changed` /
  `transport_jump` flags are wrong. That surfaces far away, as a DSP reset that
  never fires.
- **`snapshot_param_values()` runs after host input events, before `process()`**
  (see "`param_snapshot_` is post-input-events, pre-process").
- **The explicit output-event pass must precede the snapshot-diff pass** — the
  diff reads the skip-set the first pass fills, and without it a param that
  already reported sample-accurate points gets a stale offset-0 duplicate.

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

`external/vst3sdk` is not checked in. `setup.sh` clones v3.8.0_build_66 by
default. If you bump the SDK version, also update the note in
`docs/guides/formats.md` and verify `public.sdk/source/...` ABI didn't
shift.

The pin may not go below v3.8.0: that is the first tag Steinberg released
under MIT, and Pulp redistributes VST3 headers inside its own MIT-licensed
SDK artifacts. Earlier tags offer only "Steinberg VST3 License OR GPLv3",
neither of which Pulp can redistribute. `assert_vst3_license_is_mit` in
`setup.sh` fails the setup if a re-pin drifts back.

## Validation recipes

### Explicit parameter semantics and layout sets

VST3 flags, step counts, labels, and text conversion consume the shared
`ParamInfo::kind`, `value_labels`, and canonical parameter-text helpers. Do not
restore range heuristics or numeric enum parsing. Named
`supported_bus_layouts` constrain `setBusArrangements` through
`is_bus_layout_supported`; `Processor::BusesLayout` remains the unnamed
two-vector runtime proposal.

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

## Sample-accurate parameter output

The plugin→host param-output drain (`vst3_adapter.cpp`, after `process()`) emits
**one `IParamValueQueue` per `ParamID`, with points in ascending sample offset**
— VST3 requires per-queue ascending offsets. Explicit events the processor
pushed via `Processor::push_output_param_event()` are drained first (globally
sorted by offset, each offset **clamped to `[0, numSamples-1]`**), and a
per-parameter **skip-set** stops the legacy before/after snapshot diff from also
emitting a stale offset-0 point for a param that already reported explicit
events. The queue cache and skip-set are pre-sized at block start (alongside
`param_snapshot_`), so the drain is allocation-free. Values are **normalized**
(`ParamInfo::range.normalize`). `test_vst3_plugin_state.cpp` asserts two distinct
offset points (16, 48) with no offset-0 duplicate.

## Output silenceFlags are the plugin's job — clear them after an active render

VST3 puts the obligation to declare output silence on the **plugin**. A host is
free to hand us `AudioBusBuffers` whose `silenceFlags` already carry its inbound
silence, and to act on whatever we leave in that field. The adapter never wrote
it, so a plugin that synthesizes output from a silent input — generator,
oscillator, reverb tail, DC / control-voltage source — returned a full buffer
still labelled silent.

After `processor_->process(...)`, on the non-bypass path:

```cpp
for (int32 b = 0; b < data.numOutputs; ++b) data.outputs[b].silenceFlags = 0;
```

Every rendered output bus, every block — a host that keeps asserting silence must
keep having it retracted, not just on the first block. The bypass path returns
before this point and deliberately leaves the flags alone: there the plugin is a
wire, so upstream silence stays silence.

Do not try to prove this with a `Processor`-level test. `Processor::process()`
writing `0.5` into a buffer passes whether or not the flag is set; the defect
only exists at the adapter boundary. `test_vst3_plugin_state.cpp`'s `[silence]`
case drives `PulpVst3Processor::process()` with `outputs[0].silenceFlags = 0x3`
and asserts it comes back `0`.

## The StateStore must outlive the Processor

`Processor::state()` dereferences a pointer the host installs. A Processor may
follow it for its whole lifetime — from `process()`, from its destructor, and from
any worker thread that destructor is about to `join()`. So the host has to keep the
store alive until the Processor is gone.

In practice that is one rule about member order: **declare the `state::StateStore`
before the `std::unique_ptr<Processor>`.** Members are destroyed in reverse
declaration order, so the store then dies last. Every host in `core/format` had it
backwards until 2026-07; the effect is nothing at all for a Processor with no
threads, and a use-after-free on plug-in close for one with a background thread that
reads `state().get_value()` while the destructor walks to its `join()`.

It crashes only on close, only sometimes, and the DAW gets the blame. The regression
test is `test/test_store_lifetime.cpp`; it observes the store's destruction through a
sentinel owned by a parameter's `to_string` closure rather than reading freed memory
and hoping the result looks wrong.

A Processor should not *rely* on this either: a worker thread that reads the store on
every tick is one host away from the same crash. Publish what the thread needs to
atomics from `process()` instead.

## Parameter display strings — override the EditController methods, convert domains

Hosts show and accept per-parameter text through the `IEditController` pair
`getParamStringByValue` / `getParamValueByString`. `SingleComponentEffect`
provides stock numeric formatting, so if the adapter does NOT override these,
a plugin's `ParamInfo::to_string` / `from_string` are silently ignored and the
host shows the base class's generic number instead. `PulpVst3Processor`
overrides both (`vst3_adapter.cpp`):

- These methods work in the **normalized [0,1]** domain, but `to_string` /
  `from_string` work in **plain (min..max)** units. Denormalize on the way in
  (`info->range.denormalize(valueNormalized)` before `to_string`) and normalize
  on the way out (`info->range.normalize(plain)`). Skipping this shows the
  normalized 0..1 number, not the real value.
- Decline to the base class (`return SingleComponentEffect::...`) when the
  parameter has no converter, so existing plugins and the hidden MIDI-controller
  params (which have no `ParamInfo`) keep the stock path.
- `from_string` is author code: guard with `std::isfinite` and fall through to
  the base parser on a non-finite result rather than writing a garbage
  normalized value. Test: `test/test_vst3_param_display.cpp`.

## The VST3 adapter consumes the shared adapter-boundary core

`core/format/include/pulp/format/adapter_boundary.hpp` is the shared home for f64 param
marshalling, latency-compensated bypass, param dual-write, and output-parameter
publication. VST3 consumes it (`boundary::LatencyCompensatedBypass`,
`boundary::apply_param_value`, `boundary::find_param_index`,
`boundary::snapshot_param_values`, `boundary::changed_since_snapshot`, the f64 helpers,
`kBoundaryMaxChannels`) instead of carrying local copies. Fix marshalling/bypass/
publication semantics in `adapter_boundary.hpp`, not in the adapter. The header's prose
states actual per-adapter adoption — trust it over an assumption that all adapters are
migrated (AU/LV2 still carry local bypass, and AU/AAX/LV2 still pass the dry signal
through UNDELAYED while bypassed, a real PDC-misalignment bug tracked as its own
follow-up). VST3 also does **not** use `boundary::apply_host_transport`: it drives
`detail::playhead_diff` directly from `build_process_context`.

**Shared with CLAP — that is the point, and the trap.** The publication bookkeeping is one
body of code behind two host ABIs. Editing it for VST3 changes CLAP in the same commit,
and "fixing" VST3 by re-inlining a local variant silently re-opens the divergence these
two paths already drifted into once as copies. A change that is genuinely VST3-only
belongs in the adapter's host-ABI emit (the `IParamValueQueue` points, the
`setParamNormalized` sync), never in the shared bookkeeping. `find_param_index` is a
**linear scan on purpose** — param counts are small and it is what both adapters already
did. If it ever becomes hot, index it in `adapter_boundary.hpp` for *both* adapters; do
not add a VST3-local map.

## The adapter has an end-to-end null — extend it rather than trusting a diff

`test/test_vst3_audio_parity.cpp` renders one deterministic Processor through
`HeadlessHost` and through the real `PulpVst3Processor::process()` — `ProcessData` /
`AudioBusBuffers` / `IEventList` / `IParameterChanges`, the plumbing a host builds — and
requires the output to be **bit-identical** (memcmp, no tolerance) across a sample-rate ×
block-size sweep including a ragged final block. `HeadlessHost` shares no adapter code, so
the reference cannot drift with the thing it is checking. `test_adapter_boundary_parity.cpp`
drives VST3 only as a neutral-struct matrix column and never enters `process()`; it is not
a substitute.

Things that fixture learned the hard way, if you extend it:

- **The store carries one MORE param than you registered.** `initialize()` runs
  `maybe_synthesize_bypass()`, and `synthesize_bypass_parameter` defaults **on**, so a
  synthesized `Bypass` (`kSynthesizedBypassParamId`, `'pByp'`) is appended last. Every
  index the publication path resolves is a position in *that* list — an off-by-one lands
  on Bypass and silently normalizes against its `{0,1}` range without going out of bounds.
  (Relatedly, `getParameterCount()` is 2085, not 5: it also counts the 16×130 hidden
  MIDI-CC controllers registered for `IMidiMapping`. Assert on `getParameterInfo(i).id`,
  never the count.)
- **The host boundary is normalized, so a null needs values that survive it.** The harness
  normalizes and the adapter denormalizes, while the direct path writes the plain value.
  Only values that round-trip bitwise (dyadic rationals on a linear range) compare fairly.
  The fixture asserts that round-trip rather than assuming it, so a drift there names the
  range math instead of blaming the adapter.
- **VST3 has no raw-short-message event.** Notes arrive as `kNoteOnEvent` with a float
  velocity the adapter rebuilds as `uint8_t(velocity * 127.0f)` — lossy. The fixture uses
  dyadic velocities (0.5 → 63) and feeds the direct path the byte decode produces, so the
  stimulus stays identical and the null measures the adapter, not the note encoding.
- **Publication faults are invisible in the waveform.** A plugin-side param change never
  reaches the samples, so the audio null stays green through a wholly broken publication
  path. That is why the param cases are separate assertions — verified by mutation: moving
  the pre-process param snapshot after `process()` reddens only the publication cases and
  leaves the null passing.
- **`setParamNormalized` in the skip branch is load-bearing and easy to drop.** It keeps
  the EditController synced for params that emitted explicit sample-accurate points.
  Deleting it changes no sample, so it was covered by nothing until the null's
  controller-read case; without that case a refactor that drops it ships green while the
  host UI and host-side automation reads show a stale value.
