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

- The synth is meant for Roli Seaboard / Linnstrument / Sensel Morph
  / LinnStrument / KMI / similar per-note controllers.
- You need polyphonic per-note pitch bend (not just a global bend).
- You want pressure or CC 74 to modulate each voice independently.

If you only need monophonic aftertouch or a global mod wheel, plain
`MidiBuffer` in `process()` is simpler — do not reach for MPE.

## Decision tree

| You're writing... | Use |
|--|--|
| An MPE synth voice | Subclass `midi::MpeSynthVoice`, render your oscillator using `state().pitch_bend_semitones`, `state().pressure`, `state().timbre` |
| An MPE synth plugin | `MpeVoiceAllocator<YourVoice>` inside the processor, dispatch `MpeBuffer` in `process()`, set `supports_mpe = true` or `node_capabilities.supports_mpe = true` in the descriptor |
| A host that loads MPE plugins | Build an `MpeBuffer` from inbound MIDI (zone-aware) and hand it to `Processor::mpe_input()` — the CLAP adapter already does this |
| Pure MIDI 2.0 UMP work | Out of scope — Phase 4 is deferred, see `planning/next-features-plan.md` |

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

### UMP per-note management + assignable PNC (post #2860)

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
  detach-only on the live note (matches spec for the sounding note,
  Codex P1 on #2860). The armed-future-reset slice is deferred
  follow-up work; if you need the full D+S note-rotation flow, file
  an issue with a controller reproducer.

If you're routing UMP into the tracker, use the factories on
`UmpPacket`: `per_note_management(group, channel, note, flags)`,
`assignable_per_note_cc(...)`, `registered_per_note_cc(...)`,
`per_note_pitch_bend(...)`. Channel-level cache stays updated even
for detached notes so freshly-added notes on the same channel still
inherit running state via `add_note`.

### Format adapter coverage

As of the MPE Phase 1–3 merge (PR #135, #138), the CLAP adapter
populates `MpeBuffer` from inbound MIDI. VST3 and AU adapters still
forward plain MIDI only — they'll be wired in a later iteration. Until
then, an MPE synth loaded as VST3/AU sees MIDI events but the
`MpeBuffer` will be empty; the voice tracker inside the processor
still works if you extract per-note data from `MidiBuffer` yourself.

## Reference material

- Guide: [docs/guides/mpe.md](../../../docs/guides/mpe.md)
- Modules: [docs/reference/modules.md](../../../docs/reference/modules.md) — MIDI section
- Example: [examples/mpe-synth/](../../../examples/mpe-synth/) — full working MPE sine synth
- Tests: `test/test_mpe_voice_tracker.cpp`, `test/test_mpe_buffer.cpp`,
  `test/test_mpe_synth_voice.cpp` — invariants worth reading before
  touching the allocator or glide detector
- Plan: `planning/next-features-plan.md` § Feature 2

## What this skill does NOT cover

- MIDI 2.0 UMP native path — Phase 4, deferred. When it lands,
  `MpeBuffer` will have a lossless UMP round-trip and hosts with UMP
  transport (CLAP draft, future VST3) will skip the 1.0 decode step.
- VST3 / AU MPE routing — see "Format adapter coverage" above; the
  host-side adapters that emit `MpeBuffer` are tracked in the hosting
  plan, not here.
- Hosting MPE plugins (MPE output, dispatching MPE into a loaded
  plugin) — covered by the SignalGraph hosting work, not this skill.

### UMP sysex7 reassembly (post macOS-plan 8.2)

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
between the two backends used to cause the `#239` / `#292` family
of bugs.

The reassembler's `feed_packet` is a function-pointer-callback
API so it stays RT-safe in the audio render block; the
`feed_collect` convenience wrapper allocates and is meant for
tests / cold paths only.

## Implementation note: where MpeVoiceTracker bodies live

As of companion-track U-9 (2026-05-19), `MpeVoiceTracker`'s method bodies live in `core/midi/src/mpe_voice_tracker.cpp`, not inline in `core/midi/include/pulp/midi/mpe_voice_tracker.hpp`. The header keeps the class declaration + trivial inline getters; non-trivial methods (`process`, `set_config`, `reset`, `add_note`, `remove_note`, etc.) link from the .cpp.

Practical effect: editing `MpeVoiceTracker` impl no longer recompiles every TU that includes the MPE header. If you're adding a new method, put trivial getters inline; put anything with branches/loops in the .cpp.
