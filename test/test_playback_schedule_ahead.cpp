#include <pulp/playback/schedule_ahead.hpp>

#include "timebase_test_helpers.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <limits>

using namespace pulp::playback;
using namespace pulp::timebase;

namespace {

CompiledTempoMap constant_map() {
    const std::array points{TempoPoint{{0}, 120.0}};
    return require_compiled_tempo_map(points, RationalRate{48'000, 1});
}

TransportSnapshot block(MasterTransport& transport, std::uint32_t frames) {
    TransportSnapshot snapshot;
    REQUIRE(transport.begin_block(frames, snapshot) == TransportError::None);
    return snapshot;
}

void prepare_transport(MasterTransport& transport, const CompiledTempoMap& map,
                       SamplePosition start, bool playing = true) {
    MasterTransportConfig config;
    config.max_buffer_size = 1024;
    config.initial_position = map.samples_to_ticks(start);
    config.initially_playing = playing;
    config.loop = {true, {0}, {kTicksPerQuarter}};
    REQUIRE(transport.prepare(map, config) == TransportError::None);
}

} // namespace

TEST_CASE("schedule ahead rebuilds a projected loop split instead of copying offsets") {
    const auto map = constant_map();
    MasterTransport transport;
    prepare_transport(transport, map, {23'500});
    const auto base = block(transport, 1024);
    REQUIRE(base.range_count == 2);
    REQUIRE(base.ranges[0].frame_count == 500);

    TransportSnapshot projected;
    REQUIRE(project_schedule_ahead(base, 200, projected) == ScheduleAheadCode::Ok);
    REQUIRE(projected.range_count == 2);
    REQUIRE(projected.ranges[0].timeline_sample_start == SamplePosition{23'700});
    REQUIRE(projected.ranges[0].frame_count == 300);
    REQUIRE(projected.ranges[1].sample_offset == 300);
    REQUIRE(projected.ranges[1].frame_count == 724);
    REQUIRE_FALSE(projected.ranges[0].discontinuity);
    REQUIRE(projected.ranges[1].discontinuity);
}

TEST_CASE("schedule ahead marks an exact projected loop boundary at offset zero") {
    const auto map = constant_map();
    MasterTransport transport;
    prepare_transport(transport, map, {23'000});
    const auto base = block(transport, 256);

    TransportSnapshot projected;
    REQUIRE(project_schedule_ahead(base, 1000, projected) == ScheduleAheadCode::Ok);
    REQUIRE(projected.range_count == 1);
    REQUIRE(projected.ranges[0].timeline_sample_start == SamplePosition{0});
    REQUIRE(projected.ranges[0].sample_offset == 0);
    REQUIRE(projected.ranges[0].discontinuity);
    REQUIRE(projected.ranges[0].discontinuity_reason ==
            TransportDiscontinuityReason::LoopWrap);
}

TEST_CASE("schedule ahead advances monotonic time across complete loop cycles") {
    const auto map = constant_map();
    MasterTransport transport;
    prepare_transport(transport, map, {100});
    const auto base = block(transport, 64);
    constexpr std::int64_t lead = 2 * 24'000 + 500;

    TransportSnapshot projected;
    REQUIRE(project_schedule_ahead(base, lead, projected) == ScheduleAheadCode::Ok);
    REQUIRE(projected.ranges[0].timeline_sample_start == SamplePosition{600});
    const auto partial = map.resolve_sample({600}).tick - map.resolve_sample({100}).tick;
    const TickDuration expected{2 * kTicksPerQuarter + partial.value};
    REQUIRE(projected.ranges[0].monotonic_start ==
            base.ranges[0].monotonic_start + expected);
}

TEST_CASE("schedule ahead preserves external seek reset at projected offset zero") {
    const auto map = constant_map();
    MasterTransport transport;
    prepare_transport(transport, map, {0});
    (void)block(transport, 64);
    REQUIRE(transport.seek(map.samples_to_ticks({1000})) == TransportError::None);
    const auto base = block(transport, 64);
    REQUIRE(base.reset_requested);

    TransportSnapshot projected;
    REQUIRE(project_schedule_ahead(base, 500, projected) == ScheduleAheadCode::Ok);
    REQUIRE(projected.reset_requested);
    REQUIRE(projected.ranges[0].timeline_sample_start == SamplePosition{1500});
    REQUIRE(projected.ranges[0].discontinuity);
    REQUIRE(projected.ranges[0].discontinuity_reason ==
            TransportDiscontinuityReason::Seek);
}

TEST_CASE("schedule ahead preserves loop-configuration relocation at offset zero") {
    const auto map = constant_map();
    MasterTransport transport;
    prepare_transport(transport, map, {23'000});
    (void)block(transport, 64);
    REQUIRE(transport.set_loop({true, {0}, {kTicksPerQuarter / 2}}) ==
            TransportError::None);
    const auto base = block(transport, 64);
    REQUIRE(base.ranges[0].discontinuity_reason ==
            TransportDiscontinuityReason::LoopConfiguration);

    TransportSnapshot projected;
    REQUIRE(project_schedule_ahead(base, 100, projected) == ScheduleAheadCode::Ok);
    REQUIRE(projected.ranges[0].timeline_sample_start == SamplePosition{100});
    REQUIRE(projected.ranges[0].discontinuity_reason ==
            TransportDiscontinuityReason::LoopConfiguration);
}

TEST_CASE("schedule ahead keeps seek precedence when loop normalization relocates") {
    const auto map = constant_map();
    MasterTransport transport;
    prepare_transport(transport, map, {0});
    (void)block(transport, 64);
    REQUIRE(transport.seek({2 * kTicksPerQuarter}) == TransportError::None);
    const auto base = block(transport, 64);
    REQUIRE(base.reset_requested);
    REQUIRE(base.ranges[0].timeline_sample_start == SamplePosition{0});
    REQUIRE(base.ranges[0].discontinuity_reason ==
            TransportDiscontinuityReason::Seek);

    TransportSnapshot projected;
    REQUIRE(project_schedule_ahead(base, 100, projected) == ScheduleAheadCode::Ok);
    REQUIRE(projected.ranges[0].discontinuity_reason ==
            TransportDiscontinuityReason::Seek);
}

TEST_CASE("schedule ahead rejects ambiguous legacy range-zero discontinuity") {
    const auto map = constant_map();
    MasterTransport transport;
    prepare_transport(transport, map, {0});
    auto base = block(transport, 64);
    base.ranges[0].discontinuity = true;
    base.ranges[0].discontinuity_reason = TransportDiscontinuityReason::None;

    TransportSnapshot projected;
    REQUIRE(project_schedule_ahead(base, 100, projected) ==
            ScheduleAheadCode::InvalidTransport);
}

TEST_CASE("schedule ahead leaves stopped transport unchanged") {
    const auto map = constant_map();
    MasterTransport transport;
    prepare_transport(transport, map, {321}, false);
    const auto base = block(transport, 128);

    TransportSnapshot projected;
    REQUIRE(project_schedule_ahead(base, 50'000, projected) == ScheduleAheadCode::Ok);
    REQUIRE(projected.frame_count == base.frame_count);
    REQUIRE(projected.range_count == base.range_count);
    REQUIRE(projected.ranges[0].timeline_sample_start ==
            base.ranges[0].timeline_sample_start);
    REQUIRE(projected.ranges[0].timeline_tick_start ==
            base.ranges[0].timeline_tick_start);
    REQUIRE(projected.ranges[0].monotonic_start ==
            base.ranges[0].monotonic_start);
    REQUIRE(projected.ranges[0].discontinuity == base.ranges[0].discontinuity);
}

TEST_CASE("schedule ahead ignores a base natural wrap and recomputes its window") {
    const auto map = constant_map();
    MasterTransport transport;
    prepare_transport(transport, map, {23'500});
    const auto ending_at_wrap = block(transport, 500);
    REQUIRE_FALSE(ending_at_wrap.ranges[0].discontinuity);
    const auto base = block(transport, 64);
    REQUIRE(base.ranges[0].discontinuity);
    REQUIRE(base.ranges[0].discontinuity_reason ==
            TransportDiscontinuityReason::LoopWrap);

    TransportSnapshot projected;
    REQUIRE(project_schedule_ahead(base, 100, projected) == ScheduleAheadCode::Ok);
    REQUIRE(projected.ranges[0].timeline_sample_start == SamplePosition{100});
    REQUIRE_FALSE(projected.ranges[0].discontinuity);
    REQUIRE(projected.ranges[0].discontinuity_reason ==
            TransportDiscontinuityReason::None);
}

TEST_CASE("schedule ahead recomputes bars from the transport meter anchor") {
    const auto map = constant_map();
    MasterTransport transport;
    MasterTransportConfig config;
    config.max_buffer_size = 1024;
    config.initially_playing = true;
    config.meter = {4, 4};
    REQUIRE(transport.prepare(map, config) == TransportError::None);
    const auto base = block(transport, 64);

    TransportSnapshot projected;
    REQUIRE(project_schedule_ahead(base, 4 * 24'000 + 1, projected) ==
            ScheduleAheadCode::Ok);
    REQUIRE(projected.meter_anchor_tick == TickPosition{0});
    REQUIRE(projected.meter_anchor_bar == BarPosition{0});
    REQUIRE(projected.ranges[0].bar_start.value == 1);
}

TEST_CASE("schedule ahead rejects invalid leads and unrepresentable linear windows") {
    const auto map = constant_map();
    MasterTransport transport;
    MasterTransportConfig config;
    config.max_buffer_size = 1024;
    config.initially_playing = true;
    REQUIRE(transport.prepare(map, config) == TransportError::None);
    auto base = block(transport, 16);
    base.ranges[0].timeline_sample_start =
        {std::numeric_limits<std::int64_t>::max() - 32};
    base.ranges[0].timeline_tick_start =
        map.resolve_sample(base.ranges[0].timeline_sample_start).tick;

    TransportSnapshot projected;
    REQUIRE(project_schedule_ahead(base, -1, projected) ==
            ScheduleAheadCode::InvalidLead);
    REQUIRE(project_schedule_ahead(base, 64, projected) ==
            ScheduleAheadCode::SampleRangeExceeded);
}

TEST_CASE("schedule ahead advances negative preroll without signed overflow") {
    const auto map = constant_map();
    MasterTransport transport;
    MasterTransportConfig config;
    config.max_buffer_size = 1024;
    config.initially_playing = true;
    config.initial_position = map.samples_to_ticks({-10'000});
    REQUIRE(transport.prepare(map, config) == TransportError::None);
    const auto base = block(transport, 64);

    TransportSnapshot projected;
    REQUIRE(project_schedule_ahead(base, 500, projected) == ScheduleAheadCode::Ok);
    REQUIRE(projected.ranges[0].timeline_sample_start == SamplePosition{-9'500});
    REQUIRE(projected.ranges[0].frame_count == 64);
}
