---
name: auv2
description: Audio Unit v2 adapter work for Pulp — picking the right AU component type (aufx/aumf/aumi/aumu) and its matching entry macro, wiring MIDI input and output (including the aumi MIDI-processor adapter), sharing the base-class-free adapter surface, and avoiding the DAW-side component cache that silently masks repackaging.
requires:
  scripts: []
  tools: []
---

# AU v2 Skill

Use this when you are:

- Touching any AU v2 adapter — effect (`au_v2_adapter`), instrument (`au_v2_instrument`), MIDI processor (`au_v2_midi_processor`), or their shared surface (`au_v2_common`)
- Changing how Pulp packages AU v2 plug-ins in CMake (`tools/cmake/PulpUtils.cmake`, `tools/cmake/PulpInfoPlist.au.in`)
- Debugging "plug-in loads but never receives MIDI" reports against an AU host
- Picking or changing a plug-in's AU component type (4-char `type` in the Info.plist)
- Wiring new AU v2 features that require splitting base-class inheritance (adding `AUMIDIBase`, `MIDIOutput`, etc.)

Scope is AU v2 only. AU v3 (AUAudioUnit-based app extensions) has different rules and lives behind the `ios` skill + `core/format/src/au_adapter.mm`.

## AU v2 Component Types — Pick the Right 4cc

Hosts route MIDI based on the bundle's `type` field. Getting this wrong produces a silent-failure: the plug-in scans, loads, renders audio, and never sees a MIDI event.

| `type` | Constant                    | Audio I/O | MIDI I/O | When to use                                |
|--------|-----------------------------|-----------|----------|--------------------------------------------|
| `aufx` | `kAudioUnitType_Effect`       | in + out  | none     | Audio-only effect (compressor, EQ, reverb) |
| `aumf` | `kAudioUnitType_MusicEffect`  | in + out  | in       | Effect that wants inbound MIDI (arpeggiator on audio, MIDI-triggered gate, vocoder w/ MIDI carrier) |
| `aumi` | `kAudioUnitType_MIDIProcessor`| none      | in + out | MIDI-only processor (transpose, arp, chord, note filter) |
| `aumu` | `kAudioUnitType_MusicDevice`  | out only  | in       | Instrument / synth                         |

**Load-bearing rule:** Logic, MainStage, GarageBand, and other AU hosts will **never** call `MIDIEvent` / `HandleMIDIEvent` on an `aufx`-typed plug-in. If a plug-in's `Processor::descriptor()` sets `accepts_midi = true` and the bundle is still packaged as `aufx`, MIDI silently disappears — the adapter looks correct, the Info.plist looks correct to a casual reader, but no MIDI arrives.

Pulp's `pulp_add_plugin()` automates the choice from two inputs:

1. `CATEGORY` — `Effect` | `Instrument` | `MidiEffect`
2. `ACCEPTS_MIDI` — bool option that mirrors `PluginDescriptor::accepts_midi`

The resulting mapping is resolved centrally by
`tools/cmake/PulpPluginMetadata.cmake` and consumed by both `_pulp_add_au`
and `_pulp_add_auv3`:

```
(Instrument,  *)     -> aumu
(MidiEffect,  *)     -> aumi
(Effect,      true)  -> aumf    <-- easy to forget
(Effect,      false) -> aufx
```

When you add a new example or change an existing one's descriptor to declare `accepts_midi = true`, you **must** also add `ACCEPTS_MIDI` to its `pulp_add_plugin()` call. There is no runtime fallback — the two surfaces are independent and the CMake flag is what ends up in the Info.plist.

Do **not** pass `PRODUCES_MIDI` to `pulp_add_plugin()`. MIDI output is
declared by `PluginDescriptor::produces_midi` in processor code and consumed
by the format/runtime layer where supported; it is not a CMake packaging flag.
If a caller passes `PRODUCES_MIDI` anyway, CMake warns and ignores it so stale
docs/tests cannot imply a fake packaging effect.

The metadata resolver also owns AU/AUv3 four-character code validation and AU
integer version parsing. Keep it packaging-only: it must not absorb
`pulp_add_plugin()` content reload metadata or the DSP/UI hot-reload helpers
(`pulp_add_reload_logic`, `pulp_reload_host`, `pulp_reload_host_ui`).

## MIDI Input Wiring

### The entry FACTORY must dispatch MusicDevice selectors — not just the base class

Two independent things must both be true for an `aumf` to receive MIDI, and it is easy to get the first and miss the second:

1. The adapter **class** must implement the MIDI methods — `PulpAUEffect` derives `AUMIDIEffectBase` and overrides `HandleMIDIEvent` / `HandleSysEx`. ✅ (always true for Pulp effects)
2. The component **entry** must register through a factory whose *lookup table* carries the MusicDevice selectors. A factory only dispatches the selectors its lookup knows. `ausdk::AUBaseFactory` (`AUBaseLookup`) has **no** MusicDevice selectors, so even though the class implements `HandleMIDIEvent`, the host's `MusicDeviceMIDIEvent` call returns **-4 (unimpErr)** — no note ever reaches the adapter, and `auval -v aumf` fails with `-4 IN CALL MusicDeviceMIDIEvent` (often with a cascading `Bad Max Frames` line that clears once the MIDI dispatch is fixed).

So the entry macro is type-specific:

| Macro | Factory | Use for |
|-------|---------|---------|
| `PULP_AU_PLUGIN` (`au_v2_entry.hpp`) | `ausdk::AUBaseFactory` | `aufx` (audio-only effect) |
| `PULP_AU_MIDI_PLUGIN` (`au_v2_entry.hpp`) | `ausdk::AUMIDIEffectFactory` (`AUMIDILookup` = MIDIEvent + SysEx) | `aumf` (MIDI-receiving effect) |
| `PULP_AU_MIDI_EFFECT` (`au_v2_midi_effect_entry.hpp`) | `ausdk::AUMIDIEffectFactory` | `aumi` (MIDI processor) |
| `PULP_AU_INSTRUMENT` (`au_v2_instrument_entry.hpp`) | `ausdk::AUMusicDeviceFactory` (`AUMusicLookup` = + StartNote/StopNote) | `aumu` (instrument) |

**Three surfaces must agree** for an `aumf`, or the component is invalid: `descriptor().accepts_midi = true`, the `aumf` type (CMake `ACCEPTS_MIDI` or a hand-written `Info.plist.au`), **and** `PULP_AU_MIDI_PLUGIN` in the plugin's `au_v2_entry.cpp`. The dispatch contract is pinned by `test/test_au_v2_effect.cpp` (`[dispatch]` — asserts `AUMIDILookup` routes `kMusicDeviceMIDIEventSelect` and `AUBaseLookup` does not), so a regression to the base factory fails in CI instead of at auval/Logic time.

**The `aumu` instrument variant is the same trap** (seen with PulpTempoSampler): a `category = Instrument` plugin gets an `aumu` Info.plist from CMake, but if its `au_v2_entry.cpp` uses `PULP_AU_PLUGIN` (→ `AUBaseFactory`) instead of `PULP_AU_INSTRUMENT` (`#include <pulp/format/au_v2_instrument_entry.hpp>` → `AUMusicDeviceFactory`/`AUMusicLookup`), the host's `MusicDeviceMIDIEvent` returns **-4**. The plugin loads and plays UI-triggered audio (slice taps, on-screen keyboard — those bypass host MIDI via the UI→audio queue), so it looks fine, but **silently ignores all host MIDI in Logic/Ableton**. `auval -v aumu` catches it (`Test MIDI` fails); the same `[dispatch]` test now also pins `AUMusicLookup` carries the selector and `AUBaseLookup` does not. The factory name (`<ClassName>Factory`) is identical across both macros, so swapping `PULP_AU_PLUGIN` → `PULP_AU_INSTRUMENT` keeps the generated `Info.plist` factory reference valid.

