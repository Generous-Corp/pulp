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
a second playback-program model. `TimeConform::None` uses the existing bounded
native-rate source mapping after sample-rate conversion. `TimeConform::Resample`
uses bounded stateless varispeed: map each
rendered musical tick to the same fraction of the referenced source range and
derive the effective source step for anti-aliasing. Use the compiled tempo map's
analytic fractional sample-to-tick inverse for ordinary playback and precise
host ticks for host beat mapping; sample-fraction interpolation across a tempo
ramp is not musical phase. `TimeConform::Stretch` is compiled off the audio
thread: slice the source, fixed-SRC it into the compiled timeline-rate domain,
drive the finite stretcher with an analysis-boundary tempo schedule, and publish
only an immutable artifact with exactly the authored timeline frame count.
Use the scalar double finite builder for deterministic offline compilation,
then convert its exact result to the public float artifact in bounded blocks.
Document-tempo playback consumes that artifact 1:1. For live host-tempo
projection, prepare a complete `RealtimeStretchProgramRuntime` off the audio
thread and stream the artifact through its preallocated low-latency processor;
the audio callback may stretch but must never allocate, lock, or prepare DSP.
The runtime publishes one fixed causal latency for all parallel audio and MIDI
paths and resets coherently on transport/program epochs. Keep
source/tempo/algorithm semantic identity in the artifact cache key and document
revision/program generation in separate provenance.
Compiler work-block size is scheduling only and must change neither key nor
output. Never route `Stretch` through `Resample`, pad/trim a length mismatch, or
fall back to `None`.
Gain and anchor-native fade durations live on the immutable Clip. Missing,
mismatched, or over-capacity assets fail compilation instead of creating a
silent placeholder.
When sequence lowering flattens a complete nested media clip, preserve its
authored `TimeConform` value. Reject a nested source window that trims a
`Resample` or `Stretch` clip with `NestedSequenceUnsupported`; advancing a raw
source-frame offset is valid only for unconformed media and would corrupt the
authored phase until playback owns a conform-aware source-range mapping.

When host beat mapping intentionally makes musical material follow the host
tempo, keep absolute clips, take-comp segments, and frozen artifacts on
`TransportRange::timeline_sample_start`; those sources are sample-domain
content and must not inherit the beat projection. Carry precise fractional host
tick endpoints through every callback and nested `ProcessContext` projection;
integer `timeline_tick_*` fields remain compatibility metadata and must not
drive host-mapped interpolation, loop admission, note scheduling, automation
refinement, MIDI clock, or metronome enumeration. A precise host-mapping
rejection must not fall back to document-tempo placement. For musical audio,
derive the
effective source-position step per output frame. A converter prepared only for
the asset-rate/timeline-rate ratio cannot anti-alias faster host playback, so
prepare and share a per-MediaRef-range multiresolution audio pyramid off the
audio thread. Build it incrementally inside the compiler work budget, count it
against both converter-count and aggregate prepared-byte limits, including
persistent sinc tables and container storage, and seed unchanged programs back
into the cache. Clamp every pyramid level to the exact
referenced source range so neighboring asset frames cannot bleed into a clip.
Fixed-rate and variable-rate kernel construction are part of that same
incremental budget: initializing a converter must not synchronously populate
every sinc phase before yielding.
Use fixed-size, incrementally allocated prepared chunks whose persistent
footprint is computable; implementation-defined container bookkeeping cannot
sit outside the byte cap.
Each 2:1 stage must low-pass before decimation; select the coarsest level that
leaves a bounded residual step, then use its prebuilt reconstruction kernel. Do
not approximate extreme ratios by clamping a tiny cutoff onto a
fixed-width source-rate kernel: once the sinc support contains too few zero
crossings, normalization turns it into a short moving average and aliases
despite the nominal cutoff. The fixed asset-rate/timeline-rate path therefore
fails compilation beyond its honest kernel range, while host-tempo playback
uses the prepared pyramid to retain its wider bounded contract.

An active take lane replaces the track's arrangement source; zero
`active_take_lane_id` selects arrangement clips. The compiler lowers each
canonical comp selection to an `AudioClipRendererProgram` with
`SourceKind::TakeCompSegment` and a one-based lane ordinal. The typed origin
keeps repeated selections from one take distinct without inventing project
identities. Lower one selection per compile work unit, count arrangement
regions and comp selections against the same whole-program `max_clips`, and
require the take rate, asset metadata, and decoded audio rate to agree.
Inactive lanes remain document data and contribute no playback regions.

