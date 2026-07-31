#include <catch2/catch_test_macros.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <pulp/playback/realtime_stretch_state_bank.hpp>
#include <pulp/signal/realtime_pitch_time_geometry.hpp>

#include <array>
#include <cstdint>
#include <limits>

using namespace pulp;
using namespace pulp::playback;

namespace {

RealtimeStretchStateSpec spec(std::uint64_t id, std::uint32_t channels = 2,
                              float max_time_ratio = 2.0f) {
    return {{id}, channels, audio::RealtimeTimeStretchQuality::low_latency, max_time_ratio};
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
    audio::RealtimeTimeStretchConfig config;
    config.quality = state.quality;
    config.channels = static_cast<int>(state.channels);
    config.max_block = static_cast<int>(maximum_block_frames);
    config.max_time_ratio = state.max_time_ratio;
    audio::RealtimeTimeStretchPreparedGeometry geometry;
    REQUIRE(audio::checked_realtime_time_stretch_prepared_geometry(
                config, allocation_ceiling, geometry)
            == audio::RealtimeTimeStretchPrepareStatus::prepared);
    return geometry.retained_bytes;
}

std::uint64_t minimum_signal_allocation_ceiling(
    const signal::RealtimePitchTimeConfig& config) {
    std::uint64_t low = 1;
    std::uint64_t high = 16u * 1024u * 1024u;
    signal::RealtimePitchTimePreparedGeometry<float> geometry;
    REQUIRE(signal::checked_realtime_pitch_time_prepared_geometry(
                config, 1.0, high, geometry)
            == signal::PitchTimePrepareStatus::prepared);
    while (low < high) {
        const auto middle = low + (high - low) / 2u;
        const auto status = signal::checked_realtime_pitch_time_prepared_geometry(
            config, 1.0, middle, geometry);
        if (status == signal::PitchTimePrepareStatus::prepared)
            high = middle;
        else
            low = middle + 1u;
    }
    return low;
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
    REQUIRE(rejected.processor_status ==
            audio::RealtimeTimeStretchPrepareStatus::unrepresentable_capacity);
}

TEST_CASE("audio stretch facade preserves the signal per-allocation ceiling") {
    signal::RealtimePitchTimeConfig signal_config;
    signal_config.mode = signal::PitchTimeMode::time_stretch;
    signal_config.quality = signal::PitchTimeQuality::low_latency;
    signal_config.channels = 2;
    signal_config.max_block = 128;
    signal_config.max_time_ratio = 2.0f;
    const auto exact_ceiling = minimum_signal_allocation_ceiling(signal_config);

    signal::RealtimePitchTimePreparedGeometry<float> signal_geometry;
    REQUIRE(signal::checked_realtime_pitch_time_prepared_geometry(
                signal_config, 1.0, exact_ceiling, signal_geometry)
            == signal::PitchTimePrepareStatus::prepared);
    const auto signal_retained_bytes = signal_geometry.retained_bytes;
    REQUIRE(signal::checked_realtime_pitch_time_prepared_geometry(
                signal_config, 1.0, exact_ceiling - 1u, signal_geometry)
            == signal::PitchTimePrepareStatus::unrepresentable_capacity);

    audio::RealtimeTimeStretchConfig audio_config;
    audio_config.quality = audio::RealtimeTimeStretchQuality::low_latency;
    audio_config.channels = 2;
    audio_config.max_block = 128;
    audio_config.max_time_ratio = 2.0f;
    audio::RealtimeTimeStretchPreparedGeometry audio_geometry;
    REQUIRE(audio::checked_realtime_time_stretch_prepared_geometry(
                audio_config, exact_ceiling, audio_geometry)
            == audio::RealtimeTimeStretchPrepareStatus::prepared);
    REQUIRE(audio_geometry.retained_bytes > signal_retained_bytes);

    audio::RealtimeTimeStretchProcessor processor;
    REQUIRE(processor.prepare(48'000.0, audio_config, exact_ceiling) ==
            audio::RealtimeTimeStretchPrepareStatus::prepared);
}

TEST_CASE("audio stretch facade reports precise invalid preparation status") {
    audio::RealtimeTimeStretchConfig config;
    config.quality = audio::RealtimeTimeStretchQuality::low_latency;
    config.channels = 2;
    config.max_block = 128;
    config.max_time_ratio = 2.0f;
    audio::RealtimeTimeStretchProcessor processor;

    REQUIRE(processor.prepare(0.0, config, 1) ==
            audio::RealtimeTimeStretchPrepareStatus::invalid_sample_rate);

    config.channels = 0;
    REQUIRE(processor.prepare(48'000.0, config, 1) ==
            audio::RealtimeTimeStretchPrepareStatus::invalid_channel_count);
    config.channels = 2;

    config.max_block = 0;
    REQUIRE(processor.prepare(48'000.0, config, 1) ==
            audio::RealtimeTimeStretchPrepareStatus::invalid_max_block);
    config.max_block = 128;

    config.max_time_ratio = 0.5f;
    REQUIRE(processor.prepare(48'000.0, config, 1) ==
            audio::RealtimeTimeStretchPrepareStatus::invalid_max_time_ratio);
    config.max_time_ratio = 2.0f;

    config.fft_size = 300;
    config.analysis_hop = 100;
    REQUIRE(processor.prepare(48'000.0, config, 1) ==
            audio::RealtimeTimeStretchPrepareStatus::invalid_spectral_geometry);
    config.fft_size = 0;
    config.analysis_hop = 0;

    REQUIRE(processor.prepare(48'000.0, config, 1) ==
            audio::RealtimeTimeStretchPrepareStatus::unrepresentable_capacity);
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
    audio::RealtimeTimeStretchStreamFeedStatus feed =
        audio::RealtimeTimeStretchStreamFeedStatus::invalid_request;
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
    REQUIRE(feed == audio::RealtimeTimeStretchStreamFeedStatus::accepted);
    REQUIRE(allocations == 0);
}
