---
name: playback
description: Pulp timeline transport, immutable compiled playback programs, bounded arrangement audio rendering, block-level publication latches, stable shells, and ProcessContext projection.
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

For MediaRef clips, prepare a `DecodedAudioAssetPool` off the audio thread. Use
`DecodedAudioAssetPool::decode_wav()` for bounded in-memory WAV bytes, then pass
the immutable pool in `ProgramCompileRequest::audio_assets`. The existing
compiler incrementally lowers media clips into each `TrackProgram`; do not build
a second playback-program model. The renderer uses bounded stateless linear SRC so
source audio runs at its native wall-clock rate after sample-rate conversion.
A musically anchored clip changes placement and timeline extent through the
tempo map but does not imply warp or time-stretch.
Gain and anchor-native fade durations live on the immutable Clip. Missing,
mismatched, or over-capacity assets fail compilation instead of creating a
silent placeholder.

On the audio thread, call `PlaybackProgramBlockLatch::begin_block()` exactly
once per callback and pass that pin to every `StableRendererShell`. Never cache
a `TrackProgram*` past the pin. Adoption accepts skipped generations
(`candidate > active`) for the same ItemId and proves carry-state ownership
against the shell's `RendererCarryState` SeqLock snapshot.
The host's `TimelineGraphBinding` is the deliberate exception to independently
latching `PlaybackProgramStore`: its enclosing immutable binding generation
already owns the exact `PlaybackProgram` together with the exact graph snapshot
and renderer set. It constructs a non-owning `PlaybackProgramBlock` only while
that generation is pinned, so program destruction/refcount traffic still never
runs on the audio thread. Content adoption republishes the whole binding
generation; do not reintroduce a separate store latch there.
When timeline code needs graph timing, query latency through that same pinned
`SignalGraph::ExecutionSnapshot`. Its `latency_to_output()` result belongs to
the prepared graph generation; do not reconstruct PDC from live plugin slots or
collapse unavailable, failed, missing-snapshot, and unknown-node outcomes.

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

Compile an unattached `AutomationLane` with `AutomationProgram::compile()` on
the control/worker thread. The immutable program owns its exact tempo map and
retains tick-domain segment semantics. Each compile also receives a nonzero
instance token; generation orders adoption, while the token prevents an equal-
generation replacement from masquerading as the active immutable program.
`AutomationCursor::process()` consumes
the shared transport snapshot and writes plain-domain control points into a
caller-owned span. Each point says whether it seeds a range, steps immediately,
or ramps linearly from the preceding emitted point. Span capacity is the
explicit per-lane budget: range seeds and unique in-range authored knots are
mandatory, remaining capacity refines continuous spans deterministically, and
output never overflows. Keep device-wide budgeting, lane aggregation, parameter
metadata, normalization, and the SignalGraph mailbox write in the host binding;
playback must not depend on `pulp::state` merely to mirror
`ParameterEventQueue`.

Group already-compiled lane owners with `TrackAutomationProgram::create()` on
the compiler thread. The aggregate validates a compiler-supplied track ID,
requires exact tempo-map owner identity, rejects duplicate lane IDs and
device-parameter targets, and stores programs in lane-ItemId order. Preserve
unchanged program owners when rebuilding it: mixed child generations are
intentional because each cursor adopts by its lane program's generation and
instance token.

`ProgramCompiler` is the attachment boundary for authored automation. It walks
each track's ordered device placements, compiles only lanes owned by that track,
and publishes the resulting `TrackAutomationProgram` inside the immutable
`TrackProgram`. Use `AutomationPlaybackLimits` on every compile request: reject
over-limit device, lane, and point counts before reserving proportional storage,
and use `platform_defaults()` so wasm/threadless builds receive their lower
budgets. Incremental compilation retains unchanged lane owners; attachment,
target, or point edits dirty only the affected track/lane.

On the audio thread, give one `TrackAutomationRenderer` the exact immutable
track automation program and the shared transport snapshot. It emits bounded
per-device `ParameterEvent` batches in device-placement order: seeds become
zero-duration endpoints, linear points preserve their ramp duration, and
immediate points step at their sample offset. Candidate traversal and emitted
events have separate limits. A mandatory event that cannot fit fails the whole
block without exposing partial device batches; optional refinement points may
coalesce deterministically. The renderer owns all scratch storage after
`prepare()` and performs no allocation in `process()`.