A selected `TrackFreeze` supersedes both the arrangement and active take comp
with one `AudioClipRendererProgram` whose `SourceKind` is `FrozenTrack` and
whose stable identity is the owning track. It is a sealed post-device artifact:
the compiler emits no authored clip/note events, ordered device placements, or
automation program for that track, while leaving all authored document state
intact for unfreeze. Count the artifact against the same whole-program
`max_clips`, validate its project asset, decoded audio, media range, and sample
rate exactly, and reject coordinate/SRC overflow before publication. A dirty
freeze/unfreeze edit must rebuild the track program; replay selects the sealed
asset and never re-renders it. Desktop graph binding therefore accepts no
device routes for a frozen track and rejects stale routes as unexpected,
preventing a post-device freeze from traversing the authored chain twice.

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
- Scrubbing is a transport mode, not a renderer feature. `begin_scrub()` /
  `scrub_to()` / `end_scrub()` make the transport emit repeated windows that
  restart on the latest posted anchor, so a dragged playhead is audible without
  a single line of scrub-aware code in any renderer: a window restart is
  structurally a loop wrap (reposition + `discontinuity` + block split), which
  the note and automation renderers already handle. Do not add a scrub branch to
  a renderer; make the transport produce the right ranges instead.
- The scrub anchor is **latched, not immediate**: a newly posted position takes
  effect at the next window boundary. That makes the grain rate the window
  length rather than the UI event rate — posting at 60 Hz against an immediate
  anchor would machine-gun sub-grain restarts. The window must be at least
  `max_buffer_size` (`begin_scrub` rejects shorter, and `begin_block` clamps
  anyway) so a block still spans at most two windows and the fixed two-range
  representation stays complete.
- **Scrubbing suspends loop wrapping.** A drag states a position directly, so
  the transport must not pull the window back to the loop start or make
  positions outside the loop unreachable. The loop is still reported in the
  snapshot (a UI keeps drawing it) and wrapping resumes on the first block after
  `end_scrub()`, which parks the playhead on the anchor the drag released on.
  This also keeps a loop wrap and a window restart from ever needing a third
  range in one block.
- While scrubbing, `is_playing` is true even when the musical transport is
  stopped — consumers that only care whether the playhead moves need no scrub
  branch — and `scrubbing` distinguishes the mode. Entering and leaving scrub
  set `reset_requested`; the window restarts in between deliberately do not,
  because they recur many times a second and `discontinuity` already describes
  them.
- Two existing discontinuity consumers inherit scrub behavior on purpose, and
  both are correct as-is: `CaptureEngine` cancels active takes on a
  non-loop-wrap jump, so scrubbing aborts a recording rather than splicing it,
  and `ExternalSyncOutput` emits a song-position/MTC update per window restart,
  so slaved gear chases the drag. A scrub block carries at most two ranges, the
  same as a loop wrap, so neither exceeds `max_messages_per_block`. Do not add a
  scrub branch to either; if the behavior needs to change, change what the
  transport publishes.
- `TempoSyncSource` is the backend-neutral session-tempo boundary. Its only
  virtual operation is the realtime `capture_audio_block()` mapping/command
  exchange. Backend enablement, peer discovery, and start/stop-sync policy do
  not belong on the interface; the desktop `AbletonLinkTempoSync` adapter owns
  those Link-specific controls.
- A configured `TempoSyncSource*` is non-owning and must outlive
  `MasterTransport`. It switches callers to the host-time `begin_block`
  overload. Its opaque `TempoSyncHostTime` is created by the source and tagged
  with that source's clock domain; a default token or a token from another
  source fails before capture. The timestamp names the first sample at the
  output boundary, so the audio-device layer must add output latency before
  entering playback. A missing host time, disabled backend, backend failure, or
  invalid mapping fails closed and never advances on the document clock.
- Joining an external tempo session is passive. `prepare()` does not broadcast
  `initial_position` or `initially_playing`; only later explicit `seek()`,
  `set_playing()`, or `set_tempo_sync_tempo()` calls become one-shot commands
  on the next audio block. Applied generations advance only after a valid
  capture, so a failed block retries the command rather than losing it.
