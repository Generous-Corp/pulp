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

struct VariableLagRun {
    bool observed_backpressure = false;
    std::int64_t produced = 0;
};

VariableLagRun run_variable_lag_trajectory(RealtimePitchTimeConfig config,
                                           std::uint32_t seed,
                                           bool continuous_drain) {
    RealtimePitchTimePreparedGeometry<float> geometry;
    REQUIRE(checked_realtime_pitch_time_prepared_geometry(
                config, 1.0, std::numeric_limits<std::uint64_t>::max(), geometry)
            == PitchTimePrepareStatus::prepared);
    RealtimePitchTimeProcessor processor;
    REQUIRE(processor.prepare(kSampleRate, config) == PitchTimePrepareStatus::prepared);
    REQUIRE(processor.maximum_stream_output_lag_samples()
            == geometry.maximum_stream_output_lag_samples);

    std::vector<float> input(static_cast<std::size_t>(config.max_block), 0.0f);
    std::vector<float> output(static_cast<std::size_t>(config.max_block), 0.0f);
    const float* input_ptrs[] = {input.data()};
    float* output_ptrs[] = {output.data()};
    const auto minimum_ratio = 1.0f / config.max_time_ratio;
    float active_ratio = 1.0f;
    long double ideal_frontier = 0.0L;
    std::vector<long double> ideal_frame_starts;
    std::int64_t accepted = 0;
    std::int64_t drained = 0;
    bool observed_backpressure = false;

    const auto assert_bound = [&] {
        const auto produced = drained + processor.available_stretched();
        const auto lag = static_cast<std::int64_t>(
            std::ceil(std::max(0.0L, ideal_frontier - produced)));
        INFO("quality=" << static_cast<int>(config.quality)
                        << " fft=" << geometry.fft_size
                        << " hop=" << geometry.analysis_hop
                        << " block=" << config.max_block
                        << " accepted=" << accepted
                        << " produced=" << produced
                        << " ideal=" << static_cast<double>(ideal_frontier)
                        << " ratio=" << active_ratio);
        REQUIRE(lag <= geometry.maximum_stream_output_lag_samples);
    };
    const auto drain = [&](int maximum) {
        const int take = std::min(processor.available_stretched(), maximum);
        if (take > 0) {
            REQUIRE(processor.read_stretched(output_ptrs, take) == take);
            drained += take;
        }
        assert_bound();
    };
    const auto set_ratio = [&](float ratio) {
        active_ratio = ratio;
        processor.set_time_ratio(ratio);
        assert_bound();
    };
    const auto advance_oracle = [&](int count, int until_boundary) {
        int remaining = count;
        int until = until_boundary;
        while (remaining >= until) {
            ideal_frame_starts.push_back(ideal_frontier);
            ideal_frontier += static_cast<long double>(active_ratio)
                            * static_cast<long double>(geometry.analysis_hop);
            remaining -= until;
            until = geometry.analysis_hop;
        }
    };
    const auto feed = [&](int count) {
        REQUIRE(count > 0);
        REQUIRE(count <= config.max_block);
        for (;;) {
            const int until = processor.samples_until_next_analysis_frame();
            REQUIRE(until > 0);
            const auto status = processor.feed(input_ptrs, count);
            if (status == PitchTimeStreamFeedStatus::accepted) {
                advance_oracle(count, until);
                accepted += count;
                assert_bound();
                if (continuous_drain)
                    while (processor.available_stretched() > 0)
                        drain(config.max_block);
                return;
            }
            REQUIRE(status == PitchTimeStreamFeedStatus::backpressure);
            observed_backpressure = true;
            drain(config.max_block);
        }
    };

    // Deterministic boundary controls: immediately before a boundary, at the
    // boundary after it has been analyzed, and one sample after that boundary.
    set_ratio(config.max_time_ratio);
    while (processor.samples_until_next_analysis_frame() > config.max_block)
        feed(config.max_block);
    const int before_boundary = processor.samples_until_next_analysis_frame();
    if (before_boundary > 1)
        feed(before_boundary - 1);
    set_ratio(minimum_ratio);
    feed(1);
    set_ratio(config.max_time_ratio);
    feed(1);
    set_ratio(1.0f);

    const int prepared_lag = processor.maximum_stream_output_lag_samples();
    processor.reset();
    REQUIRE(processor.maximum_stream_output_lag_samples() == prepared_lag);
    REQUIRE(processor.available_stretched() == 0);
    accepted = 0;
    drained = 0;
    ideal_frontier = 0.0L;
    ideal_frame_starts.clear();
    assert_bound();

    // Deterministic xorshift trajectory. Chunk choices straddle both the
    // current analysis boundary and the nominal hop boundary.
    const auto next_random = [&seed] {
        seed ^= seed << 13u;
        seed ^= seed >> 17u;
        seed ^= seed << 5u;
        return seed;
    };
    const auto target_input = accepted
                            + static_cast<std::int64_t>(geometry.fft_size)
                            + 24 * static_cast<std::int64_t>(geometry.analysis_hop);
    while (accepted < target_input) {
        const auto bits = next_random();
        const float unit = static_cast<float>(bits & 0xffffu) / 65535.0f;
        set_ratio(minimum_ratio
                  + unit * (config.max_time_ratio - minimum_ratio));
        const int until = processor.samples_until_next_analysis_frame();
        const std::array<int, 9> candidates{
            1,
            std::max(1, until - 1),
            until,
            until + 1,
            std::max(1, geometry.analysis_hop - 1),
            geometry.analysis_hop,
            geometry.analysis_hop + 1,
            std::max(1, config.max_block - 1),
            config.max_block,
        };
        const int count = std::min(
            candidates[(bits >> 16u) % candidates.size()], config.max_block);
        feed(static_cast<int>(std::min<std::int64_t>(
            count, target_input - accepted)));
    }

    // A no-drain maximum-ratio phase deterministically reaches backpressure
    // regardless of the randomized trajectory above.
    if (!continuous_drain) {
        set_ratio(config.max_time_ratio);
        for (int guard = 0; guard < 100'000 && !observed_backpressure; ++guard)
            feed(config.max_block);
        REQUIRE(observed_backpressure);
    }

    // Final output is interpolated at the real-input cursor between the same
    // analysis-frame boundaries used by the stream, but with an independent
    // long-double cumulative ratio oracle rather than processor counters.
    const auto final_frame = static_cast<std::size_t>(
        accepted / geometry.analysis_hop);
    while (ideal_frame_starts.size() <= final_frame + 1u) {
        ideal_frame_starts.push_back(ideal_frontier);
        ideal_frontier += static_cast<long double>(active_ratio)
                        * static_cast<long double>(geometry.analysis_hop);
    }
    const auto final_offset = accepted % geometry.analysis_hop;
    const auto final_ideal =
        ideal_frame_starts[final_frame]
        + (ideal_frame_starts[final_frame + 1u] - ideal_frame_starts[final_frame])
              * static_cast<long double>(final_offset)
              / static_cast<long double>(geometry.analysis_hop);
    ideal_frontier = final_ideal;
    assert_bound();

    for (int guard = 0; guard < 100'000; ++guard) {
        const auto status = processor.finalize(config.max_block);
        assert_bound();
        if (processor.available_stretched() > 0)
            drain(config.max_block);
        if (status == PitchTimeStreamFinalizeStatus::complete) {
            REQUIRE(processor.available_stretched() == 0);
            assert_bound();
            REQUIRE(std::abs(
                        drained
                        - static_cast<std::int64_t>(std::llround(final_ideal)))
                    <= 1);
            return {observed_backpressure, drained};
        }
        REQUIRE((status == PitchTimeStreamFinalizeStatus::draining
                 || status == PitchTimeStreamFinalizeStatus::backpressure));
    }
    FAIL("variable-ratio finalization did not converge");
    return {};
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

TEST_CASE("RealtimePitchTime variable-ratio trajectories stay within the cumulative lag bound",
          "[signal][pitch-time][streaming][lag-bound]") {
    std::array<RealtimePitchTimeConfig, 3> configs;
    configs[0] = stream_config(257);
    configs[0].quality = PitchTimeQuality::low_latency;
    configs[0].max_time_ratio = 4.0f;
    configs[1] = stream_config(513);
    configs[1].quality = PitchTimeQuality::quality;
    configs[1].max_time_ratio = 4.0f;
    configs[2] = stream_config(129);
    configs[2].quality = PitchTimeQuality::low_latency;
    configs[2].fft_size = 2048;
    configs[2].analysis_hop = 128;
    configs[2].max_time_ratio = 3.0f;

    for (std::size_t index = 0; index < configs.size(); ++index) {
        const auto continuous = run_variable_lag_trajectory(
            configs[index], 0x9e3779b9u + static_cast<std::uint32_t>(index), true);
        const auto pressured = run_variable_lag_trajectory(
            configs[index], 0x7f4a7c15u + static_cast<std::uint32_t>(index), false);
        REQUIRE_FALSE(continuous.observed_backpressure);
        REQUIRE(pressured.observed_backpressure);
        REQUIRE(continuous.produced > 0);
        REQUIRE(pressured.produced > 0);
    }
}

TEST_CASE("RealtimePitchTime advertised lag is a causal fixed PDC offset",
          "[signal][pitch-time][streaming][lag-bound]") {
    auto config = stream_config(257);
    config.max_time_ratio = 4.0f;
    RealtimePitchTimeProcessor processor;
    REQUIRE(processor.prepare(kSampleRate, config) == PitchTimePrepareStatus::prepared);
    processor.set_time_ratio(1.0f);

    constexpr int input_frames = 8193;
    const int marked_impulse = processor.input_priming_samples() + 3 * 256 + 17;
    std::vector<float> input(input_frames, 0.0f);
    input[0] = 0.25f;
    input[static_cast<std::size_t>(marked_impulse)] = 1.0f;
    std::vector<float> scratch(static_cast<std::size_t>(config.max_block), 0.0f);
    std::vector<float> rendered;
    rendered.reserve(input.size());
    float* output_ptrs[] = {scratch.data()};

    const auto drain = [&] {
        while (processor.available_stretched() > 0) {
            const int take = std::min(processor.available_stretched(), config.max_block);
            REQUIRE(processor.read_stretched(output_ptrs, take) == take);
            rendered.insert(rendered.end(), scratch.begin(), scratch.begin() + take);
        }
    };
    for (int offset = 0; offset < input_frames;) {
        const int count = std::min(config.max_block, input_frames - offset);
        const float* input_ptrs[] = {input.data() + offset};
        auto status = processor.feed(input_ptrs, count);
        while (status == PitchTimeStreamFeedStatus::backpressure) {
            drain();
            status = processor.feed(input_ptrs, count);
        }
        REQUIRE(status == PitchTimeStreamFeedStatus::accepted);
        offset += count;
        drain();
    }
    for (int guard = 0; guard < 100'000; ++guard) {
        const auto status = processor.finalize(config.max_block);
        drain();
        if (status == PitchTimeStreamFinalizeStatus::complete)
            break;
        REQUIRE((status == PitchTimeStreamFinalizeStatus::draining
                 || status == PitchTimeStreamFinalizeStatus::backpressure));
        REQUIRE(guard < 99'999);
    }
    REQUIRE(rendered.size() == input.size());

    const auto peak = static_cast<std::size_t>(std::distance(
        rendered.begin(), std::max_element(rendered.begin(), rendered.end(),
                                           [](float lhs, float rhs) {
                                               return std::abs(lhs) < std::abs(rhs);
                                           })));
    REQUIRE(peak == static_cast<std::size_t>(marked_impulse));

    const auto lag = static_cast<std::size_t>(
        processor.maximum_stream_output_lag_samples());
    REQUIRE(lag > 0);
    constexpr auto invalid_index = std::numeric_limits<std::size_t>::max();
    std::vector<float> delay_line(lag, 0.0f);
    std::vector<std::size_t> delayed_indices(lag, invalid_index);
    std::vector<float> pdc_staged;
    std::vector<std::size_t> staged_indices;
    pdc_staged.reserve(lag + rendered.size());
    staged_indices.reserve(lag + rendered.size());
    std::size_t write = 0;
    const auto push_delayed = [&](float sample, std::size_t stream_index) {
        pdc_staged.push_back(delay_line[write]);
        staged_indices.push_back(delayed_indices[write]);
        delay_line[write] = sample;
        delayed_indices[write] = stream_index;
        write = (write + 1u) % lag;
    };
    for (std::size_t frame = 0; frame < rendered.size(); ++frame)
        push_delayed(rendered[frame], frame);
    for (std::size_t frame = 0; frame < lag; ++frame)
        push_delayed(0.0f, invalid_index);
    REQUIRE(pdc_staged.size() == lag + rendered.size());
    REQUIRE(staged_indices.size() == pdc_staged.size());
    REQUIRE(std::all_of(pdc_staged.begin(), pdc_staged.begin() + lag,
                        [](float sample) { return sample == 0.0f; }));
    REQUIRE(std::all_of(staged_indices.begin(), staged_indices.begin() + lag,
                        [](std::size_t index) {
                            return index == invalid_index;
                        }));
    REQUIRE(staged_indices[lag] == 0);
    REQUIRE(staged_indices[lag + marked_impulse]
            == static_cast<std::size_t>(marked_impulse));
    REQUIRE(pdc_staged[lag + peak] == rendered[peak]);
    REQUIRE(std::distance(
                pdc_staged.begin(),
                std::max_element(pdc_staged.begin(), pdc_staged.end(),
                                 [](float lhs, float rhs) {
                                     return std::abs(lhs) < std::abs(rhs);
                                 }))
            == static_cast<std::ptrdiff_t>(lag + marked_impulse));
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
    const int prepared_lag =
        previously_prepared.maximum_stream_output_lag_samples();
    REQUIRE(prepared_capacity > 0);
    REQUIRE(prepared_lag > 0);

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
        REQUIRE(previously_prepared.maximum_stream_output_lag_samples()
                == prepared_lag);
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
    const int prepared_lag =
        previously_prepared.maximum_stream_output_lag_samples();

    auto require_rejected_without_mutation = [&](RealtimePitchTimeConfig config,
                                                 PitchTimePrepareStatus expected) {
        RealtimePitchTimeProcessor fresh;
        REQUIRE(fresh.prepare(kSampleRate, config) == expected);
        REQUIRE(fresh.output_free_space() == 0);
        REQUIRE(previously_prepared.prepare(kSampleRate, config) == expected);
        REQUIRE(previously_prepared.output_free_space() == prepared_capacity);
        REQUIRE(previously_prepared.maximum_stream_output_lag_samples()
                == prepared_lag);
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
    REQUIRE(previously_prepared.maximum_stream_output_lag_samples()
            == prepared_lag);

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

    for (const int invalid_iterations :
         {-1, kSourceFilterMaximumEnvelopeIterations + 1}) {
        auto config = stream_config(256);
        config.true_envelope_iterations = invalid_iterations;

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
