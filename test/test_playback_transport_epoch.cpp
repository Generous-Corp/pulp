#include <pulp/playback/transport.hpp>

#include "timebase_test_helpers.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <limits>

using namespace pulp;
using namespace pulp::playback;
using namespace pulp::timebase;

namespace {

CompiledTempoMap constant_map() {
    const std::array points{TempoPoint{{0}, 120.0}};
    return require_compiled_tempo_map(points, RationalRate{48'000, 1});
}

MasterTransportConfig config(std::uint32_t maximum = 1024) {
    MasterTransportConfig result;
    result.max_buffer_size = maximum;
    return result;
}

TransportSnapshot block(MasterTransport& transport, std::uint32_t frames) {
    TransportSnapshot snapshot;
    REQUIRE(transport.begin_block(frames, snapshot) == TransportError::None);
    return snapshot;
}

TickPosition tick_at(const CompiledTempoMap& map, std::int64_t sample) {
    const auto tick = map.samples_to_ticks({sample});
    REQUIRE(map.ticks_to_samples(tick) == SamplePosition{sample});
    return tick;
}

} // namespace

TEST_CASE("playback epoch advances only for start seek reset and loop identity changes",
          "[playback][transport][epoch]") {
    const auto map = constant_map();
    MasterTransport transport;
    REQUIRE(transport.prepare(map, config()) == TransportError::None);

    const auto initial = block(transport, 64);
    REQUIRE(initial.ranges[0].playback_epoch == initial.playback_epoch);
    const auto steady = block(transport, 64);
    REQUIRE(steady.playback_epoch == initial.playback_epoch);

    REQUIRE(transport.set_playing(true) == TransportError::None);
    const auto started = block(transport, 64);
    REQUIRE(started.playback_epoch == initial.playback_epoch + 1);
    REQUIRE(started.ranges[0].playback_epoch == started.playback_epoch);
    REQUIRE(block(transport, 64).playback_epoch == started.playback_epoch);

    REQUIRE(transport.set_playing(false) == TransportError::None);
    const auto stopped = block(transport, 64);
    REQUIRE(stopped.playback_epoch == started.playback_epoch);
    REQUIRE(transport.set_playing(true) == TransportError::None);
    const auto restarted = block(transport, 64);
    REQUIRE(restarted.playback_epoch == stopped.playback_epoch + 1);

    REQUIRE(transport.seek({kTicksPerQuarter}) == TransportError::None);
    const auto seeked = block(transport, 64);
    REQUIRE(seeked.playback_epoch == restarted.playback_epoch + 1);
    REQUIRE(transport.seek({kTicksPerQuarter}) == TransportError::None);
    const auto explicitly_reseeked = block(transport, 64);
    REQUIRE(explicitly_reseeked.playback_epoch == seeked.playback_epoch + 1);

    const LoopRegion loop{true, {0}, {2 * kTicksPerQuarter}};
    REQUIRE(transport.set_loop(loop) == TransportError::None);
    const auto enabled_loop = block(transport, 64);
    REQUIRE(enabled_loop.playback_epoch == explicitly_reseeked.playback_epoch + 1);
    REQUIRE(transport.set_loop(loop) == TransportError::None);
    REQUIRE(block(transport, 64).playback_epoch == enabled_loop.playback_epoch);

    REQUIRE(transport.set_loop({true, {kTicksPerQuarter}, {3 * kTicksPerQuarter}})
            == TransportError::None);
    const auto moved_loop = block(transport, 64);
    REQUIRE(moved_loop.playback_epoch == enabled_loop.playback_epoch + 1);
    REQUIRE(transport.set_loop({false, {}, {}}) == TransportError::None);
    const auto disabled_loop = block(transport, 64);
    REQUIRE(disabled_loop.playback_epoch == moved_loop.playback_epoch + 1);

    transport.reset();
    REQUIRE(transport.prepare(map, config()) == TransportError::None);
    REQUIRE(block(transport, 64).playback_epoch > disabled_loop.playback_epoch);
}

TEST_CASE("ordinary loop advance and wrap retain playback epoch",
          "[playback][transport][epoch]") {
    const auto map = constant_map();
    const LoopRegion loop{true, {0}, {kTicksPerQuarter}};
    auto setup = config();
    setup.initially_playing = true;
    setup.loop = loop;
    setup.initial_position = map.samples_to_ticks({23'500});
    MasterTransport transport;
    REQUIRE(transport.prepare(map, setup) == TransportError::None);

    const auto wrapped = block(transport, 1024);
    REQUIRE(wrapped.range_count == 2);
    REQUIRE(wrapped.ranges[0].playback_epoch == wrapped.playback_epoch);
    REQUIRE(wrapped.ranges[1].playback_epoch == wrapped.playback_epoch);
    REQUIRE(wrapped.ranges[0].loop_pass_index == 0);
    REQUIRE(wrapped.ranges[1].loop_pass_index == 1);

    const auto advanced = block(transport, 1024);
    REQUIRE(advanced.playback_epoch == wrapped.playback_epoch);
    REQUIRE(advanced.ranges[0].playback_epoch == wrapped.playback_epoch);
    REQUIRE(advanced.ranges[0].loop_pass_index == 1);
}

TEST_CASE("transport range validation rejects incoherent playback epochs",
          "[playback][transport][epoch][negative]") {
    const auto map = constant_map();
    const LoopRegion loop{true, {0}, {kTicksPerQuarter}};
    auto setup = config();
    setup.initially_playing = true;
    setup.loop = loop;
    setup.initial_position = map.samples_to_ticks({23'500});
    MasterTransport transport;
    REQUIRE(transport.prepare(map, setup) == TransportError::None);
    const auto valid = block(transport, 1024);
    REQUIRE(valid.range_count == 2);
    REQUIRE(valid_transport_ranges(valid));

    auto next_epoch = valid;
    next_epoch.ranges[1].playback_epoch = valid.ranges[0].playback_epoch + 1;
    REQUIRE_FALSE(valid_transport_ranges(next_epoch));

    auto regression = valid;
    regression.ranges[1].playback_epoch = valid.ranges[0].playback_epoch - 1;
    REQUIRE_FALSE(valid_transport_ranges(regression));

    auto jump = valid;
    jump.ranges[1].playback_epoch = valid.ranges[0].playback_epoch + 2;
    REQUIRE_FALSE(valid_transport_ranges(jump));

    auto unmarked_change = next_epoch;
    unmarked_change.ranges[1].discontinuity = false;
    REQUIRE_FALSE(valid_transport_ranges(unmarked_change));

    auto wrapped_identity = valid;
    wrapped_identity.playback_epoch = std::numeric_limits<std::uint64_t>::max();
    wrapped_identity.ranges[0].playback_epoch = wrapped_identity.playback_epoch;
    wrapped_identity.ranges[1].playback_epoch = 0;
    REQUIRE_FALSE(valid_transport_ranges(wrapped_identity));
}

TEST_CASE("playback epoch advancement fails closed at exhaustion",
          "[playback][transport][epoch][negative]") {
    auto epoch = std::numeric_limits<std::uint64_t>::max() - 1;
    REQUIRE(pulp::playback::detail::advance_playback_epoch(epoch) == TransportError::None);
    REQUIRE(epoch == std::numeric_limits<std::uint64_t>::max());
    REQUIRE(pulp::playback::detail::advance_playback_epoch(epoch)
            == TransportError::PlaybackEpochExhausted);
    REQUIRE(epoch == std::numeric_limits<std::uint64_t>::max());
}

TEST_CASE("playback epoch follows scrub entry anchor application and exit",
          "[playback][transport][epoch]") {
    const auto map = constant_map();
    MasterTransport transport;
    REQUIRE(transport.prepare(map, config(300)) == TransportError::None);
    const auto baseline = block(transport, 300);
    const auto first = tick_at(map, 24'000);
    const auto second = tick_at(map, 96'000);

    REQUIRE(transport.begin_scrub(1'024, first) == TransportError::None);
    const auto entered = block(transport, 300);
    REQUIRE(entered.playback_epoch == baseline.playback_epoch + 1);
    REQUIRE(entered.ranges[0].playback_epoch == entered.playback_epoch);
    REQUIRE(block(transport, 300).playback_epoch == entered.playback_epoch);
    REQUIRE(block(transport, 300).playback_epoch == entered.playback_epoch);

    // The posted anchor remains latent for the remaining 124 frames of the
    // current window. Its epoch starts exactly at the discontinuous range that
    // applies it, not at the control call or the start of this split block.
    REQUIRE(transport.scrub_to(second) == TransportError::None);
    const auto split = block(transport, 300);
    REQUIRE(split.range_count == 2);
    REQUIRE(split.playback_epoch == entered.playback_epoch);
    REQUIRE(split.ranges[0].playback_epoch == entered.playback_epoch);
    REQUIRE(split.ranges[1].playback_epoch == entered.playback_epoch + 1);
    REQUIRE_FALSE(split.ranges[0].discontinuity);
    REQUIRE(split.ranges[1].discontinuity);
    REQUIRE(split.ranges[1].timeline_sample_start == map.ticks_to_samples(second));
    REQUIRE(valid_transport_ranges(split));

    const auto anchored_epoch = split.ranges[1].playback_epoch;
    const auto anchored = block(transport, 300);
    REQUIRE(anchored.playback_epoch == anchored_epoch);
    REQUIRE(transport.scrub_to(second) == TransportError::None);
    REQUIRE(block(transport, 300).playback_epoch == anchored_epoch);

    // A fresh drag abandons the window in flight. Reusing the same anchor is
    // still a discontinuity, unlike an ordinary periodic grain restart.
    REQUIRE(transport.begin_scrub(1'024, second) == TransportError::None);
    const auto restarted = block(transport, 300);
    REQUIRE(restarted.playback_epoch == anchored_epoch + 1);
    REQUIRE(restarted.ranges[0].discontinuity);
    const auto restarted_epoch = restarted.playback_epoch;

    REQUIRE(transport.end_scrub() == TransportError::None);
    const auto exited = block(transport, 300);
    REQUIRE(exited.playback_epoch == restarted_epoch + 1);
    REQUIRE(exited.ranges[0].playback_epoch == exited.playback_epoch);
    REQUIRE(block(transport, 300).playback_epoch == exited.playback_epoch);
}
