#include <pulp/playback/note_renderer.hpp>
#include <pulp/playback/program_compiler.hpp>
#include <pulp/midi/block_ops.hpp>

#include "harness/scoped_rt_process_probe.hpp"
#include "timebase_test_helpers.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <limits>
#include <memory>
#include <tuple>
#include <vector>

using namespace pulp;
using namespace pulp::playback;
using namespace pulp::timebase;
using namespace pulp::timeline;

namespace {

template <typename T, typename E>
T take(runtime::Result<T, E> result) {
    if (!result) std::abort();
    return std::move(result).value();
}

std::shared_ptr<const CompiledTempoMap> tempo_map() {
    const std::array points{TempoPoint{{0}, 120.0}};
    return shared_compiled_tempo_map(points, RationalRate{48'000, 1});
}

TickPosition tick_at_sample(const CompiledTempoMap& map, std::int64_t sample) {
    return map.samples_to_ticks({sample});
}

std::shared_ptr<const Project> note_project(const CompiledTempoMap& map,
                                            std::vector<NoteEvent> notes,
                                            std::int64_t clip_end_sample = 1'000) {
    auto content = take(NoteContent::create(std::move(notes)));
    const auto clip_duration = tick_at_sample(map, clip_end_sample) - TickPosition{0};
    auto clip = take(Clip::create({20}, {0}, clip_duration, std::move(content)));
    auto track = take(Track::create({10}, "notes", {std::move(clip)}));
    auto sequence = take(Sequence::create({2}, "root", clip_duration, {std::move(track)}));
    return std::make_shared<const Project>(take(Project::create(
        ProjectInput{{1}, "project", 10'000, {2}, {}, {std::move(sequence)}})));
}

NoteEvent note(const CompiledTempoMap& map, std::uint64_t id,
               std::int64_t start_sample, std::int64_t end_sample,
               std::uint8_t pitch, std::uint8_t channel = 0,
               std::uint16_t velocity = 0xffff) {
    const auto origin = tick_at_sample(map, 0);
    const TickPosition start{(tick_at_sample(map, start_sample) - origin).value};
    const TickPosition end{(tick_at_sample(map, end_sample) - origin).value};
    return {{id}, start, end - start, velocity, pitch, channel};
}

struct ProgramHarness {
    PlaybackProgramStore store;
    DeferredCompileExecutor executor;
    PlaybackProgramCompiler compiler{store, executor, std::chrono::microseconds(0)};

    void publish(std::shared_ptr<const Project> project,
                 std::shared_ptr<const CompiledTempoMap> map,
                 std::uint64_t revision, DirtyTrackSet dirty = {.all = true}) {
        ProgramCompileRequest request;
        request.project = std::move(project);
        request.sequence_id = {2};
        request.tempo_map = std::move(map);
        request.document_revision = revision;
        request.dirty = std::move(dirty);
        REQUIRE(compiler.submit(std::move(request)));
        while (compiler.status().busy)
            executor.run_for(std::chrono::seconds(1), 64);
        INFO("compile error code " << static_cast<int>(compiler.status().last_error.code)
             << " item " << compiler.status().last_error.item.value);
        REQUIRE_FALSE(compiler.status().has_error);
    }
};

class TinyBudgetExecutor final : public CompileExecutor {
  public:
    bool submit(std::unique_ptr<CompileTask> incoming,
                std::chrono::steady_clock::time_point) override {
        if (!incoming || task) return false;
        task = std::move(incoming);
        return true;
    }

    void run_one_work_unit() {
        if (!task) return;
        if (task->run_slice({std::chrono::steady_clock::now() + std::chrono::seconds(1), 1}) ==
            CompileTaskStatus::Complete)
            task.reset();
        ++slice_count;
    }

    std::unique_ptr<CompileTask> task;
    std::size_t slice_count = 0;
};

void prepare_playing_transport(MasterTransport& transport, const CompiledTempoMap& map,
                               std::uint32_t maximum = 1024, LoopRegion loop = {},
                               TickPosition initial = {}) {
    MasterTransportConfig config;
    config.max_buffer_size = maximum;
    config.initially_playing = true;
    config.loop = loop;
    config.initial_position = initial;
    REQUIRE(transport.prepare(map, config) == TransportError::None);
}

TransportSnapshot next_block(MasterTransport& transport, std::uint32_t frames) {
    TransportSnapshot snapshot;
    REQUIRE(transport.begin_block(frames, snapshot) == TransportError::None);
    return snapshot;
}

struct EventBytes {
    std::int64_t sample = 0;
    std::array<std::uint8_t, 3> bytes{};
    auto operator<=>(const EventBytes&) const = default;
};

std::vector<EventBytes> render_partition(ProgramHarness& programs,
                                         const CompiledTempoMap& map,
                                         std::span<const std::uint32_t> blocks) {
    ArrangementNoteRenderer renderer({10});
    REQUIRE(renderer.prepare(64));
    PlaybackProgramBlockLatch latch;
    MasterTransport transport;
    prepare_playing_transport(transport, map, 2048);
    std::vector<EventBytes> result;
    std::int64_t base = 0;
    for (const auto frames : blocks) {
        const auto snapshot = next_block(transport, frames);
        auto program = latch.begin_block(programs.store);
        REQUIRE(renderer.process(program, snapshot).code == NoteRenderCode::Ok);
        for (const auto& event : renderer.events())
            result.push_back({base + event.sample_offset,
                              {event.data()[0], event.data()[1], event.data()[2]}});
        base += frames;
    }
    return result;
}

} // namespace

TEST_CASE("note program compilation is deterministic and note offs win equal-sample ties") {
    const auto map = tempo_map();
    ProgramHarness programs;
    programs.publish(note_project(*map, {
        note(*map, 31, 30, 50, 62, 2, 0x8000),
        note(*map, 30, 10, 30, 60, 1),
    }), map, 1);

    const auto program = programs.store.read();
    const auto events = program->find_track({10})->arrangement_note_events();
    REQUIRE(events.size() == 4);
    REQUIRE(events[0].sample == SamplePosition{10});
    REQUIRE(events[0].kind == NoteProgramEventKind::On);
    REQUIRE(events[0].clip_id == ItemId{20});
    REQUIRE(events[0].note_id == ItemId{30});
    REQUIRE(events[1].sample == SamplePosition{30});
    REQUIRE(events[1].kind == NoteProgramEventKind::Off);
    REQUIRE(events[1].note_id == ItemId{30});
    REQUIRE(events[2].sample == SamplePosition{30});
    REQUIRE(events[2].kind == NoteProgramEventKind::On);
    REQUIRE(events[2].note_id == ItemId{31});
}

TEST_CASE("note renderer preserves native MIDI2 velocity beyond the MIDI1 mirror") {
    const auto map = tempo_map();
    ProgramHarness programs;
    programs.publish(note_project(*map, {
        note(*map, 30, 10, 20, 60, 0, 0x8000),
        note(*map, 31, 30, 40, 62, 0, 0x8001),
    }), map, 1);
    MasterTransport transport;
    prepare_playing_transport(transport, *map, 64);
    ArrangementNoteRenderer renderer({10});
    REQUIRE(renderer.prepare(8));
    PlaybackProgramBlockLatch latch;
    auto program = latch.begin_block(programs.store);

    REQUIRE(renderer.process(program, next_block(transport, 64)).code == NoteRenderCode::Ok);
    const auto& midi1 = renderer.events();
    REQUIRE(midi1.size() == 4);
    REQUIRE(midi1[0].velocity() == midi1[2].velocity());
    REQUIRE(midi1.ump() != nullptr);
    const auto& midi2 = *midi1.ump();
    REQUIRE(midi2.size() == midi1.size());
    REQUIRE(midi2[0].packet.message_type() == midi::UmpMessageType::Midi2ChannelVoice);
    REQUIRE((midi2[0].packet.status() & 0xf0u) == 0x90u);
    REQUIRE(midi2[0].packet.velocity_16() == 0x8000);
    REQUIRE((midi2[2].packet.status() & 0xf0u) == 0x90u);
    REQUIRE(midi2[2].packet.velocity_16() == 0x8001);
    REQUIRE(midi2[0].packet.velocity_16() != midi2[2].packet.velocity_16());
    REQUIRE(midi2[1].sample_offset == midi1[1].sample_offset);
    REQUIRE((midi2[1].packet.status() & 0xf0u) == 0x80u);

    midi::MidiBuffer routed;
    midi::UmpBuffer routed_ump;
    routed.reserve(8);
    routed.set_realtime_capacity_limit(true);
    routed_ump.reserve(8);
    routed_ump.set_realtime_capacity_limit(true);
    routed.attach_ump(&routed_ump);
    REQUIRE(midi::copy_midi_block(midi1, routed));
    REQUIRE(routed.size() == midi1.size());
    REQUIRE(routed_ump.size() == midi2.size());
    REQUIRE(routed_ump[0].packet.velocity_16() == 0x8000);
    REQUIRE(routed_ump[2].packet.velocity_16() == 0x8001);
}

TEST_CASE("many note merge sort advances one charged work unit per slice") {
    const auto map = tempo_map();
    std::vector<NoteEvent> notes;
    constexpr std::size_t note_count = 257;
    notes.reserve(note_count);
    for (std::size_t index = 0; index < note_count; ++index) {
        const auto start = static_cast<std::int64_t>(index * 2);
        // Descending durations make the compiler's generated off stream
        // maximally different from the NoteContent start ordering.
        const auto end = static_cast<std::int64_t>(2'000 - index);
        notes.push_back(note(*map, 1'000 + index, start, end,
                             static_cast<std::uint8_t>(index % 128)));
    }

    PlaybackProgramStore store;
    TinyBudgetExecutor executor;
    PlaybackProgramCompiler compiler(store, executor, std::chrono::microseconds(0));
    ProgramCompileRequest request{note_project(*map, std::move(notes), 2'100),
                                  {2}, map, 1, {.all = true}, {}};
    REQUIRE(compiler.submit(std::move(request)));
    while (compiler.status().busy && executor.slice_count < 100'000) {
        executor.run_one_work_unit();
    }

    REQUIRE_FALSE(compiler.status().busy);
    REQUIRE(executor.slice_count > note_count * 4);
    const auto program = store.read();
    REQUIRE(program);
    const auto events = program->find_track({10})->arrangement_note_events();
    REQUIRE(events.size() == note_count * 2);
    REQUIRE(std::is_sorted(events.begin(), events.end(), note_program_event_less));
    for (std::size_t index = 1; index < events.size(); ++index) {
        if (events[index - 1].sample == events[index].sample)
            REQUIRE_FALSE(note_program_event_less(events[index], events[index - 1]));
    }
}

TEST_CASE("note renderer golden stream is invariant under variable block partitioning") {
    const auto map = tempo_map();
    ProgramHarness programs;
    programs.publish(note_project(*map, {
        note(*map, 30, 10, 30, 60, 0),
        note(*map, 31, 30, 50, 62, 1, 0x8000),
        note(*map, 32, 80, 100, 64, 2, 1),
    }), map, 1);

    constexpr std::array one{std::uint32_t{128}};
    constexpr std::array varied{std::uint32_t{1}, std::uint32_t{64},
                                std::uint32_t{17}, std::uint32_t{46}};
    const auto golden = render_partition(programs, *map, one);
    const auto partitioned = render_partition(programs, *map, varied);
    REQUIRE(partitioned == golden);
    REQUIRE(golden.size() == 6);
    REQUIRE(golden[0] == EventBytes{10, {0x90, 60, 127}});
    REQUIRE(golden[1] == EventBytes{30, {0x80, 60, 0}});
    REQUIRE(golden[2] == EventBytes{30, {0x91, 62, 64}});
    REQUIRE(golden[5] == EventBytes{100, {0x82, 64, 0}});
}

TEST_CASE("one loop wrap flushes old notes before emitting the second range") {
    const auto map = tempo_map();
    ProgramHarness programs;
    programs.publish(note_project(*map, {
        note(*map, 30, 0, 5, 62),
        note(*map, 31, 95, 115, 60),
    }), map, 1);

    const LoopRegion loop{true, {0}, tick_at_sample(*map, 100)};
    MasterTransport transport;
    prepare_playing_transport(transport, *map, 20, loop, tick_at_sample(*map, 90));
    const auto snapshot = next_block(transport, 20);
    REQUIRE(snapshot.range_count == 2);
    ArrangementNoteRenderer renderer({10});
    REQUIRE(renderer.prepare(16));
    PlaybackProgramBlockLatch latch;
    auto program = latch.begin_block(programs.store);
    REQUIRE(renderer.process(program, snapshot).code == NoteRenderCode::Ok);
    REQUIRE(renderer.events().size() == 4);
    REQUIRE(renderer.events()[0].sample_offset == 5);
    REQUIRE(renderer.events()[0].is_note_on());
    REQUIRE(renderer.events()[0].note() == 60);
    REQUIRE(renderer.events()[1].sample_offset == 10);
    REQUIRE(renderer.events()[1].is_note_off());
    REQUIRE(renderer.events()[1].note() == 60);
    REQUIRE(renderer.events()[2].sample_offset == 10);
    REQUIRE(renderer.events()[2].is_note_on());
    REQUIRE(renderer.events()[2].note() == 62);
    REQUIRE(renderer.events()[3].sample_offset == 15);
    REQUIRE(renderer.events()[3].is_note_off());
}

TEST_CASE("seek stop and program adoption release notes without Phase 1 chase") {
    const auto map = tempo_map();
    ProgramHarness programs;
    programs.publish(note_project(*map, {note(*map, 30, 10, 100, 60)}), map, 1);
    MasterTransport transport;
    prepare_playing_transport(transport, *map, 128);
    ArrangementNoteRenderer renderer({10});
    REQUIRE(renderer.prepare(16));
    PlaybackProgramBlockLatch latch;

    {
        auto program = latch.begin_block(programs.store);
        REQUIRE(renderer.process(program, next_block(transport, 20)).code == NoteRenderCode::Ok);
        REQUIRE(renderer.events().size() == 1);
        REQUIRE(renderer.events()[0].is_note_on());
    }
    REQUIRE(transport.seek(tick_at_sample(*map, 50)) == TransportError::None);
    {
        auto program = latch.begin_block(programs.store);
        REQUIRE(renderer.process(program, next_block(transport, 20)).code == NoteRenderCode::Ok);
        REQUIRE(renderer.events().size() == 1);
        REQUIRE(renderer.events()[0].is_note_off());
        REQUIRE_FALSE(renderer.has_active_notes());
    }
    {
        auto program = latch.begin_block(programs.store);
        REQUIRE(renderer.process(program, next_block(transport, 40)).code == NoteRenderCode::Ok);
        REQUIRE(renderer.events().empty()); // note-off at 100 has no chased note-on.
    }

    REQUIRE(transport.seek({0}) == TransportError::None);
    {
        auto program = latch.begin_block(programs.store);
        (void)renderer.process(program, next_block(transport, 20));
        REQUIRE(renderer.has_active_notes());
    }
    programs.publish(note_project(*map, {}), map, 2, {false, {{10}}});
    {
        auto program = latch.begin_block(programs.store);
        const auto result = renderer.process(program, next_block(transport, 20));
        REQUIRE(result.adoption == ShellAdoptionResult::Adopted);
        REQUIRE(renderer.events().size() == 1);
        REQUIRE(renderer.events()[0].is_note_off());
        REQUIRE_FALSE(renderer.has_active_notes());
    }

    REQUIRE(transport.seek({0}) == TransportError::None);
    programs.publish(note_project(*map, {note(*map, 40, 10, 100, 60)}), map, 3,
                     {false, {{10}}});
    {
        auto program = latch.begin_block(programs.store);
        (void)renderer.process(program, next_block(transport, 20));
        REQUIRE(renderer.has_active_notes());
    }
    REQUIRE(transport.set_playing(false) == TransportError::None);
    {
        auto program = latch.begin_block(programs.store);
        REQUIRE(renderer.process(program, next_block(transport, 64)).code == NoteRenderCode::Ok);
        REQUIRE(renderer.events().size() == 1);
        REQUIRE(renderer.events()[0].is_note_off());
        REQUIRE_FALSE(renderer.has_active_notes());
    }
}

TEST_CASE("renderer overflow retains a deterministic prefix then flushes delivered state") {
    const auto map = tempo_map();
    ProgramHarness programs;
    programs.publish(note_project(*map, {
        note(*map, 30, 10, 100, 60),
        note(*map, 31, 10, 100, 61),
    }), map, 1);
    MasterTransport transport;
    prepare_playing_transport(transport, *map, 128);
    ArrangementNoteRenderer renderer({10});
    REQUIRE(renderer.prepare(1));
    PlaybackProgramBlockLatch latch;
    {
        auto program = latch.begin_block(programs.store);
        const auto result = renderer.process(program, next_block(transport, 20));
        REQUIRE(result.code == NoteRenderCode::OutputOverflow);
        REQUIRE(result.emitted_events == 1);
        REQUIRE(result.dropped_events == 1);
        REQUIRE(renderer.events()[0].note() == 60);
        REQUIRE(renderer.events().ump() != nullptr);
        REQUIRE(renderer.events().ump()->size() == renderer.events().size());
        REQUIRE((*renderer.events().ump())[0].packet.note_number() == 60);
    }
    {
        auto program = latch.begin_block(programs.store);
        const auto result = renderer.process(program, next_block(transport, 20));
        REQUIRE(result.code == NoteRenderCode::Ok);
        REQUIRE(renderer.events().size() == 1);
        REQUIRE(renderer.events()[0].is_note_off());
        REQUIRE(renderer.events()[0].note() == 60);
        REQUIRE(renderer.events().ump()->size() == renderer.events().size());
        REQUIRE(((*renderer.events().ump())[0].packet.status() & 0xf0u) == 0x80u);
        REQUIRE_FALSE(renderer.has_active_notes());
    }
}

TEST_CASE("overlapping MIDI keys release only at the final off and preserve retrigger ties") {
    const auto map = tempo_map();
    ProgramHarness programs;
    programs.publish(note_project(*map, {
        note(*map, 30, 10, 50, 60),
        note(*map, 31, 30, 70, 60),
        note(*map, 32, 70, 90, 60),
    }), map, 1);
    MasterTransport transport;
    prepare_playing_transport(transport, *map, 128);
    ArrangementNoteRenderer renderer({10});
    REQUIRE(renderer.prepare(16));
    PlaybackProgramBlockLatch latch;

    {
        auto program = latch.begin_block(programs.store);
        REQUIRE(renderer.process(program, next_block(transport, 40)).code == NoteRenderCode::Ok);
        REQUIRE(renderer.events().size() == 1); // second logical onset is folded.
        REQUIRE(renderer.events()[0].is_note_on());
        REQUIRE(renderer.events()[0].sample_offset == 10);
    }
    {
        auto program = latch.begin_block(programs.store);
        REQUIRE(renderer.process(program, next_block(transport, 20)).code == NoteRenderCode::Ok);
        REQUIRE(renderer.events().empty()); // first logical off leaves one overlap active.
        REQUIRE(renderer.has_active_notes());
    }
    {
        auto program = latch.begin_block(programs.store);
        REQUIRE(renderer.process(program, next_block(transport, 20)).code == NoteRenderCode::Ok);
        REQUIRE(renderer.events().size() == 2);
        REQUIRE(renderer.events()[0].sample_offset == 10);
        REQUIRE(renderer.events()[0].is_note_off());
        REQUIRE(renderer.events()[1].sample_offset == 10);
        REQUIRE(renderer.events()[1].is_note_on());
        REQUIRE(renderer.has_active_notes());
    }
    {
        auto program = latch.begin_block(programs.store);
        REQUIRE(renderer.process(program, next_block(transport, 20)).code == NoteRenderCode::Ok);
        REQUIRE(renderer.events().size() == 1);
        REQUIRE(renderer.events()[0].sample_offset == 10);
        REQUIRE(renderer.events()[0].is_note_off());
        REQUIRE_FALSE(renderer.has_active_notes());
    }
}

TEST_CASE("overlap adoption emits one physical flush and clears every logical count") {
    const auto map = tempo_map();
    ProgramHarness programs;
    programs.publish(note_project(*map, {
        note(*map, 30, 10, 100, 60),
        note(*map, 31, 20, 110, 60),
    }), map, 1);
    MasterTransport transport;
    prepare_playing_transport(transport, *map, 128);
    ArrangementNoteRenderer renderer({10});
    REQUIRE(renderer.prepare(16));
    PlaybackProgramBlockLatch latch;
    {
        auto program = latch.begin_block(programs.store);
        REQUIRE(renderer.process(program, next_block(transport, 40)).code == NoteRenderCode::Ok);
        REQUIRE(renderer.events().size() == 1);
        REQUIRE(renderer.has_active_notes());
    }
    programs.publish(note_project(*map, {}), map, 2, {false, {{10}}});
    {
        auto program = latch.begin_block(programs.store);
        REQUIRE(renderer.process(program, next_block(transport, 20)).code == NoteRenderCode::Ok);
        REQUIRE(renderer.events().size() == 1);
        REQUIRE(renderer.events()[0].is_note_off());
        REQUIRE_FALSE(renderer.has_active_notes());
    }
}

TEST_CASE("logical overlap saturation fails closed in the same block") {
    const auto map = tempo_map();
    std::vector<NoteEvent> notes;
    notes.reserve(ArrangementNoteRenderer::maximum_logical_overlap + 1u);
    for (std::uint16_t index = 0;
         index <= ArrangementNoteRenderer::maximum_logical_overlap; ++index) {
        notes.push_back(note(*map, 1000u + index, 10, 100, 60));
    }
    ProgramHarness programs;
    programs.publish(note_project(*map, std::move(notes)), map, 1);
    MasterTransport transport;
    prepare_playing_transport(transport, *map, 128);
    ArrangementNoteRenderer renderer({10});
    REQUIRE(renderer.prepare(4));
    PlaybackProgramBlockLatch latch;

    auto program = latch.begin_block(programs.store);
    const auto result = renderer.process(program, next_block(transport, 20));
    REQUIRE(result.code == NoteRenderCode::ActiveStateOverflow);
    REQUIRE(renderer.events().size() == 2);
    REQUIRE(renderer.events()[0].is_note_on());
    REQUIRE(renderer.events()[0].sample_offset == 10);
    REQUIRE(renderer.events()[1].is_note_off());
    REQUIRE(renderer.events()[1].sample_offset == 10);
    REQUIRE_FALSE(renderer.has_active_notes());
}

TEST_CASE("renderer rejects malformed transport ranges and tempo-map identity mismatch") {
    const auto map = tempo_map();
    ProgramHarness programs;
    programs.publish(note_project(*map, {note(*map, 30, 10, 100, 60)}), map, 1);
    PlaybackProgramBlockLatch latch;
    ArrangementNoteRenderer renderer({10});
    REQUIRE(renderer.prepare(16));

    MasterTransport valid_transport;
    prepare_playing_transport(valid_transport, *map, 128);
    auto valid = next_block(valid_transport, 64);
    auto program = latch.begin_block(programs.store);
    REQUIRE(renderer.process(program, valid).code == NoteRenderCode::Ok);

    auto malformed = valid;
    malformed.block_index += 1;
    malformed.range_count = 2;
    malformed.ranges[1] = {};
    const auto malformed_result = renderer.process(program, malformed);
    REQUIRE(malformed_result.code == NoteRenderCode::InvalidTransport);
    REQUIRE(renderer.events().size() == 1); // active note failed closed at offset zero.
    REQUIRE(renderer.events()[0].is_note_off());

    const auto other_map = tempo_map();
    MasterTransport mismatched_transport;
    prepare_playing_transport(mismatched_transport, *other_map, 128);
    const auto mismatch = renderer.process(program, next_block(mismatched_transport, 64));
    REQUIRE(mismatch.code == NoteRenderCode::TempoMapMismatch);
    REQUIRE(mismatch.adoption == ShellAdoptionResult::Rejected);
    REQUIRE(renderer.events().empty());

    auto largest_representable = valid;
    largest_representable.block_index += 3;
    largest_representable.frame_count =
        static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max());
    largest_representable.ranges[0].frame_count = largest_representable.frame_count;
    largest_representable.range_count = 1;
    largest_representable.is_playing = false;
    REQUIRE(renderer.process(program, largest_representable).code == NoteRenderCode::Ok);

    auto negative_offset_risk = largest_representable;
    negative_offset_risk.block_index += 1;
    ++negative_offset_risk.frame_count;
    negative_offset_risk.ranges[0].frame_count = negative_offset_risk.frame_count;
    REQUIRE(renderer.process(program, negative_offset_risk).code ==
            NoteRenderCode::InvalidTransport);
    REQUIRE(renderer.events().empty());
    REQUIRE(renderer.events().ump()->empty());
}

TEST_CASE("note range offsets are exact at signed sample-position extremes") {
    using timebase::SamplePosition;
    std::uint32_t offset = 999;
    constexpr auto low = std::numeric_limits<std::int64_t>::min();
    constexpr auto high = std::numeric_limits<std::int64_t>::max();

    REQUIRE(playback::detail::note_event_offset_in_range(
        SamplePosition{low + 63}, SamplePosition{low}, 64, offset));
    REQUIRE(offset == 63);
    REQUIRE_FALSE(playback::detail::note_event_offset_in_range(
        SamplePosition{low + 64}, SamplePosition{low}, 64, offset));
    REQUIRE(playback::detail::note_event_offset_in_range(
        SamplePosition{high}, SamplePosition{high - 63}, 64, offset));
    REQUIRE(offset == 63);
    REQUIRE_FALSE(playback::detail::note_event_offset_in_range(
        SamplePosition{low}, SamplePosition{high}, 64, offset));
    REQUIRE_FALSE(playback::detail::note_event_offset_in_range(
        SamplePosition{high}, SamplePosition{low}, 64, offset));
}

TEST_CASE("renderer process is allocation and lock free after prepare") {
    const auto map = tempo_map();
    ProgramHarness programs;
    programs.publish(note_project(*map, {note(*map, 30, 10, 30, 60)}), map, 1);
    MasterTransport transport;
    prepare_playing_transport(transport, *map, 128);
    const auto snapshot = next_block(transport, 64);
    ArrangementNoteRenderer renderer({10});
    REQUIRE(renderer.prepare(16));
    PlaybackProgramBlockLatch latch;
    auto program = latch.begin_block(programs.store);

    std::size_t allocations = 1;
    {
        test::ScopedRtProcessProbe probe;
        REQUIRE(renderer.process(program, snapshot).code == NoteRenderCode::Ok);
        allocations = probe.allocation_count();
    }
    REQUIRE(allocations == 0);
}

TEST_CASE("compiler rejects note ranges outside their clip and sub-sample notes") {
    const auto map = tempo_map();
    ProgramHarness programs;
    auto outside = note(*map, 30, 900, 1'100, 60);
    auto project = note_project(*map, {outside}, 1'000);
    ProgramCompileRequest request{project, {2}, map, 1, {.all = true}, {}};
    REQUIRE(programs.compiler.submit(std::move(request)));
    while (programs.compiler.status().busy)
        programs.executor.run_for(std::chrono::seconds(1), 64);
    REQUIRE(programs.compiler.status().has_error);
    REQUIRE(programs.compiler.status().last_error.code == CompileErrorCode::InvalidStructure);
    REQUIRE(programs.compiler.status().last_error.item == ItemId{30});

    ProgramHarness sub_sample_programs;
    NoteEvent sub_sample{{31}, {1}, {1}, 0xffff, 60, 0};
    auto sub_sample_project = note_project(*map, {sub_sample}, 1'000);
    ProgramCompileRequest sub_request{sub_sample_project, {2}, map, 1, {.all = true}, {}};
    REQUIRE(sub_sample_programs.compiler.submit(std::move(sub_request)));
    while (sub_sample_programs.compiler.status().busy)
        sub_sample_programs.executor.run_for(std::chrono::seconds(1), 64);
    REQUIRE(sub_sample_programs.compiler.status().has_error);
    REQUIRE(sub_sample_programs.compiler.status().last_error.item == ItemId{31});
}