### Flow

The adapter inherits `AUMIDIEffectBase` (`AUEffectBase` + `AUMIDIBase`) so the SDK's `MIDIEvent` / `SysEx` entry points exist. Inbound MIDI flows:

```
host -> AUMIDIBase::MIDIEvent(status, data1, data2, frame)
     -> AUMIDIBase::HandleMIDIEvent(strippedStatus, channel, data1, data2, frame)
     -> PulpAUEffect::HandleMIDIEvent(...)   <-- our override
        - decode_midi_event(...) -> MidiEvent
        - try_push onto the lock-free midi_in_queue_
```

At the top of `ProcessBufferLists()` we drain wait-free:

```
while (auto ev = midi_in_queue_.try_pop())   midi_in.add(*ev);
while (auto sx = sysex_in_queue_.try_pop())  midi_in.add_sysex_copy(...);
midi_in.sort()    // sample-accurate ordering
processor_->process(..., midi_in, midi_out, ctx)
```

The instrument adapter (`core/format/src/au_v2_instrument.cpp`) uses the same lock-free queue pattern against the `MusicDeviceBase` base class.

## The `aumi` MIDI processor (`PulpAUMidiProcessor`)

`core/format/src/au_v2_midi_processor.cpp` + `.hpp`, reached from a plugin via
`PULP_AU_MIDI_EFFECT` in `au_v2_midi_effect_entry.hpp`. Example:
`examples/pulp-transpose` (auval-validated `aumi`). Tests:
`test/test_au_v2_midi_processor.cpp` — a live adapter, host MIDI in through the
SDK entry points, a host-installed `kAudioUnitProperty_MIDIOutputCallback`, and
`Render()` driving the block.

The four decisions, each of which has a silent-failure mode if reversed:

- **Base is `MusicDeviceBase`, not `AUMIDIEffectBase`.** `AUMIDIEffectBase`
  derives `AUEffectBase`, which is `AUBase(ci, 1, 1)` and pulls input element 0
  every render — a MIDI processor has no audio input to pull. `MusicDeviceBase`
  is `AUBase + AUMIDIBase` with the `MIDIEvent`/`SysEx` forwarding and the
  MIDI-mapping property delegation already wired.
- **Element shape is 0 inputs / 1 output.** The output element carries no
  signal (the render path zeroes it and ORs in
  `kAudioUnitRenderAction_OutputIsSilence`) but it MUST exist: an AU v2 host
  advances a plugin by *rendering* it, and rendering is what drains the inbound
  MIDI queue, runs `process()`, and fires the MIDI-output callback. With zero
  output elements there is no bus to pull and the plugin never runs. `auval -v
  aumi` accepts this shape and reports `Input Scope Bus Count: 0`.
- **The main OUTPUT bus handed to `process(ProcessBuffers&)` is zero-channel but
  marked ACTIVE.** This is load-bearing. The default projection in
  `Processor::process(ProcessBuffers&)` does `if (!main_output()) return;`, and
  `main_output()` returns null for an inactive bus — so an inactive output bus
  means a MIDI effect written against the classic
  `process(out, in, midi_in, midi_out, ctx)` signature never runs at all, and
  the plugin passes nothing through with no error anywhere.
- **Override `HandleMIDIEvent`, not the per-message hooks.**
  `MusicDeviceBase::HandleNoteOn`/`HandleNoteOff` route notes into `StartNote`/
  `StopNote`, which an `aumi` does not implement — every note would be dropped.
  Intercepting at `HandleMIDIEvent` (as the effect adapter does) takes the whole
  stream before that dispatch.

Two behavioral differences from the effect adapter, both deliberate:

- **MIDI output is advertised unconditionally**, not gated on
  `descriptor().produces_midi`. An `aumi` that cannot emit MIDI has no reason to
  exist, and gating would turn a forgotten `produces_midi = true` into silently
  discarded output with no diagnostic. (The effect adapter still gates, so a
  plain `aufx` never advertises a MIDI output.)
- **Bypass is a WIRE, not a mute.** A declared Bypass parameter makes the
  adapter copy `midi_in` to `midi_out` untouched and skip `process()`. The
  audio-effect adapters do the opposite for MIDI (drop it), which is right when
  the plugin's MIDI is a *side product* — an arpeggiator on an audio track
  should not keep emitting while bypassed. For a MIDI processor the stream *is*
  the signal path, so dropping it would silence every instrument downstream of
  the bypassed slot. No bypass parameter is synthesized here:
  `kAudioUnitProperty_BypassEffect` lives on `AUEffectBase`, so on an `aumi` a
  synthesized control would be an ordinary parameter with no host bypass
  semantics behind it.

**`auval -v aumi` is a weak gate.** It stops after the parameter surface — no
render test, no MIDI test, no channel-config negotiation (all of which it runs
for `aufx`/`aumu`). A green `auval` on an `aumi` proves discovery, open,
initialize, scope formats, properties, and parameter persistence, and nothing
about MIDI actually flowing. In-DAW MIDI-FX behavior needs a host. Corollary:
an `aumi` built on the *effect* adapter (1 audio in / 1 audio out) also passes
`auval -v aumi` — the validator will not tell you the element shape is wrong.

## Shared adapter surface — `au_v2_common.hpp`

The three adapters derive three different SDK bases, so anything that does not
need a specific base lives in `core/format/include/pulp/format/au_v2_common.hpp`
(+ `src/au_v2_common.cpp`) and all three call it: the parameter surface
(`fill_parameter_list` / `fill_parameter_info` /
`fill_parameter_value_strings` / `parameter_string_from_value` /
`parameter_value_from_string`), the editor→host parameter bridge
(`wire_host_parameter_bridge` + the `ScopedHostParamWrite` echo guard), preset
state (`save_pulp_state` / `restore_pulp_state`), the MIDI-output callback
handoff (`MidiOutputCallbackPublisher`, `make_midi_output_names`), the Cocoa-view
hook, `decode_midi_event`, the render `ProcessContext` builders, and
`MidiOutputPacketBuilder`. Add a fourth adapter by calling these, not by copying
a third implementation.

**Do NOT introduce a `pulp::format::au::detail` namespace.** `pulp::format::detail`
already exists and carries `PlayheadSnapshot`, `au_output_offset`,
`audio_buffer_list_shape_matches`, and friends; a nested `au::detail` shadows it
for every unqualified `detail::` use inside `pulp::format::au`, and the failures
are confusing "no type named X in namespace pulp::format::au::detail" errors far
from the cause. The shared helpers sit directly in `pulp::format::au`.

### Decode helper: `decode_midi_event()`

`AUMIDIBase::HandleMIDIEvent` delivers the status byte already split into a top nibble and a separate channel. The free function `pulp::format::au::decode_midi_event(status, channel, data1, data2)` in `core/format/include/pulp/format/au_v2_adapter.hpp` recombines them into a `choc::midi::ShortMessage` with the correct on-the-wire status byte and returns a `MidiEvent` with `sample_offset == 0`. Tests cover CC, pitch bend, note-on, program change, and system messages (status 0xF0+ keep their literal byte — channel nibble is ignored).

