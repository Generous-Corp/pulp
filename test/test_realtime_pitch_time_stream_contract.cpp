#include <catch2/catch_test_macros.hpp>

#include <pulp/signal/realtime_pitch_time_processor.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

using namespace pulp::signal;

namespace {

constexpr double kSampleRate = 48000.0;
constexpr double kPi = 3.14159265358979323846;

RealtimePitchTimeConfig stream_config(int max_block) {
    RealtimePitchTimeConfig config;
    config.mode = PitchTimeMode::time_stretch;
    config.quality = PitchTimeQuality::low_latency;
    config.channels = 1;
    config.max_block = max_block;
    return config;
}

std::vector<float> sine(double frequency, double amplitude, int frames) {
    std::vector<float> samples(static_cast<std::size_t>(frames));
    for (int frame = 0; frame < frames; ++frame)
        samples[static_cast<std::size_t>(frame)] = static_cast<float>(
            amplitude * std::sin(2.0 * kPi * frequency * frame / kSampleRate));
    return samples;
}

void advance_to_final_buffer(RealtimePitchTimeProcessor& processor) {
    std::vector<float> discarded(256);
    float* output[] = {discarded.data()};
    for (int guard = 0; guard < 64; ++guard) {
        const int before = processor.available_stretched();
        const auto status = processor.finalize();
        REQUIRE(status != PitchTimeStreamFinalizeStatus::complete);
        if (status == PitchTimeStreamFinalizeStatus::draining
            && processor.available_stretched() > 0
            && processor.available_stretched() == before)
            return;
        if (status == PitchTimeStreamFinalizeStatus::backpressure) {
            const int count = std::min(processor.available_stretched(), 256);
            REQUIRE(count > 0);
            REQUIRE(processor.read_stretched(output, count) == count);
        }
    }
    FAIL("finite stream did not reach its final buffered output");
}

} // namespace

TEST_CASE("RealtimePitchTimeProcessor rejects non-positive prepared block capacity",
          "[signal][pitch-time][streaming]") {
    for (const int invalid : {0, -1}) {
        RealtimePitchTimeProcessor processor;
        REQUIRE(processor.prepare(kSampleRate, stream_config(invalid))
                == PitchTimePrepareStatus::invalid_max_block);
    }
}

TEST_CASE("RealtimePitchTimeProcessor rejects invalid prepared ratio bounds atomically",
          "[signal][pitch-time][streaming]") {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float infinity = std::numeric_limits<float>::infinity();
    const std::vector<float> invalid_time_ratios{0.0f, 0.5f, nan, infinity, -infinity};

    RealtimePitchTimeProcessor previously_prepared;
    REQUIRE(previously_prepared.prepare(kSampleRate, stream_config(256))
            == PitchTimePrepareStatus::prepared);
    const int prepared_capacity = previously_prepared.output_free_space();
    REQUIRE(prepared_capacity > 0);

    for (const float invalid : invalid_time_ratios) {
        auto config = stream_config(256);
        config.max_time_ratio = invalid;

        RealtimePitchTimeProcessor fresh;
        REQUIRE(fresh.prepare(kSampleRate, config)
                == PitchTimePrepareStatus::invalid_max_time_ratio);
        REQUIRE(fresh.output_free_space() == 0);

        REQUIRE(previously_prepared.prepare(kSampleRate, config)
                == PitchTimePrepareStatus::invalid_max_time_ratio);
        REQUIRE(previously_prepared.output_free_space() == prepared_capacity);
    }

    const float sample = 0.25f;
    const float* source[] = {&sample};
    REQUIRE(previously_prepared.feed(source, 1) == PitchTimeStreamFeedStatus::accepted);

    const std::vector<float> invalid_pitch_bounds{-1.0f, nan, infinity, -infinity,
                                                   std::numeric_limits<float>::max()};
    RealtimePitchTimeProcessor realtime_prepared;
    auto realtime_config = stream_config(256);
    realtime_config.mode = PitchTimeMode::realtime_pitch;
    REQUIRE(realtime_prepared.prepare(kSampleRate, realtime_config)
            == PitchTimePrepareStatus::prepared);
    const int prepared_fft_size = realtime_prepared.fft_size();

    for (const float invalid : invalid_pitch_bounds) {
        auto config = stream_config(256);
        config.max_pitch_semitones = invalid;

        RealtimePitchTimeProcessor fresh;
        REQUIRE(fresh.prepare(kSampleRate, config)
                == PitchTimePrepareStatus::invalid_max_pitch_semitones);
        REQUIRE(fresh.output_free_space() == 0);

        REQUIRE(realtime_prepared.prepare(kSampleRate, config)
                == PitchTimePrepareStatus::invalid_max_pitch_semitones);
        REQUIRE(realtime_prepared.fft_size() == prepared_fft_size);
        REQUIRE(realtime_prepared.feed(source, 1) == PitchTimeStreamFeedStatus::invalid_request);
    }
}

