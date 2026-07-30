#include <catch2/catch_test_macros.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <pulp/playback/realtime_stretch_state_bank.hpp>

#include <array>
#include <cstdint>
#include <limits>

using namespace pulp;
using namespace pulp::playback;

namespace {

RealtimeStretchStateSpec spec(std::uint64_t id, std::uint32_t channels = 2,
                              float max_time_ratio = 2.0f) {
    return {{id}, channels, signal::PitchTimeQuality::low_latency, max_time_ratio};
}

AudioRendererLimits limits() {
    AudioRendererLimits value;
    value.max_channels = 2;
    value.max_block_frames = 256;
    value.max_realtime_stretch_states = 2;
    value.max_realtime_stretch_allocation_bytes = 16u * 1024u * 1024u;
    value.max_realtime_stretch_state_bytes = 32u * 1024u * 1024u;
    value.realtime_stretch_max_time_ratio = 2.0f;
    return value;
}

std::uint64_t retained_charge(const RealtimeStretchStateSpec& state,
                              std::uint32_t maximum_block_frames = 128,
                              std::uint64_t allocation_ceiling =
                                  std::numeric_limits<std::uint64_t>::max()) {
    signal::RealtimePitchTimeConfig config;
    config.mode = signal::PitchTimeMode::time_stretch;
    config.quality = state.quality;
    config.channels = static_cast<int>(state.channels);
    config.max_block = static_cast<int>(maximum_block_frames);
    config.max_time_ratio = state.max_time_ratio;
    signal::RealtimePitchTimePreparedGeometry<float> geometry;
    REQUIRE(signal::checked_realtime_pitch_time_prepared_geometry(
                config, 1.0, allocation_ceiling, geometry)
            == signal::PitchTimePrepareStatus::prepared);
    return geometry.retained_bytes;
}

} // namespace