### SysEx

`AUMIDIBase::HandleSysEx(data, length)` does not carry a per-event sample offset at this SDK layer. We enqueue the payload with `sample_offset == 0` so it is delivered at the leading edge of the current `ProcessBufferLists()` block.

## Multi-plugin bundles — one binary, many plugins (Silent Way style)

The single-plugin macros (`PULP_AU_PLUGIN` / `PULP_AU_MIDI_PLUGIN` / `PULP_AU_INSTRUMENT`) set one global `registered_factory()` slot, so one binary = one plugin. To host **many** plugins in ONE `.component` (like Expert Sleepers Silent Way — one bundle, N AudioComponents), use the bundle variants in the same headers:

| Macro | Type | Base factory |
|-------|------|--------------|
| `PULP_AU_BUNDLE_PLUGIN` | `aufx` | `AUBaseFactory` |
| `PULP_AU_BUNDLE_MIDI_PLUGIN` | `aumf` | `AUMIDIEffectFactory` |
| `PULP_AU_BUNDLE_INSTRUMENT` | `aumu` | `AUMusicDeviceFactory` |

Each expands to a distinct `ClassName : PulpAUEffect`/`PulpAUInstrument` bound to its OWN factory **lexically** — via the `(AudioComponentInstance, ProcessorFactory)` ctor added to both adapters — plus one `AUSDK_COMPONENT_ENTRY` (legal N-per-binary; each generates a distinct `ClassNameFactory` the Info.plist references). So instantiation never reads the global slot, and a bundle leaves `registered_factory()` null by construction (a stray global read then fails loudly instead of building an arbitrary plugin).

Key rules, each learned the hard way:

- **`factory_fn` is the single source of truth.** The macro force-assigns it onto the registration's `.factory`, so the factory the host constructs and the one `find_plugin`/`registered_plugins` report are provably the same function. **Do NOT set `.factory` in the braced-init** — it is overwritten and ignored. (An earlier design took factory twice; a mismatch silently split-brained the class vs the registry.)
- **The registration braced-init is trailing `__VA_ARGS__`,** because the preprocessor does not protect the `{.id=…, .au_subtype=…}` commas and an extra paren pair would form a GNU statement-expression. Pass `{.id="com.x.foo", .au_type='aumf', .au_subtype='Foo1', .au_manufacturer='Mfr '}` bare.
- **One plugin, several AU types:** register it once per type (a Silent Way module is `aumf`+`aumu`+`augn`), all sharing one `id`+factory, differing only in `au_type`/`au_subtype`. `find_plugin(id)` returns the first match; `registered_plugins()` enumerates all.
- **Registry storage is static-init-safe:** a fixed-capacity (`kMaxPlugins`) POD array, constant-initialized before any registrar runs (verified: `constinit` compiles), no heap/`std::string`/logging on the registration path. Past the cap, `register_plugin` drops silently — keep the cap generously above `plugins × types`.
- The keyed API lives in `core/format/include/pulp/format/registry.hpp`: `register_plugin(const PluginRegistration&)`, `find_plugin(id)`, `registered_plugins()`, plus `reset_registry_for_testing()` (test-only). The legacy `register_plugin(ProcessorFactory)` / `registered_factory()` global path is unchanged (last-write-wins), so single-plugin bundles and the adapter save/restore test idiom are untouched.

Tests: `test/test_plugin_registry.cpp` (portable registry contract) and `test/test_au_bundle_entry.cpp` (two `aumf` plugins in one binary via the macros; asserts the mismatched-`.factory` override).

## Recent changes

### Param-events sidecar + RT-safety guard

`ProcessBufferLists()` now `set_param_events(&param_events_)` before
`processor_->process(...)` and wraps ONLY the process call in
`pulp::runtime::ScopedNoAlloc` (the preamble — param snapshot, pointer-vector
resizes — legitimately allocates, so don't widen the guard). This makes the
param-events contract uniform across formats (VST3/CLAP/AUv3 already had it).
AU v2's `AUEffectBase` has no scheduled/ramped parameter event source, so
`param_events_` is **empty** — host params still reach the Processor through
`store_` (StateStore) exactly as before. A native-component (`NativeCoreProcessor`)
plugin therefore won't receive sample-accurate params on AU v2 yet; that needs
the AUv3 `AURenderEventParameter` model and is a follow-up. Do not synthesise an
AU v2 param-event mapping by guessing.

### ProcessBuffers dispatch

`ProcessBufferLists()` now wraps the current main input/output buffers in a
stack-owned `ProcessBuffers` block and calls the additive
`processor_->process(process_buffers, midi_in, midi_out, ctx)` overload inside
the existing `ScopedNoAlloc` guard. Legacy processors still reach the original
main-in/main-out callback through the default projection, while processors that
override the richer overload can inspect AU v2 bus metadata directly. The AU v2
instrument `Render()` path uses the same additive dispatch with an inactive,
optional main input bus and the active output bus.

### Latency / tail change notifications

A Processor flags a mid-render latency or tail change via
`flag_latency_changed()` / `flag_tail_changed()` (RT-safe atomic
store-release). Never call AU SDK property-change APIs from
`process()` directly — the adapter owns the host-thread publish path.

AU v2 wiring (post-process): the adapter checks the consume helpers
and calls `PropertyChanged(kAudioUnitProperty_Latency)` /
`PropertyChanged(kAudioUnitProperty_TailTime)`. The AU v2 SDK queues
these for delivery on the main thread, so it's safe to invoke from
the audio callback path. Tests live in
`pulp-test-processor-layout-latency` plus the existing
`pulp-test-au-v2-effect` suite.

### Bypass audio must be latency-compensated (PDC alignment)

The `ProcessBufferLists` bypass short-circuit must NOT `memcpy` the dry input
straight to the output. When the Processor reports a non-zero latency, the host
has delay-aligned the plugin's *wet* path by that latency (PDC), so a raw dry
copy arrives `latency` samples early — comb-filtering on parallel busses.
Route the bypass pass-through through `boundary::render_bypass_passthrough`
(`adapter_boundary.hpp`), sizing the member `bypass_` delay line to
`reported_latency_samples(processor_->latency_samples(), host_quirks_)` in
`Initialize()`. A zero latency collapses to a straight passthrough. The real
adapter is pinned by `pulp-test-au-v2-effect [bypass]` (drives
`ProcessBufferLists` with a latency-reporting, bypass-engaged processor and
asserts the impulse lands at frame `latency`, not frame 0).

### Channel-config negotiation (`kAudioUnitProperty_SupportedNumChannels`)

`PulpAUEffect::SupportedNumChannels()` reports the supported (input, output)
channel-count pairs so hosts and `auval` can negotiate a layout instead of
guessing. The base class returned 0 ("property unsupported"), leaving every
channel query unanswered. The table is derived from the descriptor by the pure
free function `build_channel_info(descriptor, out)` in the adapter header:

- Symmetric effect (declared in width == out width) → matched `in == out` pairs
  up to the declared width: `{1,1}` for mono, `{1,1}` + `{2,2}` for stereo.
- **Asymmetric** effect (in width != out width, e.g. a mono-in / stereo-out
  widener) → the single exact declared pair, e.g. `{1,2}`. Report what the
  descriptor actually declares — do NOT collapse an asymmetric plugin into a
  matched ladder, which would mis-report its capability to the host.
- Instrument / generator with **0 inputs** → `{0,1}` (mono synth) or `{0,1}` +
  `{0,2}` (stereo synth).

Widths are clamped into Pulp's supported `{1, 2}` flex range (consistent with
`validate_channel_layout`). The returned pointer must outlive the call (the host
reads it after return), so it points at a per-instance member array
(`channel_info_`) — never call-local or shared `static` storage. `build_channel_info`
is unit-tested over several descriptors in `test_au_v2_effect.cpp` (`[channels]`).