TEST_CASE("RealtimePitchTimeProcessor rejects unrepresentable prepared geometry atomically",
          "[signal][pitch-time][streaming]") {
    RealtimePitchTimeProcessor previously_prepared;
    REQUIRE(previously_prepared.prepare(kSampleRate, stream_config(256))
            == PitchTimePrepareStatus::prepared);
    const int prepared_capacity = previously_prepared.output_free_space();

    auto require_rejected_without_mutation = [&](RealtimePitchTimeConfig config,
                                                 PitchTimePrepareStatus expected) {
        RealtimePitchTimeProcessor fresh;
        REQUIRE(fresh.prepare(kSampleRate, config) == expected);
        REQUIRE(fresh.output_free_space() == 0);
        REQUIRE(previously_prepared.prepare(kSampleRate, config) == expected);
        REQUIRE(previously_prepared.output_free_space() == prepared_capacity);
    };

    auto pitch_hop_overflow = stream_config(256);
    pitch_hop_overflow.mode = PitchTimeMode::realtime_pitch;
    pitch_hop_overflow.max_pitch_semitones = 300.0f;
    require_rejected_without_mutation(pitch_hop_overflow,
                                      PitchTimePrepareStatus::unrepresentable_capacity);

    auto stretch_capacity_overflow = stream_config(256);
    stretch_capacity_overflow.max_time_ratio = std::numeric_limits<float>::max();
    require_rejected_without_mutation(stretch_capacity_overflow,
                                      PitchTimePrepareStatus::unrepresentable_capacity);

    auto block_capacity_overflow = stream_config(std::numeric_limits<int>::max());
    require_rejected_without_mutation(block_capacity_overflow,
                                      PitchTimePrepareStatus::unrepresentable_capacity);

    // The streaming ring fits, but the nested spectral engine's one-call backlog does not.
    auto engine_ring_capacity_overflow = stream_config(600'000'000);
    engine_ring_capacity_overflow.max_time_ratio = 1.0f;
    require_rejected_without_mutation(engine_ring_capacity_overflow,
                                      PitchTimePrepareStatus::unrepresentable_capacity);

    const auto infinite_rate = stream_config(256);
    RealtimePitchTimeProcessor fresh_rate;
    REQUIRE(fresh_rate.prepare(std::numeric_limits<double>::infinity(), infinite_rate)
            == PitchTimePrepareStatus::invalid_sample_rate);
    REQUIRE(fresh_rate.output_free_space() == 0);
    REQUIRE(previously_prepared.prepare(std::numeric_limits<double>::infinity(), infinite_rate)
            == PitchTimePrepareStatus::invalid_sample_rate);
    REQUIRE(previously_prepared.output_free_space() == prepared_capacity);

    const float sample = 0.25f;
    const float* source[] = {&sample};
    REQUIRE(previously_prepared.feed(source, 1) == PitchTimeStreamFeedStatus::accepted);
}

