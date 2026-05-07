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
pass-through to the base class. Pulp's implementation:

1. Rejects with `kResultFalse` if `numIns`/`numOuts` don't match the
   descriptor's declared bus counts.
2. Rejects if any requested speaker arrangement is neither `kMono` nor
   `kStereo` — negotiation is currently limited to those two.
3. Otherwise updates each `AudioBus` via
   `bus->setArrangement(arrangement)` in place, then re-propagates to
   `SingleComponentEffect::setBusArrangements`.

Why: hosts swap project channel layouts (load a stereo session over a
mono plugin slot) and expect the plugin's `descriptor()` view to
follow. Without the in-place update, the Pulp Processor's channel
counts diverge from the VST3 bus state. See commit
`b9cda370 vst3: dynamic bus arrangements — honor setBusArrangements
(#240)`.

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
  `data.inputParameterChanges`, takes the **last** point from each
  `IParamValueQueue`, and calls `store_.set_normalized(id, value)`. VST3
  values are always normalised 0..1 — `set_normalized` converts to the
  ParamInfo's real range.
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

### MIDI events

VST3 delivers note-on / note-off through `IEventList`:

```
Event::kNoteOnEvent  → MidiEvent::note_on
Event::kNoteOffEvent → MidiEvent::note_off
Event::kDataEvent (type=kMidiSysEx) → midi_in.add_sysex(bytes, sampleOffset, 0.0)
```

Non-note short MIDI (CC, pitch bend, aftertouch) is **not** delivered
by Steinberg's event list — VST3 hosts translate those into parameter
automation using `kIsMidiCC`-tagged parameters. If you need them, model
them as Pulp parameters, not MIDI. See `docs/guides/formats.md`.

MIDI output mirrors the inverse: note_on / note_off in
`midi_out` are written back into `data.outputEvents`.

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

(See commit `c0f49a63 Workstream 01 slice 1.2: VST3 multi-bus +
sidechain routing` and the `#178` review.) Secondary **output** buses
are zero-filled every block — identical rationale to CLAP.

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
doesn't guard against this today — only the sidechain branch does
(per #178 review). If you ever hit a null-deref on bus 0, the same
guard needs to apply there. See CLAP's #277 for the parallel fix.

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

## Cross-references

- `.agents/skills/view-bridge/SKILL.md` — the editor contract;
  `vst3_plug_view.cpp` edits route through that skill.
- `.agents/skills/ara/SKILL.md` — IHostApplication-based factory
  negotiation (`kVst3AraFactoryContextKey`).
- `.agents/skills/mpe/SKILL.md` — MPE sidecar (VST3 hosts deliver MPE
  as channel-per-note short MIDI; the adapter routes it through the
  same `MpeVoiceTracker` path CLAP uses).
- `.agents/skills/clap/SKILL.md` and `.agents/skills/auv3/SKILL.md` —
  cross-format parity for host-specific regressions.
- `docs/guides/formats.md` — user-facing format overview.
- `docs/guides/host-matrix.md` — per-host VST3 + ARA compatibility.
- Memory note: "Tests ship with fixes" — every VST3 process-path
  behaviour change needs a Catch2 fixture (the #290 coverage lane
  explicitly names `format` as under active hardening).