### MIDI output (`kAudioUnitProperty_MIDIOutputCallback`)

A Processor that declares `produces_midi = true` now delivers the MIDI it writes
to `midi_out` during `process()` back to the host on AU v2. The adapter:

1. Advertises the output stream via `kAudioUnitProperty_MIDIOutputCallbackInfo`
   (a one-element `CFArrayRef` named after the plugin) and accepts the host's
   callback via `SetProperty(kAudioUnitProperty_MIDIOutputCallback)`. Both
   properties are gated on `plugin_produces_midi()` so plain audio effects never
   advertise a MIDI output. The AudioUnitSDK itself implements **neither**
   property, so the adapter handles them directly in `GetPropertyInfo` /
   `GetProperty` / `SetProperty`.
2. In `ProcessBufferLists`, packs `midi_out` into a `MIDIPacketList` via the
   header-inlined `MidiOutputPacketBuilder` and calls the host callback with
   `CurrentRenderTime()` as the base timestamp.

**Callback-pair publishing (data-race fix).** The `(callback, userData)` pair is
written on the main thread (SetProperty) and read on the render thread. A plain
two-pointer struct is a data race that can pair a fresh callback with a stale
`userData`. The adapter publishes through a **double-buffered atomic snapshot**:
SetProperty writes the inactive of two slots then release-stores an
`std::atomic<const Pair*>` to it (flipping the write cursor); the render thread
does a single acquire-load. "AU serializes property writes vs render" is NOT
relied on. The torn-pair invariant is hammered by a two-thread test
(`[midi-out][realtime]`).

**Packet ordering + clamping.** CoreMIDI packet lists must be time-ordered, but
short events and SysEx live in separate `MidiBuffer` sidecars. `build()` merges
both into one ascending-`sample_offset` order (stable insertion sort over a
fixed-capacity index — no allocation) before appending, so a SysEx@0 is
delivered before a note@64. `build(midi_out, frame_count)` clamps every offset to
`[0, frame_count - 1]` via the shared `detail::au_output_offset()` helper
(mirrors the AU v3 input-side defensive clamp).

**Cross-format offset parity — one shared contract.** The invariant "an event
emitted at in-block offset N is delivered at offset N" must hold identically for
AU v2, VST3, and CLAP. The per-format mapping lives in ONE header,
`core/format/include/pulp/format/detail/midi_out_offset.hpp`
(`au_output_offset` / `vst3_output_offset` / `clap_output_offset`), and all three
adapters route through it — do NOT re-open-code the clamp in an adapter. Only the
out-of-range handling differs by host type (CLAP `time` is unsigned → clamp neg
to 0; AU `timeStamp` is in-block → clamp past-block to `frame-1`; VST3
`sampleOffset` is signed → pass through). Pinned by
`test/test_midi_out_offset_parity.cpp` (`[midi-out][parity]`), which exercises
the REAL AU builder plus the shared VST3/CLAP helpers.

**RT-safety + capacity.** The builder owns a fixed byte buffer sized to the
per-block event budget (`kMaxOutputEvents == kMaxEventsPerBlock`, ~16 B/event)
and uses `MIDIPacketListInit` / `MIDIPacketListAdd` (write into caller storage,
no allocation). Overflow stops appending and increments a `dropped` diagnostic
rather than growing. The callback invocation sits **outside** the `ScopedNoAlloc`
scope (it is host code), matching the AU v3 `MIDIOutputEventBlock` pattern in
`au_adapter.mm`. CoreMIDI is linked into `pulp-format` (PUBLIC) for the
packet-list builders. The instrument adapter (`PulpAUInstrument`) does not yet
deliver its local `midi_out` — only the effect path is wired, and it does NOT
half-advertise the property.

### Multi-bus output — instruments carry it, effects can't (SDK wall)

The genuine AU v2 multi-bus vehicle is the **instrument** (`aumu` /
`MusicDeviceBase`), NOT the effect.

- **Instrument (`PulpAUInstrument`)** advertises one AU **output element** per
  declared `descriptor().output_buses` entry. The element count must be known
  *before* the `MusicDeviceBase(ci, 0, N, 0)` base constructor runs (it stores
  the count and `CreateElements()` materialises exactly that many), so the ctor
  probes the registered factory once (`registry_output_element_count()`) — a
  throwaway processor purely to read the descriptor. `instrument_output_element_count(desc)`
  and `build_output_bus_infos(desc, …)` are pure/header-inline and unit-tested in
  `test_au_v2_busses.cpp` (`[bus]`).
- **The multi-output render idiom.** An AU host pulls each output element with its
  own `DoRenderBus`/`PrepareBuffer`, but a single `Render()` is expected to fill
  ALL output elements — `AUBase::RenderBus` gates the re-render for the remaining
  buses pulled at the same timestamp on `NeedsToRender`. So `PulpAUInstrument::Render`
  loops every element, calls `GetOutput(e)->PrepareBuffer(frames)`, **pre-zeroes**
  each bus (a processor that only implements the simple `process()` writes just
  the main bus — aux buses must read silence, not garbage), and hands all buses to
  `process(ProcessBuffers&)`. Bus 0 = Main, 1..N-1 = Aux. `PrepareBuffer` is
  idempotent and does not clobber contents, so the copy on each later bus pull
  reads what `Render` wrote. Verified by `auval -v aumu` on `examples/pulp-multi-out`.
- **Effect (`PulpAUEffect` / AUEffectBase) is a hard single-in/single-out wall.**
  `AUEffectBase` is `AUBase(ci, 1, 1)` and its `Render` pulls ONLY input element 0,
  so an AU effect cannot receive live sidechain/aux audio through the stock render
  path — the `std::array<…,1>` at the effect's `ProcessBufferLists` is NOT an
  arbitrary cap, it reflects the SDK model. A descriptor-declared sidechain
  surfaces as an **inactive** Sidechain bus (`build_input_bus_infos`), so
  `sidechain_input()` returns null gracefully rather than exposing a bus the host
  would try to feed. Gate any sidechain-view emission on `input_buses.size() > 1`
  so the common single-input effect stays byte-identical (auval `aufx` regression).
  Real AU-effect sidechain needs a 2nd input element + manual pull, which auval
  cannot exercise — do not add it without a real sidechain-capable DAW to verify.
- **RT-safety.** Cache the descriptor once (`descriptor_` member) in the ctor —
  `descriptor()` returns by value (allocating std::string members), so copying it
  per block on the audio thread is a bug. Bus `string_view`s point into the cached
  descriptor. Per-bus channel-pointer vectors are pre-reserved in `Initialize()`.

