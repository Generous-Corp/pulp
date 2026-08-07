---
name: mpe
description: Build an MPE-aware Pulp synth — opt into MPE via PluginDescriptor, consume per-note pitch bend / pressure / timbre from MpeBuffer, and route voices through MpeVoiceAllocator without reinventing channel tracking.
---

# MPE

Use this skill when adding per-note expression (pitch bend, pressure,
CC 74 timbre) to a Pulp synth, or when writing a host that needs to
dispatch MPE data into a plugin. Pulp keeps MPE as an opt-in sidecar
to the normal MIDI path — plugins that don't set `supports_mpe` never
see the extra buffer.

## When to reach for MPE

- The synth is meant for Roli Seaboard / LinnStrument / Sensel Morph /
  KMI / similar per-note controllers.
- You need polyphonic per-note pitch bend (not just a global bend).
- You want pressure or CC 74 to modulate each voice independently.

If you only need monophonic aftertouch or a global mod wheel, plain
`MidiBuffer` in `process()` is simpler — do not reach for MPE.

## Decision tree

| You're writing... | Use |
|--|--|
| An MPE synth voice | Subclass `midi::MpeSynthVoice`, render your oscillator using `state().pitch_bend_semitones`, `state().pressure`, `state().timbre` |
| An MPE synth plugin | `MpeVoiceAllocator<YourVoice>` inside the processor, dispatch `MpeBuffer` in `process()`, set `supports_mpe = true` or `node_capabilities.supports_mpe = true` in the descriptor |
| A host that loads MPE plugins | Build an `MpeBuffer` from inbound MIDI (zone-aware) and hand it to `Processor::mpe_input()` — the CLAP, VST3, and AUv3 adapters already do this |
| Pure MIDI 2.0 UMP work | Out of scope — direct UMP-native MPE transport is deferred |

## The three-step pattern for a new MPE synth

### 1. Scaffold with `--mpe`

```bash
./build/pulp create MySynth --type instrument --mpe
```

The CLI post-processes the generated descriptor to add
`.supports_mpe = true` or `.node_capabilities.supports_mpe = true` and includes `<pulp/midi/mpe_buffer.hpp>`. No
manual wiring required.

### 2. Declare the voice

```cpp
class Voice : public pulp::midi::MpeSynthVoice {
public:
    void on_note_on(const pulp::midi::MpeNoteState& n) override {
        pulp::midi::MpeSynthVoice::on_note_on(n);  // keep base bookkeeping
        // your per-voice init
    }
    void render(float* out, int n) override {
        const auto& s = state();       // read the tracked expressions
        // s.pitch_bend_semitones, s.pressure (0..1), s.timbre (0..1)
    }
};
```

**Always call the base `on_note_on` / `on_note_off`** — the base class
maintains the smoothing state and glide refcount. Forgetting it leaves
`last_was_glide` / timbre smoothing in an inconsistent state and voice
stealing will mis-decrement the glide counter.

### 3. Dispatch from `process()`

```cpp
pulp::midi::MpeVoiceAllocator<Voice> allocator_{8};  // 8-voice polyphony

void process(pulp::audio::BufferView<float>& out,
             const pulp::audio::BufferView<const float>& /*in*/,
             pulp::midi::MidiBuffer& /*midi_in*/,
             pulp::midi::MidiBuffer& /*midi_out*/,
             const pulp::format::ProcessContext& ctx) override {
    if (auto* mpe = mpe_input()) {                       // nullptr unless
                                                         // supports_mpe=true
        for (const auto& e : mpe->events()) {
            allocator_.dispatch(e);                      // one event at a time
        }
    }
    for (std::size_t i = 0; i < allocator_.polyphony(); ++i) {
        auto& v = allocator_.voice(i);
        if (v.active()) v.render(out.channel(0), ctx.num_samples);
    }
}
```

`MpeVoiceAllocator::dispatch(const MpeExpressionEvent&)` takes a single
event at a time — iterate over `mpe_input()->events()` (the per-note
`MpeBuffer` the host/format adapter populates when the processor sets
`PluginDescriptor::supports_mpe = true`). The allocator handles note-on
allocation (oldest-steal when full), routes per-note expression updates
to the right voice, and runs note-off logic including the glide
refcount. Do not call `on_note_on` / `on_note_off` directly.

Voices are accessed by index via `allocator_.voice(i)` with
`allocator_.polyphony()` giving the count — there's no `voices()`
iterator.

## Gotchas

### Zones are configured, not auto-discovered

