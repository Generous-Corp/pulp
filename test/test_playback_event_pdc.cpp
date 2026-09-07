// Event-stream plug-in delay compensation: the shift contract, and the
// scheduling behaviour it buys. The rule under test is that compensation moves
// the SCHEDULING WINDOW and never the event data, so a compensated stream lands
// on exactly the sample the document authored.
#include <pulp/playback/event_compensation.hpp>
#include <pulp/playback/note_renderer.hpp>
#include <pulp/playback/program_compiler.hpp>

#include "timebase_test_helpers.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <limits>
#include <memory>
#include <span>
#include <vector>

using namespace pulp;
using namespace pulp::playback;
using namespace pulp::timebase;
using namespace pulp::timeline;

namespace {

template <typename T, typename E> T unwrap(runtime::Result<T, E> result) {
    if (!result)
        std::abort();
    return std::move(result).value();
}

std::shared_ptr<const CompiledTempoMap> flat_map() {
    const std::array points{TempoPoint{{0}, 120.0}};
    return shared_compiled_tempo_map(points, RationalRate{48'000, 1});
}

/// Two tempo segments, so a shifted window can be made to span the boundary.
std::shared_ptr<const CompiledTempoMap> ramped_map() {
    const std::array points{TempoPoint{{0}, 120.0}, TempoPoint{{705'600}, 60.0}};
    return shared_compiled_tempo_map(points, RationalRate{48'000, 1});
}

NoteEvent authored_note(const CompiledTempoMap& map, std::uint64_t id, std::int64_t start_sample,
                        std::int64_t end_sample, std::uint8_t pitch) {
    const auto origin = map.samples_to_ticks({0});
    const TickPosition start{(map.samples_to_ticks({start_sample}) - origin).value};
    const TickPosition end{(map.samples_to_ticks({end_sample}) - origin).value};
    return {{id}, start, end - start, 0xffff, pitch, 0};
}

std::shared_ptr<const Project> project_with(const CompiledTempoMap& map,
                                            std::vector<NoteEvent> notes,
                                            std::int64_t clip_end_sample) {
    auto content = unwrap(MidiContent::create(std::move(notes)));
    const auto duration = map.samples_to_ticks({clip_end_sample}) - TickPosition{0};
    auto clip = unwrap(Clip::create({20}, {0}, duration, std::move(content)));
    auto track = unwrap(Track::create({10}, "notes", {std::move(clip)}));
    auto sequence = unwrap(Sequence::create({2}, "root", duration, {std::move(track)}));
    return std::make_shared<const Project>(unwrap(
        Project::create(ProjectInput{{1}, "event pdc", 10'000, {2}, {}, {std::move(sequence)}})));
}

struct Programs {
    PlaybackProgramStore store;
    DeferredCompileExecutor executor;
    PlaybackProgramCompiler compiler{store, executor, std::chrono::microseconds(0)};

    void publish(std::shared_ptr<const Project> project,
                 std::shared_ptr<const CompiledTempoMap> map) {
        ProgramCompileRequest request;
        request.project = std::move(project);
        request.sequence_id = {2};
        request.tempo_map = std::move(map);
        request.sample_rate = request.tempo_map->sample_rate();
        request.document_revision = 1;
        request.dirty.all = true;
        REQUIRE(compiler.submit(std::move(request)));
        while (compiler.status().busy)
            executor.run_for(std::chrono::seconds(1), 64);
        REQUIRE_FALSE(compiler.status().has_error);
    }
};

/// One emitted physical event, restated in absolute playback samples so two
/// different block partitions are directly comparable.
struct Emitted {
    std::int64_t sample = 0;
    std::uint8_t status = 0;
    std::uint8_t data1 = 0;
    auto operator<=>(const Emitted&) const = default;
};

void collect(const midi::MidiBuffer& buffer, std::int64_t block_origin,
             std::vector<Emitted>& into) {
    for (const auto& event : buffer)
        into.push_back({block_origin + event.sample_offset, event.data()[0], event.data()[1]});
}

void start_playing(MasterTransport& transport, const CompiledTempoMap& map,
                   std::uint32_t maximum, LoopRegion loop = {}) {
    MasterTransportConfig config;
    config.max_buffer_size = maximum;
    config.initially_playing = true;
    config.loop = loop;
    REQUIRE(transport.prepare(map, config) == TransportError::None);
}

/// Renders `total_frames` of playback through `blocks` (cycled), returning every
/// emitted event in absolute playback samples.
std::vector<Emitted> render(const PlaybackProgramStore& store, const CompiledTempoMap& map,
                            EventCompensationShift shift, std::span<const std::uint32_t> blocks,
                            std::int64_t total_frames, LoopRegion loop = {}) {
    auto program = store.read();
    REQUIRE(program);
    PlaybackProgramBlock block(program.get());
    ArrangementNoteRenderer renderer({10});
    REQUIRE(renderer.prepare(256));
    MasterTransport transport;
    std::uint32_t maximum = 0;
    for (const auto candidate : blocks)
        maximum = std::max(maximum, candidate);
    start_playing(transport, map, maximum, loop);
    std::vector<Emitted> emitted;
    std::int64_t origin = 0;
    std::size_t index = 0;
    while (origin < total_frames) {
        const auto frames = blocks[index++ % blocks.size()];
        TransportSnapshot snapshot;
        REQUIRE(transport.begin_block(frames, snapshot) == TransportError::None);
        const auto result = renderer.process(block, snapshot, shift);
        REQUIRE(result.code == NoteRenderCode::Ok);
        collect(renderer.events(), origin, emitted);
        origin += frames;
    }
    std::sort(emitted.begin(), emitted.end());
    return emitted;
}

std::vector<Emitted> note_on_positions(const std::vector<Emitted>& events, std::uint8_t pitch) {
    std::vector<Emitted> result;
    for (const auto& event : events)
        if ((event.status & 0xf0u) == 0x90u && event.data1 == pitch)
            result.push_back(event);
    return result;
}

} // namespace

TEST_CASE("event chain shift accumulates only what the caller may express", "[event-pdc]") {
    constexpr int kCeiling = 65'535;
    SECTION("an empty chain compensates nothing") {
        const auto resolved = accumulate_event_chain_shift({}, kCeiling);
        REQUIRE(resolved);
        REQUIRE(resolved.shift.samples == 0);
        REQUIRE_FALSE(resolved.shift.compensating());
    }
    SECTION("latencies sum") {
        const std::array chain{128, 64, 7};
        const auto resolved = accumulate_event_chain_shift(chain, kCeiling);
        REQUIRE(resolved);
        REQUIRE(resolved.shift.samples == 199);
        REQUIRE(resolved.shift.compensating());
    }
    SECTION("a chain sitting exactly on the ceiling is admitted") {
        const std::array chain{kCeiling};
        const auto resolved = accumulate_event_chain_shift(chain, kCeiling);
        REQUIRE(resolved);
        REQUIRE(resolved.shift.samples == kCeiling);
    }
    SECTION("a negative report is refused rather than clamped") {
        const std::array chain{16, -1};
        const auto resolved = accumulate_event_chain_shift(chain, kCeiling);
        REQUIRE_FALSE(resolved);
        REQUIRE(resolved.code == EventCompensationCode::NegativeDeviceLatency);
        REQUIRE(resolved.actual == -1);
        REQUIRE(resolved.index == 1);
    }
    SECTION("one device past the ceiling reports its own number") {
        const std::array chain{kCeiling + 1};
        const auto resolved = accumulate_event_chain_shift(chain, kCeiling);
        REQUIRE_FALSE(resolved);
        REQUIRE(resolved.code == EventCompensationCode::DeviceLatencyOutOfRange);
        REQUIRE(resolved.actual == kCeiling + 1);
        REQUIRE(resolved.limit == kCeiling);
    }
    SECTION("a chain that only exceeds the ceiling in total is refused too") {
        const std::array chain{40'000, 40'000};
        const auto resolved = accumulate_event_chain_shift(chain, kCeiling);
        REQUIRE_FALSE(resolved);
        REQUIRE(resolved.code == EventCompensationCode::ChainShiftOutOfRange);
        REQUIRE(resolved.actual == 80'000);
        REQUIRE(resolved.limit == kCeiling);
    }
}

TEST_CASE("a shifted range origin saturates instead of wrapping", "[event-pdc]") {
    TransportRange range;
    range.timeline_sample_start = {1'000};
    REQUIRE(shifted_range_origin(range, {}).value == 1'000);
    REQUIRE(shifted_range_origin(range, {256}).value == 1'256);
    REQUIRE(shifted_range_origin(range, {-256}).value == 744);
    range.timeline_sample_start = {std::numeric_limits<std::int64_t>::max() - 4};
    REQUIRE(shifted_range_origin(range, {64}).value == std::numeric_limits<std::int64_t>::max());
    range.timeline_sample_start = {std::numeric_limits<std::int64_t>::min() + 4};
    REQUIRE(shifted_range_origin(range, {-64}).value == std::numeric_limits<std::int64_t>::min());
}

TEST_CASE("a host-beat-mapped range admits no sample-domain shift", "[event-pdc]") {
    TransportRange range;
    // Control: the same range accepts an uncompensated stream, so the predicate
    // is discriminating rather than unconditionally refusing.
    range.host_beat_mapping = true;
    REQUIRE(range_admits_event_compensation(range, {}));
    REQUIRE_FALSE(range_admits_event_compensation(range, {128}));
    range.host_beat_mapping = false;
    REQUIRE(range_admits_event_compensation(range, {128}));
}

TEST_CASE("live input and a compensated event chain cannot both be honoured", "[event-pdc]") {
    ProviderSelectorProgram scheduled_only;
    scheduled_only.available_mask = 1u << static_cast<unsigned>(ProviderKind::Arrangement);
    ProviderSelectorProgram with_live = scheduled_only;
    with_live.available_mask |= 1u << static_cast<unsigned>(ProviderKind::ExternalInput);

    REQUIRE(event_compensation_admits_live_input({}, scheduled_only));
    // Controls on both axes: an uncompensated chain may offer live input, and a
    // compensated chain is fine as long as it does not.
    REQUIRE(event_compensation_admits_live_input({}, with_live));
    REQUIRE(event_compensation_admits_live_input({512}, scheduled_only));
    REQUIRE_FALSE(event_compensation_admits_live_input({512}, with_live));
}

TEST_CASE("a compensated stream is read early by exactly the shift", "[event-pdc]") {
    const auto map = flat_map();
    Programs programs;
    programs.publish(project_with(*map, {authored_note(*map, 30, 600, 800, 64)}, 2'000), map);
    const std::array<std::uint32_t, 1> blocks{64};

    const auto uncompensated = render(programs.store, *map, {}, blocks, 1'600);
    const auto compensated = render(programs.store, *map, {192}, blocks, 1'600);
    const auto reference = note_on_positions(uncompensated, 64);
    const auto shifted = note_on_positions(compensated, 64);
    REQUIRE(reference.size() == 1);
    REQUIRE(shifted.size() == 1);
    REQUIRE(reference.front().sample == 600);
    // Read 192 samples early, so a chain that delays events by 192 puts them
    // back on 600.
    REQUIRE(shifted.front().sample == 600 - 192);
}

TEST_CASE("a tempo change inside the shifted window does not scale the shift", "[event-pdc]") {
    const auto map = ramped_map();
    const auto boundary = map->ticks_to_samples({705'600}).value;
    Programs programs;
    // The onset sits after the tempo point and the shift spans it, so a shift
    // expressed in musical time would land somewhere else.
    programs.publish(
        project_with(*map, {authored_note(*map, 31, boundary + 256, boundary + 512, 67)},
                     boundary + 4'000),
        map);
    const std::array<std::uint32_t, 1> blocks{128};
    const std::int64_t span = boundary + 2'000;

    const auto reference = note_on_positions(render(programs.store, *map, {}, blocks, span), 67);
    const auto shifted = note_on_positions(render(programs.store, *map, {1'024}, blocks, span), 67);
    REQUIRE(reference.size() == 1);
    REQUIRE(shifted.size() == 1);
    REQUIRE(reference.front().sample == boundary + 256);
    REQUIRE(shifted.front().sample == boundary + 256 - 1'024);
}

TEST_CASE("a compensated stream is identical across block partitions", "[event-pdc]") {
    const auto map = flat_map();
    Programs programs;
    programs.publish(project_with(*map,
                                  {
                                      authored_note(*map, 40, 300, 700, 60),
                                      authored_note(*map, 41, 705, 900, 62),
                                      authored_note(*map, 42, 1'100, 1'400, 64),
                                  },
                                  3'000),
                     map);
    // A shift that divides neither block size, and one longer than either block.
    const EventCompensationShift shift{291};
    const std::array<std::uint32_t, 1> thirty_two{32};
    const std::array<std::uint32_t, 1> forty_eight{48};
    const std::array<std::uint32_t, 3> ragged{17, 96, 5};

    const auto a = render(programs.store, *map, shift, thirty_two, 2'400);
    const auto b = render(programs.store, *map, shift, forty_eight, 2'400);
    const auto c = render(programs.store, *map, shift, ragged, 2'400);
    REQUIRE_FALSE(a.empty());
    REQUIRE(a == b);
    REQUIRE(a == c);

    const EventCompensationShift long_shift{600};
    const auto d = render(programs.store, *map, long_shift, thirty_two, 2'400);
    const auto e = render(programs.store, *map, long_shift, ragged, 2'400);
    REQUIRE_FALSE(d.empty());
    REQUIRE(d == e);
    // Control: the long shift really moved the stream, so the equality above is
    // not two identically empty or identically unshifted renders.
    REQUIRE(d != a);
}

TEST_CASE("one whole-span block matches many small blocks under a shift", "[event-pdc]") {
    const auto map = flat_map();
    Programs programs;
    programs.publish(project_with(*map,
                                  {
                                      authored_note(*map, 50, 200, 640, 60),
                                      authored_note(*map, 51, 900, 1'000, 62),
                                  },
                                  3'000),
                     map);
    const EventCompensationShift shift{137};
    const std::array<std::uint32_t, 1> whole{2'048};
    const std::array<std::uint32_t, 1> small{37};
    const auto offline = render(programs.store, *map, shift, whole, 2'048);
    const auto realtime = render(programs.store, *map, shift, small, 2'048);
    REQUIRE_FALSE(offline.empty());
    REQUIRE(offline == realtime);
}

TEST_CASE("a shift is applied per transport range, and refuses to read past a loop end",
          "[event-pdc]") {
    const auto map = flat_map();
    Programs programs;
    programs.publish(project_with(*map,
                                  {
                                      authored_note(*map, 60, 64, 200, 60),
                                      authored_note(*map, 61, 700, 900, 62),
                                  },
                                  4'000),
                     map);
    const LoopRegion loop{true, {0}, map->samples_to_ticks({1'024})};
    const EventCompensationShift shift{80};
    auto program = programs.store.read();
    REQUIRE(program);
    PlaybackProgramBlock block(program.get());

    // Control: the same loop, the same blocks, no compensation. A full pass
    // renders and the onset lands where the document authored it.
    {
        ArrangementNoteRenderer renderer({10});
        REQUIRE(renderer.prepare(256));
        MasterTransport transport;
        start_playing(transport, *map, 96, loop);
        std::vector<Emitted> emitted;
        for (std::int64_t origin = 0; origin < 2'048; origin += 96) {
            TransportSnapshot snapshot;
            REQUIRE(transport.begin_block(96, snapshot) == TransportError::None);
            const auto result = renderer.process(block, snapshot, {});
            REQUIRE(result.code == NoteRenderCode::Ok);
            collect(renderer.events(), origin, emitted);
        }
        const auto onsets = note_on_positions(emitted, 62);
        REQUIRE(onsets.size() == 2);
        REQUIRE(onsets[0].sample == 700);
        REQUIRE(onsets[1].sample - onsets[0].sample == 1'024);
    }

    // Compensated: each range is shifted from its OWN origin, so the onset well
    // inside the pass moves early by exactly the shift. The block whose window
    // would run past the loop point refuses instead: what belongs there is the
    // content after the wrap, and wrap-aware read-ahead does not exist.
    ArrangementNoteRenderer renderer({10});
    REQUIRE(renderer.prepare(256));
    MasterTransport transport;
    start_playing(transport, *map, 96, loop);
    std::vector<Emitted> emitted;
    bool refused = false;
    std::int64_t frames_before_refusal = 0;
    for (std::int64_t origin = 0; origin < 2'048; origin += 96) {
        TransportSnapshot snapshot;
        REQUIRE(transport.begin_block(96, snapshot) == TransportError::None);
        const auto result = renderer.process(block, snapshot, shift);
        if (result.code == NoteRenderCode::CompensationLoopWrapUnsupported) {
            refused = true;
            frames_before_refusal = origin;
            break;
        }
        REQUIRE(result.code == NoteRenderCode::Ok);
        collect(renderer.events(), origin, emitted);
    }
    REQUIRE(refused);
    // The refusal is at the loop end, not at the first block: the compensated
    // stream really ran past the onset at 620 before the window reached the
    // loop point, and that onset proves the shift was applied.
    REQUIRE(frames_before_refusal > 620);
    std::sort(emitted.begin(), emitted.end());
    const auto onsets = note_on_positions(emitted, 62);
    REQUIRE(onsets.size() == 1);
    REQUIRE(onsets.front().sample == 700 - 80);
}

TEST_CASE("a loop admits read-ahead that stays inside it", "[event-pdc]") {
    TransportRange range;
    range.frame_count = 64;
    range.timeline_sample_start = {512};
    const timebase::SamplePosition loop_end{1'024};
    // Control: an uncompensated window is never bounded by this guard.
    REQUIRE(loop_admits_event_compensation(range, {}, loop_end));
    REQUIRE(loop_admits_event_compensation(range, {128}, loop_end));
    range.timeline_sample_start = {896};
    // 896 + 64 + 64 == 1024, exactly the loop end, which is still inside.
    REQUIRE(loop_admits_event_compensation(range, {64}, loop_end));
    REQUIRE_FALSE(loop_admits_event_compensation(range, {65}, loop_end));
}

TEST_CASE("a changed shift is held until the transport stops", "[event-pdc]") {
    const auto map = flat_map();
    Programs programs;
    programs.publish(project_with(*map, {authored_note(*map, 70, 900, 1'100, 65)}, 3'000), map);
    auto program = programs.store.read();
    REQUIRE(program);
    PlaybackProgramBlock block(program.get());
    ArrangementNoteRenderer renderer({10});
    REQUIRE(renderer.prepare(256));

    MasterTransport transport;
    MasterTransportConfig config;
    config.max_buffer_size = 256;
    config.initially_playing = true;
    REQUIRE(transport.prepare(*map, config) == TransportError::None);

    TransportSnapshot snapshot;
    REQUIRE(transport.begin_block(64, snapshot) == TransportError::None);
    auto result = renderer.process(block, snapshot, {128});
    REQUIRE(result.code == NoteRenderCode::Ok);
    REQUIRE(result.applied_shift.samples == 128);
    REQUIRE_FALSE(result.shift_relatch_pending);

    // A device reports new latency mid-stream. Re-aligning now would move every
    // later event by the delta, so the older alignment is held.
    for (int index = 0; index < 4; ++index) {
        REQUIRE(transport.begin_block(64, snapshot) == TransportError::None);
        result = renderer.process(block, snapshot, {512});
        REQUIRE(result.code == NoteRenderCode::Ok);
        REQUIRE(result.applied_shift.samples == 128);
        REQUIRE(result.shift_relatch_pending);
    }
    REQUIRE(renderer.applied_shift().samples == 128);

    REQUIRE(transport.set_playing(false) == TransportError::None);
    REQUIRE(transport.begin_block(64, snapshot) == TransportError::None);
    REQUIRE_FALSE(snapshot.is_playing);
    result = renderer.process(block, snapshot, {512});
    REQUIRE(result.applied_shift.samples == 512);
    REQUIRE_FALSE(result.shift_relatch_pending);

    REQUIRE(transport.set_playing(true) == TransportError::None);
    REQUIRE(transport.begin_block(64, snapshot) == TransportError::None);
    result = renderer.process(block, snapshot, {512});
    REQUIRE(result.code == NoteRenderCode::Ok);
    REQUIRE(result.applied_shift.samples == 512);
    REQUIRE_FALSE(result.shift_relatch_pending);
}

TEST_CASE("a compensating shift fails closed on a host-beat-mapped range", "[event-pdc]") {
    const auto map = flat_map();
    Programs programs;
    programs.publish(project_with(*map, {authored_note(*map, 80, 100, 300, 60)}, 2'000), map);
    auto program = programs.store.read();
    REQUIRE(program);
    PlaybackProgramBlock block(program.get());
    ArrangementNoteRenderer renderer({10});
    REQUIRE(renderer.prepare(256));

    TransportSnapshot snapshot;
    snapshot.tempo_map = map.get();
    snapshot.sample_rate = map->sample_rate();
    snapshot.frame_count = 64;
    snapshot.is_playing = true;
    snapshot.range_count = 1;
    snapshot.ranges[0].frame_count = 64;
    snapshot.ranges[0].timeline_tick_end = map->samples_to_ticks({64});
    snapshot.ranges[0].host_beat_mapping = true;
    snapshot.ranges[0].has_precise_host_ticks = true;
    snapshot.ranges[0].host_tick_end = 64.0;

    // Control first: the same host-mapped range renders normally when nothing
    // asks it to compensate, so the refusal below is about the shift.
    auto result = renderer.process(block, snapshot, {});
    REQUIRE(result.code == NoteRenderCode::Ok);

    result = renderer.process(block, snapshot, {256});
    REQUIRE(result.code == NoteRenderCode::CompensationUnsupported);
}