`kAudioUnitProperty_SupportedNumChannels` / `build_channel_info` stays MAIN-bus
only — that AU property describes (main-in, main-out) channel pairs; sidechain and
aux are separate elements, not `AUChannelInfo` rows. Do not try to fold multi-bus
into it.

## Current Gaps

### Explicit parameter kinds and declared layouts

`PluginDescriptor::supported_bus_layouts` is authoritative when non-empty:
`build_channel_info` emits the unique declared main-input/main-output pairs in
descriptor order instead of synthesizing the legacy mono/stereo ladder. Keep
sidechain/aux widths out of `AUChannelInfo`; those remain separate elements.

AUv2 display conversion routes through the shared canonical parameter-text
helpers. Explicit `Integer`, `Toggle`, and `Enum` kinds drive indexed/boolean
metadata; `value_labels` drive display and reverse parsing. Never add an
adapter-local numeric fallback for toggle/enum text or invoke author callbacks
directly—the shared helpers contain exceptions.

- **AU v3 parity** for MIDI on effects is not re-audited in this pass. If you touch `core/format/src/au_adapter.mm`, confirm the AUv3 `componentType` logic in `_pulp_add_auv3` still matches the fix in `_pulp_add_au`.

- **AU v2 instrument MIDI output** is still unwired: `PulpAUInstrument::Render` builds a local `midi_out` that is discarded. The `MidiOutputCallbackPublisher` + `MidiOutputPacketBuilder` in `au_v2_common.hpp` are base-class-free, so wiring it is now the same three calls the effect and MIDI-processor adapters make.

- **`aumi` MIDI 2.0 / UMP input is not wired.** `AUMIDILookup` does carry `kMusicDeviceMIDIEventListSelect`, and `AUMIDIBase::MIDIEventList` returns `kAudio_UnimplementedError` by default, so a UMP-capable host falls back to `MusicDeviceMIDIEvent` and MIDI 1.0 still flows. Implementing it means walking the `MIDIEventList` with `pulp::midi::walk_ump_packet` + `UmpSysex7Reassembler`, the way `au_adapter.mm` does for AU v3.

## Gotchas

### DAW component cache — clear it after a `type` change

Logic, MainStage, GarageBand, Studio One, Live, and every other AU host maintain a **host-side cache** of AU descriptors, keyed on subtype + manufacturer. When you change a plug-in from `aufx` to `aumf` (or vice versa) *without* also changing the subtype, hosts will keep the cached-old-type descriptor and behave as if the fix never shipped — you'll install a fresh `.component` and the host will still treat it as `aufx`. Symptoms: rebuilt plug-in appears in the correct MIDI-effect slot of the host UI only after a restart, or never appears at all.

Mitigation when you test a type change locally:

```bash
# Kill the AU registration cache so the next host launch re-inspects the bundle.
killall -9 AudioComponentRegistrar 2>/dev/null || true

# Logic / MainStage / GarageBand — clear the AU cache next to the host DBs.
rm -rf ~/Library/Caches/AudioUnitCache
rm -rf ~/Library/Caches/com.apple.audiounits.cache

# auval rescan catches the new type without needing a host restart.
auval -a | grep <subtype>
auval -v <type> <subtype> <manufacturer>
```

Document this step in any issue or PR that flips a shipped plug-in's component type.

**Beware the transient false PASS.** Right after `killall AudioComponentRegistrar`
the daemon is re-inspecting every component, and `auval` run during that window
returns *flickering* results — it can report `PASS` once, then `FAIL` (or the
"didn't find the component" error) on the next run, against the same bundle. A
type flip burned real time here: a mid-rescan `PASS` looked like the fix worked,
but the stable result was `FAIL`. Always **let the rescan settle (`sleep 4-5`)
and run `auval` at least twice**, and only trust a result that is stable across
runs. A single green run immediately after a cache kill is not a pass.

### `auval` tests on persistent runners — kill the cache before every run

Self-hosted CI runners (and local dev iteration where the same plug-in is
rebuilt repeatedly) hit the same `AudioComponentRegistrar` cache that
hosts use. Even with the `.auvaltest.component` rename trick (copy to a
suffixed path to avoid the canonical `.component` collision), the cache is
keyed by **bundle ID**, so a stale entry from the previously-installed
canonical bundle survives. `auval` then reports:

```
ERROR: Cannot get Component's Name strings
ERROR: Error from retrieving Component Version: -50
* * FAIL
FATAL ERROR: didn't find the component
```

even though the freshly-copied bundle is well-formed (`nm` shows the AU
factory symbol, `plutil -p Info.plist` is valid, `codesign -dv` succeeds).
On a fresh machine the test passes; on a persistent runner it
intermittently fails.

The fix is one line in the `auval` ctest command — kill the registrar
between install and validation:

```cmake
add_test(NAME auval-MyPlugin
    COMMAND bash -c "d=\"$HOME/Library/Audio/Plug-Ins/Components/MyPlugin.auvaltest.component\"; \\
                     rm -rf \"$d\"; \\
                     cp -R \"${CMAKE_BINARY_DIR}/AU/MyPlugin.component\" \"$d\" && { \\
                         killall -KILL AudioComponentRegistrar 2>/dev/null || true; \\
                         sleep 1; \\
                         auval -v aufx MyFx Pulp 2>&1 | tee /dev/stderr | grep -q 'PASS'; \\
                     }; \\
                     rc=\$?; rm -rf \"$d\"; exit \$rc")
```

`|| true` prevents `set -e` exit when no registrar is running; `sleep 1`
gives macOS time to relaunch the daemon before `auval` queries it. The
PulpEffect/PulpGain/PulpTone/PulpPluck examples all use this pattern.
ChainerSynth doesn't need it because its `aumu Chnr` codes are first-time
unique on the runner, but any new `aufx`/`aumu`/`aumf` plug-in sharing a
manufacturer+subtype pattern with an existing test should add the cache
kill.

Surface symptom matches the host-cache one above; the difference is
*you can't rely on `.auvaltest.component` alone* to defeat it.

### `AUEffectBase` vs `AUMIDIEffectBase`

If you see `HandleMIDIEvent` that never fires: check the base class. `AUEffectBase` alone has no `AUMIDIBase` mixin — the SDK only wires `MIDIEvent` dispatch when the class multiply inherits `AUMIDIBase` (directly or via `AUMIDIEffectBase` / `MusicDeviceBase`). When you add a new AU v2 adapter, inheriting from `AUMIDIEffectBase` is cheap even for audio-only effects — the class does nothing extra until the host actually delivers MIDI, and it future-proofs the adapter against a later `accepts_midi` flip.

### `GetProperty` / `GetPropertyInfo` chain

With `AUMIDIEffectBase`, fall-through calls should go to `AUMIDIEffectBase::GetProperty(...)`, not `AUEffectBase::GetProperty(...)`. `AUMIDIEffectBase::GetProperty` tries `AUEffectBase::GetProperty` first and then falls back to `AUMIDIBase::DelegateGetProperty`. Calling `AUEffectBase` directly skips the MIDI-mapping property delegation — hosts that query `kAudioUnitProperty_AllParameterMIDIMappings` would silently return no mapping.

### CXX_STANDARD on tests that include AU adapter headers

