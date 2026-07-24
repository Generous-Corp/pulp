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

## Takes, comps, freeze, and capture

- A `TakeLane` owns recorded `Take` values and its comp segments. Selecting an
  active lane makes that comp the track's playback source.
- `TrackFreeze` selects a sealed media artifact and its render-plan identity.
  The original clips and device chain remain document state, but playback uses
  the selected freeze until it is cleared.
- `CaptureEngine` is the realtime recorder. Prepare its capacities off-thread,
  enqueue bounded commands, and drain completed take handles away from the
  callback.
- `materialize_midi_capture()` and the recording-commit helpers convert
  completed capture data into ordinary timeline commands. Submit those commands
  through `DocumentSession`; capture never mutates the project directly.

The application owns device I/O, media-file publication, and plugin/device
instantiation. The timeline owns editing intent and durable identity; playback
owns immutable compiled artifacts; capture owns bounded callback-time buffers.

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
