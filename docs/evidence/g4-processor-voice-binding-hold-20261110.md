# G4 Processor voice-binding qualification HOLD — 2026-11-10

Status: **HOLD / evidence only**  
Source under audit: `origin/main` at `fb0ca2601cab3f082d72e4bba9f2aaf5675eb3a8`

## Verified current surface

The public G4 surface in `core/host/include/pulp/host/voice_template.hpp` and
`voice_identity.hpp` provides deterministic authored slot identities, binding
validation, eight-slot manifest expansion, and transactional parameter-state
persistence. `examples/g4-voice-bank-consumer/main.cpp` is an installed-SDK
consumer with positive save/load/restore controls and planted shared-binding
and forged-state refusals.

The repository also contains reusable Processor-level voice machinery in
`core/audio/include/pulp/audio/voice_runtime_facade.hpp` and the existing
`examples/pulp-multi-out` Processor. Those surfaces are independent of the G4
manifest: they do not consume `VoiceBankManifest`/`VoiceSlotBinding`, map a
manifest port to a Processor audio buffer, or bind slot identity to note
allocation and generation state.

## Missing qualification seam

No current public API connects a G4 authored voice binding to a
`format::Processor` render path. In particular, the installed consumer does
not call a graph voice Processor, render per-slot audio, or prove that a slot's
identity/generation is preserved through note-on, note-off, steal, restore, and
release-tail processing. The current `VoiceBankState` deliberately cannot
represent active notes, generations, epochs, queues, pedals, or release tails.

The existing `VoiceRuntimeFacade` can exercise standalone allocator/synthesiser
owners, but using it as G4 completion evidence would conflate two separate
contracts and would not prove graph binding. Adding an adapter or new public
runtime API would be implementation work outside this evidence-only slice.

## Required next evidence

A future authorized slice must define and review a public seam that binds a
G4 manifest slot to a Processor voice instance, then provide a real installed
consumer with:

- positive per-slot audio output and an independent planted wrong-slot control;
- note-on/note-off, steal, generation invalidation, restore refusal, and
  release-tail behavior;
- bounded callback allocation/lock/I/O/WCET measurements;
- exact SDK, source, executable, and receipt hashes; and
- explicit platform/host limits.

Until that seam and consumer exist, G4 remains bounded control-side evidence
and G6 remains dependency-gated. This document does not claim Processor voice
binding, graph realtime behavior, or audio-tail qualification.
