# Voice runtime facade

Include `<pulp/audio/voice_runtime_facade.hpp>` when an instrument needs a
single, explicit entry point to Pulp's existing voice owners. The facade is a
compile-time, non-owning adapter. It does not add an allocator, scheduler,
envelope, oscillator, renderer, or DSP engine.

```cpp
pulp::midi::Synthesiser<MyVoice> synth(16);
pulp::audio::VoiceRuntimeFacade runtime(synth);
runtime.set_steal_strategy(pulp::midi::VoiceStealStrategy::Quietest);
runtime.process(midi_events, mono_output, frame_count);
```

The owner must outlive the facade. Spans passed to allocator methods are
borrowed for that call only. There is no type erasure, virtual dispatch, or
heap allocation in the facade.

Facade steal-policy setters reject invalid enum values and leave the owner's
last valid policy unchanged. This closes a low-level footgun without changing
the underlying policy enums or their deterministic tie rules.
The plain-MIDI pitch-bend range accepts every finite positive `float` semitone
value; zero, negative, NaN, and infinity are rejected without changing the
last valid range.

## Choose one owner

| Owner | Facade preserves | Deliberate boundary |
|---|---|---|
| `midi::Synthesiser<Voice>` | Sample-offset MIDI dispatch, sustain (CC64), sostenuto (CC66), soft-pedal metadata (CC67), channel controllers, voice-group choke callbacks, configured deterministic stealing, and the owner's additive voice rendering | Stealing reuses the selected voice immediately. It has no allocator termination record or steal-tail fade. Per-voice modulation remains voice-owned. |
| `audio::InstrumentVoiceAllocator` | Prepared slots, voice/choke groups, group-aware oldest stealing, exact `VoiceTerminationReason`, termination fade frames, release/finish, caller-prepared per-slot modulation buffers, and `VoiceSumMixer` | It does not interpret MIDI, schedule sample offsets, or invent pedal state. The caller owns voice rendering and termination-tail rendering. |

An unsupported owner fails at compile time with a diagnostic naming the two
supported forms. MPE remains on its existing `MpeVoiceAllocator` path and is
not silently treated as plain MIDI.

## Allocated voices

The allocator overload requires exactly one prepared
`VoiceModulationBuffer` and one `VoiceTermination` record slot per prepared
voice. Exact capacity is intentional: a choke can terminate every old slot,
and a short result span would hide the affected voice identities. Capacity or
prepared-state failures occur before the allocator or any modulation block is
mutated.

```cpp
pulp::audio::InstrumentVoiceAllocator allocator;
allocator.prepare(8); // control thread

std::array<pulp::audio::VoiceModulationBuffer, 8> modulation;
for (auto& buffer : modulation)
    buffer.prepare({.max_lanes = 6, .max_frames = max_block});

std::array<pulp::audio::VoiceTermination, 8> terminations;
pulp::audio::VoiceRuntimeFacade runtime(allocator);
const auto result = runtime.trigger(
    {.note = 60, .sample_id = sample_id, .voice_group = 1},
    terminations, modulation);
```

On success, each terminated slot and the newly allocated slot has its
modulation block reset. This prevents lanes from an old voice ID leaking into
a reused slot. Released voices keep their lanes for their release tail; lanes
clear when `finish_voice` succeeds. The returned `VoiceAllocationResult` and
termination records retain the allocator's native reasons, IDs, overflow
semantics, and fade-frame values.

## Note-event modulation bridge

`VoiceNoteModulationBridge::write` is opt-in. It replaces one prepared buffer's
current block with six constant lanes after validating every input and routing
before mutation:

| Meaning | Default target | Unit/range |
|---|---|---|
| Pitch | `PitchCents` | cents relative to `reference_note`, including normalized bend times the configured semitone range |
| Velocity | `Gain` | MIDI velocity divided by 127 |
| Gate | `Aux0` | exactly 0 or 1 |
| Pressure | `Pressure` | normalized 0 through 1 |
| Timbre | `Timbre` | normalized 0 through 1 |
| Expression | `Aux1` | normalized 0 through 1 |

The routing is explicit and may be changed, but all six targets must be valid
and distinct. The buffer needs at least six lanes and the requested nonzero
frame count must fit its prepared maximum. Rejected notes, controller values,
bend values, routing enums, duplicate targets, and capacities leave the
previous block unchanged. The bridge does not smooth, clamp, scale to a
particular oscillator, or create an envelope.

For a plain-MIDI voice, retain controller state in the existing voice hooks
and invoke the bridge from that voice's own block preparation if these lanes
match its DSP contract. For allocated sampler-style voices, pass the buffer at
the returned voice index to the renderer. Publishing telemetry or state to
another thread remains a caller-owned RT handoff.
