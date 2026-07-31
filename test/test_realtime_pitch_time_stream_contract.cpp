#include <catch2/catch_test_macros.hpp>

#include <pulp/signal/realtime_pitch_time_processor.hpp>

#include <algorithm>
#include <array>
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

template <typename Element>
constexpr std::uint64_t store_bytes(std::uint64_t elements) {
    return elements * sizeof(Element);
}

struct RetainedChargeOracle {
    std::uint64_t total = 0;
    std::uint64_t platform_fft_setups = 0;
    std::uint64_t morpher_env_a = 0;
    std::uint64_t morpher_env_b = 0;
    std::uint64_t magnitude_scratch = 0;
};

RetainedChargeOracle exact_owned_store_charge(
    const RealtimePitchTimeConfig& config,
    const RealtimePitchTimePreparedGeometry<float>& prepared) {
    const auto engine = checked_spectral_frame_engine_geometry<float>(prepared.engine_config);
    REQUIRE(engine);
    const auto channels = static_cast<std::uint64_t>(config.channels);
    const auto fft_size = static_cast<std::uint64_t>(prepared.fft_size);
    const auto bins = fft_size / 2u + 1u;
    const auto channel_bins = channels * bins;
    const auto capture_bins = 8u * bins;

    RetainedChargeOracle result;
#if PULP_FFT_HAS_VDSP
    result.platform_fft_setups =
        2u * fft_size * kVdspFftSetupChargeBytesPerPoint;
    const auto platform_split_buffers = 4u * store_bytes<float>(fft_size);
#else
    const std::uint64_t platform_split_buffers = 0;
#endif

    const std::array base_stores{
        store_bytes<float>(engine->input_ring_elements),
        store_bytes<float>(engine->output_ring_elements),
        store_bytes<float>(engine->ring_size),
        store_bytes<float>(fft_size), // engine window
        store_bytes<float>(fft_size), // engine time scratch
        store_bytes<std::complex<float>>(engine->frame_elements),
        store_bytes<std::complex<float>>(fft_size), // engine frequency scratch
        store_bytes<std::complex<float>*>(channels),
        store_bytes<std::complex<double>>(fft_size / 2u), // engine twiddles
        platform_split_buffers,
        result.platform_fft_setups,
        store_bytes<float>(prepared.stretch_ring_elements),
        store_bytes<float>(prepared.drain_elements),
        store_bytes<float>(prepared.finalize_zero_elements),
        store_bytes<float*>(channels),
        store_bytes<const float*>(channels),
        3u * store_bytes<double>(bins), // coordinator phases
        store_bytes<float>(bins), // coordinator reference magnitude
        store_bytes<int>(bins), // coordinator peaks
        store_bytes<float>(bins), // transient history
        store_bytes<float>(channels * capture_bins),
        store_bytes<double>(capture_bins),
        store_bytes<float>(channel_bins),
        store_bytes<double>(channel_bins),
        store_bytes<double>(bins),
        3u * store_bytes<float>(bins), // envelope scratch vectors
        store_bytes<std::complex<float>>(fft_size), // envelope cepstrum
        store_bytes<std::complex<double>>(fft_size / 2u), // envelope twiddles
    };
    for (const auto bytes : base_stores) result.total += bytes;

    if (config.noise_morphing) {
        const auto time_median = config.quality == PitchTimeQuality::quality ? 7u : 5u;
        const auto freq_median = config.quality == PitchTimeQuality::quality ? 11u : 7u;
        result.morpher_env_a = channels * store_bytes<float>(bins);
        result.morpher_env_b = channels * store_bytes<float>(bins);
        result.magnitude_scratch = store_bytes<float>(bins);
        const std::array noise_stores{
            store_bytes<float>(time_median * bins),
            5u * store_bytes<float>(bins), // three masks plus harmonic/percussive
            store_bytes<float>(time_median),
            store_bytes<float>(freq_median),
            store_bytes<NoiseMorpher>(channels),
            result.morpher_env_a,
            result.morpher_env_b,
            result.magnitude_scratch,
            store_bytes<float>(channel_bins),
            store_bytes<std::complex<float>>(bins),
        };
        for (const auto bytes : noise_stores) result.total += bytes;
    }
    if (config.sinc_resampling)
        result.total += store_bytes<float>(513u * 32u) + store_bytes<float>(32u);
    return result;
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

bool stream_lag_stays_within_prepared_bound(int max_block, float admitted_ratio,
                                            float active_ratio) {
    auto config = stream_config(max_block);
    config.max_time_ratio = admitted_ratio;
    RealtimePitchTimePreparedGeometry<float> geometry;
    REQUIRE(checked_realtime_pitch_time_prepared_geometry(
                config, 1.0, std::numeric_limits<std::uint64_t>::max(), geometry)
            == PitchTimePrepareStatus::prepared);

    RealtimePitchTimeProcessor processor;
    REQUIRE(processor.prepare(kSampleRate, config) == PitchTimePrepareStatus::prepared);
    REQUIRE(processor.maximum_stream_output_lag_samples()
            == geometry.maximum_stream_output_lag_samples);
    processor.set_time_ratio(active_ratio);

    constexpr std::int64_t input_frames = 12'037;
    std::vector<float> input(static_cast<std::size_t>(max_block), 0.0f);
    std::vector<float> output(static_cast<std::size_t>(max_block), 0.0f);
    const float* input_ptrs[] = {input.data()};
    float* output_ptrs[] = {output.data()};
    const std::array<int, 7> requested{1, 17, 3, 251, 29, 7, 113};
    std::int64_t accepted = 0;
    std::int64_t drained = 0;
    std::size_t request_index = 0;
    bool observed_backpressure = false;

    const auto require_bound = [&] {
        const auto produced = drained + processor.available_stretched();
        const auto ideal = static_cast<std::int64_t>(
            std::ceil(static_cast<long double>(accepted)
                      * static_cast<long double>(active_ratio)));
        REQUIRE(std::max<std::int64_t>(0, ideal - produced)
                <= geometry.maximum_stream_output_lag_samples);
    };

    while (accepted < input_frames) {
        const auto wanted = std::min<std::int64_t>(
            std::min(requested[request_index++ % requested.size()], max_block),
            input_frames - accepted);
        const auto status = processor.feed(input_ptrs, static_cast<int>(wanted));
        if (status == PitchTimeStreamFeedStatus::accepted) {
            accepted += wanted;
            require_bound();
            continue;
        }
        REQUIRE(status == PitchTimeStreamFeedStatus::backpressure);
        observed_backpressure = true;
        const int take = std::min(processor.available_stretched(), max_block);
        REQUIRE(take > 0);
        REQUIRE(processor.read_stretched(output_ptrs, take) == take);
        drained += take;
        require_bound();
    }

    const auto final_output = static_cast<std::int64_t>(
        std::llround(static_cast<long double>(accepted)
                     * static_cast<long double>(active_ratio)));
    for (int guard = 0; guard < 100'000; ++guard) {
        const int available = processor.available_stretched();
        if (available > 0) {
            const int take = std::min(available, max_block);
            REQUIRE(processor.read_stretched(output_ptrs, take) == take);
            drained += take;
        }
        REQUIRE(std::max<std::int64_t>(
                    0, final_output - drained - processor.available_stretched())
                <= geometry.maximum_stream_output_lag_samples);
        const auto status = processor.finalize(max_block);
        REQUIRE(status != PitchTimeStreamFinalizeStatus::invalid_mode);
        REQUIRE(status != PitchTimeStreamFinalizeStatus::invalid_request);
        if (status == PitchTimeStreamFinalizeStatus::complete) {
            REQUIRE(processor.available_stretched() == 0);
            REQUIRE(drained == final_output);
            return observed_backpressure;
        }
    }
    FAIL("time-stretch finalization did not converge");
    return observed_backpressure;
}

} // namespace