- Session-tempo projection still obeys the fixed one-or-two-range contract,
  including one loop wrap and precise fractional host ticks. `begin_scrub()`
  rejects an active sync source: scrubbing owns a private repeated-window clock
  and cannot share authority with a network beat mapping. The audio-thread guard
  rejects any impossible mixed state defensively as well.
- Both document-tempo and session-tempo blocks publish through the same
  canonical block/range projection pipeline. Keep flags, meter anchoring,
  monotonic ticks, host mapping, and previous-state publication there; source
  paths should only derive their mode-specific projections.
- A tempo source must preserve the host-clock time at which its reported
  `is_playing` state becomes effective. `project_tempo_sync_playing()` applies a
  transition at or before the first sample and defers one inside or beyond the
  half-open block, because `TransportSnapshot::is_playing` is block-wide. Keep
  this quantization explicit; silently discarding the timestamp makes remote
  starts and stops early, while pretending to split them would contradict the
  snapshot consumed by renderers.
- Keep `tempo_sync.cpp` in `PulpPlaybackSources.cmake`, which mirrors it into
  native, threadless, WAM, and WebCLAP builds. Keep SDK-backed adapters such as
  `adapters/ableton_link.cpp` outside `core/playback/src/` and in a separate
  non-installed target; the source-closure gate treats every `src/*.cpp` as
  portable, so an SDK-backed translation unit there would be pulled toward the
  wasm lanes.
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
  and CMake links for timebase, timeline (when present), and playback. The link
  check reads `target_link_libraries` dependencies and skips the configured
  target plus target-defining commands, so a subsystem-local helper executable
  whose own name shares the module prefix (e.g. `pulp-timeline-schema-emit`) may
  link `pulp::timeline` without tripping the floor.
- A follow action's period is anchored to `LaunchHandle::last_start()` — the
  monotonic beat the launch RESOLVED to — never to the monotonic origin and
  never to the block that carried the Start. `FollowActionTimer` builds a
  `LaunchQuantize` whose phase is that beat and walks it with the same
  `next_launch_boundary()` / `resolve_launch_sample()` pair a launch uses, so
  the fire inherits the launch's sample accuracy across a loop wrap for free.
  Recovering the launch beat from a Start event's sample offset instead would
  round through the tempo map and lose that exactness.
- A test whose launch lands on a multiple of the follow period CANNOT tell a
  launch-anchored grid from an origin-anchored one — both produce the same
  boundaries, so re-anchoring to phase 0 keeps such a test green. Prove the
  anchoring with an OFF-grid launch (an immediate launch from a non-beat
  `initial_position`); only then does the fire sample separate the two.
- The compiler asks `clip_content_role()` what a clip contributes before it
  compiles anything, and that classifier visits `timeline::ClipContent` through
  `ClipContentCases` — an overload set with no generic fallback. Do not go back
  to testing alternatives inline with `holds_alternative` / `get_if`. A clip
  whose content kind the compiler does not recognize produces no audio program
  and no notes, and nothing anywhere reports it: the document is intact, the
  compile succeeds, and the track is silent. Routing every content decision
  through one exhaustive classifier turns that into a build failure at the point
  where somebody has to decide whether the new kind renders. `audio_renderer.cpp`
  carries the matching `static_assert` on the alternative count, because its
  "not a `MediaRef` means not audio" assumption lives there too.
- `ArrangementAudioRenderer::process()` clears output, validates the complete
  zero/one-wrap snapshot, and mixes arrangement-selected tracks in stable
  PlaybackProgram order. It is immutable-input RT safe, wraps `ScopedNoAlloc`,
  and must remain covered by `rt_allocation_probe`. Mono duplicates on wider
  output, multichannel-to-mono averages, wider sources map by channel, and the
  engine does not clip or normalize deterministic float sums.

### A per-pass decision splits across compile and render — put each half where its inputs are

Per-note playback modifiers (probability, pass condition, ratchet) are the
worked example. The split is not a style choice; each half sits where its inputs
exist:

- **Authored, pass-independent → compile time.** A ratchet count is a pure
  function of the content, so `program_compiler.cpp` lowers a ratcheted note into
  N on/off pairs that tile the authored span, with the last subdivision landing
  on the note's own end so repeats never drift. A subdivision that collapses to
  zero samples at the compiled tempo fails the compile rather than emitting an
  on with no off.
- **Pass-dependent → the renderer.** Probability and the pass condition cannot be
  decided at compile time without freezing every pass to one answer, so they are
  evaluated in `ArrangementNoteRenderer::process()` against a pass index.

The pass index is **transport-owned, never renderer-local**. Each
`TransportRange` carries `loop_pass_index`; the master transport and host
projector advance it at a wrap and re-anchor it on start, seek/jump, or loop
identity changes (including precise fractional host bounds). A renderer may be
created mid-playback, skip a callback, or fail a bounded output flush and still
observes the authoritative pass on its next range. Do not reconstruct the pass
from `MonotonicBeat`: its signed tick storage intentionally saturates at the
domain boundary.

Two properties make the gate safe to apply per event. The pass index is constant
across a range, because a wrap always starts a new range — so a note's on and its
off resolve against the same pass and the gate can never admit one without the
other. And the decision is a pure function of `(draw key, pass index)`, so no
draw state crosses blocks and evaluation order cannot change a result. Anything
seeded on the audio thread must have this shape: fold the seed and the identity
into one key at compile time, then mix it with the pass index in `process()`.

Side data a renderer needs per event goes in a **sparse table on `TrackProgram`
looked up by item id**, not a field on `NoteProgramEvent`. That struct is 40
bytes and the scale suite compiles ten million of them; a `std::uint32_t` index
would not fit the existing padding and would grow every event by eight bytes to
carry data almost no note has. An empty-span check makes the common case free.
Sorting such a table is real work, so it gets its own budgeted
`BudgetedStableMergeState` stage rather than a bare `std::sort` inside a compile
slice.

### Track mixer

- **Track mixer.** `TrackProgram::mixer()` carries the track's own
  `gain_linear`/`pan` with any lanes that automate them already resolved to
  borrowed `AutomationProgram` pointers. It is applied inside the clip
  accumulate in `audio_renderer_render.cpp`, so the whole-program mixdown and
  the per-track graph renderer stay in agreement — applying it in only one would
  break offline/live parity. A lane **supersedes** the authored constant rather
  than multiplying with it, and `TrackMixerProgram::transparent()` short-circuits
  an untouched track back onto the exact pre-mixer code path. Pan is a balance:
  it attenuates the opposite side, never boosts, is inert below two channels, and
  is exactly unity at centre.
- **Mixer lanes never reach device delivery.** `TrackAutomationRenderer` skips
  any lane whose `device_target()` is null, and so does the admission scan in
  `core/host/src/timeline_automation_delivery.cpp`. A mixer lane still lives in
  the track's `TrackAutomationProgram`; it just has no device to address.
- **One curve evaluator.** `select_automation_segment` and
  `evaluate_automation_segment` in `automation_program.cpp` are shared by the
  device-delivery cursor and `TrackMixerControlCursor`, so an automated fader and
  an automated plugin parameter cannot read the same curve differently.
  `TrackMixerControlCursor` is forward-only — `restart()` before revisiting an
  earlier position, which the render loop does per channel and per transport
  range.

## Validation

Build and run `pulp-test-playback-automation-cursor`,
`pulp-test-playback-track-automation-program`,
`pulp-test-playback-track-automation-renderer`, `pulp-test-playback-program`,
`pulp-test-playback-transport`, `pulp-test-timebase`, and
`pulp-test-transport-quantizer`, plus `pulp-test-playback-audio-renderer`
(which carries the track-mixer cases, including the proof that a gain lane moves
the rendered samples rather than merely existing in the document). Keep loop-boundary, variable-block, ramp,
negative-preroll, extreme-position, SeqLock hammer, and RT-allocation cases.
Track-freeze changes also require `pulp-test-timeline-graph-binding`: prove the
artifact routes directly after the authored chain, a stale device mapping is
rejected, and a dirty thaw restores arrangement/device compilation.