`MpeVoiceTracker::process()` handles note on/off, pitch bend, channel
pressure, and CC 74 — it does **not** parse RPN 6 / 7 (MPE Configuration
Messages). Which channels belong to the lower zone (master ch 1,
members 2–N) vs the upper zone (master ch 16, members N–15) is decided
by the `MpeConfig` you pass to the tracker at construction; you're
responsible for supplying it (usually from the plugin's own
configuration / saved state), not for trusting the controller to
negotiate it.

If you need live RPN 6/7 negotiation, parse it separately (see
`core/midi/include/pulp/midi/rpn_parser.hpp`) and reconfigure the
tracker off the audio thread.

### Pressure ≠ velocity

Pressure is continuous and per-note; velocity is the note-on value and
does not change. Use `state().pressure` (smoothed, 0..1) for amplitude
modulation, not `velocity()`.

### Pitch bend range defaults to ±48 semitones

That's the MPE spec default. If your controller sends a different range
via RPN 0, `MpeVoiceTracker` honors it — but a lot of older controllers
don't send the RPN. When testing, either send the RPN or document the
assumption.

### Glide detection is refcounted

`MpeGlideDetector` tracks overlapping note-ons on the same channel
(the MPE signal for glide/legato). `MpeVoiceAllocator::last_was_glide()`
reflects that state. If you hand-roll voice allocation, you are
responsible for incrementing on note-on and decrementing on note-off,
**including the steal path** — see the test "MpeVoiceAllocator steal
path decrements glide refcount" for the invariant.

### UMP per-note management + assignable PNC

`MpeVoiceTracker` consumes the full MIDI 2.0 per-note expression
surface:

- **Status 0xF0 — Per-Note Management**: `kPerNoteResetControllers`
  bit returns per-note expression (pitch bend / pressure / timbre) to
  spec defaults (0); `kPerNoteDetachControllers` bit sets
  `MpeNoteState::detached`, after which channel-level controllers
  (status 0xE0 / 0xD0 / 0xB0) skip that note. Per-note targeted
  messages (0x60 per-note pitch bend, 0x00 registered PNC, 0x10
  assignable PNC) still apply to detached notes.
- **Status 0x10 — Assignable Per-Note CC**: the index is host-defined
  per the UMP spec, so the tracker only routes when the plugin binds
  one via `set_assignable_timbre_index(uint8_t)`. Unbound by default —
  unbound assignable PNC is silently ignored. Registered PNC 74
  (status 0x00) still routes to timbre regardless.
- **Retrigger semantics**: a note-on while the slot is still active
  clears `detached` (re-attaches the slot to channel-level controllers).
- **D+S flag combination**: when detach and reset bits arrive in the
  same management packet, detach takes effect on the currently
  sounding note (state preserved for its lifecycle); reset is *armed*
  for the next note-on at the same (channel, note) index. Pulp does
  not yet maintain the armed-reset memory — D+S currently degrades to
  detach-only on the live note, which matches the spec for the sounding note.
  The armed-future-reset behavior is deferred follow-up work; if you need the
  full D+S note-rotation flow, file an issue with a controller reproducer.

If you're routing UMP into the tracker, use the factories on
`UmpPacket`: `per_note_management(group, channel, note, flags)`,
`assignable_per_note_cc(...)`, `registered_per_note_cc(...)`,
`per_note_pitch_bend(...)`. Channel-level cache stays updated even
for detached notes so freshly-added notes on the same channel still
inherit running state via `add_note`.

### Format adapter coverage

The CLAP, VST3, and AUv3 adapters populate `MpeBuffer` from inbound MIDI and
reset their tracker state at lifecycle boundaries. AUv2 and other adapters still
forward plain MIDI only; an MPE synth loaded through one of those formats sees
MIDI events but the `MpeBuffer` will be empty unless the processor derives
per-note state from `MidiBuffer` itself.

### Realtime sidecar buffers are capacity-limited

`MpeBuffer` and `UmpBuffer` support the same adapter-owned realtime
capacity policy as `MidiBuffer`: reserve storage before the audio
thread, call `set_realtime_capacity_limit(true)`, and treat `add()`
returning `false` plus `dropped_event_count()` as the overflow signal.

This matters for CLAP because one short MIDI event can fan out to many
MPE sidecar callbacks, and native `CLAP_EVENT_MIDI2` packets append
directly to the UMP sidecar before `Processor::process()`. The CLAP
adapter reserves both sidecars in `clap_activate()` and drops rather
than growing vectors during `clap_process()`. If you add a new adapter
or widen the sidecar contract, test the overflow path without copying
large event vectors inside the processor no-allocation guard.

