#include <pulp/playback/track_automation_renderer.hpp>

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
    auto renderer = take(TrackAutomationRenderer::create(track(map, std::move(programs))));
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
