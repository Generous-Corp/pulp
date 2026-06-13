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

When `supports_mpe` is `true`, or when
`node_capabilities.supports_mpe` is true, a Pulp format adapter that
recognises MPE (currently: CLAP) runs the inbound `MidiBuffer` through
an `MpeVoiceTracker` each block, populates an `MpeBuffer`, and attaches
it via `Processor::set_mpe_input()` before calling `process()`. Adapters
read `PluginDescriptor::effective_capabilities()`, which ORs the legacy
flags with the node ABI capability field.

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

`MpeVoiceAllocator<Voice>::telemetry()` returns an owner-thread snapshot with
polyphony, active/releasing voice counts, steal count, steal mode, and the
latest glide flag. If UI or tooling needs that data, call it from the processor
owner and publish the returned value through a lock-free latest-value channel.
For optional per-voice analysis, preview, or diagnostics, pass a
`runtime::RuntimeBudgetFrame` to
`MpeVoiceAllocator<Voice>::evaluate_optional_runtime_budget()`. The allocator
uses the same voice telemetry to return a run/defer/shed/bypass decision without
changing the MPE voice render path. Budget degradation does not disable active
notes, change expression smoothing, or alter voice stealing; it is only for
optional analysis, preview, or diagnostic work.

## Example

`examples/mpe-synth/` ships an MPE-aware sine synth that demonstrates
opt-in, per-note pitch bend across the full ±48 semitone range,
pressure-driven amplitude, and CC 74 brightness control via a one-pole
lowpass.

## Adapter status

The CLAP adapter populates `mpe_input()` and `ump_input()` when the descriptor
opts in. Other adapters still deliver the standard `MidiBuffer` path only, so
MPE-aware processors should continue to handle ordinary MIDI input as their
fallback.

## SignalGraph routing

`SignalGraph` does not carry a separate graph-owned `MpeBuffer`. Graph MIDI
edges preserve the block event stream that MPE is derived from: MIDI 1.0
channel messages, SysEx sidecars, and attached `UmpBuffer` packets. That means
MIDI 1.0 MPE channel messages and MIDI 2.0 per-note UMP expression packets can
pass through `connect_midi()` routes without losing sample offsets or per-note
payloads.

At plugin/adapter boundaries, processors that opt into MPE still consume the
derived `Processor::mpe_input()` sidecar. Hosts or adapters that need an
`MpeBuffer` after graph routing should run the routed `MidiBuffer` and attached
`UmpBuffer` through `MpeVoiceTracker`, the same way format adapters derive MPE
for processors.

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
New code may also set `node_capabilities.supports_mpe` and
`node_capabilities.supports_ump`; `effective_capabilities()` makes the
two declaration styles equivalent. The CLAP adapter populates the UMP
sidecar by converting the inbound MIDI 1.0 stream with `midi1_to_ump()`
and by appending native `CLAP_EVENT_MIDI2` packets when the host sends
them.

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
