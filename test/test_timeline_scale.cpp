#include <pulp/playback/program_compiler.hpp>
#include <pulp/timebase/compiled_tempo_map.hpp>
#include <pulp/timeline/document_session.hpp>
#include <pulp/timeline/schema_json.hpp>
#include <pulp/timeline/serialize.hpp>
#include <pulp/timeline/transaction.hpp>

#include "timebase_test_helpers.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string_view>
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
constexpr std::size_t kCollaborationWriterCount = 32;
constexpr std::size_t kCollaborationEditsPerWriter = 16;
constexpr std::size_t kDeepUndoCount = 512;
constexpr std::uint64_t kCollaborationFirstTrackId = 20'000;
constexpr std::uint64_t kCollaborationFirstClipId = 30'000;

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
        auto content = take(MidiContent::create(std::move(notes)));
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

Project collaboration_scale_project() {
    std::vector<Track> tracks;
    tracks.reserve(kCollaborationWriterCount);
    for (std::size_t index = 0; index < kCollaborationWriterCount; ++index) {
        auto clip = take(Clip::create({kCollaborationFirstClipId + index}, {0}, {1},
                                      EmptyContent{}));
        tracks.push_back(take(Track::create({kCollaborationFirstTrackId + index},
                                            "collaboration-track", {std::move(clip)})));
    }
    auto sequence = take(Sequence::create({2}, "collaboration-sequence", std::nullopt,
                                          std::move(tracks)));
    ProjectInput input;
    input.id = {1};
    input.name = "collaboration-scale";
    input.next_item_id = kCollaborationFirstClipId + kCollaborationWriterCount;
    input.root_sequence_id = {2};
    input.sequences.push_back(std::move(sequence));
    return take(Project::create(std::move(input)));
}

