#include <pulp/playback/automation_cursor.hpp>

#include "harness/scoped_rt_process_probe.hpp"
#include "timebase_test_helpers.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdlib>
#include <limits>
#include <memory>
#include <vector>

using namespace pulp;
using namespace pulp::playback;
using namespace pulp::timebase;
using namespace pulp::timeline;

namespace {

template <typename T, typename E> T take(runtime::Result<T, E> result) {
    if (!result)
        std::abort();
    return std::move(result).value();
}

std::shared_ptr<const CompiledTempoMap> constant_map() {
    const std::array points{TempoPoint{{0}, 60.0}};
    return shared_compiled_tempo_map(points, RationalRate{48'000, 1});
}

std::shared_ptr<const CompiledTempoMap> constant_map(double bpm) {
    const std::array points{TempoPoint{{0}, bpm}};
    return shared_compiled_tempo_map(points, RationalRate{48'000, 1});
}

AutomationPoint point(const CompiledTempoMap& map, std::uint64_t id, std::int64_t sample,
                      float value,
                      AutomationInterpolation interpolation = AutomationInterpolation::Continuous,
                      float curvature = 0.0f) {
    return {{id}, map.samples_to_ticks({sample}), value, interpolation, curvature};
}

AutomationLane lane(ItemId id, std::vector<AutomationPoint> points) {
    auto curve = take(AutomationCurve::create(std::move(points)));
    return take(AutomationLane::create(id, DeviceParameterTarget{{99}, 7}, std::move(curve)));
}

std::shared_ptr<const AutomationProgram> program(const AutomationLane& source,
                                                 std::shared_ptr<const CompiledTempoMap> map,
                                                 ProgramGeneration generation = 1) {
    return take(AutomationProgram::compile(source, std::move(map), generation));
}

TransportSnapshot block(MasterTransport& transport, std::uint32_t frames) {
    TransportSnapshot snapshot;
    REQUIRE(transport.begin_block(frames, snapshot) == TransportError::None);
    return snapshot;
}

void prepare_transport(MasterTransport& value, const CompiledTempoMap& map, std::uint32_t maximum,
                       bool playing = true, TickPosition initial = {}, LoopRegion loop = {}) {
    MasterTransportConfig config;
    config.max_buffer_size = maximum;
    config.initially_playing = playing;
    config.initial_position = initial;
    config.loop = loop;
    REQUIRE(value.prepare(map, config) == TransportError::None);
}

} // namespace

TEST_CASE("automation program validates identity and preserves same-sample knots") {
    const auto map = constant_map();
    auto source = lane({1}, {{{10}, {0}, 0.1f}, {{11}, {1}, 0.4f}, point(*map, 12, 10, 0.9f)});

    auto missing = AutomationProgram::compile(source, {}, 1);
    REQUIRE_FALSE(missing);
    REQUIRE(missing.error().code == AutomationProgramErrorCode::MissingTempoMap);
    auto invalid_generation = AutomationProgram::compile(source, map, 0);
    REQUIRE_FALSE(invalid_generation);
    REQUIRE(invalid_generation.error().code == AutomationProgramErrorCode::InvalidGeneration);

    const auto compiled = program(source, map);
    REQUIRE(compiled->lane_id() == ItemId{1});
    REQUIRE(compiled->target() == DeviceParameterTarget{{99}, 7});
    REQUIRE(compiled->segments().size() == 3);
    REQUIRE(compiled->segments()[0].start_tick == TickPosition{0});
    REQUIRE(compiled->segments()[0].end_tick == TickPosition{1});
    REQUIRE(compiled->segments()[1].start_value == 0.4f);
    REQUIRE(compiled->segments()[2].start_value == 0.9f);
    REQUIRE(compiled->segments()[2].start_tick == compiled->segments()[2].end_tick);
}

TEST_CASE("automation cursor matches authored tick-domain interpolation and hold edges") {
    const auto map = constant_map();
    auto curve = take(AutomationCurve::create(
        {point(*map, 10, 0, -0.25f, AutomationInterpolation::Continuous, -0.65f),
         point(*map, 11, 8, 0.75f, AutomationInterpolation::Hold), point(*map, 12, 10, 0.1f)}));
    auto source = take(AutomationLane::create({1}, DeviceParameterTarget{{99}, 7}, curve));
    const auto compiled = program(source, map);
    MasterTransport clock;
    prepare_transport(clock, *map, 16);
    const auto snapshot = block(clock, 12);
    std::array<AutomationBlockEvent, 16> events{};
    AutomationCursor cursor;

    const auto result = cursor.process(*compiled, snapshot, events);
    REQUIRE(result.code == AutomationCursorCode::Ok);
    REQUIRE(result.adoption == AutomationProgramAdoption::Adopted);
    REQUIRE(result.emitted_events == 10);
    for (std::uint32_t index = 0; index < result.emitted_events; ++index) {
        const auto tick = map->samples_to_ticks({events[index].sample_offset});
        REQUIRE(std::bit_cast<std::uint32_t>(events[index].value) ==
                std::bit_cast<std::uint32_t>(*curve.value_at(tick)));
    }
    REQUIRE(events[result.emitted_events - 1].sample_offset == 10);
    REQUIRE(events[result.emitted_events - 1].value == 0.1f);
    REQUIRE(events[result.emitted_events - 1].transition == AutomationTransition::Immediate);
}

TEST_CASE("automation cursor evaluates tempo ramps in musical tick space") {
    const std::array tempo_points{
        TempoPoint{{0}, 60.0, TempoCurve::LinearInTicks},
        TempoPoint{{kTicksPerQuarter}, 180.0, TempoCurve::Constant},
    };
    const auto map = shared_compiled_tempo_map(tempo_points, RationalRate{48'000, 1});
    auto curve = take(AutomationCurve::create(
        {AutomationPoint{{10}, {0}, 0.0f}, AutomationPoint{{11}, {kTicksPerQuarter}, 1.0f}}));
    auto source = take(AutomationLane::create({1}, DeviceParameterTarget{{99}, 7}, curve));
    const auto compiled = program(source, map);
    MasterTransport clock;
    prepare_transport(clock, *map, 64);
    const auto snapshot = block(clock, 64);
    std::array<AutomationBlockEvent, 64> events{};
    AutomationCursor cursor;

    const auto result = cursor.process(*compiled, snapshot, events);
    REQUIRE(result.code == AutomationCursorCode::Ok);
    for (std::uint32_t index = 0; index < result.emitted_events; ++index) {
        const auto tick = map->samples_to_ticks({events[index].sample_offset});
        REQUIRE(std::bit_cast<std::uint32_t>(events[index].value) ==
                std::bit_cast<std::uint32_t>(*curve.value_at(tick)));
    }
    const auto midpoint = std::find_if(events.begin(), events.begin() + result.emitted_events,
                                       [](const auto& event) { return event.sample_offset == 32; });
    REQUIRE(midpoint != events.begin() + result.emitted_events);
    const auto midpoint_tick = map->samples_to_ticks({32});
    const auto tick_fraction = static_cast<float>(static_cast<double>(midpoint_tick.value) /
                                                  static_cast<double>(kTicksPerQuarter));
    const auto endpoint_sample = map->ticks_to_samples({kTicksPerQuarter}).value;
    const auto sample_fraction = static_cast<float>(32.0 / static_cast<double>(endpoint_sample));
    REQUIRE(midpoint->value == tick_fraction);
    REQUIRE(midpoint->value != sample_fraction);
}

TEST_CASE("host-beat-mapped automation follows host frames through a document tempo mismatch") {
    const auto map = constant_map(120.0);
    auto curve = take(AutomationCurve::create(
        {AutomationPoint{{10}, {0}, 0.0f}, AutomationPoint{{11}, {kTicksPerQuarter}, 1.0f}}));
    auto source = take(AutomationLane::create({1}, DeviceParameterTarget{{99}, 7}, curve));
    const auto compiled = program(source, map);

    TransportSnapshot snapshot;
    snapshot.tempo_map = map.get();
    snapshot.sample_rate = map->sample_rate();
    snapshot.frame_count = 24'000;
    snapshot.block_index = 0;
    snapshot.is_playing = true;
    snapshot.range_count = 1;
    snapshot.ranges[0].frame_count = snapshot.frame_count;
    snapshot.ranges[0].timeline_sample_start = {96'000};
    snapshot.ranges[0].timeline_tick_start = {0};
    snapshot.ranges[0].timeline_tick_end = {kTicksPerQuarter / 2};
    snapshot.ranges[0].host_beat_mapping = true;

    std::array<AutomationBlockEvent, 3> events{};
    AutomationCursor cursor;
    AutomationCursorResult result;
    std::size_t allocations = 0;
    {
        test::ScopedRtProcessProbe probe;
        result = cursor.process(*compiled, snapshot, events);
        allocations = probe.allocation_count();
    }

    REQUIRE(result.code == AutomationCursorCode::Coalesced);
    REQUIRE(allocations == 0);
    REQUIRE(result.candidate_points == snapshot.frame_count);
    REQUIRE(result.emitted_events == events.size());
    REQUIRE(events[0] == AutomationBlockEvent{0, 0.0f, AutomationTransition::Seed});
    REQUIRE(events[1].sample_offset == 8'000);
    REQUIRE(events[1].value == 1.0f / 6.0f);
    REQUIRE(events[2].sample_offset == 16'000);
    REQUIRE(events[2].value == 1.0f / 3.0f);
}

TEST_CASE("host-beat-mapped automation preserves authored knots under output coalescing") {
    const auto map = constant_map(120.0);
    auto curve = take(AutomationCurve::create(
        {AutomationPoint{{10}, {0}, 0.0f}, AutomationPoint{{11}, {kTicksPerQuarter / 4}, 1.0f},
         AutomationPoint{{12}, {kTicksPerQuarter / 2}, 0.0f}}));
    auto source = take(AutomationLane::create({1}, DeviceParameterTarget{{99}, 7}, curve));
    const auto compiled = program(source, map);

    TransportSnapshot snapshot;
    snapshot.tempo_map = map.get();
    snapshot.sample_rate = map->sample_rate();
    snapshot.frame_count = 24'000;
    snapshot.is_playing = true;
    snapshot.range_count = 1;
    snapshot.ranges[0].frame_count = snapshot.frame_count;
    snapshot.ranges[0].timeline_tick_start = {0};
    snapshot.ranges[0].timeline_tick_end = {kTicksPerQuarter / 2};
    snapshot.ranges[0].host_beat_mapping = true;

    std::array<AutomationBlockEvent, 2> events{};
    AutomationCursor cursor;
    const auto result = cursor.process(*compiled, snapshot, events);

    REQUIRE(result.code == AutomationCursorCode::Coalesced);
    REQUIRE(result.emitted_events == events.size());
    REQUIRE(events[0] == AutomationBlockEvent{0, 0.0f, AutomationTransition::Seed});
    REQUIRE(events[1] == AutomationBlockEvent{12'000, 1.0f, AutomationTransition::LinearRamp});
}

TEST_CASE("host-beat-mapped automation includes knots in the final output-frame cell") {
    const auto map = constant_map(120.0);
    auto curve = take(AutomationCurve::create(
        {AutomationPoint{{10}, {0}, 0.0f}, AutomationPoint{{11}, {39}, 1.0f},
         AutomationPoint{{12}, {40}, 0.0f}}));
    auto source = take(AutomationLane::create({1}, DeviceParameterTarget{{99}, 7}, curve));
    const auto compiled = program(source, map);

    TransportSnapshot snapshot;
    snapshot.tempo_map = map.get();
    snapshot.sample_rate = map->sample_rate();
    snapshot.frame_count = 4;
    snapshot.is_playing = true;
    snapshot.range_count = 1;
    snapshot.ranges[0].frame_count = snapshot.frame_count;
    snapshot.ranges[0].timeline_tick_start = {0};
    snapshot.ranges[0].timeline_tick_end = {40};
    snapshot.ranges[0].host_beat_mapping = true;

    std::array<AutomationBlockEvent, 2> events{};
    AutomationCursor cursor;
    const auto result = cursor.process(*compiled, snapshot, events);

    REQUIRE(result.code == AutomationCursorCode::Coalesced);
    REQUIRE(result.emitted_events == events.size());
    REQUIRE(events[0] == AutomationBlockEvent{0, 0.0f, AutomationTransition::Seed});
    REQUIRE(events[1] == AutomationBlockEvent{3, 1.0f, AutomationTransition::LinearRamp});
}

TEST_CASE("host-beat-mapped automation evaluates fractional ticks without sample quantization") {
    const auto map = constant_map(120.0);
    auto curve = take(AutomationCurve::create(
        {AutomationPoint{{10}, {0}, 0.0f}, AutomationPoint{{11}, {3}, 1.0f}}));
    auto source = take(AutomationLane::create({1}, DeviceParameterTarget{{99}, 7}, curve));
    const auto compiled = program(source, map);

    TransportSnapshot snapshot;
    snapshot.tempo_map = map.get();
    snapshot.sample_rate = map->sample_rate();
    snapshot.frame_count = 2;
    snapshot.is_playing = true;
    snapshot.range_count = 1;
    snapshot.ranges[0].frame_count = snapshot.frame_count;
    snapshot.ranges[0].timeline_tick_start = {0};
    snapshot.ranges[0].timeline_tick_end = {3};
    snapshot.ranges[0].host_beat_mapping = true;

    std::array<AutomationBlockEvent, 2> events{};
    AutomationCursor cursor;
    const auto result = cursor.process(*compiled, snapshot, events);

    REQUIRE(result.code == AutomationCursorCode::Ok);
    REQUIRE(result.emitted_events == events.size());
    REQUIRE(events[0] == AutomationBlockEvent{0, 0.0f, AutomationTransition::Seed});
    REQUIRE(events[1] == AutomationBlockEvent{1, 0.5f, AutomationTransition::LinearRamp});
}

TEST_CASE("host-beat-mapped automation remains safe at the positive tick edge") {
    const auto maximum = std::numeric_limits<std::int64_t>::max();
    const auto map = constant_map(120.0);
    auto curve = take(AutomationCurve::create(
        {AutomationPoint{{10}, {maximum - 4}, 0.0f},
         AutomationPoint{{11}, {maximum - 1}, 1.0f}}));
    auto source = take(AutomationLane::create({1}, DeviceParameterTarget{{99}, 7}, curve));
    const auto compiled = program(source, map);

    TransportSnapshot snapshot;
    snapshot.tempo_map = map.get();
    snapshot.sample_rate = map->sample_rate();
    snapshot.frame_count = 4;
    snapshot.is_playing = true;
    snapshot.range_count = 1;
    snapshot.ranges[0].frame_count = snapshot.frame_count;
    snapshot.ranges[0].timeline_tick_start = {maximum - 4};
    snapshot.ranges[0].timeline_tick_end = {maximum};
    snapshot.ranges[0].host_beat_mapping = true;

    std::array<AutomationBlockEvent, 2> events{};
    AutomationCursor cursor;
    const auto result = cursor.process(*compiled, snapshot, events);

    REQUIRE(result.code == AutomationCursorCode::Coalesced);
    REQUIRE(result.emitted_events == events.size());
    REQUIRE(events[0] == AutomationBlockEvent{0, 0.0f, AutomationTransition::Seed});
    REQUIRE(events[1] == AutomationBlockEvent{3, 1.0f, AutomationTransition::LinearRamp});
}

TEST_CASE("automation cursor preserves document position across mapped and legacy ranges") {
    const auto map = constant_map(120.0);
    auto curve = take(AutomationCurve::create(
        {AutomationPoint{{10}, {0}, 0.0f}, AutomationPoint{{11}, {kTicksPerQuarter}, 1.0f}}));
    auto source = take(AutomationLane::create({1}, DeviceParameterTarget{{99}, 7}, curve));
    const auto compiled = program(source, map);
    AutomationCursor cursor;
    std::array<AutomationBlockEvent, 4> events{};

    TransportSnapshot mapped;
    mapped.tempo_map = map.get();
    mapped.sample_rate = map->sample_rate();
    mapped.frame_count = 24'000;
    mapped.block_index = 0;
    mapped.is_playing = true;
    mapped.range_count = 1;
    mapped.ranges[0].frame_count = mapped.frame_count;
    mapped.ranges[0].timeline_tick_start = {0};
    mapped.ranges[0].timeline_tick_end = {kTicksPerQuarter / 2};
    mapped.ranges[0].host_beat_mapping = true;
    REQUIRE(cursor.process(*compiled, mapped, events).code == AutomationCursorCode::Coalesced);

    TransportSnapshot legacy;
    legacy.tempo_map = map.get();
    legacy.sample_rate = map->sample_rate();
    legacy.frame_count = 16;
    legacy.block_index = 1;
    legacy.is_playing = true;
    legacy.range_count = 1;
    legacy.ranges[0].frame_count = legacy.frame_count;
    legacy.ranges[0].timeline_sample_start = map->ticks_to_samples({kTicksPerQuarter / 2});
    legacy.ranges[0].timeline_tick_start = {kTicksPerQuarter / 2};
    legacy.ranges[0].timeline_tick_end =
        map->samples_to_ticks({legacy.ranges[0].timeline_sample_start.value + 16});
    const auto legacy_result = cursor.process(*compiled, legacy, events);
    REQUIRE(legacy_result.code == AutomationCursorCode::Coalesced);
    REQUIRE(legacy_result.adoption == AutomationProgramAdoption::Unchanged);
    REQUIRE(events[0].sample_offset == 0);
    REQUIRE(events[0].value ==
            *curve.value_at(map->samples_to_ticks(legacy.ranges[0].timeline_sample_start)));

    mapped.block_index = 2;
    mapped.ranges[0].timeline_tick_start = {3 * kTicksPerQuarter / 4};
    mapped.ranges[0].timeline_tick_end = {kTicksPerQuarter};
    const auto remapped_result = cursor.process(*compiled, mapped, events);
    REQUIRE(remapped_result.code == AutomationCursorCode::Coalesced);
    REQUIRE(remapped_result.adoption == AutomationProgramAdoption::Unchanged);
    REQUIRE(events[0].sample_offset == 0);
    REQUIRE(events[0].value == 0.75f);
}

TEST_CASE("automation cursor coalesces deterministically to the caller budget") {
    const auto map = constant_map();
    auto source = lane({1}, {point(*map, 10, 0, 0.0f), point(*map, 11, 8, 1.0f)});
    const auto compiled = program(source, map);
    MasterTransport clock;
    prepare_transport(clock, *map, 16);
    const auto snapshot = block(clock, 12);
    std::array<AutomationBlockEvent, 3> events{};
    AutomationCursor cursor;

    const auto result = cursor.process(*compiled, snapshot, events);
    REQUIRE(result.code == AutomationCursorCode::Coalesced);
    REQUIRE(result.candidate_points == 9);
    REQUIRE(result.emitted_events == events.size());
    REQUIRE(events[0].sample_offset == 0);
    REQUIRE(events[1].sample_offset == 4);
    REQUIRE(events[2].sample_offset == 8);
    REQUIRE(events[1].transition == AutomationTransition::LinearRamp);
    REQUIRE(events[2].transition == AutomationTransition::LinearRamp);
}

TEST_CASE("automation cursor preserves authored peaks before optional refinement") {
    const auto map = constant_map();
    auto source = lane({1}, {point(*map, 10, 0, 0.0f), point(*map, 11, 4, 1.0f),
                             point(*map, 12, 8, 0.0f)});
    const auto compiled = program(source, map);
    MasterTransport clock;
    prepare_transport(clock, *map, 16);
    const auto snapshot = block(clock, 9);
    AutomationCursor cursor;

    std::array<AutomationBlockEvent, 2> insufficient{};
    const auto failed = cursor.process(*compiled, snapshot, insufficient);
    REQUIRE(failed.code == AutomationCursorCode::InsufficientCapacity);
    REQUIRE(cursor.active_generation() == 0);

    std::array<AutomationBlockEvent, 3> events{};
    const auto rendered = cursor.process(*compiled, snapshot, events);
    REQUIRE(rendered.code == AutomationCursorCode::Coalesced);
    REQUIRE(rendered.emitted_events == 3);
    REQUIRE(events[0].sample_offset == 0);
    REQUIRE(events[0].value == 0.0f);
    REQUIRE(events[1].sample_offset == 4);
    REQUIRE(events[1].value == 1.0f);
    REQUIRE(events[1].transition == AutomationTransition::LinearRamp);
    REQUIRE(events[2].sample_offset == 8);
    REQUIRE(events[2].value == 0.0f);
    REQUIRE(events[2].transition == AutomationTransition::LinearRamp);
}

TEST_CASE("automation cursor preserves transition ownership across hold boundaries") {
    const auto map = constant_map();
    auto source = lane(
        {1}, {point(*map, 10, 0, 0.0f),
              point(*map, 11, 4, 1.0f, AutomationInterpolation::Hold),
              point(*map, 12, 8, 0.25f, AutomationInterpolation::Continuous),
              point(*map, 13, 12, 0.75f)});
    const auto compiled = program(source, map);
    MasterTransport clock;
    prepare_transport(clock, *map, 16);
    std::array<AutomationBlockEvent, 4> events{};
    AutomationCursor cursor;

    const auto result = cursor.process(*compiled, block(clock, 13), events);
    REQUIRE(result.code == AutomationCursorCode::Coalesced);
    REQUIRE(result.emitted_events == events.size());
    REQUIRE(events[0] == AutomationBlockEvent{0, 0.0f, AutomationTransition::Seed});
    REQUIRE(events[1] == AutomationBlockEvent{4, 1.0f, AutomationTransition::LinearRamp});
    REQUIRE(events[2] == AutomationBlockEvent{8, 0.25f, AutomationTransition::Immediate});
    REQUIRE(events[3] == AutomationBlockEvent{12, 0.75f, AutomationTransition::LinearRamp});
}

TEST_CASE("same-sample interior knots preserve the incoming segment and latest winner") {
    const auto map = constant_map();
    const auto cluster_tick = map->samples_to_ticks({10});
    auto curve = take(AutomationCurve::create(
        {point(*map, 10, 0, 0.0f),
         AutomationPoint{{11}, cluster_tick, 1.0f, AutomationInterpolation::Hold},
         AutomationPoint{{12}, {cluster_tick.value + 1}, 0.25f}, point(*map, 13, 20, 0.5f)}));
    auto source = take(AutomationLane::create({1}, DeviceParameterTarget{{99}, 7}, curve));
    const auto compiled = program(source, map);
    MasterTransport clock;
    prepare_transport(clock, *map, 16);
    std::array<AutomationBlockEvent, 12> events{};
    AutomationCursor cursor;

    const auto result = cursor.process(*compiled, block(clock, 12), events);
    REQUIRE(result.code == AutomationCursorCode::Ok);
    const auto at_nine = std::find_if(events.begin(), events.begin() + result.emitted_events,
                                      [](const auto& event) { return event.sample_offset == 9; });
    const auto at_ten = std::find_if(events.begin(), events.begin() + result.emitted_events,
                                     [](const auto& event) { return event.sample_offset == 10; });
    REQUIRE(at_nine != events.begin() + result.emitted_events);
    REQUIRE(at_ten != events.begin() + result.emitted_events);
    REQUIRE(std::bit_cast<std::uint32_t>(at_nine->value) ==
            std::bit_cast<std::uint32_t>(*curve.value_at(map->samples_to_ticks({9}))));
    REQUIRE(at_ten->value == 0.25f);
    REQUIRE(at_ten->transition == AutomationTransition::LinearRamp);

    auto hold_curve = take(AutomationCurve::create(
        {point(*map, 20, 0, 0.0f, AutomationInterpolation::Hold),
         AutomationPoint{{21}, cluster_tick, 1.0f, AutomationInterpolation::Hold},
         AutomationPoint{{22}, {cluster_tick.value + 1}, 0.25f}, point(*map, 23, 20, 0.5f)}));
    auto hold_lane = take(AutomationLane::create({2}, DeviceParameterTarget{{99}, 7}, hold_curve));
    const auto hold_program = program(hold_lane, map);
    MasterTransport hold_clock;
    prepare_transport(hold_clock, *map, 16);
    AutomationCursor hold_cursor;
    const auto hold_result = hold_cursor.process(*hold_program, block(hold_clock, 12), events);
    REQUIRE(hold_result.code == AutomationCursorCode::Ok);
    const auto hold_nine = std::find_if(events.begin(), events.begin() + hold_result.emitted_events,
                                        [](const auto& event) { return event.sample_offset == 9; });
    const auto hold_ten = std::find_if(events.begin(), events.begin() + hold_result.emitted_events,
                                       [](const auto& event) { return event.sample_offset == 10; });
    REQUIRE(hold_nine == events.begin() + hold_result.emitted_events);
    REQUIRE(hold_ten != events.begin() + hold_result.emitted_events);
    REQUIRE(hold_ten->value == 0.25f);
    REQUIRE(hold_ten->transition == AutomationTransition::Immediate);
}

TEST_CASE("same-sample final knots resolve to the latest authored winner") {
    const auto map = constant_map();
    const auto cluster_tick = map->samples_to_ticks({10});
    auto source = lane({1},
                       {point(*map, 10, 0, 0.0f),
                        AutomationPoint{{11}, cluster_tick, 1.0f},
                        AutomationPoint{{12}, {cluster_tick.value + 1}, 0.25f}});
    const auto compiled = program(source, map);
    MasterTransport clock;
    prepare_transport(clock, *map, 16);
    std::array<AutomationBlockEvent, 12> events{};
    AutomationCursor cursor;

    const auto result = cursor.process(*compiled, block(clock, 12), events);
    REQUIRE(result.code == AutomationCursorCode::Ok);
    const auto at_ten = std::find_if(events.begin(), events.begin() + result.emitted_events,
                                     [](const auto& event) { return event.sample_offset == 10; });
    REQUIRE(at_ten != events.begin() + result.emitted_events);
    REQUIRE(at_ten->value == 0.25f);
}

TEST_CASE("automation knots sharing one sample resolve to the latest authored winner") {
    const auto map = constant_map();
    auto source = lane({1}, {{{10}, {0}, 0.0f}, {{11}, {1}, 0.5f}, {{12}, {2}, 1.0f}});
    const auto compiled = program(source, map);
    MasterTransport clock;
    prepare_transport(clock, *map, 4);
    std::array<AutomationBlockEvent, 4> events{};
    AutomationCursor cursor;

    const auto result = cursor.process(*compiled, block(clock, 4), events);
    REQUIRE(result.code == AutomationCursorCode::Ok);
    REQUIRE(result.emitted_events == 1);
    REQUIRE(events[0].sample_offset == 0);
    REQUIRE(events[0].value == 1.0f);
}

TEST_CASE("failed output budgeting is transactional and leaves canaries untouched") {
    const auto map = constant_map();
    auto source = lane({1}, {point(*map, 10, 0, 0.0f), point(*map, 11, 8, 1.0f)});
    const auto compiled = program(source, map);
    MasterTransport clock;
    prepare_transport(clock, *map, 8);
    const auto snapshot = block(clock, 8);
    AutomationCursor cursor;

    const auto failed = cursor.process(*compiled, snapshot, {});
    REQUIRE(failed.code == AutomationCursorCode::InsufficientCapacity);
    REQUIRE(failed.adoption == AutomationProgramAdoption::Unchanged);
    REQUIRE(cursor.active_generation() == 0);

    std::array<AutomationBlockEvent, 4> storage{};
    storage[2].sample_offset = 0xfeedu;
    storage[3].sample_offset = 0xbeefu;
    const auto retried = cursor.process(*compiled, snapshot, std::span(storage).first(2));
    REQUIRE(retried.code == AutomationCursorCode::Coalesced);
    REQUIRE(retried.adoption == AutomationProgramAdoption::Adopted);
    REQUIRE(storage[2].sample_offset == 0xfeedu);
    REQUIRE(storage[3].sample_offset == 0xbeefu);
}

TEST_CASE("equal automation generation rejects a replacement program instance") {
    auto first_map = constant_map();
    auto replacement_map = constant_map();
    auto source = lane({1}, {point(*first_map, 10, 0, 0.0f), point(*first_map, 11, 8, 1.0f)});
    auto first = program(source, first_map, 1);
    auto replacement = program(source, replacement_map, 1);
    REQUIRE(first->instance_token().value != 0);
    REQUIRE(replacement->instance_token() != first->instance_token());
    MasterTransport first_clock;
    prepare_transport(first_clock, *first_map, 8);
    std::array<AutomationBlockEvent, 8> events{};
    AutomationCursor cursor;
    REQUIRE(cursor.process(*first, block(first_clock, 8), events).adoption ==
            AutomationProgramAdoption::Adopted);

    first.reset();
    first_map.reset();
    MasterTransport replacement_clock;
    prepare_transport(replacement_clock, *replacement_map, 8);
    const auto rejected = cursor.process(*replacement, block(replacement_clock, 8), events);
    REQUIRE(rejected.code == AutomationCursorCode::AdoptionRejected);
    REQUIRE(rejected.adoption == AutomationProgramAdoption::Rejected);
}

TEST_CASE("automation cursor accepts saturated positive sample ranges") {
    const auto map = constant_map();
    auto source = lane({1}, {point(*map, 10, 0, 0.5f)});
    const auto compiled = program(source, map);
    TransportSnapshot snapshot;
    snapshot.tempo_map = map.get();
    snapshot.sample_rate = map->sample_rate();
    snapshot.frame_count = 4;
    snapshot.is_playing = true;
    snapshot.range_count = 1;
    snapshot.ranges[0].frame_count = 4;
    snapshot.ranges[0].timeline_sample_start = {std::numeric_limits<std::int64_t>::max() - 1};
    std::array<AutomationBlockEvent, 4> events{};
    AutomationCursor cursor;

    const auto result = cursor.process(*compiled, snapshot, events);
    REQUIRE(result.code == AutomationCursorCode::Ok);
    REQUIRE(result.candidate_points == 1);
    REQUIRE(result.emitted_events == 1);
}

TEST_CASE("automation cursor seeds both half-open ranges at a loop wrap") {
    const auto map = constant_map();
    auto source = lane({1}, {point(*map, 10, 0, 0.0f), point(*map, 11, 8, 1.0f)});
    const auto compiled = program(source, map);
    const LoopRegion loop{true, map->samples_to_ticks({0}), map->samples_to_ticks({8})};
    MasterTransport clock;
    prepare_transport(clock, *map, 4, true, map->samples_to_ticks({6}), loop);
    const auto snapshot = block(clock, 4);
    REQUIRE(snapshot.range_count == 2);
    std::array<AutomationBlockEvent, 2> events{};
    AutomationCursor cursor;

    const auto result = cursor.process(*compiled, snapshot, events);
    REQUIRE(result.code == AutomationCursorCode::Coalesced);
    REQUIRE(result.emitted_events == 2);
    REQUIRE(events[0].sample_offset == 0);
    REQUIRE(events[1].sample_offset == 2);
    REQUIRE(events[1].value == 0.0f);
}

TEST_CASE("automation cursor rejects stale identities and reseeds newer generations") {
    const auto map = constant_map();
    auto source = lane({1}, {point(*map, 10, 0, 0.0f), point(*map, 11, 8, 1.0f)});
    auto other = lane({2}, {point(*map, 20, 0, 0.0f), point(*map, 21, 8, 1.0f)});
    const auto first = program(source, map, 2);
    const auto stale = program(source, map, 1);
    const auto newer = program(source, map, 3);
    const auto wrong_lane = program(other, map, 4);
    MasterTransport clock;
    prepare_transport(clock, *map, 8);
    std::array<AutomationBlockEvent, 8> events{};
    AutomationCursor cursor;

    REQUIRE(cursor.process(*first, block(clock, 8), events).adoption ==
            AutomationProgramAdoption::Adopted);
    REQUIRE(cursor.process(*stale, block(clock, 8), events).code ==
            AutomationCursorCode::AdoptionRejected);
    REQUIRE(cursor.process(*wrong_lane, block(clock, 8), events).code ==
            AutomationCursorCode::AdoptionRejected);
    const auto adopted = cursor.process(*newer, block(clock, 8), events);
    REQUIRE(adopted.adoption == AutomationProgramAdoption::Adopted);
    REQUIRE(adopted.emitted_events != 0);
}

TEST_CASE("stopped automation emits only on adoption and seek") {
    const auto map = constant_map();
    auto source = lane({1}, {point(*map, 10, 0, 0.0f), point(*map, 11, 8, 1.0f)});
    const auto compiled = program(source, map);
    MasterTransport clock;
    prepare_transport(clock, *map, 8, false);
    std::array<AutomationBlockEvent, 2> events{};
    AutomationCursor cursor;

    REQUIRE(cursor.process(*compiled, block(clock, 8), events).emitted_events == 1);
    REQUIRE(cursor.process(*compiled, block(clock, 8), events).emitted_events == 0);
    REQUIRE(clock.seek(map->samples_to_ticks({4})) == TransportError::None);
    const auto seeked = cursor.process(*compiled, block(clock, 8), events);
    REQUIRE(seeked.emitted_events == 1);
    REQUIRE(seeked.adoption == AutomationProgramAdoption::Unchanged);
}

TEST_CASE("automation cursor process is declared and probed realtime safe") {
    STATIC_REQUIRE(AutomationCursor::process_rt_safety_class ==
                   audio::RtSafetyClass::AudioCallbackSafeWithImmutableInputs);
    const auto map = constant_map();
    auto source = lane({1}, {point(*map, 10, 0, 0.0f), point(*map, 11, 8, 1.0f)});
    const auto compiled = program(source, map);
    MasterTransport clock;
    prepare_transport(clock, *map, 8);
    const auto snapshot = block(clock, 8);
    std::array<AutomationBlockEvent, 8> events{};
    AutomationCursor cursor;
    AutomationCursorResult result;
    std::size_t allocations = 0;
    {
        test::ScopedRtProcessProbe probe;
        result = cursor.process(*compiled, snapshot, events);
        allocations = probe.allocation_count();
    }
    REQUIRE(result.code == AutomationCursorCode::Ok);
    REQUIRE(allocations == 0);
}