`pulp-test-playback-note-renderer` also fuzzes the no-stuck-notes property:
fixed-seed randomized seek/loop/play sequences over overlapping notes assert
the physical MIDI stream is a per-key on/off toggle (a note-on only for an idle
key, a note-off only for a sounding key), and a terminal stop-flush must leave
`has_active_notes()` false with every note-on matched by a note-off. Seeds are
hardcoded so a red is a real defect, not a flake; keep the non-vacuity witnesses
(notes held live across seeks and loop wraps) asserting above zero so the
all-clear cannot go vacuous. The toggle invariant and terminal balance are NOT
enough on their own — they are both structurally guaranteed regardless of the
seek/loop flush: `emit()` folds logical overlaps so the physical stream is a
clean per-key toggle even when a discontinuity strands a note, and the terminal
stop-flush always rebalances the counts. A stranded note is only observable
against an independent coverage oracle: a key may sound only while the playhead
sits inside the union of that key's compiled note extents, so a still-sounding
key whose playhead has moved past every extent is the stuck note. Keep that
oracle (checked at each playing block's last played sample, stuck-direction
only — a note whose onset precedes the new range is deliberately not chased, so
covered-but-silent is legal) when touching this proof; without it, deleting the
`range.discontinuity` flush in `note_renderer.cpp` leaves the fuzz green.

The same file carries the scrub counterpart, which reuses that oracle over
randomized `begin_scrub`/`scrub_to`/`end_scrub`/seek/play/loop sequences. Its
non-vacuity witnesses are scrub-specific — window restarts that happened while
notes were sounding, and restarts that split a block — because a scrub fuzz that
never rewinds the playhead under a live note proves nothing. Deleting the
`pending_discontinuity_` assignment in `start_scrub_window()` (transport.cpp)
must red both that fuzz and the deterministic
`a scrub window restart releases the notes it strands` case; if it does not, the
scrub coverage has gone vacuous.

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

`test/cmake/sampler_runtime_tests.cmake` also registers sampler Heritage
runtime tests. Those tests exercise `pulp::audio` profile/runtime behavior and
do not make Heritage profiles part of the immutable playback-program model;
keep that ownership boundary when extending the shared test inventory.

## Compile-context subscriptions and the exact dirty set

`compile_context_registry.hpp` is the invalidation half of the
compile-context subscription contract (the document/read half lives in
`core/timeline` — see the timeline skill). It exists because the compiler's
dirty set is exact rather than diffed: a renderer that reads a sequence-owned
context lane while compiling has no dirty item of its own when that lane
changes, so without a declaration it would render stale forever.

Three pieces, and the boundaries between them matter:

- `CompileContextRegistry` maps a content **schema type name** (the identity a
  `RegisteredContent` clip actually carries) to declared subscriptions. It
  refuses a duplicate type rather than overwriting — two renderers disagreeing
  about what a content kind reads would make invalidation depend on registration
  order. An unregistered type reads nothing, which is correct: no renderer
  compiles it, so there is no program that could go stale. Built-in content
  (media, notes, empty) is not registered and declares nothing, so the contract
  cannot change the invalidation of anything that predates it.
- `ContextSubscriberIndex::build()` is the kind → reader-track reverse index.
  Rebuild it when the document's **structure** changes; a context edit alone does
  not invalidate it, because editing a lane's contents does not change who reads
  it. It walks clips through the exhaustive `ClipContentCases` visitor, so a new
  `ClipContent` alternative stops the build here until someone decides whether it
  can subscribe.
- `resolve_dirty_tracks()` is the production translation from
  `timeline::DirtySet` to `DirtyTrackSet` (tests used to hand-build the latter).
  Its precision is documented per dirty-item shape in the header. Two shapes are
  deliberately conservative and should stay that way: an item with no owning
  sequence is project-scoped (tempo, meter, assets) and sets `all`, and a
  trackless item in this sequence that is not `DirtyFlags::Context`-flagged is a
  structural sequence edit and also sets `all`.

**Adding a `CompileContextKind` is a data change, with one trap.** Both
`ContextSubscriberIndex::build()` and the `CompileContextSubscriptions` bitset
loop over `[0, kCompileContextKindCount)`, so a new kind needs no new case in
either — but it does need `kCompileContextKindCount` bumped in lockstep with the
enum. Forget that and the new kind is never indexed, never dirtied, and every
test that only checks "my subscriber recompiled" still passes because the
subscriber recompiles for some other reason. The `static_assert` on the bitset
width catches only the ninth kind, not a stale count. Write the exactness test
so it names the readers of *each* kind separately: a per-sequence index and a
per-kind index are indistinguishable until two kinds have disjoint readers.