`core/format/include/pulp/format/au_v2_adapter.hpp` pulls `AudioUnitSDK/AUMIDIEffectBase.h`, which on AudioUnitSDK 1.4 uses `std::expected` (C++23). Apple clang only exposes `std::expected` when the *consuming TU* compiles at `-std=c++23`. Any test executable that includes the adapter header must set `CXX_STANDARD 23` explicitly — linking `pulp::format` is not enough because CMake treats `CMAKE_CXX_STANDARD=20` at the root as authoritative per target. See `core/format/CMakeLists.txt` for the equivalent pin.

### `pending_midi_` mutex is a slow-path correctness tool, not a fast path

The `std::mutex` guarding `pending_midi_` is contended only on the MIDI-delivery thread (where the host calls `HandleMIDIEvent`) and the audio thread (once per block, to drain). It is NOT the right primitive for per-event audio-thread publication. Do not extend this pattern to any new path that runs multiple times per block — switch to `choc::fifo::SingleReaderSingleWriterFIFO` if you need lock-free MIDI delivery inside a single block.

### `AUMIDIBase` splits the status byte for EVERY message

`AUMIDIBase::MIDIEvent` (AudioUnitSDK 1.4 `AUMIDIBase.h`) unconditionally splits the wire-format status byte before dispatching:

```
strippedStatus = inStatus & 0xF0   // -> HandleMIDIEvent's inStatus
channel        = inStatus & 0x0F   // -> HandleMIDIEvent's inChannel
```

The split happens for system messages (0xF0-0xFF) the same way as for channel-voice (0x80-0xEF). For 0xF8 (timing clock) the SDK calls `HandleMIDIEvent(inStatus=0xF0, inChannel=0x08, ...)`. The decoder MUST reassemble `(inStatus & 0xF0) | (inChannel & 0x0F)` regardless of the top nibble — special-casing system messages and returning `inStatus` unchanged turns every clock / start / stop / song-position into 0xF0 (sysex start). The unit test in `test/test_au_v2_effect.cpp` now feeds the post-split shape (status=0xF0, channel=0x08) so the regression cannot reappear without flipping a test red.

### `AUSDK_RTSAFE` position with `override` — Xcode 16.4 incompat

`AUSDK_RTSAFE` expands to `[[clang::nonblocking]]`. AudioUnitSDK's own base-class declarations use `... AUSDK_RTSAFE;` (no `override`), but placing the attribute between a function declarator and the `override` virt-specifier in a derived class compiles under older Xcode and fails on Xcode 16.4 / Clang 17+ with:

```
error: expected ';' at end of declaration list
```

The attribute is a static-analysis hint only — dropping it from derived-class `override` declarations has no runtime effect. `PulpAUInstrument::HandleNoteOn/Off` (the reference pattern for AU v2) doesn't carry `AUSDK_RTSAFE` either. When writing a new AU v2 override that matches an `AUSDK_RTSAFE` base declaration, omit the attribute. This incompatibility surfaces on CI's Coverage-macOS leg.

### Editor `dealloc` ordering — never call `bridge->close()` explicitly

`PulpAUEditorOwnership` (in `core/format/src/au_v2_cocoa_view.mm`) declares its members as `unique_ptr<ViewBridge> bridge` then `unique_ptr<PluginViewHost> host`. C++ destroys members in REVERSE declaration order, so when `delete _ownership` runs in `PulpAUEditorOwner::dealloc`:

1. `~PluginViewHost` runs first. The host calls `root_.set_plugin_view_host(nullptr)` to clear the View → host back-pointer. The View it references is still alive (still owned by `bridge->view_`), so the call is safe.
2. `~ViewBridge` runs second. Its destructor calls `close()` → `Processor::on_view_closed(*view_raw_)` fires → `view_.reset()` destroys the View. The back-pointer was already cleared in step 1, so the View's own teardown can't reach a dead host.

Calling `_ownership->bridge->close()` HERE explicitly (BEFORE `delete _ownership`) reverses that order: the View dies first, then `~PluginViewHost` dereferences a dangling `root_` reference and crashes the AU v2 editor close path. The fix is to remove the explicit close, NOT to add it. Same rule applies to any future Cocoa-View ownership wrapper that mixes a `ViewBridge` and a `PluginViewHost` in the same C++ scope.

### Editor GPU host is auto-selected — don't hardcode `use_gpu`

`au_v2_cocoa_view.mm` no longer sets `Options::use_gpu` by hand; it calls
`pulp::format::decide_gpu_host(*bridge)` so a Skia/Dawn/scripted editor gets the
GPU `PluginViewHost` automatically (hardcoding `use_gpu=false` was the bug that
made it fall back to AutoUi/CPU). It also wires `host->set_resize_callback(...)`
because AU v2 has **no host size callback** — the DAW resizes the returned
NSView directly, so native frame changes are forwarded to `bridge->resize()`
through that seam. Full contract: the `view-bridge` skill's "GPU view host
auto-selection" section.

### The Cocoa view MUST pin the design viewport, or a Logic resize CLIPS

AU v2 has no size negotiation (no `checkSizeConstraint` / `gui_adjust_size`) —
Logic resizes the returned NSView directly. Without a design-viewport pin the
fixed-size tree CLIPS instead of scaling; VST3 and mac AUv3 always pinned, AU v2
historically did not. `au_v2_cocoa_view.mm` now routes the decision through the
shared `pulp::format::should_pin_design_viewport(ViewSize)` predicate
(`plugin_descriptor.hpp`, same one `PulpPlugView` uses): pin viewport + lock
aspect + `set_design_viewport_top_align(true)` (mac-AUv3 parity — slack collects
as one bottom strip) unless the plugin opted into free drag
(`min>0 && aspect_ratio==0`), which stays unpinned so Yoga reflows via the
resize-callback seam above. Contract test: `test/test_au_v2_cocoa_ui.mm`
`[resize]`. The windowed `set_design_viewport` call itself cannot run headlessly
(`editor_launch_blocked_by_environment` refuses editors in CI) — verify visually
in Logic/auval when touching this path.

### AU v2 MUST advertise its Cocoa view, or the host shows its generic UI

Selecting the GPU host (above) is necessary but NOT sufficient. The host only
loads the Pulp editor if the AU advertises `kAudioUnitProperty_CocoaUI`. For a
long time NO Pulp AU v2 did — `fill_cocoa_view_info()` existed but was never
wired into `GetProperty`, so Logic/auval saw `Cocoa Views Available: 0` and fell
back to a generic param view (the symptom: a plain "Level" slider instead of the
real editor). Both `PulpAUEffect` and `PulpAUInstrument` now serve
`kAudioUnitProperty_CocoaUI` in `GetProperty`/`GetPropertyInfo`. Watch-outs:

- The adapters compile into the shared `pulp-format` lib **without** `PULP_AU_GUI`,
  while the Cocoa view module (`au_v2_cocoa_view.mm`) is added per-`*_AU` target
  **with** it. So an `#ifdef PULP_AU_GUI` in the adapter is always off. The view
  is reached via a runtime hook `g_cocoa_view_info_filler` (hidden visibility,
  defined in `au_v2_adapter.cpp`) that the view module's static-init registers.
  Query it ungated; null → delegate to base (no view).
- The **instrument** (`PulpAUInstrument`, `MusicDeviceBase`) must ALSO serve
  `kPulpEditorContextProperty` — the Cocoa view factory reads it to reach the
  Processor + StateStore. It originally overrode no `GetProperty` at all.
