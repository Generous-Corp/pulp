# Creative Timeline Engine SDK

The Creative Timeline Engine is usable as three layered C++ libraries from an
installed Pulp SDK. It does not require Pulp's UI, GPU renderer, plugin-format
adapters, SignalGraph, standalone shell, or plugin host.

For task-focused integration recipes, start with the
[Timeline cookbook](timeline-cookbook.md). The
[C++ API reference](../api-reference.md) documents individual public methods,
the [module reference](../reference/modules.md#timebase) describes subsystem
boundaries, the [example gallery](../examples/index.md#validation) points to
runnable Timeline examples, and the
[interchange matrix](../reference/interchange-matrix.md) states format
capabilities and losses.

## Configure an external project

Use the cookbook's
[library-selection recipe](timeline-cookbook.md#choose-libraries-for-an-external-consumer)
for the minimal `find_package()` and `target_link_libraries()` setup. The
lowercase aliases `pulp::timebase`, `pulp::timeline`, and `pulp::playback` are
also available. Components validate that the installed SDK contains each
requested target; the package still defines its complete set of installed
targets.

## Dependency boundary

The three requested engine targets produce this installed Pulp link closure:

| Library | Why it is present |
| --- | --- |
| `Pulp::timebase` | Editable and compiled tempo/meter maps |
| `Pulp::timeline` | Immutable project model, commands, persistence |
| `Pulp::playback` | Transport, compilation, note/audio/automation rendering |
| `Pulp::platform` | Runtime's portable platform primitives |
| `Pulp::runtime` | Results, queues, slots, files, and worker primitives |
| `Pulp::audio` | Audio buffers, decoded assets, and file support |
| `Pulp::midi` | MIDI event types and scheduling |
| `Pulp::state` | Parameter-event and state primitives used by audio |
| `Pulp::signal` | Header-only signal utilities used by audio |
| `Pulp::events` | Static implementation dependency of `Pulp::state` listener dispatch |

That is ten first-party Pulp libraries. `Pulp::events` is a static
`LINK_ONLY` dependency of `Pulp::state`; omitting it from the count would not
describe the installed target graph accurately.

The complete static-link closure also contains six exported implementation
archives: `Pulp::hwy`, `Pulp::mbedcrypto`, `Pulp::mbedx509`, `Pulp::mbedtls`,
`Pulp::everest`, and `Pulp::p256m`. They support runtime SIMD and cryptography;
they are not additional timeline APIs. The external consumer smoke audits all
sixteen targets and fails if canvas, view, GPU, graph, format, standalone, or
host targets enter the closure.

Plugin hosting is deliberately outside the engine. A desktop integration
adapts its own instrument/effect ports; the caller owns audio-device I/O.

## Optional DAWproject importer

Foreign-format import stays outside the dependency-minimal Timeline model.
Applications that ingest DAWproject `project.xml` files request and link the
dedicated importer:

```cmake
find_package(Pulp REQUIRED COMPONENTS dawproject-import)
target_link_libraries(my_timeline_app PRIVATE Pulp::dawproject-import)
```

## Optional DAWproject exporter

Writing DAWproject is a separate component from reading it, because the
importer is an installed `find_package` component and overloading it would
silently change what an existing consumer links:

```cmake
find_package(Pulp REQUIRED COMPONENTS dawproject-export)
target_link_libraries(my_timeline_app PRIVATE Pulp::dawproject-export)
```

Export is **bounded**, not Tier-1 support for the format. The writer emits the
same subset the importer reads — flat tracks, beats-timed clips, inline notes,
referenced audio, one tempo and one time signature — and everything else the
document holds is declared lost in the capability table.

There is no one-shot entry point. A plan is produced first, and `run_export`
refuses before touching the writer unless the caller has accepted each lost
concept by name:

```cpp
const auto plan = interchange::plan_export(project, interchange::Format::DawProject);
interchange::ExportOptions options;
options.accepted_losses = plan.required_consent();   // review these, do not paste blindly
auto artifacts = interchange::run_export(plan, options, dawproject::writer());
```

The former `dawproject::writer(project, options)` overload remains temporarily
available for source compatibility but is deprecated. Its `project` argument
is deliberately ignored: the writer serializes the snapshot owned by the
`ExportPlan`, so callers should migrate to `dawproject::writer(options)`.

There is deliberately no force flag: a pipeline pins the exact losses it
reviewed, so a loss kind introduced later stops that pipeline instead of riding
in on consent given for something else.

Two artifacts come back: `project.xml`, and `pulp-loss-manifest.json` naming
every concept that was dropped. The manifest travels inside the package rather
than scrolling past in a console, because an export whose losses are invisible
to whoever opens the file next is the failure this contract exists to prevent.
Packing those entries plus media into a `.dawproject` zip is the caller's step.

`pulp-loss-manifest.json` schema version 1 is shared by every interchange
adapter. Its root contains numeric `schema_version`, string `format`, boolean
`lossless`, and a `losses` array. Each loss contains string `concept`, `level`,
`class`, `count`, and `detail`, plus an `owners` array of decimal strings;
`degraded_to` is present only for a degradation. Counts and owner IDs are
decimal strings so 64-bit values survive JSON consumers without precision loss.
`owners` is a bounded diagnostic sample controlled by `CensusLimits`, while
`count` is the complete occurrence count. The filename is reserved to
`run_export()` and adapters cannot replace it. Changing this schema requires a
new `schema_version`; version 1 replaces the older unversioned DAWproject
manifest shape.

Each exported track carries a neutral `<Channel>` — unity volume, centre pan.
A receiving DAW registers the track from that element, and a file without one
is rejected. It is **not** an export of the document's authored mixer state,
which the manifest still reports as dropped.

`Pulp::dawproject-import` adds the importer implementation and its audio/WAV
dependency to the closure, and exposes
`pulp::timeline::import_dawproject_xml`. Applications that only create or
deserialize native Pulp projects do not link that importer implementation.

The importer intentionally accepts a bounded linear subset rather than silently
approximating an arbitrary DAW session:

- DAWproject major version 1, one tempo and meter, flat tracks, and
  beats-timed `<Notes>` or `<Audio>` clips are supported.
- Nested group tracks, `<Warps>`, seconds-timed lanes, unknown timeline
  constructs, and unsupported clips/tracks/notes fail the whole import.
- Audio imports require a caller-supplied package-media resolver. Rooted,
  drive-qualified, and parent-traversing paths are rejected; resolved WAV bytes
  are size-bounded, inspected, hashed, and retained only as sealed
  `MediaAsset`s with safe package-relative locator hints.
- `DawProjectImportLimits` bounds XML bytes, tracks, clips, notes, media assets,
  resolver calls, locator length, and per-call/total media bytes before
  importer-owned collections grow.

The import function consumes the `project.xml` entry, not the `.dawproject` ZIP
container itself. Package readers and resolver allocations remain application
responsibilities. See
`test/fixtures/timeline/dawproject/linear_subset.dawproject.xml` for a
representative supported document; malformed or out-of-subset input returns a
typed `DawProjectImportError` rather than a partial project.

That raw-member contract belongs to the low-level installed SDK. The higher-level
`pulp seq` and Timeline MCP interchange operations own bounded package I/O:
they import a standard `.dawproject` ZIP, require a safe root `project.xml`,
resolve only safe package-relative media, and export a standard `.dawproject`
ZIP containing the XML, manifest, and referenced media.

## Optional Standard MIDI File interop

Standard MIDI File (SMF) import and export live in their own target for the same
reason DAWproject import does — foreign-format I/O stays out of the
dependency-minimal Timeline model:

```cmake
find_package(Pulp REQUIRED COMPONENTS smf-interop)
target_link_libraries(my_timeline_app PRIVATE Pulp::smf-interop)
```

`Pulp::smf-interop` exposes `pulp::timeline::import_smf` and
`pulp::timeline::export_smf` from `<pulp/timeline/smf.hpp>`.

For the same plan/consent contract as DAWproject export, request the separate
adapter and include `<pulp/smf/interchange.hpp>`:

```cmake
find_package(Pulp REQUIRED COMPONENTS smf-interchange)
target_link_libraries(my_timeline_app PRIVATE Pulp::smf-interchange)
```

```cpp
const auto plan = interchange::plan_export(project, interchange::Format::Smf);
interchange::ExportOptions options;
options.accepted_losses = plan.required_consent();   // review these, do not paste blindly
auto artifacts = interchange::run_export(plan, options, smf::writer());
```

The format-bound writer can only be invoked by `run_export`, and the plan owns
the project snapshot it measured. On success, `project.mid` and the centrally
generated `pulp-loss-manifest.json` are returned. The adapter may omit only the
concepts named by consent, strips consented note modifiers, and represents a
consented continuous tempo ramp as Set Tempo steps at authored tempo points.

Conversion runs entirely in the musical domain. An SMF carries its own timebase
— a header division in ticks per quarter note plus Set Tempo and Time Signature
meta-events — so import scales those ticks onto the canonical
`timebase::kTicksPerQuarter` grid and turns the meta-events into the project's
`TempoMap` and `MeterMap`. Nothing is flattened to seconds in either direction,
so a tempo change mid-file survives the round trip as a tempo point.

- Tick scaling is exact for every position when the division divides
  `kTicksPerQuarter` (96, 120, 192, 240, 480, 960, …; not 384 or 1920).
  Otherwise import rounds to the nearest canonical tick and reports both
  `exact_tick_conversion` and the error bound. Export is exact by default: a
  canonical tick the requested division cannot represent is an error unless
  `allow_lossy_tick_rounding` is set.
- Import accepts format 0 and 1 with a metrical division, `MTrk` chunks only,
  Note On/Note Off (including the zero-velocity Note On that means Note Off),
  and the Set Tempo, Time Signature, Sequence/Track Name, and End of Track meta
  events. SMPTE divisions, format 2, unknown chunks, malformed running status,
  unbalanced notes, zero-length notes, and any other event fail the whole
  import. `SmfUnsupportedEventPolicy::IgnoreNonNote` is the caller's explicit
  opt-in to discard non-note channel messages, system-exclusive blocks, and
  out-of-subset meta events.
- `SmfImportLimits` bounds file bytes, tracks, events, notes, simultaneously
  sounding notes, tempo and meter points, meta payload bytes, track-name bytes,
  and the absolute tick ceiling before the corresponding state grows.
- Raw `export_smf` writes a format-1 file whose track 0 is a conductor track carrying the
  tempo and meter maps. Note velocity is scaled to the 7-bit MIDI domain; a
  velocity that would scale to zero is rejected rather than rewritten, because
  a zero-velocity Note On reads as a Note Off. A tempo ramp, a sample-anchored
  clip, and registered, opaque, nested-sequence, or media content are errors,
  not approximations. Empty clips are silently absent from the raw event codec;
  use the census-backed adapter when that loss must require consent.
- Round trips through a dividing division preserve note start, duration, pitch,
  channel, and 7-bit-representable velocity exactly. Tempo returns within the
  Set Tempo event's whole-microsecond resolution.

## Optional plugin-format adapter

`Pulp::sequence` is an exported integration layer for applications that need to
present a compiled timeline as a VST3, AU, or CLAP processor. It is not part of
the engine-only dependency closure above:

```cmake
find_package(Pulp REQUIRED COMPONENTS sequence)
target_link_libraries(my_sequence_plugin PRIVATE Pulp::sequence)
```

`pulp::sequence::SequenceProcessor` adapts a caller-owned
`PlaybackProgramStore` to `pulp::format::Processor`. It projects host transport
into the timeline clock, executes the compiled track graph, and emits rendered
audio and MIDI without owning the project, compiler, media resolver, plugin
host, editor, or device I/O. The caller must publish a compatible immutable
program before processing and rebuild it away from the audio thread when the
document changes.

Choose `Pulp::timebase`, `Pulp::timeline`, and `Pulp::playback` for an editor,
standalone application, or custom host integration. Choose `Pulp::sequence`
only at the plugin-format boundary; it intentionally adds the heavier
`Pulp::format`, `Pulp::graph`, and `Pulp::state` closure required by a
format-facing processor.

## Ownership and state flow

Timeline applications move immutable values through a small set of owners:

1. `Project` is the canonical document snapshot. Model values do not mutate in
   place.
2. `DocumentSession` owns the current snapshot, revision, command journal, and
   undo/redo state. A `WriterToken` supplies ordered transaction and command
   identities.
3. `JournalSink` acknowledges a complete transaction only after it is durable.
   Native applications can open a `FileJournal` and restore the returned
   checkpoint and revision into a session.
4. `PlaybackProgramCompiler` lowers one immutable snapshot plus its resolved
   media into a `PlaybackProgramStore`. Compilation and media resolution stay
   off the audio thread.
5. `MasterTransport` creates each callback's `TransportSnapshot`. Renderers read
   one pinned playback-program block and that transport snapshot without
   allocating.

The cookbook provides the compile-backed
[transaction](timeline-cookbook.md#submit-an-optimistic-transaction-and-undo-it),
[journal recovery](timeline-cookbook.md#open-or-restore-a-durable-session), and
[compile/publish/render](timeline-cookbook.md#compile-publish-and-render)
recipes for that ownership flow.

### Audio clip time-conform intent

`Clip::time_conform()` records how authored media is intended to adapt when a
musical clip's duration and its source-media duration differ:

- `TimeConform::None` is the default and preserves the existing, unconformed
  behavior.
- `TimeConform::Resample` requests varispeed, coupling duration and pitch.
- `TimeConform::Stretch` requests tempo-preserving time stretch.

Pass the intent as the final argument to `Clip::create()` or
derive a new immutable snapshot with `with_time_conform()`. `Resample` and
`Stretch` are valid only for musical clips whose content is a `MediaRef`;
absolute clips and non-media content reject non-default intent with
`InvalidTimeConform`. Clip schema v2 persists the required values as `none`,
`resample`, and `stretch`; v1 clips load as `None`, and release downgrade to v1
refuses an authored non-default value rather than discarding it.

Playback consumes `Resample` as bounded realtime varispeed: source phase spans
the clip's musical tick interval, so tempo ramps and precise host beat mapping
change duration and pitch together. `None` retains native-rate playback.
`Stretch` compiles off the audio thread. The compiler takes the exact slice from
the decoded asset pool, converts it to the compiled timeline sample rate,
applies the authored tempo schedule at stretch-analysis boundaries, and
publishes an immutable audio artifact with exactly the clip's compiled timeline
frame count. On the document clock the renderer consumes that artifact 1:1.
With a precise host-beat mapping, `TimelineGraphPlaybackBinding` and
`SequenceProcessor` prepare a program-wide live Stretch runtime off the audio
thread. It streams the immutable artifact through a bounded causal stretcher,
reports one fixed latency for graph compensation, and delays the track's
conventional audio and mixer automation by that same amount. Publication is
object-, generation-, and tempo-map-exact. Identity changes and discontinuities
are reported explicitly while a charged FIFO preserves the old segment through
the delayed cut and a bounded finalization hands off to the reset stream.
Missing or stale state, impossible ratios, backpressure, and underflow fail
closed with distinct status rather than falling back to `None` or `Resample`.
Host-mapped Stretch scrubbing is explicitly unsupported. Capacity, ratio, or
exact-length failures in the document-clock artifact still fail compilation.

### Reusing and diverging sequences

A musical clip may contain `SequenceRef{sequence_id, source_start}` instead of
notes or media. The clip places the referenced sequence's source window at the
clip start; multiple clips can share one sequence. Project construction and
command reduction reject missing targets, cycles, and nesting deeper than
eight reference edges.

Edits to a shared sequence intentionally affect every placement. Before a
placement-specific edit, call `build_diverge_transaction()` with the reference
clip's `ItemLocation` and two command IDs allocated from the session's
`WriterToken`. It allocates a complete clone from `next_item_id` and returns one
atomic `CloneSequence` plus `SetClipSequenceRef` transaction, so journal replay
and undo reproduce the exact copy-on-edit boundary without synthesizing or
reusing writer-scoped command IDs.

`PlaybackProgramCompiler` expands supported child notes and audio away from the
audio thread. Stage 1 fails closed when a child contains device processing,
automation, takes, freeze/record state, absolute clips, or when a reference has
gain/fades. A source window that cuts through a child audio fade also fails
closed. Complete nested media clips retain their authored time-conform intent;
a source window that trims a `Resample` or `Stretch` clip fails with
`NestedSequenceUnsupported` until playback can map that partial conforming
source range without changing its authored phase.
Set `ProgramCompileRequest::sample_rate` to the rate you will render the
compiled program at. It is required, and `submit()` rejects a request whose
rate is unset or disagrees with `tempo_map->sample_rate()` with
`CompileErrorCode::InvalidRequest`. The tempo map stays authoritative — its
segments already carry frame anchors computed at its own rate, so the field
cannot override it — but stating the rate is what turns "compiled against 48
kHz, rendered at 44.1 kHz" from a silent frame-position error into a rejected
submission. The comparison normalizes, so `{96'000, 2}` and `{48'000, 1}` are
the same rate.

Set
`ProgramCompileRequest::max_expanded_note_events` to bound note expansion and
`ProgramCompileRequest::max_expanded_clips` to bound total clip materialization
and reference traversal, including charges carried by reused track programs.
`AudioRendererLimits::max_clips` remains the separate ceiling for compiled audio
regions. When compiling incrementally, build and retain one snapshot-scoped
`CompileInvalidationIndex` from the project, root sequence, and
`CompileContextRegistry`, then pass each transaction dirty set through
`resolve_dirty_tracks()`. The bundled index combines direct edits, transitive
sequence dependencies, and nested context readers so every affected root track
is rebuilt. Resolution compares an O(1) immutable Project structure token plus
the root identity and registry generation; it fails closed to a full recompile
when any of them is stale or mismatched. Rebuild the index after a relevant
structural edit or registry declaration. Context lane contents, ordinary
note/audio edits, freeze selection, and active-take selection preserve the
structure token.

`pulp seq apply`, `pulp seq explain`, and `pulp render` expose the same
load/edit/compile/render path for headless workflows. Their source-tree
CLI/MCP facade uses `pulp::tools::timeline::ProjectSource` to distinguish
inline JSON from native file paths; that tooling facade is not part of the
installed SDK. Installed embedders should deserialize through
`pulp::timeline`, compile through `pulp::playback`, and render through the
public playback program APIs described above.

## Preview and commit pure note transforms

`NoteTransformRegistry` registers control-thread, pure note functions by
`SchemaIdentity`. A function receives the clip's immutable note span, canonical
parameter JSON, and an explicit seed, and returns a zero-to-many note array.
Preparation requires an object parameter payload, parses it under the
registry's 1 MiB bound, and canonicalizes it before invoking the function.
Returning an input note's `ItemId` preserves that identity; returning an invalid
ID asks the engine to allocate a fresh one. Foreign and duplicate output IDs,
expected plus output note arrays exceeding the five-million-note durable-command
quota, invalid notes, missing clips, and non-note clips fail closed.

An `ApplyNoteTransform` is a typed preparation request, not a journal entry:

```cpp
auto preview = transforms.preview(
    session->current(), writer,
    pulp::timeline::ApplyNoteTransform{
        .sequence_id = sequence_id,
        .track_id = track_id,
        .clip_id = clip_id,
        .transform = {"example.note.octave_echo", 1},
        .canonical_params_json = R"({"interval":12})",
        .seed = 42,
    });

// Render or inspect preview.value().snapshot without changing the session.
auto committed =
    session->submit(writer, std::move(preview.value().transaction));
```

Preparation invokes extension code exactly once and lowers its result to an
ordinary `ReplaceNoteContent` transaction. The preview snapshot is the result
of reducing that exact transaction without publishing it. If another edit
lands first, submission rejects the preview as stale; it never reruns the
transform with a different input. The durable journal therefore contains only
canonical expected/replacement note arrays, and undo/redo use ordinary inverse
commands with the identity directory's tombstone rules.

## Scrubbing the playhead

Dragging the playhead is audible through `MasterTransport` itself, not through a
renderer. Scrub mode makes the transport emit short repeated windows that start
at the latest posted position:

```cpp
// Mouse down on the ruler: 2048 frames is roughly a 43 ms grain at 48 kHz.
transport.begin_scrub(2048, position_ticks);

// Every mouse move during the drag.
transport.scrub_to(new_position_ticks);

// Mouse up.
transport.end_scrub();
```

The rules that matter when wiring a UI to it:

- The window length must be at least the transport's `max_buffer_size`;
  `begin_scrub()` returns `ScrubWindowTooShortForMaximumBlock` otherwise.
- A position posted by `scrub_to()` is latched and takes effect at the next
  window boundary, so the grain rate is the window length rather than the rate
  your UI emits mouse moves at. Call it as often as you like.
- `TransportSnapshot::is_playing` is true while scrubbing even when the musical
  transport is stopped, and `TransportSnapshot::scrubbing` distinguishes the
  mode. Each window restart arrives as a range discontinuity, which is the same
  signal a loop wrap produces — renderers need no scrub-specific handling, and
  notes left sounding by a restart are released at the boundary.
- **Scrubbing suspends loop wrapping.** A drag is a direct statement of
  position, so the transport will not pull the audible window back to the loop
  start, and positions outside the loop stay reachable. The loop is still
  reported in the snapshot so a UI keeps drawing it, and wrapping resumes on the
  first block after `end_scrub()`, which parks the playhead on the position the
  drag was released at.
- A note whose onset precedes a window is not retriggered, matching the seek
  behavior of `ArrangementNoteRenderer`.
- Scrubbing reaches the same consumers a seek does: an in-progress capture take
  is cancelled, and external MIDI sync emits a song-position update per window,
  so slaved gear chases the drag.

## One typed edit through CLI and MCP

CLI and MCP consume the same generated schema and versioned command envelopes.
They edit canonical snapshots rather than owning realtime device I/O,
capture buffers, plugin instances, or a durable `FileJournal` session. Use the
cookbook's [CLI/MCP recipe](timeline-cookbook.md#apply-the-same-edit-through-cli-or-mcp)
for the exact commands and operation names.

## Takes, comps, freeze, and capture

The document types live in `<pulp/timeline/model.hpp>` and their mutations in
`<pulp/timeline/command.hpp>`. A `TakeLane` owns recorded `Take` values and an
ordered non-overlapping comp selection. `SetActiveTakeLane` chooses that comp as
the track source; zero selects the original arrangement. Removing an active lane
or a take referenced by the comp fails closed.

`TrackFreeze` selects a sealed media artifact plus a render-plan content hash.
Publish a freeze in one transaction ordered as `CreateAsset` followed by
`SetTrackFreeze`. The authored clips, takes, automation, and device chain stay in
the document for thaw; playback merely selects the frozen artifact. Clear the
freeze before removing its asset. Replay never re-renders a freeze.

A track also carries a `TrackMixer` — `gain_linear` and a `pan` balance in
`[-1, 1]` — replaced with `SetTrackMixer` under an exact optimistic gate. An
`AutomationLane` may target one of those controls with `TrackMixerTarget` instead
of a device parameter; such a lane references no device placement and supersedes
the authored constant while it plays. Both values are refused outside their
range, including a NaN, so a document can never hold a level that has no meaning.
The desktop graph binding applies this mixer after hosted devices. For a device
chain with a nontransparent mixer, set `post_device_audio_source` and
`post_mixer_audio_destination` on `TimelineTrackGraphRoute`; the binding
transactionally replaces the direct device-to-bus edge and restores it when the
route is removed. Adoption fails closed when that post-device route is absent.

The realtime recorder is `<pulp/playback/capture_engine.hpp>`. It owns bounded,
preallocated callback-time capture slots; completed handles remain immutable
until explicit release. `recording_commit.hpp` seals audio into ordinary asset
and take commands, while `midi_capture_materializer.hpp` maps captured MIDI
through the exact capture-rate tempo map. Capture never mutates the project or
journal directly. Follow the cookbook's
[capture-to-take recipe](timeline-cookbook.md#capture-audio-into-a-take) for the
required ordering and failure checks.

The application owns device I/O, media-file publication, and plugin/device
instantiation. The timeline owns editing intent and durable identity; playback
owns immutable compiled artifacts; capture owns bounded callback-time buffers.

## Durable journals

Include `<pulp/timeline/file_journal.hpp>` for native crash-consistent sessions.
`FileJournal::open()` returns the sink, exact checkpoint and revision, whether
existing state was recovered, and whether it repaired a torn trailing frame.
The revision is zero for a new journal and nonzero after recovered commits.
Restore the recovered checkpoint/revision with `DocumentSession::restore()`, or
create a new session with the fallback checkpoint and sink. The session retains
shared ownership of the sink. A transaction is not published until the sink
reports its whole frame durable.

Checkpoint only a revision the application has durably acknowledged. A sink
error is ambiguous—it may have reached storage—so the session rejects later
durable writes instead of guessing. Recovery discards only a torn final frame
and fails on earlier corruption. Symlink aliases share one lock identity, while
multiply linked journal files are rejected because atomic replacement cannot
preserve their identity.

See the cookbook's
[open-or-restore recipe](timeline-cookbook.md#open-or-restore-a-durable-session)
for the capability branch and checkpoint call-site rules.

## Sample-rate conversion

Audio clips, take-comp segments, and frozen tracks may use a different sample
rate from the prepared tempo map. Playback compiles one shared 64-tap,
512-phase Kaiser-windowed sinc table for each distinct source/target rate pair.
That allocation happens during program compilation; rendering only reads the
immutable table. `AudioRendererLimits::max_sample_rate_converters` bounds the
number of distinct tables (64 by default), and compilation rejects excess
rates. Equal-rate audio bypasses the converter and retains its exact sample
path.

The deterministic offline quality gate covers both directions. For 96→48 kHz
conversion, a 20 kHz passband tone measures within 0.1 dB and a 30 kHz
stopband tone must fold below −60 dB. A deliberately unfiltered linear
decimator is the negative control and must expose that alias above −1 dB. For
44.1→48 kHz conversion, an 18 kHz tone must remain within 0.1 dB with residual
energy below −70 dB. Those named thresholds are the portable contract.

## Peek before loading

Project browsers and background media resolvers should inspect a snapshot with
`peek_project_summary()` before constructing an editable document:

```cpp
#include <pulp/timeline/serialize.hpp>

auto summary = pulp::timeline::peek_project_summary(snapshot_json, registry);
if (!summary)
    return report(summary.error());

show_project(summary->name, summary->counts.tracks, summary->counts.clips);
```

The peek scans the complete JSON under the same depth, input-size, and authored
collection quotas as structural deserialization preflight, so malformed or
oversized structural arrays still fail closed. It decodes only the four root
scalar values needed for the summary and does not build the generic JSON DOM,
identity tree, clips, notes, or automation model.

Use `deserialize_project()` only when the project must become editable. Media
references may remain unresolved at that point; asset resolution belongs on a
background path.

## External MIDI synchronization

`pulp/playback/external_sync.hpp` keeps MIDI device I/O outside the engine while
providing the timing machinery needed by an integration:

- `MtcChaser` decodes coherent MIDI Time Code quarter-frame cycles and
  universal-realtime full-frame locate messages without allocating.
- `ExternalSyncOutput` projects a `TransportSnapshot` into sample-offset MIDI
  Clock (24 PPQN), Song Position Pointer/start/continue/stop, MTC quarter-frame,
  and full-frame locate messages.
- MTC conversion covers 24, 25, 29.97 drop-frame, and 30 fps. Invalid
  drop-frame labels fail closed.

Reserve the destination `MidiBuffer` for the worst-case block before entering
the audio callback, enable its realtime capacity limit, call
`ExternalSyncOutput::process()`, then stable-sort the combined MIDI output at
the adapter boundary. An `OutputOverflow` result means at least one sync
message was dropped and must be surfaced rather than hidden.
`ExternalSyncOutputConfig::max_messages_per_block` also caps work when a caller
forgets to capacity-limit its buffer; keep that limit sized to the integration's
worst supported tempo, sample rate, and callback size.

The deterministic software suite verifies conversion, chase lock and
discontinuity behavior, callback-partition invariance, and exact event
placement. A physical loopback remains opt-in because its acceptance
tolerances must be fixed before collecting the trace. Put those user-approved
numbers in a spec:

```json
{
  "schema": "pulp.timeline-sync-soak-spec.v1",
  "fixed_at": "2030-01-01T00:00:00Z",
  "min_duration_seconds": 3600,
  "max_abs_offset_samples": 0,
  "max_drift_ppm": 0,
  "min_points_per_stream": 1000
}
```

The zero values above are placeholders and intentionally invalid; replace them
with the agreed limits before the run. Capture reference/observed sample pairs
as:

```json
{
  "schema": "pulp.timeline-sync-soak-trace.v1",
  "captured_at": "2030-01-02T00:00:00Z",
  "sample_rate": 48000,
  "points": [
    {"stream": "midi_clock", "expected_sample": 0, "observed_sample": 0},
    {"stream": "midi_clock", "expected_sample": 48000, "observed_sample": 48000},
    {"stream": "mtc", "expected_sample": 0, "observed_sample": 0},
    {"stream": "mtc", "expected_sample": 48000, "observed_sample": 48000}
  ]
}
```

Then run the non-gating hardware proof explicitly:

```sh
PULP_TIMELINE_SYNC_SOAK_SPEC=/path/to/spec.json \
PULP_TIMELINE_SYNC_SOAK_TRACE=/path/to/trace.json \
ctest --test-dir build -R '^timeline-sync-hardware-soak$' --output-on-failure
```

Without both files, CTest reports the hardware proof as a loud skip. Providing
only one file or a malformed/unfixed spec is a failure, not a skip.
