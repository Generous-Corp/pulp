#include <pulp/playback/capture_engine.hpp>
#include <pulp/playback/compile_executor.hpp>
#include <pulp/playback/program_compiler.hpp>
#include <pulp/playback/stable_renderer_shell.hpp>
#include <pulp/timebase/compiled_tempo_map.hpp>
#include <pulp/timeline/clip_launch.hpp>
#include <pulp/timeline/document_session.hpp>
#include <pulp/timeline/file_journal.hpp>
#include <pulp/timeline/model.hpp>

#include <array>
#include <chrono>
#include <filesystem>
#include <memory>
#include <utility>

using namespace pulp;

namespace {

struct ProjectIds {
    timeline::ItemId project;
    timeline::ItemId sequence;
    timeline::ItemId track;
    timeline::ItemId clip;
};

runtime::Result<timeline::Project, timeline::ModelError>
make_project(ProjectIds& ids_out) {
    timeline::ItemIdAllocator ids;
    auto project_id = ids.allocate();
    auto sequence_id = ids.allocate();
    auto track_id = ids.allocate();
    auto clip_id = ids.allocate();
    if (!project_id)
        return runtime::Err(project_id.error());
    if (!sequence_id)
        return runtime::Err(sequence_id.error());
    if (!track_id)
        return runtime::Err(track_id.error());
    if (!clip_id)
        return runtime::Err(clip_id.error());

    ids_out = {
        project_id.value(),
        sequence_id.value(),
        track_id.value(),
        clip_id.value(),
    };

    auto clip = timeline::Clip::create(
        ids_out.clip, timebase::TickPosition{0},
        timebase::TickDuration{4 * timebase::kTicksPerQuarter},
        timeline::EmptyContent{});
    if (!clip)
        return runtime::Err(clip.error());

    auto track =
        timeline::Track::create(ids_out.track, "Track", {std::move(clip).value()});
    if (!track)
        return runtime::Err(track.error());

    auto sequence = timeline::Sequence::create(
        ids_out.sequence, "Root",
        timebase::TickDuration{8 * timebase::kTicksPerQuarter},
        {std::move(track).value()});
    if (!sequence)
        return runtime::Err(sequence.error());

    timeline::ProjectInput input;
    input.id = ids_out.project;
    input.name = "Song";
    input.next_item_id = ids.next_value();
    input.root_sequence_id = ids_out.sequence;
    input.sequences.push_back(std::move(sequence).value());
    return timeline::Project::create(std::move(input));
}

[[maybe_unused]] bool
open_durable_session(const std::filesystem::path& path, timeline::Project fallback) {
    auto registry = timeline::make_builtin_timeline_registry();
    if (!registry)
        return false;

    auto opened =
        timeline::FileJournal::open(path, std::move(fallback), std::move(registry).value());
    if (!opened)
        return false;

    auto session = opened->recovered_existing
        ? timeline::DocumentSession::restore(
              std::move(opened->checkpoint), opened->revision, {}, opened->sink)
        : timeline::DocumentSession::create(
              std::move(opened->checkpoint), {}, opened->sink);
    return static_cast<bool>(session);
}

bool compile_and_publish(const timeline::CommitResult& committed) {
    const std::array tempo_points{
        timebase::TempoPoint{timebase::TickPosition{0}, 120.0},
    };
    auto compiled_tempo =
        timebase::CompiledTempoMap::compile(tempo_points, {48'000, 1});
    if (!compiled_tempo)
        return false;
    auto tempo = std::make_shared<const timebase::CompiledTempoMap>(
        std::move(compiled_tempo).value());

    playback::PlaybackProgramStore programs;
    playback::DeferredCompileExecutor executor;
    playback::PlaybackProgramCompiler compiler(
        programs, executor, std::chrono::microseconds{0});

    playback::ProgramCompileRequest request;
    request.project = committed.snapshot;
    request.sequence_id = committed.snapshot->root_sequence_id();
    request.tempo_map = tempo;
    request.document_revision = committed.revision.value;
    // A consumer that just wants everything compiled asks for exactly that.
    // Translating a transaction's dirty set into compiler dirtiness is an
    // engine-internal path (it needs the compiler's invalidation index), and a
    // full compile is the honest default for a cookbook example.
    request.dirty.all = true;

    auto ticket = compiler.submit(std::move(request));
    if (!ticket)
        return false;
    while (compiler.status().busy)
        executor.run_for(std::chrono::milliseconds{1});
    if (compiler.status().has_error || !programs.has_value())
        return false;

    playback::MasterTransport transport;
    if (transport.prepare(*tempo, {.max_buffer_size = 128}) !=
        playback::TransportError::None)
        return false;
    playback::TransportSnapshot transport_block;
    if (transport.begin_block(128, transport_block) != playback::TransportError::None)
        return false;

    playback::PlaybackProgramBlockLatch latch;
    auto program_block = latch.begin_block(programs);
    if (!program_block)
        return false;
    audio::Buffer<float> output(2, 128);
    const auto status = playback::ArrangementAudioRenderer::process(
        *program_block.program(), transport_block, output.view());
    return status == playback::AudioRenderStatus::Rendered ||
           status == playback::AudioRenderStatus::Silent;
}

bool prepare_capture(timeline::ItemId track_id, timeline::ItemId take_lane_id) {
    playback::CaptureEngine capture;
    playback::CaptureEngineConfig config;
    config.sample_rate = {48'000, 1};
    config.maximum_block_size = 512;
    config.maximum_take_frames = 48'000 * 60;
    config.take_slots_per_track = 4;
    config.maximum_preallocated_bytes = 256ull * 1024ull * 1024ull;
    config.tracks.push_back({
        .track_id = track_id,
        .take_lane_id = take_lane_id,
        .input_channel = 0,
        .channel_count = 2,
        .armed = true,
    });
    return capture.prepare(config);
}

} // namespace

int main() {
    ProjectIds ids;
    auto project = make_project(ids);
    if (!project)
        return 1;

    auto session_result =
        timeline::DocumentSession::create(std::move(project).value());
    if (!session_result)
        return 2;
    auto session = std::move(session_result).value();

    auto writer_result = session->register_writer();
    if (!writer_result)
        return 3;
    auto writer = std::move(writer_result).value();

    const auto view = session->current();
    const auto* sequence = view.snapshot->find_sequence(ids.sequence);
    const auto* track = sequence ? sequence->find_track(ids.track) : nullptr;
    const auto* clip = track ? track->find_clip(ids.clip) : nullptr;
    if (!clip)
        return 4;

    timeline::Transaction edit;
    edit.id = writer.allocate_transaction_id();
    edit.expected_revision = view.revision;
    edit.commands.push_back({
        writer.allocate_command_id(),
        timeline::MoveClip{
            ids.sequence,
            ids.track,
            ids.clip,
            clip->time_range(),
            timeline::MusicalTimeRange{
                timebase::TickPosition{timebase::kTicksPerQuarter},
                timebase::TickDuration{4 * timebase::kTicksPerQuarter},
            },
        },
    });

    auto committed = session->submit(writer, std::move(edit));
    if (!committed)
        return 5;
    if (!session->undo(writer) || !session->redo(writer))
        return 6;

    auto authored_ids = session->snapshot()->item_id_allocator();
    auto scene_id = authored_ids.allocate();
    auto slot_id = authored_ids.allocate();
    if (!scene_id || !slot_id)
        return 7;

    timeline::Slot slot{
        slot_id.value(),
        ids.clip,
        timeline::launch_every_quarters(4),
        timeline::follow_action(
            timeline::FollowActionKind::Next,
            timebase::TickDuration{8 * timebase::kTicksPerQuarter}),
    };
    timeline::Scene scene{
        scene_id.value(),
        "Verse",
        timeline::SlotList{{std::move(slot)}},
    };

    timeline::Transaction add_scene;
    add_scene.id = writer.allocate_transaction_id();
    add_scene.expected_revision = session->revision();
    add_scene.commands.push_back({
        writer.allocate_command_id(),
        timeline::InsertScene{ids.sequence, std::move(scene)},
    });
    auto scene_commit = session->submit(writer, std::move(add_scene));
    if (!scene_commit)
        return 8;

    if (!compile_and_publish(scene_commit.value()))
        return 9;
    if (!prepare_capture(ids.track, timeline::ItemId{7}))
        return 10;
    return 0;
}