- **`CFBundleCopyBundleURL` PAC-crashes** (`__CFCheckCFInfoPACSignature`,
  PAC_EXCEPTION/SIGKILL) inside pointer-auth-hardened sandboxed hosts (Logic's
  `AUHostingServiceXPC`, auval) the instant the view is queried — a hardware trap
  a `@try` cannot catch. Use `-[NSBundle bundleURL]` instead. This was the actual
  reason the editor never loaded even in code paths that tried.
- The factory ObjC class name MUST be **per-plugin-unique** (`PULP_AU_COCOA_VIEW_CLASS`,
  injected per `*_AU` target from MFR+CODE 4ccs). ObjC class names are
  process-global; two Pulp AUs in one host would collide on a fixed name.
- Validate with `auval -v` → expect `Cocoa Views Available: 1`. Covered by
  `test/test_au_v2_cocoa_ui.mm`.

### `auval` automation must disable editor creation

`auval` can instantiate AU editor surfaces during validation, which is
not acceptable in CI, headless tests, or local agent runs. CTest/CLI
validator paths must carry
`PULP_DISABLE_PLUGIN_EDITOR=1 PULP_HEADLESS=1 PULP_TEST_MODE=1`; the AU
Cocoa view factory returns `nil` under those guards. Keep this
environment on every `auval-*` test even if the validator command itself
looks audio-only.

## Parameters are single-source-of-truth — never reconcile two stores

The adapter overrides `GetParameter`/`SetParameter` to read/write the plugin's
`StateStore` directly. The host's parameter value IS the store value — there is
NO separate `Globals()`/AUElement copy to reconcile each block. Do not
reintroduce a per-block `GetParameter()→store` pull: it reverts UI-thread edits
(XY snap-back, type-in not taking) on the very next block, because the editor
writes the store but not the host cache. The render thread must perform NO
host-parameter write or notification — `AUEventListenerNotify` /
`AUBase::SetParameter` / `Globals()->SetParameter` from `ProcessBufferLists`
reentrantly stalls Logic's render thread and silences audio. UI edits reach the
host via the gesture begin/end brackets (`set_gesture_callbacks`, UI thread) and
an Audio-thread store listener that notifies on the editing thread with a
`thread_local` echo guard so a host-originated `SetParameter` is not echoed back.

**The instrument adapter (`au_v2_instrument.cpp`) now wires the same
`set_gesture_callbacks` block** (it previously only had the value-change
listener, so slider drags in an instrument editor recorded values but
never bracketed them with `kAudioUnitEvent_BeginParameterChangeGesture` /
`…EndParameterChangeGesture` — Logic would not arm a write pass). Mirror
`au_v2_adapter.cpp`'s gesture block exactly: emit Begin on
`begin_gesture`, End on `end_gesture`, both via `AUEventListenerNotify`
with the `g_host_writing_param` echo guard.

## MIDI input is lock-free — no audio-thread mutex

`HandleMIDIEvent`/`HandleSysEx` push to lock-free `SpscQueue<MidiEvent>` +
bounded `SysexChunk` queues; `ProcessBufferLists` drains them wait-free. Don't
add a `std::mutex` to the MIDI path — short messages stay allocation-free and
the audio thread never blocks. The AU v2 *instrument* adapter
(`au_v2_instrument.cpp`) uses the same single-source params + `SpscQueue`
note-input pattern (`HandleNoteOn`/`HandleNoteOff` → lock-free queue).

## Reference pointers

- Effect adapter: `core/format/src/au_v2_adapter.cpp`, `core/format/include/pulp/format/au_v2_adapter.hpp`
- Instrument adapter (reference pattern): `core/format/src/au_v2_instrument.cpp`, `core/format/include/pulp/format/au_v2_instrument.hpp`
- MIDI-processor adapter (`aumi`): `core/format/src/au_v2_midi_processor.cpp`, `core/format/include/pulp/format/au_v2_midi_processor.hpp`
- Shared, base-class-free surface: `core/format/src/au_v2_common.cpp`, `core/format/include/pulp/format/au_v2_common.hpp`
- Cocoa view factory: `core/format/src/au_v2_cocoa_view.mm` (owned by `view-bridge` + `ios` skills)
- CMake selector: `tools/cmake/PulpUtils.cmake` — `_pulp_add_au` and `_pulp_add_auv3`
- Info.plist template: `tools/cmake/PulpInfoPlist.au.in`
- AudioUnitSDK reference: `external/AudioUnitSDK/include/AudioUnitSDK/AUMIDIBase.h`, `AUMIDIEffectBase.h`
- Support matrix entry: `docs/status/support-matrix.yaml` — `formats.au_v2` and `format_limitations.au_v2`
- Tests:
    - `test/test_au_v2_effect.cpp` — decode / sysex smoke
    - `test/test_au_v2_midi_processor.cpp` — live `aumi` adapter, MIDI in -> MIDI out
    - `test/cmake/test_au_v2_type_selection.cmake` — aumf/aufx/aumu/aumi mapping

### AU v2 Cocoa view hands GpuSurface to ScriptedUiSession

`au_v2_cocoa_view.mm` now calls
`bridge->scripted_ui()->attach_gpu_surface(host->gpu_surface())` right
after `PluginViewHost::create()` succeeds. Skip this and an AU v2
plugin whose UI uses Three.js or raw WebGPU JS renders black — the JS
shim silently falls back to mocks. See the `view-bridge` skill's
"GpuSurface plumbing into WidgetBridge" section for the cross-platform
contract.

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

This adapter synthesizes the host-quirks bypass parameter and short-circuits the
process path when bypass is active.
At init (clap_init / PulpAUEffect ctor) it calls
`pulp::format::maybe_synthesize_bypass(store, host_quirks)` then detects the
bypass param via the shared `pulp::state::is_bypass_param` contract into a
cached `bypass_param_id_`. **Param designation:** a Processor can declare
`ParamInfo::designation = ParamDesignation::Bypass` to mark the bypass control
*regardless of its name*; the legacy boolean-range heuristic (name=="Bypass",
step>=1, 0..1) remains the fallback for params that declare none, so existing
plugins are unchanged. `maybe_synthesize_bypass` uses the same contract, so a
declared-bypass param also suppresses synthesis. **Trigger params:** the
adapter calls `store_.reset_triggers_rt()` to auto-reset trigger /
momentary params (`ParamInfo::is_trigger`, or a `ParamDesignation::Reset`
"reset/panic" control) back to their default as a **single-exit
invariant** — both after `processor_->process` on the normal path AND
before the bypass short-circuit's `return noErr`, so a panic/reset raised
while bypassed clears this block instead of the next active one. AU v2
`GetParameter`/`SetParameter` read/write the store directly (single source
of truth), so the host sees the settled value on its next read — there is
no separate cached AU value to go stale. In the audio callback
(clap_process /
ProcessBufferLists) it short-circuits to a **null-guarded pass-through**
(copy main input → output, zero any output channel without a matching input)
and skips the Processor when the param value is >= 0.5 — mirroring the VST3
processBlockBypassed path. `PULP_HOST_QUIRKS=off` synthesizes nothing
(bypass_param_id stays 0). The pass-through MUST null-check each destination
channel pointer (a bus can report channels with null buffers).

## Instrument latency & bypass MIDI drain

- **Instrument latency:** `PulpAUInstrument::GetLatency()` now routes
  the processor's latency through `reported_latency_samples()` (clamped,
  policy-gated) instead of hardcoding 0.0 — instruments with lookahead get
  host PDC. MusicDeviceBase has no `GetSampleRate()`; read it from
  `GetOutput(0)->GetStreamFormat().mSampleRate` (guarded for pre-config).