TEST_CASE("RealtimePitchTimeProcessor rejects invalid spectral overrides atomically",
          "[signal][pitch-time][streaming]") {
    RealtimePitchTimeProcessor previously_prepared;
    REQUIRE(previously_prepared.prepare(kSampleRate, stream_config(256))
            == PitchTimePrepareStatus::prepared);
    const int prepared_capacity = previously_prepared.output_free_space();

    for (const auto [fft_size, analysis_hop] :
         {std::pair{256, 256}, std::pair{32768, 512}}) {
        auto config = stream_config(256);
        config.fft_size = fft_size;
        config.analysis_hop = analysis_hop;

        RealtimePitchTimeProcessor fresh;
        REQUIRE(fresh.prepare(kSampleRate, config)
                == PitchTimePrepareStatus::invalid_spectral_geometry);
        REQUIRE(fresh.output_free_space() == 0);
        REQUIRE(previously_prepared.prepare(kSampleRate, config)
                == PitchTimePrepareStatus::invalid_spectral_geometry);
        REQUIRE(previously_prepared.output_free_space() == prepared_capacity);
    }

    const float sample = 0.25f;
    const float* source[] = {&sample};
    REQUIRE(previously_prepared.feed(source, 1) == PitchTimeStreamFeedStatus::accepted);
}

TEST_CASE("RealtimePitchTimeProcessor rejects target-byte capacity atomically",
          "[signal][pitch-time][streaming]") {
    constexpr std::uint64_t wasm32_max_bytes =
        std::numeric_limits<std::uint32_t>::max();
    auto oversized = stream_config(100'000'000);
    oversized.channels = 8;
    oversized.max_time_ratio = 1.0f;

    RealtimePitchTimeProcessor fresh;
    REQUIRE(fresh.prepare(kSampleRate, oversized, wasm32_max_bytes)
            == PitchTimePrepareStatus::unrepresentable_capacity);
    REQUIRE(fresh.output_free_space() == 0);

    RealtimePitchTimeProcessor previously_prepared;
    REQUIRE(previously_prepared.prepare(kSampleRate, stream_config(256))
            == PitchTimePrepareStatus::prepared);
    const int prepared_capacity = previously_prepared.output_free_space();
    REQUIRE(previously_prepared.prepare(kSampleRate, oversized, wasm32_max_bytes)
            == PitchTimePrepareStatus::unrepresentable_capacity);
    REQUIRE(previously_prepared.output_free_space() == prepared_capacity);

    // At 4x the wrapper's stream ring is 16384 floats while the nested engine
    // and every other prepared allocation fit below 40,000 bytes.
    auto outer_ring_only = stream_config(256);
    outer_ring_only.max_time_ratio = 4.0f;
    constexpr std::uint64_t below_outer_ring_bytes = 40'000;
    RealtimePitchTimeProcessor outer_fresh;
    REQUIRE(outer_fresh.prepare(kSampleRate, outer_ring_only, below_outer_ring_bytes)
            == PitchTimePrepareStatus::unrepresentable_capacity);
    REQUIRE(outer_fresh.output_free_space() == 0);
    REQUIRE(previously_prepared.prepare(kSampleRate, outer_ring_only,
                                        below_outer_ring_bytes)
            == PitchTimePrepareStatus::unrepresentable_capacity);
    REQUIRE(previously_prepared.output_free_space() == prepared_capacity);

    auto ordinary = stream_config(256);
    ordinary.max_time_ratio = 1.0f;
    ordinary.channels = 2;
    constexpr std::uint64_t float_capacity_bytes = 8'208ULL * sizeof(float);
    RealtimePitchTimeProcessor float_processor;
    REQUIRE(float_processor.prepare(kSampleRate, ordinary, float_capacity_bytes)
            == PitchTimePrepareStatus::prepared);

    RealtimePitchTimeProcessor64 double_fresh;
    REQUIRE(double_fresh.prepare(kSampleRate, ordinary, float_capacity_bytes)
            == PitchTimePrepareStatus::unrepresentable_capacity);
    REQUIRE(double_fresh.output_free_space() == 0);

    RealtimePitchTimeProcessor64 double_prepared;
    REQUIRE(double_prepared.prepare(kSampleRate, ordinary)
            == PitchTimePrepareStatus::prepared);
    const int double_capacity = double_prepared.output_free_space();
    REQUIRE(double_prepared.prepare(kSampleRate, ordinary, float_capacity_bytes)
            == PitchTimePrepareStatus::unrepresentable_capacity);
    REQUIRE(double_prepared.output_free_space() == double_capacity);
}

