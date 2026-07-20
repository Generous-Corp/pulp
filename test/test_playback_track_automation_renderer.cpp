#include <pulp/playback/track_automation_renderer.hpp>
#include <pulp/playback/schedule_ahead.hpp>

#include "harness/scoped_rt_process_probe.hpp"
#include "timebase_test_helpers.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdlib>
#include <memory>
#include <utility>
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

std::shared_ptr<const CompiledTempoMap> tempo_map() {
    const std::array points{TempoPoint{{0}, 60.0}};
    return shared_compiled_tempo_map(points, RationalRate{48'000, 1});
}

std::shared_ptr<const AutomationProgram>
program(ItemId lane_id, DeviceParameterTarget target,
        const std::shared_ptr<const CompiledTempoMap>& map, ProgramGeneration generation,
        std::int64_t end_sample = 8) {
    auto curve = take(AutomationCurve::create(
        {AutomationPoint{{lane_id.value + 1}, map->samples_to_ticks({0}), 0.0f},
         AutomationPoint{{lane_id.value + 2}, map->samples_to_ticks({end_sample}), 1.0f}}));
    auto lane = take(AutomationLane::create(lane_id, target, std::move(curve)));
    return take(AutomationProgram::compile(lane, map, generation));
}

std::shared_ptr<const TrackAutomationProgram>
track(const std::shared_ptr<const CompiledTempoMap>& map,
      std::vector<std::shared_ptr<const AutomationProgram>> programs) {
    return take(TrackAutomationProgram::create({5}, map, std::move(programs)));
}

std::shared_ptr<const AutomationProgram>
hold_program(ItemId lane_id, DeviceParameterTarget target,
             const std::shared_ptr<const CompiledTempoMap>& map, std::uint32_t point_count) {
    std::vector<AutomationPoint> points;
    points.reserve(point_count);
    for (std::uint32_t index = 0; index < point_count; ++index) {
        points.push_back({{lane_id.value * 10'000u + index + 1u},
                          map->samples_to_ticks({index}), static_cast<float>(index),
                          AutomationInterpolation::Hold});
    }
    auto lane = take(AutomationLane::create(
        lane_id, target, take(AutomationCurve::create(std::move(points)))));
    return take(AutomationProgram::compile(lane, map, 1));
}

std::shared_ptr<const AutomationProgram>
dense_continuous_program(ItemId lane_id, DeviceParameterTarget target,
                         const std::shared_ptr<const CompiledTempoMap>& map,
                         std::uint32_t point_count) {
    std::vector<AutomationPoint> points;
    points.reserve(point_count);
    for (std::uint32_t index = 0; index < point_count; ++index) {
        points.push_back({{lane_id.value * 10'000u + index + 1u},
                          map->samples_to_ticks({index}), static_cast<float>(index)});
    }
    auto lane = take(AutomationLane::create(
        lane_id, target, take(AutomationCurve::create(std::move(points)))));
    return take(AutomationProgram::compile(lane, map, 1));
}

void prepare(MasterTransport& transport, const CompiledTempoMap& map,
             std::uint32_t maximum_frames) {
    MasterTransportConfig config;
    config.max_buffer_size = maximum_frames;
    config.initially_playing = true;
    REQUIRE(transport.prepare(map, config) == TransportError::None);
}

TransportSnapshot block(MasterTransport& transport, std::uint32_t frames) {
    TransportSnapshot snapshot;
    REQUIRE(transport.begin_block(frames, snapshot) == TransportError::None);
    return snapshot;
}

} // namespace

TEST_CASE("track automation renderer batches by canonical device placement identity") {
    const auto map = tempo_map();
    const auto lane_30 = program({30}, {{200}, 3}, map, 1);
    const auto lane_10 = program({10}, {{100}, 1}, map, 1);
    const auto lane_20 = program({20}, {{100}, 2}, map, 1);
    auto renderer = take(TrackAutomationRenderer::create(
        track(map, {lane_30, lane_20, lane_10})));
    MasterTransport transport;
    prepare(transport, *map, 16);

    const auto rendered = renderer.process(block(transport, 9));
    REQUIRE(rendered.code == TrackAutomationRendererCode::Ok);
    REQUIRE(renderer.batches().size() == 2);
    REQUIRE(renderer.batches()[0].device_placement_id == ItemId{100});
    REQUIRE(renderer.batches()[1].device_placement_id == ItemId{200});
    REQUIRE(renderer.batches()[0].events[0].lane_id == ItemId{10});
    REQUIRE(renderer.batches()[0].events[0].param_id == 1);
    REQUIRE(renderer.batches()[0].events[1].lane_id == ItemId{10});
    REQUIRE(renderer.batches()[0].events[2].lane_id == ItemId{20});
    REQUIRE(renderer.batches()[0].events[2].param_id == 2);
    REQUIRE(renderer.batches()[0].events[0].sample_offset == 0);
    REQUIRE(renderer.batches()[0].events[1].sample_offset == 0);
    REQUIRE(renderer.batches()[0].events[2].sample_offset == 0);
}

TEST_CASE("track automation renderer densely coalesces a full device time range") {
    const auto map = tempo_map();
    const auto lane_10 = program({10}, {{100}, 1}, map, 1, 599);
    const auto lane_20 = program({20}, {{100}, 2}, map, 1, 599);
    auto renderer = take(
        TrackAutomationRenderer::create(track(map, {lane_20, lane_10})));
    MasterTransport transport;
    prepare(transport, *map, 600);

    const auto snapshot = block(transport, 600);
    TrackAutomationRenderResult rendered;
    {
        test::ScopedRtProcessProbe probe;
        rendered = renderer.process(snapshot);
        REQUIRE(probe.allocation_count() == 0);
    }
    REQUIRE(rendered.code == TrackAutomationRendererCode::Coalesced);
    REQUIRE(rendered.candidate_events == 1200);
    REQUIRE(rendered.emitted_events == TrackAutomationRenderer::kEventsPerDevice);
    REQUIRE(renderer.batches().size() == 1);
    const auto& batch = renderer.batches()[0];
    REQUIRE(batch.coalesced);
    REQUIRE(batch.events.size() == TrackAutomationRenderer::kEventsPerDevice);
    REQUIRE(batch.events.front().sample_offset == 0);
    REQUIRE(batch.events.front().lane_id == ItemId{10});
    REQUIRE(batch.events.back().sample_offset == 598);
    REQUIRE(batch.events.back().lane_id == ItemId{20});
    REQUIRE(batch.events.back().value == 1.0f);
    REQUIRE(batch.events[batch.events.size() / 2].sample_offset > 250);
}

TEST_CASE("track automation renderer emits queue-ready ramp and immediate events") {
    const auto map = tempo_map();
    auto curve = take(AutomationCurve::create(
        {AutomationPoint{{11}, map->samples_to_ticks({0}), 0.0f,
                         AutomationInterpolation::Continuous},
         AutomationPoint{{12}, map->samples_to_ticks({1}), 1.0f,
                         AutomationInterpolation::Hold},
         AutomationPoint{{13}, map->samples_to_ticks({2}), 0.25f,
                         AutomationInterpolation::Continuous},
         AutomationPoint{{14}, map->samples_to_ticks({3}), 0.75f}}));
    auto lane = take(AutomationLane::create({10}, DeviceParameterTarget{{100}, 7},
                                            std::move(curve)));
    const auto compiled = take(AutomationProgram::compile(lane, map, 1));
    auto renderer = take(TrackAutomationRenderer::create(track(map, {compiled})));
    MasterTransport transport;
    prepare(transport, *map, 4);

    const auto rendered = renderer.process(block(transport, 4));
    REQUIRE(rendered.code == TrackAutomationRendererCode::Ok);
    const auto events = renderer.batches()[0].events;
    REQUIRE(events.size() == 4);
    REQUIRE(events[0] == TrackAutomationEvent{{10}, 7, 0, 0.0f, 0});
    REQUIRE(events[1] == TrackAutomationEvent{{10}, 7, 0, 1.0f, 1});
    REQUIRE(events[2] == TrackAutomationEvent{{10}, 7, 2, 0.25f, 0});
    REQUIRE(events[3] == TrackAutomationEvent{{10}, 7, 2, 0.75f, 1});
}

TEST_CASE("track automation renderer evaluates each device in its projected window") {
    const auto map = tempo_map();
    const auto first = hold_program({10}, {{100}, 1}, map, 16);
    const auto second = hold_program({20}, {{200}, 2}, map, 16);
    auto renderer =
        take(TrackAutomationRenderer::create(track(map, {second, first})));

    MasterTransport transport;
    prepare(transport, *map, 4);
    const auto base = block(transport, 4);
    TransportSnapshot first_window;
    TransportSnapshot second_window;
    REQUIRE(project_schedule_ahead(base, 0, first_window) == ScheduleAheadCode::Ok);
    REQUIRE(project_schedule_ahead(base, 8, second_window) == ScheduleAheadCode::Ok);
    const std::array views{
        DeviceAutomationTransportView{{100}, &first_window},
        DeviceAutomationTransportView{{200}, &second_window},
    };

    TrackAutomationRenderResult rendered;
    {
        test::ScopedRtProcessProbe probe;
        rendered = renderer.process(views);
        REQUIRE(probe.allocation_count() == 0);
    }
    REQUIRE(rendered.code == TrackAutomationRendererCode::Ok);
    REQUIRE(renderer.batches().size() == 2);
    REQUIRE(renderer.batches()[0].events[1].sample_offset == 1);
    REQUIRE(renderer.batches()[0].events[1].value == 1.0f);
    REQUIRE(renderer.batches()[1].events[1].sample_offset == 1);
    REQUIRE(renderer.batches()[1].events[1].value == 9.0f);
}

TEST_CASE("track automation renderer rejects incomplete device schedules atomically") {
    const auto map = tempo_map();
    const auto first = hold_program({10}, {{100}, 1}, map, 16);
    const auto second = hold_program({20}, {{200}, 2}, map, 16);
    auto renderer =
        take(TrackAutomationRenderer::create(track(map, {first, second})));
    MasterTransport transport;
    prepare(transport, *map, 4);
    const auto snapshot = block(transport, 4);

    const std::array missing{DeviceAutomationTransportView{{100}, &snapshot}};
    REQUIRE(renderer.process(missing).code ==
            TrackAutomationRendererCode::DeviceScheduleMismatch);
    const std::array reversed{
        DeviceAutomationTransportView{{200}, &snapshot},
        DeviceAutomationTransportView{{100}, &snapshot},
    };
    REQUIRE(renderer.process(reversed).code ==
            TrackAutomationRendererCode::DeviceScheduleMismatch);
    const std::array duplicate{
        DeviceAutomationTransportView{{100}, &snapshot},
        DeviceAutomationTransportView{{100}, &snapshot},
    };
    REQUIRE(renderer.process(duplicate).code ==
            TrackAutomationRendererCode::DeviceScheduleMismatch);
    const std::array null_transport{
        DeviceAutomationTransportView{{100}, &snapshot},
        DeviceAutomationTransportView{{200}, nullptr},
    };
    REQUIRE(renderer.process(null_transport).code ==
            TrackAutomationRendererCode::DeviceScheduleMismatch);

    const auto next_block = block(transport, 4);
    const std::array mismatched_block{
        DeviceAutomationTransportView{{100}, &snapshot},
        DeviceAutomationTransportView{{200}, &next_block},
    };
    const auto block_failure = renderer.process(mismatched_block);
    REQUIRE(block_failure.code == TrackAutomationRendererCode::DeviceScheduleMismatch);
    REQUIRE(block_failure.failed_device_placement_id == ItemId{200});

    MasterTransport short_transport;
    prepare(short_transport, *map, 4);
    const auto short_snapshot = block(short_transport, 3);
    const std::array mismatched_frames{
        DeviceAutomationTransportView{{100}, &snapshot},
        DeviceAutomationTransportView{{200}, &short_snapshot},
    };
    const auto frame_failure = renderer.process(mismatched_frames);
    REQUIRE(frame_failure.code == TrackAutomationRendererCode::DeviceScheduleMismatch);
    REQUIRE(frame_failure.failed_device_placement_id == ItemId{200});
    for (const auto& batch : renderer.batches())
        REQUIRE(batch.events.empty());

    const std::array complete{
        DeviceAutomationTransportView{{100}, &snapshot},
        DeviceAutomationTransportView{{200}, &snapshot},
    };
    REQUIRE(renderer.process(complete).code == TrackAutomationRendererCode::Ok);
    REQUIRE_FALSE(renderer.batches()[0].events.empty());
    REQUIRE_FALSE(renderer.batches()[1].events.empty());
}

TEST_CASE("track automation renderer preserves cursor carry across program adoption") {
    const auto map = tempo_map();
    const auto generation_one = program({10}, {{100}, 1}, map, 1, 32);
    auto renderer = take(
        TrackAutomationRenderer::create(track(map, {generation_one})));
    MasterTransport transport;
    prepare(transport, *map, 16);

    REQUIRE(renderer.process(block(transport, 16)).code == TrackAutomationRendererCode::Ok);
    const auto generation_two = program({10}, {{100}, 1}, map, 2, 32);
    const auto adopted = renderer.adopt(track(map, {generation_two}));
    REQUIRE(adopted);
    REQUIRE(adopted.value() == TrackAutomationRendererAdoption::Adopted);
    REQUIRE(renderer.process(block(transport, 16)).code == TrackAutomationRendererCode::Ok);

    const auto stale = renderer.adopt(track(map, {generation_one}));
    REQUIRE_FALSE(stale);
    REQUIRE(stale.error().code == TrackAutomationRendererCode::LaneAdoptionRejected);
    REQUIRE(stale.error().track_id == ItemId{5});
    REQUIRE(stale.error().lane_id == ItemId{10});
    REQUIRE(renderer.program()->find_lane({10})->generation() == 2);
}

TEST_CASE("track automation renderer rejects a different track without losing state") {
    const auto map = tempo_map();
    const auto lane = program({10}, {{100}, 1}, map, 1);
    auto renderer = take(TrackAutomationRenderer::create(track(map, {lane})));
    const auto other_track = take(TrackAutomationProgram::create({6}, map, {lane}));

    const auto rejected = renderer.adopt(other_track);
    REQUIRE_FALSE(rejected);
    REQUIRE(rejected.error().code == TrackAutomationRendererCode::TrackMismatch);
    REQUIRE(rejected.error().track_id == ItemId{6});
    REQUIRE(renderer.track_id() == ItemId{5});
}

TEST_CASE("track automation renderer fails closed when mandatory device topology exceeds capacity") {
    const auto map = tempo_map();
    std::vector<std::shared_ptr<const AutomationProgram>> programs;
    programs.reserve(513);
    for (std::uint32_t index = 0; index < 513; ++index) {
        const ItemId lane_id{1'000 + index};
        auto curve = take(AutomationCurve::create(
            {AutomationPoint{{100'000 + index * 2u}, map->samples_to_ticks({0}), 0.0f,
                             AutomationInterpolation::Hold},
             AutomationPoint{{100'001 + index * 2u}, map->samples_to_ticks({1}), 1.0f}}));
        auto lane = take(AutomationLane::create(
            lane_id, DeviceParameterTarget{{100}, index}, std::move(curve)));
        programs.push_back(take(AutomationProgram::compile(lane, map, 1)));
    }
    auto limits = AutomationPlaybackLimits{};
    limits.max_lanes_per_track = 513;
    auto renderer = take(
        TrackAutomationRenderer::create(track(map, std::move(programs)), limits));
    MasterTransport transport;
    prepare(transport, *map, 2);

    const auto failed = renderer.process(block(transport, 2));
    REQUIRE(failed.code == TrackAutomationRendererCode::DeviceCapacityExceeded);
    REQUIRE(failed.failed_device_placement_id == ItemId{100});
    REQUIRE_FALSE(failed.failed_lane_id.valid());
    REQUIRE(renderer.batches()[0].events.empty());
}

TEST_CASE("track automation renderer clears published batches on render failure") {
    const auto map = tempo_map();
    const auto lane = program({10}, {{100}, 1}, map, 1);
    auto renderer = take(TrackAutomationRenderer::create(track(map, {lane})));
    MasterTransport transport;
    prepare(transport, *map, 16);
    REQUIRE(renderer.process(block(transport, 9)).code == TrackAutomationRendererCode::Ok);
    REQUIRE_FALSE(renderer.batches()[0].events.empty());

    auto invalid = block(transport, 9);
    invalid.range_count = 0;
    const auto failed = renderer.process(invalid);
    REQUIRE(failed.code == TrackAutomationRendererCode::InvalidTransport);
    REQUIRE_FALSE(failed.failed_lane_id.valid());
    REQUIRE(renderer.batches()[0].events.empty());
}

TEST_CASE("track automation renderer rejects topology beyond prepared limits") {
    const auto map = tempo_map();
    const auto first = program({10}, {{100}, 1}, map, 1);
    const auto second = program({20}, {{200}, 2}, map, 1);

    auto lane_limits = AutomationPlaybackLimits{};
    lane_limits.max_lanes_per_track = 1;
    const auto too_many_lanes = TrackAutomationRenderer::create(
        track(map, {first, second}), lane_limits);
    REQUIRE_FALSE(too_many_lanes);
    REQUIRE(too_many_lanes.error().code ==
            TrackAutomationRendererCode::LaneCapacityExceeded);

    auto device_limits = AutomationPlaybackLimits{};
    device_limits.max_device_placements_per_track = 1;
    const auto too_many_devices = TrackAutomationRenderer::create(
        track(map, {first, second}), device_limits);
    REQUIRE_FALSE(too_many_devices);
    REQUIRE(too_many_devices.error().code ==
            TrackAutomationRendererCode::DeviceCapacityExceeded);

    auto invalid_limits = AutomationPlaybackLimits{};
    invalid_limits.max_intersecting_segments_per_block = 0;
    const auto invalid = TrackAutomationRenderer::create(track(map, {first}), invalid_limits);
    REQUIRE_FALSE(invalid);
    REQUIRE(invalid.error().code == TrackAutomationRendererCode::InvalidLimits);
}

TEST_CASE("track automation renderer bounds intersecting segment work") {
    const auto map = tempo_map();
    const auto lane = dense_continuous_program({10}, {{100}, 1}, map, 1'025);
    auto limits = AutomationPlaybackLimits{};
    limits.max_intersecting_segments_per_block = 1'024;
    auto renderer = take(TrackAutomationRenderer::create(track(map, {lane}), limits));
    MasterTransport transport;
    prepare(transport, *map, 1'025);

    const auto bounded = renderer.process(block(transport, 1'025));
    REQUIRE(bounded.code == TrackAutomationRendererCode::WorkCapacityExceeded);
    REQUIRE(bounded.failed_lane_id == ItemId{10});
    REQUIRE(renderer.batches()[0].events.empty());

    const auto continuous = program({20}, {{100}, 1}, map, 1, 1'999);
    limits.max_intersecting_segments_per_block = 2;
    auto coalesced =
        take(TrackAutomationRenderer::create(track(map, {continuous}), limits));
    MasterTransport second_transport;
    prepare(second_transport, *map, 2'000);
    const auto rendered = coalesced.process(block(second_transport, 2'000));
    REQUIRE(rendered.code == TrackAutomationRendererCode::Coalesced);
    REQUIRE(rendered.candidate_events > rendered.emitted_events);
}

TEST_CASE("track automation renderer rejects direct programs beyond point limits") {
    const auto map = tempo_map();
    const auto dense = hold_program({10}, {{100}, 1}, map, 5);

    auto lane_limits = AutomationPlaybackLimits{};
    lane_limits.max_points_per_lane = 4;
    lane_limits.max_points_per_track = 3;
    STATIC_REQUIRE(AutomationPlaybackLimits{
                       .max_points_per_lane = 4, .max_points_per_track = 3}
                       .valid());
    const auto lane_rejected =
        TrackAutomationRenderer::create(track(map, {dense}), lane_limits);
    REQUIRE_FALSE(lane_rejected);
    REQUIRE(lane_rejected.error().code ==
            TrackAutomationRendererCode::PointCapacityExceeded);
    REQUIRE(lane_rejected.error().lane_id == ItemId{10});

    const auto first = hold_program({20}, {{100}, 2}, map, 2);
    const auto second = hold_program({30}, {{100}, 3}, map, 2);
    auto track_limits = AutomationPlaybackLimits{};
    track_limits.max_points_per_lane = 2;
    track_limits.max_points_per_track = 3;
    const auto track_rejected = TrackAutomationRenderer::create(
        track(map, {first, second}), track_limits);
    REQUIRE_FALSE(track_rejected);
    REQUIRE(track_rejected.error().code ==
            TrackAutomationRendererCode::PointCapacityExceeded);
    REQUIRE(track_rejected.error().lane_id == ItemId{30});
}

TEST_CASE("track automation renderer honors configured per-device output headroom") {
    const auto map = tempo_map();
    const auto continuous = program({10}, {{100}, 1}, map, 1, 99);
    auto limits = AutomationPlaybackLimits{};
    limits.max_events_per_device_per_block = 16;
    auto renderer = take(
        TrackAutomationRenderer::create(track(map, {continuous}), limits));
    MasterTransport transport;
    prepare(transport, *map, 100);

    const auto rendered = renderer.process(block(transport, 100));
    REQUIRE(rendered.code == TrackAutomationRendererCode::Coalesced);
    REQUIRE(rendered.emitted_events == 16);
    REQUIRE(renderer.batches()[0].events.size() == 16);

    const auto mandatory = hold_program({20}, {{100}, 2}, map, 5);
    limits.max_events_per_device_per_block = 4;
    auto mandatory_renderer = take(
        TrackAutomationRenderer::create(track(map, {mandatory}), limits));
    MasterTransport mandatory_transport;
    prepare(mandatory_transport, *map, 5);
    const auto failed = mandatory_renderer.process(block(mandatory_transport, 5));
    REQUIRE(failed.code == TrackAutomationRendererCode::DeviceCapacityExceeded);
    REQUIRE(mandatory_renderer.batches()[0].events.empty());
}

TEST_CASE("track automation renderer never exposes partial batches when a later device fails") {
    const auto map = tempo_map();
    const auto first_device = program({10}, {{100}, 1}, map, 1, 599);
    const auto dense_one = hold_program({20}, {{200}, 2}, map, 600);
    const auto dense_two = hold_program({30}, {{200}, 3}, map, 600);
    auto renderer = take(TrackAutomationRenderer::create(
        track(map, {first_device, dense_one, dense_two})));
    MasterTransport transport;
    prepare(transport, *map, 600);

    const auto failed = renderer.process(block(transport, 600));
    REQUIRE(failed.code == TrackAutomationRendererCode::DeviceCapacityExceeded);
    REQUIRE(failed.failed_device_placement_id == ItemId{200});
    REQUIRE(renderer.batches().size() == 2);
    REQUIRE(renderer.batches()[0].events.empty());
    REQUIRE(renderer.batches()[1].events.empty());
}
