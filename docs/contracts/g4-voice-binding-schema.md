# G-4 voice-bank binding schema

Status: **bounded control-side source slice**. This schema is the public,
value-owned boundary for authored voice-template bindings. It does not create
runtime nodes, route MIDI/MPE, schedule events, render audio, or claim G-6.

`pulp/host/voice_template.hpp` defines `VoiceTemplateSchema` and
`VoiceBindingSpec`. A schema has a nonzero template revision, bank schema
version `1`, and one or more bindings. Each binding carries canonical authored
path segments, a target name, target kind (port or parameter), type,
direction, shape, rate, and a port ordinal. Shared mutable bindings are refused;
all bindings are per-slot and cannot mutate another slot.

`prepare_voice_bank` validates the complete declaration before returning a
manifest. It normalizes NFC UTF-8, rejects malformed/control/slash/dot-ambiguous
segments, rejects unsupported enum combinations, and refuses duplicate
declarations whose normalized canonical identities collide. It then expands exactly eight records for each
binding. Every record owns a G-4 canonical identity and contributes to a
manifest SHA-256 digest. Parameter ordinals are canonicalized to zero because
ordinals are meaningful only for port bindings. No runtime node ID, allocator
index, epoch, or note generation is encoded.

`VoiceBankState` persists only the manifest digest and finite values for declared
per-slot parameter bindings. The public persistence bound is 4096 parameter
records per state. `encode_voice_bank_state` emits a deterministic tagged,
length-delimited big-endian payload and digest. Decode and restore reject counts
above that bound; restore validates the whole candidate before assignment.
Stale identity, unsupported schema, duplicate/undeclared canonical paths, and
non-finite values leave the previous state untouched. A caller can retain a
prior value-owned state as an undo record and restore it through the same
transaction.

The installed `examples/g4-voice-bank-consumer` closes the public persistence
consumer boundary. This slice still does not satisfy the G-4 → G-6 admission
packet's requirements for G-5 host MIDI/MPE ingress or the A1 eight-voice
audio/tail oracle. Those remain explicit downstream gates.