`bind_tracker_to_buffer()` treats a same-note retrigger as one atomic pair:
`NoteOff(old generation)` then `NoteOn(new generation)` at the same sample
offset. One remaining realtime slot is not enough, so neither half is emitted
and the tracker does not rotate the generation. A physical note-off is
different because the controller will not replay it: when the buffer is full,
the tracker moves that release into its fixed FIFO and blocks fresh starts until
it is drained. `MpeSidecar` drains it automatically at offset zero before the
next block. A direct tracker/buffer integration must do the same by calling
`flush_pending_note_offs()` after clearing its output and before ingesting the
next block. Retrigger is a reattack, not glide: because the old generation's
NoteOff is dispatched first, `MpeVoiceAllocator::last_was_glide()` remains false.
Only genuinely overlapping held notes on a member channel count as glide.

The generic sidecar cannot own a processor-specific allocator. At deactivation,
adapters call `Processor::release()` before resetting the tracker, so an MPE
processor must call `MpeVoiceAllocator::reset_all()` from `release()`. For an
in-place host reset, adapters clear the tracker and raise
`ProcessContext::reset_requested` on the next block; clear the allocator before
dispatching that block's MPE input. Do not use `should_reset_dsp_state()` for
voice ownership: it also includes transport jumps, which do not reset the
adapter tracker. `MpeVoiceTracker::reset()` deliberately does not synthesize
callbacks, and `MpeSidecar::reset()` also clears its buffer and pending-release
FIFO.

### Scale-aware bend and voice-modulation projection

`pulp::midi::ScaleAwareMpePitch` in `utility_kernels.hpp` maps the tracked
member-channel bend onto `pulp::music::Scale` degrees and owns only one pitch
glide state. Feed it `MpeNoteState`; do not add another MPE tracker or scale
table. Its bend input is the tracker's semitone value, so configure
`input_bend_range_semitones` to the same member range used by the tracker.

`pulp::audio::MidiVoiceModulationAdapter<MaximumVoices>` is the dependency-safe
bridge to `VoiceModulationBuffer`. The instrument's existing allocator supplies
the voice index; the adapter records note/MPE values for that slot and never
allocates or steals a voice. Putting this bridge in `core/midi` would reverse
the established `audio -> midi` dependency and create a cycle. Pass the same
nonzero 64-bit `MpeNoteGeneration` to note-on, expression, and note-off calls;
stale identity updates are rejected. The tracker never recycles generations on
reset and permanently refuses note-ons after generation exhaustion; surface
that state via `note_generation_exhausted()` / `refused_note_on_count()`.
`release_voice()` also requires the matching nonzero generation; use `flush()`
or `reset()` only for an intentional
identity-free lifecycle clear. Prepare the destination for at least four lanes
before `write_voice()` so the adapter can publish its block atomically.

### Routing utilities flush through their output buffers

`ChannelRouter`, `NoteRangeFilter`, and `KeyboardSplit` own downstream note
lifecycle state even though their routing specifications are otherwise static.
Call their output-bearing `flush()` until its report is complete before a hot
swap; it emits every downstream release and retains suppression for the old
input note-offs that can still arrive. Their output-bearing `replace_spec()`
does this before adopting the new mapping. At a lifecycle boundary that also
resets the input stream, call output-bearing `reset()` instead; it emits the
same releases and then discards the old input ownership. A reset with no output
buffer cannot satisfy the no-orphan-note contract and is intentionally not an
API.

## Reference material

- Guide: [docs/guides/mpe.md](../../../docs/guides/mpe.md)
- Modules: [docs/reference/modules.md](../../../docs/reference/modules.md) — MIDI section
- Example: [examples/mpe-synth/](../../../examples/mpe-synth/) — full working MPE sine synth
- Tests: `test/test_mpe_voice_tracker.cpp`, `test/test_mpe_buffer.cpp`,
  `test/test_mpe_synth_voice.cpp` — invariants worth reading before
  touching the allocator or glide detector

## Related UMP and implementation surfaces

### UMP sysex7 reassembly

UMP type-0x3 sysex7 reassembly is **not part of MpeVoiceTracker** —
it's a separate per-stream state machine shared across every Pulp
UMP backend, exposed as `pulp::midi::UmpSysex7Reassembler` in
`core/midi/include/pulp/midi/ump_sysex7_reassembler.hpp`. Each
input port / source owns one instance (the reassembler is not
thread-safe; that's by design, since CoreMIDI / AUv3 callbacks are
already single-threaded per port).

Touching anything in `core/midi/include/**/*ump*` triggers this
skill via `tools/scripts/skill_path_map.json`. When you add a new
UMP-aware backend (WinRT MIDI 2.0, ALSA UMP, iOS CoreMIDI 2.0),
delegate sysex7 reassembly to `UmpSysex7Reassembler` rather than
re-implementing the start / continue / end state machine inline —
the AUv3 and macOS CoreMIDI backends do exactly that, and any drift
between the two backends corrupts multi-packet sysex streams.