TEST_CASE("RealtimePitchTime geometry exposes a checked causal stream lag bound",
          "[signal][pitch-time][streaming]") {
    for (const auto quality : {PitchTimeQuality::low_latency, PitchTimeQuality::quality}) {
        for (const float maximum_ratio : {1.0f, 1.0001f, 2.0f, 4.0f}) {
            auto config = stream_config(37);
            config.quality = quality;
            config.max_time_ratio = maximum_ratio;
            RealtimePitchTimePreparedGeometry<float> geometry;
            REQUIRE(checked_realtime_pitch_time_prepared_geometry(
                        config, 1.0, std::numeric_limits<std::uint64_t>::max(), geometry)
                    == PitchTimePrepareStatus::prepared);
            const auto expected = static_cast<int>(std::ceil(
                static_cast<long double>(geometry.fft_size + geometry.analysis_hop)
                    * static_cast<long double>(maximum_ratio)
                + static_cast<long double>(geometry.engine_config.max_synthesis_hop)));
            REQUIRE(geometry.maximum_stream_output_lag_samples == expected);
            REQUIRE(geometry.maximum_stream_output_lag_samples > 0);
        }
    }

    auto realtime = stream_config(37);
    realtime.mode = PitchTimeMode::realtime_pitch;
    RealtimePitchTimePreparedGeometry<float> realtime_geometry;
    REQUIRE(checked_realtime_pitch_time_prepared_geometry(
                realtime, 2.0, std::numeric_limits<std::uint64_t>::max(), realtime_geometry)
            == PitchTimePrepareStatus::prepared);
    REQUIRE(realtime_geometry.maximum_stream_output_lag_samples == 0);

    RealtimePitchTimePreparedGeometry<float> sentinel;
    sentinel.maximum_stream_output_lag_samples = 12345;
    for (const float invalid : {0.5f, std::numeric_limits<float>::quiet_NaN(),
                                std::numeric_limits<float>::infinity()}) {
        auto config = stream_config(37);
        config.max_time_ratio = invalid;
        REQUIRE(checked_realtime_pitch_time_prepared_geometry(
                    config, 1.0, std::numeric_limits<std::uint64_t>::max(), sentinel)
                == PitchTimePrepareStatus::invalid_max_time_ratio);
        REQUIRE(sentinel.maximum_stream_output_lag_samples == 12345);
    }
    auto overflow = stream_config(37);
    overflow.max_time_ratio = std::numeric_limits<float>::max();
    REQUIRE(checked_realtime_pitch_time_prepared_geometry(
                overflow, 1.0, std::numeric_limits<std::uint64_t>::max(), sentinel)
            == PitchTimePrepareStatus::unrepresentable_capacity);
    REQUIRE(sentinel.maximum_stream_output_lag_samples == 12345);
}

