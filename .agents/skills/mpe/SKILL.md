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
| An MPE synth plugin | `MpeVoiceAllocator<YourVoice>` inside the processor, dispatch `MpeBuffer` in `process()`, set `supports_mpe = true` in the descriptor |
| A host that loads MPE plugins | Build an `MpeBuffer` from inbound MIDI (zone-aware) and hand it to `Processor::mpe_input()` — the CLAP adapter already does this |
| Pure MIDI 2.0 UMP work | Out of scope — Phase 4 is deferred, see `planning/next-features-plan.md` |

## The three-step pattern for a new MPE synth

### 1. Scaffold with `--mpe`

```bash
./build/tools/cli/pulp create MySynth --type instrument --mpe
```

The CLI post-processes the generated descriptor to add
`.supports_mpe = true` and includes `<pulp/midi/mpe_buffer.hpp>`. No
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

void process(ProcessContext& ctx) override {
    allocator_.dispatch(ctx.mpe_input());  // handles note-on/off/expressions
    for (auto& v : allocator_.voices()) {
        if (v.active()) v.render(ctx.output.channel_ptr(0), ctx.num_samples);
    }
}
```

`MpeVoiceAllocator::dispatch()` walks the buffer in order, handles
note-on allocation (oldest-steal when full), routes per-note expression
updates to the right voice, and runs note-off logic including the glide
refcount. Do not call `on_note_on` / `on_note_off` directly.

## Gotchas

### Zones are negotiated, not assumed

`MpeVoiceTracker` reads RPN 6 / 7 (MPE Configuration Message) to
discover whether the controller sent a lower zone (master ch 1, members
2–N) or upper zone (master ch 16, members N–15). Don't hard-code
channel 1 as the master — trust the zone config.

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
