#include <pulp/playback/program_compiler.hpp>
#include <pulp/timebase/compiled_tempo_map.hpp>
#include <pulp/timeline/schema_json.hpp>
#include <pulp/timeline/serialize.hpp>
#include <pulp/timeline/transaction.hpp>

#include "timebase_test_helpers.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <thread>
#include <vector>

using namespace pulp;
using namespace pulp::playback;
using namespace pulp::timebase;
using namespace pulp::timeline;

namespace {

constexpr std::size_t kTrackCount = 1'000;
constexpr std::size_t kClipsPerTrack = 100;
constexpr std::uint64_t kFirstTrackId = 10;
constexpr std::uint64_t kFirstClipId = 10'000;
constexpr std::size_t kNoteTrackCount = 50;
constexpr std::size_t kNotesPerClip = 100'000;
constexpr std::size_t kAutomationTrackCount = 100;
constexpr std::size_t kAutomationPointsPerLane = 10'000;

template <typename T, typename E> T take(runtime::Result<T, E> result) {
    if (!result)
        std::abort();
    return std::move(result).value();
}

class InlineExecutor final : public CompileExecutor {
  public:
    bool submit(std::unique_ptr<CompileTask> task, std::chrono::steady_clock::time_point) override {
        if (!task)
            return false;
        while (task->run_slice({std::chrono::steady_clock::now() + std::chrono::seconds(1),
                                10'000}) == CompileTaskStatus::Pending) {
        }
        return true;
    }
};

std::shared_ptr<const CompiledTempoMap> tempo_map() {
    const std::array points{TempoPoint{{0}, 120.0}};
    return shared_compiled_tempo_map(points, RationalRate{48'000, 1});
}

std::shared_ptr<const Project> arrangement_scale_project() {
    std::vector<Track> tracks;
    tracks.reserve(kTrackCount);
    for (std::size_t track_index = 0; track_index < kTrackCount; ++track_index) {
        std::vector<Clip> clips;
        clips.reserve(kClipsPerTrack);
        for (std::size_t clip_index = 0; clip_index < kClipsPerTrack; ++clip_index) {
            const auto ordinal = track_index * kClipsPerTrack + clip_index;
            clips.push_back(take(Clip::create({kFirstClipId + ordinal},
                                              {static_cast<std::int64_t>(clip_index * 4)}, {1},
                                              EmptyContent{})));
        }
        tracks.push_back(
            take(Track::create({kFirstTrackId + track_index}, "scale-track", std::move(clips))));
    }
    auto sequence = take(Sequence::create({2}, "scale-sequence", std::nullopt, std::move(tracks)));
    ProjectInput input;
    input.id = {1};
    input.name = "arrangement-scale";
    input.next_item_id = kFirstClipId + kTrackCount * kClipsPerTrack;
    input.root_sequence_id = {2};
    input.sequences.push_back(std::move(sequence));
    return std::make_shared<const Project>(take(Project::create(std::move(input))));
}

std::shared_ptr<const Project> note_scale_project() {
    constexpr std::uint64_t first_note_id = 1'000;
    std::vector<Track> tracks;
    tracks.reserve(kNoteTrackCount);
    for (std::size_t track_index = 0; track_index < kNoteTrackCount; ++track_index) {
        std::vector<NoteEvent> notes;
        notes.reserve(kNotesPerClip);
        for (std::size_t note_index = 0; note_index < kNotesPerClip; ++note_index) {
            const auto ordinal = track_index * kNotesPerClip + note_index;
            notes.push_back({{first_note_id + ordinal},
                             {static_cast<std::int64_t>(note_index * kTicksPerQuarter / 4)},
                             {kTicksPerQuarter / 8},
                             0xffff,
                             static_cast<std::uint8_t>(48 + note_index % 24),
                             0});
        }
        auto content = take(NoteContent::create(std::move(notes)));
        auto clip = take(Clip::create(
            {100 + track_index}, {0},
            {static_cast<std::int64_t>(kNotesPerClip * kTicksPerQuarter / 4)}, std::move(content)));
        tracks.push_back(
            take(Track::create({10 + track_index}, "note-scale-track", {std::move(clip)})));
    }
    auto sequence =
        take(Sequence::create({2}, "note-scale-sequence", std::nullopt, std::move(tracks)));
    ProjectInput input;
    input.id = {1};
    input.name = "note-scale";
    input.next_item_id = first_note_id + kNoteTrackCount * kNotesPerClip;
    input.root_sequence_id = {2};
    input.sequences.push_back(std::move(sequence));
    return std::make_shared<const Project>(take(Project::create(std::move(input))));
}

std::shared_ptr<const Project> automation_scale_project() {
    constexpr std::uint64_t first_point_id = 10'000;
    std::vector<Track> tracks;
    tracks.reserve(kAutomationTrackCount);
    for (std::size_t track_index = 0; track_index < kAutomationTrackCount; ++track_index) {
        std::vector<AutomationPoint> points;
        points.reserve(kAutomationPointsPerLane);
        for (std::size_t point_index = 0; point_index < kAutomationPointsPerLane; ++point_index) {
            const auto ordinal = track_index * kAutomationPointsPerLane + point_index;
            points.push_back({{first_point_id + ordinal},
                              {static_cast<std::int64_t>(point_index)},
                              static_cast<float>(point_index % 101) / 100.0f});
        }
        const ItemId device_id{1'000 + track_index};
        auto curve = take(AutomationCurve::create(std::move(points)));
        auto lane = take(AutomationLane::create(
            {2'000 + track_index}, DeviceParameterTarget{device_id, 7}, std::move(curve)));
        TrackInput track;
        track.id = {10 + track_index};
        track.name = "automation-scale-track";
        track.device_chain = {{{device_id}}};
        track.automation_lanes.push_back(std::move(lane));
        tracks.push_back(take(Track::create(std::move(track))));
    }
    auto sequence =
        take(Sequence::create({2}, "automation-scale-sequence", std::nullopt, std::move(tracks)));
    ProjectInput input;
    input.id = {1};
    input.name = "automation-scale";
    input.next_item_id = first_point_id + kAutomationTrackCount * kAutomationPointsPerLane;
    input.root_sequence_id = {2};
    input.sequences.push_back(std::move(sequence));
    return std::make_shared<const Project>(take(Project::create(std::move(input))));
}

ProgramCompileRequest compile_request(std::shared_ptr<const Project> project,
                                      std::shared_ptr<const CompiledTempoMap> map,
                                      std::uint64_t revision, DirtyTrackSet dirty) {
    ProgramCompileRequest request;
    request.project = std::move(project);
    request.sequence_id = {2};
    request.tempo_map = std::move(map);
    request.document_revision = revision;
    request.dirty = std::move(dirty);
    return request;
}

Transaction move_transaction(std::uint64_t sequence, ItemId track_id, const Clip& clip) {
    Transaction transaction;
    transaction.id = {{1}, sequence};
    transaction.expected_revision = {sequence};
    transaction.commands.push_back(
        {{{1}, sequence},
         MoveClip{{2},
                  track_id,
                  clip.id(),
                  clip.time_range(),
                  MusicalTimeRange{{clip.start().value + 1}, clip.duration()}}});
    return transaction;
}

bool strict_performance() {
    const auto* value = std::getenv("PULP_PERF_STRICT");
    return value && value[0] && value[0] != '0';
}

bool wait_for_revision(PlaybackProgramCompiler& compiler, std::uint64_t revision,
                       std::chrono::steady_clock::duration timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto status = compiler.status();
        if (status.has_error)
            return false;
        if (status.latest_published_revision == revision)
            return true;
        std::this_thread::yield();
    }
    return false;
}

} // namespace

TEST_CASE("full arrangement scale sustains one hundred structural transactions under playback",
          "[timeline][scale][performance]") {
#if defined(PULP_TEST_WITH_SANITIZER)
    SKIP("full-scale coverage runs in non-instrumented builds");
#endif
    const auto before_identities = Project::identity_stats().nodes_created;
    auto project = arrangement_scale_project();
    REQUIRE(project->find_sequence({2})->tracks().size() == kTrackCount);
    REQUIRE(Project::identity_stats().nodes_created - before_identities ==
            2 + kTrackCount + kTrackCount * kClipsPerTrack);
    REQUIRE(project->locate({kFirstClipId + kTrackCount * kClipsPerTrack - 1}));

    {
        const auto registry = take(make_builtin_timeline_registry());
        const auto snapshot = take(serialize_project(*project, registry));
        const auto before_summary_nodes = Project::identity_stats().nodes_created;
        const auto summary = take(peek_project_summary(snapshot.json));
        REQUIRE(summary.track_count == kTrackCount);
        REQUIRE(summary.clip_count == kTrackCount * kClipsPerTrack);
        REQUIRE(Project::identity_stats().nodes_created == before_summary_nodes);
        const auto before_restore_nodes = Project::identity_stats().nodes_created;
        const auto load_started = std::chrono::steady_clock::now();
        const auto restored = take(deserialize_project(snapshot.json, registry));
        const auto load_elapsed = std::chrono::steady_clock::now() - load_started;
        REQUIRE(restored.find_sequence({2})->tracks().size() == kTrackCount);
        REQUIRE(restored.locate({kFirstClipId + kTrackCount * kClipsPerTrack - 1}));
        REQUIRE(Project::identity_stats().nodes_created - before_restore_nodes ==
                2 + kTrackCount + kTrackCount * kClipsPerTrack);
        if (strict_performance())
            REQUIRE(load_elapsed < std::chrono::seconds(2));
    }

    PlaybackProgramStore store;
    WorkerCompileExecutor executor;
    REQUIRE(executor.supported());
    PlaybackProgramCompiler compiler(store, executor, std::chrono::microseconds(0));
    const auto map = tempo_map();
    const auto cold_started = std::chrono::steady_clock::now();
    REQUIRE(compiler.submit(compile_request(project, map, 1, {.all = true})));
    REQUIRE(wait_for_revision(compiler, 1, std::chrono::seconds(5)));
    const auto cold_elapsed = std::chrono::steady_clock::now() - cold_started;
    REQUIRE(store.read());
    REQUIRE(store.read()->document_revision() == 1);

    std::atomic<bool> reader_failed{false};
    std::atomic<std::uint64_t> reader_iterations{0};
    std::jthread reader([&](std::stop_token stop) {
        std::uint64_t last_revision = 0;
        while (!stop.stop_requested()) {
            auto program = store.read();
            if (!program)
                continue;
            if (program->document_revision() < last_revision ||
                program->tracks().size() != kTrackCount)
                reader_failed.store(true, std::memory_order_relaxed);
            last_revision = program->document_revision();
            reader_iterations.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    });

    auto edits_started = std::chrono::steady_clock::now();
    auto maximum_edit_latency = std::chrono::steady_clock::duration::zero();
    for (std::uint64_t edit = 0; edit < 100; ++edit) {
        const ItemId track_id{kFirstTrackId + edit};
        const ItemId clip_id{kFirstClipId + edit * kClipsPerTrack};
        const auto* clip = project->find_sequence({2})->find_track(track_id)->find_clip(clip_id);
        REQUIRE(clip);
        const auto edit_started = std::chrono::steady_clock::now();
        auto reduced = reduce_transaction(*project, move_transaction(edit + 1, track_id, *clip));
        REQUIRE(reduced);
        project = std::make_shared<const Project>(std::move(reduced).value().project);
        REQUIRE(compiler.submit(
            compile_request(project, map, edit + 2, {.all = false, .tracks = {track_id}})));
        REQUIRE(wait_for_revision(compiler, edit + 2, std::chrono::seconds(2)));
        maximum_edit_latency =
            std::max(maximum_edit_latency, std::chrono::steady_clock::now() - edit_started);
    }
    const auto edits_elapsed = std::chrono::steady_clock::now() - edits_started;
    reader.request_stop();
    reader.join();

    const auto status = compiler.status();
    REQUIRE_FALSE(reader_failed.load(std::memory_order_relaxed));
    REQUIRE(reader_iterations.load(std::memory_order_relaxed) > 0);
    REQUIRE_FALSE(status.has_error);
    REQUIRE_FALSE(status.busy);
    REQUIRE(status.submitted_requests == 101);
    REQUIRE(status.latest_published_revision == 101);
    REQUIRE(store.read()->document_revision() == 101);
    if (strict_performance()) {
        REQUIRE(cold_elapsed < std::chrono::seconds(2));
        REQUIRE(edits_elapsed < std::chrono::seconds(1));
        REQUIRE(maximum_edit_latency < std::chrono::milliseconds(30));
    }
}

TEST_CASE("full note scale compiles five million events with one hundred thousand per clip",
          "[timeline][scale][performance]") {
#if defined(PULP_TEST_WITH_SANITIZER)
    SKIP("full-scale coverage runs in non-instrumented builds");
#endif
    const auto before_identities = Project::identity_stats().nodes_created;
    auto project = note_scale_project();
    constexpr auto total_notes = kNoteTrackCount * kNotesPerClip;
    REQUIRE(Project::identity_stats().nodes_created - before_identities ==
            2 + 2 * kNoteTrackCount + total_notes);
    REQUIRE(project->locate({1'000 + total_notes - 1}));
    const auto* final_track = project->find_sequence({2})->find_track({10 + kNoteTrackCount - 1});
    REQUIRE(final_track);
    REQUIRE(std::get<NoteContent>(final_track->clips()[0].content()).notes().size() ==
            kNotesPerClip);

    PlaybackProgramStore store;
    InlineExecutor executor;
    PlaybackProgramCompiler compiler(store, executor, std::chrono::microseconds(0));
    const auto started = std::chrono::steady_clock::now();
    REQUIRE(compiler.submit(compile_request(project, tempo_map(), 1, {.all = true})));
    const auto elapsed = std::chrono::steady_clock::now() - started;
    const auto status = compiler.status();
    CAPTURE(status.last_error.code, status.last_error.item.value, status.last_error.audio_detail);
    REQUIRE_FALSE(status.has_error);
    REQUIRE(store.read()->document_revision() == 1);
    if (strict_performance())
        REQUIRE(elapsed < std::chrono::seconds(20));
}

TEST_CASE("full automation scale compiles one million points with ten thousand per lane",
          "[timeline][scale][performance]") {
#if defined(PULP_TEST_WITH_SANITIZER)
    SKIP("full-scale coverage runs in non-instrumented builds");
#endif
    const auto before_identities = Project::identity_stats().nodes_created;
    auto project = automation_scale_project();
    constexpr auto total_points = kAutomationTrackCount * kAutomationPointsPerLane;
    REQUIRE(Project::identity_stats().nodes_created - before_identities ==
            2 + 3 * kAutomationTrackCount + total_points);
    REQUIRE(project->locate({10'000 + total_points - 1}));
    const auto* final_track =
        project->find_sequence({2})->find_track({10 + kAutomationTrackCount - 1});
    REQUIRE(final_track);
    REQUIRE(final_track->automation_lanes()[0].curve().points().size() == kAutomationPointsPerLane);

    PlaybackProgramStore store;
    InlineExecutor executor;
    PlaybackProgramCompiler compiler(store, executor, std::chrono::microseconds(0));
    const auto started = std::chrono::steady_clock::now();
    REQUIRE(compiler.submit(compile_request(project, tempo_map(), 1, {.all = true})));
    const auto elapsed = std::chrono::steady_clock::now() - started;
    const auto status = compiler.status();
    CAPTURE(status.last_error.code, status.last_error.item.value, status.last_error.audio_detail);
    REQUIRE_FALSE(status.has_error);
    REQUIRE(store.read()->document_revision() == 1);
    if (strict_performance())
        REQUIRE(elapsed < std::chrono::seconds(20));
}