**Proving invalidation exactness.** `PlaybackProgram::find_track()` returns the
compiled `TrackProgram` the published program holds. The compiler reuses an
untouched track's program object outright, so an unchanged **pointer** is a
direct observation that a track was not recompiled, and a changed pointer that it
was. Assert on that, not on a proxy like a compile counter — and assert the
program generation actually advanced in the same test, or "unchanged pointer"
could just mean no compile happened at all. A dirty-set test that still passes
when the subscription is ignored and everything recompiles is vacuous; break the
resolution both ways (over-dirty and under-dirty) and confirm it goes red.

## Production mode and replay honesty

- `provider_production_declaration` / `track_production_declaration` /
  `program_reproducibility` (`production_class.hpp`) derive what a compiled
  program may claim about being replayed, rather than storing it on the program,
  so the claim cannot drift from what the compiler actually lowered. A render
  spanning several classes aggregates with `timeline::weakest`, never with the
  first or the strongest.
- **You cannot compile a `Launcher` or `ExternalInput` track today.**
  `plan_compile` rejects any `TrackCompilePolicy` whose provider is not exactly
  `Arrangement` with `available_mask == 1`, so a `PlaybackProgram` can only ever
  carry arrangement tracks even though `ProviderSelectorProgram` models three
  kinds and really does gate rendering. Unit-test per-provider behavior against a
  hand-built `ProviderSelectorProgram`; a test that tries to compile one gets
  `CompileErrorCode::InvalidRequest` and proves nothing.
- `BufferedContentSource` composes `audio::StreamingSampleSource` with a zero
  preload window, so every frame travels through the ring where it can be
  counted. Deadline mode treats a zero producer return as “not ready yet,” not
  permanent EOF: later pumps retry at the same frame or seek to a playhead that
  already counted the interval as starved. Count starvation against the
  *declared* frame count. Size the implicit ring for both the declared
  wall-clock lookahead and the largest audio callback, while
  `StreamingSampleSource` independently caps producer read-ahead at the
  declaration's horizon.
- A new `core/playback/src/*.cpp` is compiled by
  `timeline-program-threadless-no-exceptions-check` with `-fno-exceptions
  -fno-rtti` and `PULP_COMPILE_EXECUTOR_DISABLE_THREADS=1`, and swept into both
  wasm lanes by the closure gate. Anything that owns a `std::thread` or throws
  belongs in a header or a sibling module, not in `src/`.

## Dependency floor

`playback`'s floor is declared in `MODULE_FLOORS` in
`tools/scripts/timeline_engine_dependency_floor_check.py`, which scans both
`#include <pulp/<module>/...>` in every source file under `core/playback/` and
`target_link_libraries` in its `CMakeLists.txt`. Both axes must stay inside the
declared set, so reaching for a format, host, or view type fails the gate even
when the build would have linked.

The table holds every engine-adjacent module, not just playback, and the selftest
is generic over it. Adding a module there is how a new `core/` target gets the
same enforcement; it does not widen anyone else's floor.

### An editor view never links playback

`core/timeline_editor` carries a floor that deliberately excludes `playback`, and
the selftest asserts that pair by name in both the include and the link
direction. An editor learns where the playhead is through
`timeline_editor::SequencerUiHost`, whose implementation lives with whoever owns
audio — so a plugin that draws a piano roll over its own engine consumes the
editor without acquiring a transport.

That interface hands out `UiPlayhead` **by value**, and the reason is specific to
this module: `TransportSnapshot` borrows `const CompiledTempoMap*` from the
compiled program. That is correct for a block renderer, which consumes the
snapshot inside the callback that produced it, and unsafe for a view, which keeps
its copy across frames while the engine may adopt a different program underneath.
Never widen the UI-facing seam by passing a `TransportSnapshot` — project the
fields a view needs into values, as `UiPlayhead` does. `UiPlayhead::program_generation`
is what lets a view tell a stale reading from a live one without holding anything
a program swap can invalidate.