TEST_CASE("realtime stretch bank admission is immutable and exactly bounded") {
    const std::array specs{spec(10), spec(20, 1, 1.5f)};
    const auto configured = limits();
    const auto admitted =
        admit_realtime_stretch_state_bank(specs, 48'000.0, 128, configured);
    REQUIRE(admitted);
    REQUIRE(admitted.actual == specs.size());
    REQUIRE(admitted.limit == configured.max_realtime_stretch_states);
    const auto expected_bytes = retained_charge(specs[0]) + retained_charge(specs[1]);
    REQUIRE(admitted.reserved_state_bytes == expected_bytes);

    auto rejected = admit_realtime_stretch_state_bank(
        std::array{spec(10), spec(20), spec(30)}, 48'000.0, 128, configured);
    REQUIRE(rejected.code == RealtimeStretchStateBankError::StateLimitExceeded);
    REQUIRE(rejected.actual == 3);
    REQUIRE(rejected.limit == 2);

    auto byte_limited = configured;
    byte_limited.max_realtime_stretch_state_bytes = expected_bytes - 1;
    rejected = admit_realtime_stretch_state_bank(specs, 48'000.0, 128, byte_limited);
    REQUIRE(rejected.code == RealtimeStretchStateBankError::StateBytesExceeded);
    REQUIRE(rejected.actual == expected_bytes);
    REQUIRE(rejected.limit == expected_bytes - 1);

    rejected = admit_realtime_stretch_state_bank(
        std::array{spec(10), spec(10)}, 48'000.0, 128, configured);
    REQUIRE(rejected.code == RealtimeStretchStateBankError::DuplicateIdentity);
    REQUIRE(rejected.clip_id == timeline::ItemId{10});

    rejected = admit_realtime_stretch_state_bank(
        std::array{spec(10, 3)}, 48'000.0, 128, configured);
    REQUIRE(rejected.code == RealtimeStretchStateBankError::ChannelLimitExceeded);

    rejected = admit_realtime_stretch_state_bank(
        std::array{spec(10, 2, 2.5f)}, 48'000.0, 128, configured);
    REQUIRE(rejected.code == RealtimeStretchStateBankError::TimeRatioLimitExceeded);

    auto allocation_limited = configured;
    allocation_limited.max_realtime_stretch_allocation_bytes = 1;
    allocation_limited.max_realtime_stretch_state_bytes = 2;
    rejected = admit_realtime_stretch_state_bank(
        std::array{spec(10)}, 48'000.0, 128, allocation_limited);
    REQUIRE(rejected.code == RealtimeStretchStateBankError::ProcessorPrepareRejected);
    REQUIRE(rejected.processor_status == signal::PitchTimePrepareStatus::unrepresentable_capacity);
}

TEST_CASE("realtime stretch bank retained-byte accounting survives uint64 limits") {
    const std::array specs{spec(10), spec(20, 1, 1.5f)};
    auto configured = limits();
    configured.max_realtime_stretch_allocation_bytes =
        std::numeric_limits<std::uint64_t>::max();
    configured.max_realtime_stretch_state_bytes =
        std::numeric_limits<std::uint64_t>::max();

    const auto expected = retained_charge(specs[0]) + retained_charge(specs[1]);
    const auto admitted =
        admit_realtime_stretch_state_bank(specs, 48'000.0, 128, configured);
    REQUIRE(admitted);
    REQUIRE(admitted.reserved_state_bytes == expected);
    REQUIRE(admitted.reserved_state_bytes < configured.max_realtime_stretch_state_bytes);

    configured.max_realtime_stretch_state_bytes = expected - 1;
    const auto rejected =
        admit_realtime_stretch_state_bank(specs, 48'000.0, 128, configured);
    REQUIRE(rejected.code == RealtimeStretchStateBankError::StateBytesExceeded);
    REQUIRE(rejected.actual == expected);
    REQUIRE(rejected.limit == expected - 1);
}

TEST_CASE("realtime stretch bank prepare rolls back the complete live bank") {
    RealtimeStretchStateBank bank;
    const auto configured = limits();
    REQUIRE(bank.prepare(std::array{spec(10)}, 48'000.0, 128, configured));
    REQUIRE(bank.size() == 1);
    const auto reserved = bank.reserved_state_bytes();
    auto* const original = bank.state_for_epoch({10}, 7);
    REQUIRE(original != nullptr);

    const auto rejected =
        bank.prepare(std::array{spec(20), spec(20)}, 48'000.0, 128, configured);
    REQUIRE(rejected.code == RealtimeStretchStateBankError::DuplicateIdentity);
    REQUIRE(bank.size() == 1);
    REQUIRE(bank.reserved_state_bytes() == reserved);
    REQUIRE(bank.find({20}) == nullptr);
    REQUIRE(bank.state_for_epoch({10}, 7) == original);

    REQUIRE(bank.prepare(std::array{spec(20)}, 48'000.0, 128, configured));
    REQUIRE(bank.size() == 1);
    REQUIRE(bank.find({10}) == nullptr);
    REQUIRE(bank.find({20}) != nullptr);
}

TEST_CASE("prepared realtime stretch bank audio-thread operations allocate nothing") {
    RealtimeStretchStateBank bank;
    REQUIRE(bank.prepare(std::array{spec(10)}, 48'000.0, 128, limits()));

    std::array<float, 128> left{};
    std::array<float, 128> right{};
    const float* input[] = {left.data(), right.data()};
    signal::PitchTimeStreamFeedStatus feed =
        signal::PitchTimeStreamFeedStatus::invalid_request;
    std::size_t allocations = 1;
    {
        test::RtAllocationProbe probe;
        auto* processor = bank.state_for_epoch({10}, 1);
        REQUIRE(processor != nullptr);
        for (int block = 0; block < 12; ++block)
            feed = processor->feed(input, static_cast<int>(left.size()));
        const auto available = processor->available_stretched();
        REQUIRE(available > 0);
        REQUIRE(bank.state_for_epoch({10}, 1) == processor);
        REQUIRE(processor->available_stretched() == available);
        REQUIRE(bank.state_for_epoch({10}, 2) == processor);
        REQUIRE(processor->available_stretched() == 0);
        REQUIRE(bank.find({10}) == processor);
        bank.reset();
        allocations = probe.allocation_count();
    }
    REQUIRE(feed == signal::PitchTimeStreamFeedStatus::accepted);
    REQUIRE(allocations == 0);
}