For latency-compensated source queries, call `project_schedule_ahead()` once
per prepared sink lead. The portable projector reconstructs one or two ranges
at the future timeline position, advances monotonic time across complete loop
cycles, and preserves callback-local event offsets; consumers must never render
the base window and post-shift its events. Use the per-device
`TrackAutomationRenderer::process()` view overload when device placements have
different leads. Its views must cover canonical device order exactly and all
projected snapshots must retain the same callback controls; each view pointer
must remain alive through `process()`. Projected snapshots are renderer-query
windows only. Graph `ProcessContext` still receives the unprojected master
transport and must never inherit projected control flags.

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
  range discontinuities project to `ProcessContext::transport_jump`. Preserve
  typed `Seek`, `LoopWrap`, and `LoopConfiguration` reasons when deriving new
  scheduling windows; natural wraps are recomputed at the projected boundary.
- Arrangement note events are compiled against the owning program's exact
  tempo map and ordered by sample, note-off before note-on, then clip/note ID.
  A renderer uses half-open sample ranges and never latches a callback size.
- Automation values are evaluated at the tempo map's canonical tick for each
  selected sample. Do not interpolate by sample fraction across tempo ramps.
  Each loop/seek/adoption range is reseeded, stopped blocks emit only when
  reseeding, and same-lane adoption requires a strictly newer generation.
- Attached automation compilation and rendering remain portable playback code.
  Mirror every new playback translation unit into the native target, the
  no-exceptions target, and both WAM/WebCLAP curated source lists; keep
  `web-timeline-source-closure` green. This proves wasm compilation only, not a
  JavaScript timeline API or host parameter delivery.
- Audio and note renderers must consume the same `TransportSnapshot` for a
  callback. The replay golden uses a varying schedule up to the transport's
  prepared `max_buffer_size`; never cache the first callback size in either
  renderer or bypass `MasterTransport`'s upper-bound rejection.
- `StableRendererShell`, `ArrangementAudioTrackRenderer`, and
  `ArrangementNoteRenderer` expose control-thread `reset()` for a successful
  quiesced sample-rate or maximum-block-size lifecycle change. Reset every
  bound renderer together after graph reprepare; note reset also clears active
  counts, pending flush/overflow state, current event buffers, and block index.
- Note rendering is a transport-tick MIDI lane. Do not lower it to an audio
  `CustomNodeType`; the host/embedded adapter routes its bounded MIDI output.
- `core/playback` must not include `pulp/format`, `pulp/host`, or `pulp/view`.
  `<pulp/format/playback_context_projection.hpp>` owns the one-way adapter.
  Keep `timeline-engine-dependency-floor` green; it allowlists source includes
  and CMake links for timebase, timeline (when present), and playback.
- `ArrangementAudioRenderer::process()` clears output, validates the complete
  zero/one-wrap snapshot, and mixes arrangement-selected tracks in stable
  PlaybackProgram order. It is immutable-input RT safe, wraps `ScopedNoAlloc`,
  and must remain covered by `rt_allocation_probe`. Mono duplicates on wider
  output, multichannel-to-mono averages, wider sources map by channel, and the
  engine does not clip or normalize deterministic float sums.

## Validation

Build and run `pulp-test-playback-automation-cursor`,
`pulp-test-playback-track-automation-program`,
`pulp-test-playback-track-automation-renderer`, `pulp-test-playback-program`,
`pulp-test-playback-schedule-ahead`, `pulp-test-playback-transport`, `pulp-test-timebase`, and
`pulp-test-transport-quantizer`, plus `pulp-test-playback-audio-renderer`. Keep loop-boundary, variable-block, ramp,
negative-preroll, extreme-position, SeqLock hammer, and RT-allocation cases.
When export/install wiring changes, also run the installed SDK consumer smoke.
Also build `timeline-program-threadless-no-exceptions-check`; it compiles the
program/compiler/executor/shell lane with `-fno-exceptions -fno-rtti` and the
threadless executor stub. Run the WASI SDK build when `/opt/wasi-sdk` is
available; the native compile-only gate remains mandatory when it is not.
Keep `pulp-test-timeline-replay-golden` green: it applies journaled gain, fade,
and note edits, replays from the checkpoint, and compares the audio/MIDI byte
stream with both the committed snapshot and the pinned fixture.
`web-timeline-source-closure` compares the native timebase, timeline, and
playback source lists with both curated production web ABI lists. Add a portable
engine translation unit to native, WAM, and WebCLAP ownership together.