- **Bypass MIDI drain:** the `ProcessBufferLists` bypass
  short-circuit now drains + DISCARDS `pending_midi_` under `midi_mutex_`
  before returning. Without it, MIDI received while bypassed accumulated and
  flooded the processor with stale notes/CCs the instant bypass turned off.
  A bypassed plugin is a wire — inbound MIDI is dropped with the block.

## Driving PulpAUInstrument::Render in a headless RT test

`test/test_au_v2_instrument_rt.mm` proves the instrument render path is
allocation/lock-free (`pulp::test::ScopedRtProcessProbe`, trap build). Gotcha: a
directly-constructed `AUBase` never runs the SDK dispatch's
`PostConstructorInternal()`, so a test must, in order: `CreateElements()`, set the
output element's `SetStreamFormat` + `MaximumFramesPerSlice`, then **`DoInitialize()`
— NOT the bare virtual `Initialize()`** (`DoInitialize` is what allocates the IO
buffer and flips the initialized flag). The FIRST `Render` is warm-up (one-time
IO-buffer alloc) and must run OUTSIDE the probe; measure a steady-state block. The
`ScopedNoAlloc` around the instrument `process()` is a no-op in Release (NDEBUG) —
it only traps in the test/sanitizer build, same as every other placement.

## OutputIsSilence must be cleared — overriding ProcessBufferLists bypasses the SDK's clear

`PulpAUEffect` overrides `ProcessBufferLists`, and that override is the *only*
place the silence bit can be retracted.

`AUEffectBase::Render` hands `ioActionFlags` to `AUInputElement::PullInput`, so a
host that renders silence upstream ORs `kAudioUnitRenderAction_OutputIsSilence`
into the flags before our override ever runs. The stock
`AUEffectBase::ProcessBufferLists` clears the bit again once a kernel writes
output — but Pulp never calls it. Left uncleared, the adapter hands the host a
full buffer labelled silent, and a host that honours the label substitutes
digital silence. That deletes the output of every plugin that synthesizes signal
from a silent input: generators, oscillators, reverb tails, DC / control-voltage
sources.

The failure is invisible from inside. `PulpAUEffect` passes
`inProcessesInPlace = true`, so `AUEffectBase::Render`'s
`if (silence && !ProcessesInPlace()) ZeroBuffer(output)` never fires — the buffer
really does hold the right samples. Only the flag is wrong, and only the host
acts on it. **A `Processor`-level "write 0.5, read 0.5" test passes while the bug
is live.** Test at the adapter boundary or you are testing nothing.

Rules:
- Clear the bit after a non-bypassed `process()`:
  `ioActionFlags &= ~kAudioUnitRenderAction_OutputIsSilence;`
- Clear it *unconditionally* on that path. The adapter cannot know whether this
  Processor generated output, and the cost of over-clearing is a lost host-side
  silence optimisation; the cost of under-clearing is deleted audio.
- Retract exactly that one bit — other flags the host set (`kPreRender`, …) must
  survive.
- **Leave the bypass path alone.** There the plugin really is a wire, so upstream
  silence is still silence downstream.
- Re-run `auval` after touching this: it has silence/tail contract tests.

`test_au_v2_effect.cpp`'s `[silence]` case pins the contract.

## Driving PulpAUEffect::ProcessBufferLists in a headless test

Same shape as the `PulpAUInstrument::Render` recipe below, plus an input element:

```cpp
ScopedFactoryRegistration reg(create_my_processor);   // swap the global factory
pulp::format::au::PulpAUEffect effect(nullptr);       // no AudioComponentInstance
effect.CreateElements();                              // dispatch normally does this
effect.GetInput(0)->SetStreamFormat(fmt);
effect.GetOutput(0)->SetStreamFormat(fmt);
effect.DispatchSetProperty(kAudioUnitProperty_MaximumFramesPerSlice, ...);
effect.DoInitialize();                                // NOT the bare Initialize()
AudioUnitRenderActionFlags flags = kAudioUnitRenderAction_OutputIsSilence;
effect.ProcessBufferLists(flags, in_bl, out_bl, frames);   // public; skips PullInput
effect.DoCleanup();
```

Calling `ProcessBufferLists` directly (rather than `Render`) sidesteps `PullInput`
and lets the test *inject* the host's silence claim, which is the whole point. A
two-channel `AudioBufferList` needs the trailing-storage idiom
(`struct { AudioBufferList bl; AudioBuffer second; }`) — assert the layout with a
`static_assert` on `offsetof`.

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

## Continuous-parameter value strings (DISPLAY region)

`GetParameterValueStrings` only serves DISCRETE params (an enumerated list;
Pulp gates it on `range.step >= 1`). A CONTINUOUS param with a custom
`ParamInfo::to_string` reaches the host through a different door:

1. Set `kAudioUnitParameterFlag_ValuesHaveStrings` in `GetParameterInfo` when
   the param declares a `to_string` (both discrete and continuous). Without the
   flag the host never asks for strings.
2. AUBase does NOT implement `kAudioUnitProperty_ParameterStringFromValue` /
   `...ValueFromString` — handle them in `GetPropertyInfo`/`GetProperty`. The
   host passes the target `ParamID` inside the in/out struct (not `inElement`),
   so advertise at global scope and validate the specific param in GetProperty.
   For StringFromValue the host owns/releases `outString` (create with +1
   retain); `inValue == nullptr` means "use the current value".
   Guard from_string with `std::isfinite`. Test:
   `test/test_au_v2_param_display.mm`.

## Verifying AU v2 tests locally needs the AudioUnitSDK

The AU v2 test targets (`pulp-test-au-v2-*`) link `ausdk` and only get
configured when `external/AudioUnitSDK` is present — CMake prints
`AudioUnitSDK found — AU v2 support enabled`. A fresh worktree does NOT have it
(the SDK is developer-supplied, not committed), so the AU targets silently
don't exist and `cmake --build --target pulp-test-au-v2-effect` fails with
`No rule to make target`. Before verifying any AU change:

```bash
git clone --depth 1 https://github.com/apple/AudioUnitSDK.git external/AudioUnitSDK
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DPULP_ENABLE_GPU=OFF   # reconfigure to pick it up
```

## GetParameterInfo metadata + latency/tail change notification

Two host-conformance surfaces beyond the display-string path above:

- `GetParameterInfo` maps `range.min/max/default_value` to
  `minValue/maxValue/defaultValue` and the unit string to an
  `AudioUnitParameterUnit` (`"Hz"`→Hertz, `"dB"`→Decibels, `"%"`→Percent,
  Boolean, else Generic). A wrong range/unit silently mis-scales every host
  automation lane — assert the numeric metadata, not just the
  ValuesHaveStrings flag. Test: `test/test_au_v2_param_display.mm`.
- When the processor flags a latency/tail change during `process()`,
  `ProcessBufferLists` republishes it via
  `PropertyChanged(kAudioUnitProperty_Latency / kAudioUnitProperty_TailTime)`
  so the host re-reads plugin-delay compensation. To test the delivery,
  subclass `PulpAUEffect` and override `PropertyChanged` to capture the
  property IDs, then drive a real `ProcessBufferLists` render. Test:
  `test/test_au_v2_effect.cpp`.
