# G-6 voice lifecycle and audio-tail qualification — exact-head HOLD (2026-11-10)

## Disposition

**HOLD / NO-CHANGE.** This audit starts from `origin/main` at
`b8a8278c9b3c876370fe921ddbbf7434030335ae` in a fresh classified worktree.
It adds no graph, Processor, scheduler, allocator, or audio runtime code. The
existing public surfaces do not yet provide the reviewed binding needed to
connect the G-4 authored voice identity to a prepared Processor voice
namespace, so adding a second bridge here would create a competing contract.

The smallest defensible next implementation remains: admit one reviewed
Processor voice binding, then build a fixed eight-voice installed consumer with
an audio-output and release-tail oracle. This document records why that slice
cannot be honestly claimed at this head.

## Verified current surfaces

* `core/host/include/pulp/host/voice_template.hpp` explicitly defines a
  control-side schema. `VoiceBankState` persists finite per-slot parameters;
  active notes, generations, pedals, epochs, queues, and release tails are not
  representable.
* `core/format/include/pulp/format/processor_node_adapter.hpp` exposes a
  non-owning `ProcessorNode` adapter. It prepares one Processor binding and
  translates routed block I/O, but has no voice-slot identity, allocator owner,
  note-generation lifecycle, pedal policy, or tail completion contract.
* `core/midi/include/pulp/midi/mpe_synth_voice.hpp` provides a useful
  preallocated `MpeVoiceAllocator` helper with steal policies and generation
  checks. Its installed consumer is state-only: the sample voice in
  `examples/g6-voice-contract-consumer/main.cpp` advances expression state and
  deliberately renders no audio.
* The admitted G-5 event lane carries bounded graph-authored event records and
  rejects stale, future, late, overflow, and over-budget records. It does not
  assign voice ownership, implement sustain/reset semantics, or drain a
  release tail.

These observations are source-backed and do not promote helper or control-side
tests into graph runtime evidence.

## Missing qualification gates

1. **Processor binding:** stable G-4 slot identity must select one prepared,
   isolated Processor voice namespace without runtime-node or insertion-order
   identity.
2. **Lifecycle:** note-on/off, retrigger, all documented steal policies, MPE
   expression, and stale-generation refusal must survive split block
   partitions through the graph event reader.
3. **Pedals and reset:** CC64 deferred release plus CC120/121/123 cleanup,
   epoch reset, and stale-target negatives need independent controls.
4. **Audio oracle:** a fixed eight-voice public consumer must render nonzero
   deterministic output, isolate a voice mutation, and prove finite release
   tail drain to silence under split blocks.
5. **Realtime/public parity:** the exact source and installed SDK consumer need
   governed callback WCET/allocation/lock/I/O evidence and explicit platform
   boundaries. Existing source-level preallocation and allocator checks are
   insufficient for these claims.

## Safe next action

Review and admit the G-4 Processor voice-binding contract. Once that contract
is current and approved, a separate lane may implement the fixed eight-voice
consumer and mutation-backed audio/tail receipt. Until then, this lane remains
an evidence HOLD; no speculative wrapper or duplicate scheduler is justified.