TEST_CASE("RealtimePitchTimeProcessor partial EOF read preserves the real prefix",
          "[signal][pitch-time][streaming]") {
    RealtimePitchTimeProcessor exact;
    RealtimePitchTimeProcessor partial;
    REQUIRE(exact.prepare(kSampleRate, stream_config(256)) == PitchTimePrepareStatus::prepared);
    REQUIRE(partial.prepare(kSampleRate, stream_config(256)) == PitchTimePrepareStatus::prepared);

    const auto input = sine(367.0, 0.5, 513);
    for (std::size_t offset = 0; offset < input.size();) {
        const int count = static_cast<int>(std::min<std::size_t>(256, input.size() - offset));
        const float* chunk[] = {input.data() + offset};
        REQUIRE(exact.feed(chunk, count) == PitchTimeStreamFeedStatus::accepted);
        REQUIRE(partial.feed(chunk, count) == PitchTimeStreamFeedStatus::accepted);
        offset += static_cast<std::size_t>(count);
    }

    advance_to_final_buffer(exact);
    advance_to_final_buffer(partial);
    const int available = exact.available_stretched();
    REQUIRE(available > 0);
    REQUIRE(partial.available_stretched() == available);

    std::vector<float> expected(static_cast<std::size_t>(available));
    float* expected_output[] = {expected.data()};
    REQUIRE(exact.read_stretched(expected_output, available) == available);

    constexpr int excess = 7;
    std::vector<float> actual(static_cast<std::size_t>(available + excess), 123.0f);
    float* actual_output[] = {actual.data()};
    REQUIRE(partial.read_stretched(actual_output, available + excess) == available);
    REQUIRE(std::equal(expected.begin(), expected.end(), actual.begin()));
    REQUIRE(std::all_of(actual.begin() + available, actual.end(),
                        [](float value) { return value == 0.0f; }));
    REQUIRE(partial.available_stretched() == 0);
    REQUIRE(partial.finalize() == PitchTimeStreamFinalizeStatus::complete);
}

TEST_CASE("RealtimePitchTimeProcessor finalize backpressure preserves buffered state",
          "[signal][pitch-time][streaming]") {
    auto config = stream_config(1024);
    config.max_time_ratio = 2.0f;
    RealtimePitchTimeProcessor processor;
    REQUIRE(processor.prepare(kSampleRate, config) == PitchTimePrepareStatus::prepared);
    processor.set_time_ratio(2.0f);

    std::vector<float> input(1024, 0.25f);
    const float* source[] = {input.data()};
    PitchTimeStreamFeedStatus feed_status = PitchTimeStreamFeedStatus::accepted;
    for (int guard = 0; guard < 1000; ++guard) {
        feed_status = processor.feed(source, 1024);
        if (feed_status == PitchTimeStreamFeedStatus::backpressure) break;
        REQUIRE(feed_status == PitchTimeStreamFeedStatus::accepted);
    }
    REQUIRE(feed_status == PitchTimeStreamFeedStatus::backpressure);

    const int available = processor.available_stretched();
    const int free_space = processor.output_free_space();
    REQUIRE(available > 0);
    REQUIRE(processor.finalize() == PitchTimeStreamFinalizeStatus::backpressure);
    REQUIRE(processor.available_stretched() == available);
    REQUIRE(processor.output_free_space() == free_space);
    REQUIRE(processor.feed(source, 1) == PitchTimeStreamFeedStatus::input_closed);
    REQUIRE(processor.finalize() == PitchTimeStreamFinalizeStatus::backpressure);
    REQUIRE(processor.available_stretched() == available);
    REQUIRE(processor.output_free_space() == free_space);

    std::vector<float> output_storage(1024);
    float* output[] = {output_storage.data()};
    PitchTimeStreamFinalizeStatus finalize_status = PitchTimeStreamFinalizeStatus::backpressure;
    for (int guard = 0; guard < 1000; ++guard) {
        const int count = std::min(processor.available_stretched(), 1024);
        if (count > 0) REQUIRE(processor.read_stretched(output, count) == count);
        finalize_status = processor.finalize();
        if (finalize_status == PitchTimeStreamFinalizeStatus::complete) break;
        REQUIRE((finalize_status == PitchTimeStreamFinalizeStatus::draining
                 || finalize_status == PitchTimeStreamFinalizeStatus::backpressure));
    }
    REQUIRE(finalize_status == PitchTimeStreamFinalizeStatus::complete);
    REQUIRE(processor.available_stretched() == 0);
}