TEST_CASE("RealtimePitchTime causal stream lag stays within prepared bound",
          "[signal][pitch-time][streaming]") {
    bool observed_backpressure = false;
    for (const int max_block : {1, 37, 256}) {
        for (const float ratio : {0.25f, 0.999f, 1.0f, 1.75f, 4.0f})
            observed_backpressure |= stream_lag_stays_within_prepared_bound(
                max_block, 4.0f, ratio);
    }
    REQUIRE(observed_backpressure);
}

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
#if PULP_FFT_HAS_VDSP
    // The opaque setup policy is the largest float allocation on Apple. The
    // double path has no setup, but its freeze capture is 65,664 bytes.
    constexpr std::uint64_t float_capacity_bytes = 64u * 1024u;
#else
    constexpr std::uint64_t float_capacity_bytes = 8'208ULL * sizeof(float);
#endif
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

TEST_CASE("RealtimePitchTime prepared geometry reports every retained backing store",
          "[signal][pitch-time][streaming]") {
    const auto geometry = [](RealtimePitchTimeConfig config) {
        RealtimePitchTimePreparedGeometry<float> result;
        REQUIRE(checked_realtime_pitch_time_prepared_geometry(
                    config, 1.0, std::numeric_limits<std::uint64_t>::max(), result)
                == PitchTimePrepareStatus::prepared);
        REQUIRE(result.retained_bytes > 0);
        return result;
    };

    auto base_config = stream_config(128);
    base_config.channels = 1;
    base_config.max_time_ratio = 1.0f;
    const auto base = geometry(base_config);
    const auto base_oracle = exact_owned_store_charge(base_config, base);
    REQUIRE(base.retained_bytes == base_oracle.total);

    std::uint64_t platform_setup_bytes = 1;
    REQUIRE(checked_fft_platform_setup_bytes<float>(base.fft_size, UINT64_MAX,
                                                    platform_setup_bytes));
#if PULP_FFT_HAS_VDSP
    REQUIRE(platform_setup_bytes == static_cast<std::uint64_t>(base.fft_size)
                                         * kVdspFftSetupChargeBytesPerPoint);
    REQUIRE(base_oracle.platform_fft_setups == 2u * platform_setup_bytes);
    REQUIRE_FALSE(checked_fft_platform_setup_bytes<float>(
        base.fft_size, platform_setup_bytes - 1, platform_setup_bytes));
#else
    REQUIRE(platform_setup_bytes == 0);
#endif

    auto stereo_config = base_config;
    stereo_config.channels = 2;
    const auto stereo = geometry(stereo_config);
    REQUIRE(stereo.retained_bytes == exact_owned_store_charge(stereo_config, stereo).total);
    REQUIRE(stereo.retained_bytes > base.retained_bytes);

    auto wider_block_config = stereo_config;
    wider_block_config.max_block = 256;
    const auto wider_block = geometry(wider_block_config);
    REQUIRE(wider_block.retained_bytes >= stereo.retained_bytes);

    auto noise_config = stereo_config;
    noise_config.noise_morphing = true;
    const auto noise = geometry(noise_config);
    const auto noise_oracle = exact_owned_store_charge(noise_config, noise);
    REQUIRE(noise.retained_bytes == noise_oracle.total);
    REQUIRE(noise_oracle.morpher_env_a == noise_oracle.morpher_env_b);
    REQUIRE(noise_oracle.magnitude_scratch ==
            store_bytes<float>(static_cast<std::uint64_t>(noise.fft_size / 2 + 1)));
    REQUIRE(noise.retained_bytes > stereo.retained_bytes);

    auto sinc_config = stereo_config;
    sinc_config.sinc_resampling = true;
    const auto sinc = geometry(sinc_config);
    REQUIRE(sinc.retained_bytes == exact_owned_store_charge(sinc_config, sinc).total);
    REQUIRE(sinc.retained_bytes > stereo.retained_bytes);

    // The retained charge is an aggregate, not a second address ceiling.
    RealtimePitchTimePreparedGeometry<float> bounded;
    REQUIRE(checked_realtime_pitch_time_prepared_geometry(
                base_config, 1.0, 64u * 1024u, bounded)
            == PitchTimePrepareStatus::prepared);
    REQUIRE(bounded.retained_bytes > 64u * 1024u);

    // Each NoiseMorpher owns its own env vector. Their aggregate may exceed
    // the address ceiling even though every individual allocation fits it.
    constexpr std::uint64_t env_bins = 513;
    constexpr std::uint64_t morphers = 64;
    constexpr std::uint64_t allocation_ceiling = 4u * 1024u;
    std::uint64_t repeated_bytes = 0;
    REQUIRE(checked_repeated_allocation_bytes<float>(
        env_bins, morphers, allocation_ceiling, repeated_bytes));
    REQUIRE(store_bytes<float>(env_bins) <= allocation_ceiling);
    REQUIRE(repeated_bytes > allocation_ceiling);
    REQUIRE_FALSE(checked_repeated_allocation_bytes<float>(
        env_bins, morphers, store_bytes<float>(env_bins) - 1, repeated_bytes));
}