ProgramCompileRequest compile_request(std::shared_ptr<const Project> project,
                                      std::shared_ptr<const CompiledTempoMap> map,
                                      std::uint64_t revision, DirtyTrackSet dirty) {
    ProgramCompileRequest request;
    request.project = std::move(project);
    request.sequence_id = {2};
    request.tempo_map = std::move(map);
    request.sample_rate = request.tempo_map->sample_rate();
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

Transaction session_move_transaction(WriterToken& writer, DocumentRevision revision,
                                     ItemId track_id, const Clip& clip) {
    Transaction transaction;
    transaction.id = writer.allocate_transaction_id();
    transaction.expected_revision = revision;
    transaction.commands.push_back(
        {writer.allocate_command_id(),
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

std::optional<std::chrono::milliseconds> performance_budget(const char* name) {
    const auto* value = std::getenv(name);
    if (!value || !value[0]) {
        INFO("missing performance budget: " << name);
        REQUIRE_FALSE(strict_performance());
        return std::nullopt;
    }

    const std::string_view text(value);
    std::chrono::milliseconds::rep milliseconds = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), milliseconds);
    INFO("invalid performance budget " << name << '=' << text);
    REQUIRE(parsed.ec == std::errc{});
    REQUIRE(parsed.ptr == text.data() + text.size());
    REQUIRE(milliseconds > 0);
    return std::chrono::milliseconds(milliseconds);
}

template <class Rep, class Period>
void enforce_performance_budget(const char* name,
                                std::chrono::duration<Rep, Period> elapsed) {
    const auto budget = performance_budget(name);
    if (!budget)
        return;
    INFO(name << " elapsed_us="
              << std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count()
              << " budget_ms=" << budget->count());
    REQUIRE(elapsed <= *budget);
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
        const auto summary = take(peek_project_summary(snapshot.json, registry));
        REQUIRE(summary.counts.tracks == kTrackCount);
        REQUIRE(summary.counts.clips == kTrackCount * kClipsPerTrack);
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
            enforce_performance_budget("PULP_TIMELINE_LOAD_BUDGET_MS", load_elapsed);
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
        enforce_performance_budget("PULP_TIMELINE_COLD_COMPILE_BUDGET_MS", cold_elapsed);
        enforce_performance_budget("PULP_TIMELINE_EDIT_BATCH_BUDGET_MS", edits_elapsed);
        enforce_performance_budget("PULP_TIMELINE_EDIT_MAX_BUDGET_MS", maximum_edit_latency);
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
    REQUIRE(std::get<MidiContent>(final_track->clips()[0].content()).notes().size() ==
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
        enforce_performance_budget("PULP_TIMELINE_NOTE_COMPILE_BUDGET_MS", elapsed);
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
        enforce_performance_budget("PULP_TIMELINE_AUTOMATION_COMPILE_BUDGET_MS", elapsed);
}

TEST_CASE("thirty-two writers sustain concurrent revisioned edits with immutable readers",
          "[timeline][scale][performance][collaboration]") {
#if defined(PULP_TEST_WITH_SANITIZER)
    SKIP("full-scale coverage runs in non-instrumented builds");
#endif
    auto initial = collaboration_scale_project();
    SessionLimits limits;
    limits.max_writers = kCollaborationWriterCount;
    limits.journal.max_transactions =
        kCollaborationWriterCount * kCollaborationEditsPerWriter;
    auto session = take(DocumentSession::create(initial, limits));

    std::vector<WriterToken> writers;
    writers.reserve(kCollaborationWriterCount);
    for (std::size_t index = 0; index < kCollaborationWriterCount; ++index)
        writers.push_back(take(session->register_writer()));

    std::atomic<bool> reader_failed{false};
    std::atomic<std::uint64_t> reader_iterations{0};
    std::jthread reader([&](std::stop_token stop) {
        std::uint64_t last_revision = 0;
        while (!stop.stop_requested()) {
            const auto view = session->current();
            const auto* sequence = view.snapshot->find_sequence({2});
            if (!sequence || sequence->tracks().size() != kCollaborationWriterCount ||
                view.revision.value < last_revision)
                reader_failed.store(true, std::memory_order_relaxed);
            last_revision = view.revision.value;
            reader_iterations.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::yield();
        }
    });

    std::atomic<std::size_t> accepted{0};
    std::atomic<std::size_t> incomplete_writers{0};
    std::atomic<std::size_t> unexpected_errors{0};
    std::vector<std::thread> workers;
    workers.reserve(kCollaborationWriterCount);
    const auto started = std::chrono::steady_clock::now();
    for (std::size_t writer_index = 0; writer_index < kCollaborationWriterCount;
         ++writer_index) {
        workers.emplace_back([&, writer_index] {
            const ItemId track_id{kCollaborationFirstTrackId + writer_index};
            const ItemId clip_id{kCollaborationFirstClipId + writer_index};
            std::size_t writer_accepts = 0;
            for (std::size_t attempt = 0;
                 attempt < 16'384 && writer_accepts < kCollaborationEditsPerWriter; ++attempt) {
                const auto view = session->current();
                const auto* sequence = view.snapshot->find_sequence({2});
                const auto* track = sequence ? sequence->find_track(track_id) : nullptr;
                const auto* clip = track ? track->find_clip(clip_id) : nullptr;
                if (!clip) {
                    unexpected_errors.fetch_add(1, std::memory_order_relaxed);
                    break;
                }
                auto result = session->submit(
                    writers[writer_index],
                    session_move_transaction(writers[writer_index], view.revision, track_id, *clip));
                if (result) {
                    ++writer_accepts;
                    accepted.fetch_add(1, std::memory_order_relaxed);
                } else if (result.error().code != ConflictCode::StaleRevision) {
                    unexpected_errors.fetch_add(1, std::memory_order_relaxed);
                    break;
                }
            }
            if (writer_accepts != kCollaborationEditsPerWriter)
                incomplete_writers.fetch_add(1, std::memory_order_relaxed);
        });
    }
    for (auto& worker : workers)
        worker.join();
    const auto elapsed = std::chrono::steady_clock::now() - started;
    reader.request_stop();
    reader.join();

    constexpr auto expected_edits =
        kCollaborationWriterCount * kCollaborationEditsPerWriter;
    REQUIRE_FALSE(reader_failed.load(std::memory_order_relaxed));
    REQUIRE(reader_iterations.load(std::memory_order_relaxed) > 0);
    REQUIRE(unexpected_errors.load(std::memory_order_relaxed) == 0);
    REQUIRE(incomplete_writers.load(std::memory_order_relaxed) == 0);
    REQUIRE(accepted.load(std::memory_order_relaxed) == expected_edits);
    REQUIRE(session->revision().value == expected_edits);
    REQUIRE(session->journal().entries().size() == expected_edits);
    for (std::size_t index = 0; index < kCollaborationWriterCount; ++index) {
        const auto* clip = session->snapshot()
                               ->find_sequence({2})
                               ->find_track({kCollaborationFirstTrackId + index})
                               ->find_clip({kCollaborationFirstClipId + index});
        REQUIRE(clip);
        REQUIRE(clip->start().value == kCollaborationEditsPerWriter);
    }
    if (strict_performance())
        enforce_performance_budget("PULP_TIMELINE_N_WRITER_BUDGET_MS", elapsed);
}

TEST_CASE("five-hundred-twelve-deep undo and real journal replay stay within budgets",
          "[timeline][scale][performance][journal][undo]") {
#if defined(PULP_TEST_WITH_SANITIZER)
    SKIP("full-scale coverage runs in non-instrumented builds");
#endif
    const auto initial = collaboration_scale_project();
    SessionLimits limits;
    limits.undo.max_groups = kDeepUndoCount;
    limits.undo.max_retained_bytes = 64 * 1024 * 1024;
    limits.journal.max_transactions = 2 * kDeepUndoCount;
    limits.journal.max_commands = 2 * kDeepUndoCount;
    limits.journal.max_retained_bytes = 64 * 1024 * 1024;
    auto session = take(DocumentSession::create(initial, limits));
    auto writer = take(session->register_writer());
    const ItemId track_id{kCollaborationFirstTrackId};
    const ItemId clip_id{kCollaborationFirstClipId};

    const auto undo_started = std::chrono::steady_clock::now();
    for (std::size_t edit = 0; edit < kDeepUndoCount; ++edit) {
        const auto view = session->current();
        const auto* clip = view.snapshot->find_sequence({2})->find_track(track_id)->find_clip(clip_id);
        REQUIRE(clip);
        REQUIRE(session->submit(writer,
                                session_move_transaction(writer, view.revision, track_id, *clip)));
    }
    for (std::size_t undo = 0; undo < kDeepUndoCount; ++undo) {
        REQUIRE(session->can_undo());
        REQUIRE(session->undo(writer));
    }
    const auto undo_elapsed = std::chrono::steady_clock::now() - undo_started;

    REQUIRE_FALSE(session->can_undo());
    REQUIRE(session->can_redo());
    REQUIRE(session->revision().value == 2 * kDeepUndoCount);
    REQUIRE(session->journal().entries().size() == 2 * kDeepUndoCount);
    REQUIRE(session->snapshot()
                ->find_sequence({2})
                ->find_track(track_id)
                ->find_clip(clip_id)
                ->start()
                .value == 0);

    const auto journal = session->journal();
    const auto replay_started = std::chrono::steady_clock::now();
    const auto replayed = journal.replay(initial, {});
    const auto replay_elapsed = std::chrono::steady_clock::now() - replay_started;
    REQUIRE(replayed);
    const auto registry = take(make_builtin_timeline_registry());
    REQUIRE(take(serialize_project(replayed.value(), registry)).json ==
            take(serialize_project(*session->snapshot(), registry)).json);

    if (strict_performance()) {
        enforce_performance_budget("PULP_TIMELINE_DEEP_UNDO_BUDGET_MS", undo_elapsed);
        enforce_performance_budget("PULP_TIMELINE_JOURNAL_REPLAY_BUDGET_MS", replay_elapsed);
    }
}
