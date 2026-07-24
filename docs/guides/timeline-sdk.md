# Creative Timeline Engine SDK

The Creative Timeline Engine is usable as three layered C++ libraries from an
installed Pulp SDK. It does not require Pulp's UI, GPU renderer, plugin-format
adapters, SignalGraph, standalone shell, or plugin host.

## Configure an external project

```cmake
cmake_minimum_required(VERSION 3.24)
project(MyTimelineApp LANGUAGES CXX)

find_package(Pulp REQUIRED COMPONENTS timebase timeline playback)

add_executable(my_timeline_app main.cpp)
target_link_libraries(my_timeline_app PRIVATE
    Pulp::timebase
    Pulp::timeline
    Pulp::playback
)
```

The lowercase aliases `pulp::timebase`, `pulp::timeline`, and
`pulp::playback` are also available. Components validate that the installed SDK
contains each requested target; the package still defines its complete set of
installed targets.

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

The editing portion of that flow is intentionally explicit:

```cpp
auto registry = pulp::timeline::make_builtin_timeline_registry();
auto decoded = pulp::timeline::deserialize_project(project_json, registry.value());
auto session = pulp::timeline::DocumentSession::create(std::move(decoded).value());
auto writer = session.value()->register_writer();

pulp::timeline::Transaction edit;
edit.id = writer.value().allocate_transaction_id();
edit.expected_revision = session.value()->revision();
edit.commands.push_back({
    writer.value().allocate_command_id(),
    pulp::timeline::SetRecordArm{{2}, {3}, false, true},
});

auto committed = session.value()->submit(writer.value(), std::move(edit));
if (committed) {
    auto snapshot = committed.value().snapshot;
    auto revision = committed.value().revision;
    auto dirty = committed.value().dirty;
    // Send these immutable values to the background playback compiler.
}
```

For native crash-consistent storage, call `FileJournal::open()` first. Use
`DocumentSession::restore()` when it recovered an existing file, or
`DocumentSession::create()` with its sink for a new file. Do not write the
project beside the session independently; the journal sink is the durability
boundary.

`pulp seq apply`, `pulp seq explain`, and `pulp render` expose the same
load/edit/compile/render path for headless workflows. Their source-tree
CLI/MCP facade uses `pulp::tools::timeline::ProjectSource` to distinguish
inline JSON from native file paths; that tooling facade is not part of the
installed SDK. Installed embedders should deserialize through
`pulp::timeline`, compile through `pulp::playback`, and render through the
public playback program APIs described above.

## One typed edit through CLI and MCP

Start by asking the installed CLI for the generated schema, then validate the
source before editing:

```bash
pulp seq schema > timeline-schema.json
pulp seq validate song.pulpseq.json
```

Commands are versioned envelopes. For example, this `commands.json` arms track
`6` in sequence `5` only if it is currently unarmed:

```json
[
  {
    "data": {
      "expected": false,
      "replacement": true,
      "sequence_id": "5",
      "track_id": "6"
    },
    "type_name": "pulp.timeline.command.set_record_arm",
    "version": 1
  }
]
```

Apply the complete batch transactionally, then inspect and render the result:

```bash
pulp seq apply song.pulpseq.json commands.json --out armed.pulpseq.json
pulp seq validate armed.pulpseq.json
pulp seq explain armed.pulpseq.json --sample-rate 48000
pulp render armed.pulpseq.json --out armed.wav --sample-rate 48000
```

The equivalent agent flow calls `pulp_timeline_project_open`, passes the same
envelope array to `pulp_timeline_command_apply`, then calls
`pulp_timeline_validate`, `pulp_timeline_explain`, and optionally
`pulp_timeline_render`. MCP accepts the project as a path or inline canonical
JSON; its `commands` argument is the JSON array itself, not a filename. The
generated schema is the source of truth for document and command shapes.

These headless surfaces edit existing canonical projects. Realtime device I/O,
capture-buffer ownership, plugin instantiation, and durable `FileJournal`
sessions are embedding APIs, not hidden CLI or MCP operations.

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

The realtime recorder is `<pulp/playback/capture_engine.hpp>`:

1. Build a `CaptureEngineConfig` with explicit track, block, take-frame,
   take-slot, MIDI-event, and total preallocation limits, then call `prepare()`
   away from the callback.
2. Enqueue `Start`, `Stop`, `Cancel`, and `ReleaseTake` commands from the
   control side. The callback calls `process()` with the same
   `TransportSnapshot` used for playback.
3. Drain `CaptureEvent`s away from the callback. A completed
   `CaptureTakeHandle` remains immutable until `ReleaseTake`; copy its audio or
   MIDI before releasing it. Queue drops and capacity failures are observable
   in `CaptureEngineStats`.
4. Use `<pulp/playback/recording_commit.hpp>` to
   `seal_recording_take()` (or `seal_retrospective_take()`) into WAV bytes, a
   content-hashed asset, a take, and ordered `CreateAsset`/take commands. Use
   `<pulp/playback/midi_capture_materializer.hpp>` to
   `materialize_midi_capture()` against the exact capture-rate tempo map.
5. Publish those ordinary commands through `DocumentSession`, then publish the
   media bytes through application-owned storage. Capture never mutates the
   project or journal directly.

The application owns device I/O, media-file publication, and plugin/device
instantiation. The timeline owns editing intent and durable identity; playback
owns immutable compiled artifacts; capture owns bounded callback-time buffers.

## Durable journals

Include `<pulp/timeline/file_journal.hpp>` for native crash-consistent sessions.
`FileJournal::open()` returns the sink, recovered checkpoint, nonzero revision,
and whether it repaired a torn trailing frame. Restore that exact
checkpoint/revision with `DocumentSession::restore()`, or create a new session
with the sink. A transaction is not published until the sink reports its whole
frame durable.

Checkpoint only a revision the application has durably acknowledged. A sink
error is ambiguous—it may have reached storage—so the session rejects later
durable writes instead of guessing. Recovery discards only a torn final frame
and fails on earlier corruption. Symlink aliases share one lock identity, while
multiply linked journal files are rejected because atomic replacement cannot
preserve their identity.

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