TEST_CASE("RealtimePitchTimeProcessor partial EOF read preserves the real prefix",
          "[signal][pitch-time][streaming]") {
    RealtimePitchTimeProcessor processor;
    REQUIRE(processor.prepare(kSampleRate, stream_config(256))
            == PitchTimePrepareStatus::prepared);

    const auto input = sine(367.0, 0.5, 513);
    const auto feed_input = [&input](RealtimePitchTimeProcessor& target) {
        for (std::size_t offset = 0; offset < input.size();) {
            const int count =
                static_cast<int>(std::min<std::size_t>(256, input.size() - offset));
            const float* chunk[] = {input.data() + offset};
            REQUIRE(target.feed(chunk, count) == PitchTimeStreamFeedStatus::accepted);
            offset += static_cast<std::size_t>(count);
        }
    };

    feed_input(processor);
    advance_to_final_buffer(processor);
    const int available = processor.available_stretched();
    REQUIRE(available > 0);

    std::vector<float> expected(static_cast<std::size_t>(available));
    float* expected_output[] = {expected.data()};
    REQUIRE(processor.read_stretched(expected_output, available) == available);

    // Compare two read shapes from the same prepared DSP instance. Reset is
    // bit-exact; separate float/vDSP composite instances are not guaranteed to
    // be, and that unrelated property must not weaken this strict read contract.
    processor.reset();
    feed_input(processor);
    advance_to_final_buffer(processor);
    REQUIRE(processor.available_stretched() == available);

    constexpr int excess = 7;
    std::vector<float> actual(static_cast<std::size_t>(available + excess), 123.0f);
    float* actual_output[] = {actual.data()};
    REQUIRE(processor.read_stretched(actual_output, available + excess) == available);
    REQUIRE(std::equal(expected.begin(), expected.end(), actual.begin()));
    REQUIRE(std::all_of(actual.begin() + available, actual.end(),
                        [](float value) { return value == 0.0f; }));
    REQUIRE(processor.available_stretched() == 0);
    REQUIRE(processor.finalize() == PitchTimeStreamFinalizeStatus::complete);
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
