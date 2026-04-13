# MPE (MIDI Polyphonic Expression)

Pulp provides first-class MPE support as an opt-in sidecar on top of the
normal MIDI processing path. Plugins that don't opt in see the standard
`MidiBuffer`; plugins that opt in additionally receive an `MpeBuffer`
that carries per-note pitch bend, pressure, and timbre expressions.

## Quick start

```cpp
#include <pulp/format/processor.hpp>
#include <pulp/midi/mpe_buffer.hpp>
#include <pulp/midi/mpe_synth_voice.hpp>

class MySynth : public pulp::format::Processor {
    pulp::midi::MpeVoiceAllocator<MyVoice> allocator_{8};
public:
    PluginDescriptor descriptor() const override {
        return {
            .name = "MySynth",
            // ...
            .accepts_midi  = true,
            .supports_mpe  = true,   // ← opt in
        };
    }

    void process(audio::BufferView<float>& out, const audio::BufferView<const float>&,
                 midi::MidiBuffer& midi_in, midi::MidiBuffer&,
                 const ProcessContext& ctx) override {
        if (const auto* mpe = mpe_input()) {
            allocator_.dispatch_all(*mpe);
        }
        // ... render voices into out ...
    }
};
```

When `supports_mpe` is `true`, a Pulp format adapter that recognises MPE
(currently: CLAP) runs the inbound `MidiBuffer` through an
`MpeVoiceTracker` each block, populates an `MpeBuffer`, and attaches it
via `Processor::set_mpe_input()` before calling `process()`.

## Components

| Header | Class | Role |
|--------|-------|------|
| `pulp/midi/mpe_voice_tracker.hpp` | `MpeVoiceTracker` | Parses MIDI 1.0, tracks note/expression state by channel |
| `pulp/midi/mpe_voice_tracker.hpp` | `MpeNoteState` | Snapshot of one note's channel, pitch bend (semitones), pressure, timbre |
| `pulp/midi/mpe_buffer.hpp` | `MpeBuffer`, `MpeExpressionEvent` | Sample-accurate per-note expression event stream |
| `pulp/midi/mpe_synth_voice.hpp` | `MpeSynthVoice` | Base class for voices with built-in expression smoothing |
| `pulp/midi/mpe_synth_voice.hpp` | `MpeVoiceAllocator<Voice>` | Routes `MpeBuffer` events to a voice pool; configurable steal mode |
| `pulp/midi/mpe_synth_voice.hpp` | `MpeGlideDetector` | Flags legato/glide gestures (overlap on same MPE member channel) |

## Zone configuration

`MpeConfig` (from `pulp/midi/ump.hpp`) describes which channels belong to
which zone. The tracker defaults to the standard lower zone
(`MpeConfig::standard_lower(15)` — manager on channel 0, members on
channels 1–15). Use `MpeConfig::dual(lower, upper)` for dual-zone
controllers.

## Pitch-bend ranges

Per-MPE spec: member channels default to ±48 semitones, manager channels
to ±2 semitones. Override with
`MpeVoiceTracker::set_member_bend_range()` and
`set_manager_bend_range()` if your plugin accepts a different convention
(e.g. `±12` for backwards-compat MIDI 1.0).

## Expression mapping

The tracker normalises expressions so voices see consistent units:

| Source | Tracker field | Range |
|--------|---------------|-------|
| 14-bit pitch bend on member channel | `pitch_bend_semitones` | ±member_bend_range |
| Channel pressure (status `Dx`) | `pressure` | 0.0–1.0 |
| CC 74 (timbre/slide) | `timbre` | 0.0–1.0 |

`MpeSynthVoice` adds per-sample smoothing so subclasses can read
`pitch_bend()`, `pressure()`, `timbre()` directly without fighting
zipper noise.

## Example

`examples/mpe-synth/` ships an MPE-aware sine synth that demonstrates
opt-in, per-note pitch bend across the full ±48 semitone range,
pressure-driven amplitude, and CC 74 brightness control via a one-pole
lowpass.

## Status

- **Phase 1** — `MpeVoiceTracker` (landed)
- **Phase 2** — `MpeBuffer` sidecar, `Processor::mpe_input()`, CLAP wiring (landed)
- **Phase 3** — `MpeSynthVoice`, `MpeVoiceAllocator`, `MpeGlideDetector`, `examples/mpe-synth/` (landed)
- **Phase 4** — MIDI 2.0 UMP native path (landed)

VST3 and AU adapters will gain the same sidecar wiring in a follow-up
pass; the API is stable so plugins can opt in today and benefit
automatically once the other adapters ship.

## MIDI 2.0 UMP sidecar

Plugins that want native MIDI 2.0 resolution — 16-bit velocity, per-note
pitch bend, per-note CCs — can opt in separately:

```cpp
PluginDescriptor descriptor() const override {
    return {
        // ...
        .accepts_midi = true,
        .supports_ump = true,   // ← MIDI 2.0 UMP sidecar
    };
}

void process(...) override {
    if (const auto* ump = ump_input()) {
        for (const auto& ue : *ump) {
            // ue.packet is a UmpPacket, ue.sample_offset is sample-accurate
        }
    }
}
```

`supports_mpe` and `supports_ump` are independent and can both be set.
The CLAP adapter populates the UMP sidecar by converting the inbound
MIDI 1.0 stream with `midi1_to_ump()`; when CLAP ships
`CLAP_EVENT_MIDI2` the adapter will feed the host's native packets
directly with no code change on the plugin side.

`MpeVoiceTracker::process(UmpPacket)` accepts UMP input in addition to
`MidiEvent`, routing MIDI 2.0 per-note pitch bend (status `0x60`) and
per-note CC (status `0x00`) directly to the matching note rather than
via the member-channel cache, so per-note expression stays truly
per-note even within the same MPE member channel.

Helpers in `pulp/midi/ump_conversion.hpp`:

| Function | Purpose |
|----------|---------|
| `midi1_to_ump(MidiBuffer&, UmpBuffer&)` | Convert a MIDI 1.0 block to MIDI 2.0 UMP packets, preserving sample offsets |
| `ump_to_midi1(UmpBuffer&, MidiBuffer&)` | Flatten UMP back to MIDI 1.0 (packets with no MIDI 1.0 equivalent are skipped — route those via `mpe_input()`) |
| `scale_7_to_16` / `scale_16_to_7` | Velocity scaling, round-trip-preserving for exact values |
| `scale_14_to_32` / `scale_32_to_14` | Pitch-bend scaling with centre (0x2000 ↔ 0x80000000) preserved exactly |