The reassembler's `feed_packet` is a function-pointer-callback
API so it stays RT-safe in the audio render block; the
`feed_collect` convenience wrapper allocates and is meant for
tests / cold paths only.

**Reassembly state is per-stream → per-UMP-group, not per-port.** UMP
SysEx7 streams from one endpoint can interleave across the 16 UMP
groups, so a backend that owns a *single* `UmpSysex7Reassembler` per
port will let a Start on group 1 reset/corrupt an in-flight stream on
group 0. Keep one reassembler per group (`std::array<…,16>` indexed by
`packet.group()`) — the WinRT MIDI 2.0 backend does this; mirror it in
any new UMP backend.

### UMP ↔ MIDI 1.0 conversion covers System messages (Type 0x1)

`ump_to_midi1_event` / `midi1_event_to_ump2` in
`core/midi/include/pulp/midi/ump_conversion.hpp` handle **System Real
Time and System Common** (UMP Type 0x1: clock `0xF8`, start/stop,
song-position `0xF2`, …) in addition to channel voice — system
messages encode as Type 0x1 (NOT Type 0x2 MIDI 1.0 Channel Voice,
which is malformed for them) and decode back to MIDI 1.0 short
messages. So `ump_to_midi1` flattening a UMP buffer yields
clock/transport events, not channel-voice-only — don't assume a
flattened buffer is note data. (SysEx Type 0x3 still routes through
`UmpSysex7Reassembler`, above; per-note expression still goes through
the MpeBuffer sidecar, not these converters.)

### UMP Session / Endpoint / VirtualEndpoint

Pulp exposes a Pulp-native UMP transport surface in
`core/midi/include/pulp/midi/`:

- `UmpEndpoint` (abstract) — id + direction (`can_receive` /
  `can_send`) + `send(UmpPacket)` + `set_receive_callback(...)`.
  Concrete subclasses are platform-specific (CoreMIDI 2.0 on
  macOS) or in-process (`VirtualUmpEndpoint`).
- `UmpSession` — one per app/plugin; owns the OS MIDI client and a
  registry of virtual endpoints. `enumerate_endpoints()` merges
  OS-discovered and virtual entries; `open_endpoint(id, &status)`
  returns a borrowed pointer (session owns lifetime).
- `VirtualUmpEndpoint` — purely in-process, loopback-optional,
  `send()` and `deliver()` counters. The only safe surface for
  headless tests because CoreMIDI 2.0 connections require a real
  MIDI Studio. `UmpSession::wire_virtual_loopback("from", "to")`
  threads two virtual endpoints together for round-trip fixtures.

When you add a new OS backend (WinRT MIDI 2.0, ALSA UMP), do NOT
write a parallel session abstraction — implement the
`OsBackendVTable` declared in `core/midi/src/ump_session_backend.hpp`
and register it from a static initialiser in the platform
TU. The cross-platform `ump_session.cpp` patches the vtable at
load time; if no platform backend is linked, the session reports
`os_backend_active() == false` and operates virtual-only (this is
exactly what the test target exercises everywhere).

Lifetime invariant: the input-port block on macOS captures the
endpoint's raw pointer. The endpoint is `unique_ptr` inside
`OsState::endpoints` — never reseat or move it after the block
is installed, or the block's captured pointer dangles.

## Implementation note: where MpeVoiceTracker bodies live

`MpeVoiceTracker`'s method bodies live in
`core/midi/src/mpe_voice_tracker.cpp`, not inline in
`core/midi/include/pulp/midi/mpe_voice_tracker.hpp`. The header keeps the
class declaration + trivial inline getters; non-trivial methods (`process`,
`set_config`, `reset`, `add_note`, `remove_note`, etc.) link from the .cpp.

Practical effect: editing `MpeVoiceTracker` implementation bodies only
rebuilds the .cpp users. If you're adding a new method, put trivial
getters inline; put anything with branches/loops in the .cpp.

## What this skill does NOT cover

- MIDI 2.0 UMP native path — deferred. When it lands, `MpeBuffer` will have
  a lossless UMP round-trip and hosts with UMP transport will skip the 1.0
  decode step.
- VST3 / AU MPE routing — see "Format adapter coverage" above; the
  host-side adapters that emit `MpeBuffer` are tracked in the hosting
  plan, not here.
- Hosting MPE plugins (MPE output, dispatching MPE into a loaded
  plugin) — covered by the SignalGraph hosting work, not this skill.
