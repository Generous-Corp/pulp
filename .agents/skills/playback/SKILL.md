---
name: playback
description: Pulp timeline transport and immutable compiled playback programs, bounded compile execution, block-level publication latches, stable shells, and ProcessContext projection.
---

# Playback

Playback has two independent public surfaces: `MasterTransport` for the block
clock, and immutable compiled playback programs. Build a
`ProgramCompileRequest` from one captured immutable Project snapshot, an
external monotonically increasing document revision, a shared precompiled tempo
map, and an explicit `DirtyTrackSet`. Drive it with
`DeferredCompileExecutor::run_for()` on threadless/UI hosts or use
`WorkerCompileExecutor` on native threaded hosts. The compiler is the sole
publisher to its `PlaybackProgramStore`.
Use sparse `TrackCompilePolicy` deltas when a track changes provider selection
or adoption policy. The compiler validates availability, forces that track
through the dirty path, retains omitted published policies, and coalesces
pending deltas with latest-track wins.
For this phase, only `ProviderKind::Arrangement` with the arrangement-only
availability mask is valid; reject launcher or external-input claims until
their provider payloads are compiled.

On the audio thread, call `PlaybackProgramBlockLatch::begin_block()` exactly
once per callback and pass that pin to every `StableRendererShell`. Never cache
a `TrackProgram*` past the pin. Adoption accepts skipped generations
(`candidate > active`) for the same ItemId and proves carry-state ownership
against the shell's `RendererCarryState` SeqLock snapshot.

For arrangement note playback, construct one `ArrangementNoteRenderer` per
track, call `prepare(maximum_events_per_block)` off the audio thread, then pass
the shared block pin and the current `TransportSnapshot` to `process()`. The
renderer owns a bounded realtime-limited MIDI buffer; inspect `events()` only
for the current block. The buffer carries a full-resolution native MIDI-2 UMP
sidecar alongside its MIDI-1 compatibility mirror; treat the two lanes as one
atomic event block and propagate either lane's overflow. It consumes both
transport ranges in order, releases
active notes before the second range on a loop wrap, and intentionally resets
without note chase on seek/adoption in Phase 1. `TransportSnapshot` carries the
non-owning identity of the exact compiled tempo map that resolved its ranges;
the renderer rejects a program compiled against another map. Overlapping
logical notes on one MIDI key are reference-counted into one physical note-on
and one final note-off.

Use this skill when changing `core/playback`, the master timeline transport, or
the format-layer projection from playback snapshots to `ProcessContext`.

## Contracts

- Playback owns integer `TickPosition`, `SamplePosition`, and `MonotonicBeat`
  state. Floating-point beat values exist only in the one-way format projection.
- A block has one range normally and at most two ranges when it crosses one loop
  boundary. `prepare()` rejects a loop shorter than `max_buffer_size`, which is
  what makes the fixed two-range representation complete.
- Timeline ticks wrap at the loop boundary. `MonotonicBeat` never wraps or
  reanchors on a seek; only a new prepare/reset lifecycle starts a new clock.
- A stopped block still emits one range covering all callback frames, but both
  musical clock intervals have zero duration.
- The control thread is the sole writer of the complete desired-state `SeqLock`.
  `begin_block()` is the sole audio-thread consumer and must remain allocation-
  and lock-free. It is declared `AudioCallbackSafeAfterPrepare`, wraps itself
  in `ScopedNoAlloc`, and its test uses `ScopedRtProcessProbe` so Unix CI traps
  both allocations and pthread locks.
- Starting playback is not a seek or DSP reset. Explicit seeks request a reset;
  range discontinuities project to `ProcessContext::transport_jump`.
- Arrangement note events are compiled against the owning program's exact
  tempo map and ordered by sample, note-off before note-on, then clip/note ID.
  A renderer uses half-open sample ranges and never latches a callback size.
- Note rendering is a transport-tick MIDI lane. Do not lower it to an audio
  `CustomNodeType`; the host/embedded adapter routes its bounded MIDI output.
- `core/playback` must not include `pulp/format`, `pulp/host`, or `pulp/view`.
  `<pulp/format/playback_context_projection.hpp>` owns the one-way adapter.
  Keep `timeline-engine-dependency-floor` green; it allowlists source includes
  and CMake links for timebase, timeline (when present), and playback.

## Validation

Build and run `pulp-test-playback-transport`, `pulp-test-timebase`, and
`pulp-test-transport-quantizer`. Keep loop-boundary, variable-block, ramp,
negative-preroll, extreme-position, SeqLock hammer, and RT-allocation cases.
When export/install wiring changes, also run the installed SDK consumer smoke.
Also build `timeline-program-threadless-no-exceptions-check`; it compiles the
program/compiler/executor/shell lane with `-fno-exceptions -fno-rtti` and the
threadless executor stub. Run the WASI SDK build when `/opt/wasi-sdk` is
available; the native compile-only gate remains mandatory when it is not.
