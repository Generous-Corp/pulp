# Timeline cookbook

This cookbook is the task-oriented companion to the
[Creative Timeline Engine SDK guide](timeline-sdk.md). Use the guide for
ownership, persistence, and playback contracts; use these recipes when wiring a
concrete integration.

See also:

- [C++ API reference](../api-reference.md) for public types and methods
- [Timebase, Timeline, Playback, and Sequence modules](../reference/modules.md#timebase)
- [Timeline engine examples](../examples/index.md#validation)
- [Interchange capability matrix](../reference/interchange-matrix.md)

The substantial C++ paths in this page are compiled by the installed-consumer
fixture in
[`examples/timeline-sdk-consumer/cookbook.cpp`](../../examples/timeline-sdk-consumer/cookbook.cpp).
Small names such as `report()` stand for application-specific error handling.

All model, edit, compile, import, and materialization work below belongs on a
control or worker thread. Only APIs whose declarations carry an audio-callback
RT-safety class belong in the callback.

## Choose libraries for an external consumer

Start with the smallest installed target set that owns the work:

| Task | Component and target |
| --- | --- |
| Musical/sample time and tempo or meter maps | `timebase`, `Pulp::timebase` |
| Immutable projects, commands, sessions, and native persistence | `timeline`, `Pulp::timeline` |
| Stable-root project publication with no-replace content-addressed blobs and unreachable unpublished staging | `project-package`, `Pulp::project-package` |
| Compilation, transport, rendering, launch timing, and capture | `playback`, `Pulp::playback` |
| Plugin-format-facing playback processor | `sequence`, `Pulp::sequence` |
| DAWproject `project.xml` import | `dawproject-import`, `Pulp::dawproject-import` |
| Standard MIDI File import/export | `smf-interop`, `Pulp::smf-interop` |
| Format-neutral export census and loss planning | `interchange`, `Pulp::interchange` |

An editor or standalone engine normally begins with:

```cmake
find_package(Pulp REQUIRED COMPONENTS timebase timeline playback)

target_link_libraries(my_timeline_app PRIVATE
    Pulp::timebase
    Pulp::timeline
    Pulp::playback
)
```

The installed-consumer smoke in `examples/timeline-sdk-consumer` verifies this
target closure without pulling in Pulp's view, GPU, graph, format, standalone,
or host modules. Add `Pulp::sequence` only when exposing the compiled program as
a Pulp plugin processor.

## Construct a project and reserve identities

Every Timeline-owned object has a nonzero `ItemId`. Allocate monotonically and
store the allocator's next value in `ProjectInput::next_item_id`; never scan the
document for a reusable gap.

```cpp
#include <pulp/timeline/model.hpp>

#include <utility>

using namespace pulp;

timeline::ItemIdAllocator ids;
auto project_id = ids.allocate();
auto sequence_id = ids.allocate();
auto track_id = ids.allocate();
auto clip_id = ids.allocate();
if (!project_id || !sequence_id || !track_id || !clip_id)
    return report_model_error();

const auto project_item_id = project_id.value();
const auto sequence_item_id = sequence_id.value();
const auto track_item_id = track_id.value();
const auto clip_item_id = clip_id.value();

auto clip = timeline::Clip::create(
    clip_item_id, timebase::TickPosition{0},
    timebase::TickDuration{4 * timebase::kTicksPerQuarter},
    timeline::EmptyContent{});
if (!clip)
    return report(clip.error());

auto track = timeline::Track::create(
    track_item_id, "Track", {std::move(clip).value()});
if (!track)
    return report(track.error());

auto sequence = timeline::Sequence::create(
    sequence_item_id, "Root",
    timebase::TickDuration{8 * timebase::kTicksPerQuarter},
    {std::move(track).value()});
if (!sequence)
    return report(sequence.error());

timeline::ProjectInput input;
input.id = project_item_id;
input.name = "Song";
input.next_item_id = ids.next_value();
input.root_sequence_id = sequence_item_id;
input.sequences.push_back(std::move(sequence).value());

auto project = timeline::Project::create(std::move(input));
if (!project)
    return report(project.error());
```

Factories reject duplicate identities, invalid ranges, missing references, and
non-monotonic `next_item_id`. After construction, use
`project->item_id_allocator()` when preparing new identity-bearing content for
a command.

## Submit an optimistic transaction and undo it

Observe the snapshot and revision together, allocate transaction and command
IDs from the session's writer token, and put the observed revision in
`expected_revision`.

```cpp
#include <pulp/timeline/document_session.hpp>

auto session_result =
    timeline::DocumentSession::create(std::move(project).value());
if (!session_result)
    return report(session_result.error());
auto session = std::move(session_result).value();

auto writer_result = session->register_writer();
if (!writer_result)
    return report(writer_result.error());
auto writer = std::move(writer_result).value();

const auto view = session->current();
timeline::Transaction edit;
edit.id = writer.allocate_transaction_id();
edit.expected_revision = view.revision;
edit.commands.push_back({
    writer.allocate_command_id(),
    timeline::MoveClip{
        sequence_item_id,
        track_item_id,
        clip_item_id,
        view.snapshot->find_sequence(sequence_item_id)
            ->find_track(track_item_id)
            ->find_clip(clip_item_id)
            ->time_range(),
        timeline::MusicalTimeRange{
            timebase::TickPosition{timebase::kTicksPerQuarter},
            timebase::TickDuration{4 * timebase::kTicksPerQuarter},
        },
    },
});

auto committed = session->submit(writer, std::move(edit));
if (!committed) {
    if (committed.error().code == timeline::ConflictCode::StaleRevision)
        return rebuild_against(session->current());
    return report(committed.error());
}

auto undone = session->undo(writer);
if (!undone)
    return report(undone.error());
auto redone = session->redo(writer);
if (!redone)
    return report(redone.error());
```

Every command in the batch either commits or none does. Expected old values in
commands are optimistic gates, so rebuild after `StaleRevision` or
`ExpectedValueMismatch`; do not silently overwrite newer state.

For a drag or other multi-transaction gesture, allocate one
`UndoGroupId`, submit `Begin`, zero or more `Update` transactions, and one
`End`. Keep the same writer and group throughout. Undo and redo are unavailable
while a gesture is open.

Pass `redone->dirty` with a shared `CompileContextRegistry` in
`ProgramCompileRequest::invalidation` to compile only affected tracks.

## Open or restore a durable session

`FileJournal::open()` returns both the durable sink and the exact checkpoint
state recovered from the file. Branch only on `recovered_existing`:

```cpp
#include <pulp/timeline/file_journal.hpp>
#include <pulp/timeline/document_session.hpp>

#include <utility>

auto registry = timeline::make_builtin_timeline_registry();
if (!registry)
    return report(registry.error());

auto opened = timeline::FileJournal::open(
    "song.ptlj", std::move(fallback_project), std::move(registry).value());
if (!opened)
    return report(opened.error());

auto durable_session = opened->recovered_existing
    ? timeline::DocumentSession::restore(
          std::move(opened->checkpoint), opened->revision, {}, opened->sink)
    : timeline::DocumentSession::create(
          std::move(opened->checkpoint), {}, opened->sink);
if (!durable_session)
    return report(durable_session.error());
```

`DocumentSession` retains shared ownership of `opened->sink`. The local open
result may be released after session creation succeeds. Do not separately
overwrite a snapshot beside the journal: a commit is published only after its
complete journal frame is acknowledged durable. A torn trailing frame may be
repaired and reported by `repaired_torn_tail`; corruption in committed history
fails the open.

Call `DocumentSession::checkpoint(revision)` only for a revision successfully
published by that durable session. Check the returned `bool`; failure leaves
the prior in-memory checkpoint intact and disables later durable writes when
the sink failed.

## Compile, publish, and render

Compilation owns allocations and model traversal. A `PlaybackProgramStore`
publishes the immutable result; the callback pins one program for the entire
block.

```cpp
#include <pulp/playback/audio_renderer.hpp>
#include <pulp/playback/compile_context_registry.hpp>
#include <pulp/playback/compile_executor.hpp>
#include <pulp/playback/program_compiler.hpp>
#include <pulp/playback/stable_renderer_shell.hpp>
#include <pulp/playback/transport.hpp>
#include <pulp/timebase/compiled_tempo_map.hpp>

#include <array>
#include <chrono>
#include <memory>
#include <utility>

const std::array tempo_points{
    timebase::TempoPoint{timebase::TickPosition{0}, 120.0},
};
auto compiled_tempo =
    timebase::CompiledTempoMap::compile(tempo_points, {48'000, 1});
if (!compiled_tempo)
    return report(compiled_tempo.error());
auto tempo = std::make_shared<const timebase::CompiledTempoMap>(
    std::move(compiled_tempo).value());

playback::PlaybackProgramStore programs;
playback::DeferredCompileExecutor executor;
playback::PlaybackProgramCompiler compiler(
    programs, executor, std::chrono::microseconds{0});
auto compile_contexts = std::make_shared<playback::CompileContextRegistry>();

playback::ProgramCompileRequest request;
request.project = redone->snapshot;
request.sequence_id = redone->snapshot->root_sequence_id();
request.tempo_map = std::move(tempo);
request.sample_rate = request.tempo_map->sample_rate();
request.document_revision = redone->revision.value;
request.dirty.all = true;
request.invalidation = playback::CompileInvalidationInput{
    compile_contexts, *redone};

auto ticket = compiler.submit(std::move(request));
if (!ticket)
    return report(ticket.error());
while (compiler.status().busy)
    executor.run_for(std::chrono::milliseconds{1});
const auto compile_status = compiler.status();
if (compile_status.has_error ||
    compile_status.latest_published_epoch < ticket.value().submission_epoch)
    return report(compile_status.last_error);
```

For later incremental compiles, omit the unconditional `all` assignment and
keep passing the same registry plus each committed dirty set through
`request.invalidation`. Add the immutable decoded asset pool to
`request.audio_assets` when the project references audio. A native application
can use `WorkerCompileExecutor`; check `supported()` before submitting. The
store and executor must outlive the compiler and every accepted task.

### Add a registered chord-pattern clip

Use the installed-SDK
[registered chord renderer example](../../examples/timeline-sdk-consumer/registered_chord_renderer.cpp)
as the runnable recipe:

```cpp
timeline::SchemaRegistryBuilder schemas;
if (!timeline::register_builtin_timeline_schemas(schemas) ||
    !playback::register_chord_pattern_content_schema(schemas))
    return report_registration_error();
auto schema_registry = std::move(schemas).build();

auto compile_contexts =
    std::make_shared<playback::CompileContextRegistry>();
if (playback::declare_chord_pattern_renderer(
        *compile_contexts, schema_registry.value()))
    return report_registration_error();
```

Create clip content with `create_chord_pattern_content()` against that same
immutable schema registry. For the initial compile use
`CompileInvalidationInput::baseline()`; after an edit, construct invalidation
from the exact `CommitResult`. Wait until
`latest_published_epoch >= submission_epoch`, not merely until the document
revision matches. An unresolved registered clip and a renderer that exceeds its
bounded note fragment fail compilation with typed diagnostics; neither is
silently ignored. The example also checks exact deterministic output, unrelated
MIDI-track owner reuse, quota `actual`/`limit`, and weakest production
aggregation.

Keep registrations within the current boundary: note output, `Reset` state,
and at most 4096 fragment notes per clip. A trimmed nested registered clip is
unsupported because the hook cannot recover its original pattern phase.
Nondefault production declarations are useful for in-process honesty, but
`ProgramWire` refuses to serialize them; redeclare the hook in each process.

At the callback boundary:

```cpp
playback::TransportSnapshot transport_block;
if (transport.begin_block(frame_count, transport_block) !=
    playback::TransportError::None)
    return clear_output();

playback::PlaybackProgramBlockLatch latch;
auto program_block = latch.begin_block(programs);
if (!program_block)
    return clear_output();

const auto status = playback::ArrangementAudioRenderer::process(
    *program_block.program(), transport_block, output);
```

Use the same `program_block` and `transport_block` for note and automation
renderers. Do not read the store again halfway through the callback.

## Add and resolve audio assets

The Project owns media metadata and a SHA-256 `ContentHash`; the application
owns the bytes and resolution policy. A locator is a hint, never identity.

1. Read or receive complete bytes away from the callback.
2. Hash the exact bytes and construct a validated `MediaAsset`.
3. Publish it with `CreateAsset` before publishing any clip, take, or freeze that
   references it.
4. Decode WAV bytes with `DecodedAudioAssetPool::decode_wav()`.
5. Build one immutable `DecodedAudioAssetPool` and pass it in
   `ProgramCompileRequest::audio_assets`.

```cpp
auto decoded = playback::DecodedAudioAssetPool::decode_wav(asset.id, wav_bytes);
if (!decoded)
    return report(decoded.error());

auto pool = playback::DecodedAudioAssetPool::create(
    {std::move(decoded).value()});
if (!pool)
    return report(pool.error());
```

The model's frame count and rational sample rate must agree with the decoded
asset. Playback prepares sample-rate conversion artifacts during compilation;
the callback only reads them.

## Capture audio into a take

Prepare fixed capture capacity before entering the callback:

```cpp
playback::CaptureEngine capture;
playback::CaptureEngineConfig config;
auto capture_view = session->current();
auto capture_ids = capture_view.snapshot->item_id_allocator();
auto take_lane_id = capture_ids.allocate();
if (!take_lane_id)
    return report(take_lane_id.error());

auto take_lane =
    timeline::TakeLane::create(take_lane_id.value(), "Recording", {});
if (!take_lane)
    return report(take_lane.error());

timeline::Transaction reserve_lane;
reserve_lane.id = writer.allocate_transaction_id();
reserve_lane.expected_revision = capture_view.revision;
reserve_lane.commands.push_back({
    writer.allocate_command_id(),
    timeline::InsertTakeLane{
        sequence_item_id,
        track_item_id,
        std::move(take_lane).value(),
    },
});
auto lane_commit = session->submit(writer, std::move(reserve_lane));
if (!lane_commit)
    return report(lane_commit.error());

config.sample_rate = {48'000, 1};
config.maximum_block_size = 512;
config.maximum_take_frames = 48'000 * 60;
config.take_slots_per_track = 4;
config.maximum_preallocated_bytes = 256ull * 1024ull * 1024ull;
config.tracks.push_back({
    .track_id = track_item_id,
    .take_lane_id = take_lane_id.value(),
    .input_channel = 0,
    .channel_count = 2,
    .armed = true,
});
if (!capture.prepare(config))
    return report_capture_configuration();
```

The control side enqueues `Start`, `Stop`, `Cancel`, and `ReleaseTake`.
The callback calls `capture.process()` with the same `TransportSnapshot` used
for playback. Away from the callback:

1. Drain `CaptureEvent`s until `TakeCompleted`.
2. Copy the completed handle's audio before releasing it.
3. Allocate fresh asset and take identities from the current post-capture
   snapshot, then call `seal_recording_take()` for the existing take lane.
4. Persist the returned WAV bytes in application-owned storage.
5. Wrap the returned `commands` in one transaction and submit them through
   `DocumentSession`.
6. Enqueue `ReleaseTake` only after copying the captured data.

`seal_recording_take()` returns a sealed asset, a take, WAV bytes, and ordered
commands. The recipe publishes the empty lane before capture so its identity
cannot collide with intervening edits; leave `create_take_lane` false when
sealing into that lane. If an application instead creates the lane and take in
one post-capture transaction, it must coordinate identity allocation so no
intervening writer can consume the configured lane identity. Queue drops and
capacity failures are visible through `CaptureEngineStats`; treat them as
recording failures, not silence.

## Author scenes, slots, and follow actions

Scenes and slots are durable document intent. A nonempty slot references a clip
already owned by the same sequence:

```cpp
#include <pulp/timeline/clip_launch.hpp>

auto authored_ids = session->snapshot()->item_id_allocator();
auto scene_id = authored_ids.allocate();
auto slot_id = authored_ids.allocate();
if (!scene_id || !slot_id)
    return report_model_error();

timeline::Slot slot{
    slot_id.value(),
    clip_item_id,
    timeline::launch_every_quarters(4),
    timeline::follow_action(
        timeline::FollowActionKind::Next,
        timebase::TickDuration{8 * timebase::kTicksPerQuarter}),
};
timeline::Scene scene{
    scene_id.value(), "Verse", timeline::SlotList{{std::move(slot)}}};

timeline::Transaction add_scene;
add_scene.id = writer.allocate_transaction_id();
add_scene.expected_revision = session->revision();
add_scene.commands.push_back({
    writer.allocate_command_id(),
    timeline::InsertScene{sequence_item_id, std::move(scene)},
});
auto added = session->submit(writer, std::move(add_scene));
```

`LaunchQuantize` snaps against the transport's non-looping monotonic beat.
Weighted follow selection is deterministic for the same seed, slot identity,
and draw index. The playback launch primitives resolve start, stop, and follow
boundaries to callback sample offsets.

The program compiler accepts only `ProviderKind::Arrangement` with the
arrangement availability bit. Authored Scene/Slot persistence and the low-level
launch state machines do not provide scene-to-track arbitration or make
`ArrangementAudioRenderer` play a scene. Keep runtime launcher ownership in the
embedding application and do not select the Launcher provider in a
`ProgramCompileRequest`.

## Import, export, and require loss consent

DAWproject import consumes the package's `project.xml`, not the ZIP container.
Link `Pulp::dawproject-import` and provide a resolver for referenced package
media:

```cpp
auto imported = timeline::import_dawproject_xml(
    project_xml,
    [&](std::string_view package_path)
        -> std::optional<std::vector<std::uint8_t>> {
        return package_reader.read_bounded(package_path);
    });
if (!imported)
    return report(imported.error());
```

The importer rejects unsupported or unresolved input as a whole. Review its
bounded subset in the [SDK guide](timeline-sdk.md#optional-dawproject-importer).
For MIDI-only raw codec access, link `Pulp::smf-interop` and use `import_smf()`
or `export_smf()` with explicit unsupported-event and tick-rounding policies.
For project-wide census and loss consent, link `Pulp::smf-interchange` and pass
`smf::writer()` to `run_export()`.

Before any format writer runs, produce and show its loss plan:

```cpp
#include <pulp/interchange/export_plan.hpp>
#include <pulp/dawproject/dawproject_export.hpp>

const auto plan = interchange::plan_export(
    project, interchange::Format::DawProject);

for (const auto& loss : plan.losses().entries())
    show_loss(loss.concept_value, loss.loss_class, loss.count, loss.detail);

interchange::ExportOptions options;
options.accepted_losses = user_approved_concepts;
auto artifacts = interchange::run_export(plan, options, dawproject::writer());
```

Consent is per concept. There is no blanket force option. `run_export()` refuses
before calling the writer when any required loss is unaccepted. The plan owns
the immutable project snapshot the built-in writer serializes, so consent cannot
be measured on one revision and applied to another. On success the central
runner returns `project.xml` plus the versioned, reserved
`pulp-loss-manifest.json` artifact.

## Apply the same edit through CLI or MCP

The generated schema is the source of truth for document and command envelopes:

```bash
pulp seq schema > timeline-schema.json
pulp seq validate song.pulpseq.json
pulp seq apply song.pulpseq.json commands.json --out changed.pulpseq.json
pulp seq explain changed.pulpseq.json --sample-rate 48000
pulp render changed.pulpseq.json --out changed.wav --sample-rate 48000
```

The matching MCP operations are:

- `pulp_timeline_project_open`
- `pulp_timeline_command_apply`
- `pulp_timeline_validate`
- `pulp_timeline_explain`
- `pulp_timeline_render`

MCP accepts a project path or inline canonical JSON. Its `commands` argument is
the envelope array itself, not a path. CLI and MCP edit snapshots; native
`FileJournal` session ownership and realtime device I/O remain embedding APIs.

## Failure and realtime checklist

Before publishing an integration:

- Check every `runtime::Result`; preserve the typed model, transaction, compile,
  import, persistence, and render error instead of replacing it with a generic
  failure.
- Rebuild optimistic commands from a fresh `DocumentView` after stale revision
  or expected-value conflicts.
- Treat identity exhaustion, resource-limit rejection, dropped capture
  commands/events, and output-capacity rejection as visible failures.
- Create model values, hash/decode media, serialize, journal, compile, and
  materialize captures away from the audio callback.
- Prepare capture engines, transports, renderer storage, MIDI buffers, and
  explicit capacity limits before processing.
- Pin one `PlaybackProgramBlock` and one `TransportSnapshot` for the whole
  callback.
- Perform no file I/O, allocation, locking, session submission, undo/redo, or
  media resolution on the audio thread.
- Keep borrowed tempo maps, decoded pools, program stores, executors, journal
  sinks, and capture handles alive for the lifetimes stated by their APIs.
- Surface unsupported provider and interchange capabilities; do not replace
  them with silent fallback behavior.
